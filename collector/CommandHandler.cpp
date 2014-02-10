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

#include <asm/byteorder.h>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include "CommandHandler.h"
#include "numeric_conversion.cpp"
#define BYTEFORMAT_HEX \
    "0x" << std::setbase(16) << std::setw(2) << std::setfill('0') << (unsigned int)
#define BYTEFORMAT_DEC \
    std::dec << (unsigned int)
        
CommandHandler::CommandHandler(TcpHandler& handler,
			       boost::asio::ip::tcp::endpoint& endpoint) :
    m_handler(handler),
    m_acceptor(handler, endpoint),
    m_sendTimer(handler)
{
    startAccepting();
}

CommandHandler::~CommandHandler()
{
    m_acceptor.close();
    std::for_each(m_connections.begin(), m_connections.end(),
		  boost::bind(&CommandConnection::close, _1));
    m_connections.clear();
    m_sendTimer.cancel();
}

void
CommandHandler::handleAccept(CommandConnection::Ptr connection,
			     const boost::system::error_code& error)
{
    if (error) {
	if (error != boost::asio::error::operation_aborted) {
	    std::cerr << "Accept error: " << error.message() << std::endl;
	}
	return;
    }

    startConnection(connection);
    startAccepting();
}

void
CommandHandler::startConnection(CommandConnection::Ptr connection)
{
    m_connections.insert(connection);
    if (Options::enablecli()) {
      connection->respond("\nBuderus EMS interface extended edition");
      connection->respond("(c) 2014 by Danny Baumann, Michael Moosbauer\n");
      connection->respond("For help type 'help'.\n");
      connection->prompt();
    }
    connection->startRead();
}

void
CommandHandler::stopConnection(CommandConnection::Ptr connection)
{
    m_connections.erase(connection);
    connection->close();
}

void
CommandHandler::handlePcMessage(const EmsMessage& message)
{
    m_lastCommTimes[message.getSource()] = boost::posix_time::microsec_clock::universal_time();

    std::for_each(m_connections.begin(), m_connections.end(),
		  boost::bind(&CommandConnection::handlePcMessage,
			      _1, message));
}

void
CommandHandler::startAccepting()
{
    CommandConnection::Ptr connection(new CommandConnection(*this));
    m_acceptor.async_accept(connection->socket(),
		            boost::bind(&CommandHandler::handleAccept, this,
					connection, boost::asio::placeholders::error));
}

void
CommandHandler::sendMessage(const EmsMessage& msg)
{
    std::map<uint8_t, boost::posix_time::ptime>::iterator timeIter = m_lastCommTimes.find(msg.getDestination());
    bool scheduled = false;

    if (timeIter != m_lastCommTimes.end()) {
	boost::posix_time::ptime now(boost::posix_time::microsec_clock::universal_time());
	boost::posix_time::time_duration diff = now - timeIter->second;

	if (diff.total_milliseconds() <= MinDistanceBetweenRequests) {
	    m_sendTimer.expires_at(timeIter->second + boost::posix_time::milliseconds(MinDistanceBetweenRequests));
	    m_sendTimer.async_wait(boost::bind(&CommandHandler::doSendMessage, this, msg));
	    scheduled = true;
	}
    }
    if (!scheduled) {
	doSendMessage(msg);
    }
}

void
CommandHandler::doSendMessage(const EmsMessage& msg)
{
    m_handler.sendMessage(msg);
    m_lastCommTimes[msg.getDestination()] = boost::posix_time::microsec_clock::universal_time();
}


CommandConnection::CommandConnection(CommandHandler& handler) :
    m_socket(handler.getHandler()),
    m_handler(handler),
    m_waitingForResponse(false),
    m_responseTimeout(handler.getHandler()),
    m_responseCounter(0),
    m_parsePosition(0)
    
{
    m_showrawdata=false;

}

CommandConnection::~CommandConnection()
{
    m_responseTimeout.cancel();
}

void
CommandConnection::handleRequest(const boost::system::error_code& error)
{
    if (error) {
	if (error != boost::asio::error::operation_aborted) {
	    m_handler.stopConnection(shared_from_this());
	}
	return;
    }

    std::istream requestStream(&m_request);

    if (m_waitingForResponse) {
	respond("ERRBUSY");
    } else if (m_request.size() > 2) {
	CommandResult result = handleCommand(requestStream);

	switch (result) {
	    case Ok:
		break;
	    case InvalidCmd:
		respond("ERRCMD");
		break;
	    case InvalidArgs:
		respond("ERRARGS");
		break;
	}
    }

    /* drain remainder */
    std::string remainder;
    std::getline(requestStream, remainder);
    startRead();
    if (!m_waitingForResponse) prompt();
}

void
CommandConnection::handleWrite(const boost::system::error_code& error)
{
    if (error && error != boost::asio::error::operation_aborted) {
	m_handler.stopConnection(shared_from_this());
    }
}

CommandConnection::CommandResult
CommandConnection::handleCommand(std::istream& request)
{
    std::string category;
    request >> category;
    
    std::string cc_ta, cc_ty, cc_off, cc_len;
    unsigned int ci_ta, ci_ty, ci_off, ci_len;

    if (category == "help") {
        respond("Buderus EMS interface extended edition");
        respond("(c) 2014 by Danny Baumann, Michael Moosbauer\n");
	respond("\nAvailable commands (further help with '<subcommand> help'):\n\n"
	        "  ww <subcommand>               -- control hot water subsystem\n"
	        "  hk[1|2|3|4] <subcommand>      -- control heating subsystem\n"
	        "  uba <subcommand>              -- options for heater\n"
	        "  getversion [me | <deviceid>]  -- read firmware version from myself / device\n"
	        "  totalhours                    -- show total uptime\n"
	        "  getcontactinfo [1|2]          -- get stored service contact info\n"
	        "  setcontactinfo [1|2] <text>   -- set contact info\n"
	        "  geterrors                     -- get system errors\n"
	        "  geterrors2                    -- get blocking errors\n"
	        "  geterrors3                    -- get locking errors\n");
	respond("  emsqry [0xtarget 0xtype \n"
                "               offset len]      -- do custom EMS query\n");
	respond("  emscmd [0xtarget 0xtype \n"
	        "               offset data]     -- write custom value to EMS device\n"
	        "                                   (CAUTION!!)\n");
	return Ok;
    } else if (category == "quit") {
        respond("Bye.");
        m_socket.close();
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
    } else if (category == "totalhours") {
	startRequest(EmsMessage::addressUBA, 0x14, 0, 3);
	return Ok;
    } else if (category == "getcontactinfo") {
        int line;
        request >> line;
        if (!request || (line < 1) || (line > 2)) {
            return InvalidArgs;
        }
        line = (line-1)*21;

	startRequest(EmsMessage::addressRC, 0xa4, line, 22);
	return Ok;
    } else if (category == "setcontactinfo") {
        int line;
        std::string tmp;
        std::string data;
        request >> line;
        if (!request || (line < 1) || (line > 2)) {
            return InvalidArgs;
        }
        line = (line-1)*21;
        
        while (request) {
          request >> tmp;
          data += tmp+" ";
          tmp = "";
        }

        data += "                     ";

	sendCommand(EmsMessage::addressRC, 0xa4, line,reinterpret_cast<const unsigned char *> (data.c_str()), 21);
	return Ok;
    } else if (category == "geterrors") {
	startRequest(EmsMessage::addressRC, 0x12, 0, 4 * sizeof(EmsMessage::ErrorRecord));
	return Ok;
    } else if (category == "geterrors2") {
	startRequest(EmsMessage::addressUBA, 0x10, 0, 4 * sizeof(EmsMessage::ErrorRecord));
	return Ok;
    } else if (category == "geterrors3") {
	startRequest(EmsMessage::addressUBA, 0x11, 0, 4 * sizeof(EmsMessage::ErrorRecord));
	return Ok;

    } else if (category == "getversion") {
        try{
        if (!request) return InvalidCmd;
        request >> cc_ta;
        if (cc_ta=="me"){
        
          respond("\n" VERSIONSTR "\n" );
          return Ok;
        }
        ci_ta = numeric_conversion::hexstring_to_size_t(cc_ta);

        } catch (boost::bad_lexical_cast& e) {
          return InvalidArgs;
        }
                                        
	startRequest(ci_ta, 0x02, 0, 3);
	return Ok;
    } else if (category == "emsqry") {
        m_showrawdata=true;
        try{
        if (!request) return InvalidCmd;
        request >> cc_ta;
        ci_ta = numeric_conversion::hexstring_to_size_t(cc_ta);

        if (!request) return InvalidCmd;
        request >> cc_ty;
        ci_ty = numeric_conversion::hexstring_to_size_t(cc_ty);        

        if (!request) return InvalidCmd;
        request >> cc_off;
        ci_off = boost::lexical_cast<unsigned int>(cc_off);

        if (!request) return InvalidCmd;
        request >> cc_len;
        ci_len = boost::lexical_cast<unsigned int>(cc_len);        

        } catch (boost::bad_lexical_cast& e) {
          return InvalidArgs;
        }
                                        
	startRequest(ci_ta, ci_ty, ci_off, ci_len);
	        
	return Ok;
    } else if (category == "emscmd") {
        m_showrawdata=true;
        try{
          if (!request) return InvalidCmd;
          request >> cc_ta;
          ci_ta = numeric_conversion::hexstring_to_size_t(cc_ta);

          if (!request) return InvalidCmd;
          request >> cc_ty;
          ci_ty = numeric_conversion::hexstring_to_size_t(cc_ty);        

          if (!request) return InvalidCmd;
          request >> cc_off;
          ci_off = boost::lexical_cast<unsigned int>(cc_off);



          float value;
          uint8_t valueByte;
   
          request >> value;
          if (!request) {
              return InvalidArgs;
          }
   
          valueByte = boost::numeric_cast<uint8_t>(value);
          sendCommand(ci_ta, ci_ty, ci_off, &valueByte, 1);
          return Ok;
  
       } catch (boost::numeric::bad_numeric_cast& e) {
           return InvalidArgs;
       }


    } else if (category == "uba") {
       return handleUbaCommand(request);
    }

    return InvalidCmd;
}


