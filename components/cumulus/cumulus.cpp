#include "cumulus.h"

#include <cmath>
#include <ctime>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace cumulus {

static const char *const TAG = "cumulus";

// Chaleur massique de l'eau, J/(kg.K). On considere 1 kg/L pour un ballon standard.
static constexpr float WATER_CP_J_PER_KG_K = 4185.0f;
// Temperature de reference "eau froide reseau" pour l'estimation d'energie stockee.
static constexpr float COLD_WATER_REF_C = 15.0f;
// Marge en dessous du seuil d'overheat pour sortir du safety latch (10 C).
static constexpr float SAFETY_RECOVERY_MARGIN_C = 10.0f;
// Save flash au plus toutes les 60 s (limite l'usure NVS).
static constexpr uint32_t PREF_SAVE_INTERVAL_MS = 60u * 1000u;

const char *cumulus_mode_to_string(CumulusMode mode) {
  switch (mode) {
    case CUMULUS_MODE_OFF: return "off";
    case CUMULUS_MODE_IDLE: return "idle";
    case CUMULUS_MODE_PV_WAIT: return "pv_wait";
    case CUMULUS_MODE_THERMOSTAT: return "thermostat";
    case CUMULUS_MODE_PV_MOD: return "pv_mod";
    case CUMULUS_MODE_PV_ECO: return "pv_eco";
    case CUMULUS_MODE_BOOST: return "boost";
    case CUMULUS_MODE_LEGIONELLA: return "legionella";
    case CUMULUS_MODE_SAFETY: return "safety";
  }
  return "unknown";
}

// -----------------------------------------------------------------------------
// Cycle de vie
// -----------------------------------------------------------------------------

void CumulusController::setup() {
  this->pref_state_ = global_preferences->make_preference<PersistedState>(
      fnv1_hash("cumulus_state_v1"), true);
  this->load_state_();
  const uint32_t now = millis();
  this->last_update_ms_ = now;
  this->duty_history_last_bucket_ms_ = now;
  this->last_valid_sensor_ms_ = now;

  if (this->heater_output_ != nullptr) {
    this->heater_output_->set_level(0.0f);
  }
  if (this->pump_switch_ != nullptr) {
    this->pump_switch_->turn_off();
  }
}

void CumulusController::loop() {
  // Watchdog sondes: si aucune donnee valide depuis longtemps, on force IDLE
  // pour eviter de chauffer a l'aveugle.
  const uint32_t now = millis();
  const bool has_top = this->top_temp_sensor_ != nullptr &&
                       !std::isnan(this->top_temp_sensor_->state);
  if (has_top) {
    this->last_valid_sensor_ms_ = now;
  }
}

