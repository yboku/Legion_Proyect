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

#ifndef _DATABASEWORKERPOOL_H
#define _DATABASEWORKERPOOL_H

#include "Common.h"
#include "MySQLConnection.h"
#include "Transaction.h"
#include "DatabaseWorker.h"
#include "PreparedStatement.h"
#include "Log.h"
#include "QueryResult.h"
#include "QueryHolder.h"
#include "AdhocStatement.h"
#include <mysqld_error.h>
#include "DatabaseEnvFwd.h"
#include "QueryCallback.h"

#define MIN_MYSQL_SERVER_VERSION 50100u
#define MIN_MYSQL_CLIENT_VERSION 50100u

class PingOperation : public SQLOperation
{
    bool Execute() override
    {
        m_conn->Ping();
        return true;
    }
};

template <class T>
class DatabaseWorkerPool
{
    enum InternalIndex
    {
        IDX_ASYNC,
        IDX_SYNCH,
        IDX_SIZE
    };

    ProducerConsumerQueue<SQLOperation*>* _queue;   //! Queue shared by async worker threads.
    std::vector<std::vector<T*>> _connections;
    uint32 _connectionCount[2];                     //! Counter of MySQL connections;
    MySQLConnectionInfo _connectionInfo;
	uint8 _async_threads, _synch_threads;


public:
    DatabaseWorkerPool()
    {
        _queue = new ProducerConsumerQueue<SQLOperation*>();
        memset(_connectionCount, 0, sizeof(_connectionCount));
        _connections.resize(IDX_SIZE);

        WPFatal(mysql_thread_safe(), "Used MySQL library isn't thread-safe.");
    }

    ~DatabaseWorkerPool()
    {
        _queue->Cancel();
        delete _queue;
    }

	bool PrepareStatements()
	{
		for (auto& connections : _connections)
			for (auto& connection : connections)
			{
				connection->LockIfReady();
				if (!connection->PrepareStatements())
				{
					connection->Unlock();
					Close();
					return false;
				}
				else
					connection->Unlock();
			}

		return true;
	}


	void SetConnectionInfo(std::string const& infoString,
		uint8 const asyncThreads, uint8 const synchThreads)
	{
		_connectionInfo = MySQLConnectionInfo(infoString);

		_async_threads = asyncThreads;
		_synch_threads = synchThreads;
	}

	inline MySQLConnectionInfo GetConnectionInfo() const
	{
		return _connectionInfo;
	}

    bool Open(const std::string& infoString, uint8 async_threads, uint8 synch_threads)
    {
        bool res = true;
        _connectionInfo = MySQLConnectionInfo(infoString);

        TC_LOG_INFO(LOG_FILTER_SQL_DRIVER, "Opening DatabasePool '%s'. Asynchronous connections: %u, synchronous connections: %u.", GetDatabaseName(), async_threads, synch_threads);

        res = OpenConnections(IDX_ASYNC, async_threads);

        if (!res)
            return res;

        res = OpenConnections(IDX_SYNCH, synch_threads);

        if (res)
            TC_LOG_INFO(LOG_FILTER_SQL_DRIVER, "DatabasePool '%s' opened successfully. %u total connections running.", GetDatabaseName(), (_connectionCount[IDX_SYNCH] + _connectionCount[IDX_ASYNC]));

        return res;
    }

    void Close()
    {
        TC_LOG_INFO(LOG_FILTER_SQL_DRIVER, "Closing down DatabasePool '%s'.", GetDatabaseName());

        for (uint8 i = 0; i < _connectionCount[IDX_ASYNC]; ++i)
        {
            T* t = _connections[IDX_ASYNC][i];
            t->Close();         //! Closes the actualy MySQL connection.
        }

        TC_LOG_INFO(LOG_FILTER_SQL_DRIVER, "Asynchronous connections on DatabasePool '%s' terminated. Proceeding with synchronous connections.",
            GetDatabaseName());

        //! Shut down the synchronous connections
        //! There's no need for locking the connection, because DatabaseWorkerPool<>::Close
        //! should only be called after any other thread tasks in the core have exited,
        //! meaning there can be no concurrent access at this point.
        for (uint8 i = 0; i < _connectionCount[IDX_SYNCH]; ++i)
            _connections[IDX_SYNCH][i]->Close();

        TC_LOG_INFO(LOG_FILTER_SQL_DRIVER, "All connections on DatabasePool '%s' closed.", GetDatabaseName());
    }

    /**
        Delayed one-way statement methods.
    */