CommandConnection::CommandResult
CommandConnection::handleUbaCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "help") {
	respond("\nAvailable subcommands for heater:\n\n"
		"antipendel <minutes>            -- Heater lock time \n"
		"hyst [on|off] <kelvin>          -- Hysteresis temperature\n"
		"getstatus                       -- get heater status info\n"
		"getmaintenance                  -- get maintenance settings\n"
		"setmaintenance [ off | \n"
		"          byhour <hours/100> |\n" 
		"          bydate DD-MM-YYYY ]   -- set maintenance cycle\n"
		"isinmaintenance                 -- is maintenance due?\n"
		"pumpdelay <minutes>             -- pump runtime after heater off\n"
		"pumpmodulation <minpercent> \n"
		"               <maxpercent>     -- min and max pump power\n"
		"testmode [on|off] \n"
		"         <brennerpercent> \n"
                "         <3w-vent:0=heat,1=ww>\n"
                "         <zirkpump:0=off,1=on>  -- start component test\n"
		"                                   use at own risk! \n"
		"                                   repeat command periodically.\n");
	return Ok;


    } else if (cmd == "getmaintenance") {
	startRequest(EmsMessage::addressUBA, 0x15, 0, 5);
	return Ok;
    } else if (cmd == "isinmaintenance") {
	startRequest(EmsMessage::addressUBA, 0x1c, 5, 3);
	return Ok;
    } else if (cmd == "setmaintenance") {
        std::string kind;
        std::string string;
        unsigned int hours;
        uint8_t data[5];


        request >> kind;

        if (kind == "bydate") {

          if (!request) {
              return InvalidArgs;
          }
  
          request >> string;

          size_t pos = string.find('-');
          if (pos == std::string::npos) {
              return InvalidArgs;
          }

          size_t pos2 = string.find('-', pos + 1);
          if (pos2 == std::string::npos) {
              return InvalidArgs;
          }

          unsigned int day = boost::lexical_cast<unsigned int>(string.substr(0, pos));
          unsigned int month = boost::lexical_cast<unsigned int>(string.substr(pos + 1, pos2 - pos - 1));
          unsigned int year = boost::lexical_cast<unsigned int>(string.substr(pos2 + 1));
          if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
              return InvalidArgs;;
          }

         data[0] = 0x02;
         data[1] = 60;
         data[2] = day;
         data[3] = month;
         data[4] = (year-2000);



        } else if (kind == "byhours") {

          if (!request) {
              return InvalidArgs;
          }
  
          request >> hours;

          if (hours > 60) return InvalidArgs;

          data[0] = 0x01;
          data[1] = hours;
          data[2] = 1;
          data[3] = 1;
          data[4] = 4;


        } else if (kind == "off") {

          data[0] = 0;
          data[1] = 60;
          data[2] = 1;
          data[3] = 1;
          data[4] = 4;

        } else {

          return InvalidArgs;

        }

        sendCommand(EmsMessage::addressUBA, 0x15, 0, data, sizeof(data));

	return Ok;


    } else if (cmd == "testmode") {
        std::string mode;
        unsigned int active;
        unsigned int brennerperc;
        unsigned int pumpeperc;
        unsigned int dwventstat;
        unsigned int zirkstat;

        

        uint8_t data[11];

        if (!request) {
            return InvalidArgs;
        }
        request >> mode;

        
        if (mode == "off") {
          active = brennerperc = pumpeperc = dwventstat = zirkstat = 0;
          
        } else {
        
          if (!request || (mode != "on")) {
              return InvalidArgs;
          }
          request >> brennerperc;
          if (brennerperc > 100) return InvalidArgs;


          if (!request) {
              return InvalidArgs;
          }
          request >> pumpeperc;
          if (pumpeperc > 100) return InvalidArgs;


          if (!request) {
              return InvalidArgs;
          }
          request >> dwventstat;
          if (dwventstat > 1) return InvalidArgs;
          if (dwventstat == 1) dwventstat = 255;

          if (!request) {
              return InvalidArgs;
          }
          request >> zirkstat;
          if (zirkstat > 1) return InvalidArgs;
          if (zirkstat == 1) zirkstat = 255;

           active = 0x5a;
        }
        
        data[0] = active;
        data[1] = brennerperc;
        data[2] = 0;
        data[3] = pumpeperc;
        data[4] = dwventstat;
        data[5] = zirkstat;
        data[6] = 0;
        data[7] = 0;
        data[8] = 0;
        data[9] = 0;
        data[10] = 0;


        sendCommand(EmsMessage::addressUBA, 0x1d, 0, data, sizeof(data));

	return Ok;


    } else if (cmd == "antipendel") {
	unsigned int minutes;
	uint8_t data;

	request >> minutes;
	if (!request || minutes > 120) {
	    return InvalidArgs;
	}
	data = minutes;

	sendCommand(EmsMessage::addressUBA, 0x16, 6, &data, 1);
	return Ok;
    } else if (cmd == "pumpdelay") {
	unsigned int minutes;
	uint8_t data;

	request >> minutes;
	if (!request || minutes > 120) {
	    return InvalidArgs;
	}
	data = minutes;

	sendCommand(EmsMessage::addressUBA, 0x16, 8, &data, 1);
	return Ok;
    } else if (cmd == "hyst") {
	std::string direction;
	unsigned int hysteresis;
	uint8_t data;

	request >> direction;
	request >> hysteresis;
	if (!request || (direction != "on" && direction != "off") ) {
	    return InvalidArgs;
	}

	data = hysteresis;
	sendCommand(EmsMessage::addressUBA, 0x16, direction == "on" ? 5 : 4, &data, 1);
	return Ok;
    } else if (cmd == "pumpmodulation") {
	unsigned int min, max;
	uint8_t data[2];

	request >> min >> max;
	if (!request || min > max || max > 100) {
	    return InvalidArgs;
	}

	data[0] = max;
	data[1] = min;

	sendCommand(EmsMessage::addressUBA, 0x16, 9, data, sizeof(data));
	return Ok;
    } else if (cmd == "getstatus") {
	startRequest(EmsMessage::addressUBA, 0x16, 0, 20);
	return Ok;
    }

    return InvalidCmd;
}

