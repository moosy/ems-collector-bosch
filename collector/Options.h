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

#ifndef __OPTIONS_H__
#define __OPTIONS_H__

#include <iostream>
#include <fstream>

class DebugStream : public std::ostream
{
    public:
	DebugStream() :
	    std::ostream(std::cout.rdbuf()),
	    m_active(false)
	{ }

	void reset() {
	    m_active = false;
	    rdbuf(std::cout.rdbuf());
	}

	void setFile(const std::string& file) {
	    if (file == "stdout") {
		rdbuf(std::cout.rdbuf());
	    } else if (file == "stderr") {
		rdbuf(std::cerr.rdbuf());
	    } else if (!file.empty()) {
		m_fileBuf.open(file.c_str(), std::ios::out | std::ios::app);
		rdbuf(&m_fileBuf);
	    }
	    m_active = true;
	}

	operator bool() const {
	    return m_active;
	}

    private:
	bool m_active;
	std::filebuf m_fileBuf;
};

class Options
{
    public:
	typedef enum {
	    ParseFailure,
	    ParseSuccess,
	    CloseAfterParse
	} ParseResult;

	typedef enum {
	    RCUnknown,
	    RC30,
	    RC35
	} RoomControllerType;

	static unsigned int rateLimit() {
	    return m_rateLimit;
	}

	static const std::string& target() {
	    return m_target;
	}
	static const std::string& mqttTarget() {
	    return m_mqttTarget;
	}
	static const std::string& mqttPrefix() {
	    return m_mqttPrefix;
	}
	static bool daemonize() {
	    return m_daemonize;
	}
	static const std::string& pidFilePath() {
	    return m_pidFilePath;
	}
	static const std::string& databasePath() {
	    return m_dbPath;
	}
	static const std::string& databaseUser() {
	    return m_dbUser;
	}
	static const std::string& databasePassword() {
	    return m_dbPass;
	}
	static const std::string& databaseName() {
	    return m_dbName;
	}
	static unsigned int commandPort() {
	    return m_commandPort;
	}
	static unsigned int dataPort() {
	    return m_dataPort;
	}

	static RoomControllerType roomControllerType() {
	    return m_rcType;
	}

	static ParseResult parse(int argc, char *argv[]);

    private:
	static const unsigned int DebugIo = 0;
	static const unsigned int DebugMessages = 1;
	static const unsigned int DebugData = 2;
	static const unsigned int DebugCount = 3;
	static DebugStream m_debugStreams[DebugCount];

    public:
	static DebugStream& ioDebug() {
	    return m_debugStreams[DebugIo];
	}
	static DebugStream& messageDebug() {
	    return m_debugStreams[DebugMessages];
	}
	static DebugStream& dataDebug() {
	    return m_debugStreams[DebugData];
	}

    private:
	static std::string m_target;
	static std::string m_mqttTarget;
	static std::string m_mqttPrefix;
	static unsigned int m_rateLimit;
	static std::string m_pidFilePath;
	static bool m_daemonize;
	static std::string m_dbPath;
	static std::string m_dbUser;
	static std::string m_dbPass;
	static std::string m_dbName;
	static unsigned int m_commandPort;
	static unsigned int m_dataPort;
	static RoomControllerType m_rcType;
};

#endif /* __OPTIONS_H__ */
