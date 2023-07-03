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

#include <boost/foreach.hpp>
#include <boost/tokenizer.hpp>
#include <boost/program_options.hpp>
#include "Options.h"

namespace bpo = boost::program_options;

std::string Options::m_target;
std::string Options::m_mqttTarget;
std::string Options::m_mqttPrefix;
unsigned int Options::m_rateLimit = 0;
DebugStream Options::m_debugStreams[DebugCount];
std::string Options::m_pidFilePath;
bool Options::m_daemonize = true;
std::string Options::m_dbPath;
std::string Options::m_dbUser;
std::string Options::m_dbPass;
std::string Options::m_dbName;
unsigned int Options::m_commandPort = 0;
unsigned int Options::m_dataPort = 0;
Options::RoomControllerType Options::m_rcType = Options::RCUnknown;

static void
usage(std::ostream& stream, const char *programName,
      bpo::options_description& options)
{
    stream << "Usage: " << programName << " [options] <target>" << std::endl;
    stream << std::endl << "Possible values for target:" << std::endl;
    stream << "  serial:<device>     Connect to serial device <device> without sending support (e.g. Atmega8)" << std::endl;
    stream << "  tx-serial:<device>  Connect to serial device <device> with sending support (e.g. EMS Gateway)" << std::endl;
    stream << "  tcp:<host>:<port>   Connect to TCP address <host> at <port> (e.g. NetIO)" << std::endl;
    stream << options << std::endl;
}

Options::ParseResult
Options::parse(int argc, char *argv[])
{
    std::string defaultPidFilePath;
    std::string config, rcType;

    defaultPidFilePath = "/var/run/";
    defaultPidFilePath += argv[0];
    defaultPidFilePath += ".pid";

    bpo::options_description general("General options");
    general.add_options()
	("help,h", "Show this help message")
	("rc-type,R", bpo::value<std::string>(&rcType)->composing(),
	 "Type of used room controller (rc30 or rc35)")
	("ratelimit,r", bpo::value<unsigned int>(&m_rateLimit)->default_value(60),
	 "Rate limit (in s) for writing numeric sensor values into DB")
	("debug,d", bpo::value<std::string>()->default_value("none"),
	 "Comma separated list of debug flags (all, io, message, data, stats, none) "
	 " and their files, e.g. message=/tmp/messages.txt");

    bpo::options_description daemon("Daemon options");
    daemon.add_options()
#ifdef HAVE_DAEMONIZE
	("pid-file,P",
	 bpo::value<std::string>(&m_pidFilePath)->default_value(defaultPidFilePath),
	 "Pid file path")
	("foreground,f", "Run in foreground")
#endif
	("config-file,c", bpo::value<std::string>(&config),
	 "File name to read configuration from");

#ifdef HAVE_MYSQL
    bpo::options_description db("Database options");
    db.add_options()
	("db-path", bpo::value<std::string>(&m_dbPath)->composing(),
	 "Path or server:port specification of database server (none to not connect to DB)")
	("db-user,u", bpo::value<std::string>(&m_dbUser)->composing(),
	 "Database user name")
	("db-pass,p", bpo::value<std::string>(&m_dbPass)->composing(),
	 "Database password")
	("db-name,n", bpo::value<std::string>(&m_dbName)->composing(),
	 "Database name");
#endif

    bpo::options_description tcp("TCP options");
    tcp.add_options()
	("command-port,C", bpo::value<unsigned int>(&m_commandPort)->composing(),
	 "TCP port for remote command interface (0 to disable)")
	("data-port,D", bpo::value<unsigned int>(&m_dataPort)->composing(),
	 "TCP port for broadcasting live sensor data (0 to disable)");

#ifdef HAVE_MQTT
    bpo::options_description interface("Interface options");
    interface.add_options()
	("mqtt-broker", bpo::value<std::string>(&m_mqttTarget)->composing(),
	 "MQTT broker address (<host>:<port>)")
	("mqtt-prefix", bpo::value<std::string>(&m_mqttPrefix)->composing(),
	 "MQTT topic prefix (default: /ems)");
#endif

    bpo::options_description hidden("Hidden options");
    hidden.add_options()
	("target", bpo::value<std::string>(&m_target), "Connection target");

    bpo::options_description options;
    options.add(general);
    options.add(daemon);
#ifdef HAVE_MYSQL
    options.add(db);
#endif
    options.add(tcp);
#ifdef HAVE_MQTT
    options.add(interface);
#endif
    options.add(hidden);

    bpo::options_description configOptions;
    configOptions.add(general);
#ifdef HAVE_MYSQL
    configOptions.add(db);
#endif
    configOptions.add(tcp);
#ifdef HAVE_MQTT
    configOptions.add(interface);
#endif

    bpo::options_description visible;
    visible.add(general);
    visible.add(daemon);
#ifdef HAVE_MYSQL
    visible.add(db);
#endif
    visible.add(tcp);
#ifdef HAVE_MQTT
    visible.add(interface);
#endif

    bpo::positional_options_description p;
    p.add("target", 1);

    bpo::variables_map variables;
    try {
	bpo::store(bpo::command_line_parser(argc, argv).options(options).positional(p).run(),
		   variables);
	bpo::notify(variables);

	if (!config.empty()) {
	    std::ifstream configFile(config.c_str());
	    bpo::store(bpo::parse_config_file(configFile, configOptions), variables);
	    bpo::notify(variables);
	}
    } catch (bpo::unknown_option& e) {
	usage(std::cerr, argv[0], visible);
	return ParseFailure;
    } catch (bpo::multiple_occurrences& e) {
	usage(std::cerr, argv[0], visible);
	return ParseFailure;
    }

    if (variables.count("help")) {
	usage(std::cout, argv[0], visible);
	return CloseAfterParse;
    }

    /* check for missing variables */
    if (!variables.count("target")) {
	usage(std::cerr, argv[0], visible);
	return ParseFailure;
    }

    if (variables.count("rc-type")) {
	std::string type = variables["rc-type"].as<std::string>();
	if (type == "rc30") {
	    m_rcType = Options::RC30;
	} else if (type == "rc35") {
	    m_rcType = Options::RC35;
	} else {
	    usage(std::cerr, argv[0], visible);
	    return ParseFailure;
	}
    }

    if (variables.count("foreground")) {
	m_daemonize = false;
    }

    if (variables.count("debug")) {
	std::string flags = variables["debug"].as<std::string>();
	if (flags == "none") {
	    for (unsigned int i = 0; i < DebugCount; i++) {
		m_debugStreams[i].reset();
	    }
	} else if (flags.substr(0, 3) == "all") {
	    size_t start = flags.find('=', 3);
	    std::string file;
	    if (start != std::string::npos) {
		file = flags.substr(start + 1);
	    }
	    for (unsigned int i = 0; i < DebugCount; i++) {
		m_debugStreams[i].setFile(file);
	    }
	} else {
	    boost::char_separator<char> sep(",");
	    boost::tokenizer<boost::char_separator<char> > tokens(flags, sep);
	    BOOST_FOREACH(const std::string& item, tokens) {
		std::string file;
		size_t start = item.find('=');
		unsigned int module;

		if (item.compare(0, 2, "io") == 0) {
		    module = DebugIo;
		} else if (item.compare(0, 7, "message") == 0) {
		    module = DebugMessages;
		} else if (item.compare(0, 4, "data") == 0) {
		    module = DebugData;
		} else {
		    continue;
		}

		if (start != std::string::npos) {
		    file = item.substr(start + 1);
		}
		m_debugStreams[module].setFile(file);
	    }
	}
    }

    return ParseSuccess;
}

