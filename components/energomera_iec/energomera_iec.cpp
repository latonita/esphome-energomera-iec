#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/application.h"
#include "esphome/core/time.h"
#include "energomera_iec.h"
#include <sstream>

namespace esphome {
namespace energomera_iec {

static const char *TAG0 = "energomera_iec_";
#define TAG (this->tag_.c_str())

static constexpr uint8_t SOH = 0x01;
static constexpr uint8_t STX = 0x02;
static constexpr uint8_t ETX = 0x03;
static constexpr uint8_t EOT = 0x04;
static constexpr uint8_t ENQ = 0x05;
static constexpr uint8_t ACK = 0x06;
static constexpr uint8_t CR = 0x0D;
static constexpr uint8_t LF = 0x0A;
static constexpr uint8_t NAK = 0x15;

static const uint8_t CMD_ACK_SET_BAUD_AND_MODE[] = {ACK, '0', '5', '1', CR, LF};
static const uint8_t CMD_CLOSE_SESSION[] = {SOH, 0x42, 0x30, ETX, 0x75};

static constexpr uint8_t BOOT_WAIT_S = 10;

static char empty_str[] = "";

static char format_hex_char(uint8_t v) { return v >= 10 ? 'A' + (v - 10) : '0' + v; }

static std::string format_frame_pretty(const uint8_t *data, size_t length) {
  if (length == 0)
    return "";
  std::string ret;
  ret.resize(3 * length - 1);
  std::ostringstream ss(ret);

  for (size_t i = 0; i < length; i++) {
    switch (data[i]) {
      case 0x00:
        ss << "<NUL>";
        break;
      case 0x01:
        ss << "<SOH>";
        break;
      case 0x02:
        ss << "<STX>";
        break;
      case 0x03:
        ss << "<ETX>";
        break;
      case 0x04:
        ss << "<EOT>";
        break;
      case 0x05:
        ss << "<ENQ>";
        break;
      case 0x06:
        ss << "<ACK>";
        break;
      case 0x0d:
        ss << "<CR>";
        break;
      case 0x0a:
        ss << "<LF>";
        break;
      case 0x15:
        ss << "<NAK>";
        break;
      case 0x20:
        ss << "<SP>";
        break;
      default:
        if (data[i] <= 0x20 || data[i] >= 0x7f) {
          ss << "<" << format_hex_char((data[i] & 0xF0) >> 4) << format_hex_char(data[i] & 0x0F) << ">";
        } else {
          ss << (char) data[i];
        }
        break;
    }
  }
  if (length > 4)
    ss << " (" << length << ")";
  return ss.str();
}

uint8_t baud_rate_to_byte(uint32_t baud) {
  constexpr uint16_t BAUD_BASE = 300;
  constexpr uint8_t BAUD_MULT_MAX = 6;

  uint8_t idx = 0;  // 300
  for (size_t i = 0; i <= BAUD_MULT_MAX; i++) {
    if (baud == BAUD_BASE * (1 << i)) {
      idx = i;
      break;
    }
  }
  return idx + '0';
}

void EnergomeraIecComponent::set_baud_rate_(uint32_t baud_rate) {
  ESP_LOGV(TAG, "Setting baud rate %u bps", baud_rate);
  iuart_->update_baudrate(baud_rate);
}

void EnergomeraIecComponent::setup() {
  ESP_LOGD(TAG, "setup");
#ifdef USE_ESP32_FRAMEWORK_ARDUINO
  iuart_ = make_unique<EnergomeraIecUart>(*static_cast<uart::ESP32ArduinoUARTComponent *>(this->parent_));
#endif

#ifdef USE_ESP_IDF
  iuart_ = make_unique<EnergomeraIecUart>(*static_cast<uart::IDFUARTComponent *>(this->parent_));
#endif

#if USE_ESP8266
  iuart_ = make_unique<EnergomeraIecUart>(*static_cast<uart::ESP8266UartComponent *>(this->parent_));
#endif
  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
  }
  this->set_baud_rate_(this->baud_rate_handshake_);
  this->set_timeout(BOOT_WAIT_S * 1000, [this]() {
    ESP_LOGD(TAG, "Boot timeout, component is ready to use");
    this->clear_rx_buffers_();
    this->set_next_state_(State::IDLE);
  });
}

void EnergomeraIecComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Energomera IEC: %p", this);

