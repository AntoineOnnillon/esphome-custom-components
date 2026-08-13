# Protocole Atlantic — notes de reverse engineering

Document vivant : chaque decouverte est ajoutee ici.
Derniere mise a jour : 2026-08-13 (routage wire vs. applicatif decouvert)

## 1. Couche physique

- UART **4800 bauds, 8O2** (8 bits, parite ODD, 2 stops).
- Signaux **inverses** cote ESP (`inverted: true` sur `rx_pin` et `tx_pin`), donc le premier
  octet lu par ESPHome est `0x23` = `~0xDC`.

## 2. Trame filaire (apres inversion cote soft)

```
0xDC | 0x80|sender_wire | dest_wire | frame_len | payload... | crc16_hi | crc16_lo
```

- `sender_wire` = **toujours 0x00** dans nos captures → le maitre du bus (arbitre) relaie
  toutes les trames applicatives ; l'identite reelle de l'emetteur est dans `payload[1]`.
- `dest_wire` : notre adresse (par defaut `0x07`), `0x7F` = broadcast.
- `frame_len` : longueur totale de la trame (incluant header 4 octets + payload + CRC 2 octets).
- **CRC16 XMODEM** (polynome `0x1021`, seed 0) sur les octets `header + payload`.

## 3. Payload applicatif

```
opcode | src_module | dst_module | reg_hi | reg_lo | data... 
```

### Opcodes connus

| Op | Sens | Notes |
|----|------|-------|
| `0x02` | notify (broadcast periodique ou event) | Emis par un module vers `dst_module=0x00` (maitre) |
| `0x03` | write | On l'utilise pour ecrire mode + setpoint |
| `0x06` | read request | On l'emet vers un module pour lire un registre |
| `0x07` | read reply | Reponse a `0x06` : contient les donnees |
| `0x08` | NACK / registre inconnu | Reponse quand le module ne connait pas le registre demande |

### Modules identifies (`src_module`)

| Adresse | Role |
|---------|------|
| `0x00` | Maitre / gateway (adresse de destination des notify) |
| `0x05` | Panneau de controle (ecran + sonde ambiante) |
| `0x2D` | Chaudiere / regulateur — repond aux `0x06` |
| `0x2E`, `0x2F`, `0x31` | Modules secondaires (contenus toujours `FF FF ...` = vides) |
| `0x3D` | Notre identite applicative (utilisee comme `src_module` dans nos requetes) |
| `0x07` | Notre adresse wire (`address:` dans le YAML) |

## 4. Encodages

### 4.1 Temperature "setpoint" (registre `0x058E`)

```
raw16 = (T_celsius * 2) << 5   =>   T = (raw >> 5) / 2   (equivalent T = raw / 64)
```

Resolution 0.5°C, 11 bits utiles ; les 5 bits de poids faible portent probablement
un flag/checksum non exploite.

### 4.2 Autres encodages hypothetiques a tester

- `raw / 100` — temperature au 0.01°C
- `raw / 10` — temperature au 0.1°C
- `raw / 16` — Q4 fixed-point
- `raw / 8` — Q3 fixed-point (0.125°C)

## 5. Registres identifies (module `0x2D`, sauf indication contraire)

Legende encodage : `S` = setpoint (`/64`), `?` = a confirmer.

