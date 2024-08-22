#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#include <cstdint>
#include <string>
#include <memory>
#include <map>
#include <list>

#include "energomera_iec_uart.h"
#include "energomera_iec_sensor.h"

namespace esphome {
namespace energomera_iec {

static const size_t MAX_IN_BUF_SIZE = 256;
static const size_t MAX_OUT_BUF_SIZE = 84;

const uint8_t VAL_NUM = 12;
using ValueRefsArray = std::array<char *, VAL_NUM>;

using SensorMap = std::multimap<std::string, EnergomeraIecSensorBase *>;
using SingleRequests = std::list<std::string>;

using FrameStopFunction = std::function<bool(uint8_t *buf, size_t size)>;
using ReadFunction = std::function<size_t()>;

class EnergomeraIecComponent : public PollingComponent, public uart::UARTDevice {
 public:
  EnergomeraIecComponent() = default;

  void setup() override;
  void dump_config() override;
  void loop() override;
  void update() override;
  float get_setup_priority() const override { return setup_priority::DATA; };

  void set_meter_address(const std::string &addr) { this->meter_address_ = addr; };
  void set_baud_rates(uint32_t baud_rate_handshake, uint32_t baud_rate) {
    this->baud_rate_handshake_ = baud_rate_handshake;
    this->baud_rate_ = baud_rate;
  };
  void set_receive_timeout_ms(uint32_t timeout) { this->receive_timeout_ms_ = timeout; };
  void set_delay_between_requests_ms(uint32_t delay) { this->delay_between_requests_ms_ = delay; };
  void set_flow_control_pin(GPIOPin *flow_control_pin) { this->flow_control_pin_ = flow_control_pin; };

  void register_sensor(EnergomeraIecSensorBase *sensor);
  void set_reboot_after_failure(uint16_t number_of_failures) { this->failures_before_reboot_ = number_of_failures; }

  void queue_single_read(const std::string &req);

 protected:
  std::string meter_address_{""};
  uint32_t receive_timeout_ms_{500};
  uint32_t delay_between_requests_ms_{50};

  GPIOPin *flow_control_pin_{nullptr};
  std::unique_ptr<EnergomeraIecUart> iuart_;

  SensorMap sensors_;
  SingleRequests single_requests_;

  sensor::Sensor *crc_errors_per_session_sensor_{};

  enum class State : uint8_t {
    NOT_INITIALIZED,
    IDLE,
    WAIT,
    WAITING_FOR_RESPONSE,
    OPEN_SESSION,
    OPEN_SESSION_GET_ID,
    SET_BAUD,
    ACK_START_GET_INFO,
    DATA_ENQ,
    DATA_RECV,
    DATA_NEXT,
    CLOSE_SESSION,
    PUBLISH,
    SINGLE_READ,
    SINGLE_READ_ACK,
  } state_{State::NOT_INITIALIZED};

  struct {
    uint32_t start_time{0};
    uint32_t delay_ms{0};
    State next_state{State::IDLE};
  } wait_;

  bool is_idling() const { return this->state_ == State::WAIT || this->state_ == State::IDLE; };

  void set_next_state_(State next_state) { state_ = next_state; };
  void set_next_state_delayed_(uint32_t ms, State next_state);

  void read_reply_and_go_next_state_(ReadFunction read_fn, State next_state, uint8_t retries, bool mission_critical,
                                     bool check_crc);
  struct {
    ReadFunction read_fn;
    State next_state;
    bool mission_critical;
    bool check_crc;
    uint8_t tries_max;
    uint8_t tries_counter;
    uint32_t err_crc;
    uint32_t err_invalid_frames;
  } reading_state_{nullptr, State::IDLE, false, false, 0, 0, 0, 0};
  size_t received_frame_size_{0};

  uint32_t baud_rate_handshake_{9600};
  uint32_t baud_rate_{9600};

  uint32_t last_rx_time_{0};

  struct {
    uint8_t in[MAX_IN_BUF_SIZE];
    size_t amount_in;
    uint8_t out[MAX_OUT_BUF_SIZE];
    size_t amount_out;
  } buffers_;

  void clear_rx_buffers_();

  void set_baud_rate_(uint32_t baud_rate);
  bool are_baud_rates_different_() const { return baud_rate_handshake_ != baud_rate_; }

  uint8_t calculate_crc_prog_frame_(uint8_t *data, size_t length, bool set_crc = false);
  bool check_crc_prog_frame_(uint8_t *data, size_t length);

  void prepare_frame_(const uint8_t *data, size_t length);
  void prepare_prog_frame_(const char *request);
  void prepare_non_session_prog_frame_(const char *request);

  void send_frame_(const uint8_t *data, size_t length);
  void send_frame_prepared_();

  size_t receive_frame_(FrameStopFunction stop_fn);
  size_t receive_frame_ascii_();
  size_t receive_prog_frame_(uint8_t start_byte, bool accept_ack_and_nack = false);

  inline void update_last_rx_time_() { this->last_rx_time_ = millis(); }
  bool check_wait_timeout_() { return millis() - wait_.start_time >= wait_.delay_ms; }
  bool check_rx_timeout_() { return millis() - this->last_rx_time_ >= receive_timeout_ms_; }

  char *extract_meter_id_(size_t frame_size);
  uint8_t get_values_from_brackets_(char *line, ValueRefsArray &vals);
  char *get_nth_value_from_csv_(char *line, uint8_t idx);
  bool set_sensor_value_(EnergomeraIecSensorBase *sensor, ValueRefsArray &vals);

  void report_failure(bool failure);
  void abort_mission_();

  const char *state_to_string(State state);
  void log_state_(State *next_state = nullptr);

  struct Stats {
    uint32_t connections_tried_{0};
    uint32_t crc_errors_{0};
    uint32_t crc_errors_recovered_{0};
    uint32_t invalid_frames_{0};
    uint8_t failures_{0};

    float crc_errors_per_session() const { return (float) crc_errors_ / connections_tried_; }
    void dump();
  } stats_;

  uint8_t failures_before_reboot_{0};
};

}  // namespace energomera_iec
}  // namespace esphome