CommandConnection::CommandResult
CommandConnection::handleHkCommand(std::istream& request, uint8_t type)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "help") {
	respond("\nAvailable heating subsystem subcommands:\n\n"
		"mode [day|night|auto]           -- operating mode \n"
		"daytemperature <temp>           -- desired day roomtemp\n"
		"nighttemperature <temp>         -- desired temp for reduced mode\n"
		"holidaytemperature <temp>       -- desired temp for vacation\n"
		"getholiday                      -- get holiday time\n"
		"holidaymode <start:DD-MM-YYYY> \n"
		"            <end:DD-MM-YYYY>\n  -- set holiday time\n"
		"getvacation                     -- get vacation time\n"
		"vacationmode <start:DD-MM-YYYY> \n"
		"             <end:DD-MM-YYYY>   -- get vacation time\n"
		"partymode <hours>               -- (de)activate partymode\n"
		"pausemode <hours>               -- (de)activate pausemode\n"
		"minouttemp <temp>               -- minimum outdoor temp for region\n"
                "temptemp <temp, 0=disable>      -- temporarily different roomtemp\n"
		"building [leicht|mittel|schwer] -- building type (for damping)\n"
		"enabledamping [on|off]          -- switch damping on/off\n"
		"minheatflowtemp <temp>          -- minimum temp for heatingwater\n"
		"maxheatflowtemp <temp>          -- maximum temp for heatingwater\n"      
		"redmode [Abschalt|Reduziert|\n"
		"         Raumhalt|Aussenhalt]   -- type of off-time-reduction\n"
		"refinput [Raum|Aussen]          -- temp all calcs are based on \n"
		"refinputvac [Raum|Aussen]       -- same for vacationmode\n"
	        "maxroomeffect <temp>            -- maximum effect of roomtemp\n"
		"designtemp <temp>               -- heatwater temp at min outdoor temp\n"
		"schedoptimizer [on|off]         -- on-off-time schedule optimization\n"
	        "frostmode [off|Raum|Aussen]     -- frost protection type\n"
		"tempoffset <temp>               -- offset for heating curve\n"
		"frosttemp <temp>                -- below this temp frost protect is active\n"
		"summertimetemp <temp>           -- over this temp heater is off\n"
		"stopnighttemp <temp>            -- below this temp off-time-red. is cancelled\n"
		"nightdoredtemp <temp>           -- Aussenhalt: below this red. mode, over off\n"
		"getstatus\n"
		"getstatus2\n"
		"getstatus3\n"
		"getstatus4                      -- show various parameters\n"
		"getpartypause                   -- get partymode / pausemode settings\n"
                "actschedule                     -- show active schedule\n"
                "chooseschedule [Familie|Morgen|\n"
                "                Frueh|Abend|\n"
                "                Vorm|Nachm|Mittag|\n"
                "                Single|Senioren|\n"
                "                Eigen1|Eigen2]  -- choose active schedule\n"
		"getschedule [1|2]               -- get custom schedule\n"
		"schedule [1|2] <index> unset    -- unset a switchpoint\n"
		"schedule [1|2] <index>\n"
		"         [MO|TU|WE|TH|FR|SA|SU]\n"
		"          HH:MM [ON|OFF]        -- set a switchpoint\n");
	return Ok;
    } else if (cmd == "mode") {
	uint8_t data;
	std::string mode;

	request >> mode;

	if (mode == "day")        data = 0x01;
	else if (mode == "night") data = 0x00;
	else if (mode == "auto")  data = 0x02;
	else return InvalidArgs;

	sendCommand(EmsMessage::addressRC, type, 7, &data, 1);
	return Ok;

    } else if (cmd == "redmode") {
	std::string ns;
	uint8_t data;

	request >> ns;
//	"redmode [Abschalt|Reduziert|Raumhalt|Aussenhalt]\n"

	data = 99;
        if (ns == "Abschalt") data = 0;
        if (ns == "Reduziert") data = 1;
        if (ns == "Raumhalt") data = 2;
        if (ns == "Aussenhalt") data = 3;

        if (data == 99) return InvalidArgs;

        
	sendCommand(EmsMessage::addressRC, type , 25, &data, 1);
        return Ok;

    } else if (cmd == "schedoptimizer") {
	std::string ns;
	uint8_t data;

	request >> ns;

	data = 99;
        if (ns == "on") data = 255;
        if (ns == "off") data = 0;

        if (data == 99) return InvalidArgs;

        
	sendCommand(EmsMessage::addressRC, type , 19, &data, 1);
        return Ok;
    } else if (cmd == "building") {
	std::string ns;
	uint8_t data;

	request >> ns;

	data = 99;
        if (ns == "leicht") data = 0;
        if (ns == "mittel") data = 1;
        if (ns == "schwer") data = 2;

        if (data == 99) return InvalidArgs;

        
	sendCommand(EmsMessage::addressRC, 0xa5 , 6, &data, 1);
        return Ok;
    } else if (cmd == "enabledamping") {
	std::string ns;
	uint8_t data;

	request >> ns;

	data = 99;
        if (ns == "on") data = 255;
        if (ns == "off") data = 0;

        if (data == 99) return InvalidArgs;

        
	sendCommand(EmsMessage::addressRC, 0xa5 , 21, &data, 1);
        return Ok;

    } else if (cmd == "refinput") {
	std::string ns;
	uint8_t data;

	request >> ns;

	data = 99;
        if (ns == "Aussen") data = 0;
        if (ns == "Raum") data = 1;

        if (data == 99) return InvalidArgs;

        
	sendCommand(EmsMessage::addressRC, type , 33, &data, 1);
        return Ok;

    } else if (cmd == "refinputvac") {
	std::string ns;
	uint8_t data;

	request >> ns;

	data = 99;
        if (ns == "Aussen") data = 3;
        if (ns == "Raum") data = 2;

        if (data == 99) return InvalidArgs;

        
	sendCommand(EmsMessage::addressRC, type , 41, &data, 1);
        return Ok;

    } else if (cmd == "frostmode") {
	std::string ns;
	uint8_t data;

	request >> ns;

	data = 99;
        if (ns == "off") data = 0;
        if (ns == "Aussen") data = 1;
        if (ns == "Raum") data = 2;

        if (data == 99) return InvalidArgs;

        
	sendCommand(EmsMessage::addressRC, type , 28, &data, 1);
        return Ok;


    } else if (cmd == "daytemperature") {
	return handleHkTemperatureCommand(request, type, 2);
    } else if (cmd == "nighttemperature") {
	return handleHkTemperatureCommand(request, type, 1);
    } else if (cmd == "holidaytemperature") {
	return handleHkTemperatureCommand(request, type, 3);
    } else if (cmd == "holidaymode") {
	return handleSetHolidayCommand(request, type + 2, 93);
    } else if (cmd == "vacationmode") {
	return handleSetHolidayCommand(request, type + 2, 87);
    } else if (cmd == "partymode") {
	unsigned int hours;
	uint8_t data;

	request >> hours;

	if (!request || hours > 99) {
	    return InvalidArgs;
	}
	data = hours;
	sendCommand(EmsMessage::addressRC, 0x3f , 86, &data, 1);
	sendCommand(EmsMessage::addressRC, 0x49 , 86, &data, 1);
	sendCommand(EmsMessage::addressRC, 0x53 , 86, &data, 1);
	sendCommand(EmsMessage::addressRC, 0x5d , 86, &data, 1);
	return Ok;
    } else if (cmd == "pausemode") {
	unsigned int hours;
	uint8_t data;

	request >> hours;

	if (!request || hours > 99) {
	    return InvalidArgs;
	}
	data = hours;
	sendCommand(EmsMessage::addressRC, 0x3f , 85, &data, 1);
	sendCommand(EmsMessage::addressRC, 0x49 , 85, &data, 1);
	sendCommand(EmsMessage::addressRC, 0x53 , 85, &data, 1);
	sendCommand(EmsMessage::addressRC, 0x5d , 85, &data, 1);
	return Ok;
       
       } else if (cmd == "designtemp") {
	unsigned int temp;
	uint8_t data;

	request >> temp;

	if (!request || temp > 80) {
	    return InvalidArgs;
	}
	data = temp ;
	sendCommand(EmsMessage::addressRC, type , 36, &data, 1);
        return Ok;
       } else if (cmd == "minouttemp") {
	unsigned int temp;
	uint8_t data;

	request >> temp;

	if (!request || temp < 70) {
	    return InvalidArgs;
	}
	data = temp ;
	sendCommand(EmsMessage::addressRC, 0xa5 , 5, &data, 1);
        return Ok;
       } else if (cmd == "frosttemp") {
	unsigned int temp;
	uint8_t data;

	request >> temp;

	if (!request ) {
	    return InvalidArgs;
	}
	data = temp ;
	sendCommand(EmsMessage::addressRC, type , 23, &data, 1);
	return Ok;
       } else if (cmd == "summertimetemp") {
	unsigned int temp;
	uint8_t data;

	request >> temp;

	if (!request ) {
	    return InvalidArgs;
	}
	data = temp ;
	sendCommand(EmsMessage::addressRC, type , 22, &data, 1);
        return Ok;
       } else if (cmd == "stopnighttemp") {
	unsigned int temp;
	uint8_t data;

	request >> temp;

	if (!request) {
	    return InvalidArgs;
	}
	data = temp ;
	sendCommand(EmsMessage::addressRC, type , 38, &data, 1);
        return Ok;

       } else if (cmd == "minheatflowtemp") {
	unsigned int temp;
	uint8_t data;

	request >> temp;

	if (!request) {
	    return InvalidArgs;
	}
	data = temp ;
	sendCommand(EmsMessage::addressRC, type , 16, &data, 1);
        return Ok;
       } else if (cmd == "maxheatflowtemp") {
	unsigned int temp;
	uint8_t data;

	request >> temp;

	if (!request) {
	    return InvalidArgs;
	}
	data = temp ;
	sendCommand(EmsMessage::addressRC, type , 35, &data, 1);
        return Ok;
       } else if (cmd == "maxroomeffect") {

	float value;
	uint8_t data;

	request >> value;

        try {
            data = boost::numeric_cast<uint8_t>(2 * value);
        } catch (boost::numeric::bad_numeric_cast& e) {
            return InvalidArgs;
        }

	sendCommand(EmsMessage::addressRC, type , 4, &data, 1);
        return Ok;

       } else if (cmd == "temptemp") {
        float value;
        uint8_t data;

        request >> value;

        try {
            data = boost::numeric_cast<uint8_t>(2 * value);
        } catch (boost::numeric::bad_numeric_cast& e) {
            return InvalidArgs;
        }

        sendCommand(EmsMessage::addressRC, 0x3d , 37, &data, 1);
        sendCommand(EmsMessage::addressRC, 0x47 , 37, &data, 1);
        sendCommand(EmsMessage::addressRC, 0x51 , 37, &data, 1);
        sendCommand(EmsMessage::addressRC, 0x5b , 37, &data, 1);
        return Ok;


       } else if (cmd == "nightdoredtemp") {
	unsigned int temp;
	uint8_t data;

	request >> temp;

	if (!request) {
	    return InvalidArgs;
	}
	data = temp ;
	sendCommand(EmsMessage::addressRC, type , 39, &data, 1);
        return Ok;
       } else if (cmd == "tempoffset") {
	float value;
	uint8_t data;

	request >> value;

        try {
            data = boost::numeric_cast<uint8_t>(2 * value);
        } catch (boost::numeric::bad_numeric_cast& e) {
            return InvalidArgs;
        }

	sendCommand(EmsMessage::addressRC, type , 6, &data, 1);
        return Ok;
        
        } else if (cmd == "schedule") {
        unsigned int prog;
	unsigned int index;
	EmsMessage::ScheduleEntry entry;

        request >> prog;

        if (!request) { return InvalidArgs; }
        
	request >> index;

	if (!request || index > 42 || !parseScheduleEntry(request, &entry)) {
	    return InvalidArgs;
	}

        if ((prog < 1) || (prog > 2)) return InvalidArgs;
        if (prog == 2) prog = 4;
        
	sendCommand(EmsMessage::addressRC, type + prog + 1,
		(index - 1) * sizeof(EmsMessage::ScheduleEntry),
		(uint8_t *) &entry, sizeof(entry));
	return Ok;
    } else if (cmd == "getschedule") {
        unsigned int prog;
        request >> prog;
        if (!request) { return InvalidArgs; }

        if ((prog < 1) || (prog > 2)) return InvalidArgs;
        if (prog == 2) prog = 4;

	startRequest(EmsMessage::addressRC, type + prog + 1, 0, 42 * sizeof(EmsMessage::ScheduleEntry));
	return Ok;
    } else if (cmd == "actschedule") {
	startRequest(EmsMessage::addressRC, type + 2, 84, 1);
	return Ok;
    } else if (cmd == "getpartypause") {
	startRequest(EmsMessage::addressRC, type + 2, 85, 2);
	return Ok;
    } else if (cmd == "chooseschedule") {
	std::string ns;
	uint8_t data;

	request >> ns;

        // chooseschedule [Familie|Morgen|Frueh|Abend|Vorm|Nachm|Mittag|Single|Senioren|Eigen1|Eigen2]
	
	data = 99;
        if (ns == "Eigen1") data = 0;
        if (ns == "Familie") data = 1;
        if (ns == "Morgen") data = 2;
        if (ns == "Frueh") data = 3;
        if (ns == "Abend") data = 4;
        if (ns == "Vorm") data = 5;
        if (ns == "Nachm") data = 6;
        if (ns == "Mittag") data = 7;
        if (ns == "Single") data = 8;
        if (ns == "Senioren") data = 9;
        if (ns == "Eigen2") data = 10;
        
        if (data == 99) return InvalidArgs;
        
	sendCommand(EmsMessage::addressRC, type+2 , 84, &data, 1);
        return Ok;

    } else if (cmd == "getvacation") {
	startRequest(EmsMessage::addressRC, type + 2, 87, 2 * sizeof(EmsMessage::HolidayEntry));
	return Ok;
    } else if (cmd == "getholiday") {
	startRequest(EmsMessage::addressRC, type + 2, 93, 2 * sizeof(EmsMessage::HolidayEntry));
	return Ok;
    } else if (cmd == "getstatus") {
        startRequest(EmsMessage::addressRC, type + 1 , 0, 20);	
	return Ok;
    } else if (cmd == "getstatus2") {
        startRequest(EmsMessage::addressRC, type , 0, 25);	
	return Ok;
    } else if (cmd == "getstatus3") {
        startRequest(EmsMessage::addressRC, type , 25, 25);	
	return Ok;
    } else if (cmd == "getstatus4") {
        startRequest(EmsMessage::addressRC, 0xa5 , 0, 25);	
	return Ok;
    }


    return InvalidCmd;
}