  LOG_UPDATE_INTERVAL(this);
  LOG_PIN("  Flow Control Pin: ", this->flow_control_pin_);
  ESP_LOGCONFIG(TAG, "  Receive Timeout: %ums", this->receive_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Supported Meter Types: CE102M/CE301/CE303/...");
  ESP_LOGCONFIG(TAG, "  Sensors:");
  for (const auto &sensors : sensors_) {
    auto &s = sensors.second;
    ESP_LOGCONFIG(TAG, "    REQUEST: %s", s->get_request().c_str());
  }
}

void EnergomeraIecComponent::register_sensor(EnergomeraIecSensorBase *sensor) {
  this->sensors_.insert({sensor->get_request(), sensor});
}

void EnergomeraIecComponent::abort_mission_() {
  // try close connection ?
  ESP_LOGE(TAG, "Abort mission. Closing session");
  this->send_frame_(CMD_CLOSE_SESSION, sizeof(CMD_CLOSE_SESSION));
  this->unlock_uart_session_();
  this->set_next_state_(State::IDLE);
  this->report_failure(true);
}

void EnergomeraIecComponent::report_failure(bool failure) {
  if (!failure) {
    this->stats_.failures_ = 0;
    return;
  }

  this->stats_.failures_++;
  if (this->failures_before_reboot_ > 0 && this->stats_.failures_ > this->failures_before_reboot_) {
    ESP_LOGE(TAG, "Too many failures in a row. Let's try rebooting device.");
    delay(100);
    App.safe_reboot();
  }
}

void EnergomeraIecComponent::loop() {
  if (!this->is_ready() || this->state_ == State::NOT_INITIALIZED)
    return;

  ValueRefsArray vals;                                  // values from brackets, refs to this->buffers_.in
  char *in_param_ptr = (char *) &this->buffers_.in[1];  // ref to second byte, first is STX/SOH in R1 requests

  switch (this->state_) {
    case State::IDLE: {
      this->update_last_rx_time_();
      // auto request = this->single_requests_.front();

      // if (this->single_requests_.empty())
      //   break;

      // this->single_requests_.pop_front();
      // ESP_LOGD(TAG, "Performing single request '%s'", request.c_str());
      // this->prepare_non_session_prog_frame_(request.c_str());
      // this->send_frame_prepared_();
      // auto read_fn = [this]() { return this->receive_prog_frame_(STX, true); };
      // this->read_reply_and_go_next_state_(read_fn, State::SINGLE_READ_ACK, 3, false, true);

    } break;

    case State::TRY_LOCK_BUS: {
      this->log_state_();
      if (this->try_lock_uart_session_()) {
        this->set_next_state_(State::OPEN_SESSION);
      } else {
        ESP_LOGV(TAG, "UART Bus is busy, waiting ...");
        this->set_next_state_delayed_(1000, State::TRY_LOCK_BUS);
      }
    } break;

    case State::WAIT:
      if (this->check_wait_timeout_()) {
        this->set_next_state_(this->wait_.next_state);
        this->update_last_rx_time_();
      }
      break;

    case State::WAITING_FOR_RESPONSE: {
      this->log_state_(&reading_state_.next_state);
      received_frame_size_ = reading_state_.read_fn();

      bool crc_is_ok = true;
      if (reading_state_.check_crc && received_frame_size_ > 0) {
        crc_is_ok = check_crc_prog_frame_(this->buffers_.in, received_frame_size_);
      }

      // happy path first
      if (received_frame_size_ > 0 && crc_is_ok) {
        this->set_next_state_(reading_state_.next_state);
        this->update_last_rx_time_();
        this->stats_.crc_errors_ += reading_state_.err_crc;
        this->stats_.crc_errors_recovered_ += reading_state_.err_crc;
        this->stats_.invalid_frames_ += reading_state_.err_invalid_frames;
        return;
      }

      // half-happy path
      // if not timed out yet, wait for data to come a little more
      if (crc_is_ok && !this->check_rx_timeout_()) {
        return;
      }

      if (received_frame_size_ == 0) {
        this->reading_state_.err_invalid_frames++;
        ESP_LOGW(TAG, "RX timeout.");
      } else if (!crc_is_ok) {
        this->reading_state_.err_crc++;
        ESP_LOGW(TAG, "Frame received, but CRC failed.");
      } else {
        this->reading_state_.err_invalid_frames++;
        ESP_LOGW(TAG, "Frame corrupted.");
      }

      // if we are here, we have a timeout and no data
      // it means we have a failure
      // - either no reply from the meter at all
      // - or corrupted data and id doesn't trigger stop function
      if (this->buffers_.amount_in > 0) {
        // most likely its CRC error in STX/SOH/ETX. unclear.
        this->stats_.crc_errors_++;
        ESP_LOGV(TAG, "RX: %s", format_frame_pretty(this->buffers_.in, this->buffers_.amount_in).c_str());
        ESP_LOGVV(TAG, "RX: %s", format_hex_pretty(this->buffers_.in, this->buffers_.amount_in).c_str());
      }
      this->clear_rx_buffers_();

      if (reading_state_.mission_critical) {
        this->stats_.crc_errors_ += reading_state_.err_crc;
        this->stats_.invalid_frames_ += reading_state_.err_invalid_frames;
        this->abort_mission_();
        return;
      }

      if (reading_state_.tries_counter < reading_state_.tries_max) {
        reading_state_.tries_counter++;
        ESP_LOGW(TAG, "Retrying [%d/%d]...", reading_state_.tries_counter, reading_state_.tries_max);
        this->send_frame_prepared_();
        this->update_last_rx_time_();
        return;
      }
      received_frame_size_ = 0;
      // failure, advancing to next state with no data received (frame_size = 0)
      this->stats_.crc_errors_ += reading_state_.err_crc;
      this->stats_.invalid_frames_ += reading_state_.err_invalid_frames;
      this->set_next_state_(reading_state_.next_state);
    } break;

    case State::OPEN_SESSION: {
      this->stats_.connections_tried_++;
      this->loop_state_.session_started_ms = millis();
      this->log_state_();

      this->clear_rx_buffers_();
      if (this->are_baud_rates_different_()) {
        this->set_baud_rate_(this->baud_rate_handshake_);
        delay(5);
      }

      uint8_t open_cmd[32]{0};
      uint8_t open_cmd_len = snprintf((char *) open_cmd, 32, "/?%s!\r\n", this->meter_address_.c_str());
      this->loop_state_.request_iter = this->sensors_.begin();
      this->send_frame_(open_cmd, open_cmd_len);
      this->set_next_state_(State::OPEN_SESSION_GET_ID);
      auto read_fn = [this]() { return this->receive_frame_ascii_(); };
      // mission crit, no crc
      this->read_reply_and_go_next_state_(read_fn, State::OPEN_SESSION_GET_ID, 0, true, false);

    } break;

    case State::OPEN_SESSION_GET_ID:
      this->log_state_();

      if (received_frame_size_) {
        char *id = this->extract_meter_id_(received_frame_size_);
        if (id == nullptr) {
          ESP_LOGE(TAG, "Invalid meter identification frame");
          this->stats_.invalid_frames_++;
          this->abort_mission_();
          return;
        }

        this->update_last_rx_time_();
        if (this->are_baud_rates_different_()) {
          this->prepare_frame_(CMD_ACK_SET_BAUD_AND_MODE, sizeof(CMD_ACK_SET_BAUD_AND_MODE));

          this->buffers_.out[2] = baud_rate_to_byte(this->baud_rate_);  // set baud rate
          this->send_frame_prepared_();
          this->flush();
          this->set_next_state_delayed_(250, State::SET_BAUD);

        } else {
          this->send_frame_(CMD_ACK_SET_BAUD_AND_MODE, sizeof(CMD_ACK_SET_BAUD_AND_MODE));
          auto read_fn = [this]() { return this->receive_prog_frame_(SOH); };
          this->read_reply_and_go_next_state_(read_fn, State::ACK_START_GET_INFO, 3, true, true);
        }
      }
      break;

    case State::SET_BAUD:
      this->log_state_();
      this->update_last_rx_time_();
      this->set_baud_rate_(this->baud_rate_);
      this->set_next_state_delayed_(150, State::ACK_START_GET_INFO);
      break;

    case State::ACK_START_GET_INFO:
      this->log_state_();

      if (received_frame_size_ == 0) {
        ESP_LOGE(TAG, "No response from meter.");
        this->stats_.invalid_frames_++;
        this->abort_mission_();
        return;
      }

      if (!get_values_from_brackets_(in_param_ptr, vals)) {
        ESP_LOGE(TAG, "Invalid frame format: '%s'", in_param_ptr);
        this->stats_.invalid_frames_++;
        this->abort_mission_();
        return;
      }

      ESP_LOGD(TAG, "Meter address: %s", vals[0]);

      // did we have a time correction request?
      if (this->time_to_set_ != 0) {
        this->set_next_state_(State::GET_DATE);
      } else {
        this->set_next_state_(State::DATA_ENQ);
      }
      break;

    case State::GET_DATE: {
      this->log_state_();
      this->update_last_rx_time_();

      this->meter_datetime_str_[0] = '\0';  // reset string

      this->set_next_state_(State::GET_TIME);
      // happy to use group reads, but GROUP readings work differently on different meters and not part of the standard
      // protocol
      //      this->prepare_prog_frame_("GROUP(DATE_()TIME_())");
      this->prepare_prog_frame_("DATE_()");
      this->send_frame_prepared_();
      auto read_fn = [this]() { return this->receive_prog_frame_(STX); };
      this->read_reply_and_go_next_state_(read_fn, State::GET_TIME, 3, false, true);
    } break;

    case State::GET_TIME: {
      this->log_state_();
      this->update_last_rx_time_();
      // We receive either of the following:
      //       0         1         2         3
      //       01234567890123456789012345678901234567890
      //  <STX>DATE_(5.30.05.25)<CR><LF><ETX><ACK> (22)     // ce207
      //  <STX>DATE_(05.30.05.25)<CR><LF><ETX><ACK> (23)    // ce301, etc.
      if (received_frame_size_ != 22 && received_frame_size_ != 23) {
        // no data or something wrong. error or malformed response
        ESP_LOGE(TAG, "No response or wrong response from meter. Can't get date, skipping sync.");
        this->stats_.invalid_frames_++;
        this->set_next_state_(State::DATA_ENQ);
        return;
      }
      // 2020-08-25 05:30:00
      // copy date parts
      // assume it is year 20xx.
      size_t d = 23 - received_frame_size_;  // 23 - 22 = 1, 23 - 23 = 0
      this->meter_datetime_str_[0] = '2';
      this->meter_datetime_str_[1] = '0';  // year 20xx
      this->meter_datetime_str_[2] = in_param_ptr[d + 15];
      this->meter_datetime_str_[3] = in_param_ptr[d + 16];  // year 20xx
      this->meter_datetime_str_[4] = '-';                   // separator
      this->meter_datetime_str_[5] = in_param_ptr[d + 12];  // month
      this->meter_datetime_str_[6] = in_param_ptr[d + 13];  // month
      this->meter_datetime_str_[7] = '-';                   // separator
      this->meter_datetime_str_[8] = in_param_ptr[d + 9];   // day
      this->meter_datetime_str_[9] = in_param_ptr[d + 10];  // day
      this->meter_datetime_str_[10] = ' ';                  // separator
      this->meter_datetime_str_[11] = '\0';

      this->set_next_state_(State::CORRECT_TIME);
      this->prepare_prog_frame_("TIME_()");
      this->send_frame_prepared_();
      auto read_fn = [this]() { return this->receive_prog_frame_(STX); };
      this->read_reply_and_go_next_state_(read_fn, State::CORRECT_TIME, 3, false, true);

    } break;

    case State::CORRECT_TIME: {
      this->log_state_();

      // do not care, not real point in this
      // auto time_received_delay_ms = millis() - this->last_rx_time_;

      this->update_last_rx_time_();
      this->set_next_state_(State::DATA_ENQ);

      // here we expect to receive time in format HH:MM:SS
      //      0         1         2         3
      //      01234567890123456789012345678901234567890
      // <STX>TIME_(23:40:10)<CR><LF><ETX><17> (20)

      if (received_frame_size_ != 20) {
        // no data or something wrong. error or malformed response
        ESP_LOGE(TAG, "No response or wrong response from meter. Can't get time, skipping sync.");
        this->stats_.invalid_frames_++;
        return;
      }
      memcpy(this->meter_datetime_str_ + 11, in_param_ptr + 6, 8);  // copy HH:MM:SS
      this->meter_datetime_str_[19] = '\0';                         // null-terminate the string

      ESPTime meter_datetime;
      meter_datetime.day_of_week = 1;
      meter_datetime.day_of_year = 1;
      int num = 0;
      {
        uint16_t year;
        uint8_t month;
        uint8_t day;
        uint8_t hour;
        uint8_t minute;
        uint8_t second;
        num = sscanf(this->meter_datetime_str_, "%04hu-%02hhu-%02hhu %02hhu:%02hhu:%02hhu",
                     &year,    // NOLINT
                     &month,   // NOLINT
                     &day,     // NOLINT
                     &hour,    // NOLINT
                     &minute,  // NOLINT
                     &second);
        meter_datetime.year = year;
        meter_datetime.month = month;
        meter_datetime.day_of_month = day;
        meter_datetime.hour = hour;
        meter_datetime.minute = minute;
        meter_datetime.second = second;
        meter_datetime.day_of_week = 1;
        meter_datetime.day_of_year = 1;  // not used, but set to avoid uninitialized value
      }
      meter_datetime.recalc_timestamp_local();
      if (num != 6) {
        ESP_LOGE(TAG, "Invalid time received from meter: %s %d", this->meter_datetime_str_, num);
        this->stats_.invalid_frames_++;
        return;
      }

      if (!meter_datetime.is_valid()) {
        ESP_LOGE(TAG, "Invalid time received from meter: %s", this->meter_datetime_str_);
        this->stats_.invalid_frames_++;
        return;
      }

      // check if time is 2 mins before or after midnight, then skip correction, wait for next data request
      // just to make sure we have proper date/time
      if ((meter_datetime.hour == 0 && meter_datetime.minute < 2) ||
          (meter_datetime.hour == 23 && meter_datetime.minute > 58)) {
        ESP_LOGD(TAG, "Time is too close to midnight, skipping correction.");
        return;
      }

      auto now_ms = millis();
#ifdef USE_TIME
      if (this->time_source_ != nullptr) {
        auto tm = this->time_source_->now();
        if (!tm.is_valid()) {
          ESP_LOGE(TAG, "Time sync requested, but time provider is not yet ready");
          return;
        }
        this->time_to_set_ = this->time_source_->now().timestamp;
        this->time_to_set_requested_at_ms_ = now_ms;
      }
#endif
      // if we are here, we have a valid time
      // find what is real time now
      uint32_t ms_since_asked = now_ms - this->time_to_set_requested_at_ms_;

      meter_datetime.recalc_timestamp_local();
      int32_t correction_seconds = (this->time_to_set_ + ms_since_asked / 1000) - meter_datetime.timestamp;

      this->time_to_set_ = 0;
      this->time_to_set_requested_at_ms_ = 0;

      if (correction_seconds > -2 && correction_seconds < 2) {
        ESP_LOGD(TAG, "No time correction needed (less than 2 seconds)");
        return;
      }

      ESP_LOGD(TAG, "Time correction needed: %d seconds", correction_seconds);

      constexpr int32_t SECONDS_IN_24H = 24 * 3600;

      // if correction is more than 24 hours,
      // it is serious meter failure, meter shall be replaced or time is completely wrong
      if (correction_seconds > SECONDS_IN_24H || correction_seconds < -SECONDS_IN_24H) {
        ESP_LOGE(TAG, "Time correction is more than 24 hours, meter is broken or time is completely wrong.");
        return;
      }

      if (correction_seconds > 29) {
        correction_seconds = 29;
      } else if (correction_seconds < -29) {
        correction_seconds = -29;
      }
      ESP_LOGD(TAG, "Setting time correction within +/- 29 seconds: %d", correction_seconds);

      char set_time_cmd[16]{0};
      size_t len = snprintf(set_time_cmd, sizeof(set_time_cmd), "CTIME(%d)", correction_seconds);
      this->prepare_prog_frame_(set_time_cmd, true);
      this->send_frame_prepared_();
      auto read_fn = [this]() { return this->receive_frame_ack_nack_(); };
      this->read_reply_and_go_next_state_(read_fn, State::RECV_CORRECTION_RESULT, 0, false, false);
    } break;

    case State::RECV_CORRECTION_RESULT: {
      this->log_state_();
      this->set_next_state_(State::DATA_ENQ);

      if (received_frame_size_ == 0) {
        ESP_LOGW(TAG, "No response from meter after time correction request. Not supported?");
        this->stats_.invalid_frames_++;
        return;
      }
      char reply = this->buffers_.in[0];
      if (reply == ACK) {
        ESP_LOGD(TAG, "Time correction acknowledged");
      } else if (reply == NAK) {
        ESP_LOGD(TAG, "Time correction declined");
      } else {
        ESP_LOGD(TAG, "Time correction failed");
      }

    } break;

    case State::DATA_ENQ:
      this->log_state_();
      if (this->loop_state_.request_iter == this->sensors_.end()) {
        ESP_LOGD(TAG, "All requests done");
        this->set_next_state_(State::CLOSE_SESSION);
        break;
      } else {
        auto req = this->loop_state_.request_iter->first;
        ESP_LOGD(TAG, "Requesting data for '%s'", req.c_str());
        this->prepare_prog_frame_(req.c_str());
        this->send_frame_prepared_();
        auto read_fn = [this]() { return this->receive_prog_frame_(STX); };
        this->read_reply_and_go_next_state_(read_fn, State::DATA_RECV, 3, false, true);
      }
      break;

    case State::DATA_RECV: {
      this->log_state_();
      this->set_next_state_(State::DATA_NEXT);

      if (received_frame_size_ == 0) {
        ESP_LOGD(TAG, "Response not received or corrupted. Next.");
        this->update_last_rx_time_();
        this->clear_rx_buffers_();
        return;
      }

      auto req = this->loop_state_.request_iter->first;

      uint8_t brackets_found = get_values_from_brackets_(in_param_ptr, vals);
      if (!brackets_found) {
        ESP_LOGE(TAG, "Invalid frame format: '%s'", in_param_ptr);
        this->stats_.invalid_frames_++;
        return;
      }

      ESP_LOGD(TAG,
               "Received name: '%s', values: %d, idx: 1(%s), 2(%s), 3(%s), 4(%s), 5(%s), 6(%s), 7(%s), 8(%s), 9(%s), "
               "10(%s), 11(%s), 12(%s)",
               in_param_ptr, brackets_found, vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6], vals[7],
               vals[8], vals[9], vals[10], vals[11]);

      if (in_param_ptr[0] == '\0') {
        if (vals[0][0] == 'E' && vals[0][1] == 'R' && vals[0][2] == 'R') {
          ESP_LOGE(TAG, "Request '%s' either not supported or malformed. Error code %s", in_param_ptr, vals[0]);
        } else {
          ESP_LOGE(TAG, "Request '%s' either not supported or malformed.", in_param_ptr);
        }
        return;
      }

      if (this->loop_state_.request_iter->second->get_function() != in_param_ptr) {
        ESP_LOGE(TAG, "Returned data name mismatch. Skipping frame");
        return;
      }

      auto range = sensors_.equal_range(req);
      for (auto it = range.first; it != range.second; ++it) {
        if (!it->second->is_failed())
          set_sensor_value_(it->second, vals);
      }
    } break;

    case State::DATA_NEXT:
      this->log_state_();
      this->loop_state_.request_iter = this->sensors_.upper_bound(this->loop_state_.request_iter->first);
      if (this->loop_state_.request_iter != this->sensors_.end()) {
        this->set_next_state_delayed_(this->delay_between_requests_ms_, State::DATA_ENQ);
      } else {
        this->set_next_state_delayed_(this->delay_between_requests_ms_, State::CLOSE_SESSION);
      }
      break;

    case State::CLOSE_SESSION:
      this->log_state_();
      ESP_LOGD(TAG, "Closing session");
      this->send_frame_(CMD_CLOSE_SESSION, sizeof(CMD_CLOSE_SESSION));
      this->set_next_state_(State::PUBLISH);
      ESP_LOGD(TAG, "Total connection time: %u ms", millis() - this->loop_state_.session_started_ms);
      this->loop_state_.sensor_iter = this->sensors_.begin();
      break;

    case State::PUBLISH:
      this->log_state_();
      ESP_LOGD(TAG, "Publishing data");
      this->update_last_rx_time_();

      if (this->loop_state_.sensor_iter != this->sensors_.end()) {
        this->loop_state_.sensor_iter->second->publish();
        this->loop_state_.sensor_iter++;
      } else {
        this->stats_dump_();
        if (this->crc_errors_per_session_sensor_ != nullptr) {
          this->crc_errors_per_session_sensor_->publish_state(this->stats_.crc_errors_per_session());
        }
        this->report_failure(false);
        this->unlock_uart_session_();
        this->set_next_state_(State::IDLE);
      }
      break;

    case State::SINGLE_READ_ACK: {
      this->log_state_();
      if (received_frame_size_) {
        ESP_LOGD(TAG, "Single read frame received");
      } else {
        ESP_LOGE(TAG, "Failed to make single read call");
      }
      this->set_next_state_(State::IDLE);
    } break;

    default:
      break;
  }
}

