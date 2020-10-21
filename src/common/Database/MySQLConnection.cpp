/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include "Common.h"

#ifdef _WIN32
  #include <winsock2.h>
#endif
#include <mysql.h>
#include <mysqld_error.h>
#include <errmsg.h>

#include "MySQLConnection.h"
#include "MySQLThreading.h"
#include "QueryResult.h"
#include "SQLOperation.h"
#include "PreparedStatement.h"
#include "DatabaseWorker.h"
#include "Timer.h"
#include "Log.h"
#include "ProducerConsumerQueue.h"

MySQLConnection::MySQLConnection(MySQLConnectionInfo& connInfo) :
m_reconnecting(false),
m_prepareError(false),
m_queue(NULL),
m_worker(NULL),
m_Mysql(NULL),
m_connectionInfo(connInfo),
m_connectionFlags(CONNECTION_SYNCH)
{
}

MySQLConnection::MySQLConnection(ProducerConsumerQueue<SQLOperation*>* queue, MySQLConnectionInfo& connInfo) :
m_reconnecting(false),
m_prepareError(false),
m_queue(queue),
m_Mysql(NULL),
m_connectionInfo(connInfo),
m_connectionFlags(CONNECTION_ASYNC)
{
    m_worker = new DatabaseWorker(m_queue, this);
}

MySQLConnection::~MySQLConnection()
{
    for (size_t i = 0; i < m_stmts.size(); ++i)
        delete m_stmts[i];

    if (m_Mysql)
        mysql_close(m_Mysql);

    if (m_worker)
        delete m_worker;
}

void MySQLConnection::Close()
{
    /// Only close us if we're not operating
    delete this;
}

bool MySQLConnection::Open()
{
    MYSQL *mysqlInit;
    mysqlInit = mysql_init(NULL);
    if (!mysqlInit)
    {
        TC_LOG_ERROR(LOG_FILTER_SQL, "Could not initialize Mysql connection to database `%s`", m_connectionInfo.database.c_str());
        return false;
    }

    int port;
    char const* unix_socket;

    mysql_options(mysqlInit, MYSQL_SET_CHARSET_NAME, "utf8");
    #ifdef _WIN32
    if (m_connectionInfo.host == ".")                                           // named pipe use option (Windows)
    {
        unsigned int opt = MYSQL_PROTOCOL_PIPE;
        mysql_options(mysqlInit, MYSQL_OPT_PROTOCOL, (char const*)&opt);
        port = 0;
        unix_socket = 0;
    }
    else                                                    // generic case
    {
        port = atoi(m_connectionInfo.port_or_socket.c_str());
        unix_socket = 0;
    }
    #else
    if (m_connectionInfo.host == ".")                                           // socket use option (Unix/Linux)
    {
        unsigned int opt = MYSQL_PROTOCOL_SOCKET;
        mysql_options(mysqlInit, MYSQL_OPT_PROTOCOL, (char const*)&opt);
        m_connectionInfo.host = "localhost";
        port = 0;
        unix_socket = m_connectionInfo.port_or_socket.c_str();
    }
    else                                                    // generic case
    {
        port = atoi(m_connectionInfo.port_or_socket.c_str());
        unix_socket = 0;
    }
    #endif

    m_Mysql = mysql_real_connect(mysqlInit, m_connectionInfo.host.c_str(), m_connectionInfo.user.c_str(),
        m_connectionInfo.password.c_str(), m_connectionInfo.database.c_str(), port, unix_socket, 0);

    if (m_Mysql)
    {
        if (!m_reconnecting)
        {
            TC_LOG_INFO(LOG_FILTER_SQL, "MySQL client library: %s", mysql_get_client_info());
            TC_LOG_INFO(LOG_FILTER_SQL, "MySQL server ver: %s ", mysql_get_server_info(m_Mysql));
            if (mysql_get_server_version(m_Mysql) != mysql_get_client_version())
                TC_LOG_INFO(LOG_FILTER_SQL, "[WARNING] MySQL client/server version mismatch; may conflict with behaviour of prepared statements.");
        }

        TC_LOG_INFO(LOG_FILTER_SQL, "Connected to MySQL database at %s", m_connectionInfo.host.c_str());
        mysql_autocommit(m_Mysql, 1);

        // set connection properties to UTF8 to properly handle locales for different
        // server configs - core sends data in UTF8, so MySQL must expect UTF8 too
        mysql_set_character_set(m_Mysql, "utf8");
        return PrepareStatements();
    }
    else
    {
        TC_LOG_ERROR(LOG_FILTER_SQL, "Could not connect to MySQL database at %s: %s\n", m_connectionInfo.host.c_str(), mysql_error(mysqlInit));
        mysql_close(mysqlInit);
        return false;
    }
}

