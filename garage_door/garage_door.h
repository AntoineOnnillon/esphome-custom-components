#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome {
namespace garage_door {

class GarageDoorCover : public cover::Cover, public Component, public i2c::I2CDevice {
 public:
  static constexpr size_t FRAME_MAX = 16;

  void set_scl_monitor_pin(InternalGPIOPin *pin) { this->scl_monitor_pin_ = pin; }
  void set_sda_monitor_pin(InternalGPIOPin *pin) { this->sda_monitor_pin_ = pin; }
  void set_obstacle_binary_sensor(binary_sensor::BinarySensor *bs) { this->obstacle_bs_ = bs; }
  void set_online_binary_sensor(binary_sensor::BinarySensor *bs) { this->online_bs_ = bs; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  cover::CoverTraits get_traits() override;
  void control(const cover::CoverCall &call) override;

 protected:
  void send_open_frame_();
  void send_close_frame_();
  void handle_status_(uint8_t code, uint8_t mode);
  void set_obstacle_(bool detected);
  void save_calibration_();
  void save_position_();
  void update_estimated_position_();
  bool is_calibrated_() const {
    return this->cal_.open_ms > 0 && this->cal_.close_ms > 0;
  }
  static bool matches_signature_(const uint8_t *buf, uint8_t len, uint8_t direction);
  static bool matches_status_frame_(const uint8_t *buf, uint8_t len);

  static void IRAM_ATTR scl_isr(GarageDoorCover *arg);
  static void IRAM_ATTR sda_isr(GarageDoorCover *arg);

  InternalGPIOPin *scl_monitor_pin_{nullptr};
  InternalGPIOPin *sda_monitor_pin_{nullptr};
  ISRInternalGPIOPin scl_isr_pin_;
  ISRInternalGPIOPin sda_isr_pin_;

  binary_sensor::BinarySensor *obstacle_bs_{nullptr};
  binary_sensor::BinarySensor *online_bs_{nullptr};

  // Timestamp de la derniere trame recue du bus (pour detecter panne controleur).
  uint32_t last_activity_ms_{0};
  bool online_state_{true};

  // Dernier "mode" observe (byte[6] du status frame): 0x02=normal, 0x06=obstacle.
  uint8_t last_status_mode_{0x02};

  // Calibration persistee: durees d'un cycle complet ferme<->ouvert en ms.
  struct Calibration {
    uint32_t open_ms;
    uint32_t close_ms;
  } cal_{0, 0};
  ESPPreferenceObject pref_cal_;

  // Position persistee (survit reboot / OTA).
  ESPPreferenceObject pref_pos_;

  // Tracking du mouvement en cours pour la calibration automatique.
  uint32_t motion_start_ms_{0};
  float motion_start_position_{NAN};
  cover::CoverOperation motion_direction_{cover::COVER_OPERATION_IDLE};

  // Throttle pour la publication live de position (500 ms suffit pour l'UI HA).
  uint32_t last_position_publish_ms_{0};

  volatile bool in_transaction_{false};
  volatile bool frame_ready_{false};
  volatile uint8_t bit_pos_{0};
  volatile uint8_t current_byte_{0};
  volatile uint8_t frame_len_{0};
  volatile uint8_t frame_buf_[FRAME_MAX]{0};
};

}  // namespace garage_door
}  // namespace esphome