void CumulusController::update() {
  const uint32_t now = millis();
  const float dt_s = (now - this->last_update_ms_) / 1000.0f;
  this->last_update_ms_ = now;
  if (dt_s <= 0.0f) {
    return;
  }

  this->handle_daily_rollover_();
  this->accumulate_pv_budget_(dt_s);
  this->recompute_effective_target_();

  // Mise a jour du safety latch en debut de cycle: le compute_mode_() peut ensuite
  // decider LEGIONELLA/BOOST/etc. mais SAFETY prime toujours.
  this->check_safety_();

  const CumulusMode mode = this->compute_mode_();
  const float duty = this->compute_duty_(mode);

  this->apply_outputs_(duty);
  this->update_pump_(duty > 0.01f);
  this->deduct_pv_budget_(dt_s, duty);
  this->sample_duty_history_(duty, dt_s);

  // Puissance instantanee: on prefere la mesure (CT clamp) au produit duty * P_nom.
  const bool has_measured =
      this->heater_power_measured_sensor_ != nullptr &&
      !std::isnan(this->heater_power_measured_sensor_->state);
  const float instant_power_w = has_measured
                                    ? this->heater_power_measured_sensor_->state
                                    : (this->element_power_w_ * duty);
  this->energy_today_wh_ += instant_power_w * (dt_s / 3600.0f);

  // Sante resistance: EMA sur (mesure / attendu) pour detecter defaillance / calibration.
  if (has_measured && duty > 0.2f) {
    const float expected_w = this->element_power_w_ * duty;
    if (expected_w > 100.0f) {
      const float health = 100.0f * this->heater_power_measured_sensor_->state / expected_w;
      if (std::isnan(this->element_health_pct_)) {
        this->element_health_pct_ = health;
      } else {
        this->element_health_pct_ = 0.9f * this->element_health_pct_ + 0.1f * health;
      }
    }
  }

  // Suivi standby (top_temp qui derive quand duty = 0).
  if (duty < 0.01f && this->top_temp_sensor_ != nullptr &&
      !std::isnan(this->top_temp_sensor_->state)) {
    const float t = this->top_temp_sensor_->state;
    if (std::isnan(this->standby_last_top_c_)) {
      this->standby_last_top_c_ = t;
      this->standby_last_top_ms_ = now;
    } else {
      const float delta_h = (now - this->standby_last_top_ms_) / (1000.0f * 3600.0f);
      if (delta_h >= 0.5f) {
        // Fenetre glissante 30min minimum pour lisser le bruit.
        const float rate = (this->standby_last_top_c_ - t) / delta_h;
        if (this->standby_loss_sensor_ != nullptr) {
          this->standby_loss_sensor_->publish_state(rate);
        }
        this->standby_last_top_c_ = t;
        this->standby_last_top_ms_ = now;
      }
    }
  } else {
    // Reset des que la chauffe reprend.
    this->standby_last_top_c_ = NAN;
  }

  // Publications telemetrie.
  if (this->heater_level_sensor_ != nullptr) {
    this->heater_level_sensor_->publish_state(duty * 100.0f);
  }
  if (this->heater_power_sensor_ != nullptr) {
    this->heater_power_sensor_->publish_state(instant_power_w);
  }
  if (this->stored_energy_sensor_ != nullptr) {
    this->stored_energy_sensor_->publish_state(this->estimate_stored_energy_wh_() / 1000.0f);
  }
  if (this->energy_today_sensor_ != nullptr) {
    this->energy_today_sensor_->publish_state(this->energy_today_wh_ / 1000.0f);
  }
  if (this->pv_budget_sensor_ != nullptr) {
    this->pv_budget_sensor_->publish_state(this->pv_budget_wh_ / 1000.0f);
  }
  if (this->pv_surplus_used_sensor_ != nullptr) {
    this->pv_surplus_used_sensor_->publish_state(this->pv_surplus_used_today_wh_ / 1000.0f);
  }
  if (this->time_to_target_sensor_ != nullptr) {
    this->time_to_target_sensor_->publish_state(this->estimate_time_to_target_min_(duty));
  }
  if (this->overheat_count_sensor_ != nullptr) {
    this->overheat_count_sensor_->publish_state(this->overheat_count_);
  }
  if (this->days_since_legionella_sensor_ != nullptr) {
    // Approximation: on utilise le time source globale.
    const time_t now_time = ::time(nullptr);
    if (this->last_legionella_unix_ == 0 || now_time <= 0) {
      this->days_since_legionella_sensor_->publish_state(NAN);
    } else {
      const uint64_t now_unix = static_cast<uint64_t>(now_time);
      const float days = (now_unix - this->last_legionella_unix_) / 86400.0f;
      this->days_since_legionella_sensor_->publish_state(days);
    }
  }
  if (this->mode_text_sensor_ != nullptr) {
    const char *s = cumulus_mode_to_string(mode);
    if (this->mode_text_sensor_->state != s) {
      this->mode_text_sensor_->publish_state(s);
    }
  }
  if (this->pump_duty_sensor_ != nullptr) {
    // Duty pompe = fraction du jour ou la pompe a ete active.
    const float day_s = 86400.0f;
    this->pump_duty_sensor_->publish_state(100.0f * this->pump_run_today_s_ / day_s);
  }

  // Telemetrie planificateur.
  if (this->effective_target_sensor_ != nullptr) {
    this->effective_target_sensor_->publish_state(this->effective_target_c_);
  }
  if (this->dynamic_offset_sensor_ != nullptr) {
    this->dynamic_offset_sensor_->publish_state(this->dynamic_offset_c_);
  }
  if (this->horizon_available_sensor_ != nullptr) {
    this->horizon_available_sensor_->publish_state(this->compute_horizon_available_kwh_());
  }
  if (this->energy_deficit_forecast_sensor_ != nullptr) {
    const float available = this->compute_horizon_available_kwh_();
    const float needed = this->daily_consumption_kwh_ * this->horizon_days_;
    this->energy_deficit_forecast_sensor_->publish_state(needed - available);
  }
  if (this->peak_window_active_sensor_ != nullptr) {
    this->peak_window_active_sensor_->publish_state(this->is_in_peak_window_() ? 1.0f : 0.0f);
  }
  if (this->element_health_sensor_ != nullptr && !std::isnan(this->element_health_pct_)) {
    this->element_health_sensor_->publish_state(this->element_health_pct_);
  }

  this->current_mode_ = mode;
  this->last_duty_ = duty;

  this->save_state_throttled_();
}