    //! Enqueues a one-way SQL operation in string format that will be executed asynchronously.
    //! This method should only be used for queries that are only executed once, e.g during startup.
    void Execute(const char* sql)
    {
        if (!sql)
            return;

        BasicStatementTask* task = new BasicStatementTask(sql);
        Enqueue(task);
    }

    //! Enqueues a one-way SQL operation in string format -with variable args- that will be executed asynchronously.
    //! This method should only be used for queries that are only executed once, e.g during startup.
    template<typename Format, typename... Args>
    void PExecute(Format&& sql, Args&&... args)
    {
        if (Trinity::IsFormatEmptyOrNull(sql))
            return;

        Execute(Trinity::StringFormat(std::forward<Format>(sql), std::forward<Args>(args)...).c_str());
    }

    //! Enqueues a one-way SQL operation in prepared statement format that will be executed asynchronously.
    //! Statement must be prepared with CONNECTION_ASYNC flag.
    void Execute(PreparedStatement* stmt)
    {
        PreparedStatementTask* task = new PreparedStatementTask(stmt);
        Enqueue(task);
    }

    /**
        Direct synchronous one-way statement methods.
    */

    //! Directly executes a one-way SQL operation in string format, that will block the calling thread until finished.
    //! This method should only be used for queries that are only executed once, e.g during startup.
    void DirectExecute(const char* sql)
    {
        if (!sql)
            return;

        T* t = GetFreeConnection();
        t->Execute(sql);
        t->Unlock();
    }

    //! Directly executes a one-way SQL operation in string format -with variable args-, that will block the calling thread until finished.
    //! This method should only be used for queries that are only executed once, e.g during startup.
    template<typename Format, typename... Args>
    void DirectPExecute(Format&& sql, Args&&... args)
    {
        if (Trinity::IsFormatEmptyOrNull(sql))
            return;

        DirectExecute(Trinity::StringFormat(std::forward<Format>(sql), std::forward<Args>(args)...).c_str());
    }

    //! Directly executes a one-way SQL operation in prepared statement format, that will block the calling thread until finished.
    //! Statement must be prepared with the CONNECTION_SYNCH flag.
    void DirectExecute(PreparedStatement* stmt)
    {
        T* t = GetFreeConnection();
        t->Execute(stmt);
        t->Unlock();
    }

    /**
        Synchronous query (with resultset) methods.
    */

    //! Directly executes an SQL query in string format that will block the calling thread until finished.
    //! Returns reference counted auto pointer, no need for manual memory management in upper level code.
    QueryResult Query(const char* sql, MySQLConnection* conn = NULL)
    {
        if (!conn)
            conn = GetFreeConnection();

        ResultSet* result = conn->Query(sql);
        conn->Unlock();
        if (!result || !result->GetRowCount())
        {
            delete result;
            return QueryResult(NULL);
        }
        result->NextRow();
        return QueryResult(result);
    }

    //! Directly executes an SQL query in string format -with variable args- that will block the calling thread until finished.
    //! Returns reference counted auto pointer, no need for manual memory management in upper level code.
    template<typename Format, typename... Args>
    QueryResult PQuery(Format&& sql, T* conn, Args&&... args)
    {
        if (Trinity::IsFormatEmptyOrNull(sql))
            return QueryResult(nullptr);

        return Query(Trinity::StringFormat(std::forward<Format>(sql), std::forward<Args>(args)...).c_str(), conn);
    }

    //! Directly executes an SQL query in string format -with variable args- that will block the calling thread until finished.
    //! Returns reference counted auto pointer, no need for manual memory management in upper level code.
    template<typename Format, typename... Args>
    QueryResult PQuery(Format&& sql, Args&&... args)
    {
        if (Trinity::IsFormatEmptyOrNull(sql))
            return QueryResult(nullptr);

        return Query(Trinity::StringFormat(std::forward<Format>(sql), std::forward<Args>(args)...).c_str());
    }

    //! Directly executes an SQL query in prepared format that will block the calling thread until finished.
    //! Returns reference counted auto pointer, no need for manual memory management in upper level code.
    //! Statement must be prepared with CONNECTION_SYNCH flag.
    PreparedQueryResult Query(PreparedStatement* stmt)
    {
        T* t = GetFreeConnection();
        PreparedResultSet* ret = t->Query(stmt);
        t->Unlock();

        //! Delete proxy-class. Not needed anymore
        delete stmt;

        if (!ret || !ret->GetRowCount())

        {
            delete ret;
            return PreparedQueryResult(NULL);
        }
        return PreparedQueryResult(ret);
    }

    /**
        Asynchronous query (with resultset) methods.
    */

