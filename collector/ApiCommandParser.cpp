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

#include <boost/date_time.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include "ApiCommandParser.h"
#include "ByteOrder.h"
#include "Options.h"

/* version of our command API */
#define API_VERSION "2023070601"

ApiCommandParser::ApiCommandParser(EmsCommandSender& sender,
				   const boost::shared_ptr<EmsCommandClient>& client,
				   ValueCache *cache,
				   OutputCallback outputCb,
				   boost::asio::io_service& ios) :
    m_sender(sender),
    m_client(client),
    m_cache(cache),
    m_outputCb(outputCb),
    m_responseCounter(0),
    m_parsePosition(0),
    m_outputRawData(false),
    testModeRepeater(ios)
    
{
}

static const char * scheduleNames[] = {
    "custom1", "family", "morning", "early", "evening", "forenoon",
    "afternoon", "noon", "single", "senior", "custom2"
};
static const size_t scheduleNameCount = sizeof(scheduleNames) / sizeof(scheduleNames[0]);

static const char * dayNames[] = {
    "monday", "tuesday", "wednesday", "thursday",
    "friday", "saturday", "sunday"
};
static const size_t dayNameCount = sizeof(dayNames) / sizeof(dayNames[0]);

ApiCommandParser::CommandResult
ApiCommandParser::parse(std::istream& request)
{
    if (m_activeRequest) {
	return Busy;
    }

    std::string category;
    request >> category;

    if (category == "help") {
	output("Available commands (help with '<command> help'):\n"
		"hk[1|2|3|4]\n"
		"ww\n"
		"uba\n"
		"rc\n"
#if defined(HAVE_RAW_READWRITE_COMMAND)
		"raw\n"
#endif
		"cache\n"
		"getversion\n"
		"OK");
	return Ok;
    } else if (category == "hk1") {
	return handleHkCommand(request, 61);
    } else if (category == "hk2") {
	return handleHkCommand(request, 71);
    } else if (category == "hk3") {
	return handleHkCommand(request, 81);
    } else if (category == "hk4") {
	return handleHkCommand(request, 91);
    } else if (category == "ww") {
	return handleWwCommand(request);
    } else if (category == "rc") {
	return handleRcCommand(request);
    } else if (category == "uba") {
	return handleUbaCommand(request);
#if defined (HAVE_RAW_READWRITE_COMMAND)
    } else if (category == "raw") {
	return handleRawCommand(request);
#endif
    } else if (category == "cache") {
	return handleCacheCommand(request);
    } else if (category == "getversion") {
	output("collector version: " API_VERSION);
	startRequest(EmsProto::addressUBA2, 0x02, 0, 3);
	return Ok;
    }

    return InvalidCmd;
}

