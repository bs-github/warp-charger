/* warp-charger
 * Copyright (C) 2020-2021 Erik Fleckstein <erik@tinkerforge.com>
 * Copyright (C)      2021 Birger Schmidt <bs-warp@netgaroo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "sonoff.h"

#include "bindings/errors.h"

#include "api.h"
#include "event_log.h"
#include "task_scheduler.h"
#include "tools.h"
#include "web_server.h"
#include "modules.h"

#include <ArduinoJson.h>

extern EventLog logger;

extern TaskScheduler task_scheduler;
/* extern TF_HalContext hal; */
extern WebServer server;

extern API api;
extern bool firmware_update_allowed;

#define RELAY1 27
#define RELAY2 14
#define SWITCH1 32
#define SWITCH2 33

sonoff::sonoff()
{
    evse_config = Config::Object({
        {"auto_start_charging", Config::Bool(true)},
        {"managed", Config::Bool(true)},
        {"max_current_configured", Config::Uint16(0)}
    });

    evse_state = Config::Object({
        {"iec61851_state", Config::Uint8(0)},
        {"vehicle_state", Config::Uint8(0)},
        {"contactor_state", Config::Uint8(0)},
        {"contactor_error", Config::Uint8(0)},
        {"charge_release", Config::Uint8(0)},
        {"allowed_charging_current", Config::Uint16(0)},
        {"error_state", Config::Uint8(0)},
        {"lock_state", Config::Uint8(0)},
        {"time_since_state_change", Config::Uint32(0)},
        {"uptime", Config::Uint32(0)}
    });

    evse_hardware_configuration = Config::Object({
        {"jumper_configuration", Config::Uint8(0)},
        {"has_lock_switch", Config::Bool(false)}
    });

    evse_low_level_state = Config::Object ({
        {"low_level_mode_enabled", Config::Bool(false)},
        {"led_state", Config::Uint8(0)},
        {"cp_pwm_duty_cycle", Config::Uint16(0)},
        {"adc_values", Config::Array({
                Config::Uint16(0),
                Config::Uint16(0),
            }, Config::Uint16(0), 2, 2, Config::type_id<Config::ConfUint>())
        },
        {"voltages", Config::Array({
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
            }, Config::Int16(0), 3, 3, Config::type_id<Config::ConfInt>())
        },
        {"resistances", Config::Array({
                Config::Uint32(0),
                Config::Uint32(0),
            }, Config::Uint32(0), 2, 2, Config::type_id<Config::ConfUint>())
        },
        {"gpio", Config::Array({Config::Bool(false),Config::Bool(false),Config::Bool(false),Config::Bool(false), Config::Bool(false)}, Config::Bool(false), 5, 5, Config::type_id<Config::ConfBool>())}
    });

    evse_max_charging_current = Config::Object ({
        {"max_current_configured", Config::Uint16(0)},
        {"max_current_incoming_cable", Config::Uint16(std::numeric_limits<std::uint16_t>::max())},
        {"max_current_outgoing_cable", Config::Uint16(std::numeric_limits<std::uint16_t>::max())},
        {"max_current_managed", Config::Uint16(0)},
    });

    evse_auto_start_charging = Config::Object({
        {"auto_start_charging", Config::Bool(true)}
    });

    evse_auto_start_charging_update = Config::Object({
        {"auto_start_charging", Config::Bool(true)}
    });
    evse_current_limit = Config::Object({
        {"current", Config::Uint(32000, 6000, 32000)}
    });

    evse_stop_charging = Config::Null();
    evse_start_charging = Config::Null();

    evse_managed_current = Config::Object ({
        {"current", Config::Uint16(0)}
    });

    evse_managed = Config::Object({
        {"managed", Config::Bool(false)}
    });

    evse_managed_update = Config::Object({
        {"managed", Config::Bool(false)},
        {"password", Config::Uint32(0)}
    });

    evse_user_calibration = Config::Object({
        {"user_calibration_active", Config::Bool(false)},
        {"voltage_diff", Config::Int16(0)},
        {"voltage_mul", Config::Int16(0)},
        {"voltage_div", Config::Int16(0)},
        {"resistance_2700", Config::Int16(0)},
        {"resistance_880", Config::Array({
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
                Config::Int16(0),
            }, Config::Int16(0), 14, 14, Config::type_id<Config::ConfInt>())},
    });
}