bool MySQLConnection::PrepareStatements()
{
    DoPrepareStatements();
    return !m_prepareError;
}

bool MySQLConnection::Execute(const char* sql)
{
    if (!m_Mysql)
        return false;

    {
        uint32 _s = getMSTime();

        if (mysql_query(m_Mysql, sql))
        {
            uint32 lErrno = mysql_errno(m_Mysql);

            TC_LOG_INFO(LOG_FILTER_SQL, "SQL: %s", sql);
            TC_LOG_ERROR(LOG_FILTER_SQL, "[%u] %s", lErrno, mysql_error(m_Mysql));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return Execute(sql);       // Try again

            return false;
        }
        else
        {
            #ifdef WIN32
            uint32 _ms = getMSTimeDiff(_s, getMSTime());
            //if (_ms > 3)
                TC_LOG_DEBUG(LOG_FILTER_SQL, "[%u ms] SQL: %s", _ms, sql);
            #endif
        }
    }

    return true;
}

bool MySQLConnection::Execute(PreparedStatement* stmt)
{
    if (!m_Mysql)
        return false;

    uint32 index = stmt->m_index;
    volatile std::string query = m_queries[index].query;
    {
        MySQLPreparedStatement* m_mStmt = GetPreparedStatement(index);
        if(!m_mStmt)
        {
            TC_LOG_ERROR(LOG_FILTER_SQL, "[%u] %s index %i", mysql_errno(m_Mysql), mysql_error(m_Mysql), index);
            return false;
        }
        //ASSERT(m_mStmt);            // Can only be null if preparation failed, server side error or bad query
        m_mStmt->m_stmt = stmt;     // Cross reference them for debug output

        stmt->BindParameters(m_mStmt);

        MYSQL_STMT* msql_STMT = m_mStmt->GetSTMT();
        MYSQL_BIND* msql_BIND = m_mStmt->GetBind();

        uint32 _s = getMSTime();

        if (mysql_stmt_bind_param(msql_STMT, msql_BIND))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            TC_LOG_ERROR(LOG_FILTER_SQL, "SQL(p): %s\n [ERROR]: [%u] %s", m_mStmt->getQueryString(m_queries[index].query).c_str(), lErrno, mysql_stmt_error(msql_STMT));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return Execute(stmt);       // Try again

            m_mStmt->ClearParameters();
            return false;
        }

        if (mysql_stmt_execute(msql_STMT))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            TC_LOG_ERROR(LOG_FILTER_SQL, "SQL(p): %s\n [ERROR]: [%u] %s", m_mStmt->getQueryString(m_queries[index].query).c_str(), lErrno, mysql_stmt_error(msql_STMT));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return Execute(stmt);       // Try again

            m_mStmt->ClearParameters();
            return false;
        }

        #ifdef WIN32
        uint32 _ms = getMSTimeDiff(_s, getMSTime());
        //if (_ms > 3)
            TC_LOG_DEBUG(LOG_FILTER_SQL, "[%u ms] SQL(p): %s", _ms, m_mStmt->getQueryString(m_queries[index].query).c_str());
        #endif
        m_mStmt->ClearParameters();
        return true;
    }
}