ApiCommandParser::CommandResult
ApiCommandParser::handleRcCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "help") {
	output("Available subcommands:\n"
	       "getcontactinfo\n"
	       "setcontactinfo 1|2|3 <text>\n"
		"settime YYYY-MM-DD HH:MM:SS\n"
		"OK");
	return Ok;
    } else if (cmd == "settime") {
	std::locale prevLocale = request.imbue(std::locale(std::locale::classic(),
		new boost::local_time::local_time_input_facet("%Y-%m-%d %H:%M:%S")));
	boost::posix_time::ptime time;

	request >> time;
	request.imbue(prevLocale);

	if (!request) {
	    return InvalidArgs;
	}

	boost::gregorian::date date = time.date();
	boost::posix_time::time_duration timeOfDay = time.time_of_day();

	EmsProto::SystemTimeRecord record;
	memset(&record, 0, sizeof(record));

	record.common.year = date.year() - 2000;
	record.common.month = date.month();
	record.common.day = date.day();
	record.common.hour = timeOfDay.hours();
	record.common.minute = timeOfDay.minutes();
	record.second = timeOfDay.seconds();
	switch (date.day_of_week()) {
	    case boost::date_time::Monday: record.dayOfWeek = 0; break;
	    case boost::date_time::Tuesday: record.dayOfWeek = 1; break;
	    case boost::date_time::Wednesday: record.dayOfWeek = 2; break;
	    case boost::date_time::Thursday: record.dayOfWeek = 3; break;
	    case boost::date_time::Friday: record.dayOfWeek = 4; break;
	    case boost::date_time::Saturday: record.dayOfWeek = 5; break;
	    case boost::date_time::Sunday: record.dayOfWeek = 6; break;
	}

	sendCommand(EmsProto::addressUI800, 0x06, 0, (uint8_t *) &record, sizeof(record));
	return Ok;
    } else if (cmd == "getcontactinfo") {
        startRequest(EmsProto::addressUI800, 0x0137, 0, 120);
        return Ok;
    } else if (cmd == "setcontactinfo") {
        unsigned int line;
        std::ostringstream buffer;
        std::string text;

        request >> line;
        if (!request || line < 1 || line > 3) {
            return InvalidArgs;
        }

        while (request) {
            std::string token;
            request >> token;
            buffer << token << " ";
        }

        // make sure there's at least 20 characters in there
        buffer << "                     ";

        text = buffer.str();  // Special Chars have more than one byte, so no cropping here...


        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert; 
        std::u16string wtxt = convert.from_bytes(text).substr(0,20); // ...we crop the widestring instead


        uint8_t buf2[20];
       
        for (int chunk=0; chunk<2; chunk++){
       
          for (size_t i = 0; i < 10; i++) {
            buf2[2*i+1] = (uint8_t) (wtxt[i+chunk*10] & 0xFF);
            buf2[2*i] = (uint8_t) ((wtxt[i+chunk*10] >> 8) & 0xFF);
          }
        
          sendCommand(EmsProto::addressUI800, 0x0137, chunk * 20 + (line - 1) * 40, (uint8_t *) buf2, 20);
        }

        output("\nOK");
        return Ok;
    }

    return InvalidCmd;
}


void 
ApiCommandParser::refreshTestMode()
{
    uint8_t data[1];
    memset(&data, 0, sizeof(data));
    data[0] = 0x5a;

    sendCommand(EmsProto::addressUBA2, 0x1d, 0, data, sizeof(data));
    testModeRepeater.expires_from_now(boost::posix_time::milliseconds(5000));
    testModeRepeater.async_wait([this] (const boost::system::error_code& error) {
       if (error != boost::asio::error::operation_aborted) refreshTestMode();

    });


}


ApiCommandParser::CommandResult
ApiCommandParser::handleUbaCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "help") {
	output("Available subcommands:\n"
	        "testmode <on|off>\n"
	        "teststate <burnerpct> <fanpct> <pumppct> <3way012> <zirkpump01> <ignition01> <ionisator01>\n"
		"OK");
	return Ok;


    } else if (cmd == "testmode") {

        std::string mode;
        request >> mode;

        if (mode == "on") {

            testModeRepeater.expires_from_now(boost::posix_time::milliseconds(1000));
            testModeRepeater.async_wait([this] (const boost::system::error_code& error) {
              refreshTestMode();
            
            });
            
            output("OK");        

            return Ok;
            

        } else if (mode == "off") {

            testModeRepeater.cancel();
            
            uint8_t data[1];
            memset(&data, 0, sizeof(data));
            data[0] = 0x00;

            sendCommand(EmsProto::addressUBA2, 0x1d, 0, data, sizeof(data));

            output("OK");        

            return Ok;
        }

        return InvalidArgs;
        
        
    } else if (cmd == "teststate") {
        std::string mode;
        uint8_t data[13];

        memset(&data, 0, sizeof(data));
        unsigned int brennerPercent, fanPercent, pumpePercent, threeWayMode;
        bool zirkPumpOn, ionisatorOn, ignitionOn;

        request >> brennerPercent;
        if (!request || brennerPercent > 100) {
            brennerPercent = 0;
        }

        request >> fanPercent;
        if (!request || fanPercent > 100) {
            fanPercent = 0;
        }

        
        request >> pumpePercent;
        if (!request || pumpePercent > 100) {
            pumpePercent = 0;
        }
        
        request >> threeWayMode;
        if (!request || threeWayMode > 2) {
            threeWayMode = 0;
        }
                  
        request >> zirkPumpOn;
        if (!request) {
            zirkPumpOn = false;
        }

        request >> ignitionOn;
        if (!request) {
            ignitionOn = false;
        }

        request >> ionisatorOn;
        if (!request) {
            ionisatorOn = false;
        }


        data[0] = brennerPercent;
        data[2] = pumpePercent;
        data[3] = threeWayMode;
        data[4] = zirkPumpOn ? 1 : 0;
        data[7] = ignitionOn ? 1 : 0;
        data[9] = fanPercent;
        data[12] = ionisatorOn ? 1 : 0;
        
        sendCommand(EmsProto::addressUBA2, 0x2d, 0, data, sizeof(data));
        return Ok;
    }

    return InvalidCmd;
}

