/*
 * sensors.h
 *
 *  Created on: Jun 24, 2023
 *      Author: oleksandr
 */

#ifndef SRC_SENSORS_H_
#define SRC_SENSORS_H_
#include <functional>
#include <stdint.h>

namespace sensor {

void init();
void power_off();
void bme280_get(
    std::function<void(const float temperature, const float pressure, const float humidity, const bool is_successful)>
        cb);
void sth30_get(std::function<void(const float temperature, const float humidity, const bool is_successful)> cb);
void bh1750_light_get(std::function<void(const float lux)> cb);
void battery_get(std::function<void(const float volt)> cb);

} // namespace sensor

#endif /* SRC_SENSORS_H_ */