    //! Enqueues a query in string format that will set the value of the QueryResultFuture return object as soon as the query is executed.
    //! The return value is then processed in ProcessQueryCallback methods.
    QueryCallback AsyncQuery(const char* sql)
    {
        BasicStatementTask* task = new BasicStatementTask(sql, true);
        // Store future result before enqueueing - task might get already processed and deleted before returning from this method
        QueryResultFuture result = task->GetFuture();
        Enqueue(task);
        return QueryCallback(std::move(result));
    }

    void CallBackQuery(const char* sql, std::function<void(QueryResult)> callback)
    {
        BasicStatementTask* task = new BasicStatementTask(sql, true, true, std::move(callback));
        Enqueue(task);
    }

    //! Enqueues a query in prepared format that will set the value of the PreparedQueryResultFuture return object as soon as the query is executed.
    //! The return value is then processed in ProcessQueryCallback methods.
    //! Statement must be prepared with CONNECTION_ASYNC flag.
    QueryCallback AsyncQuery(PreparedStatement* stmt)
    {
        PreparedStatementTask* task = new PreparedStatementTask(stmt, true);
        PreparedQueryResultFuture result = task->GetFuture();
        Enqueue(task);
        return QueryCallback(std::move(result));
    }

    void CallBackQuery(PreparedStatement* stmt, std::function<void(PreparedQueryResult)> callback)
    {
        PreparedStatementTask* task = new PreparedStatementTask(stmt, true, true, std::move(callback));
        Enqueue(task);
    }

    //! Enqueues a vector of SQL operations (can be both adhoc and prepared) that will set the value of the QueryResultHolderFuture
    //! return object as soon as the query is executed.
    //! The return value is then processed in ProcessQueryCallback methods.
    //! Any prepared statements added to this holder need to be prepared with the CONNECTION_ASYNC flag.
    QueryResultHolderFuture DelayQueryHolder(SQLQueryHolder* holder)
    {
        SQLQueryHolderTask* task = new SQLQueryHolderTask(holder);
        // Store future result before enqueueing - task might get already processed and deleted before returning from this method
        QueryResultHolderFuture result = task->GetFuture();
        Enqueue(task);
        return result;
    }

    /**
        Transaction context methods.
    */

    //! Begins an automanaged transaction pointer that will automatically rollback if not commited. (Autocommit=0)
    SQLTransaction BeginTransaction()
    {
        return std::make_shared<Transaction>();
    }

    //! Enqueues a collection of one-way SQL operations (can be both adhoc and prepared). The order in which these operations
    //! were appended to the transaction will be respected during execution.
    void CommitTransaction(SQLTransaction transaction, std::function<void()>&& callback = []() -> void {})
    {
#ifdef TRINITY_DEBUG
        //! Only analyze transaction weaknesses in Debug mode.
        //! Ideally we catch the faults in Debug mode and then correct them,
        //! so there's no need to waste these CPU cycles in Release mode.
        switch (transaction->GetSize())
        {
        case 0:
            TC_LOG_DEBUG(LOG_FILTER_SQL_DRIVER, "Transaction contains 0 queries. Not executing.");
            return;
        case 1:
            TC_LOG_DEBUG(LOG_FILTER_SQL_DRIVER, "Warning: Transaction only holds 1 query, consider removing Transaction context in code.");
            break;
        default:
            break;
        }
#endif // TRINITY_DEBUG

        Enqueue(new TransactionTask(transaction, std::move(callback)));
    }

    //! Directly executes a collection of one-way SQL operations (can be both adhoc and prepared). The order in which these operations
    //! were appended to the transaction will be respected during execution.
    void DirectCommitTransaction(SQLTransaction& transaction)
    {
        MySQLConnection* con = GetFreeConnection();
        int errorCode = con->ExecuteTransaction(transaction);
        if (!errorCode)
        {
            con->Unlock();      // OK, operation succesful
            return;
        }

        //! Handle MySQL Errno 1213 without extending deadlock to the core itself
        //! TODO: More elegant way
        if (errorCode == ER_LOCK_DEADLOCK)
        {
            uint8 loopBreaker = 5;
            for (uint8 i = 0; i < loopBreaker; ++i)
            {
                if (!con->ExecuteTransaction(transaction))
                    break;
            }
        }

        //! Clean up now.
        transaction->Cleanup();

        con->Unlock();
    }

    //! Method used to execute prepared statements in a diverse context.
    //! Will be wrapped in a transaction if valid object is present, otherwise executed standalone.
    void ExecuteOrAppend(SQLTransaction& trans, PreparedStatement* stmt)
    {
        if (!trans)
            Execute(stmt);
        else
            trans->Append(stmt);
    }

