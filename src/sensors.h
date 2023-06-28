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
void bmp180_get(std::function<void(const float temperature, const float pressure, bool status)> cb);

void dht_get(std::function<void(const float temperature, const float humidity, bool status)> cb);

void BH1750_get(std::function<void(const float lux, bool status)> cb);
void battery_get(std::function<void(const float volt, bool status)> cb);

} // namespace sensor

#endif /* SRC_SENSORS_H_ */