CommandConnection::CommandResult
CommandConnection::handleHkTemperatureCommand(std::istream& request, uint8_t type, uint8_t offset)
{
    float value;
    uint8_t valueByte;

    request >> value;
    if (!request) {
	return InvalidArgs;
    }

    try {
	valueByte = boost::numeric_cast<uint8_t>(2 * value);
	if (valueByte < 20 || valueByte > 60) {
	    return InvalidArgs;
	}
    } catch (boost::numeric::bad_numeric_cast& e) {
	return InvalidArgs;
    }

    sendCommand(EmsMessage::addressRC, type, offset, &valueByte, 1);
    return Ok;
}

CommandConnection::CommandResult
CommandConnection::handleSetHolidayCommand(std::istream& request, uint8_t type, uint8_t offset)
{
    std::string beginString, endString;
    EmsMessage::HolidayEntry entries[2];
    EmsMessage::HolidayEntry *begin = entries;
    EmsMessage::HolidayEntry *end = entries + 1;

    request >> beginString;
    request >> endString;

    if (!request) {
	return InvalidArgs;
    }

    if (!parseHolidayEntry(beginString, begin) || !parseHolidayEntry(endString, end)) {
	return InvalidArgs;
    }

    /* make sure begin is not later than end */
    if (begin->year > end->year) {
	return InvalidArgs;
    } else if (begin->year == end->year) {
	if (begin->month > end->month) {
	    return InvalidArgs;
	} else if (begin->month == end->month) {
	    if (begin->day > end->day) {
		return InvalidArgs;
	    }
	}
    }

    sendCommand(EmsMessage::addressRC, type, offset, (uint8_t *) entries, sizeof(entries));
    return Ok;
}