#if defined(HAVE_RAW_READWRITE_COMMAND)
ApiCommandParser::CommandResult
ApiCommandParser::handleRawCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "help") {
	output("Available subcommands:\n"
		"read <target> <type> <offset> <len>\n"
		"write <target> <type> <offset> <data>\n"
		"OK");
	return Ok;
    } else if (cmd == "read") {
        uint16_t type;
	uint8_t target, offset, len;
	if (!parseIntParameter(request, target, UCHAR_MAX)     ||
		!parseIntParameter(request, type, USHRT_MAX)   ||
		!parseIntParameter(request, offset, UCHAR_MAX) ||
		!parseIntParameter(request, len, UCHAR_MAX)) {
	    return InvalidArgs;
	}
	startRequest(target, type, offset, len, true, true);
	return Ok;
    } else if (cmd == "write") {
        uint16_t type;
	uint8_t target, offset, value;
	if (!parseIntParameter(request, target, UCHAR_MAX)     ||
		!parseIntParameter(request, type, USHRT_MAX)   ||
		!parseIntParameter(request, offset, UCHAR_MAX) ||
		!parseIntParameter(request, value, UCHAR_MAX)) {
	    return InvalidArgs;
	}

	sendCommand(target, type, offset, &value, 1);
	
	return Ok;
    }

    return InvalidCmd;
}
#endif

ApiCommandParser::CommandResult
ApiCommandParser::handleCacheCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (m_cache) {
	if (cmd == "help") {
	    output("Available subcommands:\n"
		   "fetch <key>\n"
		   "OK");
	    return Ok;
	} else if (cmd == "fetch") {
	    std::ostringstream stream;
	    std::vector<std::string> selector;

	    while (request) {
		std::string token;
		request >> token;
		if (!token.empty()) {
		    selector.push_back(token);
		}
	    }

	    m_cache->outputValues(selector, stream);
	    output(stream.str());
	    output("OK");
	    return Ok;
	}
    }

    return InvalidCmd;
}