int sonoff::bs_evse_start_charging(TF_EVSE *evse) {
    charging = true;
    logger.printfln("EVSE start charging");
    digitalWrite(RELAY2, HIGH);
    return 0;
}

int sonoff::bs_evse_stop_charging(TF_EVSE *evse) {
    charging = false;
    logger.printfln("EVSE stop charging");
    digitalWrite(RELAY2, LOW);
    return 0;
}

int sonoff::bs_evse_persist_config() {
    String error = api.callCommand("evse/config_update", Config::ConfUpdateObject{{
        {"auto_start_charging", evse_auto_start_charging.get("auto_start_charging")->asBool()},
        {"max_current_configured", evse_max_charging_current.get("max_current_configured")->asUint()},
        {"managed", evse_managed.get("managed")->asBool()}
    }});
    if (error != "") {
        logger.printfln("Failed to save config: %s", error.c_str());
        return 500;
    } else {
	logger.printfln("saved config - auto_start_charging: %s, managed: %s, max_current_configured: %d",
            evse_config.get("auto_start_charging")->asBool() ?"true":"false",
            evse_config.get("managed")->asBool() ?"true":"false",
            evse_config.get("max_current_configured")->asUint());
        return 0;
    }
}

int sonoff::bs_evse_set_charging_autostart(TF_EVSE *evse, bool autostart) {
    logger.printfln("EVSE set auto start charging to %s", autostart ? "true" :"false");
    evse_auto_start_charging.get("auto_start_charging")->updateBool(autostart);
    bs_evse_persist_config();
    return 0;
}

int sonoff::bs_evse_set_max_charging_current(TF_EVSE *evse, uint16_t max_current) {
    evse_max_charging_current.get("max_current_configured")->updateUint(max_current);
    bs_evse_persist_config();
    update_evse_state();
    uint8_t allowed_charging_current = evse_state.get("allowed_charging_current")->asUint()/1000;
    logger.printfln("EVSE set configured charging limit to %d Ampere", uint8_t(max_current/1000));
    logger.printfln("EVSE calculated allowed charging limit is %d Ampere", allowed_charging_current);

    return 0;
}

int sonoff::bs_evse_get_state(TF_EVSE *evse, uint8_t *ret_iec61851_state, uint8_t *ret_vehicle_state, uint8_t *ret_contactor_state, uint8_t *ret_contactor_error, uint8_t *ret_charge_release, uint16_t *ret_allowed_charging_current, uint8_t *ret_error_state, uint8_t *ret_lock_state, uint32_t *ret_time_since_state_change, uint32_t *ret_uptime) {
//    bool response_expected = true;
//    tf_tfp_prepare_send(evse->tfp, TF_EVSE_FUNCTION_GET_STATE, 0, 17, response_expected);
    uint32_t allowed_charging_current;

    *ret_iec61851_state = evse_state.get("iec61851_state")->asUint();
    *ret_vehicle_state = evse_state.get("iec61851_state")->asUint(); // == 1 ? // charging ? 2 : 1; // 1 verbunden 2 leadt
    *ret_contactor_state = 2;
    *ret_contactor_error = 0;
    *ret_charge_release = 1; // manuell 0 automatisch
    // find the charging current maximum
    allowed_charging_current = min(
        evse_max_charging_current.get("max_current_incoming_cable")->asUint(),
        evse_max_charging_current.get("max_current_outgoing_cable")->asUint());
    if(evse_managed.get("managed")->asBool()) {
        allowed_charging_current = min(
            allowed_charging_current,
            evse_max_charging_current.get("max_current_managed")->asUint());
    }
    *ret_allowed_charging_current = min(
        allowed_charging_current,
        evse_max_charging_current.get("max_current_configured")->asUint());
    *ret_error_state = 0;
    *ret_lock_state = 0;
    *ret_time_since_state_change = evse_state.get("time_since_state_change")->asUint();
    *ret_uptime = millis();

    return TF_E_OK;
}