void CumulusController::dump_config() {
  ESP_LOGCONFIG(TAG, "Cumulus Controller:");
  ESP_LOGCONFIG(TAG, "  Volume ballon: %.0f L", this->tank_volume_l_);
  ESP_LOGCONFIG(TAG, "  Puissance element: %.0f W", this->element_power_w_);
  ESP_LOGCONFIG(TAG, "  Consigne par defaut: %.1f C", this->target_c_);
  ESP_LOGCONFIG(TAG, "  Bornes: min=%.1f C  max=%.1f C  hyst=%.1f C",
                this->min_temp_c_, this->max_temp_c_, this->hysteresis_c_);
  ESP_LOGCONFIG(TAG, "  Overheat: element=%.1f C  ssr=%.1f C",
                this->overheat_c_, this->ssr_overheat_c_);
  ESP_LOGCONFIG(TAG, "  Pompe: delta_on=%.1f delta_off=%.1f target=%.1f",
                this->pump_delta_on_c_, this->pump_delta_off_c_, this->pump_target_c_);
  ESP_LOGCONFIG(TAG, "  Legionella: target=%.1f C interval=%u h hold=%u min",
                this->legionella_target_c_, (unsigned) this->legionella_interval_h_,
                (unsigned) this->legionella_hold_min_);
  ESP_LOGCONFIG(TAG, "  PV: threshold=%.0f W burst_hyst=%.0f Wh prefer_burst=%s",
                this->pv_surplus_threshold_w_, this->pv_burst_hysteresis_wh_,
                YESNO(this->pv_prefer_burst_));
  ESP_LOGCONFIG(TAG, "  Planificateur: %s wait_peak=%s window=%uh..%uh (pad -%u/+%u)",
                YESNO(this->planner_enabled_), YESNO(this->wait_for_peak_),
                (unsigned) this->peak_window_start_h_,
                (unsigned) this->peak_window_end_h_,
                (unsigned) this->peak_window_pad_before_h_,
                (unsigned) this->peak_window_pad_after_h_);
  ESP_LOGCONFIG(TAG, "    horizon=%u j  conso/j=%.1f kWh  boost_max=+%.1f C  deficit_max=-%.1f C",
                (unsigned) this->horizon_days_, this->daily_consumption_kwh_,
                this->boost_offset_max_c_, this->deficit_offset_max_c_);
  ESP_LOGCONFIG(TAG, "  Etat persiste: enabled=%s boost=%s pv=%s target=%.1f overheat_cnt=%u",
                YESNO(this->enabled_), YESNO(this->boost_), YESNO(this->pv_priority_),
                this->target_c_, (unsigned) this->overheat_count_);
  LOG_UPDATE_INTERVAL(this);
}

// -----------------------------------------------------------------------------
// Machine a etats
// -----------------------------------------------------------------------------

CumulusMode CumulusController::compute_mode_() {
  if (!this->enabled_) {
    return CUMULUS_MODE_OFF;
  }
  if (this->safety_latch_) {
    return CUMULUS_MODE_SAFETY;
  }
  // Watchdog: sondes muettes trop longtemps.
  if (millis() - this->last_valid_sensor_ms_ > this->watchdog_timeout_ms_) {
    return CUMULUS_MODE_IDLE;
  }

  if (this->is_legionella_due_() || this->legionella_pending_) {
    return CUMULUS_MODE_LEGIONELLA;
  }
  if (this->boost_) {
    return CUMULUS_MODE_BOOST;
  }

  const float top = (this->top_temp_sensor_ != nullptr) ? this->top_temp_sensor_->state : NAN;
  const float target = std::isnan(this->effective_target_c_) ? this->target_c_ : this->effective_target_c_;

  // Filet de secours: en dessous du minimum on chauffe quel que soit le mode PV.
  if (!std::isnan(top) && top < this->min_temp_c_ - this->hysteresis_c_) {
    return CUMULUS_MODE_THERMOSTAT;
  }
  // Au-dessus de la consigne + hyst: rien a faire.
  if (!std::isnan(top) && top >= target + this->hysteresis_c_) {
    return CUMULUS_MODE_IDLE;
  }

  const bool pv_available = this->pv_priority_ &&
                            (this->pv_surplus_sensor_ != nullptr ||
                             this->grid_export_sensor_ != nullptr);
  if (!pv_available) {
    return CUMULUS_MODE_THERMOSTAT;
  }

  // Planification fenetre PV: on differe la chauffe hors fenetre, avec catch-up si la nuit approche.
  if (this->planner_enabled_ && this->wait_for_peak_ && !this->is_in_peak_window_()) {
    const time_t now_time = ::time(nullptr);
    struct tm *tm_now = (now_time > 0) ? ::localtime(&now_time) : nullptr;
    const int hour_now = (tm_now != nullptr) ? tm_now->tm_hour : -1;
    const bool past_peak_end = (hour_now >= 0) &&
                               (hour_now >= this->peak_window_end_h_ + this->peak_window_pad_after_h_);
    // Catch-up: soirs, si on est deja sous la consigne de base + hyst, on chauffe au reseau
    // pour eviter d'entamer la nuit sur une reserve insuffisante.
    if (past_peak_end && !std::isnan(top) && top < this->target_c_ - this->hysteresis_c_) {
      return CUMULUS_MODE_THERMOSTAT;
    }
    return CUMULUS_MODE_PV_WAIT;
  }
  return this->pv_prefer_burst_ ? CUMULUS_MODE_PV_ECO : CUMULUS_MODE_PV_MOD;
}

