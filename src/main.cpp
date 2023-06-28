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
#define DEBUG
#include <ArduinoJson.h>

#include "sensors.h"

using namespace std;
using namespace std::chrono_literals;

constexpr auto SERIAL_BAUND    = 115200;
constexpr auto SERVER_PORT_WEB = 80;

constexpr auto AP_MODE_TIMEOUT           = 30s; // switch to ap if no wifi
constexpr auto AUTO_REBOOT_AFTER_AP_MODE = 5min; // switch to ap if no wifi
constexpr auto BEFORE_SLEEP_TIMEOUT      = 5s; // switch to ap if no wifi

constexpr auto DEVICE_NAME = "weather3";
const char*    update_path = "/firmware";
constexpr auto DEF_AP_PWD  = "12345678";

using led_status::cled_status;

const char* pDeviceName = nullptr;

ESP8266WebServer        serverWeb(SERVER_PORT_WEB);
CMQTT                   mqtt;
ESP8266HTTPUpdateServer otaUpdater;
CWifiStateSignal        wifiStateSignal;
auto                    config = CConfig<512>();
cled_status             status_led;

StaticJsonDocument<256> sensors;

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
    const auto sleeptime = chrono::seconds(config.getInt("DEEP_SLEEP_S"));
    DBG_OUT << "deepSleep, sleeptime=" << sleeptime.count() << endl;
    ESP.deepSleep(chrono::microseconds(sleeptime).count());
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

    wifiStateSignal.onChange([](const wl_status_t& status) {
        DBG_OUT << "wifiState" << status << endl;
        if (WL_CONNECTED == status) {
            // skip AP mode
            DBG_OUT << "wifi RSSI=" << static_cast<signed>(WiFi.RSSI()) << endl;
            if (to_ap_mode_thread) {
                to_ap_mode_thread->cancel();
            }
            mqtt.setup(config.getCSTR("MQTT_SERVER_IP"), config.getInt("MQTT_PORT"), pDeviceName);
            mqtt.connect([](auto is_connected) {
                if (is_connected) {
                    try_tosend_data(false);
                }
            });
        }
        wifi_status(cout);
    });

    to_ap_mode_thread = event_loop::set_timeout(
        []() {
            status_led.set(cled_status::value_t::Warning);
            WiFi.persistent(false);
            setup_wifi("", DEF_AP_PWD, pDeviceName, WIFI_AP, false);

            event_loop::set_timeout(
                []() {
                    DBG_OUT << "Rebooting" << std::endl;
                    ESP.restart();
                },
                AUTO_REBOOT_AFTER_AP_MODE);
        },
        AP_MODE_TIMEOUT);

    if (WIFI_STA == WiFi.getMode()) {
        DBG_OUT << "connecting <" << WiFi.SSID() << "> " << endl;
        return;
    }
}

void try_tosend_data(bool force) {
    constexpr auto all_sensors = 4;

    if (force || (sensors.size() == all_sensors)) {
        if (mqtt.isConnected()) {
            String json_string;
            serializeJson(sensors, json_string);
            mqtt.publish(std::string("stat/") + pDeviceName, json_string.c_str());
            status_led.set(cled_status::value_t::Work);
        } else {
            status_led.set(cled_status::value_t::Warning);
            DBG_OUT << "mqtt is not connected, state=" << mqtt.get_client().state() << endl;
        }

        if (WiFi.getMode() != WIFI_AP) {
            event_loop::set_timeout([]() { deep_sleep(); }, BEFORE_SLEEP_TIMEOUT);
        }
    }
}

void collect_data() {
    sensors.clear();
    sensor::bmp180_get([](auto temperature, auto pressure, auto status) {
        if (status) {
            sensors["bmp180"]["temperature"] = String(temperature, 2);
            sensors["bmp180"]["pressure"]    = String(pressure, 2);
            try_tosend_data(false);
        }
    });
    sensor::dht_get([](auto temperature, auto humidity, auto status) {
        if (status) {
            sensors["dht"]["temperature"] = String(temperature, 2);
            sensors["dht"]["humidity"]    = String(humidity, 2);
            try_tosend_data(false);
        }
    });
    sensor::BH1750_get([](const float lux, bool status) {
        if (status) {
            sensors["BH1750"]["value"] = String(lux, 2);
            try_tosend_data(false);
        }
    });
    sensor::battery_get([](const float volt, bool status) {
        if (status) {
            sensors["battery"]["value"] = String(volt, 2);
            try_tosend_data(false);
        }
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
    config.getConfig()["DEVICE_NAME"]        = DEVICE_NAME;
    config.getConfig()["MQTT_SERVER_IP"]     = "";
    config.getConfig()["MQTT_PORT"]          = 0;
    config.getConfig()["MQTT_PERIOD"]        = 60 * 1000;
    config.getConfig()["OTA_USERNAME"]       = "";
    config.getConfig()["OTA_PASSWORD"]       = "";
    config.getConfig()["DEEP_SLEEP_S"]       = 60 * 15;
    config.getConfig()["MAX_COLLECT_TIME_S"] = 15;
    if (!config.load("/www/config/config.json")) {
        // write file
        config.write("/www/config/config.json");
    }
    pDeviceName = config.getCSTR("DEVICE_NAME");
}

void setup() {
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

    //-----------------
    setup_WIFIConnect();
    DBG_OUT << "Setup done" << endl;
}

void loop() {
    wifiStateSignal.loop();
    serverWeb.handleClient();
    event_loop::loop();
    mqtt.loop();
}
