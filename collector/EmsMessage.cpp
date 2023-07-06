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

#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <boost/format.hpp>
#include "EmsMessage.h"
#include "Options.h"

static const uint8_t INVALID_TEMP_VALUE_LOWER[] = { 0x7d, 0x00 };
static const uint8_t INVALID_TEMP_VALUE_UPPER[] = { 0x83, 0x00 };
const std::vector<const uint8_t *> EmsMessage::INVALID_TEMPERATURE_VALUES = {
    INVALID_TEMP_VALUE_LOWER, INVALID_TEMP_VALUE_UPPER
};

EmsValue::EmsValue(Type type, SubType subType, const uint8_t *data,
		   size_t len, int divider, bool isSigned,
		   const std::vector<const uint8_t *> *invalidValues) :
    m_type(type),
    m_subType(subType),
    m_readingType(Numeric),
    m_isValid(true)
{
    int value = 0;
    for (size_t i = 0; i < len; i++) {
	value = (value << 8) | data[i];
    }

    if (isSigned) {
	int highestbit = 1 << (8 * len - 1);
	if (value & highestbit) {
	    value &= ~highestbit;
	    if (value == 0) {
		// only highest bit set -> value is unavailable
		m_isValid = false;
	    }
	    // remainder -> value is negative
	    // e.g. value 0xffff -> actual value -1
	    value = value - highestbit;
	}
    } else {
	int maxValue = (1 << 8 * len) - 1;
	m_isValid = value != maxValue;
    }

    if (invalidValues) {
	for (auto& invalid : *invalidValues) {
	    if (memcmp(data, invalid, len) == 0) {
		m_isValid = false;
		break;
	    }
	}
    }

    if (divider == 0) {
	m_value = (unsigned int) value;
	m_readingType = Integer;
    } else {
	m_value = (float) value / (float) divider;
    }
}

EmsValue::EmsValue(Type type, SubType subType, uint8_t value, uint8_t bit) :
    m_type(type),
    m_subType(subType),
    m_readingType(Boolean),
    m_value((value & (1 << bit)) != 0),
    m_isValid(true)
{
}

EmsValue::EmsValue(Type type, SubType subType, uint8_t low, uint8_t medium, uint8_t high) :
    m_type(type),
    m_subType(subType),
    m_readingType(Kennlinie),
    m_value(std::vector<uint8_t>({ low, medium, high })),
    m_isValid(true)
{
}

EmsValue::EmsValue(Type type, SubType subType, uint8_t value) :
    m_type(type),
    m_subType(subType),
    m_readingType(Enumeration),
    m_value(value),
    m_isValid(true)
{
}

EmsValue::EmsValue(Type type, SubType subType, const ErrorEntry& error) :
    m_type(type),
    m_subType(subType),
    m_readingType(Error),
    m_value(error),
    m_isValid(true)
{
}

EmsValue::EmsValue(Type type, SubType subType, const EmsProto::DateRecord& record) :
    m_type(type),
    m_subType(subType),
    m_readingType(Date),
    m_value(record),
    m_isValid(true)
{
}

EmsValue::EmsValue(Type type, SubType subType, const EmsProto::SystemTimeRecord& record) :
    m_type(type),
    m_subType(subType),
    m_readingType(SystemTime),
    m_value(record),
    m_isValid(true)
{
}

EmsValue::EmsValue(Type type, SubType subType, const std::string& value) :
    m_type(type),
    m_subType(subType),
    m_readingType(Formatted),
    m_value(value),
    m_isValid(true)
{
}

EmsMessage::EmsMessage(ValueHandler& valueHandler, CacheAccessor cacheAccessor,
		       const std::vector<uint8_t>& data) :
    m_valueHandler(valueHandler),
    m_cacheAccessor(cacheAccessor),
    m_data(data),
    m_source(0),
    m_dest(0),
    m_type(0),
    m_extType(0),
    m_offset(0)
{
    if (m_data.size() < 4) {
	return;
    }

    bool isRead = ((m_data[1] & 0x80) == 0);
    bool isPlus = m_data[2] >= 0xf0 && m_data.size() >= (isRead ? 7 : 6);

    m_source = m_data[0];
    m_dest = m_data[1];
    m_type = m_data[2];
    m_offset = m_data[3];
    m_data.erase(m_data.begin(), m_data.begin() + 4);

    if (isPlus) {
	size_t start = isRead ? 0 : 1;
	m_extType = (m_data[start] << 8) | m_data[start + 1];
	m_data.erase(m_data.begin() + start, m_data.begin() + start + 2);


    }
}

