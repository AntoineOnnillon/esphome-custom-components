#include "power_meter.h"

#include <cmath>
#include <vector>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace power_meter {

static const char *const TAG = "power_meter";

// deg / (360 * 50Hz) = seconds, ×1e6 -> µs. Precomputed: 1e6/18000 ≈ 55.5556.
static constexpr float DEG_TO_US_50HZ = 1.0e6f / (360.0f * 50.0f);
// 2 * sqrt(2) precomputed. Used to convert ADC peak swing to RMS voltage.
static constexpr float TWO_SQRT2 = 2.828427125f;
// EMA weight for the voltage-factor auto-calibration (0.1 = ~30 updates to converge).
static constexpr float AUTOCAL_ALPHA = 0.1f;
// Persist voltage_factor to flash only when it drifts more than this relative amount.
static constexpr float FLASH_SAVE_THRESHOLD = 0.005f;  // 0.5 %
// Minimum interval between flash writes to protect NVS cells, esp. in sequential mode.
static constexpr uint32_t MIN_FLASH_SAVE_INTERVAL_MS = 30000;

void PowerMeter::setup() {
  // Group channels by physical MCP3008 (order preserved from YAML) so that
  // sequential_per_mcp mode can process one group per update() call.
  for (auto *c : this->channels_) {
    bool found = false;
    for (auto &g : this->groups_) {
      if (g.front()->adc_ == c->adc_) {
        g.push_back(c);
        found = true;
        break;
      }
    }
    if (!found) {
      this->groups_.emplace_back(std::vector<PowerMeterChannel *>{c});
    }
  }

  if (this->voltage_adc_ == nullptr || this->reference_voltage_sensor_ == nullptr) {
    return;
  }
  this->pref_voltage_factor_ =
      global_preferences->make_preference<float>(fnv1_hash("power_meter_voltage_factor"), true);
  this->has_pref_ = true;
  float saved;
  if (this->pref_voltage_factor_.load(&saved) && saved > 1.0f && saved < 100000.0f) {
    ESP_LOGI(TAG, "Restored voltage_factor from flash: %.3f (YAML initial: %.3f)",
             saved, this->voltage_factor_);
    this->voltage_factor_ = saved;
  }
  this->last_saved_voltage_factor_ = this->voltage_factor_;
}

void PowerMeter::on_shutdown() {
  if (this->has_pref_ && this->voltage_factor_ != this->last_saved_voltage_factor_) {
    this->pref_voltage_factor_.save(&this->voltage_factor_);
  }
}

void PowerMeter::dump_config() {
  ESP_LOGCONFIG(TAG, "Power Meter:");
  ESP_LOGCONFIG(TAG, "  Samples per burst: %u", this->samples_);
  ESP_LOGCONFIG(TAG, "  ADC reference voltage: %.2f V", this->adc_ref_);
  if (this->voltage_adc_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Voltage: MCP3008 channel %u, factor %.2f (mode: W)",
                  this->voltage_channel_, this->voltage_factor_);
    if (this->reference_voltage_sensor_ != nullptr) {
      ESP_LOGCONFIG(TAG, "  Auto-calibration: enabled (EMA α=%.2f)", AUTOCAL_ALPHA);
    }
  } else {
    ESP_LOGCONFIG(TAG, "  No voltage channel -- VA only against nominal_voltage");
  }
  ESP_LOGCONFIG(TAG, "  Channels: %u (default phase_corr=%+.2f deg, noise_gate=%.2f %%)",
                (unsigned) this->channels_.size(), this->default_phase_correction_deg_,
                this->noise_gate_pct_);
  ESP_LOGCONFIG(TAG, "  Sequential per MCP: %s (%u group%s)",
                YESNO(this->sequential_per_mcp_), (unsigned) this->groups_.size(),
                this->groups_.size() > 1 ? "s" : "");
  for (uint32_t g = 0; g < this->groups_.size(); g++) {
    for (auto *c : this->groups_[g]) {
      ESP_LOGCONFIG(TAG,
                    "    - group=%u ch=%u burden=%.2f Ohm ct_ratio=%.1f phase_corr=%+.2f deg",
                    g, c->channel_, c->burden_resistor_, c->ct_ratio_,
                    c->phase_correction_deg_ + this->default_phase_correction_deg_);
    }
  }
  LOG_UPDATE_INTERVAL(this);
}

