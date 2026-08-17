#pragma once

#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace cumulus {

// Etat de la machine a etats principale. L'ordre est utilise comme priorite:
// SAFETY (le plus prioritaire) > LEGIONELLA > BOOST > PV_ECO/PV_MOD > THERMOSTAT > IDLE > PV_WAIT > OFF.
// PV_WAIT: on differe la chauffe en attendant la fenetre PV optimale.
enum CumulusMode : uint8_t {
  CUMULUS_MODE_OFF = 0,
  CUMULUS_MODE_IDLE,
  CUMULUS_MODE_PV_WAIT,
  CUMULUS_MODE_THERMOSTAT,
  CUMULUS_MODE_PV_MOD,
  CUMULUS_MODE_PV_ECO,
  CUMULUS_MODE_BOOST,
  CUMULUS_MODE_LEGIONELLA,
  CUMULUS_MODE_SAFETY,
};

const char *cumulus_mode_to_string(CumulusMode mode);

class CumulusController : public PollingComponent {
 public:
  // ---- Sorties pilotees ----
  // Sortie flottante 0..1 : c'est l'utilisateur qui choisit dans le YAML
  // (slow_pwm pour SSR zero-cross, ledc/PWM pour triac, esp32_dac pour SSR proportionnel, etc.).
  void set_heater_output(output::FloatOutput *o) { this->heater_output_ = o; }
  // Pompe de circulation optionnelle (relais GPIO).
  void set_pump_switch(switch_::Switch *s) { this->pump_switch_ = s; }

  // ---- Sondes NTC (toutes optionnelles) ----
  void set_top_temp_sensor(sensor::Sensor *s) { this->top_temp_sensor_ = s; }
  void set_element_temp_sensor(sensor::Sensor *s) { this->element_temp_sensor_ = s; }
  void set_outlet_temp_sensor(sensor::Sensor *s) { this->outlet_temp_sensor_ = s; }
  void set_ssr_temp_sensor(sensor::Sensor *s) { this->ssr_temp_sensor_ = s; }

  // ---- Entrees Home Assistant (optionnelles) ----
  void set_pv_surplus_sensor(sensor::Sensor *s) { this->pv_surplus_sensor_ = s; }
  void set_pv_forecast_sensor(sensor::Sensor *s) { this->pv_forecast_sensor_ = s; }
  // Previsions J+1 / J+2 (kWh totales de la journee) pour la planification multi-jours.
  void set_pv_forecast_d1_sensor(sensor::Sensor *s) { this->pv_forecast_d1_sensor_ = s; }
  void set_pv_forecast_d2_sensor(sensor::Sensor *s) { this->pv_forecast_d2_sensor_ = s; }
  // Conso maison prevue restante aujourd'hui (kWh), soustraite du forecast pour obtenir le surplus net.
  void set_consumption_forecast_sensor(sensor::Sensor *s) { this->consumption_forecast_sensor_ = s; }
  // Heure (0..23) du pic de surplus PV attendu aujourd'hui (optionnel; sinon on utilise la fenetre fixe).
  void set_peak_hour_sensor(sensor::Sensor *s) { this->peak_hour_sensor_ = s; }
  // Puissance instantanee reellement consommee par le cumulus (W). Idealement une CT clamp dediee.
  void set_heater_power_measured_sensor(sensor::Sensor *s) { this->heater_power_measured_sensor_ = s; }
  // Injection reseau Linky (W). SINSTI, positif quand on exporte du surplus PV.
  void set_grid_export_sensor(sensor::Sensor *s) { this->grid_export_sensor_ = s; }
  // Soutirage reseau Linky (W). SINSTS, positif quand on tire du reseau.
  void set_grid_import_sensor(sensor::Sensor *s) { this->grid_import_sensor_ = s; }