void EnergomeraIecComponent::update() {
  if (this->state_ != State::IDLE) {
    ESP_LOGD(TAG, "Starting data collection impossible - component not ready");
    return;
  }
  ESP_LOGD(TAG, "Starting data collection");
  this->set_next_state_(State::TRY_LOCK_BUS);
}

void EnergomeraIecComponent::queue_single_read(const std::string &request) {
  ESP_LOGD(TAG, "Queueing single read for '%s'", request.c_str());
  this->single_requests_.push_back(request);
}

bool char2float(const char *str, float &value) {
  char *end;
  value = strtof(str, &end);
  return *end == '\0';
}

#ifdef USE_TIME
void EnergomeraIecComponent::sync_device_time() {
  if (this->time_source_ == nullptr) {
    ESP_LOGE(TAG, "Time source not set. Time can not be synced.");
    return;
  }
  auto time = this->time_source_->now();
  if (!time.is_valid()) {
    ESP_LOGW(TAG, "Time is not yet valid.  Time can not be synced.");
    return;
  }
  this->set_device_time(1);
}
#endif

void EnergomeraIecComponent::set_device_time(uint32_t timestamp) {
  ESP_LOGD(TAG, "set_device_time: %u", timestamp);
  if (!timestamp)
    return;
  this->time_to_set_ = timestamp;
  this->time_to_set_requested_at_ms_ = millis();
}