| Reg | Description | Encodage | Exemple | Notes |
|-----|-------------|----------|---------|-------|
| `0x0211` | Mode + preset (broadcast module 0x2D) | Bitcode | `0x0000`=OFF, `0x0302`=HEAT+CONFORT, `0x0201`=HEAT+ECO, `0x0102/0x0104`=AUTO+CONFORT, `0x0101/0x0103`=AUTO+ECO | 8 octets trailer inconnus |
| `0x0212` | Mode module `0x31` | ? | `00 00 00` | Toujours zero |
| `0x0213` | ? panneau | ? | `00 00 42 01` | Stable a travers sessions |
| `0x0219` | Compteur monotone (panneau) | Compteur | byte[6] ~+1.6/min | **N'est PAS la temperature** |
| `0x0248` | Etat panneau, 22o | ? | contient `D1 00 08 00 02` | Stable, byte `D1` = ? |
| `0x0459` | ⭐ Sonde inconnue (evaporateur ? PAC arret) | signed `/64` ? | `01 FF 40` → -3.0°C en aout | byte[5]=`01` = flag actif ? A re-tester en hiver |
| `0x04B7` | ? | ? | `00 00` (2o) | Zero a l'arret |
| `0x04BB` | ? | ? | `00 00` (2o) | Zero a l'arret |
| `0x04C2` | ? | ? | `00 00` (2o) | Zero a l'arret |
| `0x051E` | ✅ **Sonde ambiante (raw)** | `/64` | Point A: `0x075A` → 29.41°C (panneau 29.4°C). Point B: `0x070A` → 28.16°C (panneau 28.1°C) | **Confirme** — pente ~64.0 sur 2 points |
| `0x056A` | ✅ **Sonde ambiante (filtre)** | `/64` | Converge vers 0x051E quand stable ; diverge de ~0.1°C en transitoire | Copie filtree de 0x051E — utile aussi comme fallback |
| `0x0574` | Flag | ? | `00 00` | |
| `0x058E` | ✅ Setpoint utilisateur | `(raw>>5)/2` (5 bits bas = flags) | `0x0540` = 21.0°C, `0x0580` = 22.0°C, `0x0500` = 20.0°C | Ecriture via `0x03` |
| `0x0590` | Setpoint reduit / ECO ? | `S` | `0x0480` → 18.0°C | |
| `0x0592` | Setpoint antigel ? | `S` | `0x0200` → 8.0°C | |
| `0x0593` | Idem 0x0592 | `S` | `0x0200` → 8.0°C | Copie |
| `0x059D` | Offset / correction ? | signe /64 ? | `0xFEC0` → -5.0°C | |
| `0x059E` | Flags | — | `0x010000` | 3 octets de donnees |
| `0x05A5` | Setpoint confort max ? | `S` | `0x0700` → 28.0°C | Correspond a la borne visuelle |
| `0x05B8` | Statut | — | `0x04` | |
| `0x05E8` | Flag | — | `00` | |
| `0x05E9` | Copie setpoint courant ? | `S` | `0x0590` → 22.25°C | Proche du setpoint |
| `0x05F3` | Flag | — | `0x0000` | |
| `0x05F6` | Compteur / temp basse ? | ? | `0x0019` → 25 | |
| `0x05FD` | Setpoint reduit copie | `S` | `0x0480` → 18.0°C | Identique a 0x0590 |

## 6. Ecriture connue (opcode `0x03`)

### 6.1 Setpoint (`reg=0x058E`)

Payload : `03 3D 2D 05 8E 01 <raw_hi> <raw_lo>` avec `raw = (T*2) << 5`.

### 6.2 Mode / preset

Payload : `03 3D 2D 05 <sub> 01 <code>` ou :
- `<sub> = 0x74` en mode manuel/OFF, `0x72` en mode AUTO
- `<code>` :
  - `0x00` = OFF
  - HEAT : `0x03` = CONFORT, `0x02` = ECO
  - AUTO : `0x02` = CONFORT, `0x01` = ECO/NONE

## 7. Requete de lecture (opcode `0x06`)

Payload : `06 <notre_id=0x3D> <cible> <reg_hi> <reg_lo>`.

- Cible `0x2D` (chaudiere) : repond avec `0x07` sur registres connus, `0x08` sinon.
- Cible `0x05` (panneau) : semble ne repondre qu'avec `0x08` (opcode `0x06` non supporte cote panneau).

## 8. Questions ouvertes

