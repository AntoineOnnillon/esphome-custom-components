#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
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
  // 64 pour absorber 0x0248 (22 o payload) et laisser marge en mode promiscue.
  static constexpr size_t BUFFER_SIZE = 64;
  // Notre identite applicative par defaut (utilisee comme src_module dans nos requetes 0x06/0x03).
  static constexpr uint8_t DEFAULT_SRC_MODULE = 0x3D;

  void set_address(uint8_t address) { this->address_ = address; }
  void set_debug_frames(bool v) { this->debug_frames_ = v; }
  // Mode promiscue: accepte toutes les trames du bus, y compris celles non adressees a nous.
  // Rend visibles les communications directes panel<->chaudiere (ex: ecriture sonde ambiante
  // du panneau vers le regulateur), invisibles autrement.
  void set_sniff_all_frames(bool v) { this->sniff_all_frames_ = v; }

  // --- Modulation autonome ---
  // Sonde de reference (typiquement celle du salon, la piece ou le panneau est installe).
  // Sert d'ancrage: target = reference + delta_calcule.
  void set_reference_sensor(sensor::Sensor *s) { this->reference_sensor_ = s; }
  // Chaque piece definit (sonde ambiante, consigne HA) + optionnellement un text_sensor
  // portant l'etat HVAC (off/heat/auto). Une piece en 'off' est ignoree dans le calcul.
  void add_room(sensor::Sensor *current, sensor::Sensor *target, text_sensor::TextSensor *state);
  void set_deadband(float v) { this->deadband_ = v; }
  void set_delta_max(float v) { this->delta_max_ = v; }
  void set_idle_offset(float v) { this->idle_offset_ = v; }

  // --- Overrides + securite ---
  // Duree pendant laquelle la modulation autonome est suspendue apres un `control()` HA.
  void set_manual_hold_ms(uint32_t v) { this->manual_hold_ms_ = v; }
  // Duree sans donnee valide avant de basculer en mode antigel.
  void set_watchdog_timeout_ms(uint32_t v) { this->watchdog_timeout_ms_ = v; }
  void set_safe_target(float v) { this->safe_target_ = v; }

  // --- Telemetrie exposee via la plateforme sensor: ---
  void set_ambient_temperature_sensor(sensor::Sensor *s) { this->ambient_temp_sensor_ = s; }
  void set_deficit_sensor(sensor::Sensor *s) { this->deficit_sensor_ = s; }
  void set_delta_sensor(sensor::Sensor *s) { this->delta_sensor_ = s; }
  void set_active_rooms_sensor(sensor::Sensor *s) { this->active_rooms_sensor_ = s; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  // Envoie une requete de lecture (opcode 0x06) vers un module et un registre arbitraires,
  // avec notre identite par defaut (0x3D).
  void probe_register(uint8_t target, uint16_t reg);
  // Meme chose, mais en usurpant un autre src_module. Permet de tester si la chaudiere
  // repond differemment selon l'identite du demandeur.
  void probe_register_as(uint8_t src_module, uint8_t target, uint16_t reg);

  // --- Emission bas niveau (experimentation protocole) ---
  // Envoie un payload applicatif arbitraire encapsule dans une trame bus valide.
  // Le header (0xDC, sender_wire, dest_wire, len) et le CRC XMODEM sont calcules ici.
  // Attention: pouvoir emettre n'importe quoi peut perturber la chaudiere. A utiliser
  // uniquement pour du reverse engineering, en observant les reactions via `debug_frames`.
  void send_raw_payload(const std::vector<uint8_t> &payload);

  // Helpers de haut niveau pour l'impersonation:
  // - broadcast_notify: emet une trame `02 src 00 reg_hi reg_lo <data...>` comme si on etait `src`.
  void broadcast_notify(uint8_t src_module, uint16_t reg, const std::vector<uint8_t> &data);
  // - write_register_as: emet une trame `03 src target reg_hi reg_lo <data...>` (opcode write).
  //   Utile pour tester si la chaudiere accepte une ecriture provenant d'une identite differente.
  void write_register_as(uint8_t src_module, uint8_t target, uint16_t reg,
                          const std::vector<uint8_t> &data);
  // - impersonate_ambient: fait croire au bus que le module `src_module` mesure `T` sur reg 0x051E.
  //   Utile pour spoofer la sonde vue par la chaudiere si src_module=0x05.
  void impersonate_ambient(uint8_t src_module, float temperature);
  // - emulate_panel_burst: rejoue la sequence de notify observee du panneau reel (0x05),
  //   mais depuis un src_module fake. Test cle pour verifier si la chaudiere accepte un
  //   nouveau "panneau" sur le bus. `counter` est le compteur interne, `state_flag` = octet
  //   principal du reg 0x0248 (0xD1 sur le panneau reel).
  void emulate_panel_burst(uint8_t src_module, uint8_t counter, uint8_t state_flag);

 protected:
  // Emission
  void request_setpoint_();
  void request_ambient_temperature_();
  void encode_temperature_frame_(float temperature);
  void encode_mode_frame_(climate::ClimateMode mode, climate::ClimatePreset preset);
  void send_pending_frame_();

  // Reception (non bloquante, alimentee octet par octet par loop()).
  void feed_rx_byte_(uint8_t byte);
  void process_frame_();
  bool parse_mode_payload_(const uint8_t *data, uint8_t len);
  bool parse_setpoint_payload_(const uint8_t *data, uint8_t len);
  bool parse_ambient_payload_(const uint8_t *data, uint8_t len);

  // Aide au reverse-engineering: dump hex + candidats temperature a chaque offset.
  // `wire_src` et `wire_dst` sont pris dans le header (rx_buffer_[1]&0x7F / rx_buffer_[2]).
  void debug_dump_frame_(uint8_t wire_src, uint8_t wire_dst, const uint8_t *payload, uint8_t len);
  static uint32_t fnv1a_(const uint8_t *data, uint8_t len);

  // Utilitaires bas niveau.
  static uint16_t crc16_(const uint8_t *data, uint8_t data_len, uint16_t seed = 0);
  static void invert_(uint8_t *data, uint8_t data_len);
  uint8_t build_frame_(uint8_t *dst, const uint8_t *payload, uint8_t payload_len);

  uint8_t address_{7};
  bool debug_frames_{false};
  bool sniff_all_frames_{false};

  // Table de deduplication pour le sniff: cle = 6 octets packes (wire_src + 5 premiers octets du payload),
  // valeur = hash FNV-1a du payload. On ne re-log une trame que si son hash change.
  // 32 slots pour absorber la variete supplementaire quand sniff_all_frames_ est actif.
  struct SniffSlot {
    uint64_t key;
    uint32_t hash;
  };
  SniffSlot sniff_slots_[32]{};
  uint8_t sniff_next_slot_{0};

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

  // Modulation autonome.
  struct Room {
    sensor::Sensor *current;
    sensor::Sensor *target;
    text_sensor::TextSensor *state;  // optionnel: hvac_mode HA, piece ignoree si "off"
  };
  std::vector<Room> rooms_;
  sensor::Sensor *reference_sensor_{nullptr};
  float deadband_{0.2f};
  float delta_max_{6.0f};
  float idle_offset_{-0.5f};

  void modulation_recompute_();
  float modulation_delta_for_deficit_(float deficit) const;
  void update_action_(float max_deficit);

  float last_sent_target_{NAN};
  climate::ClimateMode last_sent_mode_{climate::CLIMATE_MODE_OFF};
  uint32_t last_modulation_send_ms_{0};

  // --- Overrides manuels ---
  uint32_t manual_hold_ms_{15u * 60u * 1000u};
  uint32_t manual_override_until_ms_{0};

  // --- Watchdog ---
  uint32_t watchdog_timeout_ms_{10u * 60u * 1000u};
  uint32_t last_valid_data_ms_{0};
  float safe_target_{18.0f};
  bool in_safe_mode_{false};
  void watchdog_check_();

  // --- Telemetrie ---
  sensor::Sensor *ambient_temp_sensor_{nullptr};
  sensor::Sensor *deficit_sensor_{nullptr};
  sensor::Sensor *delta_sensor_{nullptr};
  sensor::Sensor *active_rooms_sensor_{nullptr};

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