ApiCommandParser::CommandResult
ApiCommandParser::handleHkCommand(std::istream& request, uint16_t type)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "help") {
	output("Available subcommands:\n"
	       "mode off|manual|auto\n"
	       "manualtemp <temp>\n"
	       "temporarytemp <temp>|off\n"
	       "activateboost on|off\n"
	       "boosthours <hours>\n"
	       "boosttemp <temp>\n"
	       "requestdata\n"
		"OK");
	return Ok;


    } else if (cmd == "requestdata") {
        startRequest(EmsProto::addressUI800, 0x01b9, 0, 32, true, true);
        startRequest(EmsProto::addressUI800, 0x01a5, 0, 46, true, true);
        return Ok;

    } else if (cmd == "mode") {
        uint8_t data;
        std::string mode;

        request >> mode;

        if (mode == "off")        data = 0x00;
        else if (mode == "manual") data = 0x01;
        else if (mode == "auto")  data = 0x02;
        else return InvalidArgs;

        sendCommand(EmsProto::addressUI800, 0x01b9, 21, &data, 1);
        return Ok;

    } else if (cmd == "manualtemp") {
        uint8_t temperature;
        if (!parseIntParameter(request, temperature, 30) || temperature < 5) {
            return InvalidArgs;
        }
        // FIXME: Add support for x.5 temp values
        temperature = temperature << 1;
        sendCommand(EmsProto::addressUI800, 0x01b9, 22, &temperature, 1);
        return Ok;

    } else if (cmd == "temporarytemp") {
        uint8_t temperature;
        if (!parseIntParameter(request, temperature, 30) || temperature < 0) {
            temperature = 0xff;  // disable
        } else {
            // FIXME: Add support for x.5 temp values

            temperature = temperature << 1;
        }
        sendCommand(EmsProto::addressUI800, 0x01b9, 8, &temperature, 1);
        return Ok;

    } else if (cmd == "activateboost") {
        uint8_t data;
        std::string mode;

        request >> mode;

        if (mode == "off")        data = 0x00;
        else if (mode == "on")    data = 0xff;
        else return InvalidArgs;

        sendCommand(EmsProto::addressUI800, 0x01b9, 23, &data, 1);
        return Ok;

    } else if (cmd == "boosttemp") {
        uint8_t temperature;
        if (!parseIntParameter(request, temperature, 30) || temperature < 5) {
            return InvalidArgs;
        }
        // FIXME: Add support for x.5 temp values
        temperature = temperature << 1;
        sendCommand(EmsProto::addressUI800, 0x01b9, 25, &temperature, 1);
        return Ok;

    } else if (cmd == "boosthours") {
        uint8_t hours;
        if (!parseIntParameter(request, hours, 8) || hours < 1) {
            return InvalidArgs;
        }
        sendCommand(EmsProto::addressUI800, 0x01b9, 24, &hours, 1);
        return Ok;

    }
    return InvalidCmd;
}

ApiCommandParser::CommandResult
ApiCommandParser::handleSingleByteValue(std::istream& request, uint8_t dest, uint16_t type,
					 uint8_t offset, int multiplier, int min, int max)
{
    float value;
    int valueInt;
    int8_t valueByte;

    request >> value;
    if (!request) {
	return InvalidArgs;
    }

    try {
	valueInt = boost::numeric_cast<int>(multiplier * value);
	if (valueInt < min * multiplier || valueInt > max * multiplier) {
	    return InvalidArgs;
	}
	valueByte = valueInt;
    } catch (boost::numeric::bad_numeric_cast& e) {
	return InvalidArgs;
    }

    sendCommand(dest, type, offset, (uint8_t *) &valueByte, 1);
    return Ok;
}

ApiCommandParser::CommandResult
ApiCommandParser::handleWwCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "help") {
	output("Available subcommands:\n"
	       "mode off|eco|comfort|followheater|auto \n"
	       "zirkmode off|on|followww|auto \n"
               "zirksperhour <number> \n"
               "comforttemp <temp> \n"
               "reducedtemp <temp> \n"
               "extratemp <temp> \n"
               "extra15mins <extra duration quarterly hours> \n"
               "extra on|off \n"
               "requestdata \n"
		"OK");
	return Ok;

    } else if (cmd == "mode") {
        uint8_t data;
        std::string mode;

        request >> mode;

        if (mode == "off")        data = 0x00;
        else if (mode == "eco") data = 0x01;
        else if (mode == "comfort") data = 0x02;
        else if (mode == "followheater") data = 0x03;
        else if (mode == "auto")  data = 0x04;
        else return InvalidArgs;

        sendCommand(EmsProto::addressUI800, 0x01f5, 2, &data, 1);
        return Ok;

    } else if (cmd == "zirkmode") {
        uint8_t data;
        std::string mode;

        request >> mode;

        if (mode == "off")        data = 0x00;
        else if (mode == "on") data = 0x01;
        else if (mode == "followww") data = 0x02;
        else if (mode == "auto")  data = 0x03;
        else return InvalidArgs;

        sendCommand(EmsProto::addressUI800, 0x01f5, 3, &data, 1);
        return Ok;


    } else if (cmd == "requestdata") {
        startRequest(EmsProto::addressUBA2, 0xea, 0, 25, true, true);
        startRequest(EmsProto::addressUI800, 0x01f5, 0, 21, true, true);

        return Ok;

    } else if (cmd == "comforttemp") {
        uint8_t temperature;
        if (!parseIntParameter(request, temperature, 80) || temperature < 30) {
            return InvalidArgs;
        }
        sendCommand(EmsProto::addressUBA2, 0xea, 6, &temperature, 1);
        return Ok;

    } else if (cmd == "reducedtemp") {
        uint8_t temperature;
        if (!parseIntParameter(request, temperature, 80) || temperature < 30) {
            return InvalidArgs;
        }
        sendCommand(EmsProto::addressUBA2, 0xea, 18, &temperature, 1);
        return Ok;

    } else if (cmd == "extratemp") {
        uint8_t temperature;
        if (!parseIntParameter(request, temperature, 80) || temperature < 30) {
            return InvalidArgs;
        }
        sendCommand(EmsProto::addressUBA2, 0xea, 16, &temperature, 1);
        return Ok;

    } else if (cmd == "extra15mins") {
        uint8_t duration;
        if (!parseIntParameter(request, duration, 16)) {
            return InvalidArgs;
        }
        sendCommand(EmsProto::addressUI800, 0x01f5, 10, &duration, 1);
        return Ok;

    } else if (cmd == "zirksperhour") {
        uint8_t duration;
        if (!parseIntParameter(request, duration, 10)) {
            return InvalidArgs;
        }
        sendCommand(EmsProto::addressUBA2, 0xea, 11, &duration, 1);
        return Ok;
        
        
   } else if (cmd == "extra") {
        uint8_t data;
        std::string mode;

        request >> mode;

        if (mode == "off")        data = 0x00;
        else if (mode == "on") data = 0xff;
        else return InvalidArgs;

        sendCommand(EmsProto::addressUI800, 0x01f5, 11, &data, 1);
        return Ok;
        
    }
    
    return InvalidCmd;
}


