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

void AtlanticClimate::probe_register(uint8_t target, uint16_t reg) {
  uint8_t *p = this->tx_payload_;
  uint8_t i = 0;
  p[i++] = 0x06;
  p[i++] = 0x3D;  // notre identite sur le bus applicatif
  p[i++] = target;
  p[i++] = (reg >> 8) & 0xFF;
  p[i++] = reg & 0xFF;
  this->tx_payload_len_ = i;
  this->send_pending_frame_();
  ESP_LOGI(TAG, "Probe envoye: target=0x%02X reg=0x%04X", target, reg);
}

void AtlanticClimate::send_raw_payload(const std::vector<uint8_t> &payload) {
  if (payload.empty() || payload.size() > BUFFER_SIZE - 6) {
    ESP_LOGE(TAG, "send_raw_payload: taille invalide (%u)", static_cast<unsigned>(payload.size()));
    return;
  }
  for (size_t i = 0; i < payload.size(); i++)
    this->tx_payload_[i] = payload[i];
  this->tx_payload_len_ = static_cast<uint8_t>(payload.size());
  this->send_pending_frame_();
  ESP_LOGI(TAG, "Raw payload envoye (%u o)", static_cast<unsigned>(payload.size()));
}

void AtlanticClimate::broadcast_notify(uint8_t src_module, uint16_t reg,
                                       const std::vector<uint8_t> &data) {
  std::vector<uint8_t> payload;
  payload.reserve(5 + data.size());
  payload.push_back(0x02);              // opcode notify
  payload.push_back(src_module);        // src module (fake)
  payload.push_back(0x00);              // dst = maitre
  payload.push_back((reg >> 8) & 0xFF);
  payload.push_back(reg & 0xFF);
  for (auto b : data)
    payload.push_back(b);
  this->send_raw_payload(payload);
  ESP_LOGW(TAG, "IMPERSONATION: notify emise en tant que 0x%02X reg=0x%04X", src_module, reg);
}

void AtlanticClimate::impersonate_ambient(uint8_t src_module, float temperature) {
  if (temperature < 0.0f || temperature > 50.0f) {
    ESP_LOGE(TAG, "impersonate_ambient: temperature hors plage (%.1f)", temperature);
    return;
  }
  uint16_t raw = static_cast<uint16_t>(temperature * 64.0f);
  std::vector<uint8_t> data = {0x00, static_cast<uint8_t>((raw >> 8) & 0xFF),
                                static_cast<uint8_t>(raw & 0xFF)};
  this->broadcast_notify(src_module, 0x051E, data);
  ESP_LOGW(TAG, "IMPERSONATION: sonde ambiante spoofee = %.2fC (src=0x%02X)", temperature, src_module);
}

void AtlanticClimate::request_setpoint_() {
  this->probe_register(0x2D, 0x058E);
}

void AtlanticClimate::request_ambient_temperature_() {
  this->probe_register(0x2D, 0x051E);
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

  this->debug_dump_frame_(payload, payload_len);

  if (this->parse_mode_payload_(payload, payload_len))
    return;
  if (this->parse_setpoint_payload_(payload, payload_len))
    return;
  if (this->parse_ambient_payload_(payload, payload_len))
    return;

  ESP_LOGD(TAG, "Trame inconnue (%u o payload)", payload_len);
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, payload_len, ESP_LOG_DEBUG);
}

uint32_t AtlanticClimate::fnv1a_(const uint8_t *data, uint8_t len) {
  uint32_t h = 0x811C9DC5u;
  for (uint8_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 0x01000193u;
  }
  return h;
}

