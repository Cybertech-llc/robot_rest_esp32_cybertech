#include "server.hpp"
#include "cJSON.h"
#include "config.hpp"
#include "motor.hpp"
#include <cstdint>
#include <cstring>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <stdio.h>
#include <string.h>

#include "imu.hpp"
#include "motor.hpp"
#include "ttf.hpp"

static const char *TAG = __FILENAME__;
char Server::chip_id[17] = "FFFFFFFFFFFFFFFF";

Server::Server() {}
Server::~Server() {}

extern Robot robot;
extern Imu imu;
extern Ttf ttf;

void Server::wifi_init_sta() {
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                      &Server::event_handler_stat, this,
                                      &instance_any_id);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                      &Server::event_handler_stat, this,
                                      &instance_got_ip);

  wifi_config_t wifi_config = {};
  strncpy((char *)wifi_config.sta.ssid, WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, WIFI_PASS,
          sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  esp_wifi_start();

  esp_wifi_set_ps(WIFI_PS_NONE);

  ESP_LOGI(TAG, "wifi_init_sta finished.");
}

void Server::event_handler(esp_event_base_t event_base, int32_t event_id,
                           void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    ESP_LOGI(TAG, "Trying to connect to the AP...");
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (retry_num < 3) {
      esp_wifi_connect();
      retry_num++;
      ESP_LOGI(TAG, "Retrying to connect to the AP...");
    } else {
      ESP_LOGI(TAG, "Failed to connect to the AP");
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    retry_num = 0;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    sprintf(ip_addr, IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, " <--- IP Address: %s ---> ", ip_addr);
  }
}

void Server::start_rest_server(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = NULL;
  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &motor);
    httpd_register_uri_handler(server, &sensor);
    httpd_register_uri_handler(server, &move);
    httpd_register_uri_handler(server, &sensor_config);
    ESP_LOGI(TAG, "REST server started.");
  } else
    ESP_LOGE(TAG, "Error starting server!");
}

esp_err_t Server::motor_put_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Connection", "close");
  char content[1000];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to receive request content");
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (json == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *json_id = cJSON_GetObjectItem(json, "id");
  if (!cJSON_IsString(json_id) || (json_id->valuestring == NULL)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing or invalid 'id' field");
    return ESP_FAIL;
  }

  if (strcmp(json_id->valuestring, chip_id) != 0) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid ID");
    return ESP_FAIL;
  }

  cJSON *json_left_motor = cJSON_GetObjectItem(json, "l");
  cJSON *json_right_motor = cJSON_GetObjectItem(json, "r");
  cJSON *json_left_time = cJSON_GetObjectItem(json, "l_time");
  cJSON *json_right_time = cJSON_GetObjectItem(json, "r_time");

  if (!cJSON_IsNumber(json_left_motor) || !cJSON_IsNumber(json_right_motor) ||
      !cJSON_IsNumber(json_left_time) || !cJSON_IsNumber(json_right_time)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid motor values");
    return ESP_FAIL;
  }

  int16_t l_value = json_left_motor->valueint;
  int16_t r_value = json_right_motor->valueint;

  if (l_value < -255 || l_value > 255 || r_value < -255 || r_value > 255) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Motor values out of range");
    return ESP_FAIL;
  }

  int32_t l_time_value = json_left_time->valueint;
  int32_t r_time_value = json_right_time->valueint;

  if (l_time_value < 0 || l_time_value > 60 * 60 * 1000 || r_time_value < 0 ||
      r_time_value > 60 * 60 * 1000) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Motor time is invalid");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Set Motor Left:%d, Right:%d, L_time:%d, R_time:%d.",
           (int)l_value, (int)r_value, (int)l_time_value, (int)r_time_value);
  robot.set_PWM(r_value, l_value, r_time_value, l_time_value);

  cJSON_Delete(json);

  httpd_resp_sendstr(req, "");
  return ESP_OK;
}

