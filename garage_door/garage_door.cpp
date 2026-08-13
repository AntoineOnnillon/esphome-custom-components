#include "garage_door.h"
#include "esphome/core/log.h"

namespace esphome {
namespace garage_door {

static const char *const TAG = "garage_door";

void GarageDoorCover::setup() {
  this->scl_monitor_pin_->setup();
  this->sda_monitor_pin_->setup();
  this->scl_isr_pin_ = this->scl_monitor_pin_->to_isr();
  this->sda_isr_pin_ = this->sda_monitor_pin_->to_isr();
  this->scl_monitor_pin_->attach_interrupt(&GarageDoorCover::scl_isr, this,
                                           gpio::INTERRUPT_RISING_EDGE);
  this->sda_monitor_pin_->attach_interrupt(&GarageDoorCover::sda_isr, this,
                                           gpio::INTERRUPT_ANY_EDGE);

  this->pref_cal_ = global_preferences->make_preference<Calibration>(this->get_object_id_hash());
  if (this->pref_cal_.load(&this->cal_)) {
    ESP_LOGI(TAG, "Calibration chargee: OPEN=%u ms, CLOSE=%u ms",
             this->cal_.open_ms, this->cal_.close_ms);
  }

  // Etat par defaut au boot: ferme, sauf si une position a ete persistee.
  this->pref_pos_ = global_preferences->make_preference<float>(this->get_object_id_hash() ^ 0x1EA1);
  float saved_pos;
  if (this->pref_pos_.load(&saved_pos) && saved_pos >= 0.0f && saved_pos <= 1.0f) {
    this->position = saved_pos;
    ESP_LOGI(TAG, "Position restauree: %.0f%%", saved_pos * 100.0f);
  } else {
    this->position = cover::COVER_CLOSED;
  }
  this->current_operation = cover::COVER_OPERATION_IDLE;

  if (this->obstacle_bs_ != nullptr) {
    this->obstacle_bs_->publish_state(false);
  }
  // Optimiste: on considere le controleur online au boot (grace de 15 min avant offline).
  this->last_activity_ms_ = millis();
  if (this->online_bs_ != nullptr) {
    this->online_bs_->publish_state(true);
  }
}

void GarageDoorCover::dump_config() {
  ESP_LOGCONFIG(TAG, "Garage Door Cover:");
  LOG_I2C_DEVICE(this);
  LOG_PIN("  SCL Monitor Pin: ", this->scl_monitor_pin_);
  LOG_PIN("  SDA Monitor Pin: ", this->sda_monitor_pin_);
  ESP_LOGCONFIG(TAG, "  Calibration OPEN:  %u ms%s", this->cal_.open_ms,
                this->cal_.open_ms == 0 ? " (non calibre)" : "");
  ESP_LOGCONFIG(TAG, "  Calibration CLOSE: %u ms%s", this->cal_.close_ms,
                this->cal_.close_ms == 0 ? " (non calibre)" : "");
  if (this->obstacle_bs_ != nullptr) {
    LOG_BINARY_SENSOR("  ", "Obstacle", this->obstacle_bs_);
  }
}

cover::CoverTraits GarageDoorCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_is_assumed_state(false);
  traits.set_supports_position(true);
  traits.set_supports_stop(true);
  traits.set_supports_tilt(false);
  return traits;
}

void GarageDoorCover::control(const cover::CoverCall &call) {
  // Stop demande par HA: on renvoie la meme direction que le mouvement en cours -> controleur pause (0x13).
  if (call.get_stop()) {
    this->cancel_timeout("intermediate_pause");
    if (this->current_operation == cover::COVER_OPERATION_OPENING) {
      ESP_LOGI(TAG, "Stop demande pendant OUVERTURE, envoi trame de pause");
      this->send_open_frame_();
    } else if (this->current_operation == cover::COVER_OPERATION_CLOSING) {
      ESP_LOGI(TAG, "Stop demande pendant FERMETURE, envoi trame de pause");
      this->send_close_frame_();
    } else {
      ESP_LOGI(TAG, "Stop demande mais portail deja a l'arret");
    }
    return;
  }

  if (!call.get_position().has_value())
    return;
  float target = *call.get_position();

  bool at_open = (this->position == cover::COVER_OPEN) &&
                 (this->current_operation == cover::COVER_OPERATION_IDLE);
  bool at_closed = (this->position == cover::COVER_CLOSED) &&
                   (this->current_operation == cover::COVER_OPERATION_IDLE);
  bool opening = (this->current_operation == cover::COVER_OPERATION_OPENING);
  bool closing = (this->current_operation == cover::COVER_OPERATION_CLOSING);

  // Cible = ouverture totale
  if (target == cover::COVER_OPEN) {
    if (at_open || opening) {
      ESP_LOGI(TAG, "Deja ouvert ou en ouverture, trame OPEN non envoyee");
    } else {
      this->motion_start_ms_ = millis();
      this->motion_start_position_ = this->position;
      this->motion_direction_ = cover::COVER_OPERATION_OPENING;
      this->current_operation = cover::COVER_OPERATION_OPENING;
      this->publish_state();
      this->send_open_frame_();
    }
    return;
  }

  // Cible = fermeture totale
  if (target == cover::COVER_CLOSED) {
    if (at_closed || closing) {
      ESP_LOGI(TAG, "Deja ferme ou en fermeture, trame CLOSE non envoyee");
    } else {
      this->motion_start_ms_ = millis();
      this->motion_start_position_ = this->position;
      this->motion_direction_ = cover::COVER_OPERATION_CLOSING;
      this->current_operation = cover::COVER_OPERATION_CLOSING;
      this->publish_state();
      this->send_close_frame_();
    }
    return;
  }

  // Cible intermediaire: necessite calibration et pas de mouvement en cours.
  if (!this->is_calibrated_()) {
    ESP_LOGW(TAG, "Calibration incomplete, cible intermediaire %.0f%% ignoree. Faites un cycle complet", target * 100.0f);
    return;
  }
  if (opening || closing) {
    ESP_LOGW(TAG, "Portail deja en mouvement, cible intermediaire ignoree");
    return;
  }

  float delta = target - this->position;
  if (fabsf(delta) < 0.01f) {
    ESP_LOGI(TAG, "Cible %.0f%% deja atteinte (position=%.2f), aucune commande",
             target * 100.0f, this->position);
    return;
  }

  bool go_open = delta > 0;
  uint32_t duration_ms = go_open ? this->cal_.open_ms : this->cal_.close_ms;
  uint32_t travel_ms = static_cast<uint32_t>((go_open ? delta : -delta) * duration_ms);

  ESP_LOGI(TAG, "Cible intermediaire %.0f%% depuis %.0f%%: %s pendant %u ms",
           target * 100.0f, this->position * 100.0f,
           go_open ? "OPEN" : "CLOSE", travel_ms);

  this->motion_start_ms_ = millis();
  this->motion_start_position_ = this->position;
  this->motion_direction_ =
      go_open ? cover::COVER_OPERATION_OPENING : cover::COVER_OPERATION_CLOSING;
  this->current_operation = this->motion_direction_;
  this->publish_state();

  if (go_open) {
    this->send_open_frame_();
  } else {
    this->send_close_frame_();
  }

  // Pause au bout du temps calcule en renvoyant la meme direction (le controleur passe en 0x13).
  this->set_timeout("intermediate_pause", travel_ms, [this, go_open, target]() {
    if (go_open) {
      this->send_open_frame_();
    } else {
      this->send_close_frame_();
    }
    ESP_LOGI(TAG, "Trame de pause envoyee (cible %.0f%%)", target * 100.0f);
    this->position = target;
    this->current_operation = cover::COVER_OPERATION_IDLE;
    this->publish_state();
    this->save_position_();
  });
}

void GarageDoorCover::send_open_frame_() {
  const uint8_t frame1[12] = {0x0A, 0x01, 0x28, 0x01, 0x01, 0x00,
                              0x08, 0x00, 0x00, 0x00, 0x00, 0x4A};
  auto err = this->write(frame1, sizeof(frame1));
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "I2C write OPEN frame1 -> err=%d", err);
  }
  // 2e sous-frame envoyee 250 ms plus tard sans bloquer le loop().
  this->set_timeout("send_open_frame2", 250, [this]() {
    const uint8_t frame2[12] = {0x0A, 0x01, 0x28, 0x01, 0x01, 0x00,
                                0x04, 0x00, 0x00, 0x00, 0x00, 0x88};
    auto err = this->write(frame2, sizeof(frame2));
    if (err != i2c::ERROR_OK) {
      ESP_LOGW(TAG, "I2C write OPEN frame2 -> err=%d", err);
    }
  });
}

