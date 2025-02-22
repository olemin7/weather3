/*
 * sensors.cpp
 *
 *  Created on: Jun 24, 2023
 *      Author: oleksandr
 */

#include "sensors.h"
#include <chrono>

#include <Arduino.h>
#include <BH1750.h>
#include "SparkFunBME280.h"
#include <Wire.h>
#include <SHT31.h>
#include <eventloop.h>
#include <logs.h>
#include <misk.h>

namespace sensor {
using namespace std::chrono_literals;

BME280         bme280;
BH1750         bh1750(0x23); // 0x23 or 0x5C
SHT31          sht;
constexpr auto DHTPin  = D4;
constexpr auto ADC_pin = A0;

constexpr auto     measuring_timeout = 20ms;
event_loop::pevent p_bmp_timer;

void init() {
    DBG_FUNK();
    bme280.setI2CAddress(0x76);
    if (!bme280.beginI2C()) {
        DBG_OUT << "BME280 init fail" << std::endl;
    } else {
        bme280.setMode(MODE_SLEEP); // Sleep for now
        DBG_OUT << "BME280 init ok, id=" << static_cast<unsigned>(bme280.readRegister(BME280_CHIP_ID_REG)) << std::endl;
    }
    if (!bh1750.begin(BH1750::ONE_TIME_HIGH_RES_MODE)) {
        DBG_OUT << "BH1750 init fail" << std::endl;
    } else {
        DBG_OUT << "BH1750 init ok" << std::endl;
    }
    if (sht.begin(0x44) && sht.isConnected()) {
        DBG_OUT << "sht connected, status=" << to_hex(sht.readStatus()) << std::endl;
    } else {
        DBG_OUT << "sht init fail" << std::endl;
    }

    pinMode(ADC_pin, INPUT);
}

void power_off() {
    bme280.setMode(MODE_SLEEP);
    bh1750.configure(BH1750::UNCONFIGURED);
    sht.heatOff();
}

void bme280_get(
    std::function<void(const float temperature, const float pressure, const float humidity, const bool is_successful)>
        cb) {
    bme280.setMode(MODE_FORCED); // Wake up sensor and take reading
    while (bme280.isMeasuring()) {
    }; // waiting for initial state

    static event_loop::pevent p_timer;
    p_timer = event_loop::set_interval( // wait for finish measure
        [cb = std::move(cb)]() {
            if (bme280.isMeasuring() == false) {
                p_timer->cancel();
                BME280_SensorMeasurements measurements;
                bme280.readAllMeasurements(&measurements, 0);
                measurements.pressure /= 100;
                DBG_OUT << "temp= " << measurements.temperature << ", pressure=" << measurements.pressure
                        << ", humidity=" << measurements.humidity << std::endl;

                cb(measurements.temperature, measurements.pressure, measurements.humidity, true);
            }
        },
        measuring_timeout, false);
}

void sth30_get(std::function<void(const float temperature, const float humidity, const bool is_successful)> cb) {
    sht.requestData();
    static event_loop::pevent p_timer;
    p_timer = event_loop::set_interval(
        [cb = std::move(cb)]() {
            if (sht.readData()) {
                const auto temperature = sht.getTemperature();
                const auto humidity    = sht.getHumidity();
                DBG_OUT << "temperature= " << temperature << ", humidity=" << humidity << std::endl;
                p_timer->cancel();
                cb(temperature, humidity, true);
            }
        },
        measuring_timeout, true);
}

void bh1750_light_get(std::function<void(const float lux)> cb) {
    static event_loop::pevent p_timer;
    p_timer = event_loop::set_interval(
        [cb = std::move(cb)]() {
            if (bh1750.measurementReady()) {
                const auto lux = bh1750.readLightLevel();
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
            const auto volt = static_cast<float>(val) * 4.1 / 954; //(133 + 220 + 100) / 100 / 1024;
            DBG_OUT << "adc val= " << val << ",volt=" << volt << std::endl;
            cb(volt);
        },
        300ms);
}

} // namespace sensor