boost::tribool
ApiCommandParser::onIncomingMessage(const EmsMessage& message)
{
    
    if (!m_activeRequest) {
	return boost::indeterminate;
    }


    const std::vector<uint8_t>& data = message.getData();
    uint8_t source = message.getSource();
    uint16_t type = message.getType();
    uint8_t offset = message.getOffset();

    if (type == 0xff) {
	m_activeRequest.reset();
	return offset != 0x04;
    }

    if (source != m_requestDestination ||
	    type != m_requestType      ||
	    offset != (m_requestResponse.size() + m_requestOffset)) {
	/* likely a response to a request we already retried, ignore it */
	return boost::indeterminate;
    }


    if (data.empty()) {
	// no more data is available
	m_requestLength = m_requestResponse.size();
    } else {
	m_requestResponse.insert(m_requestResponse.end(), data.begin(), data.end());
    }


    boost::tribool result;

    if (m_outputRawData) {
	if (!continueRequest()) {
	    std::ostringstream outputStream;
	    for (size_t i = 0; i < m_requestResponse.size(); i++) {
		outputStream << boost::format("0x%02x ") % (unsigned int) m_requestResponse[i];
	    }
	    output(outputStream.str());
	    result = true;
	} else {
	    result = boost::indeterminate;
	}
    } else {
	result = handleResponse();
    }

    if (result) {
	m_activeRequest.reset();
    }
    return result;
}

boost::tribool
ApiCommandParser::handleResponse()
{
    switch (m_requestType) {
	case 0x02: /* get version */ {
	    static const struct {
		uint8_t source;
		const char *name;
	    } SOURCES[] = {
		{ EmsProto::addressUBA2, "UBA2" },
		{ EmsProto::addressUI800, "UI800" },
		{ EmsProto::addressRH800, "RH800" }

	    };
	    static const size_t SOURCECOUNT = sizeof(SOURCES) / sizeof(SOURCES[0]);

	    unsigned int major = m_requestResponse[1];
	    unsigned int minor = m_requestResponse[2];
	    size_t index;

	    for (index = 0; index < SOURCECOUNT; index++) {
		if (m_requestDestination == SOURCES[index].source) {
		    boost::format f("%s version: %d.%02d");
		    f % SOURCES[index].name % major % minor;
		    output(f.str());
		    break;
		}
	    }
	    if (index >= (SOURCECOUNT - 1)) {
		return true;
	    }
	    startRequest(SOURCES[index + 1].source, 0x02, 0, 3);
	    break;
	}
	
        case 0x0137: { /* get contact info */
            if (!continueRequest()) {
                for (size_t i = 0; i < m_requestResponse.size(); i += 40) {
                    size_t len = std::min(m_requestResponse.size() - i, static_cast<size_t>(40));
                    char buffer[40];
                    memcpy(buffer, &m_requestResponse.at(i), len);
                    
                    std::u16string inln = u"                    "; 
                    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>,char16_t> convert; 

                    for (int i=0; i<20; i++){

                      inln[i] = ((buffer[2*i] & 0xff)<<8)  + (buffer[2*i+1] & 0xff); 
                    }

                    std::string outln = convert.to_bytes(inln);     
                    
                    output(outln);
                }
                return true;
            }
            break;

	}
	
	default:
	    /* unhandled message */
	    return false;
    }

    return boost::indeterminate;
}