bool EnergomeraIecComponent::set_sensor_value_(EnergomeraIecSensorBase *sensor, ValueRefsArray &vals) {
  auto type = sensor->get_type();
  bool ret = true;

  uint8_t idx = sensor->get_index() - 1;
  if (idx >= VAL_NUM) {
    ESP_LOGE(TAG, "Invalid sensor index %u", idx);
    return false;
  }
  char str_buffer[128] = {'\0'};
  strncpy(str_buffer, vals[idx], 128);

  char *str = str_buffer;
  uint8_t sub_idx = sensor->get_sub_index();
  if (sub_idx == 0) {
    ESP_LOGD(TAG, "Setting value for sensor '%s', idx = %d to '%s'", sensor->get_request().c_str(), idx + 1, str);
  } else {
    ESP_LOGD(TAG, "Extracting value for sensor '%s', idx = %d, sub_idx = %d from '%s'", sensor->get_request().c_str(),
             idx + 1, sub_idx, str);
    str = this->get_nth_value_from_csv_(str, sub_idx);
    if (str == nullptr) {
      ESP_LOGE(TAG,
               "Cannot extract sensor value by sub-index %d. Is data comma-separated? Also note that sub-index starts "
               "from 1",
               sub_idx);
      str_buffer[0] = '\0';
      str = str_buffer;
    }
    ESP_LOGD(TAG, "Setting value using sub-index = %d, extracted sensor value is '%s'", sub_idx, str);
  }

  if (type == SensorType::SENSOR) {
    float f = 0;
    ret = str && str[0] && char2float(str, f);
    if (ret) {
      static_cast<EnergomeraIecSensor *>(sensor)->set_value(f);
    } else {
      ESP_LOGE(TAG, "Cannot convert incoming data to a number. Consider using a text sensor. Invalid data: '%s'", str);
    }
  } else {
#ifdef USE_TEXT_SENSOR
    static_cast<EnergomeraIecTextSensor *>(sensor)->set_value(str);
#endif
  }
  return ret;
}

