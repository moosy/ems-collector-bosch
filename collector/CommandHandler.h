/*
 * Buderus EMS data collector
 *
 * Copyright (C) 2011 Danny Baumann <dannybaumann@web.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __COMMANDHANDLER_H__
#define __COMMANDHANDLER_H__

#include <deque>
#include <set>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <memory>
#include "ApiCommandParser.h"
#include "CommandScheduler.h"
#include "EmsMessage.h"
#include "Noncopyable.h"

class CommandHandler;

class CommandConnection : public std::enable_shared_from_this<CommandConnection>,
			  private boost::noncopyable
{
    public:
	typedef std::shared_ptr<CommandConnection> Ptr;

    public:
	CommandConnection(boost::asio::io_context& ios,
			  EmsCommandSender& sender,
			  CommandHandler& handler,
			  ValueCache *cache);

    public:
	boost::asio::ip::tcp::socket& socket() {
	    return m_socket;
	}
	void startRead() {
	    boost::asio::async_read_until(m_socket, m_request, "\n",
		boost::bind(&CommandConnection::handleRequest, shared_from_this(),
			    boost::asio::placeholders::error));
	}

        void initAfterConstruction();   // NEU
	void close() {
	    m_socket.close();
	}
	bool onIncomingMessage(const EmsMessage& message);
	void onTimeout();

    private:
	void handleRequest(const boost::system::error_code& error);
	void handleWrite(const boost::system::error_code& error);


        class CommandClient : public EmsCommandClient {
            public:
                CommandClient(std::weak_ptr<CommandConnection> connection) :
                    m_connection(connection)
                {}
                bool onIncomingMessage(const EmsMessage& message) override {
                    if (auto conn = m_connection.lock()) {
                        return conn->onIncomingMessage(message);
                    }
                    return false;
                }
                void onTimeout() {
                    if (auto conn = m_connection.lock()) {
                        conn->onTimeout();
                    }
                }
            private:
                std::weak_ptr<CommandConnection> m_connection;
        };



	/* Asio allows only one async_write per socket at a time, and the buffer must stay valid
	 * until it completes - so queue the lines and write them one after another. */
	void respond(const std::string& response) {
	    bool idle = m_outQueue.empty();
	    m_outQueue.push_back(response + "\n");
	    if (idle) {
		writeNext();
	    }
	}
	void writeNext() {
	    boost::asio::async_write(m_socket, boost::asio::buffer(m_outQueue.front()),
		boost::bind(&CommandConnection::handleWrite, shared_from_this(),
			    boost::asio::placeholders::error));
	}

    private:
	boost::asio::ip::tcp::socket m_socket;
	boost::asio::streambuf m_request;
	std::deque<std::string> m_outQueue;
	std::shared_ptr<EmsCommandClient> m_commandClient;
	ApiCommandParser m_parser;
	CommandHandler& m_handler;
};

class CommandHandler : private boost::noncopyable
{
    public:
	CommandHandler(boost::asio::io_context& ios,
		       EmsCommandSender& sender,
		       ValueCache *cache,
		       boost::asio::ip::tcp::endpoint& endpoint);
	~CommandHandler();

    public:
	void startConnection(CommandConnection::Ptr connection);
	void stopConnection(CommandConnection::Ptr connection);

    private:
	void handleAccept(CommandConnection::Ptr connection,
			  const boost::system::error_code& error);
	void startAccepting();

    private:
	boost::asio::io_context& m_ios;
	EmsCommandSender& m_sender;
	ValueCache *m_cache;
	boost::asio::ip::tcp::acceptor m_acceptor;
	std::set<CommandConnection::Ptr> m_connections;
};

#endif /* __COMMANDHANDLER_H__ */