void sonoff::setup()
{
    setup_evse();
    if(!api.restorePersistentConfig("evse/config", &evse_config)) {
        logger.printfln("EVSE error, could not restore persistent storage config");
    } else {
        evse_auto_start_charging.get("auto_start_charging")     -> updateBool(evse_config.get("auto_start_charging")->asBool());
        evse_max_charging_current.get("max_current_configured") -> updateUint(evse_config.get("max_current_configured")->asUint());
        evse_managed.get("managed")                             -> updateBool(evse_config.get("managed")->asBool());
	logger.printfln("restored config - auto_start_charging: %s, managed: %s, max_current_configured: %d",
            evse_config.get("auto_start_charging")->asBool() ?"true":"false",
            evse_config.get("managed")->asBool() ?"true":"false",
            evse_config.get("max_current_configured")->asUint());
    }

    // switch on RELAY2 (supposed to be connected to the status light)
    digitalWrite(RELAY2, HIGH);

    /* task_scheduler.scheduleWithFixedDelay("esp32_temp", [this](){ */
	    /* logger.printfln("Temp: %d", (temperatureRead() - 32) / 1.8); */
    /* }, 0, 5000); */

    task_scheduler.scheduleWithFixedDelay("update_evse_state", [this](){
        update_evse_state();
    }, 0, 1000);

    task_scheduler.scheduleWithFixedDelay("update_evse_low_level_state", [this](){
        update_evse_low_level_state();
    }, 0, 1000);

#ifdef MODULE_CM_NETWORKING_AVAILABLE
    cm_networking.register_client([this](uint16_t current){
        set_managed_current(current);
    });

    task_scheduler.scheduleWithFixedDelay("evse_send_cm_networking_client", [this](){
        cm_networking.send_client_update(
            evse_state.get("iec61851_state")->asUint(),
            evse_state.get("vehicle_state")->asUint(),
            evse_state.get("error_state")->asUint(),
            evse_state.get("charge_release")->asUint(),
            evse_state.get("uptime")->asUint(),
            evse_state.get("allowed_charging_current")->asUint()
        );
    }, 1000, 1000);

    task_scheduler.scheduleWithFixedDelay("evse_managed_current_watchdog", [this]() {
        if (!deadline_elapsed(this->last_current_update + 30000))
            return;
        if(!this->shutdown_logged)
            logger.printfln("Got no managed current update for more than 30 seconds. Setting managed current to 0");
        this->shutdown_logged = true;
        evse_managed_current.get("current")->updateUint(0);
    }, 1000, 1000);
#endif
}

String sonoff::get_evse_debug_header() {
    return "millis,iec,vehicle,contactor,_error,charge_release,allowed_current,error,lock,t_state_change,uptime,low_level_mode_enabled,led,cp_pwm,adc_pe_cp,adc_pe_pp,voltage_pe_cp,voltage_pe_pp,voltage_pe_cp_max,resistance_pe_cp,resistance_pe_pp,gpio_in,gpio_out,gpio_motor_in,gpio_relay,gpio_motor_error\n";
}