template<typename T> boost::tribool
ApiCommandParser::loopOverResponse(const char *prefix)
{
    const size_t msgSize = sizeof(T);
    while (m_parsePosition + msgSize <= m_requestResponse.size()) {
	T *record = (T *) &m_requestResponse.at(m_parsePosition);
	std::string response = buildRecordResponse(record);

	m_parsePosition += msgSize;
	m_responseCounter++;

	if (response.empty()) {
	    return true;
	}

	boost::format f("%s%02d %s");
	f % prefix % m_responseCounter % response;
	output(f.str());
    }

    if (!continueRequest()) {
	return true;
    }

    return boost::indeterminate;
}

bool
ApiCommandParser::onTimeout()
{
    if (!m_activeRequest) {
	return false;
    }
    m_retriesLeft--;
    if (m_retriesLeft == 0) {
	m_activeRequest.reset();
	return true;
    }

    sendActiveRequest();
    return false;
}


std::string
ApiCommandParser::buildRecordResponse(const EmsProto::ErrorRecord *record)
{
    if (record->errorAscii[0] == 0) {
	/* no error at this position */
	return "";
    }

    std::ostringstream response;

    if (record->time.valid) {
	response << boost::format("%04d-%02d-%02d %02d:%02d")
		% (2000 + record->time.year) % (unsigned int) record->time.month
		% (unsigned int) record->time.day % (unsigned int) record->time.hour
		% (unsigned int) record->time.minute;
    } else {
	response  << "xxxx-xx-xx xx:xx";
    }

    response << " ";
    response << boost::format("%02x %c%c %d %d")
	    % (unsigned int) record->source % record->errorAscii[0] % record->errorAscii[1]
	    % BE16_TO_CPU(record->code_be16) % BE16_TO_CPU(record->durationMinutes_be16);

    return response.str();
}

std::string
ApiCommandParser::buildRecordResponse(const EmsProto::ScheduleEntry *entry)
{
    if (entry->time >= 0x90) {
	/* unset */
	return "";
    }

    unsigned int minutes = entry->time * 10;
    boost::format f("%s %02d:%02d %s");
    f % dayNames[entry->day / 2] % (minutes / 60) % (minutes % 60) % (entry->on ? "on" : "off");

    return f.str();
}

bool
ApiCommandParser::parseScheduleEntry(std::istream& request, EmsProto::ScheduleEntry *entry)
{
    std::string day, time, mode;

    request >> day;
    if (!request) {
	return false;
    }

    if (day == "unset") {
	entry->on = 7;
	entry->day = 0xe;
	entry->time = 0x90;
	return true;
    }

    request >> time >> mode;
    if (!request) {
	return false;
    }

    if (mode == "on") {
	entry->on = 1;
    } else if (mode == "off") {
	entry->on = 0;
    } else {
	return false;
    }

    bool hasDay = false;
    for (size_t i = 0; i < dayNameCount; i++) {
	if (day == dayNames[i]) {
	    entry->day = 2 * i;
	    hasDay = true;
	    break;
	}
    }
    if (!hasDay) {
	return false;
    }

    size_t pos = time.find(":");
    if (pos == std::string::npos) {
	return false;
    }
    try {
	unsigned int hours = boost::lexical_cast<unsigned int>(time.substr(0, pos));
	unsigned int minutes = boost::lexical_cast<unsigned int>(time.substr(pos + 1));
	if (hours > 23 || minutes >= 60 || (minutes % 10) != 0) {
	    return false;
	}

	entry->time = (uint8_t) ((hours * 60 + minutes) / 10);
    } catch (boost::bad_lexical_cast& e) {
	return false;
    }

    return true;
}

