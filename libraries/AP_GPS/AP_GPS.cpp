// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <AP_Common.h>
#include <AP_Math.h>
#include <AP_HAL.h>
#include <AP_Notify.h>
#include <AP_GPS.h>

extern const AP_HAL::HAL& hal;

// table of user settable parameters
const AP_Param::GroupInfo AP_GPS::var_info[] PROGMEM = {
    // @Param: TYPE
    // @DisplayName: GPS type
    // @Description: GPS type
    // @Values: 0:None,1:AUTO,2:uBlox,3:MTK,4:MTK19,5:NMEA,6:SiRF
    AP_GROUPINFO("TYPE",    0, AP_GPS, _type[0], 1),

    // @Param: TYPE2
    // @DisplayName: 2nd GPS type
    // @Description: GPS type of 2nd GPS
    // @Values: 0:None,1:AUTO,2:uBlox,3:MTK,4:MTK19,5:NMEA,6:SiRF
    AP_GROUPINFO("TYPE2",   1, AP_GPS, _type[1], 0),

    // @Param: NAVFILTER
    // @DisplayName: Navigation filter setting
    // @Description: Navigation filter engine setting
    // @Values: 0:Portable,1:Stationary,2:Pedestrian,3:Automotive,4:Sea,5:Airborne1G,6:Airborne2G,8:Airborne4G
    AP_GROUPINFO("NAVFILTER", 2, AP_GPS, _navfilter, GPS_ENGINE_AIRBORNE_4G),

    AP_GROUPEND
};

/// Startup initialisation.
void AP_GPS::init(DataFlash_Class *dataflash)
{
    _DataFlash = dataflash;
    hal.uartB->begin(38400UL, 256, 16);
    if (hal.uartE != NULL) {
        hal.uartE->begin(38400UL, 256, 16);        
    }
}

// baudrates to try to detect GPSes with
const uint16_t AP_GPS::_baudrates[] PROGMEM = {4800U, 38400U, 57600U, 9600U};

// initialisation blobs to send to the GPS to try to get it into the
// right mode
const prog_char AP_GPS::_initialisation_blob[] PROGMEM = UBLOX_SET_BINARY MTK_SET_BINARY SIRF_SET_BINARY;

/*
  send some more initialisation string bytes if there is room in the
  UART transmit buffer
 */
void AP_GPS::send_blob_start(uint8_t instance, const prog_char *_blob, uint16_t size)
{
    initblob_state[instance].blob = _blob;
    initblob_state[instance].remaining = size;
}

/*
  send some more initialisation string bytes if there is room in the
  UART transmit buffer
 */
void AP_GPS::send_blob_update(uint8_t instance)
{
    // see if we can write some more of the initialisation blob
    if (initblob_state[instance].remaining > 0) {
        AP_HAL::UARTDriver *port = instance==0?hal.uartB:hal.uartE;
        int16_t space = port->txspace();
        if (space > (int16_t)initblob_state[instance].remaining) {
            space = initblob_state[instance].remaining;
        }
        while (space > 0) {
            port->write(pgm_read_byte(initblob_state[instance].blob));
            initblob_state[instance].blob++;
            space--;
            initblob_state[instance].remaining--;
        }
    }
}

/*
  run detection step for one GPS instance. If this finds a GPS then it
  will fill in drivers[instance] and change state[instance].status
  from NO_GPS to NO_FIX.
 */