void GarageDoorCover::send_close_frame_() {
  const uint8_t frame1[12] = {0x0A, 0x01, 0x28, 0x00, 0x01, 0x00,
                              0x02, 0x00, 0x00, 0x01, 0x00, 0xFA};
  auto err = this->write(frame1, sizeof(frame1));
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "I2C write CLOSE frame1 -> err=%d", err);
  }
  this->set_timeout("send_close_frame2", 250, [this]() {
    const uint8_t frame2[12] = {0x0A, 0x01, 0x28, 0x00, 0x01, 0x00,
                                0x08, 0x00, 0x00, 0x01, 0x00, 0x59};
    auto err = this->write(frame2, sizeof(frame2));
    if (err != i2c::ERROR_OK) {
      ESP_LOGW(TAG, "I2C write CLOSE frame2 -> err=%d", err);
    }
  });
}

// ISR front montant SCL: echantillonne SDA (bits 0..7), saute le 9e (ACK).
void IRAM_ATTR GarageDoorCover::scl_isr(GarageDoorCover *arg) {
  if (!arg->in_transaction_)
    return;
  if (arg->bit_pos_ < 8) {
    arg->current_byte_ = (arg->current_byte_ << 1) |
                         (arg->sda_isr_pin_.digital_read() ? 1 : 0);
    arg->bit_pos_++;
    if (arg->bit_pos_ == 8) {
      if (arg->frame_len_ < FRAME_MAX)
        arg->frame_buf_[arg->frame_len_++] = arg->current_byte_;
      arg->current_byte_ = 0;
    }
  } else {
    arg->bit_pos_ = 0;
  }
}

