/*
 * Copyright (c) 2017, Arm Limited and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "AT_CellularDevice.h"
#include "AT_CellularInformation.h"
#include "AT_CellularNetwork.h"
#include "AT_CellularPower.h"
#include "AT_CellularSMS.h"
#include "AT_CellularContext.h"
#include "AT_CellularStack.h"
#include "CellularLog.h"
#include "ATHandler.h"
#include "UARTSerial.h"
#include "FileHandle.h"

using namespace mbed_cellular_util;
using namespace events;
using namespace mbed;

#define DEFAULT_AT_TIMEOUT 1000 // at default timeout in milliseconds
const int MAX_SIM_RESPONSE_LENGTH = 16;

AT_CellularDevice::AT_CellularDevice(FileHandle *fh) : CellularDevice(fh), _atHandlers(0), _network(0), _sms(0),
    _power(0), _information(0), _context_list(0), _default_timeout(DEFAULT_AT_TIMEOUT),
    _modem_debug_on(false)
{
    MBED_ASSERT(fh);
    _at = get_at_handler(fh);
    MBED_ASSERT(_at);
}

AT_CellularDevice::~AT_CellularDevice()
{
    delete _state_machine;

    // make sure that all is deleted even if somewhere close was not called and reference counting is messed up.
    _network_ref_count = 1;
    _sms_ref_count = 1;
    _power_ref_count = 1;
    _info_ref_count = 1;

    close_network();
    close_sms();
    close_power();
    close_information();

    AT_CellularContext *curr = _context_list;
    AT_CellularContext *next;
    while (curr) {
        next = (AT_CellularContext *)curr->_next;
        delete curr;
        curr = next;
    }

    ATHandler *atHandler = _atHandlers;
    while (atHandler) {
        ATHandler *old = atHandler;
        atHandler = atHandler->_nextATHandler;
        delete old;
    }
}

// each parser is associated with one filehandle (that is UART)
ATHandler *AT_CellularDevice::get_at_handler(FileHandle *fileHandle)
{
    if (!fileHandle) {
        fileHandle = _fh;
    }
    ATHandler *atHandler = _atHandlers;
    while (atHandler) {
        if (atHandler->get_file_handle() == fileHandle) {
            atHandler->inc_ref_count();
            return atHandler;
        }
        atHandler = atHandler->_nextATHandler;
    }

    atHandler = new ATHandler(fileHandle, _queue, _default_timeout, "\r", get_send_delay());
    if (_modem_debug_on) {
        atHandler->set_debug(_modem_debug_on);
    }
    atHandler->_nextATHandler = _atHandlers;
    _atHandlers = atHandler;

    return atHandler;
}

void AT_CellularDevice::release_at_handler(ATHandler *at_handler)
{
    if (!at_handler) {
        return;
    }
    at_handler->dec_ref_count();
    if (at_handler->get_ref_count() == 0) {
        // we can delete this at_handler
        ATHandler *atHandler = _atHandlers;
        ATHandler *prev = NULL;
        while (atHandler) {
            if (atHandler == at_handler) {
                if (prev == NULL) {
                    _atHandlers = _atHandlers->_nextATHandler;
                } else {
                    prev->_nextATHandler = atHandler->_nextATHandler;
                }
                delete atHandler;
                break;
            } else {
                prev = atHandler;
                atHandler = atHandler->_nextATHandler;
            }
        }
    }
}

nsapi_error_t AT_CellularDevice::get_sim_state(SimState &state)
{
    char simstr[MAX_SIM_RESPONSE_LENGTH];
    _at->lock();
    _at->flush();
    _at->cmd_start("AT+CPIN?");
    _at->cmd_stop();
    _at->resp_start("+CPIN:");
    ssize_t len = _at->read_string(simstr, sizeof(simstr));
    if (len != -1) {
        if (len >= 5 && memcmp(simstr, "READY", 5) == 0) {
            state = SimStateReady;
        } else if (len >= 7 && memcmp(simstr, "SIM PIN", 7) == 0) {
            state = SimStatePinNeeded;
        } else if (len >= 7 && memcmp(simstr, "SIM PUK", 7) == 0) {
            state = SimStatePukNeeded;
        } else {
            simstr[len] = '\0';
            tr_error("Unknown SIM state %s", simstr);
            state = SimStateUnknown;
        }
    } else {
        tr_warn("SIM not readable.");
        state = SimStateUnknown; // SIM may not be ready yet or +CPIN may be unsupported command
    }
    _at->resp_stop();
    nsapi_error_t error = _at->get_last_error();
    _at->unlock();
#if MBED_CONF_MBED_TRACE_ENABLE
    switch (state) {
        case SimStatePinNeeded:
            tr_info("SIM PIN required");
            break;
        case SimStatePukNeeded:
            tr_error("SIM PUK required");
            break;
        case SimStateUnknown:
            tr_warn("SIM state unknown");
            break;
        default:
            tr_info("SIM is ready");
            break;
    }
#endif
    return error;
}

nsapi_error_t AT_CellularDevice::set_pin(const char *sim_pin)
{
    // if SIM is already in ready state then settings the PIN
    // will return error so let's check the state before settings the pin.
    SimState state;
    if (get_sim_state(state) == NSAPI_ERROR_OK && state == SimStateReady) {
        return NSAPI_ERROR_OK;
    }

    if (sim_pin == NULL) {
        return NSAPI_ERROR_PARAMETER;
    }

    _at->lock();
    _at->cmd_start("AT+CPIN=");
    _at->write_string(sim_pin);
    _at->cmd_stop_read_resp();
    return _at->unlock_return_error();
}

CellularContext *AT_CellularDevice::get_context_list() const
{
    return _context_list;
}

CellularContext *AT_CellularDevice::create_context(FileHandle *fh, const char *apn)
{
    ATHandler *atHandler = get_at_handler(fh);
    if (atHandler) {
        AT_CellularContext *ctx = create_context_impl(*atHandler, apn);
        AT_CellularContext *curr = _context_list;

        if (_context_list == NULL) {
            _context_list = ctx;
            return ctx;
        }

        AT_CellularContext *prev;
        while (curr) {
            prev = curr;
            curr = (AT_CellularContext *)curr->_next;
        }

        prev->_next = ctx;
        return ctx;
    }
    return NULL;
}

AT_CellularContext *AT_CellularDevice::create_context_impl(ATHandler &at, const char *apn)
{
    return new AT_CellularContext(at, this, apn);
}

void AT_CellularDevice::delete_context(CellularContext *context)
{
    AT_CellularContext *curr = _context_list;
    AT_CellularContext *prev = NULL;
    while (curr) {
        if (curr == context) {
            if (prev == NULL) {
                _context_list = (AT_CellularContext *)curr->_next;
            } else {
                prev->_next = curr->_next;
            }
        }
        prev = curr;
        curr = (AT_CellularContext *)curr->_next;
    }
    delete (AT_CellularContext *)context;
}

CellularNetwork *AT_CellularDevice::open_network(FileHandle *fh)
{
    if (!_network) {
        ATHandler *atHandler = get_at_handler(fh);
        if (atHandler) {
            _network = open_network_impl(*atHandler);
        }
    }
    if (_network) {
        _network_ref_count++;
    }
    return _network;
}

CellularSMS *AT_CellularDevice::open_sms(FileHandle *fh)
{
    if (!_sms) {
        ATHandler *atHandler = get_at_handler(fh);
        if (atHandler) {
            _sms = open_sms_impl(*atHandler);
        }
    }
    if (_sms) {
        _sms_ref_count++;
    }
    return _sms;
}

CellularPower *AT_CellularDevice::open_power(FileHandle *fh)
{
    if (!_power) {
        ATHandler *atHandler = get_at_handler(fh);
        if (atHandler) {
            _power = open_power_impl(*atHandler);
        }
    }
    if (_power) {
        _power_ref_count++;
    }
    return _power;
}

CellularInformation *AT_CellularDevice::open_information(FileHandle *fh)
{
    if (!_information) {
        ATHandler *atHandler = get_at_handler(fh);
        if (atHandler) {
            _information = open_information_impl(*atHandler);
        }
    }
    if (_information) {
        _info_ref_count++;
    }
    return _information;
}

AT_CellularNetwork *AT_CellularDevice::open_network_impl(ATHandler &at)
{
    return new AT_CellularNetwork(at);
}

AT_CellularSMS *AT_CellularDevice::open_sms_impl(ATHandler &at)
{
    return new AT_CellularSMS(at);
}

AT_CellularPower *AT_CellularDevice::open_power_impl(ATHandler &at)
{
    return new AT_CellularPower(at);
}

AT_CellularInformation *AT_CellularDevice::open_information_impl(ATHandler &at)
{
    return new AT_CellularInformation(at);
}

void AT_CellularDevice::close_network()
{
    if (_network) {
        _network_ref_count--;
        if (_network_ref_count == 0) {
            ATHandler *atHandler = &_network->get_at_handler();
            delete _network;
            _network = NULL;
            release_at_handler(atHandler);
        }
    }
}

void AT_CellularDevice::close_sms()
{
    if (_sms) {
        _sms_ref_count--;
        if (_sms_ref_count == 0) {
            ATHandler *atHandler = &_sms->get_at_handler();
            delete _sms;
            _sms = NULL;
            release_at_handler(atHandler);
        }
    }
}

void AT_CellularDevice::close_power()
{
    if (_power) {
        _power_ref_count--;
        if (_power_ref_count == 0) {
            ATHandler *atHandler = &_power->get_at_handler();
            delete _power;
            _power = NULL;
            release_at_handler(atHandler);
        }
    }
}

void AT_CellularDevice::close_information()
{
    if (_information) {
        _info_ref_count--;
        if (_info_ref_count == 0) {
            ATHandler *atHandler = &_information->get_at_handler();
            delete _information;
            _information = NULL;
            release_at_handler(atHandler);
        }
    }
}

void AT_CellularDevice::set_timeout(int timeout)
{
    _default_timeout = timeout;

    ATHandler *atHandler = _atHandlers;
    while (atHandler) {
        atHandler->set_at_timeout(_default_timeout, true); // set as default timeout
        atHandler = atHandler->_nextATHandler;
    }
}

uint16_t AT_CellularDevice::get_send_delay() const
{
    return 0;
}

void AT_CellularDevice::modem_debug_on(bool on)
{
    _modem_debug_on = on;

    ATHandler *atHandler = _atHandlers;
    while (atHandler) {
        atHandler->set_debug(_modem_debug_on);
        atHandler = atHandler->_nextATHandler;
    }
}

nsapi_error_t AT_CellularDevice::is_ready()
{
    _at->lock();
    _at->cmd_start("AT");
    _at->cmd_stop_read_resp();

    // we need to do this twice because for example after data mode the first 'AT' command will give modem a
    // stimulus that we are back to command mode.
    _at->clear_error();
    _at->cmd_start("AT");
    _at->cmd_stop_read_resp();

    return _at->unlock_return_error();
}

nsapi_error_t AT_CellularDevice::set_ready_cb(Callback<void()> callback)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

nsapi_error_t AT_CellularDevice::set_power_save_mode(int periodic_time, int active_time)
{
    _at->lock();

    if (periodic_time == 0 && active_time == 0) {
        // disable PSM
        _at->cmd_start("AT+CPSMS=");
        _at->write_int(0);
        _at->cmd_stop_read_resp();
    } else {
        const int PSMTimerBits = 5;

        /**
            Table 10.5.163a/3GPP TS 24.008: GPRS Timer 3 information element

            Bits 5 to 1 represent the binary coded timer value.

            Bits 6 to 8 defines the timer value unit for the GPRS timer as follows:
            8 7 6
            0 0 0 value is incremented in multiples of 10 minutes
            0 0 1 value is incremented in multiples of 1 hour
            0 1 0 value is incremented in multiples of 10 hours
            0 1 1 value is incremented in multiples of 2 seconds
            1 0 0 value is incremented in multiples of 30 seconds
            1 0 1 value is incremented in multiples of 1 minute
            1 1 0 value is incremented in multiples of 320 hours (NOTE 1)
            1 1 1 value indicates that the timer is deactivated (NOTE 2).
         */
        char pt[8 + 1]; // timer value encoded as 3GPP IE
        const int ie_value_max = 0x1f;
        uint32_t periodic_timer = 0;
        if (periodic_time <= 2 * ie_value_max) { // multiples of 2 seconds
            periodic_timer = periodic_time / 2;
            strcpy(pt, "01100000");
        } else {
            if (periodic_time <= 30 * ie_value_max) { // multiples of 30 seconds
                periodic_timer = periodic_time / 30;
                strcpy(pt, "10000000");
            } else {
                if (periodic_time <= 60 * ie_value_max) { // multiples of 1 minute
                    periodic_timer = periodic_time / 60;
                    strcpy(pt, "10100000");
                } else {
                    if (periodic_time <= 10 * 60 * ie_value_max) { // multiples of 10 minutes
                        periodic_timer = periodic_time / (10 * 60);
                        strcpy(pt, "00000000");
                    } else {
                        if (periodic_time <= 60 * 60 * ie_value_max) { // multiples of 1 hour
                            periodic_timer = periodic_time / (60 * 60);
                            strcpy(pt, "00100000");
                        } else {
                            if (periodic_time <= 10 * 60 * 60 * ie_value_max) { // multiples of 10 hours
                                periodic_timer = periodic_time / (10 * 60 * 60);
                                strcpy(pt, "01000000");
                            } else { // multiples of 320 hours
                                int t = periodic_time / (320 * 60 * 60);
                                if (t > ie_value_max) {
                                    t = ie_value_max;
                                }
                                periodic_timer = t;
                                strcpy(pt, "11000000");
                            }
                        }
                    }
                }
            }
        }

        uint_to_binary_str(periodic_timer, &pt[3], sizeof(pt) - 3, PSMTimerBits);
        pt[8] = '\0';

        /**
            Table 10.5.172/3GPP TS 24.008: GPRS Timer information element

            Bits 5 to 1 represent the binary coded timer value.

            Bits 6 to 8 defines the timer value unit for the GPRS timer as follows:

            8 7 6
            0 0 0  value is incremented in multiples of 2 seconds
            0 0 1  value is incremented in multiples of 1 minute
            0 1 0  value is incremented in multiples of decihours
            1 1 1  value indicates that the timer is deactivated.

            Other values shall be interpreted as multiples of 1 minute in this version of the protocol.
        */
        char at[8 + 1];
        uint32_t active_timer; // timer value encoded as 3GPP IE
        if (active_time <= 2 * ie_value_max) { // multiples of 2 seconds
            active_timer = active_time / 2;
            strcpy(at, "00000000");
        } else {
            if (active_time <= 60 * ie_value_max) { // multiples of 1 minute
                active_timer = (1 << 5) | (active_time / 60);
                strcpy(at, "00100000");
            } else { // multiples of decihours
                int t = active_time / (6 * 60);
                if (t > ie_value_max) {
                    t = ie_value_max;
                }
                active_timer = t;
                strcpy(at, "01000000");
            }
        }

        uint_to_binary_str(active_timer, &at[3], sizeof(at) - 3, PSMTimerBits);
        at[8] = '\0';

        // request for both GPRS and LTE
        _at->cmd_start("AT+CPSMS=");
        _at->write_int(1);
        _at->write_string(pt);
        _at->write_string(at);
        _at->write_string(pt);
        _at->write_string(at);
        _at->cmd_stop_read_resp();

        if (_at->get_last_error() != NSAPI_ERROR_OK) {
            tr_warn("Power save mode not enabled!");
        } else {
            // network may not agree with power save options but
            // that should be fine as timeout is not longer than requested
        }
    }

    return _at->unlock_return_error();
}

nsapi_error_t AT_CellularDevice::init_module()
{
#if MBED_CONF_MBED_TRACE_ENABLE
    CellularInformation *information = open_information();
    if (information) {
        char *pbuf = new char[100];
        nsapi_error_t ret = information->get_model(pbuf, sizeof(*pbuf));
        close_information();
        if (ret == NSAPI_ERROR_OK) {
            tr_info("Model %s", pbuf);
        }
        delete[] pbuf;
    }
#endif
    return NSAPI_ERROR_OK;
}
