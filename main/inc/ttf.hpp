#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "vl53l0x.hpp"
#include <cstdint>
#include "config.hpp"

class Ttf {
private:
  static void task_wrapper(void *pvParameters) {
    Ttf *pThis = static_cast<Ttf *>(pvParameters);
    pThis->task();
  }
  void task();
  esp_err_t i2c_switch_to_ch(uint8_t ch);
  uint16_t laser_values[10];
  uint64_t laser_timestamps[10];
  VL53L0X sensor[6];

public:
  static const constexpr char* const sensor_names[] = VL53L0X_ORDER;
  Ttf();
  ~Ttf();
  void init();
  void get_laser_data(uint16_t laser_buff[10], uint64_t laser_timestamps_buff[10]);
  void get_laser_data(uint16_t laser_buff[10]);
  void set_enabled_sensors(bool enabled[6]);
  void set_interval(uint8_t interval);
  bool enabled_sensors[6] = {true,true,false,true,false,true};
};