// ISR SDA: START (SDA descend) / STOP (SDA monte) uniquement quand SCL est haut.
void IRAM_ATTR GarageDoorCover::sda_isr(GarageDoorCover *arg) {
  if (!arg->scl_isr_pin_.digital_read())
    return;
  if (!arg->sda_isr_pin_.digital_read()) {
    // Trame precedente non consommee -> saute pour eviter la corruption.
    if (arg->frame_ready_)
      return;
    arg->in_transaction_ = true;
    arg->bit_pos_ = 0;
    arg->current_byte_ = 0;
    arg->frame_len_ = 0;
  } else {
    if (arg->in_transaction_)
      arg->frame_ready_ = true;
    arg->in_transaction_ = false;
  }
}

void GarageDoorCover::loop() {
  // Estimation live de la position pendant le mouvement (throttlee a 500 ms).
  if ((this->current_operation == cover::COVER_OPERATION_OPENING ||
       this->current_operation == cover::COVER_OPERATION_CLOSING) &&
      this->is_calibrated_()) {
    uint32_t now = millis();
    if (now - this->last_position_publish_ms_ >= 500) {
      this->last_position_publish_ms_ = now;
      this->update_estimated_position_();
    }
  }

  // Watchdog: si mouvement dure > 1.5x la duree calibree sans transition, on force IDLE.
  if (this->current_operation != cover::COVER_OPERATION_IDLE && this->is_calibrated_()) {
    uint32_t max_ms = (this->current_operation == cover::COVER_OPERATION_OPENING)
                          ? this->cal_.open_ms
                          : this->cal_.close_ms;
    max_ms = (max_ms * 3) / 2;
    if (millis() - this->motion_start_ms_ > max_ms) {
      ESP_LOGW(TAG, "Watchdog: mouvement %s depuis %u ms (max %u ms), force IDLE",
               this->current_operation == cover::COVER_OPERATION_OPENING ? "OUVERTURE" : "FERMETURE",
               millis() - this->motion_start_ms_, max_ms);
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->motion_direction_ = cover::COVER_OPERATION_IDLE;
      this->publish_state();
      this->save_position_();
    }
  }

  // Detection de panne controleur: si aucune trame depuis > 15 min, marquer offline.
  if (this->online_bs_ != nullptr) {
    constexpr uint32_t OFFLINE_TIMEOUT_MS = 15 * 60 * 1000;
    bool timed_out = (millis() - this->last_activity_ms_) > OFFLINE_TIMEOUT_MS;
    if (this->online_state_ && timed_out) {
      ESP_LOGW(TAG, "Controleur portail offline (aucun trafic depuis %u ms)",
               millis() - this->last_activity_ms_);
      this->online_state_ = false;
      this->online_bs_->publish_state(false);
    } else if (!this->online_state_ && !timed_out) {
      ESP_LOGI(TAG, "Controleur portail de nouveau online");
      this->online_state_ = true;
      this->online_bs_->publish_state(true);
    }
  }

  if (!this->frame_ready_)
    return;

  uint8_t local_buf[FRAME_MAX];
  uint8_t local_len;
  {
    InterruptLock lock;
    local_len = this->frame_len_;
    for (uint8_t i = 0; i < local_len; i++)
      local_buf[i] = this->frame_buf_[i];
    this->frame_ready_ = false;
  }

  // Trame plausible (adresse 0x00 + payload) = trafic bus valide, note l'activite.
  if (local_len >= 5 && local_buf[0] == 0x00) {
    this->last_activity_ms_ = millis();
  }

  char hex[FRAME_MAX * 3 + 1] = {0};
  for (uint8_t i = 0; i < local_len && i < FRAME_MAX; i++) {
    snprintf(hex + i * 3, 4, "%02X ", local_buf[i]);
  }
  ESP_LOGD(TAG, "I2C frame (%u): %s", local_len, hex);

  // Status frame autoritatif du controleur portail: prime sur les commandes.
  if (matches_status_frame_(local_buf, local_len)) {
    uint8_t mode = local_len >= 7 ? local_buf[6] : 0x02;
    this->handle_status_(local_buf[4], mode);
    return;
  }

  // Frame commande OPEN/CLOSE. Byte[10] identifie l'emetteur:
  //   0x00=nous OPEN, 0x01=nous CLOSE (ignorees car deja gerees par control())
  //   0x02=remote OPEN, 0x03=remote CLOSE (source externe -> reflet UI immediat)
  bool is_external = (local_len >= 11) && (local_buf[10] > 0x01);
  if (!is_external)
    return;

  if (matches_signature_(local_buf, local_len, 0x01)) {
    bool at_open = (this->position == cover::COVER_OPEN) &&
                   (this->current_operation == cover::COVER_OPERATION_IDLE);
    bool already_opening = (this->current_operation == cover::COVER_OPERATION_OPENING);
    if (!at_open && !already_opening) {
      ESP_LOGI(TAG, "Commande OPEN detectee sur le bus (externe)");
      this->motion_start_ms_ = millis();
      this->motion_start_position_ = this->position;
      this->motion_direction_ = cover::COVER_OPERATION_OPENING;
      this->current_operation = cover::COVER_OPERATION_OPENING;
      this->publish_state();
    }
  } else if (matches_signature_(local_buf, local_len, 0x00)) {
    bool at_closed = (this->position == cover::COVER_CLOSED) &&
                     (this->current_operation == cover::COVER_OPERATION_IDLE);
    bool already_closing = (this->current_operation == cover::COVER_OPERATION_CLOSING);
    if (!at_closed && !already_closing) {
      ESP_LOGI(TAG, "Commande CLOSE detectee sur le bus (externe)");
      this->motion_start_ms_ = millis();
      this->motion_start_position_ = this->position;
      this->motion_direction_ = cover::COVER_OPERATION_CLOSING;
      this->current_operation = cover::COVER_OPERATION_CLOSING;
      this->publish_state();
    }
  }
}