EmsMessage::EmsMessage(uint8_t dest, uint16_t type, uint8_t offset,
		       const std::vector<uint8_t>& data,
		       bool expectResponse) :
    m_valueHandler(),
    m_data(data),
    m_source(EmsProto::addressPC),
    m_dest(expectResponse ? dest | 0x80 : dest & 0x7f),

    m_type(type < 0xf0 ? type : 0xff),
    m_extType(type >= 0xf0 ? type : 0),


    m_offset(offset)
{
}

std::vector<uint8_t>
EmsMessage::getSendData(bool omitSenderAddress) const
{
    std::vector<uint8_t> data;
    static constexpr uint8_t ourSenderAddress = EmsProto::addressPC;

    if (!omitSenderAddress) {
	data.push_back(ourSenderAddress);
    }
    
    data.push_back(m_dest);
    
    bool isRead = ((m_dest & 0x80) > 0);
    
    data.push_back(m_type);
    data.push_back(m_offset);
    

    if (m_type >= 0xf0) {  // EMS plus

        if (isRead) { // read command, m_data[0] is the length
        
           data.push_back(m_data[0]);
        }

	data.push_back(m_extType >> 8);
	data.push_back(m_extType & 0xff);
	
	if (!isRead){
	
           data.insert(data.end(), m_data.begin(), m_data.end());

	}
    } else { // EMS classic
    

      data.insert(data.end(), m_data.begin(), m_data.end());
    }



    DebugStream& debug = Options::messageDebug();


    debug << "EmsMessage DATA COMPOSED: ";
    for (size_t i = 0; i < data.size(); i++) {
          debug << " 0x" << std::hex << std::setw(2)
                  << std::setfill('0') << (unsigned int) data[i];
    }
    debug << std::endl;




    return data;
}

void
EmsMessage::handle()
{
    bool handled = false;
    DebugStream& debug = Options::messageDebug();

    if (debug) {
	time_t now = time(NULL);
	struct tm time;

	localtime_r(&now, &time);
	boost::format f("MESSAGE[%02d.%02d.%04d %02d:%02d:%02d]: "
			"source 0x%02x, dest 0x%02x, type 0x%04x, offset %d");
	f  % time.tm_mday % (time.tm_mon + 1) % (time.tm_year + 1900);
	f % time.tm_hour % time.tm_min % time.tm_sec;
	f % (unsigned int) m_source % (unsigned int) m_dest;
	f % getType() % (unsigned int) m_offset;

	debug << f << ", data:";
	for (size_t i = 0; i < m_data.size(); i++) {
	    debug << " 0x" << std::hex << std::setw(2)
		  << std::setfill('0') << (unsigned int) m_data[i];
	}
	debug << std::endl;
    }

    if (!m_valueHandler) {
	/* kind of pointless to parse in that case */
	return;
    }

    if (!m_source && !m_dest && !m_type) {
	/* invalid packet */
	return;
    }


//    if ((m_dest & 0x80)==0) {
//     /* if highest bit of dest is NOT set, it's a polling request -> ignore */
//      return;
//    }

    switch (m_source) {
	case EmsProto::addressUBA2:
	    /* BOSCH UBA message */
	    switch (m_type) {
		case 0xd1: parseUBA2OutdoorMessage(); handled = true; break;
                case 0xe4: parseUBA2MonitorMessage(); handled = true; break;
                case 0xe5: parseUBA2MonitorMessage2(); handled = true; break;
                case 0xe9: parseUBA2WWMonitorMessage(); handled = true; break;
                case 0x2d: parseUBA2WWMonitorMessage2(); handled = true; break;
		case 0xbf: parseUBA2ErrorMessage(); handled = true; break;

	    }
	    break;
	case EmsProto::addressUI800:
	    /* BOSCH UI controller message */
	    switch (m_type) {
		case 0x06: parseRCTimeMessage(); handled = true; break;
		case 0xbf: parseUI800ErrorMessage(); handled = true; break;

	    }
	    break;
    }    
	    
    if (!handled) {
	DebugStream& dataDebug = Options::dataDebug();
	if (dataDebug) {
	    dataDebug << "DATA: Unhandled message received";
	    dataDebug << boost::format("(source 0x%02x, type 0x%02x).")
		    % (unsigned int) m_source % (unsigned int) m_type;
	    dataDebug << std::endl;
	}
    }
}

void
EmsMessage::parseEnum(size_t offset, EmsValue::Type type, EmsValue::SubType subtype)
{
    if (canAccess(offset, 1)) {
	EmsValue value(type, subtype, m_data[offset - m_offset]);
	m_valueHandler(value);
    }
}