QResult MySQLConnection::_Query(PreparedStatement* stmt, MYSQL_RES **pResult, uint64* pRowCount, uint32* pFieldCount)
{
    if (!m_Mysql)
        return QResult(false, NULL);

    uint32 index = stmt->m_index;
    volatile std::string query = m_queries[index].query;
    {
        MySQLPreparedStatement* m_mStmt = GetPreparedStatement(index);
        ASSERT(m_mStmt);            // Can only be null if preparation failed, server side error or bad query
        m_mStmt->m_stmt = stmt;     // Cross reference them for debug output

        stmt->BindParameters(m_mStmt);

        MYSQL_STMT* msql_STMT = m_mStmt->GetSTMT();
        MYSQL_BIND* msql_BIND = m_mStmt->GetBind();

        uint32 _s = getMSTime();

        if (mysql_stmt_bind_param(msql_STMT, msql_BIND))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            TC_LOG_ERROR(LOG_FILTER_SQL, "SQL(p): %s\n [ERROR]: [%u] %s", m_mStmt->getQueryString(m_queries[index].query).c_str(), lErrno, mysql_stmt_error(msql_STMT));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return _Query(stmt, pResult, pRowCount, pFieldCount);       // Try again

            m_mStmt->ClearParameters();
            return QResult(false, NULL);
        }

        if (mysql_stmt_execute(msql_STMT))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            TC_LOG_ERROR(LOG_FILTER_SQL, "SQL(p): %s\n [ERROR]: [%u] %s",
                m_mStmt->getQueryString(m_queries[index].query).c_str(), lErrno, mysql_stmt_error(msql_STMT));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return _Query(stmt, pResult, pRowCount, pFieldCount);      // Try again

            m_mStmt->ClearParameters();
            return QResult(false, NULL);
        }

        #ifdef WIN32
        uint32 _ms = getMSTimeDiff(_s, getMSTime());
        //if (_ms > 3)
            TC_LOG_DEBUG(LOG_FILTER_SQL, "[%u ms] SQL(p): %s", _ms, m_mStmt->getQueryString(m_queries[index].query).c_str());
        #endif
        m_mStmt->ClearParameters();

        *pResult = mysql_stmt_result_metadata(msql_STMT);
        *pRowCount = mysql_stmt_num_rows(msql_STMT);
        *pFieldCount = mysql_stmt_field_count(msql_STMT);

        return QResult(true, m_mStmt);;
    }
}

ResultSet* MySQLConnection::Query(const char* sql)
{
    if (!sql)
        return NULL;

    MYSQL_RES *result = NULL;
    MYSQL_FIELD *fields = NULL;
    uint64 rowCount = 0;
    uint32 fieldCount = 0;

    if (!_Query(sql, &result, &fields, &rowCount, &fieldCount))
        return NULL;

    return new ResultSet(result, fields, rowCount, fieldCount);
}

bool MySQLConnection::_Query(const char *sql, MYSQL_RES **pResult, MYSQL_FIELD **pFields, uint64* pRowCount, uint32* pFieldCount)
{
    if (!m_Mysql)
        return false;

    {
        uint32 _s = getMSTime();

        if (mysql_query(m_Mysql, sql))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            TC_LOG_INFO(LOG_FILTER_SQL, "SQL: %s", sql);
            TC_LOG_ERROR(LOG_FILTER_SQL, "[%u] %s", lErrno, mysql_error(m_Mysql));

            if (lErrno == ER_BAD_FIELD_ERROR || lErrno == ER_NO_SUCH_TABLE || lErrno == ER_PARSE_ERROR)
                TC_LOG_ERROR(LOG_FILTER_SQL, "TRANSFERT ERROR %u : query : %s", lErrno, sql);

            if (_HandleMySQLErrno(lErrno))      // If it returns true, an error was handled successfully (i.e. reconnection)
                return _Query(sql, pResult, pFields, pRowCount, pFieldCount);    // We try again

            return false;
        }
        else
        {
            #ifdef WIN32
            uint32 _ms = getMSTimeDiff(_s, getMSTime());
            //if (_ms > 3)
                TC_LOG_DEBUG(LOG_FILTER_SQL, "[%u ms] SQL: %s", _ms, sql);
            #endif
        }

        *pResult = mysql_store_result(m_Mysql);
        *pRowCount = mysql_affected_rows(m_Mysql);
        *pFieldCount = mysql_field_count(m_Mysql);
    }

    if (!*pResult )
        return false;

    if (!*pRowCount)
    {
        mysql_free_result(*pResult);
        return false;
    }

    *pFields = mysql_fetch_fields(*pResult);

    return true;
}