void GarageDoorCover::handle_status_(uint8_t code, uint8_t mode) {
  // Seul 0x06 est confirme comme obstacle. Tout autre mode est traite "pas obstacle".
  if (mode != this->last_status_mode_) {
    bool obstacle_now = (mode == 0x06);
    bool obstacle_prev = (this->last_status_mode_ == 0x06);
    if (obstacle_now) {
      ESP_LOGW(TAG, "OBSTACLE detecte / demi-tour securite du controleur (mode 0x06)");
    } else if (obstacle_prev) {
      ESP_LOGI(TAG, "Mode portail revenu a normal (0x%02X)", mode);
    } else {
      ESP_LOGD(TAG, "Changement de mode observe: 0x%02X -> 0x%02X", this->last_status_mode_, mode);
    }
    if (obstacle_now != obstacle_prev) {
      this->set_obstacle_(obstacle_now);
    }
    this->last_status_mode_ = mode;
  }

  float new_pos = this->position;
  cover::CoverOperation new_op = this->current_operation;
  const char *label = nullptr;

  // Codes observes en live: bits 0-3 = etat (1=ouvert, 2=ferme, 3=arret intermediaire, 4=ouverture, 5=fermeture).
  switch (code) {
    case 0x11:
      new_pos = cover::COVER_OPEN;
      new_op = cover::COVER_OPERATION_IDLE;
      label = "OUVERT";
      break;
    case 0x12:
      new_pos = cover::COVER_CLOSED;
      new_op = cover::COVER_OPERATION_IDLE;
      label = "FERME";
      break;
    case 0x13:
      // Position intermediaire: garde la position actuelle si deja intermediaire (probablement une cible envoyee).
      // Si mode 0x00 (heartbeat idle du controleur), on ne force PAS 0.5 : c'est un broadcast de vie, pas un vrai arret mid-motion.
      if (!(this->position > 0.0f && this->position < 1.0f) && mode != 0x00) {
        new_pos = 0.5f;
      }
      new_op = cover::COVER_OPERATION_IDLE;
      label = "ARRET INTERMEDIAIRE";
      break;
    case 0x14:
      new_op = cover::COVER_OPERATION_OPENING;
      label = "OUVERTURE";
      break;
    case 0x15:
      new_op = cover::COVER_OPERATION_CLOSING;
      label = "FERMETURE";
      break;
    default:
      ESP_LOGW(TAG, "Status portail inconnu (byte[4]=0x%02X)", code);
      return;
  }

  // Debut de mouvement: on enregistre position + timestamp pour calibration/estimation.
  if ((new_op == cover::COVER_OPERATION_OPENING || new_op == cover::COVER_OPERATION_CLOSING) &&
      this->motion_direction_ != new_op) {
    this->motion_start_ms_ = millis();
    this->motion_start_position_ = this->position;
    this->motion_direction_ = new_op;
  }

  // Fin de mouvement en mode normal + arrive a l'extreme oppose = cycle complet -> calibration.
  if (mode == 0x02 && new_op == cover::COVER_OPERATION_IDLE &&
      this->motion_direction_ != cover::COVER_OPERATION_IDLE) {
    uint32_t elapsed = millis() - this->motion_start_ms_;
    // Bornes de sanite pour eviter d'enregistrer un cycle partiel/tronque.
    if (elapsed >= 3000 && elapsed <= 120000) {
      if (this->motion_direction_ == cover::COVER_OPERATION_OPENING &&
          this->motion_start_position_ == cover::COVER_CLOSED && code == 0x11) {
        if (this->cal_.open_ms != elapsed) {
          this->cal_.open_ms = elapsed;
          this->save_calibration_();
          ESP_LOGI(TAG, "Calibration OPEN: %u ms", elapsed);
        }
      } else if (this->motion_direction_ == cover::COVER_OPERATION_CLOSING &&
                 this->motion_start_position_ == cover::COVER_OPEN && code == 0x12) {
        if (this->cal_.close_ms != elapsed) {
          this->cal_.close_ms = elapsed;
          this->save_calibration_();
          ESP_LOGI(TAG, "Calibration CLOSE: %u ms", elapsed);
        }
      }
    }
    this->motion_direction_ = cover::COVER_OPERATION_IDLE;
  }

  bool changed = (this->position != new_pos) || (this->current_operation != new_op);
  if (changed) {
    ESP_LOGI(TAG, "Etat portail: %s (0x%02X)", label, code);
    this->position = new_pos;
    this->current_operation = new_op;
    this->publish_state();
    if (new_op == cover::COVER_OPERATION_IDLE) {
      this->save_position_();
    }
  }
}