CommandConnection::CommandResult
CommandConnection::handleWwCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "help") {
	respond("\nAvailable warm water subsystem subcommands:\n\n"
	        "mode [on|off|auto]                -- operating mode\n"
		"temperature <temp>                -- desired warm water temp\n"
		"limittemp <temp>                  -- limit warm water temp to\n"
		"thermdesinfect mode [on|off]      -- do thermal desinfection\n"
		"thermdesinfect day [monday|\n"
		"                    ...|sunday]   -- day for thermal desinfection\n"
		"thermdesinfect temperature <temp> -- temp for thermal desinfection\n"
		"thermdesinfect hour <hour>        -- hour for thermal desinfection\n"
		"getschedule                       -- get warmwater schedule\n"
		"getstatus\n"
		"getstatus2\n"
		"getstatus3                        -- show different ww parameters\n"
		"chooseschedule [Eigen1|Heizkreis] -- choose active ww schedule\n"
		"schedule <index> unset            -- unset a switchpoint\n"
		"schedule <index>\n"
		"         [MO|TU|WE|TH|FR|SA|SU]\n"
		"          HH:MM [ON|OFF]          -- set a switchpoint\n"
		"zirkpump mode [on|off|auto]       -- zirkpump operation mode\n"
		"zirkpump count [1-6, 7=alwayson]  -- zirkpump operations per hour\n"
		"zirkpump chooseschedule \n"
		"              [Eigen1|Heizkreis]  -- choose active zirkpump schedule\n"
		"zirkpump schedule <index> unset   -- unset a switchpoint\n"
		"zirkpump schedule <index>\n"
		"         [MO|TU|WE|TH|FR|SA|SU]\n"
		"          HH:MM [ON|OFF]          -- set a switchpoint\n"
                "loadled [on|off]                  -- enable one-time-loading-LED\n"
		"loadonce                          -- heat up warmwater once\n"
		"canloadonce                       -- cancel one-time-ww-preparation\n");
		
	return Ok;
    } else if (cmd == "thermdesinfect") {
	return handleThermDesinfectCommand(request);
    } else if (cmd == "zirkpump") {
	return handleZirkPumpCommand(request);
    } else if (cmd == "mode") {

	uint8_t data;
	std::string mode;

	request >> mode;

	if (mode == "on")        data = 0x01;
	else if (mode == "off")  data = 0x00;
	else if (mode == "auto") data = 0x02;
	else return InvalidArgs;

	sendCommand(EmsMessage::addressRC, 0x37, 2, &data, 1);
	return Ok;
    } else if (cmd == "loadled") {

	uint8_t data;
	std::string mode;

	request >> mode;

	if (mode == "on")        data = 0xff;
	else if (mode == "off")  data = 0x00;
	else return InvalidArgs;

	sendCommand(EmsMessage::addressRC, 0x37, 9, &data, 1);
	return Ok;
    } else if (cmd == "temperature") {
	unsigned int temperature;
	uint8_t data;

	request >> temperature;

	if (!request || temperature < 30 || temperature > 80) {
	    return InvalidArgs;
	}
	data = temperature;

	sendCommand(EmsMessage::addressUBA, 0x33, 2, &data, 1);
	return Ok;
    } else if (cmd == "limittemp") {
	unsigned int temperature;
	uint8_t data;

	request >> temperature;

	if (!request || temperature < 30 || temperature > 80) {
	    return InvalidArgs;
	}
	data = temperature;

	sendCommand(EmsMessage::addressRC, 0x37, 8, &data, 1);
	return Ok;
    } else if (cmd == "getstatus3") {
	startRequest(EmsMessage::addressUBA, 0x33, 0, 10);
	return Ok;
    } else if (cmd == "loadonce") {
        uint8_t data;
        data = 35;
	sendCommand(EmsMessage::addressUBA, 0x35, 0, &data, 1);
	return Ok;
    } else if (cmd == "canloadonce") {
        uint8_t data;
        data = 3;
	sendCommand(EmsMessage::addressUBA, 0x35, 0, &data , 1);
	return Ok;
    } else if (cmd == "getschedule") {
	startRequest(EmsMessage::addressRC, 0x38 , 0, 42 * sizeof(EmsMessage::ScheduleEntry));
	return Ok;
    } else if (cmd == "schedule") {
	unsigned int index;
	EmsMessage::ScheduleEntry entry;

        if (!request) { return InvalidArgs; }
        
	request >> index;

	if (!request || index > 42 || !parseScheduleEntry(request, &entry)) {
	    return InvalidArgs;
	}

	sendCommand(EmsMessage::addressRC, 0x38,
		(index - 1) * sizeof(EmsMessage::ScheduleEntry),
		(uint8_t *) &entry, sizeof(entry));
	return Ok;
    } else if (cmd == "getstatus") {
	startRequest(EmsMessage::addressRC, 0x37, 0, 12);
	return Ok;
    } else if (cmd == "getstatus2") {
	startRequest(EmsMessage::addressUBA, 0x34, 0, 20);
	return Ok;
    } else if (cmd == "chooseschedule") {
    
	std::string ns;
	uint8_t data;

	request >> ns;

	data = 99;
        if (ns == "Eigen1") data = 0xff;
        if (ns == "Heizkreis") data = 0x00;
        
        if (data == 99) return InvalidArgs;
        
	sendCommand(EmsMessage::addressRC, 0x37 , 0,  &data, 1);
        return Ok;
    }

    return InvalidCmd;
}

CommandConnection::CommandResult
CommandConnection::handleThermDesinfectCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "mode") {
	uint8_t data;
	std::string mode;

	request >> mode;

	if (mode == "on")       data = 0xff;
	else if (mode == "off") data = 0x00;
	else return InvalidArgs;

	sendCommand(EmsMessage::addressRC, 0x37, 4, &data, 1);
	return Ok;
    } else if (cmd == "day") {
	uint8_t data;
	std::string day;

	request >> day;

	if (day == "monday")         data = 0x00;
	else if (day == "tuesday")   data = 0x01;
	else if (day == "wednesday") data = 0x02;
	else if (day == "thursday")  data = 0x03;
	else if (day == "friday")    data = 0x04;
	else if (day == "saturday")  data = 0x05;
	else if (day == "sunday")    data = 0x06;
	else if (day == "everyday")  data = 0x07;
	else if (day == "0")   data = 0x00;
	else if (day == "1")   data = 0x01;
	else if (day == "2")   data = 0x02;
	else if (day == "3")   data = 0x03;
	else if (day == "4")   data = 0x04;
	else if (day == "5")   data = 0x05;
	else if (day == "6")   data = 0x06;
	else if (day == "7")   data = 0x07;
	else return InvalidArgs;

	sendCommand(EmsMessage::addressRC, 0x37, 5, &data, 1);
	return Ok;
    } else if (cmd == "hour") {
	uint8_t data;
        unsigned int hour;
	request >> hour;

	if (!request || hour > 23 ) {
	    return InvalidArgs;
	}
	data = hour;

	sendCommand(EmsMessage::addressRC, 0x37, 6, &data, 1);
	return Ok;
    } else if (cmd == "temperature") {
	unsigned int temperature;
	uint8_t data;

	request >> temperature;

	if (!request || temperature < 60 || temperature > 80) {
	    return InvalidArgs;
	}
	data = temperature;

	sendCommand(EmsMessage::addressUBA, 0x33, 8, &data, 1);
	return Ok;
    }

    return InvalidCmd;
}