String sonoff::get_evse_debug_line() {
    if(!initialized)
        return "EVSE is not initialized!";

    uint8_t iec61851_state, vehicle_state, contactor_state, contactor_error, charge_release, error_state, lock_state;
    uint16_t allowed_charging_current;
    uint32_t time_since_state_change, uptime;

    /* int rc = bs_evse_get_state(&evse, */
    /*     &iec61851_state, */
    /*     &vehicle_state, */
    /*     &contactor_state, */
    /*     &contactor_error, */
    /*     &charge_release, */
    /*     &allowed_charging_current, */
    /*     &error_state, */
    /*     &lock_state, */
    /*     &time_since_state_change, */
    /*     &uptime); */

    /* if(rc != TF_E_OK) { */
    /*     return String("evse_get_state failed: rc: ") + String(rc); */
    /* } */

    bool low_level_mode_enabled;
    uint8_t led_state;
    uint16_t cp_pwm_duty_cycle;

    uint16_t adc_values[2];
    int16_t voltages[3];
    uint32_t resistances[2];
    bool gpio[5];

//    rc = tf_evse_get_low_level_state(&evse,
//        &low_level_mode_enabled,
//        &led_state,
//        &cp_pwm_duty_cycle,
//        adc_values,
//        voltages,
//        resistances,
//        gpio);

    /* if(rc != TF_E_OK) { */
    /*     return String("evse_get_low_level_state failed: rc: ") + String(rc); */
    /* } */

    char line[150] = {0};
    snprintf(line, sizeof(line)/sizeof(line[0]), "%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%c,%u,%u,%u,%u,%d,%d,%d,%u,%u,%c,%c,%c,%c,%c\n",
        millis(),
        iec61851_state,
        vehicle_state,
        contactor_state,
        contactor_error,
        charge_release,
        allowed_charging_current,
        error_state,
        lock_state,
        time_since_state_change,
        uptime,
        low_level_mode_enabled ? '1' : '0',
        led_state,
        cp_pwm_duty_cycle,
        adc_values[0],adc_values[1],
        voltages[0],voltages[1],voltages[2],
        resistances[0],resistances[1],
        gpio[0] ? '1' : '0',gpio[1] ? '1' : '0',gpio[2] ? '1' : '0',gpio[3] ? '1' : '0',gpio[4] ? '1' : '0');

    return String(line);
}

void sonoff::set_managed_current(uint16_t current) {
    evse_managed_current.get("current")->updateUint(current);
    evse_max_charging_current.get("max_current_managed")->updateUint(current);
    this->last_current_update = millis();
    this->shutdown_logged = false;
}

void sonoff::register_urls()
{
    if (!evse_found)
        return;

    api.addPersistentConfig("evse/config", &evse_config, {}, 1000);

    api.addState("evse/state", &evse_state, {}, 1000);
    api.addState("evse/hardware_configuration", &evse_hardware_configuration, {}, 1000);
    api.addState("evse/low_level_state", &evse_low_level_state, {}, 1000);
    api.addState("evse/max_charging_current", &evse_max_charging_current, {}, 1000);
    api.addState("evse/auto_start_charging", &evse_auto_start_charging, {}, 1000);
    api.addState("evse/privcomm", &evse_privcomm, {}, 1000);

    api.addCommand("evse/auto_start_charging_update", &evse_auto_start_charging_update, {}, [this](){
        bs_evse_set_charging_autostart(&evse, evse_auto_start_charging_update.get("auto_start_charging")->asBool());
    }, false);

    api.addCommand("evse/current_limit", &evse_current_limit, {}, [this](){
        bs_evse_set_max_charging_current(&evse, evse_current_limit.get("current")->asUint());
    }, false);

    api.addCommand("evse/stop_charging", &evse_stop_charging, {}, [this](){bs_evse_stop_charging(&evse);}, true);
    api.addCommand("evse/start_charging", &evse_start_charging, {}, [this](){bs_evse_start_charging(&evse);}, true);

    api.addCommand("evse/managed_current_update", &evse_managed_current, {}, [this](){
        this->set_managed_current(evse_managed_current.get("current")->asUint());
    }, true);

    api.addState("evse/managed", &evse_managed, {}, 1000);
    api.addCommand("evse/managed_update", &evse_managed_update, {"password"}, [this](){
        evse_managed.get("managed")->updateBool(evse_managed_update.get("managed")->asBool());
        bs_evse_persist_config();
    }, true);

    api.addState("evse/user_calibration", &evse_user_calibration, {}, 1000);
    api.addCommand("evse/user_calibration_update", &evse_user_calibration, {}, [this](){
        this->set_managed_current(std::numeric_limits<std::uint16_t>::max());
        evse_state.get("iec61851_state")->updateUint(2); // connected
        evse_state.get("vehicle_state")->updateUint(2); // charging
//        int16_t resistance_880[14];
//        evse_user_calibration.get("resistance_880")->fillArray<int16_t, Config::ConfInt>(resistance_880, sizeof(resistance_880)/sizeof(resistance_880[0]));
//
//        tf_evse_set_user_calibration(&evse,
//            0xCA11B4A0,
//            evse_user_calibration.get("user_calibration_active")->asBool(),
//            evse_user_calibration.get("voltage_diff")->asInt(),
//            evse_user_calibration.get("voltage_mul")->asInt(),
//            evse_user_calibration.get("voltage_div")->asInt(),
//            evse_user_calibration.get("resistance_2700")->asInt(),
//            resistance_880
//            );
    }, true);

#ifdef MODULE_WS_AVAILABLE
    server.on("/evse/start_debug", HTTP_GET, [this](WebServerRequest request) {
        task_scheduler.scheduleOnce("enable evse debug", [this](){
            ws.pushStateUpdate(this->get_evse_debug_header(), "evse/debug_header");
            debug = true;
        }, 0);
        request.send(200);
    });

    server.on("/evse/stop_debug", HTTP_GET, [this](WebServerRequest request){
        task_scheduler.scheduleOnce("enable evse debug", [this](){
            debug = false;
        }, 0);
        request.send(200);
    });
#endif
}