void GarageDoorCover::set_obstacle_(bool detected) {
  if (this->obstacle_bs_ != nullptr) {
    this->obstacle_bs_->publish_state(detected);
  }
}

void GarageDoorCover::save_calibration_() {
  this->pref_cal_.save(&this->cal_);
}

void GarageDoorCover::save_position_() {
  this->pref_pos_.save(&this->position);
}

void GarageDoorCover::update_estimated_position_() {
  // Interpolation lineaire basee sur la calibration et le temps ecoule depuis debut du mouvement.
  if (!(this->motion_start_position_ >= 0.0f && this->motion_start_position_ <= 1.0f))
    return;
  uint32_t elapsed = millis() - this->motion_start_ms_;
  float estimated;
  if (this->current_operation == cover::COVER_OPERATION_OPENING) {
    estimated = this->motion_start_position_ + (float) elapsed / (float) this->cal_.open_ms;
  } else {
    estimated = this->motion_start_position_ - (float) elapsed / (float) this->cal_.close_ms;
  }
  if (estimated < 0.0f) estimated = 0.0f;
  if (estimated > 1.0f) estimated = 1.0f;
  if (fabsf(estimated - this->position) >= 0.01f) {
    this->position = estimated;
    this->publish_state();
  }
}

// Signature commune commande = [addr 0x00, 0x0A, 0x01, 0x28, direction].
bool GarageDoorCover::matches_signature_(const uint8_t *buf, uint8_t len,
                                         uint8_t direction) {
  return len >= 5 && buf[0] == 0x00 && buf[1] == 0x0A && buf[2] == 0x01 &&
         buf[3] == 0x28 && buf[4] == direction;
}

// Signature status controleur = [addr 0x00, 0xB4, 0xFF, 0x08, state_code].
bool GarageDoorCover::matches_status_frame_(const uint8_t *buf, uint8_t len) {
  return len >= 5 && buf[0] == 0x00 && buf[1] == 0xB4 && buf[2] == 0xFF &&
         buf[3] == 0x08;
}

}  // namespace garage_door
}  // namespace esphome
