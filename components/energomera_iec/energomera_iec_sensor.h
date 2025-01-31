#pragma once

#include "esphome/components/sensor/sensor.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace energomera_iec {

static constexpr uint8_t MAX_TRIES = 10;

enum SensorType { SENSOR, TEXT_SENSOR };

class EnergomeraIecSensorBase {
 public:
  static const uint8_t MAX_REQUEST_SIZE = 15;

  virtual SensorType get_type() const = 0;
  virtual void publish() = 0;

  void set_request(const char *req) {
    // request can be:
    // 1. REQUEST
    // 2. REQUEST()
    // 3. REQUEST(PARAMETER)
    // But after checks in sensor.py it only can be 2 and 3
    request_ = req;

    char *p = strchr(req, '(');
    if (p != nullptr) {
      size_t len = p - req;
      function_.assign(req, len);
    }
  };
  const std::string &get_request() const { return request_; }
  const std::string &get_function() const { return function_; }

  void set_index(const uint8_t idx) { idx_ = idx; };
  uint8_t get_index() const { return idx_; };
  
  void set_sub_index(const uint8_t sub_idx) { sub_idx_ = sub_idx; };
  uint8_t get_sub_index() const { return sub_idx_; };

  void reset() {
    has_value_ = false;
    tries_ = 0;
  }

  bool has_value() { return has_value_; }

  void record_failure() {
    if (tries_ < MAX_TRIES) {
      tries_++;
    } else {
      has_value_ = false;
    }
  }
  bool is_failed() { return tries_ == MAX_TRIES; }

 protected:
  std::string request_;
  std::string function_;
  uint8_t idx_{1};
  uint8_t sub_idx_{0};
  bool has_value_;
  uint8_t tries_{0};
};

class EnergomeraIecSensor : public EnergomeraIecSensorBase, public sensor::Sensor {
 public:
  SensorType get_type() const override { return SENSOR; }
  void publish() override { publish_state(value_); }

  void set_value(float value) {
    value_ = value;
    has_value_ = true;
    tries_ = 0;
  }

 protected:
  float value_;
};

#ifdef USE_TEXT_SENSOR
class EnergomeraIecTextSensor : public EnergomeraIecSensorBase, public text_sensor::TextSensor {
 public:
  SensorType get_type() const override { return TEXT_SENSOR; }
  void publish() override { publish_state(value_); }

  void set_value(const char *value) {
    value_ = value;
    has_value_ = true;
    tries_ = 0;
  }

 protected:
  std::string value_;
};
#endif

}  // namespace energomera_iec
}  // namespace esphome