void sonoff::loop()
{
    if(initialized) {
        static bool switch1_before;
        static bool switch2_before;
        static uint32_t charge_manager_available_current;

        switch1_before = switch1;
        switch2_before = switch2;
        switch1 = digitalRead(SWITCH1);
        switch2 = digitalRead(SWITCH2);

        if(switch1 != switch1_before) {
            digitalWrite(RELAY1, switch1);
            logger.printfln("Der Energieversorger %s das Laden von Elektroautos.", switch1 ? "verbietet" : "erlaubt");
            if(switch1) {
                charge_manager_available_current = api.getState("charge_manager/available_current")->get("current")->asUint();
                api.callCommand("charge_manager/available_current_update", Config::ConfUpdateObject{{ {"current", 0} }}); // der Rundsteuerempfänger sagt NEIN
                api.blockCommand("charge_manager/available_current_update", "Rundsteuerempfänger blockiert!");
                //api.callCommand("statuslight/color", Config::ConfUpdateObject{{ {"color", 0xff0000} }}); // red
            } else {
                api.callCommand("charge_manager/available_current_update", Config::ConfUpdateObject{{ {"current", charge_manager_available_current} }}); // alles wieder gut, restore the saved value
                api.blockCommand("charge_manager/available_current_update", "");
                //api.callCommand("statuslight/color", Config::ConfUpdateObject{{ {"color", 0x00ff00} }}); // green
            }
        }
        if(switch2 != switch2_before) {
            //digitalWrite(RELAY2, switch2);
            logger.printfln("Schalteingang 2 ist jetzt %sgeschaltet.", switch2 ? "aus" : "ein");
        }
    }

#ifdef MODULE_WS_AVAILABLE
    static uint32_t last_debug = 0;
    if(debug && deadline_elapsed(last_debug + 50)) {
        last_debug = millis();
        ws.pushStateUpdate(this->get_evse_debug_line(), "evse/debug");
    }
#endif
}

void sonoff::setup_evse()
{
    pinMode(RELAY1, OUTPUT);
    pinMode(RELAY2, OUTPUT);
    pinMode(SWITCH1, INPUT);
    pinMode(SWITCH2, INPUT);
    /* digitalWrite(RELAY1, digitalRead(SWITCH1)); */
    /* digitalWrite(RELAY2, digitalRead(SWITCH2)); */

    evse_found = true;
    initialized = true;
}