std::string
ApiCommandParser::buildRecordResponse(const char *type, const EmsProto::HolidayEntry *entry)
{
    boost::format f("%s %04d-%02d-%02d");
    f % type % (2000 + entry->year) % (unsigned int) entry->month % (unsigned int) entry->day;

    return f.str();
}

bool
ApiCommandParser::parseHolidayEntry(const std::string& string, EmsProto::HolidayEntry *entry)
{
    size_t pos = string.find('-');
    if (pos == std::string::npos) {
	return false;
    }

    size_t pos2 = string.find('-', pos + 1);
    if (pos2 == std::string::npos) {
	return false;
    }

    try {
	unsigned int year = boost::lexical_cast<unsigned int>(string.substr(0, pos));
	unsigned int month = boost::lexical_cast<unsigned int>(string.substr(pos + 1, pos2 - pos - 1));
	unsigned int day = boost::lexical_cast<unsigned int>(string.substr(pos2 + 1));
	if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
	    return false;
	}

	entry->year = (uint8_t) (year - 2000);
	entry->month = (uint8_t) month;
	entry->day = (uint8_t) day;
    } catch (boost::bad_lexical_cast& e) {
	return false;
    }

    return true;
}

void
ApiCommandParser::startRequest(uint8_t dest, uint16_t type, size_t offset,
			        size_t length, bool newRequest, bool raw)
{
    DebugStream& debug = Options::messageDebug();
    debug << "STARTREQUEST: dest=";
    debug << boost::format("0x%02x ") % (unsigned int) dest << " type=";
    debug << boost::format("0x%04x ") % (unsigned int) type << " offset=";
    debug << boost::format("%d ") % (unsigned int) offset << " \n";


    m_requestOffset = offset;
    m_requestLength = length;
    m_requestDestination = dest;
    m_requestType = type;
    m_requestResponse.clear();
    m_requestResponse.reserve(length);
    m_parsePosition = 0;
    m_outputRawData = raw;
    if (newRequest) {
	m_responseCounter = 0;
    }

    continueRequest();
}

bool
ApiCommandParser::continueRequest()
{

    size_t alreadyReceived = m_requestResponse.size();

    if (alreadyReceived >= m_requestLength) {
	return false;
    }

    uint8_t offset = (uint8_t) (m_requestOffset + alreadyReceived);
    uint8_t remaining = (uint8_t) (m_requestLength - alreadyReceived);

    sendCommand(m_requestDestination, m_requestType, offset, &remaining, 1, true);
    return true;
}

void
ApiCommandParser::sendCommand(uint8_t dest, uint16_t type, uint8_t offset,
			       const uint8_t *data, size_t count,
			       bool expectResponse)
{   
    std::vector<uint8_t> sendData(data, data + count);

    m_retriesLeft = MaxRequestRetries;

    DebugStream& debug = Options::messageDebug();
    debug << "New EmsMessage: dest=";
    debug << boost::format("0x%02x ") % (unsigned int) dest << " type=";
    debug << boost::format("0x%04x ") % (unsigned int) type << " offset=";
    debug << boost::format("%d ") % (unsigned int) offset << " \n";
//    debug << boost::format("0x%08x ") % sendData <<"\n";
    

    
    m_activeRequest.reset(new EmsMessage(dest, type, offset, sendData, expectResponse));
    sendActiveRequest();
}

template<typename T>bool
ApiCommandParser::parseIntParameter(std::istream& request, T& data, unsigned int max)
{
    unsigned int value;

    request.unsetf(std::ios_base::basefield);
    request >> value;

    if (!request || value > max) {
	return false;
    }

    data = value;
    return true;
}

void
ApiCommandParser::sendActiveRequest()
{
    m_sender.sendMessage(m_client, m_activeRequest);
}