uint8_t EnergomeraIecComponent::calculate_crc_prog_frame_(uint8_t *data, size_t length, bool set_crc) {
  uint8_t crc = 0;
  if (length < 2) {
    return 0;
  }
  for (size_t i = 1; i < length - 1; i++) {
    crc = (crc + data[i]) & 0x7f;
  }
  if (set_crc) {
    data[length - 1] = crc;
  }
  return crc;
}

bool EnergomeraIecComponent::check_crc_prog_frame_(uint8_t *data, size_t length) {
  uint8_t crc = this->calculate_crc_prog_frame_(data, length);
  return crc == data[length - 1];
}

void EnergomeraIecComponent::set_next_state_delayed_(uint32_t ms, State next_state) {
  if (ms == 0) {
    set_next_state_(next_state);
  } else {
    ESP_LOGV(TAG, "Short delay for %u ms", ms);
    set_next_state_(State::WAIT);
    wait_.start_time = millis();
    wait_.delay_ms = ms;
    wait_.next_state = next_state;
  }
}

void EnergomeraIecComponent::read_reply_and_go_next_state_(ReadFunction read_fn, State next_state, uint8_t retries,
                                                           bool mission_critical, bool check_crc) {
  reading_state_ = {};
  reading_state_.read_fn = read_fn;
  reading_state_.mission_critical = mission_critical;
  reading_state_.tries_max = retries;
  reading_state_.tries_counter = 0;
  reading_state_.check_crc = check_crc;
  reading_state_.next_state = next_state;
  received_frame_size_ = 0;

  set_next_state_(State::WAITING_FOR_RESPONSE);
}

