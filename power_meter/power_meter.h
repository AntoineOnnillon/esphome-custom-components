#pragma once

#include <vector>

#include "esphome/components/mcp3008/mcp3008.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace power_meter {

class PowerMeterChannel {
 public:
  void set_adc(mcp3008::MCP3008 *adc) { this->adc_ = adc; }
  void set_channel(uint8_t ch) { this->channel_ = ch; }
  void set_burden_resistor(float r) { this->burden_resistor_ = r; }
  void set_ct_ratio(float r) { this->ct_ratio_ = r; }
  void set_phase_correction_deg(float d) { this->phase_correction_deg_ = d; }
  void set_current_sensor(sensor::Sensor *s) { this->current_sensor_ = s; }
  void set_power_sensor(sensor::Sensor *s) { this->power_sensor_ = s; }
  void set_apparent_power_sensor(sensor::Sensor *s) { this->apparent_power_sensor_ = s; }
  void set_power_factor_sensor(sensor::Sensor *s) { this->power_factor_sensor_ = s; }

  mcp3008::MCP3008 *adc_{nullptr};
  uint8_t channel_{0};
  float burden_resistor_{22.0f};
  float ct_ratio_{2000.0f};
  float phase_correction_deg_{0.0f};

  sensor::Sensor *current_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *apparent_power_sensor_{nullptr};
  sensor::Sensor *power_factor_sensor_{nullptr};

  // Per-update accumulators (reset in update()).
  double sum_i_{0};
  double sum_i2_{0};
  double sum_vi_{0};
  uint32_t count_{0};
};

class PowerMeter : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_samples(uint32_t n) { this->samples_ = n; }
  void set_adc_reference_voltage(float v) { this->adc_ref_ = v; }
  // Templatable setter: accepts a raw float or a lambda from cg.templatable().
  template<typename V> void set_nominal_voltage(V v) { this->nominal_voltage_ = v; }
  float nominal_voltage() { return this->nominal_voltage_.value(); }
  void set_voltage_channel(mcp3008::MCP3008 *adc, uint8_t ch, float factor) {
    this->voltage_adc_ = adc;
    this->voltage_channel_ = ch;
    this->voltage_factor_ = factor;
  }
  void set_reference_voltage_sensor(sensor::Sensor *s) { this->reference_voltage_sensor_ = s; }
  void set_overload_sensor(sensor::Sensor *s) { this->overload_sensor_ = s; }
  void set_default_phase_correction_deg(float d) { this->default_phase_correction_deg_ = d; }
  void set_noise_gate_pct(float p) { this->noise_gate_pct_ = p; }
  void set_sequential_per_mcp(bool b) { this->sequential_per_mcp_ = b; }
  void add_channel(PowerMeterChannel *c) { this->channels_.push_back(c); }

 protected:
  uint32_t samples_{200};
  float adc_ref_{5.0f};

  mcp3008::MCP3008 *voltage_adc_{nullptr};
  uint8_t voltage_channel_{0};
  // Mutable: EMA-drifted toward reference_voltage_sensor when auto-cal enabled.
  float voltage_factor_{1.0f};

  sensor::Sensor *reference_voltage_sensor_{nullptr};
  sensor::Sensor *overload_sensor_{nullptr};

  // Global phase correction added to each channel's per-channel value.
  float default_phase_correction_deg_{0.0f};
  // Per-channel P threshold = P_max_channel * noise_gate_pct_ / 100.
  float noise_gate_pct_{0.0f};

  // When true, each update() burst covers channels of one MCP3008 only, cycling.
  bool sequential_per_mcp_{false};
  // Cached groups (one entry per MCP3008 hit by the configured channels).
  std::vector<std::vector<PowerMeterChannel *>> groups_;
  uint32_t current_group_{0};

  std::vector<PowerMeterChannel *> channels_;

  TemplatableValue<float> nominal_voltage_{230.0f};

  // Rolling estimate of per-iteration wall-clock time -- feeds phase correction.
  float last_t_iter_us_{0.0f};

  // Persist the auto-calibrated voltage_factor across reboots.
  ESPPreferenceObject pref_voltage_factor_;
  bool has_pref_{false};
  float last_saved_voltage_factor_{0.0f};
  // Throttle flash writes: at most once every MIN_FLASH_SAVE_INTERVAL_MS.
  uint32_t last_flash_save_ms_{0};

  // In sequential_per_mcp mode, tracks the worst-burst clipping ratio across
  // the full cycle so the overload sensor doesn't oscillate between groups.
  float cycle_max_overload_pct_{0.0f};
};

}  // namespace power_meter
}  // namespace esphome