CommandConnection::CommandResult
CommandConnection::handleZirkPumpCommand(std::istream& request)
{
    std::string cmd;
    request >> cmd;

    if (cmd == "mode") {
	uint8_t data;
	std::string mode;

	request >> mode;

	if (mode == "on")        data = 0x01;
	else if (mode == "off")  data = 0x00;
	else if (mode == "auto") data = 0x02;
	else return InvalidArgs;

	sendCommand(EmsMessage::addressRC, 0x37, 3, &data, 1);
	return Ok;
    } else if (cmd == "count") {
	uint8_t count;
	std::string countString;

	request >> countString;

	if (countString == "alwayson") {
	    count = 0x07;
	} else {
	    try {
		count = boost::lexical_cast<unsigned int>(countString);
		if (count < 1 || count > 7) {
		    return InvalidArgs;
		}
	    } catch (boost::bad_lexical_cast& e) {
		return InvalidArgs;
	    }
	}
	sendCommand(EmsMessage::addressUBA, 0x33, 7, &count, 1);
	return Ok;
    } else if (cmd == "getschedule") {
	startRequest(EmsMessage::addressRC, 0x39, 0, 42 * sizeof(EmsMessage::ScheduleEntry));
	return Ok;
    } else if (cmd == "schedule") {
	unsigned int index;
	EmsMessage::ScheduleEntry entry;

        if (!request) { return InvalidArgs; }
        
	request >> index;

	if (!request || index > 42 || !parseScheduleEntry(request, &entry)) {
	    return InvalidArgs;
	}

	sendCommand(EmsMessage::addressRC, 0x39,
		(index - 1) * sizeof(EmsMessage::ScheduleEntry),
		(uint8_t *) &entry, sizeof(entry));
	return Ok;
    } else if (cmd == "chooseschedule") {
    
	std::string ns;
	uint8_t data;


	request >> ns;

	data = 99;
        if (ns == "Eigen1") data = 0xff;
        if (ns == "Heizkreis") data = 0x00;
        
        if (data == 99) return InvalidArgs;
        
	sendCommand(EmsMessage::addressRC, 0x37 , 1 ,  &data, 1);
        return Ok;
    }

    return InvalidCmd;
}

