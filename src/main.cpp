// #define DEBUG
// clang-format off
#include <Arduino.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <LittleFS.h>

#include <Wire.h> // must be included here so that Arduino library object file references work
#include <pgmspace.h>
#include <stdio.h>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <chrono>
#include <algorithm>

#include <CMQTT.h>

// clang-format on
#include <CConfig.h>
#include <cledstatus.h>
#include <eventloop.h>
#include <logs.h>
#include <misk.h>
#include <wifiHandle.h>
#include <ArduinoJson.h>

#include "sensors.h"

using namespace std;
using namespace std::chrono_literals;

constexpr auto SERIAL_BAUND    = 115200;
constexpr auto SERVER_PORT_WEB = 80;

constexpr auto AP_MODE_TIMEOUT           = 30s; // switch to ap if no wifi
constexpr auto AUTO_REBOOT_AFTER_AP_MODE = 5min; // switch to ap if no wifi
constexpr auto BEFORE_SLEEP_TIMEOUT      = 1s; //

constexpr auto DEVICE_NAME = "weather3";
const char*    update_path = "/firmware";
constexpr auto DEF_AP_PWD  = "12345678";

using led_status::cled_status;

const char* pDeviceName = nullptr;

bool           is_service_mode  = false;
constexpr auto SERVICE_MODE_PIN = D5;

ESP8266WebServer        serverWeb(SERVER_PORT_WEB);
CMQTT                   mqtt;
ESP8266HTTPUpdateServer otaUpdater;
auto                    config = CConfig<512>();
cled_status             status_led;

StaticJsonDocument<512> sensors;

void try_tosend_data(bool force);

te_ret get_about(ostream& out) {
    out << "{";
    out << "\"firmware\":\"" << DEVICE_NAME << "\"" << __DATE__ << " " << __TIME__ << "\"";
    out << ",\"deviceName\":\"" << pDeviceName << "\"";
    out << ",\"resetInfo\":" << system_get_rst_info()->reason;
    out << "}";
    return er_ok;
}

te_ret get_status(ostream& out) {
    out << "{";
    out << "\"mqtt\":" << mqtt.isConnected();
    out << "}";
    return er_ok;
}

void deep_sleep() {
    if (is_service_mode) {
        DBG_OUT << "iservice_mode, deepSleep is disabled" << std::endl;
        return;
    }
    sensor::power_off();
    auto sleeptime_s = config.getInt("DEEP_SLEEP_S");
    if (sensors.containsKey("upd_period")) {
        sleeptime_s = sensors["upd_period"];
    }

    DBG_OUT << "deepSleep, sleeptime=" << sleeptime_s << "s" << std::endl;
    delay(1000);
    ESP.deepSleep(sleeptime_s * 1000000); // microseconds
}

void setup_WebPages() {
    DBG_FUNK();
    otaUpdater.setup(&serverWeb, update_path, config.getCSTR("OTA_USERNAME"), config.getCSTR("OTA_PASSWORD"));

    serverWeb.on("/restart", []() {
        webRetResult(serverWeb, er_ok);
        delay(1000);
        ESP.restart();
    });

    serverWeb.on("/about", [] { wifiHandle_send_content_json(serverWeb, get_about); });

    serverWeb.on("/status", [] { wifiHandle_send_content_json(serverWeb, get_status); });

    serverWeb.on("/filesave", []() {
        DBG_FUNK();
        if (!serverWeb.hasArg("path") || !serverWeb.hasArg("payload")) {
            webRetResult(serverWeb, er_no_parameters);
            return;
        }
        const auto path = string("/www/") + serverWeb.arg("path").c_str();
        cout << path << endl;
        auto file = LittleFS.open(path.c_str(), "w");
        if (!file) {
            webRetResult(serverWeb, er_createFile);
            return;
        }
        if (!file.print(serverWeb.arg("payload"))) {
            webRetResult(serverWeb, er_FileIO);
            return;
        }
        file.close();
        webRetResult(serverWeb, er_ok);
    });

    serverWeb.on("/scanwifi", HTTP_ANY, [&]() { wifiHandle_sendlist(serverWeb); });
    serverWeb.on("/connectwifi", HTTP_ANY, [&]() { wifiHandle_connect(pDeviceName, serverWeb, true); });

    serverWeb.on("/getlogs", HTTP_ANY, [&]() {
        serverWeb.send(200, "text/plain", log_buffer.c_str());
        log_buffer = "";
    });

    serverWeb.serveStatic("/", LittleFS, "/www/");

    serverWeb.onNotFound([] {
        Serial.println("Error no handler");
        Serial.println(serverWeb.uri());
        webRetResult(serverWeb, er_fileNotFound);
    });
    serverWeb.begin();
}