- **Difference `0x051E` vs `0x056A`** : quelle est la constante de temps du filtre ? Ecart de 0.1°C observe en transitoire (probablement moyennage mobile).
- **Temperature exterieure** (loi d'eau) : registre inconnu — `0x0590` (18°C) ne varie pas comme une sonde exterieure devrait.
- **Etat brûleur / puissance modulee** : pas identifie.
- **Autres modules** : `0x2E`, `0x2F`, `0x31` — role reel ? Toujours `FF FF ...` = deconnectes ?
- **8 octets trailer** de `reg=0x0211` (`24 33 63 8A FF FF 00 01`) : Que codent-ils ?
- **`reg=0x0248`** (`D1 00 08 00 02`) : que codent ces octets sur le panneau ?
- **Signification des 5 bits bas** de la temperature ambiante (`raw & 0x1F`) : `26` vs `19` observes — flags de validite/precision ?

## 9. Prochaines etapes

- [x] Confirmer que `0x051E` suit bien la sonde ambiante en variant de plusieurs °C. *(fait : 2 points confirment pente ~64)*
- [x] Ecrire un parser natif publiant `this->current_temperature`. *(fait)*
- [ ] **Sweep etendu chaudiere 0x2D** (roadmap phase 1) :
  - [ ] Plage `0x0400-0x04FF` — sondes eau (depart/retour), etat compresseur, puissance modulee
  - [ ] Plage `0x0300-0x03FF` — configuration / parametres constructeur
  - [ ] Plage `0x0600-0x06FF` — stats, compteurs (heures, cycles, energie)
  - [ ] Plage `0x0100-0x02FF` — registres systeme
- [ ] **Sweep autres modules** (phase 2) : `0x2E`, `0x2F`, `0x31` (probablement vides)
- [ ] **Test opcodes non standards** (phase 3) : `0x04`, `0x05`, `0x09`
- [ ] Identifier un registre "demande de chauffe" pour piloter directement sans passer par le setpoint.
- [ ] Verifier si `0x058E` accepte des valeurs hors [19, 28] (bornes visuelles actuelles) — utile pour la modulation.

## 10. Cibles prioritaires du reverse engineering

Ce qu'on veut ajouter comme sensors/donnees exploitables :

| Grandeur | Utilite | Encodage attendu |
|----------|---------|------------------|
| Sonde exterieure | Loi d'eau, correction meteo | signe `/64` ou `/16` |
| Temperature eau depart | Monitoring, protection | `/64` ou `/16` (peut etre chaud, 30-70°C) |
| Temperature eau retour | Delta T PAC | idem |
| Temperature evaporateur | Detection degivrage | signe (peut etre negatif) |
| Puissance compresseur | Suivi COP | pourcentage 0-100 (`/1`) |
| Etat vanne 4V | Chaud/degivrage | booleen |
| Etat pompe circulation | Debug demarrage | booleen |
| Code defaut | Alerte HA | uint16 |
| Heures fonctionnement | Maintenance | uint32 |
| Nombre demarrages compresseur | Usure | uint32 |

## 11. Methode de tri des reponses du sweep

Pour chaque `[sniff NEW] op=0x07(read-reply) reg=0xXXXX`, appliquer cette grille :

1. **Valeur constante** (payload identique a chaque probe) → registre de **configuration** ou setpoint
2. **Valeur qui varie lentement** (~toutes les X min) → **sonde de temperature** ou statut evolutif
3. **Valeur qui oscille** entre 2-3 etats discrets → **flag** (marche/arret, mode operationnel)
4. **Valeur qui monte de facon monotone** → **compteur** (heures, cycles)
5. **`/64` donne une temp plausible** → sonde de temperature (`/64` a 0.5°C ou 0.016°C res)
6. **`/16` donne un pourcentage** → puissance / debit / ouverture vanne
7. **Bit unique** dans un octet → flag / etat binaire

## 12. Impersonation / emission depuis l'ESP

L'ESP est deja reconnu comme **client** (nos `0x06`/`0x03` sont acceptes). Pour aller plus loin
et devenir un **module a part entiere** qui broadcast ses propres notify `0x02`, on a expose
plusieurs primitives dans le composant :

- `send_raw_payload(vec)` — payload applicatif brut, header+CRC calcules automatiquement
- `broadcast_notify(src_module, reg, data)` — trame `02 src 00 reg_hi reg_lo <data>`
- `write_register_as(src_module, target, reg, data)` — trame `03 src target reg_hi reg_lo <data>` (write depuis fake src)
- `probe_register_as(src_module, target, reg)` — trame `06 src target reg_hi reg_lo` (read depuis fake src)
- `impersonate_ambient(src_module, temperature)` — helper pour spoofer `reg=0x051E`
- `emulate_panel_burst(src_module, counter, state_flag)` — rejoue les 3 notify observees du panneau (0x0213 + 0x0219 + 0x0248)

### 12.1 Sniff promiscue (`sniff_all_frames: true`)

Par defaut, on ne recoit que les trames dont le `wire_dst` == notre adresse (0x07) ou 0x7F.
Avec `sniff_all_frames: true`, on capture **toutes les trames du bus**, incluant :

- `panel (0x05) -> chaudiere (0x2D)` : la vraie ecriture de la sonde ambiante par le panneau
- `chaudiere -> module fake` : reponse d'un `probe_register_as(0x40, ...)`
- Handshakes de boot / negociation d'adresse

Les parsers d'etat climate ne tournent **que** sur les trames pour nous (evite de polluer
`current_temperature` / `target` avec les valeurs destinees a un autre module). Le sniff
log a maintenant `wire=SRC->DST` en prefixe pour distinguer.