void EnergomeraIecComponent::prepare_prog_frame_(const char *request, bool write) {
  // we assume request has format "XXXX(params)"
  // we assume it always has brackets
  this->buffers_.amount_out = snprintf((char *) this->buffers_.out, MAX_OUT_BUF_SIZE, "%c%c1%c%s%c\xFF", SOH,
                                       (write ? 'W' : 'R'), STX, request, ETX);
  this->calculate_crc_prog_frame_(this->buffers_.out, this->buffers_.amount_out, true);
}

void EnergomeraIecComponent::prepare_non_session_prog_frame_(const char *request) {
  // we assume request has format "XXXX(params)"
  // we assume it always has brackets

  // "/?!<SOH>R1<STX>NAME()<ETX><BCC>" broadcast
  // "/?<address>!<SOH>R1<STX>NAME()<ETX><BCC>" direct

  this->buffers_.amount_out = snprintf((char *) this->buffers_.out, MAX_OUT_BUF_SIZE, "/?%s!%cR1%c%s%c\xFF",
                                       this->meter_address_.c_str(), SOH, STX, request, ETX);
  // find SOH
  uint8_t *r1_ptr = std::find(this->buffers_.out, this->buffers_.out + this->buffers_.amount_out, SOH);
  size_t r1_size = r1_ptr - this->buffers_.out;
  calculate_crc_prog_frame_(r1_ptr, this->buffers_.amount_out - r1_size, true);
}

void EnergomeraIecComponent::prepare_ctime_frame_(uint8_t hh, uint8_t mm, uint8_t ss) {
  // "/?CTIME(HH:MM:SS)!\r\n"
  // no crc

  this->buffers_.amount_out =
      snprintf((char *) this->buffers_.out, MAX_OUT_BUF_SIZE, "/?CTIME(%02d:%02d:%02d)!\r\n", hh, mm, ss);
}