void AtlanticClimate::debug_dump_frame_(const uint8_t *payload, uint8_t len) {
  if (!this->debug_frames_ || len == 0)
    return;

  // Ignore les NACK (opcode 0x08 = "registre inconnu / non supporte"): trop bruyant lors d'un sweep.
  if (payload[0] == 0x08) {
    ESP_LOGV(TAG, "NACK reg=0x%04X code=0x%02X",
             len >= 5 ? (static_cast<uint16_t>(payload[3]) << 8) | payload[4] : 0,
             len >= 6 ? payload[5] : 0);
    return;
  }

  // Cle = 5 premiers octets du payload (opcode + src + dst + registre 2o).
  uint64_t key = 0;
  for (uint8_t i = 0; i < len && i < 5; i++)
    key |= static_cast<uint64_t>(payload[i]) << ((4 - i) * 8);
  uint32_t h = fnv1a_(payload, len);

  const char *tag_state = "NEW";
  int slot = -1;
  for (int i = 0; i < 16; i++) {
    if (this->sniff_slots_[i].key == key) {
      slot = i;
      break;
    }
  }
  if (slot >= 0) {
    if (this->sniff_slots_[slot].hash == h)
      return;  // Identique a la derniere fois, on ne re-log pas.
    this->sniff_slots_[slot].hash = h;
    tag_state = "CHANGED";
  } else {
    for (int i = 0; i < 16; i++) {
      if (this->sniff_slots_[i].key == 0) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      slot = this->sniff_next_slot_;
      this->sniff_next_slot_ = (this->sniff_next_slot_ + 1) & 15;
    }
    this->sniff_slots_[slot].key = key;
    this->sniff_slots_[slot].hash = h;
  }

  // Dump hex complet du payload.
  char hexbuf[3 * BUFFER_SIZE + 1] = {0};
  size_t off = 0;
  for (uint8_t i = 0; i < len && off + 3 < sizeof(hexbuf); i++)
    off += snprintf(hexbuf + off, sizeof(hexbuf) - off, "%02X ", payload[i]);

  const char *op_desc = "?";
  switch (payload[0]) {
    case 0x02: op_desc = "notify"; break;
    case 0x03: op_desc = "write"; break;
    case 0x06: op_desc = "read"; break;
    case 0x07: op_desc = "read-reply"; break;
  }
  uint8_t src = len >= 2 ? payload[1] : 0;
  uint8_t dst = len >= 3 ? payload[2] : 0;
  uint16_t reg = len >= 5 ? (static_cast<uint16_t>(payload[3]) << 8) | payload[4] : 0;
  ESP_LOGI(TAG, "[sniff %s] src=0x%02X dst=0x%02X op=0x%02X(%s) reg=0x%04X len=%u: %s",
           tag_state, src, dst, payload[0], op_desc, reg, len, hexbuf);

  // Test de decodages 16 bits BE + LE a chaque offset.
  static const struct {
    const char *name;
    float (*fn)(uint16_t);
  } decoders[] = {
      {"/10", [](uint16_t r) { return static_cast<float>(r) / 10.0f; }},
      {"/16", [](uint16_t r) { return static_cast<float>(r) / 16.0f; }},
      {"/100", [](uint16_t r) { return static_cast<float>(r) / 100.0f; }},
      {">>5/2", [](uint16_t r) { return static_cast<float>(r >> 5) / 2.0f; }},
      {">>4/10", [](uint16_t r) { return static_cast<float>(r >> 4) / 10.0f; }},
      {"/8", [](uint16_t r) { return static_cast<float>(r) / 8.0f; }},
  };

  for (uint8_t i = 0; i + 1 < len; i++) {
    uint16_t be = (static_cast<uint16_t>(payload[i]) << 8) | payload[i + 1];
    uint16_t le = (static_cast<uint16_t>(payload[i + 1]) << 8) | payload[i];
    char line[192] = {0};
    size_t o = 0;
    bool any = false;
    for (auto &d : decoders) {
      float t = d.fn(be);
      if (t >= 10.0f && t <= 40.0f && o + 28 < sizeof(line)) {
        o += snprintf(line + o, sizeof(line) - o, " BE%s=%.2fC", d.name, t);
        any = true;
      }
      t = d.fn(le);
      if (t >= 10.0f && t <= 40.0f && o + 28 < sizeof(line)) {
        o += snprintf(line + o, sizeof(line) - o, " LE%s=%.2fC", d.name, t);
        any = true;
      }
    }
    if (any)
      ESP_LOGI(TAG, "  @[%u] raw=0x%04X%s", i, be, line);
  }

  // Test 1 octet (0.5C step).
  for (uint8_t i = 0; i < len; i++) {
    float t = payload[i] / 2.0f;
    if (t >= 15.0f && t <= 40.0f && payload[i] != 0xFF)
      ESP_LOGI(TAG, "  @[%u] byte=0x%02X /2=%.1fC", i, payload[i], t);
  }
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

  // Modulation autonome: chaque changement de sonde recalcule la target.
  for (auto &r : this->rooms_) {
    if (r.current != nullptr)
      r.current->add_on_state_callback([this](float) { this->modulation_recompute_(); });
    if (r.target != nullptr)
      r.target->add_on_state_callback([this](float) { this->modulation_recompute_(); });
    if (r.state != nullptr)
      r.state->add_on_state_callback([this](std::string) { this->modulation_recompute_(); });
  }
  if (this->reference_sensor_ != nullptr)
    this->reference_sensor_->add_on_state_callback([this](float) { this->modulation_recompute_(); });

  // Watchdog: verifie toutes les 60s que les sondes HA sont toujours vivantes.
  this->set_interval("atlantic_wdog", 60000, [this]() { this->watchdog_check_(); });
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

  // Toute action venant de HA suspend la modulation autonome pour manual_hold_ms_.
  if (mode_change || preset_change || temp_change) {
    this->manual_override_until_ms_ = millis() + this->manual_hold_ms_;
    ESP_LOGI(TAG, "Override manuel actif pendant %u s", this->manual_hold_ms_ / 1000u);
  }

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

// -------- Modulation autonome --------

void AtlanticClimate::add_room(sensor::Sensor *current, sensor::Sensor *target,
                                text_sensor::TextSensor *state) {
  this->rooms_.push_back({current, target, state});
}

float AtlanticClimate::modulation_delta_for_deficit_(float deficit) const {
  // Rampe par morceaux: petit deficit -> petit delta (basse modulation PAC),
  // grand deficit -> grand delta (haute modulation), sature a delta_max_.
  if (deficit <= this->deadband_)
    return this->idle_offset_;
  if (deficit < 1.0f) {
    float t = (deficit - this->deadband_) / (1.0f - this->deadband_);
    return 0.5f + t * 1.0f;  // 0.5 -> 1.5
  }
  if (deficit < 3.0f)
    return 1.5f + (deficit - 1.0f) * 1.25f;  // 1.5 -> 4.0
  float d = 4.0f + (deficit - 3.0f) * 1.0f;
  return d < this->delta_max_ ? d : this->delta_max_;
}

void AtlanticClimate::modulation_recompute_() {
  if (this->rooms_.empty() || this->reference_sensor_ == nullptr)
    return;

  float ref = this->reference_sensor_->state;
  if (std::isnan(ref))
    return;

  float max_deficit = -1000.0f;
  bool any_valid = false;
  uint32_t active_rooms = 0;
  for (auto &r : this->rooms_) {
    if (r.current == nullptr || r.target == nullptr)
      continue;
    if (r.state != nullptr && r.state->has_state() && r.state->state == "off")
      continue;
    float c = r.current->state;
    float t = r.target->state;
    if (std::isnan(c) || std::isnan(t))
      continue;
    float d = t - c;
    if (d > max_deficit)
      max_deficit = d;
    any_valid = true;
    active_rooms++;
  }
  if (!any_valid)
    return;

  // Sondes vivantes: le watchdog peut se detendre.
  this->last_valid_data_ms_ = millis();
  if (this->in_safe_mode_) {
    ESP_LOGI(TAG, "Sortie du mode antigel: sondes HA de retour");
    this->in_safe_mode_ = false;
  }

  float delta = this->modulation_delta_for_deficit_(max_deficit);
  float desired_target = ref + delta;
  if (desired_target < 15.0f) desired_target = 15.0f;
  if (desired_target > 28.0f) desired_target = 28.0f;
  desired_target = std::round(desired_target * 2.0f) / 2.0f;

  // Telemetrie (sans throttle: c'est le calcul actuel a chaque changement).
  if (this->deficit_sensor_ != nullptr)
    this->deficit_sensor_->publish_state(max_deficit);
  if (this->delta_sensor_ != nullptr)
    this->delta_sensor_->publish_state(delta);
  if (this->active_rooms_sensor_ != nullptr)
    this->active_rooms_sensor_->publish_state(static_cast<float>(active_rooms));

  this->update_action_(max_deficit);

  // Respect override manuel: on ne pousse pas de trame tant que le hold est actif.
  uint32_t now = millis();
  if (this->manual_override_until_ms_ != 0 && now < this->manual_override_until_ms_) {
    ESP_LOGD(TAG, "Modulation en pause (override manuel, %u s restants)",
             (this->manual_override_until_ms_ - now) / 1000u);
    return;
  }
  this->manual_override_until_ms_ = 0;

  climate::ClimateMode desired_mode = climate::CLIMATE_MODE_HEAT;
  bool mode_changed = desired_mode != this->last_sent_mode_;
  bool target_changed = std::isnan(this->last_sent_target_) ||
                        std::fabs(desired_target - this->last_sent_target_) >= 0.5f;

  if (!mode_changed && !target_changed)
    return;

  if (this->last_modulation_send_ms_ != 0 && (now - this->last_modulation_send_ms_) < 30000u) {
    ESP_LOGD(TAG, "Modulation throttlee (deficit=%.2f target=%.1f)", max_deficit, desired_target);
    return;
  }

  ESP_LOGI(TAG, "Modulation: ref=%.1fC deficit=%.2fC delta=%.2fC -> target=%.1fC (%u pieces actives)",
           ref, max_deficit, delta, desired_target, active_rooms);

  if (mode_changed) {
    this->encode_mode_frame_(desired_mode, climate::CLIMATE_PRESET_COMFORT);
    this->send_pending_frame_();
    this->last_sent_mode_ = desired_mode;
  }
  if (target_changed) {
    this->encode_temperature_frame_(desired_target);
    this->send_pending_frame_();
    this->last_sent_target_ = desired_target;
    this->target_temperature = desired_target;
    this->publish_state();
  }
  this->last_modulation_send_ms_ = now;
  this->save_persistent_state_();
}

void AtlanticClimate::update_action_(float max_deficit) {
  climate::ClimateAction new_action;
  if (this->mode == climate::CLIMATE_MODE_OFF)
    new_action = climate::CLIMATE_ACTION_OFF;
  else if (max_deficit > this->deadband_)
    new_action = climate::CLIMATE_ACTION_HEATING;
  else
    new_action = climate::CLIMATE_ACTION_IDLE;

  if (new_action != this->action) {
    this->action = new_action;
    this->publish_state();
  }
}

void AtlanticClimate::watchdog_check_() {
  if (this->rooms_.empty())
    return;  // Pas de modulation autonome configuree, rien a surveiller.

  uint32_t now = millis();
  if (this->last_valid_data_ms_ == 0)
    this->last_valid_data_ms_ = now;  // premier passage: on demarre le compteur.

  bool stale = (now - this->last_valid_data_ms_) > this->watchdog_timeout_ms_;
  if (stale && !this->in_safe_mode_) {
    ESP_LOGW(TAG, "Watchdog: aucune sonde valide depuis %u s -> mode antigel (target=%.1fC)",
             (now - this->last_valid_data_ms_) / 1000u, this->safe_target_);
    this->encode_mode_frame_(climate::CLIMATE_MODE_HEAT, climate::CLIMATE_PRESET_COMFORT);
    this->send_pending_frame_();
    this->encode_temperature_frame_(this->safe_target_);
    this->send_pending_frame_();
    this->last_sent_mode_ = climate::CLIMATE_MODE_HEAT;
    this->last_sent_target_ = this->safe_target_;
    this->target_temperature = this->safe_target_;
    this->publish_state();
    this->in_safe_mode_ = true;
    this->save_persistent_state_();
  }
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
  ESP_LOGCONFIG(TAG, "  Pieces surveillees: %u", static_cast<unsigned>(this->rooms_.size()));
  ESP_LOGCONFIG(TAG, "  Sonde de reference: %s",
                this->reference_sensor_ != nullptr ? "configuree" : "NON DEFINIE");
  ESP_LOGCONFIG(TAG, "  Deadband: %.2fC / delta_max: %.1fC / idle_offset: %+.2fC",
                this->deadband_, this->delta_max_, this->idle_offset_);
  ESP_LOGCONFIG(TAG, "  Override manuel: %u s", this->manual_hold_ms_ / 1000u);
  ESP_LOGCONFIG(TAG, "  Watchdog: %u s / target antigel: %.1fC",
                this->watchdog_timeout_ms_ / 1000u, this->safe_target_);
  ESP_LOGCONFIG(TAG, "  Debug frames: %s", this->debug_frames_ ? "ON" : "OFF");
  this->dump_traits_(TAG);
}

}  // namespace atlantic_climate
}  // namespace esphome