void
CommandConnection::handlePcMessage(const EmsMessage& message)
{
    DebugStream& debug = Options::dataDebug();
    std::stringstream tmp;

    if (!m_waitingForResponse) {
	return;
    }

    const std::vector<uint8_t>& data = message.getData();
    uint8_t source = message.getSource();
    uint8_t type = message.getType();

    if (type == 0xff) {
	m_waitingForResponse = false;
	respond(data[0] == 0x04 ? "FAIL" : "OK");
        prompt();
	return;
    }

/*    if (source != EmsMessage::addressRC) {
	return;
     }
*/
    m_responseTimeout.cancel();
    
    m_requestResponse.insert(m_requestResponse.end(), data.begin() + 1, data.end());

    bool done = false;
    switch (type) {
	case 0x02: /* version info */
	
          printNumber(2,1,1,"Version Major number","",data);	       
          printNumber(3,1,1,"Version Minor number","",data);	       
          done = true;
          break;

	case 0x14: /* total hours */
	
          printNumber(1,3,60,"Betriebszeit Gesamtanlage","h",data);	       
          done = true;
          break;

	case 0x15: /* maintenance */

          printAuswahl(1, "Wartungsmeldungen" , data, 
                        0 , "keine", 
                        1 , "nach Betriebsstunden",
                        2 , "nach Datum",
                        99 , "");
          printNumber(2,1,1,"Wartungsintervall Betriebsstunden","*100h",data);	       
          printNumber(3,1,1,"Wartungsintervall Tag","",data);	       
          printNumber(4,1,1,"Wartungsintervall Monat","",data);	       
          printNumber(5,1,1,"Wartungsintervall Jahr","",data);	       
          done = true;
          break;

	case 0x1c: /* maintenance */

          printAuswahl(6, "Wartung faellig" , data, 
                        0 , "nein", 
                        3 , "ja, wegen Betriebsstunden",
                        8 , "ja, wegen Datum",
                        99 , "");
          done = true;
          break;

	case 0x10: /* get errors */
	case 0x11: /* get errors */
	case 0x12: /* get errors */
	case 0x13: /* get errors 2 */ {
	    done = loopOverResponse<EmsMessage::ErrorRecord>();
	    if (!done) {
		done = !continueRequest();
		if (done && type == 0x12) {
		    startRequest(source, 0x13, 0, 4 * sizeof(EmsMessage::ErrorRecord), false);
		    done = false;
		}
	    }
	    break;
	}
	case 0x33: /* getzirk */

          tmp << "DATA: Anzahl Schaltpunkte Zirkulation = ";
          switch ((int)data[8]) {
              case 1: tmp << "1x 3min"; break;
              case 2: tmp << "2x 3min"; break;
              case 3: tmp << "3x 3min"; break;
              case 4: tmp << "4x 3min"; break;
              case 5: tmp << "5x 3min"; break;
              case 6: tmp << "6x 3min"; break;
              case 7: tmp << "staendig an"; break;
           }
          
           respond(tmp.str());
          
          if (debug) {
            tmp << std::endl;
            debug << tmp.str();
          }
          
          printNumber(3, 1, 1, "Warmwassertemperatur Tag", "째C", data);
          printNumber(9, 1, 1, "Solltemperatur Thermische Desinfektion", "째C", data);
          
	  done = true;
	  break; 
	case 0x3f: /* get schedule HK1 */
	case 0x49: /* get schedule HK2 */
	case 0x53: /* get schedule HK3 */
	case 0x5d: /* get schedule HK4 */
	case 0x42: /* get schedule HK1 */
	case 0x4c: /* get schedule HK2 */
	case 0x56: /* get schedule HK3 */
	case 0x60: /* get schedule HK4 */
	case 0x38: /* get schedule WW */
	case 0x39: /* get schedule ZIRK */
	


	    if (data[0] == 85) {
	       /* We want to know the party-pausemode state */
               printNumber(86,1,1,"Verbleibende Stunden Pausenmodus","",data);	       
               printNumber(87,1,1,"Verbleibende Stunden Partymodus","",data);	       
	       done=true;
	       
	    } else  if (data[0] == 84) {
	       /* We want to know what the active schedule is! */
	       switch (data[1]){
	         case 0:
	           respond ("Eigen1");
	           break;
	         case 1:
	           respond ("Familie");
	           break;
	         case 2:
	           respond ("Morgen");
	           break;
	         case 3:
	           respond ("Frueh");
	           break;
	         case 4:
	           respond ("Abend");
	           break;
	         case 5:
	           respond ("Vorm.");
	           break;
	         case 6:
	           respond ("Nachm.");
	           break;
	         case 7:
	           respond ("Mittag");
	           break;
	         case 8:
	           respond ("Single");
	           break;
	         case 9:
	           respond ("Senioren");
	           break;
	         case 10:
	           respond ("Eigen2");
	           break;
               }
               done = true;
	    
	    } else if (data[0] > 80) {
		/* it's at the end -> holiday schedule */
		const size_t msgSize = sizeof(EmsMessage::HolidayEntry);

		if (m_requestResponse.size() >= 2 * msgSize) {
		    EmsMessage::HolidayEntry *begin = (EmsMessage::HolidayEntry *) &m_requestResponse.at(0);
		    EmsMessage::HolidayEntry *end = (EmsMessage::HolidayEntry *) &m_requestResponse.at(msgSize);
		    respond(buildRecordResponse("BEGIN", begin));
		    respond(buildRecordResponse("END", end));
		    done = true;
		} else {
		    respond("FAIL");
		}
	    } else {
		/* it's at the beginning -> heating schedule */
		done = loopOverResponse<EmsMessage::ScheduleEntry>();
		if (!done) {
		    done = !continueRequest();
		}
	    }
	    break;
         
        case  0x37: /*WW-Betriebsart*/
        
          printAuswahl(1, "Programm Warmwasser" , data, 
                        0 , "nach Heizkreisen", 
                        255 , "Eigenes Programm",
                        99 , "",
                        99 , "");

          printAuswahl(2, "Programm Zirkulationspumpe" , data, 
                        0 , "nach Warmwasser", 
                        255 , "Eigenes Programm",
                        99 , "",
                        99 , "");

          printAuswahl(3, "Betriebsart Warmwasser" , data, 
                        0 , "staendig aus", 
                        1 , "staendig an",
                        2 , "Automatik",
                        99 , "");

          printAuswahl(4, "Betriebsart Zirkulationspumpe" , data, 
                        0 , "staendig aus", 
                        1 , "staendig an",
                        2 , "Automatik",
                        99 , "");
                               
          printAuswahl(5, "Thermische Desinfektion" , data, 
                        0 , "aus", 
                        255 , "ein",
                        99 , "",
                        99 , "");
                        
          printNumber(6,1,1,"Therm. Desinfektion Tag (7:alle)","",data);
          printNumber(7,1,1,"Therm. Desinfektion Stunde","",data);
          printNumber(9, 1, 1, "max. Warmwassertemperatur", "째C", data);
          printBool(10, 0, "Einmalladungstaste",data);
          
          
          done=true;
          break;

        case  0x16: /*UBA-Status*/
        
          printNumber(2, 1, 1, "Temperatureinstellung Kessel", "째C", data);
          printNumber(3, 1, 1, "max. Kesselleistung", "�%", data);
          printNumber(4, 1, 1, "min. Kesselleistung", "�%", data);
          printNumber(5, 1, 1, "Abschalthysterese", "째C", data);
          printNumber(6, 1, 1, "Einschalthysterese", "째C", data);
          printNumber(7, 1, 1, "Antipendelzeit", "헿in", data);
          printNumber(9, 1, 1, "Kesselpumpennachlauf", "헿in", data);
          printNumber(10, 1, 1, "max. Kesselpumpenleistung", "�%", data);
          printNumber(11, 1, 1, "min. Kesselpumpenleistung", "�%", data);
          
          done=true;
          break;

        case  0x34: /*WW-Status*/
        
          printNumber(1, 1, 1, "Warmwasser-Solltemperatur", "째C", data);
          printNumber(2, 2, 10, "Warmwassertemperatur", "째C", data);
          printBool(6, 0, "WW-Tagbetrieb",data);
          printBool(8, 0, "Zirkulation-Tagbetrieb",data);
          printBool(8, 1, "Zirkulation manuell gestartet",data);
          printBool(8, 2, "Zirkulation",data);
          
          done=true;
          break;

        case  0x3e: /*HK-Status*/
        
          printBool(1, 2, "Automatikbetrieb",data);
          printBool(1, 0, "Ausschaltoptimierung",data);
          printBool(1, 1, "Einschaltoptimierung",data);
          printBool(1, 3, "Warmwasservorrang",data);
          printBool(1, 4, "Estrichtrocknung",data);
          printBool(1, 5, "Ferienbetrieb",data);
          printBool(1, 6, "Frostschutz",data);
          printBool(1, 7, "Manueller Betrieb",data);
          printBool(2, 0, "Sommerbetrieb",data);
          printBool(2, 1, "HK-Tagbetrieb",data);
          printBool(2, 7, "Partybetrieb",data);
          printNumber(13, 1, 1, "angeforderte Heizleistung", "�%", data);
          printNumber(15, 1, 1, "berechnete Solltemperatur Vorlauf", "째C", data);
                               
          done=true;
          break;

        case  0x3d: /*HK-Betriebsart*/
        
          printNumber(2, 1, 2, "Raumtemperatur Nacht", "째C", data);
          printNumber(3, 1, 2, "Raumtemperatur Tag", "째C", data);
          printNumber(4, 1, 2, "Raumtemperatur Ferien", "째C", data);
          printNumber(5, 1, 2, "Max. Raumtemperatureinfluss", "째C", data);
          printNumber(7, 1, 2, "Raumtemperaturoffset", "째C", data);
          printNumber(17, 1, 1, "Minimale Vorlauftemperatur", "째C", data);
          printNumber(36, 1, 1, "Maximale Vorlauftemperatur", "째C", data);
          printBool(20, 1, "Optimierung Schaltzeiten",data);
          printNumber(23, 1, 1, "Sommerbetrieb ab", "째C", data);
          printNumber(24, 1, 1, "Frostschutztemperatur", "째C", data);
          printNumber(37, 1, 1, "Auslegungstemperatur", "째C", data);
          printNumber(39, 1, 1, "Absenkung abbrechen ab", "째C", data);
          printNumber(40, 1, 1, "Aussentemp. fuer Absenkbetrieb", "째C", data);
          printNumber(38, 1, 2, "Temporaere Raumtemperatur (0:inaktiv)", "째C", data);

          printAuswahl(26, "Betriebsart" , data, 
                        0 , "Abschaltbetrieb", 
                        1 , "Reduzierter Betrieb",
                        2 , "Raumhaltebetrieb",
                        3 , "Aussenhaltebetrieb");
          printAuswahl(29, "Frostschutzart" , data, 
                        0 , "kein",
                        1 , "per Aussentemperatur",
                        2 , "per Aussentemperatur",
                        99, "");

          printAuswahl(33, "Heizsystem" , data, 
                        1 , "Heizkoerper", 
                        2 , "Konvektor",
                        3 , "Fussboden",
                        99 , "");
                        
          printAuswahl(34, "Fuehrungsgroesse" , data, 
                        0 , "Aussentemperaturgefuehrt", 
                        1 , "Raumtemperaturgefuehrt",
                        99 , "",
                        99 , "");
                        

          printAuswahl(42, "Absenkung Urlaub" , data, 
                        3 , "Aussenhaltebetrieb", 
                        2 , "Raumhaltebetrieb",
                        99 , "",
                        99 , "");
                        
                        
          done=true;
          break;

        case 0xa4: /*Kontaktdaten*/
          respond("TEST");
          printASCII(data[0]+1,21,"Kontaktinfo",data);
          done=true;
          break;

        case 0xa5: /*Anlagenparameter*/
        
          printNumber(6, 1, 1, "Minimale Aussentemperatur", "째C", data);
          printAuswahl(7, "Gebaeudeart" , data, 
                        0 , "leicht", 
                        1 , "mittel",
                        2 , "schwer",
                        99 , "");
          printAuswahl(22, "Daempfung Aussentemperatur" , data, 
                        255 , "aktiviert", 
                        0 , "deaktiviert",
                        99 , "",
                        99 , "");
                        

          done=true;
          break;

	default:
          char tmp[100];
          sprintf(tmp,"Unknown type 0x%x (dec. %u)",type,type);
          respond(tmp); 
	  done=true;  
    }

        if(m_showrawdata){
          std::stringstream tmp;
         
          tmp << "source " << BYTEFORMAT_HEX source;
          tmp << ", type " << BYTEFORMAT_HEX  type;
          tmp << ", offset " << BYTEFORMAT_HEX data[0];
          tmp << ", data ";
          for (size_t i = 1; i < data.size(); i++) {
              tmp << " " << BYTEFORMAT_HEX data[i];
          }
          tmp << std::endl;

          respond(tmp.str());
          m_showrawdata = false;
        }

    if (done) {
	m_waitingForResponse = false;
	respond("OK");
	prompt();
    }
}

template<typename T> bool
CommandConnection::loopOverResponse()
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

	std::ostringstream os;
	os << std::setw(2) << std::setfill('0') << m_responseCounter << " " << response;
	respond(os.str());
    }

    return false;
}