void
EmsMessage::parseNumeric(size_t offset, size_t size, int divider,
			 EmsValue::Type type, EmsValue::SubType subtype,
			 bool isSigned, const std::vector<const uint8_t *> *invalidValues)
{
    if (canAccess(offset, size)) {
	EmsValue value(type, subtype, &m_data.at(offset - m_offset),
		size, divider, isSigned, invalidValues);
	m_valueHandler(value);
    }
}

void
EmsMessage::parseBool(size_t offset, uint8_t bit,
		      EmsValue::Type type, EmsValue::SubType subtype)
{
    if (canAccess(offset, 1)) {
	EmsValue value(type, subtype, m_data.at(offset - m_offset), bit);
	m_valueHandler(value);
    }
}



void
EmsMessage::parseUBA2ErrorMessage()
{
  parseUI800ErrorMessage();

}


void
EmsMessage::parseUI800ErrorMessage()
{
    bool errorsfound = false;
    
    for (int i=0; i<3; i++){

        if (canAccess(5+i*7, 3)) {
            std::ostringstream ss;
            ss <<  m_data[5+i*7] <<  m_data[6+i*7] << m_data[7+i*7];
            if ( (m_data[5+i*7] |  m_data[6+i*7] | m_data[7+i*7]) > 0) {
                m_valueHandler(EmsValue(EmsValue::StoerungsCode, EmsValue::None, ss.str()));
                errorsfound = true;
            }
        }

        if (canAccess(8+i*7, 2)) {
            std::ostringstream ss;
            ss << std::dec << (m_data[8+i*7] << 8 | m_data[9+i*7]);
            if ( (m_data[8+i*7] |  m_data[9+i*7] ) > 0) {
                m_valueHandler(EmsValue(EmsValue::StoerungsNummer, EmsValue::None, ss.str()));
                errorsfound = true;
            }
        }
    }
    
    if (!errorsfound) {
                m_valueHandler(EmsValue(EmsValue::StoerungsCode, EmsValue::None, "OK" ));
                m_valueHandler(EmsValue(EmsValue::StoerungsNummer, EmsValue::None, "0" ));

    }
    

}



void
EmsMessage::parseUBA2MonitorMessage()
{
    parseNumeric(6, 1, 1, EmsValue::SollTemp, EmsValue::Kessel);
    parseTemperature(7, EmsValue::IstTemp, EmsValue::Kessel);
    parseTemperature(13, EmsValue::IstTemp, EmsValue::Waermetauscher);
    parseTemperature(17, EmsValue::IstTemp, EmsValue::Ruecklauf);
    parseNumeric(19, 2, 10, EmsValue::Flammenstrom, EmsValue::None);
    parseNumeric(21, 1, 10, EmsValue::Systemdruck, EmsValue::None, false);
    parseInteger(40, 1, EmsValue::IstModulation, EmsValue::Brenner);
    parseInteger(41, 1, EmsValue::SollModulation, EmsValue::Brenner);




    if (canAccess(4, 2)) {
	std::ostringstream ss;
	ss << std::dec << (m_data[4] << 8 | m_data[5]);
	m_valueHandler(EmsValue(EmsValue::FehlerCode, EmsValue::None, ss.str()));
        m_valueHandler(EmsValue(EmsValue::ServiceCode, EmsValue::None, "--"));
	
    }

    if (canAccess(19, 2)) {
	int bakt = ( (m_data[19] << 8 | m_data[20]) > 0 );
//
        m_valueHandler(EmsValue(EmsValue::FlammeAktiv, EmsValue::None, bakt, 0 ));
	
    }
}

void
EmsMessage::parseUBA2MonitorMessage2()
{
    parseInteger(25, 1, EmsValue::IstModulation, EmsValue::KesselPumpe);

    parseBool(26, 5, EmsValue::DreiWegeVentilAufWW, EmsValue::None); // 100=WW, 50=Mix, Bit5 = >0
    parseBool(2, 7, EmsValue::ZirkulationAktiv, EmsValue::None);

}

void 
EmsMessage::parseUBA2OutdoorMessage()
{        
    parseTemperature(0, EmsValue::IstTemp, EmsValue::Aussen);
}
        

void
EmsMessage::parseUBA2WWMonitorMessage()
{
    parseNumeric(0, 1, 1, EmsValue::SollTemp, EmsValue::WW);
    parseTemperature(1, EmsValue::IstTemp, EmsValue::WW);

}

void
EmsMessage::parseUBA2WWMonitorMessage2()
{
}



void
EmsMessage::parseRCTimeMessage()
{
    if (canAccess(0, sizeof(EmsProto::SystemTimeRecord))) {
	EmsProto::SystemTimeRecord *record = (EmsProto::SystemTimeRecord *) &m_data.at(0);
	EmsValue value(EmsValue::SystemZeit, EmsValue::None, *record);
	m_valueHandler(value);
    }
}
