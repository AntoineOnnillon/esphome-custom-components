#include "atlantic_climate.h"

#include <cmath>

#include "esphome/core/log.h"

namespace esphome {
namespace atlantic_climate {

static const char *const TAG = "atlantic_climate";

// -------- Utilitaires CRC / bit inversion --------

uint16_t AtlanticClimate::crc16_(const uint8_t *data, uint8_t data_len, uint16_t seed) {
  uint32_t crc = seed;
  for (; data_len > 0; data_len--) {
    crc ^= static_cast<uint32_t>(*data++) << 8;
    for (uint8_t i = 0; i < 8; i++) {
      crc <<= 1;
      if (crc & 0x10000)
        crc = (crc ^ POLY_XMODEM) & 0xFFFF;
    }
  }
  return static_cast<uint16_t>(crc);
}

void AtlanticClimate::invert_(uint8_t *data, uint8_t data_len) {
  for (uint8_t i = 0; i < data_len; i++)
    data[i] = static_cast<uint8_t>(~data[i]);
}

// -------- Construction de trame --------

uint8_t AtlanticClimate::build_frame_(uint8_t *dst, const uint8_t *payload, uint8_t payload_len) {
  uint8_t idx = 0;
  dst[idx++] = 0xDC;                                // Start
  dst[idx++] = 0x80 | this->address_;               // Sender
  dst[idx++] = 0x00;                                // Dest = maitre
  dst[idx++] = static_cast<uint8_t>(payload_len + 6);  // Longueur totale incluant CRC
  for (uint8_t i = 0; i < payload_len; i++)
    dst[idx++] = payload[i];
  uint16_t crc = crc16_(dst, idx);
  dst[idx++] = (crc >> 8) & 0xFF;
  dst[idx++] = crc & 0xFF;
  return idx;
}

void AtlanticClimate::send_pending_frame_() {
  if (this->tx_payload_len_ == 0)
    return;
  uint8_t buf[BUFFER_SIZE]{};
  uint8_t len = this->build_frame_(buf, this->tx_payload_, this->tx_payload_len_);
  this->invert_(buf, len);
  this->write_array(buf, len);
  this->flush();
    ESP_LOGV(TAG, "Trame emise, %u octets", len);
  this->tx_payload_len_ = 0;
}

// -------- Encodage des payloads --------

void AtlanticClimate::encode_temperature_frame_(float temperature) {
  uint16_t raw = static_cast<uint16_t>(temperature * 2.0f) << 5;
  uint8_t *p = this->tx_payload_;
  uint8_t i = 0;
  p[i++] = 0x03;
  p[i++] = 0x3D;
  p[i++] = 0x2D;
  p[i++] = 0x05;
  p[i++] = 0x8E;
  p[i++] = 0x01;
  p[i++] = (raw >> 8) & 0xFF;
  p[i++] = raw & 0xFF;
  this->tx_payload_len_ = i;
}

void AtlanticClimate::probe_register_(uint8_t target, uint16_t reg) {
  uint8_t *p = this->tx_payload_;
  uint8_t i = 0;
  p[i++] = 0x06;
  p[i++] = 0x3D;
  p[i++] = target;
  p[i++] = (reg >> 8) & 0xFF;
  p[i++] = reg & 0xFF;
  this->tx_payload_len_ = i;
  this->send_pending_frame_();
  ESP_LOGV(TAG, "Probe envoye: target=0x%02X reg=0x%04X", target, reg);
}

void AtlanticClimate::request_setpoint_() {
  this->probe_register_(0x2D, 0x058E);
}

void AtlanticClimate::request_ambient_temperature_() {
  this->probe_register_(0x2D, 0x051E);
}

void AtlanticClimate::encode_mode_frame_(climate::ClimateMode mode, climate::ClimatePreset preset) {
  // Determine preset effectif suivant le mode, en s'appuyant sur la derniere valeur connue
  // pour eviter d'envoyer des trames incompletes (bug historique quand HEAT + preset=NONE
  // sans que manual_preset_ n'ait jamais ete initialise cote hardware).
  climate::ClimatePreset effective = preset;
  if (mode == climate::CLIMATE_MODE_HEAT) {
    if (preset == climate::CLIMATE_PRESET_NONE)
      effective = this->manual_preset_;
    else
      this->manual_preset_ = preset;
  } else if (mode == climate::CLIMATE_MODE_AUTO) {
    if (preset == climate::CLIMATE_PRESET_NONE)
      effective = this->auto_preset_;
    else
      this->auto_preset_ = preset;
  }

  uint8_t *p = this->tx_payload_;
  uint8_t i = 0;
  p[i++] = 0x03;
  p[i++] = 0x3D;
  p[i++] = 0x2D;
  p[i++] = 0x05;
  // 0x74 en mode manuel/off, 0x72 en mode auto.
  p[i++] = (mode == climate::CLIMATE_MODE_AUTO) ? 0x72 : 0x74;
  p[i++] = 0x01;

  uint8_t code = 0x00;
  switch (mode) {
    case climate::CLIMATE_MODE_OFF:
      code = 0x00;
      break;
    case climate::CLIMATE_MODE_HEAT:
      code = (effective == climate::CLIMATE_PRESET_COMFORT) ? 0x03 : 0x02;
      break;
    case climate::CLIMATE_MODE_AUTO:
      code = (effective == climate::CLIMATE_PRESET_COMFORT) ? 0x02 : 0x01;
      break;
    default:
      ESP_LOGW(TAG, "Mode non supporte demande: %d", static_cast<int>(mode));
      this->tx_payload_len_ = 0;
      return;
  }
  p[i++] = code;
  this->tx_payload_len_ = i;
}

// -------- Reception non bloquante --------

void AtlanticClimate::feed_rx_byte_(uint8_t raw) {
  switch (this->rx_state_) {
    case RX_IDLE:
      // 0x23 = ~0xDC : debut de trame apres inversion cablage.
      if (raw != 0x23)
        return;
      this->rx_buffer_[0] = raw;
      this->rx_len_ = 1;
      this->rx_state_ = RX_HEADER;
      return;

    case RX_HEADER:
      this->rx_buffer_[this->rx_len_++] = raw;
      if (this->rx_len_ < 4)
        return;
      // Header complet: inverser et valider.
      invert_(this->rx_buffer_, 4);
      if (this->rx_buffer_[0] != 0xDC || this->rx_buffer_[1] != 0x80) {
        this->rx_state_ = RX_IDLE;
        this->rx_len_ = 0;
        return;
      }
      if (this->rx_buffer_[2] != this->address_ && this->rx_buffer_[2] != BROADCAST_ADDR) {
        this->rx_state_ = RX_IDLE;
        this->rx_len_ = 0;
        return;
      }
      this->rx_expected_ = this->rx_buffer_[3];
      if (this->rx_expected_ < 10 || this->rx_expected_ > BUFFER_SIZE) {
        this->rx_state_ = RX_IDLE;
        this->rx_len_ = 0;
        return;
      }
      this->rx_state_ = RX_PAYLOAD;
      return;

    case RX_PAYLOAD:
      this->rx_buffer_[this->rx_len_++] = raw;
      if (this->rx_len_ < this->rx_expected_)
        return;
      invert_(this->rx_buffer_ + 4, this->rx_expected_ - 4);
      this->process_frame_();
      this->rx_state_ = RX_IDLE;
      this->rx_len_ = 0;
      return;
  }
}

void AtlanticClimate::process_frame_() {
  // Verifie le CRC.
  uint16_t crc = crc16_(this->rx_buffer_, this->rx_expected_ - 2);
  if (this->rx_buffer_[this->rx_expected_ - 2] != ((crc >> 8) & 0xFF) ||
      this->rx_buffer_[this->rx_expected_ - 1] != (crc & 0xFF)) {
    ESP_LOGW(TAG, "CRC invalide (attendu 0x%04X)", crc);
    return;
  }

  const uint8_t *payload = &this->rx_buffer_[4];
  uint8_t payload_len = this->rx_expected_ - 6;

  if (this->parse_mode_payload_(payload, payload_len))
    return;
  if (this->parse_setpoint_payload_(payload, payload_len))
    return;
  if (this->parse_ambient_payload_(payload, payload_len))
    return;

  ESP_LOGD(TAG, "Trame inconnue (%u o payload)", payload_len);
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, payload_len, ESP_LOG_DEBUG);
}

bool AtlanticClimate::parse_mode_payload_(const uint8_t *data, uint8_t len) {
  if (len != 15)
    return false;
  if (data[0] != 0x02 || data[1] != 0x2D || data[2] != 0x00 || data[3] != 0x02 || data[4] != 0x11)
    return false;

  uint16_t code = (static_cast<uint16_t>(data[5]) << 8) | data[6];
  climate::ClimateMode new_mode = this->mode;
  climate::ClimatePreset new_preset = this->preset.value_or(climate::CLIMATE_PRESET_NONE);

  switch (code) {
    case 0x0000:
      new_mode = climate::CLIMATE_MODE_OFF;
      new_preset = climate::CLIMATE_PRESET_NONE;
      break;
    case 0x0302:
      new_mode = climate::CLIMATE_MODE_HEAT;
      new_preset = climate::CLIMATE_PRESET_COMFORT;
      this->manual_preset_ = climate::CLIMATE_PRESET_COMFORT;
      break;
    case 0x0201:
      new_mode = climate::CLIMATE_MODE_HEAT;
      new_preset = climate::CLIMATE_PRESET_ECO;
      this->manual_preset_ = climate::CLIMATE_PRESET_ECO;
      break;
    case 0x0103:  // Auto 1 / ECO
    case 0x0101:  // Auto 2 / ECO
      new_mode = climate::CLIMATE_MODE_AUTO;
      new_preset = climate::CLIMATE_PRESET_ECO;
      this->auto_preset_ = climate::CLIMATE_PRESET_ECO;
      break;
    case 0x0102:  // Auto 1 / CONFORT
    case 0x0104:  // Auto 2 / CONFORT
      new_mode = climate::CLIMATE_MODE_AUTO;
      new_preset = climate::CLIMATE_PRESET_COMFORT;
      this->auto_preset_ = climate::CLIMATE_PRESET_COMFORT;
      break;
    default:
      ESP_LOGW(TAG, "Mode inconnu 0x%04X", code);
      return true;  // trame reconnue mais code inutile => on evite le fallback "inconnu"
  }

  this->mode = new_mode;
  this->preset = new_preset;
  this->update_action_from_mode_();
  this->publish_state();
  return true;
}

bool AtlanticClimate::parse_setpoint_payload_(const uint8_t *data, uint8_t len) {
  if (len != 8)
    return false;
  if (data[0] != 0x07 || data[1] != 0x2D || data[2] != 0x3D || data[3] != 0x05 ||
      data[4] != 0x8E || data[5] != 0x00)
    return false;

  // Setpoint: 11 bits utiles, 5 bits bas reserves aux flags/status.
  uint16_t raw = (static_cast<uint16_t>(data[6]) << 8) | data[7];
  this->target_temperature = static_cast<float>(raw >> 5) / 2.0f;
  ESP_LOGI(TAG, "Setpoint recu: %.1fC (raw=0x%04X)", this->target_temperature, raw);
  this->publish_state();
  return true;
}

bool AtlanticClimate::parse_ambient_payload_(const uint8_t *data, uint8_t len) {
  // Reponse a une lecture sur 0x051E (raw) ou 0x056A (filtre).
  // Encodage identique au setpoint: T = raw / 64.
  if (len != 8)
    return false;
  if (data[0] != 0x07 || data[1] != 0x2D || data[2] != 0x3D || data[5] != 0x00)
    return false;
  uint16_t reg = (static_cast<uint16_t>(data[3]) << 8) | data[4];
  if (reg != 0x051E && reg != 0x056A)
    return false;

  uint16_t raw = (static_cast<uint16_t>(data[6]) << 8) | data[7];
  float t = static_cast<float>(raw) / 64.0f;
  if (t < 0.0f || t > 50.0f) {
    ESP_LOGW(TAG, "Sonde ambiante hors plage: %.2fC (raw=0x%04X reg=0x%04X)", t, raw, reg);
    return true;
  }
  this->current_temperature = t;
  ESP_LOGI(TAG, "Sonde ambiante: %.2fC (raw=0x%04X reg=0x%04X)", t, raw, reg);
  this->publish_state();
  ESP_LOGD(TAG, "  -> this->current_temperature relu apres publish = %.2f", this->current_temperature);
  if (this->ambient_temp_sensor_ != nullptr)
    this->ambient_temp_sensor_->publish_state(t);
  return true;
}

// -------- API composant ESPHome --------

void AtlanticClimate::setup() {
  this->mode = climate::CLIMATE_MODE_OFF;
  this->preset = climate::CLIMATE_PRESET_NONE;
  this->action = climate::CLIMATE_ACTION_OFF;
  this->load_persistent_state_();
  this->request_setpoint_();
  this->request_ambient_temperature_();
}

void AtlanticClimate::update() {
  this->request_setpoint_();
  this->request_ambient_temperature_();
}

void AtlanticClimate::loop() {
  // Draine tout ce qui est disponible; le parser gere lui-meme la resynchronisation.
  while (this->available()) {
    uint8_t byte;
    if (!this->read_byte(&byte))
      break;
    this->feed_rx_byte_(byte);
  }
}

void AtlanticClimate::control(const climate::ClimateCall &call) {
  bool mode_change = call.get_mode().has_value();
  bool preset_change = call.get_preset().has_value();
  bool temp_change = call.get_target_temperature().has_value();

  if (mode_change || preset_change) {
    climate::ClimateMode target_mode = mode_change ? *call.get_mode() : this->mode;
    climate::ClimatePreset target_preset =
        preset_change ? *call.get_preset()
                      : this->preset.value_or(climate::CLIMATE_PRESET_NONE);
    this->encode_mode_frame_(target_mode, target_preset);
    this->send_pending_frame_();
    this->last_sent_mode_ = target_mode;
    this->save_persistent_state_();
  }

  if (temp_change) {
    float temp = *call.get_target_temperature();
    this->encode_temperature_frame_(temp);
    this->send_pending_frame_();
    this->target_temperature = temp;
    this->last_sent_target_ = temp;
    this->publish_state();
    this->save_persistent_state_();
  }
}

climate::ClimateTraits AtlanticClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                            climate::CLIMATE_SUPPORTS_ACTION);
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_AUTO,
  });
  traits.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,
      climate::CLIMATE_PRESET_COMFORT,
      climate::CLIMATE_PRESET_ECO,
  });
  traits.set_visual_min_temperature(19);
  traits.set_visual_max_temperature(28);
  traits.set_visual_target_temperature_step(0.5);
  traits.set_visual_current_temperature_step(0.5);
  return traits;
}