void
CommandConnection::scheduleResponseTimeout()
{
    m_waitingForResponse = true;
    m_responseTimeout.expires_from_now(boost::posix_time::seconds(1));
    m_responseTimeout.async_wait(boost::bind(&CommandConnection::responseTimeout,
					     this, boost::asio::placeholders::error));
}

void
CommandConnection::responseTimeout(const boost::system::error_code& error)
{
    if (m_waitingForResponse && error != boost::asio::error::operation_aborted) {
	respond("ERRTIMEOUT");
	m_waitingForResponse = false;
	prompt();
    }
}

std::string
CommandConnection::buildRecordResponse(const EmsMessage::ErrorRecord *record)
{
    if (record->errorAscii[0] == 0) {
	/* no error at this position */
	return "";
    }

    std::ostringstream response;

    if (record->hasDate) {
	response << std::setw(2) << std::setfill('0') << (unsigned int) record->day << "-";
	response << std::setw(2) << std::setfill('0') << (unsigned int) record->month << "-";
	response << std::setw(4) << (unsigned int) (2000 + record->year) << " ";
	response << std::setw(2) << std::setfill('0') << (unsigned int) record->hour << ":";
	response << std::setw(2) << std::setfill('0') << (unsigned int) record->minute;
    } else {
	response  << "---";
    }

    response << " ";
    response << std::hex << (unsigned int) record->source << " ";

    response << std::dec << record->errorAscii[0] << record->errorAscii[1] << " ";
    response << __be16_to_cpu(record->code_be16) << " ";
    response << __be16_to_cpu(record->durationMinutes_be16);

    return response.str();
}

static const char * dayNames[] = {
    "MO", "TU", "WE", "TH", "FR", "SA", "SU"
};

std::string
CommandConnection::buildRecordResponse(const EmsMessage::ScheduleEntry *entry)
{
    if (entry->time >= 0x90) {
	/* unset */
	return "";
    }

    std::ostringstream response;
    unsigned int minutes = entry->time * 10;
    response << dayNames[entry->day / 2] << " ";
    response << std::setw(2) << std::setfill('0') << (minutes / 60) << ":";
    response << std::setw(2) << std::setfill('0') << (minutes % 60) << " ";
    response << (entry->on ? "ON" : "OFF");

    return response.str();
}

bool
CommandConnection::parseScheduleEntry(std::istream& request, EmsMessage::ScheduleEntry *entry)
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

    if (mode == "ON") {
	entry->on = 1;
    } else if (mode == "OFF") {
	entry->on = 0;
    } else {
	return false;
    }

    bool hasDay = false;
    for (size_t i = 0; i < sizeof(dayNames) / sizeof(dayNames[0]); i++) {
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
    unsigned int hours = boost::lexical_cast<unsigned int>(time.substr(0, pos));
    unsigned int minutes = boost::lexical_cast<unsigned int>(time.substr(pos + 1));
    if (hours > 23 || minutes >= 60 || (minutes % 10) != 0) {
	return false;
    }

    entry->time = (uint8_t) ((hours * 60 + minutes) / 10);

    return true;
}

std::string
CommandConnection::buildRecordResponse(const char *type, const EmsMessage::HolidayEntry *entry)
{
    std::ostringstream response;

    response << type << " ";
    response << std::setw(2) << std::setfill('0') << (unsigned int) entry->day << "-";
    response << std::setw(2) << std::setfill('0') << (unsigned int) entry->month << "-";
    response << std::setw(4) << (unsigned int) (2000 + entry->year);

    return response.str();
}

bool
CommandConnection::parseHolidayEntry(const std::string& string, EmsMessage::HolidayEntry *entry)
{
    size_t pos = string.find('-');
    if (pos == std::string::npos) {
	return false;
    }

    size_t pos2 = string.find('-', pos + 1);
    if (pos2 == std::string::npos) {
	return false;
    }

    unsigned int day = boost::lexical_cast<unsigned int>(string.substr(0, pos));
    unsigned int month = boost::lexical_cast<unsigned int>(string.substr(pos + 1, pos2 - pos - 1));
    unsigned int year = boost::lexical_cast<unsigned int>(string.substr(pos2 + 1));
    if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
	return false;
    }

    entry->year = (uint8_t) (year - 2000);
    entry->month = (uint8_t) month;
    entry->day = (uint8_t) day;

    return true;
}

void
CommandConnection::startRequest(uint8_t dest, uint8_t type, size_t offset,
			        size_t length, bool newRequest)
{
    m_requestOffset = offset;
    m_requestLength = length;
    m_requestDestination = dest;
    m_requestType = type;
    m_requestResponse.clear();
    m_requestResponse.reserve(length);
    m_parsePosition = 0;
    if (newRequest) {
	m_responseCounter = 0;
    }

    continueRequest();
}

bool
CommandConnection::continueRequest()
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
CommandConnection::sendCommand(uint8_t dest, uint8_t type, uint8_t offset,
			       const uint8_t *data, size_t count,
			       bool expectResponse)
{
    std::vector<uint8_t> sendData(data, data + count);
    sendData.insert(sendData.begin(), offset);

    scheduleResponseTimeout();

    EmsMessage msg(dest, type, sendData, expectResponse);
    m_handler.sendMessage(msg);
}

void
CommandConnection::printBool(int byte, int bit, const char *name, const std::vector<uint8_t>& data)
    {

    byte = byte - (int)data[0];
    if (byte >= (int)data.size()) return;
    if (byte <= 0) return;

    bool flagSet = data[byte] & (1 << bit);
    std::stringstream tmp;
    tmp << "DATA: " << name << " = "
                   << (flagSet ? "AN" : "AUS");
    respond(tmp.str());
    tmp << std::endl;

    if (Options::dataDebug()) {
        Options::dataDebug() << tmp.str();

    }
}

void
CommandConnection::printASCII(int byte, unsigned int len, const char *name, const std::vector<uint8_t>& data)
    {

    byte = byte - (int)data[0];
    if (byte >= (int)data.size()) return;
    if (byte <= 0) return;


    std::stringstream tmp;
    tmp << "DATA: " << name << " = ";

    std::string s(data.begin()+byte,data.begin()+byte+len); 
    tmp << s;

    respond(tmp.str());
    tmp << std::endl;

    if (Options::dataDebug()) {
        Options::dataDebug() << tmp.str();

    }
}

void
CommandConnection::printAuswahl(int byte, const char *name, const std::vector<uint8_t>& data, const int key1, const char *v1, const int key2, const char *v2, const int key3, const char *v3, const int key4, const char *v4 )
    {

    byte = byte - (int)data[0];
    if (byte >= (int)data.size()) return;
    if (byte <= 0) return;


    std::stringstream tmp;
    tmp << "DATA: " << name << " = ";

    if(data[byte] == key1) tmp << v1;
    if(data[byte] == key2) tmp << v2;
    if(data[byte] == key3) tmp << v3;
    if(data[byte] == key4) tmp << v4;

    respond(tmp.str());
    tmp << std::endl;

    if (Options::dataDebug()) {
        Options::dataDebug() << tmp.str();

    }
}

void
CommandConnection::printNumber(size_t offset, size_t size, int divider,
                                  const char *name, const char *unit,
                                  const std::vector<uint8_t>& data)

{
    int value = 0;
    float floatVal;

    offset = offset - (int)data[0];
    
    if (offset >= data.size()) return;
    
    if (offset <= 0) return;

    for (size_t i = offset; i < offset + size; i++) {
        value = (value << 8) | data[i];
    }

    /* treat values with highest bit set as negative
     * e.g. size = 2, value = 0xfffe -> real value -2
     */
    if (data[offset] & 0x80) {
        value = value - (1 << (size * 8));
    }

    floatVal = value;
    if (divider > 1) {
        floatVal /= divider;
    }

    std::stringstream tmp;
    tmp << "DATA: " << name << " = " << floatVal << " " << unit;

    respond(tmp.str());

    tmp << std::endl;

    if (Options::dataDebug()) {
        Options::dataDebug() << tmp.str();
    }
}