float CumulusController::compute_duty_(CumulusMode mode) {
  switch (mode) {
    case CUMULUS_MODE_OFF:
    case CUMULUS_MODE_IDLE:
    case CUMULUS_MODE_PV_WAIT:
    case CUMULUS_MODE_SAFETY:
      return 0.0f;

    case CUMULUS_MODE_BOOST:
    case CUMULUS_MODE_LEGIONELLA: {
      // Objectif: cible boost (consigne base) ou cible legionella. On sort quand atteint + hold.
      const float top = (this->top_temp_sensor_ != nullptr) ? this->top_temp_sensor_->state : NAN;
      const float goal = (mode == CUMULUS_MODE_LEGIONELLA)
                             ? this->legionella_target_c_
                             : this->target_c_;
      if (!std::isnan(top) && top >= goal) {
        if (mode == CUMULUS_MODE_LEGIONELLA) {
          const uint32_t now = millis();
          if (this->legionella_hold_start_ms_ == 0) {
            this->legionella_hold_start_ms_ = now;
          }
          if (now - this->legionella_hold_start_ms_ >=
              this->legionella_hold_min_ * 60u * 1000u) {
            // Cycle valide: on memorise et on sort.
            const time_t now_time = ::time(nullptr);
            this->last_legionella_unix_ =
                (now_time > 0) ? static_cast<uint64_t>(now_time) : 0ull;
            this->legionella_pending_ = false;
            this->legionella_hold_start_ms_ = 0;
            ESP_LOGI(TAG, "Cycle anti-legionellose termine (top=%.1f C)", top);
          }
          return 1.0f;  // maintien plein regime pendant le hold
        }
        // Boost: on relache automatiquement quand la cible est atteinte.
        this->set_boost(false);
        return 0.0f;
      }
      return 1.0f;
    }

    case CUMULUS_MODE_THERMOSTAT: {
      const float top = (this->top_temp_sensor_ != nullptr) ? this->top_temp_sensor_->state : NAN;
      if (std::isnan(top)) {
        return 0.0f;  // pas de mesure -> pas de chauffe
      }
      // Hysteresis autour de la consigne effective (recouvre les cas catch-up soirs
      // ou l'appelant a deja limite l'ambition a la base target).
      const float goal = std::isnan(this->effective_target_c_) ? this->target_c_ : this->effective_target_c_;
      const bool was_on = this->last_duty_ > 0.5f;
      const float on_threshold = goal - this->hysteresis_c_;
      const float off_threshold = goal;
      if (was_on) {
        return top < off_threshold ? 1.0f : 0.0f;
      }
      return top < on_threshold ? 1.0f : 0.0f;
    }

    case CUMULUS_MODE_PV_MOD: {
      const float surplus = this->get_effective_surplus_w_();
      if (std::isnan(surplus)) return 0.0f;

      // Boucle fermee Linky: si on a la puissance mesuree du cumulus + injection reseau,
      // on cible "conso cumulus = conso cumulus actuelle + surplus - marge", ce qui
      // annule le soutirage tout en gardant une petite marge d'export securite.
      const bool has_measured =
          this->heater_power_measured_sensor_ != nullptr &&
          !std::isnan(this->heater_power_measured_sensor_->state);
      if (has_measured && this->grid_export_sensor_ != nullptr) {
        const float current_heater_w = this->heater_power_measured_sensor_->state;
        const float target_heater_w =
            current_heater_w + surplus - this->grid_import_safety_margin_w_;
        float duty = target_heater_w / this->element_power_w_;
        if (duty > 1.0f) duty = 1.0f;
        if (duty < 0.0f) duty = 0.0f;
        return duty;
      }
      // Boucle ouverte: si surplus < seuil, on ne demarre pas.
      if (surplus < this->pv_surplus_threshold_w_) {
        return 0.0f;
      }
      float duty = surplus / this->element_power_w_;
      if (duty > 1.0f) duty = 1.0f;
      if (duty < 0.0f) duty = 0.0f;
      return duty;
    }

    case CUMULUS_MODE_PV_ECO: {
      // Strategie budget: on n'allume qu'une fois la reserve suffisante pour couvrir
      // la recharge complete jusqu'a effective_target, puis on chauffe 100% en salve.
      const float top = (this->top_temp_sensor_ != nullptr) ? this->top_temp_sensor_->state : NAN;
      if (std::isnan(top)) return 0.0f;

      const float goal = std::isnan(this->effective_target_c_) ? this->target_c_ : this->effective_target_c_;
      const float needed_wh = this->tank_volume_l_ * WATER_CP_J_PER_KG_K *
                              (goal - top) / 3600.0f;
      const bool budget_ok =
          this->pv_budget_wh_ >= (needed_wh + this->pv_burst_hysteresis_wh_);
      const bool already_burning = this->last_duty_ > 0.5f;
      // Une fois demarre, on chauffe jusqu'a la cible (ou budget epuise) pour
      // eviter le hachage. Sinon on attend d'avoir accumule assez.
      if (budget_ok || (already_burning && this->pv_budget_wh_ > 0.0f)) {
        return 1.0f;
      }
      return 0.0f;
    }
  }
  return 0.0f;
}

// -----------------------------------------------------------------------------
// Actionneurs
// -----------------------------------------------------------------------------

void CumulusController::apply_outputs_(float duty) {
  if (duty < 0.0f) duty = 0.0f;
  if (duty > 1.0f) duty = 1.0f;
  if (this->heater_output_ != nullptr) {
    this->heater_output_->set_level(duty);
  }
}