void PowerMeter::update() {
  const bool has_v = this->voltage_adc_ != nullptr;
  const float adc_ref = this->adc_ref_;

  // Pick the channel subset for this burst: full list or one MCP group at a time.
  std::vector<PowerMeterChannel *> *active_channels = &this->channels_;
  if (this->sequential_per_mcp_ && !this->groups_.empty()) {
    // Reset the cycle-wide overload tracker each time we start a new full cycle.
    if (this->current_group_ == 0) {
      this->cycle_max_overload_pct_ = 0.0f;
    }
    active_channels = &this->groups_[this->current_group_];
    ESP_LOGD(TAG, "Sequential burst: group %u/%u",
             this->current_group_ + 1, (unsigned) this->groups_.size());
    this->current_group_ = (this->current_group_ + 1) % this->groups_.size();
  }
  const uint32_t n_ch = active_channels->size();
  if (n_ch == 0) {
    ESP_LOGW(TAG, "No channels configured");
    return;
  }

  double sum_v = 0.0, sum_v2 = 0.0;
  uint32_t v_count = 0;
  uint32_t clip_count = 0, total_readings = 0;
  for (auto *c : *active_channels) {
    c->sum_i_ = 0.0;
    c->sum_i2_ = 0.0;
    c->sum_vi_ = 0.0;
    c->count_ = 0;
  }

  // Per-channel phase offset β = phase_deg × (µs / deg@50Hz) / T_iter_us.
  // First burst: bootstrap T_iter from ~350 µs/read (75 kHz SPI + overhead).
  const float t_iter_est_us =
      this->last_t_iter_us_ > 0.0f ? this->last_t_iter_us_ : (n_ch + 1) * 350.0f;
  std::vector<float> beta(n_ch);
  for (uint32_t k = 0; k < n_ch; k++) {
    const float total_deg =
        (*active_channels)[k]->phase_correction_deg_ + this->default_phase_correction_deg_;
    beta[k] = total_deg * DEG_TO_US_50HZ / t_iter_est_us;
  }

  float v_prev = 0.0f;
  if (has_v) {
    float raw = this->voltage_adc_->read_data(this->voltage_channel_);
    if (raw <= 0.001f || raw >= 0.999f)
      clip_count++;
    total_readings++;
    v_prev = raw * adc_ref;
    sum_v += v_prev;
    sum_v2 += v_prev * v_prev;
    v_count++;
  }

  std::vector<float> i_samples(n_ch);
  const uint32_t t_start_us = micros();

  for (uint32_t s = 0; s < this->samples_; s++) {
    for (uint32_t k = 0; k < n_ch; k++) {
      auto *c = (*active_channels)[k];
      float raw = c->adc_->read_data(c->channel_);
      if (raw <= 0.001f || raw >= 0.999f)
        clip_count++;
      total_readings++;
      i_samples[k] = raw * adc_ref;
    }
    float v_curr = v_prev;
    if (has_v) {
      float raw = this->voltage_adc_->read_data(this->voltage_channel_);
      if (raw <= 0.001f || raw >= 0.999f)
        clip_count++;
      total_readings++;
      v_curr = raw * adc_ref;
      sum_v += v_curr;
      sum_v2 += v_curr * v_curr;
      v_count++;
    }

    const float v_delta = v_curr - v_prev;
    const float inv_np1 = 1.0f / static_cast<float>(n_ch + 1);
    for (uint32_t k = 0; k < n_ch; k++) {
      auto *c = (*active_channels)[k];
      // α = time fraction of I_k between v_prev and v_curr, minus per-channel phase-corr shift.
      const float alpha = (k + 1.0f) * inv_np1 - beta[k];
      const float v_k = v_prev + alpha * v_delta;
      const float i_k = i_samples[k];
      c->sum_i_ += i_k;
      c->sum_i2_ += static_cast<double>(i_k) * i_k;
      if (has_v) {
        c->sum_vi_ += static_cast<double>(v_k) * i_k;
      }
      c->count_++;
    }

    v_prev = v_curr;
    if ((s & 0x1F) == 0)
      App.feed_wdt();
  }

  const uint32_t t_burst_us = micros() - t_start_us;
  this->last_t_iter_us_ = static_cast<float>(t_burst_us) / this->samples_;
  ESP_LOGD(TAG, "Burst: %u samples in %u us (T_iter=%.0f us, clipped=%u/%u)",
           this->samples_, t_burst_us, this->last_t_iter_us_, clip_count, total_readings);

  if (this->overload_sensor_ != nullptr && total_readings > 0) {
    const float burst_pct = 100.0f * clip_count / total_readings;
    if (this->sequential_per_mcp_) {
      if (burst_pct > this->cycle_max_overload_pct_)
        this->cycle_max_overload_pct_ = burst_pct;
      this->overload_sensor_->publish_state(this->cycle_max_overload_pct_);
    } else {
      this->overload_sensor_->publish_state(burst_pct);
    }
  }

  double mean_v = 0.0, vrms_adc = 0.0;
  if (has_v && v_count > 0) {
    mean_v = sum_v / v_count;
    double var_v = sum_v2 / v_count - mean_v * mean_v;
    if (var_v < 0.0)
      var_v = 0.0;
    vrms_adc = std::sqrt(var_v);
  }

  if (has_v && this->reference_voltage_sensor_ != nullptr && vrms_adc > 0.05) {
    const float v_ref = this->reference_voltage_sensor_->state;
    if (!std::isnan(v_ref) && v_ref > 50.0f) {
      const float new_factor = v_ref / static_cast<float>(vrms_adc);
      this->voltage_factor_ =
          this->voltage_factor_ * (1.0f - AUTOCAL_ALPHA) + new_factor * AUTOCAL_ALPHA;
      ESP_LOGD(TAG, "Auto-cal: v_ref=%.1f V, vrms_adc=%.3f V, factor=%.3f (instant=%.3f)",
               v_ref, vrms_adc, this->voltage_factor_, new_factor);

      if (this->has_pref_) {
        const float ref_val =
            this->last_saved_voltage_factor_ > 0.0f ? this->last_saved_voltage_factor_ : 1.0f;
        const float drift =
            std::fabs(this->voltage_factor_ - this->last_saved_voltage_factor_) / ref_val;
        const uint32_t now = millis();
        if (drift > FLASH_SAVE_THRESHOLD &&
            (this->last_flash_save_ms_ == 0 ||
             now - this->last_flash_save_ms_ >= MIN_FLASH_SAVE_INTERVAL_MS)) {
          this->pref_voltage_factor_.save(&this->voltage_factor_);
          this->last_saved_voltage_factor_ = this->voltage_factor_;
          this->last_flash_save_ms_ = now;
          ESP_LOGD(TAG, "Saved voltage_factor to flash: %.3f", this->voltage_factor_);
        }
      }
    }
  }

  const double vrms_line =
      has_v ? (vrms_adc * this->voltage_factor_) : this->nominal_voltage();

  for (auto *c : *active_channels) {
    if (c->count_ == 0)
      continue;
    const double mean_i = c->sum_i_ / c->count_;
    double var_i = c->sum_i2_ / c->count_ - mean_i * mean_i;
    if (var_i < 0.0)
      var_i = 0.0;
    const double irms_adc = std::sqrt(var_i);
    const double irms = (irms_adc / c->burden_resistor_) * c->ct_ratio_;

    if (c->current_sensor_ != nullptr)
      c->current_sensor_->publish_state(static_cast<float>(irms));

    const double s_va = vrms_line * irms;
    if (c->apparent_power_sensor_ != nullptr)
      c->apparent_power_sensor_->publish_state(static_cast<float>(s_va));

    if (has_v) {
      const double cov_adc = c->sum_vi_ / c->count_ - mean_v * mean_i;
      double p_watt =
          cov_adc * this->voltage_factor_ * (c->ct_ratio_ / c->burden_resistor_);
      // Per-channel gate: % of the theoretical full-scale P for this CT.
      // I_max_rms = (adc_ref / (2*sqrt(2))) / burden * ct_ratio  (symmetric swing).
      bool gated = false;
      if (this->noise_gate_pct_ > 0.0f) {
        const double i_max_rms =
            (adc_ref / TWO_SQRT2) / c->burden_resistor_ * c->ct_ratio_;
        const double p_max_ch = i_max_rms * vrms_line;
        const double threshold = p_max_ch * this->noise_gate_pct_ / 100.0;
        if (std::fabs(p_watt) < threshold) {
          p_watt = 0.0;
          gated = true;
        }
      }
      if (c->power_sensor_ != nullptr)
        c->power_sensor_->publish_state(static_cast<float>(p_watt));
      if (c->power_factor_sensor_ != nullptr) {
        double pf = 0.0;
        if (!gated && s_va > 1e-3) {
          pf = p_watt / s_va;
          if (pf > 1.0)
            pf = 1.0;
          else if (pf < -1.0)
            pf = -1.0;
        }
        c->power_factor_sensor_->publish_state(static_cast<float>(pf));
      }
    } else {
      if (c->power_sensor_ != nullptr)
        c->power_sensor_->publish_state(NAN);
      if (c->power_factor_sensor_ != nullptr)
        c->power_factor_sensor_->publish_state(NAN);
    }
  }
}

}  // namespace power_meter
}  // namespace esphome