esp_err_t Server::move_put_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Connection", "close");
  char content[1000];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to receive request content");
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (json == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *json_id = cJSON_GetObjectItem(json, "id");
  if (!cJSON_IsString(json_id) || (json_id->valuestring == NULL)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing or invalid 'id' field");
    return ESP_FAIL;
  }

  if (strcmp(json_id->valuestring, chip_id) != 0) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid ID");
    return ESP_FAIL;
  }

  cJSON *json_directions = cJSON_GetObjectItem(json, "direction");
  cJSON *json_len = cJSON_GetObjectItem(json, "len");
  cJSON *json_speed = cJSON_GetObjectItem(json, "speed");

  if (!cJSON_IsString(json_directions)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid move cmd");
    return ESP_FAIL;
  }

  Robot::move_directions dir = Robot::invalid;

  if (strcmp(json_directions->valuestring, "forward") == 0)
    dir = Robot::forward;
  else if (strcmp(json_directions->valuestring, "backward") == 0)
    dir = Robot::backward;
  else if (strcmp(json_directions->valuestring, "left") == 0)
    dir = Robot::left;
  else if (strcmp(json_directions->valuestring, "right") == 0)
    dir = Robot::right;
  else if (strcmp(json_directions->valuestring, "break") == 0) {
    robot.break_flag = 1;
    ESP_LOGI(TAG, "Motor break.");
    cJSON_Delete(json);
    httpd_resp_sendstr(req, "");
    return ESP_OK;
  } else {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Motor directions name is invalid");
    return ESP_FAIL;
  }

  if (!cJSON_IsNumber(json_len) &&
      (dir == Robot::forward || dir == Robot::backward || dir == Robot::left ||
       dir == Robot::right)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid move cmd");
    return ESP_FAIL;
  }

  if (dir == Robot::forward || dir == Robot::backward) {
    if (json_len->valueint > 0 && json_len->valueint < 10000 &&
        json_speed->valueint > -1000 && json_speed->valueint <= 1000) {
      // motors.move_cmd(dir, json_len->valueint);
      robot.send_cmd_to_queue({dir, json_len->valueint, json_speed->valueint});
    } else {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Motor len is invalid");
      return ESP_FAIL;
    }
  }

  if (dir == Robot::left || dir == Robot::right) {
    if (json_len->valueint > 0 && json_len->valueint <= 360 &&
        json_speed->valueint > -1000 && json_speed->valueint <= 1000) {
      // motors.move_cmd(dir, json_len->valueint);
      robot.send_cmd_to_queue({dir, json_len->valueint, json_speed->valueint});
    } else {
      cJSON_Delete(json);
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Motor len is invalid");
      return ESP_FAIL;
    }
  }

  ESP_LOGI(TAG, "Set Motor deriction:%s, len:%d.", json_directions->valuestring,
           json_len->valueint);
  cJSON_Delete(json);

  httpd_resp_sendstr(req, "");
  return ESP_OK;
}