void CumulusController::update_pump_(bool heater_active) {
  if (this->pump_switch_ == nullptr) return;
  const uint32_t now = millis();

  const float outlet = (this->outlet_temp_sensor_ != nullptr)
                           ? this->outlet_temp_sensor_->state : NAN;
  const float top = (this->top_temp_sensor_ != nullptr)
                        ? this->top_temp_sensor_->state : NAN;

  // Sans sonde sortie utilisable, on suit simplement l'etat chauffe.
  bool want_on;
  if (!std::isnan(outlet) && !std::isnan(top)) {
    // Delta ballon <-> sortie: si le tuyau est encore chaud (proche du haut de ballon)
    // et sortie sous cible, on continue de tourner.
    const float delta = top - outlet;
    if (this->pump_on_) {
      want_on = (delta > this->pump_delta_off_c_) && (outlet < this->pump_target_c_ + 5.0f);
    } else {
      want_on = (delta > this->pump_delta_on_c_) && (outlet < this->pump_target_c_);
    }
  } else {
    want_on = heater_active;
  }

  // Anti-court-cycle: interdit un changement d'etat sous pump_min_interval_ms_.
  if (want_on != this->pump_on_ &&
      (now - this->pump_last_change_ms_) < this->pump_min_interval_ms_) {
    return;
  }

  if (want_on != this->pump_on_) {
    this->pump_on_ = want_on;
    this->pump_last_change_ms_ = now;
    if (want_on) {
      this->pump_switch_->turn_on();
    } else {
      this->pump_switch_->turn_off();
    }
  }

  if (this->pump_on_) {
    // Approximation: chaque update() = update_interval, on ajoute la duree.
    // (Le cumul est reset a minuit.)
    // Pas critique -> on incremente en secondes en supposant update_interval ~10s.
    this->pump_run_today_s_ += 10;
  }
}

// -----------------------------------------------------------------------------
// Securite
// -----------------------------------------------------------------------------

bool CumulusController::check_safety_() {
  const float element = (this->element_temp_sensor_ != nullptr)
                            ? this->element_temp_sensor_->state : NAN;
  const float ssr = (this->ssr_temp_sensor_ != nullptr)
                        ? this->ssr_temp_sensor_->state : NAN;
  const float top = (this->top_temp_sensor_ != nullptr)
                        ? this->top_temp_sensor_->state : NAN;

  bool trip = false;
  if (!std::isnan(element) && element > this->overheat_c_) {
    ESP_LOGW(TAG, "Overheat element: %.1f C > %.1f C -> cutoff", element, this->overheat_c_);
    trip = true;
  }
  if (!std::isnan(ssr) && ssr > this->ssr_overheat_c_) {
    ESP_LOGW(TAG, "Overheat SSR/thyristor: %.1f C > %.1f C -> cutoff", ssr, this->ssr_overheat_c_);
    trip = true;
  }
  if (!std::isnan(top) && top > this->max_temp_c_ + 5.0f) {
    ESP_LOGW(TAG, "Top ballon > max+5: %.1f C -> cutoff", top);
    trip = true;
  }

  if (trip && !this->safety_latch_) {
    this->safety_latch_ = true;
    this->overheat_count_++;
    ESP_LOGE(TAG, "SAFETY LATCH engage (n=%u)", (unsigned) this->overheat_count_);
    return true;
  }
  if (this->safety_latch_) {
    // Recovery: toutes les temperatures sont redescendues sous seuil-margin.
    const bool element_ok = std::isnan(element) ||
                            element < (this->overheat_c_ - SAFETY_RECOVERY_MARGIN_C);
    const bool ssr_ok = std::isnan(ssr) ||
                        ssr < (this->ssr_overheat_c_ - SAFETY_RECOVERY_MARGIN_C);
    const bool top_ok = std::isnan(top) ||
                        top < (this->max_temp_c_ - SAFETY_RECOVERY_MARGIN_C / 2.0f);
    if (element_ok && ssr_ok && top_ok) {
      this->safety_latch_ = false;
      ESP_LOGI(TAG, "SAFETY latch relache");
    }
  }
  return this->safety_latch_;
}

// -----------------------------------------------------------------------------
// Anti-legionellose
// -----------------------------------------------------------------------------

bool CumulusController::is_legionella_due_() const {
  const time_t now_time = ::time(nullptr);
  if (now_time <= 0) return false;  // horloge pas encore synchronisee
  const uint64_t now_unix = static_cast<uint64_t>(now_time);
  if (this->last_legionella_unix_ != 0) {
    const uint64_t delta_s = now_unix - this->last_legionella_unix_;
    if (delta_s < this->legionella_interval_h_ * 3600ull) return false;
  }
  // On declenche dans l'heure creuse (3h du matin par convention).
  struct tm *tm_now = ::localtime(&now_time);
  return tm_now != nullptr && tm_now->tm_hour == 3;
}

// -----------------------------------------------------------------------------
// PV budget
// -----------------------------------------------------------------------------

float CumulusController::get_effective_surplus_w_() const {
  // Priorite 1: Linky. SINSTI - SINSTS = surplus net vraiment injecte au reseau.
  if (this->grid_export_sensor_ != nullptr &&
      !std::isnan(this->grid_export_sensor_->state)) {
    float sur = this->grid_export_sensor_->state;
    if (this->grid_import_sensor_ != nullptr &&
        !std::isnan(this->grid_import_sensor_->state)) {
      sur -= this->grid_import_sensor_->state;
    }
    return sur;
  }
  // Priorite 2: sensor externe deja calcule.
  if (this->pv_surplus_sensor_ != nullptr) {
    return this->pv_surplus_sensor_->state;
  }
  return NAN;
}