void sonoff::update_evse_low_level_state() {
    if(!initialized)
        return;

    /* bool low_level_mode_enabled; */
    /* uint8_t led_state; */
    /* uint16_t cp_pwm_duty_cycle; */

    /* uint16_t adc_values[2]; */
    /* int16_t voltages[3]; */
    /* uint32_t resistances[2]; */
    /* bool gpio[5]; */

//    int rc = tf_evse_get_low_level_state(&evse,
//        &low_level_mode_enabled,
//        &led_state,
//        &cp_pwm_duty_cycle,
//        adc_values,
//        voltages,
//        resistances,
//        gpio);

        /* low_level_mode_enabled = true; */
        /* led_state = 1; */
        /* cp_pwm_duty_cycle = 100; */
        /* adc_values[0] = 200; */
        /* adc_values[1] = 201; */
        /* voltages[0] = 300; */
        /* voltages[1] = 301; */
        /* voltages[2] = 302; */
        /* resistances[0] = 400; */
        /* resistances[1] = 401; */
        /* gpio[0] = false; */
        /* gpio[1] = false; */
        /* gpio[2] = false; */
        /* gpio[3] = false; */
        /* gpio[4] = false; */

    /* evse_low_level_state.get("low_level_mode_enabled")->updateBool(low_level_mode_enabled); */
    /* evse_low_level_state.get("led_state")->updateUint(led_state); */
    /* evse_low_level_state.get("cp_pwm_duty_cycle")->updateUint(cp_pwm_duty_cycle); */

    /* for(int i = 0; i < sizeof(adc_values)/sizeof(adc_values[0]); ++i) */
        /* evse_low_level_state.get("adc_values")->get(i)->updateUint(adc_values[i]); */

    /* for(int i = 0; i < sizeof(voltages)/sizeof(voltages[0]); ++i) */
        /* evse_low_level_state.get("voltages")->get(i)->updateInt(voltages[i]); */

    /* for(int i = 0; i < sizeof(resistances)/sizeof(resistances[0]); ++i) */
        /* evse_low_level_state.get("resistances")->get(i)->updateUint(resistances[i]); */

    /* for(int i = 0; i < sizeof(gpio)/sizeof(gpio[0]); ++i) */
        /* evse_low_level_state.get("gpio")->get(i)->updateBool(gpio[i]); */
}

void sonoff::update_evse_state() {
    if(!initialized)
        return;
    uint8_t iec61851_state, vehicle_state, contactor_state, contactor_error, charge_release, error_state, lock_state;
    uint16_t last_allowed_charging_current = evse_state.get("allowed_charging_current")->asUint();
    uint16_t allowed_charging_current;
    uint32_t time_since_state_change, uptime;

    /* int rc = bs_evse_get_state(&evse, */
    /*     &iec61851_state, */
    /*     &vehicle_state, */
    /*     &contactor_state, */
    /*     &contactor_error, */
    /*     &charge_release, */
    /*     &allowed_charging_current, */
    /*     &error_state, */
    /*     &lock_state, */
    /*     &time_since_state_change, */
    /*     &uptime); */

    /* firmware_update_allowed = true; */

    /* evse_state.get("iec61851_state")->updateUint(iec61851_state); */
    /* evse_state.get("vehicle_state")->updateUint(vehicle_state); */
    /* evse_state.get("contactor_state")->updateUint(contactor_state); */
    /* bool contactor_error_changed = evse_state.get("contactor_error")->updateUint(contactor_error); */
    /* evse_state.get("charge_release")->updateUint(charge_release); */
    if(last_allowed_charging_current != allowed_charging_current) {
        evse_state.get("allowed_charging_current")->updateUint(allowed_charging_current);
        logger.printfln("EVSE: allowed_charging_current %d", allowed_charging_current);
    }
    /* bool error_state_changed = evse_state.get("error_state")->updateUint(error_state); */
    /* evse_state.get("lock_state")->updateUint(lock_state); */
    //evse_state.get("time_since_state_change")->updateUint(time_since_state_change);
    evse_state.get("uptime")->updateUint(millis());
}