void MySQLConnection::BeginTransaction()
{
    Execute("START TRANSACTION");
}

void MySQLConnection::RollbackTransaction()
{
    Execute("ROLLBACK");
}

void MySQLConnection::CommitTransaction()
{
    Execute("COMMIT");
}

int MySQLConnection::ExecuteTransaction(SQLTransaction& transaction)
{
    std::list<SQLElementData> const& queries = transaction->m_queries;
    if (queries.empty())
        return -1;

    BeginTransaction();

    std::list<SQLElementData>::const_iterator itr;
    for (itr = queries.begin(); itr != queries.end(); ++itr)
    {
        SQLElementData const& data = *itr;
        switch (itr->type)
        {
            case SQL_ELEMENT_PREPARED:
            {
                PreparedStatement* stmt = data.element.stmt;
                ASSERT(stmt);
                if (!Execute(stmt))
                {
                    TC_LOG_WARN(LOG_FILTER_SQL, "Transaction aborted. %u queries not executed.", (uint32)queries.size());
                    int errorCode = GetLastError();
                    RollbackTransaction();
                    return errorCode;
                }
            }
            break;
            case SQL_ELEMENT_RAW:
            {
                const char* sql = data.element.query;
                ASSERT(sql);
                if (!Execute(sql))
                {
                    TC_LOG_WARN(LOG_FILTER_SQL, "Transaction aborted. %u queries not executed.", (uint32)queries.size());
                    int errorCode = GetLastError();
                    RollbackTransaction();
                    return errorCode;
                }
            }
            break;
        }
    }

    // we might encounter errors during certain queries, and depending on the kind of error
    // we might want to restart the transaction. So to prevent data loss, we only clean up when it's all done.
    // This is done in calling functions DatabaseWorkerPool<T>::DirectCommitTransaction and TransactionTask::Execute,
    // and not while iterating over every element.

    CommitTransaction();
    return 0;
}

MySQLPreparedStatement* MySQLConnection::GetPreparedStatement(uint32 index)
{
    ASSERT(index < m_stmts.size());
    MySQLPreparedStatement* ret = m_stmts[index];
    if (!ret)
        TC_LOG_ERROR(LOG_FILTER_SQL, "Could not fetch prepared statement %u on database `%s`, connection type: %s.",
            index, m_connectionInfo.database.c_str(), (m_connectionFlags & CONNECTION_ASYNC) ? "asynchronous" : "synchronous");

    return ret;
}