void CumulusController::accumulate_pv_budget_(float dt_s) {
  const float surplus = this->get_effective_surplus_w_();
  if (std::isnan(surplus) || surplus <= 0.0f) return;
  this->pv_budget_wh_ += surplus * dt_s / 3600.0f;
}

void CumulusController::deduct_pv_budget_(float dt_s, float duty) {
  if (duty <= 0.0f) return;
  // Puissance reellement consommee: mesure si disponible.
  const bool has_measured =
      this->heater_power_measured_sensor_ != nullptr &&
      !std::isnan(this->heater_power_measured_sensor_->state);
  const float instant_power_w = has_measured
                                    ? this->heater_power_measured_sensor_->state
                                    : (this->element_power_w_ * duty);
  const float consumed_wh = instant_power_w * dt_s / 3600.0f;
  // On ne debite le budget que si la chauffe est motivee par le PV (ECO/MOD).
  if (this->current_mode_ == CUMULUS_MODE_PV_ECO ||
      this->current_mode_ == CUMULUS_MODE_PV_MOD) {
    if (this->pv_budget_wh_ >= consumed_wh) {
      this->pv_budget_wh_ -= consumed_wh;
    } else {
      this->pv_budget_wh_ = 0.0f;
    }
    // Fraction du surplus reellement utilisee = min(consomme, surplus dispo).
    const float surplus = this->get_effective_surplus_w_();
    const float used_wh =
        std::fmin(consumed_wh, std::fmax(0.0f, std::isnan(surplus) ? 0.0f : surplus) * dt_s / 3600.0f);
    this->pv_surplus_used_today_wh_ += used_wh;
  }
}

// -----------------------------------------------------------------------------
// Utilitaires
// -----------------------------------------------------------------------------

void CumulusController::handle_daily_rollover_() {
  const time_t now_time = ::time(nullptr);
  if (now_time <= 0) return;
  struct tm *tm_now = ::localtime(&now_time);
  if (tm_now == nullptr) return;
  const uint32_t doy = tm_now->tm_year * 400 + tm_now->tm_yday;
  if (this->last_day_of_year_ == 0) {
    this->last_day_of_year_ = doy;
    return;
  }
  if (doy != this->last_day_of_year_) {
    ESP_LOGI(TAG, "Rollover journalier -> reset compteurs");
    this->last_day_of_year_ = doy;
    this->energy_today_wh_ = 0.0f;
    this->pv_surplus_used_today_wh_ = 0.0f;
    this->pump_run_today_s_ = 0;
    // Note: pv_budget_wh_ n'est PAS reset -- on veut pouvoir chauffer le matin
    // avec le residu de la veille si pas consomme.
  }
}

float CumulusController::estimate_tank_temperature_() const {
  if (this->top_temp_sensor_ != nullptr && !std::isnan(this->top_temp_sensor_->state)) {
    return this->top_temp_sensor_->state;
  }
  return NAN;
}

float CumulusController::estimate_stored_energy_wh_() const {
  const float t = this->estimate_tank_temperature_();
  if (std::isnan(t)) return NAN;
  // E = m * cp * (T - Tref). m en kg = volume L (approx), cp en J/(kg.K).
  const float e_j = this->tank_volume_l_ * WATER_CP_J_PER_KG_K * (t - COLD_WATER_REF_C);
  return e_j / 3600.0f;
}

float CumulusController::estimate_time_to_target_min_(float duty) const {
  if (duty <= 0.01f) return NAN;
  const float t = this->estimate_tank_temperature_();
  const float goal = std::isnan(this->effective_target_c_) ? this->target_c_ : this->effective_target_c_;
  if (std::isnan(t) || t >= goal) return 0.0f;
  const float delta = goal - t;
  const float e_needed_wh =
      this->tank_volume_l_ * WATER_CP_J_PER_KG_K * delta / 3600.0f;
  const float p_now = duty * this->element_power_w_;
  if (p_now <= 0.0f) return NAN;
  return (e_needed_wh / p_now) * 60.0f;
}

void CumulusController::sample_duty_history_(float duty, float dt_s) {
  this->duty_history_accum_ += duty * dt_s;
  const uint32_t now = millis();
  const uint32_t bucket_ms = 60u * 1000u;
  while (now - this->duty_history_last_bucket_ms_ >= bucket_ms) {
    const float bucket_dt = bucket_ms / 1000.0f;
    const float avg = this->duty_history_accum_ / bucket_dt;
    this->duty_history_[this->duty_history_index_] = std::fmin(avg, 1.0f);
    this->duty_history_index_ = (this->duty_history_index_ + 1) % DUTY_HISTORY_SIZE;
    this->duty_history_accum_ = 0.0f;
    this->duty_history_last_bucket_ms_ += bucket_ms;
  }
  if (this->duty_cycle_1h_sensor_ != nullptr) {
    float sum = 0.0f;
    for (float v : this->duty_history_) sum += v;
    this->duty_cycle_1h_sensor_->publish_state(100.0f * sum / DUTY_HISTORY_SIZE);
  }
}