void
AP_GPS::detect_instance(uint8_t instance)
{
    AP_GPS_Backend *new_gps = NULL;
    AP_HAL::UARTDriver *port = instance==0?hal.uartB:hal.uartE;
    struct detect_state *dstate = &detect_state[instance];

    if (port == NULL) {
        // UART not available
        return;
    }

    uint32_t now = hal.scheduler->millis();

    state[instance].instance = instance;
    state[instance].status = NO_GPS;

    // record the time when we started detection. This is used to try
    // to avoid initialising a uBlox as a NMEA GPS
    if (dstate->detect_started_ms == 0) {
        dstate->detect_started_ms = now;
    }

    if (now - dstate->last_baud_change_ms > 1200) {
        // try the next baud rate
		dstate->last_baud++;
		if (dstate->last_baud == sizeof(_baudrates) / sizeof(_baudrates[0])) {
			dstate->last_baud = 0;
		}
		uint16_t baudrate = pgm_read_word(&_baudrates[dstate->last_baud]);
		port->begin(baudrate, 256, 16);		
		dstate->last_baud_change_ms = now;
        send_blob_start(instance, _initialisation_blob, sizeof(_initialisation_blob));
    }

    send_blob_update(instance);

    while (port->available() > 0 && new_gps == NULL) {
        uint8_t data = port->read();
        /*
          running a uBlox at less than 38400 will lead to packet
          corruption, as we can't receive the packets in the 200ms
          window for 5Hz fixes. The NMEA startup message should force
          the uBlox into 38400 no matter what rate it is configured
          for.
        */
        if ((_type[instance] == GPS_TYPE_AUTO || _type[instance] == GPS_TYPE_UBLOX) &&
            pgm_read_word(&_baudrates[dstate->last_baud]) >= 38400 && 
            AP_GPS_UBLOX::_detect(dstate->ublox_detect_state, data)) {
            hal.console->print_P(PSTR(" ublox "));
            new_gps = new AP_GPS_UBLOX(*this, state[instance], port);
        } 
		else if ((_type[instance] == GPS_TYPE_AUTO || _type[instance] == GPS_TYPE_MTK19) &&
                 AP_GPS_MTK19::_detect(dstate->mtk19_detect_state, data)) {
			hal.console->print_P(PSTR(" MTK19 "));
			new_gps = new AP_GPS_MTK19(*this, state[instance], port);
		} 
		else if ((_type[instance] == GPS_TYPE_AUTO || _type[instance] == GPS_TYPE_MTK) &&
                 AP_GPS_MTK::_detect(dstate->mtk_detect_state, data)) {
			hal.console->print_P(PSTR(" MTK "));
			new_gps = new AP_GPS_MTK(*this, state[instance], port);
		}
#if !defined( __AVR_ATmega1280__ )
		// save a bit of code space on a 1280
		else if ((_type[instance] == GPS_TYPE_AUTO || _type[instance] == GPS_TYPE_SIRF) &&
                 AP_GPS_SIRF::_detect(dstate->sirf_detect_state, data)) {
			hal.console->print_P(PSTR(" SIRF "));
			new_gps = new AP_GPS_SIRF(*this, state[instance], port);
		}
		else if (now - dstate->detect_started_ms > 5000) {
			// prevent false detection of NMEA mode in
			// a MTK or UBLOX which has booted in NMEA mode
			if ((_type[instance] == GPS_TYPE_AUTO || _type[instance] == GPS_TYPE_NMEA) &&
                AP_GPS_NMEA::_detect(dstate->nmea_detect_state, data)) {
				hal.console->print_P(PSTR(" NMEA "));
				new_gps = new AP_GPS_NMEA(*this, state[instance], port);
			}
		}
#endif
	}

	if (new_gps != NULL) {
        state[instance].status = NO_FIX;
        drivers[instance] = new_gps;
        timing[instance].last_message_time_ms = now;
	}
}


/*
  update one GPS instance. This should be called at 10Hz or greater
 */
void
AP_GPS::update_instance(uint8_t instance)
{
    if (_type[instance] == GPS_TYPE_NONE) {
        // not enabled
        state[instance].status = NO_GPS;
        return;
    }
    if (drivers[instance] == NULL || state[instance].status == NO_GPS) {
        // we don't yet know the GPS type of this one, or it has timed
        // out and needs to be re-initialised
        detect_instance(instance);
        return;
    }

    send_blob_update(instance);

    // we have an active driver for this instance
    bool result = drivers[instance]->read();
    uint32_t tnow = hal.scheduler->millis();

    // if we did not get a message, and the idle timer of 1.2 seconds
    // has expired, re-initialise the GPS. This will cause GPS
    // detection to run again
    if (!result) {
        if (tnow - timing[instance].last_message_time_ms > 1200) {
            state[instance].status = NO_GPS;
            timing[instance].last_message_time_ms = tnow;
            // free the driver before we run the next detection, so we
            // don't end up with two allocated at any time
            delete drivers[instance];
            drivers[instance] = NULL;
        }
    } else {
        timing[instance].last_message_time_ms = tnow;
        if (state[instance].status >= GPS_OK_FIX_2D) {
            timing[instance].last_fix_time_ms = tnow;
        }
    }
}

/*
  update all GPS instances
 */
void
AP_GPS::update(void)
{
    for (uint8_t i=0; i<GPS_MAX_INSTANCES; i++) {
        update_instance(i);
    }

    // update notify with gps status
    AP_Notify::flags.gps_status = state[0].status;
}

/*
  set HIL (hardware in the loop) status for a GPS instance
 */
void 
AP_GPS::setHIL(GPS_Status _status, uint64_t time_epoch_ms, 
               Location &_location, Vector3f &_velocity, uint8_t _num_sats)
{
    uint32_t tnow = hal.scheduler->millis();
    GPS_State &istate = state[0];
    istate.status = _status;
    istate.location = _location;
    istate.location.options = 0;
    istate.velocity = _velocity;
    istate.ground_speed = pythagorous2(istate.velocity.x, istate.velocity.y);
    istate.ground_course_cd = degrees(atan2f(istate.velocity.y, istate.velocity.x)) * 100UL;
    istate.hdop = 0;
    istate.num_sats = _num_sats;
    istate.have_vertical_velocity = false;
    istate.last_gps_time_ms = tnow;
    istate.time_week     = time_epoch_ms / (86400*7*(uint64_t)1000);
    istate.time_week_ms  = time_epoch_ms - istate.time_week*(86400*7*(uint64_t)1000);
    timing[0].last_message_time_ms = tnow;
    timing[0].last_fix_time_ms = tnow;
    _type[0].set(GPS_TYPE_NONE);
}