void setup_WIFIConnect() {
    DBG_FUNK();
    static event_loop::pevent to_ap_mode_thread;
    WiFi.hostname(pDeviceName);
    WiFi.begin();
    if (is_service_mode) {
        to_ap_mode_thread = event_loop::set_timeout(
            []() {
                DBG_OUT << "Rebooting" << std::endl;
                setup_wifi("", DEF_AP_PWD, pDeviceName, WIFI_AP, false);

                event_loop::set_timeout(
                    []() {
                        DBG_OUT << "Rebooting" << std::endl;
                        ESP.restart();
                    },
                    AUTO_REBOOT_AFTER_AP_MODE);
            },
            AP_MODE_TIMEOUT);
    }

    static WiFiEventHandler onStationModeConnected = WiFi.onStationModeConnected([](auto event) {
        DBG_OUT << "WiFi conected, ssid =" << event.ssid << ", channel=" << static_cast<unsigned>(event.channel)
                << endl;
    });

    static WiFiEventHandler onStationModeGotIP = WiFi.onStationModeGotIP([](auto event) {
        if (to_ap_mode_thread) {
            to_ap_mode_thread->cancel();
        }
        DBG_OUT << "WiFi IP=" << event.ip << ", mask=" << event.mask << ", gw=" << event.gw << endl;
        sensors["wifi"]["rssi"] = WiFi.RSSI();
        sensors["wifi"]["ip"]   = event.ip.toString();

        event_loop::set_timeout(
            []() {
                DBG_OUT << "mqtt connection" << endl;
                mqtt.setup(config.getCSTR("MQTT_SERVER_IP"), config.getInt("MQTT_PORT"), pDeviceName);
                mqtt.connect([](auto is_connected) {
                    if (is_connected) {
                        try_tosend_data(false);
                    } else {
                        deep_sleep();
                    }
                });
            },
            0s);
    });
}

bool is_data_collected(const JsonDocument& data) {
    if (data.containsKey("weather")) {
        if (sensors["weather"].containsKey("temperature") == false) {
            return false;
        }
        if (sensors["weather"].containsKey("pressure") == false) {
            return false;
        }
        if (sensors["weather"].containsKey("humidity") == false) {
            return false;
        }
        if (sensors["weather"].containsKey("ambient_light") == false) {
            return false;
        }
    } else {
        return false;
    }
    if (data.containsKey("battery") == false) {
        return false;
    }
    if (data.containsKey("wifi") == false) {
        return false;
    }

    return true;
}

void try_tosend_data(bool force) {
    if (force || is_data_collected(sensors)) {
        DBG_OUT << "sensors used sz=" << sensors.memoryUsage() << endl;

        if (mqtt.isConnected()) {
            String      json_string;
            std::string topic;
            topic = config.getCSTR("MQTT_TOPIC_SENSORS");
            topic += "/";
            topic += pDeviceName;
            serializeJson(sensors, json_string);
            mqtt.publish(topic.c_str(), json_string.c_str());
            mqtt.get_client().flush(); // force to send
            status_led.set(cled_status::value_t::Work);
        } else {
            status_led.set(cled_status::value_t::Warning);
            DBG_OUT << "mqtt is not connected, state=" << mqtt.get_client().state() << endl;
        }
        event_loop::set_timeout([]() { deep_sleep(); }, BEFORE_SLEEP_TIMEOUT);
    }
}

