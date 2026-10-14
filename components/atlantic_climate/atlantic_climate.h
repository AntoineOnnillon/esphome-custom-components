#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace atlantic_climate {

// Protocole proprietaire Atlantic ("bus fil pilote" evolue) sur UART 4800 8O2 inverse.
// Trame sur le fil apres inversion:
//   0xDC | 0x80|sender | dest | frame_len | payload... | crc16_hi | crc16_lo
// Le premier octet vu sur l'UART est donc 0x23 (inverse de 0xDC).
class AtlanticClimate : public climate::Climate, public PollingComponent, public uart::UARTDevice {
 public:
  static constexpr uint8_t BROADCAST_ADDR = 0x7F;
  static constexpr uint16_t POLY_XMODEM = 0x1021;
  static constexpr size_t BUFFER_SIZE = 32;

  void set_address(uint8_t address) { this->address_ = address; }

  void set_ambient_temperature_sensor(sensor::Sensor *s) { this->ambient_temp_sensor_ = s; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

 protected:
  // Emission
  void request_setpoint_();
  void request_ambient_temperature_();
  void probe_register_(uint8_t target, uint16_t reg);
  void encode_temperature_frame_(float temperature);
  void encode_mode_frame_(climate::ClimateMode mode, climate::ClimatePreset preset);
  void send_pending_frame_();

  // Reception (non bloquante, alimentee octet par octet par loop()).
  void feed_rx_byte_(uint8_t byte);
  void process_frame_();
  bool parse_mode_payload_(const uint8_t *data, uint8_t len);
  bool parse_setpoint_payload_(const uint8_t *data, uint8_t len);
  bool parse_ambient_payload_(const uint8_t *data, uint8_t len);
  void update_action_from_mode_();

  // Utilitaires bas niveau.
  static uint16_t crc16_(const uint8_t *data, uint8_t data_len, uint16_t seed = 0);
  static void invert_(uint8_t *data, uint8_t data_len);
  uint8_t build_frame_(uint8_t *dst, const uint8_t *payload, uint8_t payload_len);

  uint8_t address_{7};

  // Buffer d'emission.
  uint8_t tx_payload_[BUFFER_SIZE]{};
  uint8_t tx_payload_len_{0};

  // Etat de reception (parser byte par byte).
  enum RxState : uint8_t {
    RX_IDLE = 0,
    RX_HEADER,
    RX_PAYLOAD,
  } rx_state_{RX_IDLE};
  uint8_t rx_buffer_[BUFFER_SIZE]{};
  uint8_t rx_len_{0};       // octets deja recus dans rx_buffer_
  uint8_t rx_expected_{0};  // longueur totale attendue (une fois header decode)

  // Presets memorises pour rejouer un changement mode <-> preset sans perdre l'intention.
  climate::ClimatePreset manual_preset_{climate::CLIMATE_PRESET_COMFORT};
  climate::ClimatePreset auto_preset_{climate::CLIMATE_PRESET_COMFORT};

  float last_sent_target_{NAN};
  climate::ClimateMode last_sent_mode_{climate::CLIMATE_MODE_OFF};

  sensor::Sensor *ambient_temp_sensor_{nullptr};

  // --- Persistance flash ---
  struct PersistedState {
    float last_sent_target;
    uint8_t last_sent_mode;
    uint8_t manual_preset;
    uint8_t auto_preset;
    uint8_t reserved;
  } __attribute__((packed));
  ESPPreferenceObject pref_state_;
  void save_persistent_state_();
  void load_persistent_state_();
};

}  // namespace atlantic_climate
}  // namespace esphome