esp_err_t Server::sensor_post_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Connection", "close");
  char content[1000];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to receive request content");
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (json == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *json_id = cJSON_GetObjectItem(json, "id");
  if (!cJSON_IsString(json_id) || (json_id->valuestring == NULL)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing or invalid 'id' field");
    return ESP_FAIL;
  }

  if (strcmp(json_id->valuestring, chip_id) != 0) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid ID");
    return ESP_FAIL;
  }

  cJSON *json_type = cJSON_GetObjectItem(json, "type");
  const char *type = json_type->valuestring;
  cJSON *response_json = cJSON_CreateObject();

  if (strcmp(type, "all") == 0 || strcmp(type, "laser") == 0) {
    cJSON *laser_json = cJSON_CreateObject();
    uint16_t laser_values[10] = {};
    uint64_t laser_timestamps[10] = {};
#ifdef ENABLE_DISTANCE_SENSOR
    ttf.get_laser_data(laser_values, laser_timestamps);
#endif // ENABLE_DISTANCE_SENSOR
    for (int i = 0; i < 6; i++) {
      float value = (float)laser_values[i];
      if (!ttf.enabled_sensors[i]) {
        value = 65535;
      }
      cJSON_AddNumberToObject(laser_json, ttf.sensor_names[i], value);
      char timestamp_buff[100]; snprintf(timestamp_buff, sizeof(timestamp_buff), "%s_timestamp", ttf.sensor_names[i]);
      cJSON_AddNumberToObject(laser_json, timestamp_buff, (float)laser_timestamps[i]);
    }
    cJSON_AddItemToObject(response_json, "laser", laser_json);
  }

  if (strcmp(type, "all") == 0 || strcmp(type, "imu") == 0) {
    cJSON *imu_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(imu_json, "roll", (int16_t)imu.get_roll());
    cJSON_AddNumberToObject(imu_json, "pitch", (int16_t)imu.get_pitch());
    cJSON_AddNumberToObject(imu_json, "yaw", (int16_t)imu.get_yaw());
    cJSON_AddItemToObject(response_json, "imu", imu_json);
  }

  if (strcmp(type, "all") == 0 || strcmp(type, "motor") == 0) {
    cJSON *motor_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(motor_json, "left_pwm", robot.left_pwm);
    cJSON_AddNumberToObject(motor_json, "right_pwm", robot.right_pwm);
    cJSON_AddItemToObject(response_json, "motor", motor_json);
  }

  if (strcmp(type, "all") == 0 || strcmp(type, "encoders") == 0) {
    cJSON *motor_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(motor_json, "left_encoder_delta_sum", robot.left_encoder_delta_sum);
    cJSON_AddNumberToObject(motor_json, "right_encoder_delta_sum", robot.right_encoder_delta_sum);
    cJSON_AddItemToObject(response_json, "encoders", motor_json);

    robot.left_encoder_delta_sum = 0;
    robot.right_encoder_delta_sum = 0;
  }

  const char *response_str = cJSON_Print(response_json);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, response_str);

  free((void *)response_str);
  cJSON_Delete(response_json);
  cJSON_Delete(json);

  return ESP_OK;
}

esp_err_t Server::sensor_config_post_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Connection", "close");
  char content[1000];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to receive request content");
    return ESP_FAIL;
  }
  content[ret] = '\0';

  cJSON *json = cJSON_Parse(content);
  if (json == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *json_id = cJSON_GetObjectItem(json, "id");
  if (!cJSON_IsString(json_id) || (json_id->valuestring == NULL)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Missing or invalid 'id' field");
    return ESP_FAIL;
  }

  if (strcmp(json_id->valuestring, chip_id) != 0) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Invalid ID");
    return ESP_FAIL;
  }

  cJSON *json_interval = cJSON_GetObjectItem(json, "interval");
  if (!cJSON_IsNumber(json_interval) || json_interval->valueint < 20 || json_interval->valueint > 200) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid interval");
    return ESP_FAIL;
  }

  cJSON *json_enabled_sensors = cJSON_GetObjectItem(json, "enabled_sensors");
  if (!cJSON_IsArray(json_enabled_sensors)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Sensors selection must be an array");
    return ESP_FAIL;
  }

  bool enabled_sensors[6] = {0};

  cJSON *sensor_name;
  cJSON_ArrayForEach(sensor_name, json_enabled_sensors) {
    for(int i = 0;i<6;i++){
      if(strcmp(sensor_name->valuestring, ttf.sensor_names[i]) == 0){
        enabled_sensors[i] = true;
        break;
      }
      if(i == 5){
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid sensors selection");
        return ESP_FAIL;
      }
    }
  }

  ttf.set_interval(json_interval->valueint);
  ttf.set_enabled_sensors(enabled_sensors);

  cJSON_Delete(json);

  httpd_resp_sendstr(req, "");
  return ESP_OK;
}

void Server::init() {
  uint64_t unique_id;
  ESP_ERROR_CHECK(esp_flash_read_unique_chip_id(NULL, &unique_id));
  sprintf(chip_id, "%llX", unique_id);
  ESP_LOGI(TAG, " <--- Chip ID: %s ---> ", chip_id);

  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);
  esp_log_level_set("phy_init", ESP_LOG_WARN);
  esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
  esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
  esp_log_level_set("dhcpc", ESP_LOG_WARN);

  wifi_init_sta();
  start_rest_server();
}
