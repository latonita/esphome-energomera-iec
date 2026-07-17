#pragma once
#include <cstddef>
#include <cstdint>

#include "esphome/components/uart/uart.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace energomera_iec {

static const uint32_t TIMEOUT = 20;  // ms; default timeout in the uart implementation is 100ms

// Thin helper around the configured UART bus.
//
// It exists only to (a) change the baud rate at runtime -- the IEC 61107 handshake negotiates a
// new speed in the middle of a session -- and (b) read a single byte with a short timeout instead
// of the default 100ms. Both are done through the public UARTComponent API, so it makes no
// assumptions about the concrete platform driver (ESP-IDF, ESP8266, ...).
class EnergomeraIecUart {
 public:
  explicit EnergomeraIecUart(uart::UARTComponent *parent) : parent_(parent) {}

  // Reconfigure the bus to a new baud rate.
  void update_baudrate(uint32_t baudrate) {
    this->parent_->set_baud_rate(baudrate);
    this->parent_->load_settings(false);
  }

  // Read one byte, waiting at most TIMEOUT ms for it to arrive.
  bool read_one_byte(uint8_t *data) {
    if (!this->wait_for_data_(1))
      return false;
    return this->parent_->read_byte(data);
  }

 protected:
  bool wait_for_data_(size_t len) {
    uint32_t start_time = millis();
    while (this->parent_->available() < len) {
      if (millis() - start_time > TIMEOUT) {
        return false;
      }
      yield();
    }
    return true;
  }

  uart::UARTComponent *parent_;
};

}  // namespace energomera_iec
}  // namespace esphome
