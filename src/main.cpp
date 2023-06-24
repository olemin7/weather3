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

#include "DHTesp.h"

#include <CMQTT.h>

// clang-format on
#include <misk.h>
#include <wifiHandle.h>
#include <logs.h>
#include <CConfig.h>

#include <eventloop.h>
#include <cledstatus.h>
#define DEBUG
#include <chrono>

using namespace std;
using namespace std::chrono_literals;

constexpr auto SERIAL_BAUND = 115200;
constexpr auto SERVER_PORT_WEB = 80;

constexpr auto AP_MODE_TIMEOUT = 30s; // switch to ap if no wifi
constexpr auto AUTO_REBOOT_AFTER_AP_MODE = 5min; // switch to ap if no wifi

constexpr auto pinCS = D6;
constexpr auto numberOfHorizontalDisplays = 4;
constexpr auto numberOfVerticalDisplays = 1;

constexpr auto DHTPin = D4;

constexpr auto DEVICE_NAME = "weather3";
const char *update_path = "/firmware";
constexpr auto DEF_AP_PWD = "12345678";

using led_status::cled_status;

void mqtt_send();

const char *pDeviceName = nullptr;

DHTesp dht;

ESP8266WebServer serverWeb(SERVER_PORT_WEB);
CMQTT mqtt;
ESP8266HTTPUpdateServer otaUpdater;
CWifiStateSignal wifiStateSignal;
auto config = CConfig<512>();
cled_status status_led;

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
    out << "\"temperature\":";
    toJson(out, dht.getTemperature());
    out << ",\"humidity\":";
    toJson(out, dht.getHumidity());
    out << ",\"mqtt\":" << mqtt.isConnected();
    out << "}";
    return er_ok;
}

void setup_WebPages() {
    DBG_FUNK();
    otaUpdater.setup(&serverWeb, update_path, config.getCSTR("OTA_USERNAME"),
            config.getCSTR("OTA_PASSWORD"));

    serverWeb.on("/restart", []() {
        webRetResult(serverWeb, er_ok);
        delay(1000);
        ESP.restart();
    });

    serverWeb.on("/about",
            [] {
                wifiHandle_send_content_json(serverWeb, get_about);
            });

    serverWeb.on("/status",
            [] {
                wifiHandle_send_content_json(serverWeb, get_status);
            });

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

    serverWeb.on("/scanwifi", HTTP_ANY,
            [&]() {
                wifiHandle_sendlist(serverWeb);
            });
    serverWeb.on("/connectwifi", HTTP_ANY,
            [&]() {
                wifiHandle_connect(pDeviceName, serverWeb, true);
            });

    serverWeb.on("/getlogs", HTTP_ANY, [&]() {
        serverWeb.send(200, "text/plain", log_buffer.c_str());
        log_buffer = "";
    });

    serverWeb.on("/set_time", [&]() {
        DBG_FUNK();
        // todo
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
        DBG_OUT << "wifiStateSignal" << endl;
        if (WL_CONNECTED == status) {
            // skip AP mode
            if (to_ap_mode_thread) {
                status_led.set(cled_status::value_t::Work);
                to_ap_mode_thread->cancel();
            }
        }
        wifi_status(cout);
    });

    to_ap_mode_thread = event_loop::set_timeout(
            []() {
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

void setup_signals() {
}

void setup_mqtt() {
    DBG_FUNK();
    mqtt.setup(config.getCSTR("MQTT_SERVER"), config.getInt("MQTT_PORT"),
            pDeviceName);
    string topic = "cmd/";
    topic += pDeviceName;
    mqtt.callback(topic, [](char* topic, uint8_t* payload, unsigned int length) {
        DBG_OUT << "MQTT>>[" << topic << "]:";
        auto tt = reinterpret_cast<const char*>(payload);
        auto i = length;
        while (i--) {
            DBG_OUT << *tt;
            tt++;
        };
        DBG_OUT << endl;
        StaticJsonDocument<512> json_cmd;
        DeserializationError error = deserializeJson(json_cmd, payload, length);
        if (error) {
            DBG_OUT << "Failed to read file, using default configuration" << endl;
        }
    });
}

void setup_config() {
    config.getConfig().clear();
    config.getConfig()["DEVICE_NAME"] = DEVICE_NAME;
    config.getConfig()["MQTT_SERVER"] = "";
    config.getConfig()["MQTT_PORT"] = 0;
    config.getConfig()["MQTT_PERIOD"] = 60 * 1000;
    config.getConfig()["OTA_USERNAME"] = "";
    config.getConfig()["OTA_PASSWORD"] = "";
    if (!config.load("/www/config/config.json")) {
        // write file
        config.write("/www/config/config.json");
    }
    pDeviceName = config.getCSTR("DEVICE_NAME");
}

void setup() {
    Serial.begin(SERIAL_BAUND);
    logs_begin();
    DBG_FUNK();

    hw_info(DBG_OUT);
    LittleFS.begin();
    event_loop::init();
    status_led.setup();
    status_led.set(cled_status::value_t::Warning);
    setup_config();
    MDNS.addService("http", "tcp", SERVER_PORT_WEB);
    MDNS.begin(pDeviceName);
    setup_WebPages();
    setup_signals();

    LittleFS_info(DBG_OUT);
    setup_mqtt();

    //------------------
    dht.setup(DHTPin, DHTesp::DHT22);
    //-----------------
    setup_WIFIConnect();
    DBG_OUT << "Setup done" << endl;
}

static unsigned long nextMsgMQTT = 0;

void mqtt_send() {
    if (mqtt.isConnected()) {
        nextMsgMQTT = millis() + config.getULong("MQTT_PERIOD");
        string topic = "stat/";
        topic += pDeviceName;
        ostringstream payload;
        get_status(payload);
        DBG_OUT << "MQTT<<[" << topic << "]:" << payload.str() << endl;
        mqtt.publish(topic, payload.str());
    } else {
        nextMsgMQTT = 0; // force to send after connection
    }
}

void mqtt_loop() {
    if (WL_CONNECTED != WiFi.status()) {
        return;
    }
    mqtt.loop();

    if (millis() >= nextMsgMQTT) { // send
        mqtt_send();
    }
}

void loop() {
    wifiStateSignal.loop();
    mqtt_loop();
    serverWeb.handleClient();
    event_loop::loop();
}
