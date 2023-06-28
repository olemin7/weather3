/*
 * sensors.cpp
 *
 *  Created on: Jun 24, 2023
 *      Author: oleksandr
 */

#include "sensors.h"

#include <Arduino.h>
#include <BH1750.h>
#include <SFE_BMP180.h>
#include <Wire.h>
#include <eventloop.h>
#include <logs.h>

#include <chrono>

#include "DHTesp.h"

namespace sensor {
using namespace std::chrono_literals;

SFE_BMP180     pressure;
DHTesp         dht;
BH1750         lightMeter(0x23); // 0x23 or 0x5C
constexpr auto DHTPin  = D4;
constexpr auto ADC_pin = A0;

void init() {
    if (!pressure.begin()) {
        DBG_OUT << "BMP180 init fail" << std::endl;
    }
    dht.setup(DHTPin, DHTesp::AM2302);
    if (!lightMeter.begin(BH1750::ONE_TIME_HIGH_RES_MODE)) {
        DBG_OUT << "BH1750 init fail" << std::endl;
    }
    pinMode(ADC_pin, INPUT);
}

void bmp180_get(std::function<void(const float temperature, const float pressure, bool status)> cb) {
    const auto status = pressure.startTemperature();
    if (status != 0) {
        event_loop::set_timeout(
            [cb]() {
                double     temperature;
                const auto status = pressure.getTemperature(temperature);
                if (status) {
                    DBG_OUT << "Temperature: " << temperature << " deg C" << std::endl;
                    auto status = pressure.startPressure(3); // The parameter is the oversampling setting, from 0 to 3
                                                             // (highest res, longest wait)
                    if (status) {
                        event_loop::set_timeout(
                            [cb, temperature]() {
                                double     presure, tmp = temperature;
                                const auto status = pressure.getPressure(presure, tmp);
                                if (status) {
                                    DBG_OUT << "absolute pressure: " << presure << " mb" << std::endl;
                                    cb(tmp, presure, true);
                                } else {
                                    DBG_OUT << "error, getPressure " << std::endl;
                                    cb(0, 0, false);
                                }
                            },
                            std::chrono::milliseconds(status));
                    } else {
                        DBG_OUT << "error, startPressure" << std::endl;
                        cb(0, 0, false);
                    }

                } else {
                    DBG_OUT << "error, getTemperature= " << std::endl;
                    cb(0, 0, false);
                }
            },
            std::chrono::milliseconds(status));
    } else {
        DBG_OUT << "error, startTemperature " << std::endl;
        cb(0, 0, false);
    }
}
void dht_get(std::function<void(const float temperature, const float humidity, bool status)> cb) {
    event_loop::set_timeout(
        [cb]() {
            const auto temperature = dht.getTemperature();
            DBG_OUT << "temperature= " << temperature << ", status=" << dht.getStatusString() << std::endl;
            const auto st       = dht.getStatus() == DHTesp::ERROR_NONE;
            const auto humidity = dht.getHumidity();
            DBG_OUT << "humidity= " << humidity << ", status=" << dht.getStatusString() << std::endl;
            const auto sh = dht.getStatus() == DHTesp::ERROR_NONE;
            cb(temperature, humidity, st & sh);
        },
        std::chrono::milliseconds(dht.getMinimumSamplingPeriod()));
}

void BH1750_get(std::function<void(const float lux, bool status)> cb) {
    if (lightMeter.measurementReady()) {
        const auto lux = lightMeter.readLightLevel();
        DBG_OUT << "Light= " << lux << "lx" << std::endl;
        cb(lux, true);
    } else {
        event_loop::set_timeout([cb = std::move(cb)]() { BH1750_get(std::move(cb)); }, 200ms);
    }
}
void battery_get(std::function<void(const float volt, bool status)> cb) {
    const auto first = analogRead(ADC_pin);
    event_loop::set_timeout(
        [first, cb = std::move(cb)]() {
            const auto val  = (first + analogRead(ADC_pin)) / 2;
            const auto volt = static_cast<float>(val) * (130 + 220 + 100) / 100 / 1024;
            DBG_OUT << "adc val= " << val << ",volt=" << volt << std::endl;
            cb(volt, true);
        },
        100ms);
}

} // namespace sensor