// -----------------------------------------------------------------------------
// Planificateur multi-jours
// -----------------------------------------------------------------------------

float CumulusController::compute_horizon_available_kwh_() const {
  float total = 0.0f;
  // Aujourd'hui restant: PV_forecast - conso_forecast (si dispo). Sinon PV brute.
  if (this->pv_forecast_sensor_ != nullptr &&
      !std::isnan(this->pv_forecast_sensor_->state)) {
    float today = this->pv_forecast_sensor_->state;
    if (this->consumption_forecast_sensor_ != nullptr &&
        !std::isnan(this->consumption_forecast_sensor_->state)) {
      today -= this->consumption_forecast_sensor_->state;
    }
    total += std::fmax(today, 0.0f);
  }
  // J+1 et J+2: on ne connait typiquement que la production. On approxime la conso
  // par daily_consumption_kwh_ (conso maison globale du chauffe-eau + reste ballon).
  auto add_day = [&](sensor::Sensor *s) {
    if (s == nullptr || std::isnan(s->state)) return;
    const float prod = s->state;
    const float cons = this->daily_consumption_kwh_;
    total += std::fmax(prod - cons, 0.0f);
  };
  if (this->horizon_days_ >= 2) add_day(this->pv_forecast_d1_sensor_);
  if (this->horizon_days_ >= 3) add_day(this->pv_forecast_d2_sensor_);
  return total;
}

void CumulusController::recompute_effective_target_() {
  // Base = consigne utilisateur. Toujours definie meme sans planificateur.
  float base = this->target_c_;
  this->dynamic_offset_c_ = 0.0f;

  if (!this->planner_enabled_) {
    this->effective_target_c_ = base;
    return;
  }
  // Sans aucune donnee forecast on ne peut rien planifier.
  const bool has_any_forecast =
      (this->pv_forecast_sensor_ != nullptr && !std::isnan(this->pv_forecast_sensor_->state)) ||
      (this->pv_forecast_d1_sensor_ != nullptr && !std::isnan(this->pv_forecast_d1_sensor_->state)) ||
      (this->pv_forecast_d2_sensor_ != nullptr && !std::isnan(this->pv_forecast_d2_sensor_->state));
  if (!has_any_forecast) {
    this->effective_target_c_ = base;
    return;
  }

  const float needed = this->daily_consumption_kwh_ * this->horizon_days_;
  const float available = this->compute_horizon_available_kwh_();
  if (needed <= 0.1f) {
    this->effective_target_c_ = base;
    return;
  }
  const float ratio = available / needed;

  float offset;
  if (ratio <= 1.0f) {
    // Deficit: on stocke plus, borne a boost_offset_max_c_.
    offset = this->boost_offset_max_c_ * (1.0f - ratio);
    if (offset > this->boost_offset_max_c_) offset = this->boost_offset_max_c_;
  } else {
    // Surplus: on relaxe, borne a deficit_offset_max_c_ (valeur positive -> devient offset negatif).
    const float surplus_ratio = std::fmin(ratio - 1.0f, 1.0f);
    offset = -this->deficit_offset_max_c_ * surplus_ratio;
  }
  this->dynamic_offset_c_ = offset;
  float eff = base + offset;
  if (eff < this->min_temp_c_) eff = this->min_temp_c_;
  if (eff > this->max_temp_c_) eff = this->max_temp_c_;
  this->effective_target_c_ = eff;
}

bool CumulusController::is_in_peak_window_() const {
  const time_t now_time = ::time(nullptr);
  if (now_time <= 0) return true;  // sans horloge on n'entrave pas la chauffe
  struct tm *tm_now = ::localtime(&now_time);
  if (tm_now == nullptr) return true;
  const int h = tm_now->tm_hour;

  int start = this->peak_window_start_h_;
  int end = this->peak_window_end_h_;
  // Si un sensor "heure de pic" est cable, on centre la fenetre autour.
  if (this->peak_hour_sensor_ != nullptr && !std::isnan(this->peak_hour_sensor_->state)) {
    const int peak = static_cast<int>(std::round(this->peak_hour_sensor_->state));
    start = peak - this->peak_window_pad_before_h_;
    end = peak + this->peak_window_pad_after_h_;
  }
  if (start < 0) start = 0;
  if (end > 24) end = 24;
  if (start <= end) {
    return h >= start && h < end;
  }
  // Wraparound (jamais realiste pour le solaire mais on gere proprement).
  return h >= start || h < end;
}

// -----------------------------------------------------------------------------
// Commandes (switches / numbers)
// -----------------------------------------------------------------------------

void CumulusController::set_enabled(bool v) {
  if (v == this->enabled_) return;
  this->enabled_ = v;
  ESP_LOGI(TAG, "enabled=%s", YESNO(v));
  if (!v) {
    this->apply_outputs_(0.0f);
  }
  this->save_state_throttled_();
}

void CumulusController::set_boost(bool v) {
  if (v == this->boost_) return;
  this->boost_ = v;
  ESP_LOGI(TAG, "boost=%s", YESNO(v));
  this->save_state_throttled_();
}