  // ---- Configuration fixe (YAML) ----
  void set_tank_volume_l(float v) { this->tank_volume_l_ = v; }
  void set_element_power_w(float w) { this->element_power_w_ = w; }
  void set_default_target_c(float t) { this->target_c_ = t; }
  void set_min_temperature_c(float t) { this->min_temp_c_ = t; }
  void set_max_temperature_c(float t) { this->max_temp_c_ = t; }
  void set_hysteresis_c(float h) { this->hysteresis_c_ = h; }
  void set_overheat_temperature_c(float t) { this->overheat_c_ = t; }
  void set_ssr_overheat_c(float t) { this->ssr_overheat_c_ = t; }
  void set_pump_delta_on_c(float d) { this->pump_delta_on_c_ = d; }
  void set_pump_delta_off_c(float d) { this->pump_delta_off_c_ = d; }
  void set_pump_target_c(float t) { this->pump_target_c_ = t; }
  void set_pump_min_interval_ms(uint32_t v) { this->pump_min_interval_ms_ = v; }
  void set_legionella_target_c(float t) { this->legionella_target_c_ = t; }
  void set_legionella_interval_h(uint32_t h) { this->legionella_interval_h_ = h; }
  void set_legionella_hold_min(uint32_t m) { this->legionella_hold_min_ = m; }
  void set_pv_surplus_threshold_w(float w) { this->pv_surplus_threshold_w_ = w; }
  void set_pv_burst_hysteresis_wh(float wh) { this->pv_burst_hysteresis_wh_ = wh; }
  void set_pv_prefer_burst(bool b) { this->pv_prefer_burst_ = b; }
  // Marge de securite export (W) pour la modulation Linky en boucle fermee:
  // on cible SINSTI = margin > 0 pour ne jamais dipoter dans le reseau.
  void set_grid_import_safety_margin_w(float w) { this->grid_import_safety_margin_w_ = w; }
  // ---- Planificateur multi-jours ----
  void set_planner_enabled(bool b) { this->planner_enabled_ = b; }
  void set_wait_for_peak(bool b) { this->wait_for_peak_ = b; }
  void set_peak_window_start_h(uint8_t h) { this->peak_window_start_h_ = h; }
  void set_peak_window_end_h(uint8_t h) { this->peak_window_end_h_ = h; }
  void set_peak_window_pad_before_h(uint8_t h) { this->peak_window_pad_before_h_ = h; }
  void set_peak_window_pad_after_h(uint8_t h) { this->peak_window_pad_after_h_ = h; }
  void set_daily_consumption_kwh(float v) { this->daily_consumption_kwh_ = v; }
  void set_horizon_days(uint8_t d) { this->horizon_days_ = d; }
  void set_boost_offset_max_c(float v) { this->boost_offset_max_c_ = v; }
  void set_deficit_offset_max_c(float v) { this->deficit_offset_max_c_ = v; }
  void set_watchdog_timeout_ms(uint32_t v) { this->watchdog_timeout_ms_ = v; }

  // ---- Commandes internes (switches enfants) ----
  // Appelees par les Switch generees dans switch.py.
  void set_enabled(bool v);
  void set_boost(bool v);
  void set_pv_priority(bool v);
  void trigger_legionella_now();

  bool is_enabled() const { return this->enabled_; }
  bool is_boost() const { return this->boost_; }
  bool is_pv_priority() const { return this->pv_priority_; }

  // ---- Numbers exposes (setters "runtime") ----
  void set_target_temperature_c(float t);
  void set_pv_surplus_threshold_runtime_w(float w);
  float get_target_temperature_c() const { return this->target_c_; }
  float get_pv_surplus_threshold_w() const { return this->pv_surplus_threshold_w_; }
  // Consigne effective apres offset planificateur (bornee [min, max]).
  float get_effective_target_c() const { return this->effective_target_c_; }