void MySQLConnection::PrepareStatement(uint32 index, std::string const& sql, ConnectionFlags flags)
{
    if (!m_reconnecting)
    {
        // Pre-calculate the size of the prepared statements
        size_t const m_argument_count = std::count(sql.begin(), sql.end(), '?');

        // TC only supports uint8 indices.
        ASSERT(m_argument_count < std::numeric_limits<uint8>::max());

        m_queries.insert(std::make_pair(index, PreparedStatementInfo(sql, static_cast<uint8>(m_argument_count), flags)));
    }
    // For reconnection case
    else
        delete m_stmts[index];

    // Check if specified query should be prepared on this connection
    // i.e. don't prepare async statements on synchronous connections
    // to save memory that will not be used.
    if (!(m_connectionFlags & flags))
    {
        m_stmts[index] = NULL;
        return;
    }

    MYSQL_STMT* stmt = mysql_stmt_init(m_Mysql);
    if (!stmt)
    {
        TC_LOG_ERROR(LOG_FILTER_SQL, "In mysql_stmt_init() id: %u, sql: \"%s\"", index, sql.c_str());
        TC_LOG_ERROR(LOG_FILTER_SQL, "%s", mysql_error(m_Mysql));
        m_prepareError = true;
    }
    else
    {
        if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.length())))
        {
            TC_LOG_ERROR(LOG_FILTER_SQL, "In mysql_stmt_prepare() id: %u, sql: \"%s\"", index, sql.c_str());
            TC_LOG_ERROR(LOG_FILTER_SQL, "%s", mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            m_prepareError = true;
        }
        else
        {
            MySQLPreparedStatement* mStmt = new MySQLPreparedStatement(stmt);
            m_stmts[index] = mStmt;
        }
    }
}

PreparedResultSet* MySQLConnection::Query(PreparedStatement* stmt)
{
    MYSQL_RES *result = NULL;
    uint64 rowCount = 0;
    uint32 fieldCount = 0;

    QResult res = _Query(stmt, &result, &rowCount, &fieldCount);
    if (!res.res)
        return NULL;

    if (mysql_more_results(m_Mysql))
    {
        mysql_next_result(m_Mysql);
    }

    return new PreparedResultSet(res.m_stmt->GetSTMT(), result, rowCount, fieldCount);
}

bool MySQLConnection::_HandleMySQLErrno(uint32 errNo)
{
    switch (errNo)
    {
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_LOST:
        // case CR_INVALID_CONN_HANDLE:
        case CR_SERVER_LOST_EXTENDED:
        {
            m_reconnecting = true;
            uint64 oldThreadId = mysql_thread_id(GetHandle());
            mysql_close(GetHandle());
            if (this->Open())                           // Don't remove 'this' pointer unless you want to skip loading all prepared statements....
            {
                TC_LOG_INFO(LOG_FILTER_SQL, "Connection to the MySQL server is active.");
                if (oldThreadId != mysql_thread_id(GetHandle()))
                    TC_LOG_INFO(LOG_FILTER_SQL, "Successfully reconnected to %s @%s:%s (%s).",
                        m_connectionInfo.database.c_str(), m_connectionInfo.host.c_str(), m_connectionInfo.port_or_socket.c_str(),
                            (m_connectionFlags & CONNECTION_ASYNC) ? "asynchronous" : "synchronous");

                m_reconnecting = false;
                return true;
            }

            uint32 lErrno = mysql_errno(GetHandle());   // It's possible this attempted reconnect throws 2006 at us. To prevent crazy recursive calls, sleep here.
            std::this_thread::sleep_for(std::chrono::seconds(3)); // Sleep 3 seconds
            return _HandleMySQLErrno(lErrno);                     // Call self (recursive)
        }

        case ER_LOCK_DEADLOCK:
            return false;    // Implemented in TransactionTask::Execute and DatabaseWorkerPool<T>::DirectCommitTransaction
        // Query related errors - skip query
        case ER_WRONG_VALUE_COUNT:
        case ER_DUP_ENTRY:
            return false;

        // Outdated table or database structure - terminate core
        case ER_BAD_FIELD_ERROR:
        case ER_NO_SUCH_TABLE:
            TC_LOG_ERROR(LOG_FILTER_SQL, "Your database structure is not up to date. Please make sure you've executed all queries in the sql/updates folders.");
            std::this_thread::sleep_for(Seconds(10));
            std::abort();
            return false;
        case ER_PARSE_ERROR:
            TC_LOG_ERROR(LOG_FILTER_SQL, "Error while parsing SQL. Core fix required.");
            std::this_thread::sleep_for(Seconds(10));
            std::abort();
            return false;
        default:
            TC_LOG_ERROR(LOG_FILTER_SQL, "Unhandled MySQL errno %u. Unexpected behaviour possible.", errNo);
            return false;
    }
}
