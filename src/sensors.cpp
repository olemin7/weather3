/*
 * sensors.cpp
 *
 *  Created on: Jun 24, 2023
 *      Author: oleksandr
 */

#include "sensors.h"

#include <Arduino.h>
#include <BH1750.h>
#include "SparkFunBME280.h"
#include <Wire.h>
#include <eventloop.h>
#include <logs.h>

#include <chrono>

namespace sensor {
using namespace std::chrono_literals;

BME280         sensor;
BH1750         lightMeter(0x23); // 0x23 or 0x5C
constexpr auto DHTPin  = D4;
constexpr auto ADC_pin = A0;

constexpr auto     measuring_timeout = 20ms;
event_loop::pevent p_bmp_timer;

void init() {
    sensor.setI2CAddress(0x76);
    if (!sensor.beginI2C()) {
        DBG_OUT << "BME280 init fail" << std::endl;
    }
    sensor.setMode(MODE_SLEEP); // Sleep for now
    if (!lightMeter.begin(BH1750::ONE_TIME_HIGH_RES_MODE)) {
        DBG_OUT << "BH1750 init fail" << std::endl;
    }
    pinMode(ADC_pin, INPUT);
}

void bmp_get(std::function<void(const float temperature, const float pressure, const float humidity)> cb) {
    sensor.setMode(MODE_FORCED); // Wake up sensor and take reading
    while (sensor.isMeasuring()) // waiting for initial state
        ;
    static event_loop::pevent p_timer;
    p_timer = event_loop::set_interval( // wait for finish measure
        [cb = std::move(cb)]() {
            if (sensor.isMeasuring() == false) {
                p_timer->cancel();
                const auto temp     = sensor.readTempC();
                const auto pressure = sensor.readFloatPressure() / 100;
                const auto humidity = sensor.readFloatHumidity();
                DBG_OUT << "temp= " << temp << ", pressure=" << pressure << ", humidity=" << humidity << std::endl;

                cb(temp, pressure, humidity);
            }
        },
        measuring_timeout, false);
}

void ambient_light_get(std::function<void(const float lux)> cb) {
    static event_loop::pevent p_timer;
    p_timer = event_loop::set_interval(
        [cb = std::move(cb)]() {
            if (lightMeter.measurementReady()) {
                const auto lux = lightMeter.readLightLevel();
                DBG_OUT << "ambient_light= " << lux << "lx" << std::endl;
                p_timer->cancel();
                cb(lux);
            }
        },
        measuring_timeout, true);
}

void battery_get(std::function<void(const float volt)> cb) {
    const auto first = analogRead(ADC_pin);
    event_loop::set_timeout(
        [first, cb = std::move(cb)]() {
            const auto val  = (first + analogRead(ADC_pin)) / 2;
            const auto volt = static_cast<float>(val) * (130 + 220 + 100) / 100 / 1024;
            DBG_OUT << "adc val= " << val << ",volt=" << volt << std::endl;
            cb(volt);
        },
        300ms);
}

} // namespace sensor
