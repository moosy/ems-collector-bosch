/*
 * Buderus EMS data collector
 *
 * Copyright (C) 2016 Danny Baumann <dannybaumann@web.de>
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

#ifndef __COMMANDSCHEDULER_H__
#define __COMMANDSCHEDULER_H__

#include <map>
#include <list>
#include <boost/asio.hpp>
#include "EmsMessage.h"
#include "Noncopyable.h"

class EmsCommandClient
{
    public:
	virtual void onIncomingMessage(const EmsMessage& message) = 0;
	virtual void onTimeout() = 0;
};

class EmsCommandSender : public boost::noncopyable
{
    public:
	typedef boost::shared_ptr<EmsMessage> MessagePtr;
	typedef boost::shared_ptr<EmsCommandClient> ClientPtr;

	EmsCommandSender(boost::asio::io_service& ios) :
	    m_responseTimeout(ios),
	    m_sendTimer(ios)
        {}
	~EmsCommandSender() {
	    m_responseTimeout.cancel();
	    m_sendTimer.cancel();
	}

	void handlePcMessage(const EmsMessage& message);
	void sendMessage(ClientPtr& client, MessagePtr& message);

    protected:
	virtual void sendMessageImpl(const EmsMessage& message) = 0;

    private:
	void continueWithNextRequest();
	void scheduleResponseTimeout(bool fakeAnswer);
	void sendMessage(const MessagePtr& message);
	void doSendMessage(const EmsMessage& message);

    private:
	static const unsigned int RequestTimeout = 1000; /* ms */
	static const long MinDistanceBetweenRequests = 100; /* ms */

	ClientPtr m_currentClient;
	std::list<std::pair<ClientPtr, MessagePtr> > m_pending;
	boost::asio::deadline_timer m_responseTimeout;
	boost::asio::deadline_timer m_sendTimer;
	std::map<uint8_t, boost::posix_time::ptime> m_lastCommTimes;
};

#endif /* __COMMANDSCHEDULER_H__ */
