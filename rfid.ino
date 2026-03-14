// ─────────────────────────────────────────────────────────────────────────────
//  rfid.ino — RC522 RFID Reader
//  Handles card tap, UID whitelist check, publish state
// ─────────────────────────────────────────────────────────────────────────────

#include <SPI.h>
#include <MFRC522.h>

MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);

// Anti-spam: don't re-fire for same card within this window
#define RFID_SAME_CARD_COOLDOWN_MS  3000
static String   g_last_rfid_uid    = "";
static unsigned long g_last_rfid_ms = 0;

// ── Setup ────────────────────────────────────────────────────────────────────
void rfid_setup() {
  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(MFRC522::RxGain_max);
  delay(50);

  byte v = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
  if (v == 0x00 || v == 0xFF) {
    Serial.println("[RFID] RC522 not found — check SPI wiring");
  } else {
    Serial.printf("[RFID] RC522 v0x%02X ready\n", v);
  }
}

// ── Loop — called from main loop() ───────────────────────────────────────────
bool rfid_loop() {
  if (!mfrc522.PICC_IsNewCardPresent()) return false;
  if (!mfrc522.PICC_ReadCardSerial())   return false;

  String uid = rfid_uid_to_string(mfrc522.uid.uidByte, mfrc522.uid.size);
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  // Cooldown: same card tapped repeatedly
  if (uid == g_last_rfid_uid &&
      millis() - g_last_rfid_ms < RFID_SAME_CARD_COOLDOWN_MS) {
    return true;
  }
  g_last_rfid_uid = uid;
  g_last_rfid_ms  = millis();

  Serial.printf("[RFID] Card UID: %s\n", uid.c_str());

  check_lockout_expiry();
  if (g_lockout_active) {
    Serial.println("[RFID] Lockout active — denied");
    return true;
  }

  String user_id = cache_get_user_for_rfid(uid);

  if (user_id.length() == 0) {
    Serial.printf("[RFID] UID %s not in whitelist\n", uid.c_str());
    record_failed_attempt("rfid", "uid:" + uid);
    return true;
  }

  // Build complex rfid_key for logging — same approach as fingerprint
  // Format: "RF" + 6-char hex from uid hash
  String rfid_key = rfid_uid_to_key(uid);

  Serial.printf("[RFID] Allowed: uid_key=%s user=%s\n",
                rfid_key.c_str(), user_id.c_str());
  trigger_relay(g_relay_ms);
  g_failed_attempts = 0;

  publish_state("door_opened", "rfid", user_id,
                "uid_key:" + rfid_key, true, "");
  return true;
}

// ── UID → readable string ─────────────────────────────────────────────────────
// "A3:F2:11:CC" format — consistent across cards
String rfid_uid_to_string(byte *buf, byte len) {
  String uid = "";
  for (byte i = 0; i < len; i++) {
    if (i) uid += ":";
    if (buf[i] < 0x10) uid += "0";
    uid += String(buf[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// ── UID → complex log key (not the raw UID) ───────────────────────────────────
// We log rfid_key, not the raw UID — slight obfuscation in logs
// "A3:F2:11:CC" → "RF" + djb2-like hash → "RF8B4C1A"
String rfid_uid_to_key(String uid) {
  uint32_t h = 5381;
  for (int i = 0; i < (int)uid.length(); i++) {
    h = ((h << 5) + h) + uid[i];
  }
  char key[9];
  snprintf(key, sizeof(key), "RF%06X", h & 0xFFFFFF);
  return String(key);
}

// ── Register new RFID card ────────────────────────────────────────────────────
// Called when app sends "rfid_add" command — wait for card tap
// Returns uid_key on success
String rfid_add(String user_id) {
  Serial.println("[RFID] Waiting for new card tap (10s)...");
  unsigned long t0 = millis();

  while (millis() - t0 < 10000) {
    if (!mfrc522.PICC_IsNewCardPresent()) { delay(100); continue; }
    if (!mfrc522.PICC_ReadCardSerial())   { delay(100); continue; }

    String uid = rfid_uid_to_string(mfrc522.uid.uidByte, mfrc522.uid.size);
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    // Check not already registered
    if (cache_get_user_for_rfid(uid).length() > 0) {
      Serial.printf("[RFID] UID %s already registered\n", uid.c_str());
      return "";
    }

    String uid_key = rfid_uid_to_key(uid);
    cache_add_rfid(uid, uid_key, user_id);
    cache_save();

    rfid_publish_add_event(uid_key, user_id);

    Serial.printf("[RFID] Added: uid=%s uid_key=%s user=%s\n",
                  uid.c_str(), uid_key.c_str(), user_id.c_str());
    return uid_key;
  }

  Serial.println("[RFID] Timeout — no card presented");
  return "";
}

// ── Remove RFID card ──────────────────────────────────────────────────────────
bool rfid_remove(String uid_key) {
  bool ok = cache_remove_rfid(uid_key);
  if (ok) {
    cache_save();
    Serial.printf("[RFID] Removed uid_key=%s\n", uid_key.c_str());
  }
  return ok;
}

// ── Publish add event ─────────────────────────────────────────────────────────
void rfid_publish_add_event(String uid_key, String user_id) {
  StaticJsonDocument<256> doc;
  doc["event"]    = "rfid_added";
  doc["uid_key"]  = uid_key;
  doc["user_id"]  = user_id;
  doc["timestamp"]= get_iso_timestamp();
  String out; serializeJson(doc, out);
  if (mqtt.connected()) mqtt.publish(TOPIC_STATE, out.c_str(), false);
}