void EnergomeraIecComponent::send_frame_prepared_() {
  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(true);

  this->write_array(this->buffers_.out, this->buffers_.amount_out);
  this->flush();

  if (this->flow_control_pin_ != nullptr)
    this->flow_control_pin_->digital_write(false);

  ESP_LOGV(TAG, "TX: %s", format_frame_pretty(this->buffers_.out, this->buffers_.amount_out).c_str());
  ESP_LOGVV(TAG, "TX: %s", format_hex_pretty(this->buffers_.out, this->buffers_.amount_out).c_str());
}

void EnergomeraIecComponent::prepare_frame_(const uint8_t *data, size_t length) {
  memcpy(this->buffers_.out, data, length);
  this->buffers_.amount_out = length;
}

void EnergomeraIecComponent::send_frame_(const uint8_t *data, size_t length) {
  this->prepare_frame_(data, length);
  this->send_frame_prepared_();
}

size_t EnergomeraIecComponent::receive_frame_(FrameStopFunction stop_fn) {
  const uint32_t read_time_limit_ms = 25;
  size_t ret_val;

  auto count = this->available();
  if (count <= 0)
    return 0;

  uint32_t read_start = millis();
  uint8_t *p;
  while (count-- > 0) {
    if (millis() - read_start > read_time_limit_ms) {
      return 0;
    }

    if (this->buffers_.amount_in < MAX_IN_BUF_SIZE) {
      p = &this->buffers_.in[this->buffers_.amount_in];
      if (!iuart_->read_one_byte(p)) {
        return 0;
      }
      this->buffers_.amount_in++;
    } else {
      memmove(this->buffers_.in, this->buffers_.in + 1, this->buffers_.amount_in - 1);
      p = &this->buffers_.in[this->buffers_.amount_in - 1];
      if (!iuart_->read_one_byte(p)) {
        return 0;
      }
    }

    if (stop_fn(this->buffers_.in, this->buffers_.amount_in)) {
      ESP_LOGV(TAG, "RX: %s", format_frame_pretty(this->buffers_.in, this->buffers_.amount_in).c_str());
      ESP_LOGVV(TAG, "RX: %s", format_hex_pretty(this->buffers_.in, this->buffers_.amount_in).c_str());
      ret_val = this->buffers_.amount_in;
      this->buffers_.amount_in = 0;
      this->update_last_rx_time_();
      return ret_val;
    }

    yield();
    App.feed_wdt();
  }
  return 0;
}

size_t EnergomeraIecComponent::receive_frame_ascii_() {
  // "data<CR><LF>"
  ESP_LOGVV(TAG, "Waiting for ASCII frame");
  auto frame_end_check_crlf = [this](uint8_t *b, size_t s) {
    auto ret = s >= 2 && b[s - 1] == '\n' && b[s - 2] == '\r';
    if (ret) {
      ESP_LOGVV(TAG, "Frame CRLF Stop");
    }
    return ret;
  };
  return receive_frame_(frame_end_check_crlf);
}

size_t EnergomeraIecComponent::receive_frame_ack_nack_() {
  // "<ACK/NAK>"
  //  ESP_LOGVV(TAG, "Waiting for ACK/NAK frame");
  auto frame_end_check_ack_nack = [this](uint8_t *b, size_t s) {
    auto ret = s == 1 && (b[0] == ACK || b[0] == NAK);
    if (ret) {
      if (b[0] == ACK) {
        ESP_LOGVV(TAG, "Frame ACK Stop");
      } else {
        ESP_LOGVV(TAG, "Frame NAK Stop");
      }
    }
    return ret;
  };
  return receive_frame_(frame_end_check_ack_nack);
}

size_t EnergomeraIecComponent::receive_prog_frame_(uint8_t start_byte, bool accept_ack_and_nack) {
  // "<start_byte>data<ETX><BCC>"
  //  ESP_LOGVV(TAG, "Waiting for R1 frame, start byte: 0x%02x", start_byte);
  auto frame_end_check_iec = [=](uint8_t *b, size_t s) {
    auto ret = (accept_ack_and_nack && s == 1 && b[0] == ACK) ||  // ACK - request accepted
               (accept_ack_and_nack && s == 1 && b[0] == NAK) ||  // NACK - request rejected
               (s > 3 && b[0] == start_byte && b[s - 2] == ETX);  // Normal reply frame
    if (ret) {
      if (s == 1 && b[0] == ACK) {
        ESP_LOGVV(TAG, "Frame ACK Stop");
      } else if (s == 1 && b[0] == NAK) {
        ESP_LOGVV(TAG, "Frame NAK Stop");
      } else {
        ESP_LOGVV(TAG, "Frame ETX Stop");
      }
    }
    return ret;
  };
  return receive_frame_(frame_end_check_iec);
}

void EnergomeraIecComponent::clear_rx_buffers_() {
  int available = this->available();
  if (available > 0) {
    ESP_LOGVV(TAG, "Cleaning garbage from UART input buffer: %d bytes", available);
  }

  int len;
  while (available > 0) {
    len = std::min(available, (int) MAX_IN_BUF_SIZE);
    this->read_array(this->buffers_.in, len);
    available -= len;
  }
  memset(this->buffers_.in, 0, MAX_IN_BUF_SIZE);
  this->buffers_.amount_in = 0;
}

char *EnergomeraIecComponent::extract_meter_id_(size_t frame_size) {
  uint8_t *p = &this->buffers_.in[frame_size - 1 - 2 /*\r\n*/];
  size_t min_id_data_size = 7;  // min packet is '/XXXZ\r\n'

  while (p >= this->buffers_.in && frame_size >= min_id_data_size) {
    if ('/' == *p) {
      if ((size_t) (&this->buffers_.in[MAX_IN_BUF_SIZE - 1] - p) < min_id_data_size) {
        ESP_LOGV(TAG, "Invalid Meter ID packet.");
        // garbage, ignore
        break;
      }
      this->buffers_.in[frame_size - 2] = '\0';  // terminate string and remove \r\n
      ESP_LOGD(TAG, "Meter identification: '%s'", p);

      return (char *) p;
    }

    p--;
  }

  return nullptr;
}