### 12.2 Adresses disponibles a tester comme identite emettrice

| Adresse | Statut observe | Risque |
|---------|----------------|--------|
| `0x40+` | Jamais vue → adresse libre | ⭐ safe |
| `0x2E` | Actif mais vide (broadcast FF...) | ⭐⭐ risque conflit avec le vrai module |
| `0x2F` | Idem 0x2E | ⭐⭐ |
| `0x31` | Actif, broadcast 0x0212 sur reg 0x0212 | ⭐⭐⭐ risque conflit |
| `0x05` | **Panneau physique !** | ⭐⭐⭐⭐ conflit direct, DANGER |
| `0x2D` | **Chaudiere elle-meme** | ⛔ INTERDIT |

### 12.3 Roadmap experimentation (boutons YAML fournis)

- [x] **Phase 0 (baseline)** : `sniff_all_frames: true` + 2-3 min d'observation ->
  lister les nouvelles trames panel<->chaudiere jamais vues avant (probablement une
  ecriture 0x03 depuis 0x05 vers 0x2D sur reg 0x051E ou proche).
  ⚠ *Constate : quand le systeme est **Mode:OFF**, le panneau et 0x2D ne broadcastent
  quasiment plus. Refaire l'observation en Mode:HEAT.*
- [x] **Phase A** : bouton `Test A - Fake 0x40 notify 0x0211` (mode HEAT+CONFORT).
  *(2026-08-13 : trame emise OK, aucune reaction visible de la chaudiere)*
- [x] **Phase A bis** : bouton `Test A - Fake 0x40 notify 0x0212`. *(idem: silence)*
- [x] **Phase B panel** : bouton `Test B - Fake 0x40 panel burst`.
  *(2026-08-13 : 2 bursts, aucun handshake / write vers 0x40 en retour)*
- [x] **Phase B read** : `Test B - Fake 0x40 probe 0x051E`.
  ✅ **DECOUVERTE CLE** : la chaudiere repond a notre probe fake avec `app_dst=0x40`
  (`07 2D 40 05 1E 00 06 AD`). Elle traite 0x40 comme un module legitime pour les reads.
  ⚠ En revanche, le probe `0x058E` avec src=0x40 **n'a pas provoque de reponse visible**
  (a re-tester : peut-etre filtre par identite, peut-etre dedup si valeur inchangee).
- [ ] **Phase C conflit** : bouton `Test C - Impersonate 0x2E notify 0x0212`.
  Attendu : la valeur qu'on injecte remplace-t-elle le broadcast du vrai module ?
- [ ] **Phase D (dangereux)** : bouton `Test D - Wire-spoof 0x05 notify 0x0213` disponible.
  Emet avec `wire_sender=0x05` (celui du panneau physique). Si le maitre route les
  replies au *vrai panneau* au lieu de nous -> confirmation que le routage physique
  est bien base sur le wire_sender du dernier message. **Faire uniquement panneau debranche**.
- [ ] **Phase E** : capturer les frames du boot de la chaudiere (couper/remettre alim)
  pour identifier un eventuel handshake permettant d'enregistrer notre propre ID.

### 12.4 Routage wire vs. applicatif (decouverte 2026-08-13)

Les tests phase B ont revele que **le routage physique est decorrele du routage applicatif** :

| Champ | Role |
|---|---|
| `wire_sender` (header, bit 7 fixe a 1) | Adresse physique de l'emetteur *sur le bus UART* |
| `wire_dst` (header) | Adresse physique du destinataire — utilisee par le maitre pour router |
| `payload src` (app_src) | Identite *logique* du module emetteur (peut etre spoofee librement) |
| `payload dst` (app_dst) | Identite *logique* du destinataire — le module cible verifie que c'est bien lui |

Consequence :

- Depuis notre wire address `0x07`, on peut emettre un `read` avec `app_src=0x40` : la
  chaudiere traite la requete comme venant de `0x40`, met `app_dst=0x40` dans la reply,
  mais le maitre relaie physiquement avec `wire_dst=0x07` (nous).
- Pour intercepter une reply destinee a un **autre** module physique (ex: panneau `0x05`),
  il faut spoofer aussi le `wire_sender` du header (helper `send_raw_payload_with_wire_src`).
- La chaudiere semble ne repondre aux `0x06` que sur certains registres selon l'identite
  (`0x051E` OK depuis 0x40, `0x058E` a re-verifier).
