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

#include <boost/bind/bind.hpp>
#include "CommandScheduler.h"
#include "Options.h"

void
EmsCommandSender::handlePcMessage(const EmsMessage& message)

{
    m_lastCommTimes[message.getSource()] = std::chrono::steady_clock::now();
    /* Only a response to the active request finishes it. A late response to a request that was
     * already retried (or any other message for us) must neither cancel the response timeout nor
     * advance the queue - otherwise the client waits forever and gets neither OK nor ERRTIMEOUT. */
    if (!m_currentClient || !m_currentClient->onIncomingMessage(message)) {
	return;
    }
    m_responseTimeout.cancel();
    continueWithNextRequest();
}

void
EmsCommandSender::sendMessage(ClientPtr& client, MessagePtr& message)
{
    bool wasIdle = !m_currentClient;
    m_pending.push_back(std::make_pair(client, message));
    if (wasIdle) {
	continueWithNextRequest();
    }
}

void
EmsCommandSender::scheduleResponseTimeout(bool fakeAnswer)
{
    if (fakeAnswer){
        m_responseTimeout.expires_after(std::chrono::milliseconds(1000));

        m_responseTimeout.async_wait([this] (const boost::system::error_code& error) {
            if (error != boost::asio::error::operation_aborted) {
                /* Write commands get no response: after a short delay, acknowledge them ourselves.
                 * Deliver directly (not via handlePcMessage(), which would also advance the queue)
                 * and advance the queue exactly once, whether or not the client still waited. */
                if (m_currentClient) {
                    std::vector<uint8_t> fakeData;
                    EmsMessage fake(0x0b, 0xff, 0x01, fakeData, false);
                    m_currentClient->onIncomingMessage(fake);
                }
                continueWithNextRequest();
            }
        });

    } else {

        m_responseTimeout.expires_after(std::chrono::milliseconds(RequestTimeout));

        m_responseTimeout.async_wait([this] (const boost::system::error_code& error) {
            if (error != boost::asio::error::operation_aborted) {
                if (m_currentClient) {

                    m_currentClient->onTimeout();

                }
                continueWithNextRequest();
            }
        });

    }
}

void
EmsCommandSender::sendMessage(const MessagePtr& message)
{
    auto timeIter = m_lastCommTimes.find(message->getDestination());
    bool scheduled = false;

    if (timeIter != m_lastCommTimes.end()) {

        auto now = std::chrono::steady_clock::now();
        auto diff = now - timeIter->second;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() <= MinDistanceBetweenRequests) {
            m_sendTimer.expires_at(timeIter->second + std::chrono::milliseconds(MinDistanceBetweenRequests));
	    m_sendTimer.async_wait(boost::bind(&EmsCommandSender::doSendMessage, this, *message));
	    scheduled = true;
	}
    }
    if (!scheduled) {
	doSendMessage(*message);
    }
}

void
EmsCommandSender::doSendMessage(const EmsMessage& message)
{
    bool fakeAnswer = ((message.getDestination() & 0x80) == 0);

    sendMessageImpl(message);
    scheduleResponseTimeout(fakeAnswer);

    m_lastCommTimes[message.getDestination()] = std::chrono::steady_clock::now();
}

void
EmsCommandSender::continueWithNextRequest()
{
    if (m_pending.empty()) {
	m_currentClient.reset();
	return;
    }
    auto& item = m_pending.front();
    m_currentClient = item.first;

    sendMessage(item.second);
    m_pending.pop_front();
}
