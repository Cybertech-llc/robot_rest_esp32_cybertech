#include "ttf.hpp"
#include "bno055.hpp"
#include "config.hpp"
#include "vl53l0x.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>
#include <stdio.h>

static const char *TAG = __FILENAME__;

Ttf::~Ttf() {}
Ttf::Ttf() {}

i2c_master_dev_handle_t i2c_sw_handle;
i2c_master_dev_handle_t i2c_lv53l0x_handle;
bool interval_changed = false;
uint8_t new_interval = 200;

void Ttf::set_enabled_sensors(bool enabled[6]){
  for(int i = 0;i<6;i++){
    if (i == 2 || i == 4) continue;
    enabled_sensors[i] = enabled[i];
  }
}

void Ttf::set_interval(uint8_t interval){
  new_interval = interval;
  interval_changed = true;
}

void Ttf::get_laser_data(uint16_t laser_buff[10], uint64_t laser_timestamps_buff[10]) {
  for (size_t i = 0; i < 10; i++) {
    laser_buff[i] = laser_values[i];
    laser_timestamps_buff[i] = laser_timestamps[i];
  }
}

esp_err_t Ttf::i2c_switch_to_ch(uint8_t ch) {
  uint8_t data = 1 << ch;
  esp_err_t ret = i2c_master_transmit(i2c_sw_handle, &data, 1, 100);
  // vTaskDelay(10 / portTICK_PERIOD_MS);
  return ret;
}

void Ttf::init() {
  i2c_master_bus_config_t i2c_bus_0_config = {};
  i2c_bus_0_config.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c_bus_0_config.i2c_port = I2C_NUM_0;
  i2c_bus_0_config.scl_io_num = LASER_SCL;
  i2c_bus_0_config.sda_io_num = LASER_SDA;
  i2c_bus_0_config.glitch_ignore_cnt = 7;
  i2c_bus_0_config.flags.enable_internal_pullup = 1;
  i2c_master_bus_handle_t bus_0_handle;
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_0_config, &bus_0_handle));

#ifdef I2C_SCAN_ENABLE
  ESP_LOGI(TAG, "I2C scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (i2c_master_probe(bus_handle, addr, 100) == ESP_OK)
      ESP_LOGI(TAG, " ---> find device: 0x%02X <---", addr);
  }
#endif // I2C_SCAN_ENABLE

  i2c_device_config_t i2c_switch_config = {};
  i2c_switch_config.device_address = SWITCH_DEFAULT_ADDR;
  i2c_switch_config.scl_speed_hz = 100000;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_0_handle, &i2c_switch_config,
                                            &i2c_sw_handle));

  i2c_switch_to_ch(0);

  i2c_device_config_t i2c_lv53l0x_config = {};
  i2c_lv53l0x_config.device_address = VL53L0X_DEFAULT_ADDR;
  i2c_lv53l0x_config.scl_speed_hz = 100000;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_0_handle, &i2c_lv53l0x_config,
                                            &i2c_lv53l0x_handle));

  xTaskCreate(&task_wrapper, "lv53l0x", 1024 * 4, this, 10, NULL);
}

void Ttf::task() {

  for (uint8_t i = 0; i < 6; i++) {
    if (i == 2 || i == 4) continue;
    i2c_switch_to_ch(i);

    sensor[i].setTimeout(500);
    if (!sensor[i].init())
      ESP_LOGE(TAG, "Failed to init lv53l0x sensor - N:%d!", i);
    sensor[i].setMeasurementTimingBudget(200000);
  }

  while (1) {
    for (uint8_t i = 0; i < 6; i++) {
      if (i == 2 || i == 4) continue;
      if (!enabled_sensors[i]) continue;

      i2c_switch_to_ch(i);

      uint16_t buff = sensor[i].readRangeSingleMillimeters();
      laser_values[i] = buff;

      laser_timestamps[i] = esp_timer_get_time();

      if (sensor[i].timeoutOccurred())
        ESP_LOGW(TAG, "Sensor %d, timeout...", i);
    }

    if(interval_changed){
      for (uint8_t i = 0; i < 6; i++) {
        if (i == 2 || i == 4) continue;
        i2c_switch_to_ch(i);
        sensor[i].setMeasurementTimingBudget(new_interval*1000);
      }
    }

    // ESP_LOGI(TAG, "Sensor -> 1:%d - 2:%d - 3:%d - 4:%d - 5:%d - 6:%d.",
    //          laser_values[0], laser_values[1], laser_values[2],
    //          laser_values[3], laser_values[4], laser_values[5]);

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