uint8_t EnergomeraIecComponent::get_values_from_brackets_(char *line, ValueRefsArray &vals) {
  // line = "VOLTA(100.1)VOLTA(200.1)VOLTA(300.1)VOLTA(400.1)"
  vals.fill(empty_str);

  uint8_t idx = 0;
  bool got_param_name{false};
  char *p = line;
  while (*p && idx < VAL_NUM) {
    if (*p == '(') {
      if (!got_param_name) {
        got_param_name = true;
        *p = '\0';  // null-terminate param name
      }
      char *start = p + 1;
      char *end = strchr(start, ')');
      if (end) {
        *end = '\0';  // null-terminate value
        if (idx < VAL_NUM) {
          vals[idx++] = start;
        }
        p = end;
      }
    }
    p++;
  }
  return idx;  // at least one bracket found
}

// Get N-th value from comma-separated string, 1-based index
// line = "20.08.24,0.45991"
// get_nth_value_from_csv_(line, 1) -> "20.08.24"
// get_nth_value_from_csv_(line, 2) -> "0.45991"
char *EnergomeraIecComponent::get_nth_value_from_csv_(char *line, uint8_t idx) {
  if (idx == 0) {
    return line;
  }
  char *ptr;
  ptr = strtok(line, ",");
  while (ptr != nullptr) {
    if (idx-- == 1)
      return ptr;
    ptr = strtok(nullptr, ",");
  }
  return nullptr;
}

const char *EnergomeraIecComponent::state_to_string(State state) {
  switch (state) {
    case State::NOT_INITIALIZED:
      return "NOT_INITIALIZED";
    case State::IDLE:
      return "IDLE";
    case State::TRY_LOCK_BUS:
      return "TRY_LOCK_BUS";
    case State::WAIT:
      return "WAIT";
    case State::WAITING_FOR_RESPONSE:
      return "WAITING_FOR_RESPONSE";
    case State::OPEN_SESSION:
      return "OPEN_SESSION";
    case State::OPEN_SESSION_GET_ID:
      return "OPEN_SESSION_GET_ID";
    case State::SET_BAUD:
      return "SET_BAUD";
    case State::ACK_START_GET_INFO:
      return "ACK_START_GET_INFO";
    case State::GET_DATE:
      return "GET_DATE";
    case State::GET_TIME:
      return "GET_TIME";
    case State::CORRECT_TIME:
      return "CORRECT_TIME";
    case State::RECV_CORRECTION_RESULT:
      return "RECV_CORRECTION_RESULT";
    case State::DATA_ENQ:
      return "DATA_ENQ";
    case State::DATA_RECV:
      return "DATA_RECV";
    case State::DATA_NEXT:
      return "DATA_NEXT";
    case State::CLOSE_SESSION:
      return "CLOSE_SESSION";
    case State::PUBLISH:
      return "PUBLISH";
    case State::SINGLE_READ_ACK:
      return "SINGLE_READ_ACK";
    default:
      return "UNKNOWN";
  }
}

void EnergomeraIecComponent::log_state_(State *next_state) {
  if (this->state_ != this->last_reported_state_) {
    if (next_state == nullptr) {
      ESP_LOGV(TAG, "State::%s", this->state_to_string(this->state_));
    } else {
      ESP_LOGV(TAG, "State::%s -> %s", this->state_to_string(this->state_), this->state_to_string(*next_state));
    }
    this->last_reported_state_ = this->state_;
  }
}

void EnergomeraIecComponent::stats_dump_() {
  ESP_LOGV(TAG, "============================================");
  ESP_LOGV(TAG, "Data collection and publishing finished.");
  ESP_LOGV(TAG, "Total number of sessions ............. %u", this->stats_.connections_tried_);
  ESP_LOGV(TAG, "Total number of invalid frames ....... %u", this->stats_.invalid_frames_);
  ESP_LOGV(TAG, "Total number of CRC errors ........... %u", this->stats_.crc_errors_);
  ESP_LOGV(TAG, "Total number of CRC errors recovered . %u", this->stats_.crc_errors_recovered_);
  ESP_LOGV(TAG, "CRC errors per session ............... %f", this->stats_.crc_errors_per_session());
  ESP_LOGV(TAG, "Number of failures ................... %u", this->stats_.failures_);
  ESP_LOGV(TAG, "============================================");
}

bool EnergomeraIecComponent::try_lock_uart_session_() {
  if (AnyObjectLocker::try_lock(this->parent_)) {
    ESP_LOGVV(TAG, "UART bus %p locked by %s", this->parent_, this->tag_.c_str());
    return true;
  }
  ESP_LOGVV(TAG, "UART bus %p busy", this->parent_);
  return false;
}

void EnergomeraIecComponent::unlock_uart_session_() {
  AnyObjectLocker::unlock(this->parent_);
  ESP_LOGVV(TAG, "UART bus %p released by %s", this->parent_, this->tag_.c_str());
}

uint8_t EnergomeraIecComponent::next_obj_id_ = 0;

std::string EnergomeraIecComponent::generateTag() { return str_sprintf("%s%03d", TAG0, ++next_obj_id_); }

}  // namespace energomera_iec
}  // namespace esphome