    //! Method used to execute ad-hoc statements in a diverse context.
    //! Will be wrapped in a transaction if valid object is present, otherwise executed standalone.
    void ExecuteOrAppend(SQLTransaction& trans, const char* sql)
    {
        if (!trans)
            Execute(sql);
        else
            trans->Append(sql);
    }

    /**
        Other
    */

    //! Automanaged (internally) pointer to a prepared statement object for usage in upper level code.
    //! Pointer is deleted in this->Query(PreparedStatement*) or PreparedStatementTask::~PreparedStatementTask.
    //! This object is not tied to the prepared statement on the MySQL context yet until execution.
    PreparedStatement* GetPreparedStatement(uint32 index)
    {
        return new PreparedStatement(index, _connections[IDX_SYNCH][0]->m_queries[index].capacity);
    }

    //! Apply escape string'ing for current collation. (utf8)
    void EscapeString(std::string& str)
    {
        if (str.empty())
            return;

        char* buf = new char[str.size() * 2 + 1];
        EscapeString(buf, str.c_str(), str.size());
        str = buf;
        delete[] buf;
    }

    //! Keeps all our MySQL connections alive, prevent the server from disconnecting us.
    void KeepAlive()
    {
        //! Ping synchronous connections
        for (uint8 i = 0; i < _connectionCount[IDX_SYNCH]; ++i)
        {
            T* t = _connections[IDX_SYNCH][i];
            if (t->LockIfReady())
            {
                t->Ping();
                t->Unlock();
            }
        }

        //! Assuming all worker threads are free, every worker thread will receive 1 ping operation request
        //! If one or more worker threads are busy, the ping operations will not be split evenly, but this doesn't matter
        //! as the sole purpose is to prevent connections from idling.
        for (size_t i = 0; i < _connections[IDX_ASYNC].size(); ++i)
            Enqueue(new PingOperation);
    }

    char const* GetDatabaseName() const
    {
        return _connectionInfo.database.c_str();
    }

    void WaitExecution()
    {
        while (!_queue->Empty())
        {
        }
    }
private:
    bool OpenConnections(InternalIndex type, uint8 numConnections)
    {
        _connections[type].resize(numConnections);
        for (uint8 i = 0; i < numConnections; ++i)
        {
            T* t;

            if (type == IDX_ASYNC)
                t = new T(_queue, _connectionInfo);
            else if (type == IDX_SYNCH)
                t = new T(_connectionInfo);
            else
                ASSERT(false);

            _connections[type][i] = t;
            ++_connectionCount[type];

            bool res = t->Open();

            if (res)
            {
                if (mysql_get_server_version(t->GetHandle()) < MIN_MYSQL_SERVER_VERSION)
                {
                    TC_LOG_ERROR(LOG_FILTER_SQL_DRIVER, "TrinityCore does not support MySQL versions below 5.1");
                    res = false;
                }
            }

            // Failed to open a connection or invalid version, abort and cleanup
            if (!res)
            {
                TC_LOG_ERROR(LOG_FILTER_SQL_DRIVER, "DatabasePool %s NOT opened. There were errors opening the MySQL connections. Check your SQLDriverLogFile "
                    "for specific errors. Read wiki at http://collab.kpsn.org/display/tc/TrinityCore+Home", GetDatabaseName());

                while (_connectionCount[type] != 0)
                {
                    T* t = _connections[type][i--];
                    delete t;
                    --_connectionCount[type];
                }

                return false;
            }
        }

        return true;
    }

    unsigned long EscapeString(char *to, const char *from, unsigned long length)
    {
        if (!to || !from || !length)
            return 0;

        return mysql_real_escape_string(_connections[IDX_SYNCH][0]->GetHandle(), to, from, length);
    }

    void Enqueue(SQLOperation* op)
    {
        _queue->Push(op);
    }

    //! Gets a free connection in the synchronous connection pool.
    //! Caller MUST call t->Unlock() after touching the MySQL context to prevent deadlocks.
    T* GetFreeConnection()
    {
        uint8 i = 0;
        size_t num_cons = _connectionCount[IDX_SYNCH];
        //! Block forever until a connection is free
        for (;;)
        {
            T* t = _connections[IDX_SYNCH][++i % num_cons];
            //! Must be matched with t->Unlock() or you will get deadlocks
            if (t->LockIfReady())
                return t;
        }

        //! This will be called when Celine Dion learns to sing
        return NULL;
    }
};

#endif