void CumulusController::set_pv_priority(bool v) {
  if (v == this->pv_priority_) return;
  this->pv_priority_ = v;
  ESP_LOGI(TAG, "pv_priority=%s", YESNO(v));
  this->save_state_throttled_();
}

void CumulusController::trigger_legionella_now() {
  ESP_LOGI(TAG, "Cycle legionellose declenche manuellement");
  this->legionella_pending_ = true;
  this->legionella_hold_start_ms_ = 0;
}

void CumulusController::set_target_temperature_c(float t) {
  if (t < this->min_temp_c_) t = this->min_temp_c_;
  if (t > this->max_temp_c_) t = this->max_temp_c_;
  if (std::abs(t - this->target_c_) < 0.01f) return;
  this->target_c_ = t;
  ESP_LOGI(TAG, "Consigne modifiee: %.1f C", t);
  this->save_state_throttled_();
}

void CumulusController::set_pv_surplus_threshold_runtime_w(float w) {
  if (w < 0.0f) w = 0.0f;
  if (std::abs(w - this->pv_surplus_threshold_w_) < 0.5f) return;
  this->pv_surplus_threshold_w_ = w;
  ESP_LOGI(TAG, "Seuil surplus PV modifie: %.0f W", w);
  this->save_state_throttled_();
}

// -----------------------------------------------------------------------------
// Persistance flash
// -----------------------------------------------------------------------------

void CumulusController::save_state_() {
  PersistedState s{};
  s.target_c = this->target_c_;
  s.pv_surplus_threshold_w = this->pv_surplus_threshold_w_;
  s.enabled = this->enabled_ ? 1 : 0;
  s.boost = this->boost_ ? 1 : 0;
  s.pv_priority = this->pv_priority_ ? 1 : 0;
  s.reserved = 0;
  s.overheat_count = this->overheat_count_;
  s.last_legionella_unix = this->last_legionella_unix_;
  s.energy_today_wh = this->energy_today_wh_;
  s.pv_budget_wh = this->pv_budget_wh_;
  s.pv_surplus_used_today_wh = this->pv_surplus_used_today_wh_;
  s.last_day_of_year = this->last_day_of_year_;
  this->pref_state_.save(&s);
}

void CumulusController::save_state_throttled_() {
  const uint32_t now = millis();
  if (now - this->last_pref_save_ms_ < PREF_SAVE_INTERVAL_MS) return;
  this->save_state_();
  this->last_pref_save_ms_ = now;
}

void CumulusController::load_state_() {
  PersistedState s{};
  if (!this->pref_state_.load(&s)) {
    return;
  }
  if (s.target_c >= 20.0f && s.target_c <= 90.0f) this->target_c_ = s.target_c;
  if (s.pv_surplus_threshold_w >= 0.0f && s.pv_surplus_threshold_w < 5000.0f) {
    this->pv_surplus_threshold_w_ = s.pv_surplus_threshold_w;
  }
  this->enabled_ = s.enabled != 0;
  this->boost_ = s.boost != 0;
  this->pv_priority_ = s.pv_priority != 0;
  this->overheat_count_ = s.overheat_count;
  this->last_legionella_unix_ = s.last_legionella_unix;
  this->energy_today_wh_ = s.energy_today_wh;
  this->pv_budget_wh_ = s.pv_budget_wh;
  this->pv_surplus_used_today_wh_ = s.pv_surplus_used_today_wh;
  this->last_day_of_year_ = s.last_day_of_year;
  ESP_LOGI(TAG, "Etat restaure depuis flash: target=%.1f enabled=%s pv=%s",
           this->target_c_, YESNO(this->enabled_), YESNO(this->pv_priority_));
}

// -----------------------------------------------------------------------------
// Switches enfants
// -----------------------------------------------------------------------------

void CumulusEnableSwitch::setup() {
  if (this->parent_ != nullptr) {
    this->publish_state(this->parent_->is_enabled());
  }
}
void CumulusEnableSwitch::write_state(bool state) {
  if (this->parent_ != nullptr) this->parent_->set_enabled(state);
  this->publish_state(state);
}

void CumulusBoostSwitch::setup() {
  if (this->parent_ != nullptr) {
    this->publish_state(this->parent_->is_boost());
  }
}
void CumulusBoostSwitch::write_state(bool state) {
  if (this->parent_ != nullptr) this->parent_->set_boost(state);
  this->publish_state(state);
}

void CumulusPvPrioritySwitch::setup() {
  if (this->parent_ != nullptr) {
    this->publish_state(this->parent_->is_pv_priority());
  }
}
void CumulusPvPrioritySwitch::write_state(bool state) {
  if (this->parent_ != nullptr) this->parent_->set_pv_priority(state);
  this->publish_state(state);
}

void CumulusLegionellaNowSwitch::write_state(bool state) {
  if (state && this->parent_ != nullptr) {
    this->parent_->trigger_legionella_now();
  }
  // Momentary: retombe a OFF immediatement.
  this->publish_state(false);
}

}  // namespace cumulus
}  // namespace esphome