void AtlanticClimate::update_action_from_mode_() {
  climate::ClimateAction new_action = climate::CLIMATE_ACTION_IDLE;
  if (this->mode == climate::CLIMATE_MODE_OFF)
    new_action = climate::CLIMATE_ACTION_OFF;
  else if (this->mode == climate::CLIMATE_MODE_HEAT || this->mode == climate::CLIMATE_MODE_AUTO)
    new_action = climate::CLIMATE_ACTION_HEATING;
  this->action = new_action;
}

void AtlanticClimate::save_persistent_state_() {
  PersistedState s{};
  s.last_sent_target = this->last_sent_target_;
  s.last_sent_mode = static_cast<uint8_t>(this->last_sent_mode_);
  s.manual_preset = static_cast<uint8_t>(this->manual_preset_);
  s.auto_preset = static_cast<uint8_t>(this->auto_preset_);
  this->pref_state_.save(&s);
}

void AtlanticClimate::load_persistent_state_() {
  this->pref_state_ = this->make_entity_preference<PersistedState>();
  PersistedState s{};
  if (this->pref_state_.load(&s)) {
    if (!std::isnan(s.last_sent_target) && s.last_sent_target >= 15.0f && s.last_sent_target <= 28.0f)
      this->last_sent_target_ = s.last_sent_target;
    this->last_sent_mode_ = static_cast<climate::ClimateMode>(s.last_sent_mode);
    this->manual_preset_ = static_cast<climate::ClimatePreset>(s.manual_preset);
    this->auto_preset_ = static_cast<climate::ClimatePreset>(s.auto_preset);
    ESP_LOGI(TAG, "Etat persistant charge: target=%.1fC mode=%d manual_preset=%d auto_preset=%d",
             this->last_sent_target_, static_cast<int>(this->last_sent_mode_),
             static_cast<int>(this->manual_preset_), static_cast<int>(this->auto_preset_));
  } else {
    ESP_LOGD(TAG, "Aucun etat persistant");
  }
}

void AtlanticClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Atlantic Climate:");
  ESP_LOGCONFIG(TAG, "  Adresse bus: %u", this->address_);
  this->check_uart_settings(4800, 2, uart::UART_CONFIG_PARITY_ODD, 8);
  this->dump_traits_(TAG);
}

}  // namespace atlantic_climate
}  // namespace esphome
