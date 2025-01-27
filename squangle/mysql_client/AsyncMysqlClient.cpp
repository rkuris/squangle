/*
 *  Copyright (c) Facebook, Inc. and its affiliates..
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 *
 */

#include "squangle/mysql_client/AsyncMysqlClient.h"
#include "squangle/logger/DBEventLogger.h"
#include "squangle/mysql_client/FutureAdapter.h"
#include "squangle/mysql_client/Operation.h"

#include <vector>

#include <folly/Memory.h>
#include <folly/Singleton.h>
#include <folly/Unit.h>
#include <folly/futures/Future.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/portability/GFlags.h>
#include <folly/ssl/Init.h>
#include <folly/system/ThreadName.h>

#include <mysql.h>

#include <fcntl.h>
#include <unistd.h>

DECLARE_int64(mysql_mysql_timeout_micros);

namespace {
class InitMysqlLibrary {
 public:
  InitMysqlLibrary() {
    folly::ssl::init();
    mysql_library_init(-1, nullptr, nullptr);
  }
};
} // namespace

namespace facebook {
namespace common {
namespace mysql_client {

namespace {
folly::Singleton<AsyncMysqlClient> client(
    []() { return new AsyncMysqlClient; },
    AsyncMysqlClient::deleter);
} // namespace

std::shared_ptr<AsyncMysqlClient> AsyncMysqlClient::defaultClient() {
  return folly::Singleton<AsyncMysqlClient>::try_get();
}

MysqlClientBase::MysqlClientBase(
    std::unique_ptr<db::SquangleLoggerBase> db_logger,
    std::unique_ptr<db::DBCounterBase> db_stats)
    : db_logger_(std::move(db_logger)), client_stats_(std::move(db_stats)) {
  static InitMysqlLibrary unused;
}

AsyncMysqlClient::AsyncMysqlClient(
    std::unique_ptr<db::SquangleLoggerBase> db_logger,
    std::unique_ptr<db::DBCounterBase> db_stats)
    : MysqlClientBase(std::move(db_logger), std::move(db_stats)),
      pools_conn_limit_(std::numeric_limits<uint64_t>::max()),
      stats_tracker_(std::make_shared<StatsTracker>()) {
  init();
}

AsyncMysqlClient::AsyncMysqlClient()
    : AsyncMysqlClient(nullptr, std::make_unique<db::SimpleDbCounter>()) {}

void AsyncMysqlClient::init() {
  auto eventBase = getEventBase();
  eventBase->setObserver(stats_tracker_);
  thread_ = std::thread([eventBase]() {
#ifdef __GLIBC__
    folly::setThreadName("async-mysql");
#endif
    folly::EventBaseManager::get()->setEventBase(eventBase, false);
    eventBase->loopForever();
    mysql_thread_end();
  });
  eventBase->waitUntilRunning();
}

bool AsyncMysqlClient::runInThread(folly::Cob&& fn) {
  auto scheduleTime = std::chrono::steady_clock::now();
  getEventBase()->runInEventBaseThread(
      [fn = std::move(fn), scheduleTime, this]() mutable {
        auto delay = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - scheduleTime)
                         .count();
        stats_tracker_->callbackDelayAvg.addSample(delay);
        fn();
      });
  return true;
}

void AsyncMysqlClient::drain(bool also_block_operations) {
  {
    std::unique_lock<std::mutex> lock(pending_operations_mutex_);
    block_operations_ = also_block_operations;

    auto it = pending_operations_.begin();
    // Clean out any unstarted operations.
    while (it != pending_operations_.end()) {
      // So here the Operation `run` was not called
      // We don't need to lock the state change in the operation here since the
      // cancelling process is going to fire not matter in which part it is.
      if ((*it)->state() == OperationState::Unstarted) {
        (*it)->cancel();
        it = pending_operations_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Now wait for any started operations to complete.
  std::unique_lock<std::mutex> counter_lock(this->counters_mutex_);
  active_connections_closed_cv_.wait(
      counter_lock, [&also_block_operations, this] {
        if (also_block_operations) {
          VLOG(11)
              << "Waiting for " << this->active_connection_counter_
              << " connections to be released before shutting client down ";
        }
        return this->active_connection_counter_ == 0;
      });
}

void AsyncMysqlClient::shutdownClient() {
  DCHECK(std::this_thread::get_id() != threadId());
  if (is_shutdown_.exchange(true)) {
    return;
  }
  // Drain anything we currently have, and if those operations make
  // new operations, that's okay.
  drain(false);
  // Once that pass is done, finish anything that happened to sneak
  // in, but guarantee no new operations will come along.
  drain(true);

  CHECK_EQ(numStartedAndOpenConnections(), 0);

  DCHECK(connection_references_.size() == 0);

  // TODO: Maybe add here a runInThread to cancel the AsyncTimeout

  // All operations are done.  Shut the thread down.
  getEventBase()->terminateLoopSoon();
  if (std::this_thread::get_id() != threadId()) {
    thread_.join();
  } else {
    LOG(ERROR) << "shutdownClient() called from AsyncMysql thread";
    thread_.detach();
  }
}

AsyncMysqlClient::~AsyncMysqlClient() {
  shutdownClient();
  VLOG(2) << "AsyncMysqlClient finished destructor";
}

void MysqlClientBase::logQuerySuccess(
    const db::QueryLoggingData& logging_data,
    const Connection& conn) {
  auto conn_context = conn.getConnectionContext();
  stats()->incrSucceededQueries(conn_context);

  if (db_logger_) {
    db_logger_->logQuerySuccess(
        logging_data, makeSquangleLoggingData(conn.getKey(), conn_context));
  }
}

void MysqlClientBase::logQueryFailure(
    const db::QueryLoggingData& logging_data,
    db::FailureReason reason,
    unsigned int mysqlErrno,
    const std::string& error,
    const Connection& conn) {
  auto conn_context = conn.getConnectionContext();
  stats()->incrFailedQueries(conn_context, mysqlErrno);

  if (db_logger_) {
    db_logger_->logQueryFailure(
        logging_data,
        reason,
        mysqlErrno,
        error,
        makeSquangleLoggingData(conn.getKey(), conn_context));
  }
}

void MysqlClientBase::logConnectionSuccess(
    const db::CommonLoggingData& logging_data,
    const ConnectionKey& conn_key,
    const db::ConnectionContextBase* connection_context) {
  if (db_logger_) {
    db_logger_->logConnectionSuccess(
        logging_data, makeSquangleLoggingData(&conn_key, connection_context));
  }
}

void MysqlClientBase::logConnectionFailure(
    const db::CommonLoggingData& logging_data,
    db::FailureReason reason,
    const ConnectionKey& conn_key,
    unsigned int mysqlErrno,
    const std::string& error,
    const db::ConnectionContextBase* connection_context) {
  stats()->incrFailedConnections(connection_context, mysqlErrno);

  if (db_logger_) {
    db_logger_->logConnectionFailure(
        logging_data,
        reason,
        mysqlErrno,
        error,
        makeSquangleLoggingData(&conn_key, connection_context));
  }
}

db::SquangleLoggingData AsyncMysqlClient::makeSquangleLoggingData(
    const ConnectionKey* connKey,
    const db::ConnectionContextBase* connContext) {
  db::SquangleLoggingData ret(connKey, connContext);
  ret.clientPerfStats = collectPerfStats();
  return ret;
}

void AsyncMysqlClient::cleanupCompletedOperations() {
  std::unique_lock<std::mutex> lock(pending_operations_mutex_);
  size_t num_erased = 0, before = pending_operations_.size();

  VLOG(11) << "removing pending operations";
  for (auto& op : operations_to_remove_) {
    if (pending_operations_.erase(op) > 0) {
      ++num_erased;
    } else {
      LOG(DFATAL) << "asked to remove non-pending operation";
    }
  }

  operations_to_remove_.clear();

  VLOG(11) << "erased: " << num_erased << ", before: " << before
           << ", after: " << pending_operations_.size();
}

folly::SemiFuture<ConnectResult> AsyncMysqlClient::connectSemiFuture(
    const std::string& host,
    int port,
    const std::string& database_name,
    const std::string& user,
    const std::string& password,
    const ConnectionOptions& conn_opts) {
  return toSemiFuture(beginConnection(host, port, database_name, user, password)
                          ->setConnectionOptions(conn_opts));
}

folly::Future<ConnectResult> AsyncMysqlClient::connectFuture(
    const std::string& host,
    int port,
    const std::string& database_name,
    const std::string& user,
    const std::string& password,
    const ConnectionOptions& conn_opts) {
  return toFuture(
      connectSemiFuture(host, port, database_name, user, password, conn_opts));
}

std::unique_ptr<Connection> AsyncMysqlClient::connect(
    const std::string& host,
    int port,
    const std::string& database_name,
    const std::string& user,
    const std::string& password,
    const ConnectionOptions& conn_opts) {
  auto op = beginConnection(host, port, database_name, user, password);
  op->setConnectionOptions(conn_opts);
  // This will throw (intended behavour) in case the operation didn't succeed
  auto conn = blockingConnectHelper(op);
  return conn;
}

std::shared_ptr<ConnectOperation> MysqlClientBase::beginConnection(
    const std::string& host,
    int port,
    const std::string& database_name,
    const std::string& user,
    const std::string& password) {
  return beginConnection(
      ConnectionKey(host, port, database_name, user, password));
}

std::shared_ptr<ConnectOperation> MysqlClientBase::beginConnection(
    ConnectionKey conn_key) {
  auto ret = std::make_shared<ConnectOperation>(this, std::move(conn_key));
  if (connection_cb_) {
    ret->setObserverCallback(connection_cb_);
  }
  addOperation(ret);
  return ret;
}

std::unique_ptr<Connection> AsyncMysqlClient::createConnection(
    ConnectionKey conn_key,
    MYSQL* mysql_conn) {
  return std::make_unique<AsyncConnection>(
      this, std::move(conn_key), mysql_conn);
}

static inline MysqlHandler::Status toHandlerStatus(net_async_status status) {
  if (status == NET_ASYNC_ERROR) {
    return MysqlHandler::Status::ERROR;
  } else if (status == NET_ASYNC_COMPLETE) {
    return MysqlHandler::Status::DONE;
  } else {
    return MysqlHandler::Status::PENDING;
  }
}

MysqlHandler::Status AsyncMysqlClient::AsyncMysqlHandler::tryConnect(
    MYSQL* mysql,
    const ConnectionOptions& /*opts*/,
    const ConnectionKey& conn_key,
    int flags) {
  return toHandlerStatus(mysql_real_connect_nonblocking(
      mysql,
      conn_key.host.c_str(),
      conn_key.user.c_str(),
      conn_key.password.c_str(),
      conn_key.db_name.c_str(),
      conn_key.port,
      nullptr,
      flags));
}

MysqlHandler::Status AsyncMysqlClient::AsyncMysqlHandler::runQuery(
    MYSQL* mysql,
    folly::StringPiece queryStmt) {
  return toHandlerStatus(
      mysql_real_query_nonblocking(mysql, queryStmt.begin(), queryStmt.size()));
}

MysqlHandler::Status AsyncMysqlClient::AsyncMysqlHandler::resetConn(
    MYSQL* mysql) {
  return toHandlerStatus(mysql_reset_connection_nonblocking(mysql));
}

MysqlHandler::Status AsyncMysqlClient::AsyncMysqlHandler::changeUser(
    MYSQL* mysql,
    const std::string& user,
    const std::string& password,
    const std::string& database) {
  return toHandlerStatus(mysql_change_user_nonblocking(
      mysql, user.c_str(), password.c_str(), database.c_str()));
}

MysqlHandler::Status AsyncMysqlClient::AsyncMysqlHandler::nextResult(
    MYSQL* mysql) {
  return toHandlerStatus(mysql_next_result_nonblocking(mysql));
}

MysqlHandler::Status AsyncMysqlClient::AsyncMysqlHandler::fetchRow(
    MYSQL_RES* res,
    MYSQL_ROW& row) {
  auto status = toHandlerStatus(mysql_fetch_row_nonblocking(res, &row));
  DCHECK_NE(status, ERROR); // Should never be an error
  return status;
}

MYSQL_RES* AsyncMysqlClient::AsyncMysqlHandler::getResult(MYSQL* mysql) {
  return mysql_use_result(mysql);
}

std::unique_ptr<Connection> MysqlClientBase::adoptConnection(
    MYSQL* raw_conn,
    const std::string& host,
    int port,
    const std::string& database_name,
    const std::string& user,
    const std::string& password) {
  auto conn = createConnection(
      ConnectionKey(host, port, database_name, user, password), raw_conn);
  conn->socketHandler()->changeHandlerFD(
      folly::NetworkSocket::fromFd(mysql_get_socket_descriptor(raw_conn)));
  return conn;
}

bool Connection::isSSL() const {
  CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
  return mysql_connection_->mysql()->client_flag & CLIENT_SSL;
}

void Connection::initMysqlOnly() {
  DCHECK(isInEventBaseThread());
  CHECK_THROW(mysql_connection_ == nullptr, db::InvalidConnectionException);
  mysql_connection_ = std::make_unique<MysqlConnectionHolder>(
      mysql_client_, mysql_init(nullptr), conn_key_);
  mysql_connection_->mysql()->options.client_flag &= ~CLIENT_LOCAL_FILES;
  // Turn off SSL by default for tests that rely on this.
  enum mysql_ssl_mode ssl_mode = SSL_MODE_DISABLED;
  mysql_options(mysql_connection_->mysql(), MYSQL_OPT_SSL_MODE, &ssl_mode);
}

void Connection::initialize(bool initMysql) {
  if (initMysql) {
    initMysqlOnly();
  }
  initialized_ = true;
}

Connection::~Connection() {
  if (mysql_connection_ && conn_dying_callback_ && needToCloneConnection_ &&
      isReusable() && !inTransaction() &&
      getConnectionOptions().isEnableResetConnBeforeClose()) {
    // We clone this Connection object to send COM_RESET_CONNECTION command
    // via the connection before returning it to the connection pool.
    // The callback function points to recycleMysqlConnection(), which is
    // responsible for recyclining the connection.
    // This object's callback is set to null and the cloned object's
    // callback instead points to the original callback function, which will
    // be called after COM_RESET_CONNECTION.

    // TODO: just set NeedResetBeforeReuse in any case
    if (!isInEventBaseThread()) {
      auto connHolder = stealMysqlConnectionHolder(true);
      auto conn = std::make_unique<AsyncConnection>(
          client(), *getKey(), std::move(connHolder));
      conn->needToCloneConnection_ = false;
      conn->setConnectionOptions(getConnectionOptions());
      conn->setConnectionDyingCallback(std::move(conn_dying_callback_));
      conn_dying_callback_ = nullptr;

      auto resetOp = Connection::resetConn(std::move(conn));
      if (client()->runInThread([resetOp]() {
            // addOperation() is necessary here for proper cancelling of reset
            // operation in case of sudden AsyncMysqlClient shutdown
            resetOp->connection()->client()->addOperation(resetOp);
            resetOp->run();
          })) {
        resetOp->wait();
      }
    } else if (getConnectionOptions().isEnableDelayedResetConn()) {
      mysql_connection_->setNeedResetBeforeReuse();
    }
  }

  if (mysql_connection_ && conn_dying_callback_) {
    // Recycle connection, if not needed the client will throw it away
    conn_dying_callback_(std::move(mysql_connection_));
  }
}

std::shared_ptr<ResetOperation> Connection::resetConn(
    std::unique_ptr<Connection> conn) {
  // This function is very similar to beginQuery(), but this does not call
  // addOperation(), which is called by the caller prior to calling
  // resetOp->run(). This is to avoid race condition where shutdownClient() can
  // remove the reset operation from pending_operations_ queue, while the
  // operation still exists in operations_to_remove_ queue; in that case,
  // cleanupCompletedOperations() hits FATAL error.
  auto resetOperationPtr = std::make_shared<ResetOperation>(
      Operation::ConnectionProxy(Operation::OwnedConnection(std::move(conn))));
  Duration timeout =
      resetOperationPtr->connection()->conn_options_.getQueryTimeout();
  if (timeout.count() > 0) {
    resetOperationPtr->setTimeout(timeout);
  }
  resetOperationPtr->connection()->socket_handler_.setOperation(
      resetOperationPtr.get());
  return resetOperationPtr;
}

std::shared_ptr<ChangeUserOperation> Connection::changeUser(
    std::unique_ptr<Connection> conn,
    const std::string& user,
    const std::string& password,
    const std::string& database) {
  auto changeUserOperationPtr = std::make_shared<ChangeUserOperation>(
      Operation::ConnectionProxy(Operation::OwnedConnection(std::move(conn))),
      user,
      password,
      database);
  Duration timeout =
      changeUserOperationPtr->connection()->conn_options_.getTimeout();
  if (timeout.count() > 0) {
    // set its timeout longer than connection timeout to prevent change user
    // operation from hitting timeout earlier than connection timeout itself
    changeUserOperationPtr->setTimeout(timeout + std::chrono::seconds(1));
  }
  changeUserOperationPtr->connection()->socket_handler_.setOperation(
      changeUserOperationPtr.get());
  return changeUserOperationPtr;
}

template <>
std::shared_ptr<QueryOperation> Connection::beginQuery(
    std::unique_ptr<Connection> conn,
    Query&& query) {
  return beginAnyQuery<QueryOperation>(
      Operation::ConnectionProxy(Operation::OwnedConnection(std::move(conn))),
      std::move(query));
}

template <>
std::shared_ptr<MultiQueryOperation> Connection::beginMultiQuery(
    std::unique_ptr<Connection> conn,
    std::vector<Query>&& queries) {
  auto is_queries_empty = queries.empty();
  auto operation = beginAnyQuery<MultiQueryOperation>(
      Operation::ConnectionProxy(Operation::OwnedConnection(std::move(conn))),
      std::move(queries));
  if (is_queries_empty) {
    operation->setAsyncClientError("Given vector of queries is empty");
    operation->cancel();
  }
  return operation;
}

template <>
std::shared_ptr<MultiQueryStreamOperation> Connection::beginMultiQueryStreaming(
    std::unique_ptr<Connection> conn,
    std::vector<Query>&& queries) {
  auto is_queries_empty = queries.empty();
  auto operation = beginAnyQuery<MultiQueryStreamOperation>(
      Operation::ConnectionProxy(Operation::OwnedConnection(std::move(conn))),
      std::move(queries));
  if (is_queries_empty) {
    operation->setAsyncClientError("Given vector of queries is empty");
    operation->cancel();
  }
  return operation;
}

template <typename QueryType, typename QueryArg>
std::shared_ptr<QueryType> Connection::beginAnyQuery(
    Operation::ConnectionProxy&& conn_proxy,
    QueryArg&& query) {
  CHECK_THROW(conn_proxy.get(), db::InvalidConnectionException);
  CHECK_THROW(conn_proxy.get()->ok(), db::InvalidConnectionException);
  conn_proxy.get()->checkOperationInProgress();
  auto ret =
      std::make_shared<QueryType>(std::move(conn_proxy), std::move(query));
  Duration timeout = ret->connection()->conn_options_.getQueryTimeout();
  if (timeout.count() > 0) {
    ret->setTimeout(timeout);
  }

  auto* conn = ret->connection();
  conn->mysql_client_->addOperation(ret);
  conn->socket_handler_.setOperation(ret.get());
  ret->setPreOperationCallback([conn](Operation& op) {
    if (conn->callbacks_.pre_operation_callback_) {
      conn->callbacks_.pre_operation_callback_(op);
    }
  });
  ret->setPostOperationCallback([conn](Operation& op) {
    if (conn->callbacks_.post_operation_callback_) {
      conn->callbacks_.post_operation_callback_(op);
    }
  });
  auto opType = ret->getOperationType();
  if (opType == db::OperationType::Query ||
      opType == db::OperationType::MultiQuery) {
    ret->setPreQueryCallback([conn](FetchOperation& op) {
      return conn->callbacks_.pre_query_callback_
          ? conn->callbacks_.pre_query_callback_(op)
          : folly::makeSemiFuture(folly::unit);
    });
    ret->setPostQueryCallback([conn](AsyncPostQueryResult&& result) {
      return conn->callbacks_.post_query_callback_
          ? conn->callbacks_.post_query_callback_(std::move(result))
          : folly::makeSemiFuture(std::move(result));
    });
  }
  return ret;
}

// A query might already be semicolon-separated, so we allow this to
// be a MultiQuery.  Or it might just be one query; that's okay, too.
template <>
std::shared_ptr<MultiQueryOperation> Connection::beginMultiQuery(
    std::unique_ptr<Connection> conn,
    Query&& query) {
  return Connection::beginMultiQuery(
      std::move(conn), std::vector<Query>{std::move(query)});
}

template <>
std::shared_ptr<MultiQueryStreamOperation> Connection::beginMultiQueryStreaming(
    std::unique_ptr<Connection> conn,
    Query&& query) {
  return Connection::beginMultiQueryStreaming(
      std::move(conn), std::vector<Query>{std::move(query)});
}

folly::SemiFuture<DbQueryResult> Connection::querySemiFuture(
    std::unique_ptr<Connection> conn,
    Query&& query,
    QueryOptions&& options) {
  auto op = beginQuery(std::move(conn), std::move(query));
  op->setAttributes(std::move(options.getAttributes()));
  return toSemiFuture(op);
}

template <>
folly::Future<DbQueryResult> Connection::queryFuture(
    std::unique_ptr<Connection> conn,
    Query&& query) {
  return toFuture(querySemiFuture(std::move(conn), std::move(query)));
}

folly::SemiFuture<DbMultiQueryResult> Connection::multiQuerySemiFuture(
    std::unique_ptr<Connection> conn,
    Query&& args,
    QueryOptions&& options) {
  auto op = beginMultiQuery(std::move(conn), std::move(args));
  op->setAttributes(std::move(options.getAttributes()));
  return toSemiFuture(op);
}

folly::SemiFuture<DbMultiQueryResult> Connection::multiQuerySemiFuture(
    std::unique_ptr<Connection> conn,
    std::vector<Query>&& args,
    QueryOptions&& options) {
  auto op = beginMultiQuery(std::move(conn), std::move(args));
  op->setAttributes(std::move(options.getAttributes()));
  return toSemiFuture(op);
}

folly::Future<DbMultiQueryResult> Connection::multiQueryFuture(
    std::unique_ptr<Connection> conn,
    Query&& args) {
  return toFuture(multiQuerySemiFuture(std::move(conn), std::move(args)));
}

folly::Future<DbMultiQueryResult> Connection::multiQueryFuture(
    std::unique_ptr<Connection> conn,
    std::vector<Query>&& args) {
  return toFuture(multiQuerySemiFuture(std::move(conn), std::move(args)));
}

template <>
DbQueryResult Connection::query(Query&& query) {
  auto op = beginAnyQuery<QueryOperation>(
      Operation::ConnectionProxy(Operation::ReferencedConnection(this)),
      std::move(query));
  SCOPE_EXIT {
    operation_in_progress_ = false;
  };
  operation_in_progress_ = true;

  if (op->callbacks_.pre_query_callback_) {
    op->callbacks_.pre_query_callback_(*op).get();
  }
  op->run()->wait();

  if (!op->ok()) {
    throw QueryException(
        op->numQueriesExecuted(),
        op->result(),
        op->mysql_errno(),
        op->mysql_error(),
        *getKey(),
        op->elapsed());
  }
  auto conn_key = *op->connection()->getKey();
  DbQueryResult result(
      std::move(op->stealQueryResult()),
      op->numQueriesExecuted(),
      op->resultSize(),
      nullptr,
      op->result(),
      conn_key,
      op->elapsed());
  if (op->callbacks_.post_query_callback_) {
    // If we have a callback set, wrap (and then unwrap) the result to/from the
    // callback's std::variant wrapper
    return op->callbacks_.post_query_callback_(std::move(result))
        .deferValue([](AsyncPostQueryResult&& result) {
          return std::get<DbQueryResult>(std::move(result));
        })
        .get();
  }
  return result;
}

template <>
DbMultiQueryResult Connection::multiQuery(std::vector<Query>&& queries) {
  auto op = beginAnyQuery<MultiQueryOperation>(
      Operation::ConnectionProxy(Operation::ReferencedConnection(this)),
      std::move(queries));
  auto guard = folly::makeGuard([&] { operation_in_progress_ = false; });

  operation_in_progress_ = true;
  if (op->callbacks_.pre_query_callback_) {
    op->callbacks_.pre_query_callback_(*op).get();
  }
  op->run()->wait();

  if (!op->ok()) {
    throw QueryException(
        op->numQueriesExecuted(),
        op->result(),
        op->mysql_errno(),
        op->mysql_error(),
        *getKey(),
        op->elapsed());
  }

  auto conn_key = *op->connection()->getKey();
  DbMultiQueryResult result(
      std::move(op->stealQueryResults()),
      op->numQueriesExecuted(),
      op->resultSize(),
      nullptr,
      op->result(),
      std::move(conn_key),
      op->elapsed());
  if (op->callbacks_.post_query_callback_) {
    // If we have a callback set, wrap (and then unwrap) the result to/from the
    // callback's std::variant wrapper
    return op->callbacks_.post_query_callback_(std::move(result))
        .deferValue([](AsyncPostQueryResult&& result) {
          return std::get<DbMultiQueryResult>(std::move(result));
        })
        .get();
  }
  return result;
}

template <>
DbMultiQueryResult Connection::multiQuery(Query&& query) {
  return multiQuery(std::vector<Query>{std::move(query)});
}

template <typename... Args>
DbMultiQueryResult Connection::multiQuery(Args&&... args) {
  return multiQuery(std::vector<Query>{std::forward<Args>(args)...});
}

MultiQueryStreamHandler Connection::streamMultiQuery(
    std::unique_ptr<Connection> conn,
    std::vector<Query>&& queries,
    const std::unordered_map<std::string, std::string>& attributes) {
  // MultiQueryStreamHandler needs to be alive while the operation is running.
  // To accomplish that, ~MultiQueryStreamHandler waits until
  // `postOperationEnded` is called.
  auto operation = beginAnyQuery<MultiQueryStreamOperation>(
      Operation::ConnectionProxy(Operation::OwnedConnection(std::move(conn))),
      std::move(queries));
  if (attributes.size() > 0) {
    operation->setAttributes(attributes);
  }
  return MultiQueryStreamHandler(operation);
}

MultiQueryStreamHandler Connection::streamMultiQuery(
    std::unique_ptr<Connection> conn,
    MultiQuery&& multi_query,
    const std::unordered_map<std::string, std::string>& attributes) {
  auto proxy =
      Operation::ConnectionProxy(Operation::OwnedConnection(std::move(conn)));
  auto connP = proxy.get();
  auto ret = connP->createOperation(std::move(proxy), std::move(multi_query));
  if (attributes.size() > 0) {
    ret->setAttributes(attributes);
  }
  Duration timeout = ret->connection()->conn_options_.getQueryTimeout();
  if (timeout.count() > 0) {
    ret->setTimeout(timeout);
  }
  ret->connection()->mysql_client_->addOperation(ret);
  ret->connection()->socket_handler_.setOperation(ret.get());

  // MultiQueryStreamHandler needs to be alive while the operation is running.
  // To accomplish that, ~MultiQueryStreamHandler waits until
  // `postOperationEnded` is called.
  return MultiQueryStreamHandler(ret);
}

std::shared_ptr<QueryOperation> Connection::beginTransaction(
    std::unique_ptr<Connection> conn) {
  return beginQuery(std::move(conn), "BEGIN");
}

std::shared_ptr<QueryOperation> Connection::commitTransaction(
    std::unique_ptr<Connection> conn) {
  return beginQuery(std::move(conn), "COMMIT");
}

std::shared_ptr<QueryOperation> Connection::rollbackTransaction(
    std::unique_ptr<Connection> conn) {
  return beginQuery(std::move(conn), "ROLLBACK");
}

std::shared_ptr<QueryOperation> Connection::beginTransaction(
    std::shared_ptr<QueryOperation>& op) {
  return beginQuery(op, "BEGIN");
}

std::shared_ptr<QueryOperation> Connection::commitTransaction(
    std::shared_ptr<QueryOperation>& op) {
  return beginQuery(op, "COMMIT");
}

std::shared_ptr<QueryOperation> Connection::rollbackTransaction(
    std::shared_ptr<QueryOperation>& op) {
  return beginQuery(op, "ROLLBACK");
}

ConnectionSocketHandler::ConnectionSocketHandler(folly::EventBase* base)
    : EventHandler(base), AsyncTimeout(base), op_(nullptr) {}

void ConnectionSocketHandler::timeoutExpired() noexcept {
  op_->timeoutTriggered();
}

void ConnectionSocketHandler::handlerReady(uint16_t /*events*/) noexcept {
  DCHECK(op_->conn()->isInEventBaseThread());
  CHECK_THROW(
      op_->state_ != OperationState::Completed &&
          op_->state_ != OperationState::Unstarted,
      db::OperationStateException);

  if (op_->state() == OperationState::Cancelling) {
    op_->cancel();
  } else {
    op_->invokeSocketActionable();
  }
}
} // namespace mysql_client
} // namespace common
} // namespace facebook
