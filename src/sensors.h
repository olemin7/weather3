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
void bmp_get(std::function<void(const float temperature, const float pressure, const float humidity)> cb);
void ambient_light_get(std::function<void(const float lux)> cb);
void battery_get(std::function<void(const float volt)> cb);

} // namespace sensor

#endif /* SRC_SENSORS_H_ */