void collect_data() {
    DBG_FUNK();
    sensors.clear();
    sensor::bme280_get([](auto temperature, auto pressure, auto humidity, auto is_successful) {
        if (is_successful) {
            sensors["weather"]["pressure"] = pressure;
            try_tosend_data(false);
        }
    });

    sensor::sth30_get([](auto temperature, auto humidity, auto is_successful) {
        if (is_successful) {
            sensors["weather"]["temperature"] = temperature;
            sensors["weather"]["humidity"]    = humidity;
            try_tosend_data(false);
        }
    });

    sensor::bh1750_light_get([](auto lux) {
        sensors["weather"]["ambient_light"] = static_cast<int>(lux);
        try_tosend_data(false);
    });
    sensor::battery_get([](const float volt) {
        const auto min        = config.getFloat("V_BAT_MIN");
        const auto max        = config.getFloat("V_BAT_MAX");
        auto       percentage = static_cast<int>((volt - min) * 100 / (max - min));
        if (percentage < 0) {
            percentage = 0;
        }
        if (percentage > 100) {
            percentage = 100;
        }

        DBG_OUT << "battery adc=" << volt << ", pers=" << percentage << std::endl;

        sensors["battery"] = percentage;

        sensors["upd_period"]         = config.getInt("DEEP_SLEEP_S");
        const auto low_bat_percentage = config.getInt("LOW_BAT_PERCENAGE");
        if (percentage < low_bat_percentage) {
            DBG_OUT << "low bat=" << percentage << ", Threshold=" << low_bat_percentage << std::endl;
            sensors["upd_period"] = config.getInt("DEEP_SLEEP_LOW_BAT_S");
        }

        try_tosend_data(false);
    });

    event_loop::set_timeout(
        []() {
            DBG_OUT << "MAX_COLLECT_TIME passed, sending as is" << endl;
            try_tosend_data(true);
        },
        chrono::seconds(config.getInt("MAX_COLLECT_TIME_S")));
}

void setup_config() {
    config.getConfig().clear();
    config.getConfig()["DEVICE_NAME"]          = DEVICE_NAME;
    config.getConfig()["MQTT_SERVER_IP"]       = "";
    config.getConfig()["MQTT_PORT"]            = 0;
    config.getConfig()["MQTT_TOPIC_SENSORS"]   = "sensors";
    config.getConfig()["OTA_USERNAME"]         = "";
    config.getConfig()["OTA_PASSWORD"]         = "";
    config.getConfig()["DEEP_SLEEP_S"]         = 60 * 15;
    config.getConfig()["DEEP_SLEEP_LOW_BAT_S"] = 60 * 60;
    config.getConfig()["MAX_COLLECT_TIME_S"]   = 15;
    config.getConfig()["V_BAT_MIN"]            = 2.4;
    config.getConfig()["V_BAT_MAX"]            = 4.2;
    config.getConfig()["LOW_BAT_PERCENAGE"]    = 30;
    if (!config.load("/www/config/config.json")) {
        // write file
        config.write("/www/config/config.json");
    }
    pDeviceName = config.getCSTR("DEVICE_NAME");
}

void setup() {
    pinMode(SERVICE_MODE_PIN, INPUT_PULLUP);
    Serial.begin(SERIAL_BAUND);
    Wire.begin();
    logs_begin();
    DBG_FUNK();

    hw_info(DBG_OUT);
    LittleFS.begin();
    event_loop::init();
    status_led.setup();
    status_led.set(cled_status::value_t::Processing);
    setup_config();
    sensor::init();
    collect_data();

    MDNS.addService("http", "tcp", SERVER_PORT_WEB);
    MDNS.begin(pDeviceName);
    setup_WebPages();

    LittleFS_info(DBG_OUT);
    is_service_mode = !deboncedPin(SERVICE_MODE_PIN, 30);
    if (is_service_mode) {
        DBG_OUT << "in service mode" << endl;
        status_led.set(cled_status::value_t::On);
    }

    //-----------------
    setup_WIFIConnect();

    DBG_OUT << "Setup done" << endl;
}

void loop() {
    serverWeb.handleClient();
    event_loop::loop();
    mqtt.loop();
}