  // ---- Telemetrie exposee ----
  void set_heater_level_sensor(sensor::Sensor *s) { this->heater_level_sensor_ = s; }
  void set_heater_power_sensor(sensor::Sensor *s) { this->heater_power_sensor_ = s; }
  void set_stored_energy_sensor(sensor::Sensor *s) { this->stored_energy_sensor_ = s; }
  void set_energy_today_sensor(sensor::Sensor *s) { this->energy_today_sensor_ = s; }
  void set_pv_budget_sensor(sensor::Sensor *s) { this->pv_budget_sensor_ = s; }
  void set_pv_surplus_used_sensor(sensor::Sensor *s) { this->pv_surplus_used_sensor_ = s; }
  void set_time_to_target_sensor(sensor::Sensor *s) { this->time_to_target_sensor_ = s; }
  void set_duty_cycle_1h_sensor(sensor::Sensor *s) { this->duty_cycle_1h_sensor_ = s; }
  void set_standby_loss_sensor(sensor::Sensor *s) { this->standby_loss_sensor_ = s; }
  void set_days_since_legionella_sensor(sensor::Sensor *s) { this->days_since_legionella_sensor_ = s; }
  void set_overheat_count_sensor(sensor::Sensor *s) { this->overheat_count_sensor_ = s; }
  void set_pump_duty_sensor(sensor::Sensor *s) { this->pump_duty_sensor_ = s; }
  void set_effective_target_sensor(sensor::Sensor *s) { this->effective_target_sensor_ = s; }
  void set_dynamic_offset_sensor(sensor::Sensor *s) { this->dynamic_offset_sensor_ = s; }
  void set_energy_deficit_forecast_sensor(sensor::Sensor *s) { this->energy_deficit_forecast_sensor_ = s; }
  void set_horizon_available_sensor(sensor::Sensor *s) { this->horizon_available_sensor_ = s; }
  void set_peak_window_active_sensor(sensor::Sensor *s) { this->peak_window_active_sensor_ = s; }
  void set_element_health_sensor(sensor::Sensor *s) { this->element_health_sensor_ = s; }
  void set_mode_text_sensor(text_sensor::TextSensor *s) { this->mode_text_sensor_ = s; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  // Mise a jour de la sortie physique (heater + pompe) selon le mode courant.
  void apply_outputs_(float duty);
  // Recompute la MAE en fonction des entrees courantes.
  CumulusMode compute_mode_();
  // Retourne le duty cible 0..1 pour le mode donne.
  float compute_duty_(CumulusMode mode);
  // Gestion de la pompe (hysteresis + anti-court-cycle).
  void update_pump_(bool heater_active);
  // Verifie les seuils de securite. Retourne true si un cutoff doit etre force.
  bool check_safety_();
  // Sequence anti-legionellose: doit-on la declencher maintenant ?
  bool is_legionella_due_() const;
  // Accumulation du budget PV (integree sur dt).
  void accumulate_pv_budget_(float dt_s);
  // Consommation de la reserve PV a partir du duty courant.
  void deduct_pv_budget_(float dt_s, float duty);
  // Reset des compteurs journaliers a minuit.
  void handle_daily_rollover_();
  // Estimation temperature moyenne du ballon (fallback si top_temp unique).
  float estimate_tank_temperature_() const;
  // Energie stockee (Wh) = m * cp * (T - Tref) avec Tref = 15 C (eau froide reseau).
  float estimate_stored_energy_wh_() const;
  // Estimation du "temps jusqu'a la cible" en minutes au duty courant.
  float estimate_time_to_target_min_(float duty) const;
  // Sample duty et update duty_cycle_1h.
  void sample_duty_history_(float duty, float dt_s);
  // Retourne la meilleure estimation du surplus PV (W) disponible parmi les sources cablees:
  // 1) Linky SINSTI - SINSTS (le plus precis), 2) SINSTI seul, 3) pv_surplus_sensor_ (fallback).
  float get_effective_surplus_w_() const;
  // Recalcule la consigne effective (base + offset planificateur), bornee [min, max].
  void recompute_effective_target_();
  // Retourne true si on est actuellement dans la fenetre de production PV utile.
  bool is_in_peak_window_() const;
  // Somme kWh disponibles sur l'horizon (aujourd'hui restant + J+1 + ... jusqu'a horizon_days_).
  float compute_horizon_available_kwh_() const;
  // Persistance flash.
  void save_state_();
  void load_state_();

  // ---- Sorties ----
  output::FloatOutput *heater_output_{nullptr};
  switch_::Switch *pump_switch_{nullptr};

  // ---- Sondes ----
  sensor::Sensor *top_temp_sensor_{nullptr};
  sensor::Sensor *element_temp_sensor_{nullptr};
  sensor::Sensor *outlet_temp_sensor_{nullptr};
  sensor::Sensor *ssr_temp_sensor_{nullptr};
  sensor::Sensor *pv_surplus_sensor_{nullptr};
  sensor::Sensor *pv_forecast_sensor_{nullptr};
  sensor::Sensor *pv_forecast_d1_sensor_{nullptr};
  sensor::Sensor *pv_forecast_d2_sensor_{nullptr};
  sensor::Sensor *consumption_forecast_sensor_{nullptr};
  sensor::Sensor *peak_hour_sensor_{nullptr};
  sensor::Sensor *heater_power_measured_sensor_{nullptr};
  sensor::Sensor *grid_export_sensor_{nullptr};
  sensor::Sensor *grid_import_sensor_{nullptr};

  // ---- Telemetrie ----
  sensor::Sensor *heater_level_sensor_{nullptr};
  sensor::Sensor *heater_power_sensor_{nullptr};
  sensor::Sensor *stored_energy_sensor_{nullptr};
  sensor::Sensor *energy_today_sensor_{nullptr};
  sensor::Sensor *pv_budget_sensor_{nullptr};
  sensor::Sensor *pv_surplus_used_sensor_{nullptr};
  sensor::Sensor *time_to_target_sensor_{nullptr};
  sensor::Sensor *duty_cycle_1h_sensor_{nullptr};
  sensor::Sensor *standby_loss_sensor_{nullptr};
  sensor::Sensor *days_since_legionella_sensor_{nullptr};
  sensor::Sensor *overheat_count_sensor_{nullptr};
  sensor::Sensor *pump_duty_sensor_{nullptr};
  sensor::Sensor *effective_target_sensor_{nullptr};
  sensor::Sensor *dynamic_offset_sensor_{nullptr};
  sensor::Sensor *energy_deficit_forecast_sensor_{nullptr};
  sensor::Sensor *horizon_available_sensor_{nullptr};
  sensor::Sensor *peak_window_active_sensor_{nullptr};
  sensor::Sensor *element_health_sensor_{nullptr};
  text_sensor::TextSensor *mode_text_sensor_{nullptr};

  // ---- Configuration ----
  float tank_volume_l_{200.0f};
  float element_power_w_{2400.0f};
  float target_c_{55.0f};
  float min_temp_c_{40.0f};
  float max_temp_c_{65.0f};
  float hysteresis_c_{4.0f};
  float overheat_c_{85.0f};
  float ssr_overheat_c_{80.0f};
  float pump_delta_on_c_{3.0f};
  float pump_delta_off_c_{1.0f};
  float pump_target_c_{45.0f};
  uint32_t pump_min_interval_ms_{5u * 60u * 1000u};
  float legionella_target_c_{65.0f};
  uint32_t legionella_interval_h_{7u * 24u};
  uint32_t legionella_hold_min_{30u};
  float pv_surplus_threshold_w_{100.0f};
  // Marge sur le budget PV pour lancer une salve ECO: on attend
  // (Wh necessaires + pv_burst_hysteresis_wh_) avant de demarrer.
  float pv_burst_hysteresis_wh_{100.0f};
  bool pv_prefer_burst_{true};
  // Cible d'export minimum (W) pour eviter les micro-soutirages en boucle fermee Linky.
  float grid_import_safety_margin_w_{50.0f};
  // ---- Planificateur multi-jours ----
  bool planner_enabled_{true};
  bool wait_for_peak_{true};
  uint8_t peak_window_start_h_{10};
  uint8_t peak_window_end_h_{16};
  uint8_t peak_window_pad_before_h_{1};
  uint8_t peak_window_pad_after_h_{2};
  float daily_consumption_kwh_{8.0f};
  uint8_t horizon_days_{2};
  // Offsets max sur la consigne (deg C). Positif quand la reserve PV est jugee insuffisante,
  // negatif quand elle est abondante.
  float boost_offset_max_c_{8.0f};
  float deficit_offset_max_c_{5.0f};
  uint32_t watchdog_timeout_ms_{5u * 60u * 1000u};

  // ---- Etat mode / commandes ----
  bool enabled_{true};
  bool boost_{false};
  bool pv_priority_{true};
  bool legionella_pending_{false};
  bool safety_latch_{false};
  uint32_t legionella_hold_start_ms_{0};

  CumulusMode current_mode_{CUMULUS_MODE_IDLE};
  float last_duty_{0.0f};
  // Consigne effective courante (recalculee a chaque update).
  float effective_target_c_{NAN};
  float dynamic_offset_c_{0.0f};
  // Sante resistance = P_mesure / (duty * P_nom) * 100, EMA lissee.
  float element_health_pct_{NAN};

  // ---- Historique duty (fenetre glissante 1h, 60 buckets d'une minute) ----
  static constexpr size_t DUTY_HISTORY_SIZE = 60;
  float duty_history_[DUTY_HISTORY_SIZE]{};
  size_t duty_history_index_{0};
  float duty_history_accum_{0.0f};
  uint32_t duty_history_last_bucket_ms_{0};

  // ---- Compteurs journaliers ----
  float energy_today_wh_{0.0f};
  float pv_budget_wh_{0.0f};
  float pv_surplus_used_today_wh_{0.0f};
  uint32_t pump_run_today_s_{0};
  uint32_t last_day_of_year_{0};

  // ---- Etat pompe ----
  bool pump_on_{false};
  uint32_t pump_last_change_ms_{0};

  // ---- Watchdog sondes ----
  uint32_t last_valid_sensor_ms_{0};

  // ---- Suivi standby losses ----
  float standby_last_top_c_{NAN};
  uint32_t standby_last_top_ms_{0};

  // ---- Timing update() ----
  uint32_t last_update_ms_{0};

  // ---- Compteur overheat + timestamp legionella ----
  uint32_t overheat_count_{0};
  uint64_t last_legionella_unix_{0};  // 0 = jamais fait

  // ---- Persistance flash ----
  struct PersistedState {
    float target_c;
    float pv_surplus_threshold_w;
    uint8_t enabled;
    uint8_t boost;
    uint8_t pv_priority;
    uint8_t reserved;
    uint32_t overheat_count;
    uint64_t last_legionella_unix;
    float energy_today_wh;
    float pv_budget_wh;
    float pv_surplus_used_today_wh;
    uint32_t last_day_of_year;
  } __attribute__((packed));
  ESPPreferenceObject pref_state_;
  uint32_t last_pref_save_ms_{0};
  void save_state_throttled_();
};

// -----------------------------------------------------------------------------
// Switches enfants: fines interfaces qui delegent au controlleur parent.
// -----------------------------------------------------------------------------
class CumulusEnableSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(CumulusController *p) { this->parent_ = p; }
  void setup() override;
  void write_state(bool state) override;

 protected:
  CumulusController *parent_{nullptr};
};

class CumulusBoostSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(CumulusController *p) { this->parent_ = p; }
  void setup() override;
  void write_state(bool state) override;

 protected:
  CumulusController *parent_{nullptr};
};

class CumulusPvPrioritySwitch : public switch_::Switch, public Component {
 public:
  void set_parent(CumulusController *p) { this->parent_ = p; }
  void setup() override;
  void write_state(bool state) override;

 protected:
  CumulusController *parent_{nullptr};
};

// Momentary: passe a ON pour declencher un cycle, puis retombe a OFF automatiquement.
class CumulusLegionellaNowSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(CumulusController *p) { this->parent_ = p; }
  void write_state(bool state) override;

 protected:
  CumulusController *parent_{nullptr};
};

}  // namespace cumulus
}  // namespace esphome
