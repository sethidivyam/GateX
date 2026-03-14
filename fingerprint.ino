// ─────────────────────────────────────────────────────────────────────────────
//  fingerprint.ino — R503 Capacitive Fingerprint Sensor
//  Sensor: M22 R503 (Adafruit library compatible, UART)
//
//  Slot ID strategy:
//    R503 stores templates in slots 1–200 internally.
//    We generate a complex slot_key (8-char hex) that maps to the physical slot.
//    slot_key is stored in cache — "FPA3C91B" is harder to guess/brute than "1".
//    Physical slot is the index (1-200), slot_key is what we log and reference.
// ─────────────────────────────────────────────────────────────────────────────

#include <Adafruit_Fingerprint.h>

// R503 uses UART2 on ESP32
HardwareSerial fp_serial(2);
Adafruit_Fingerprint finger(&fp_serial);

// ── Touch / wakeup interrupt ──────────────────────────────────────────────────
volatile bool g_fp_touch = false;
void IRAM_ATTR fp_touch_isr() { g_fp_touch = true; }

// ── Setup ────────────────────────────────────────────────────────────────────
void fp_setup() {
  fp_serial.begin(FP_BAUD, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(FP_BAUD);
  delay(100);

  if (finger.verifyPassword()) {
    Serial.println("[FP] R503 connected — password OK");
  } else {
    Serial.println("[FP] R503 NOT found — check wiring/baud");
    return;
  }

  Serial.printf("[FP] Capacity: %d  Security: %d  Status: 0x%X\n",
    finger.capacity, finger.security_level, finger.status_reg);

  // Touch wakeup pin — R503 pulls this HIGH when finger detected
  pinMode(FP_WAKEUP_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FP_WAKEUP_PIN),
                  fp_touch_isr, RISING);

  Serial.println("[FP] Ready — waiting for touch");
}

// ── Loop — called from main loop() ───────────────────────────────────────────
// Returns true if a finger event was processed (so main loop can skip rfid etc.)
bool fp_loop() {
  if (!g_fp_touch) return false;
  g_fp_touch = false;

  check_lockout_expiry();
  if (g_lockout_active) {
    Serial.println("[FP] Lockout active — ignoring touch");
    return true;
  }

  uint8_t result = fp_get_image_and_match();
  if (result == FINGERPRINT_OK) {
    uint16_t slot = finger.fingerID;
    uint16_t conf = finger.confidence;

    if (conf < 50) {
      Serial.printf("[FP] Low confidence %d — rejected (slot %d)\n", conf, slot);
      record_failed_attempt("fingerprint",
                            "slot:" + String(slot) + " conf:" + String(conf));
      return true;
    }

    String slot_key = cache_get_slot_key(slot);
    String user_id  = cache_get_user_for_fp_slot(slot);

    if (user_id.length() == 0) {
      Serial.printf("[FP] Slot %d not in cache\n", slot);
      record_failed_attempt("fingerprint", "slot:" + slot_key);
      return true;
    }

    Serial.printf("[FP] Match! slot_key=%s user=%s conf=%d\n",
                  slot_key.c_str(), user_id.c_str(), conf);
    trigger_relay(g_relay_ms);
    g_failed_attempts = 0;

    publish_state("door_opened", "fingerprint", user_id,
                  "slot_key:" + slot_key + " conf:" + String(conf),
                  true, "");
  } else if (result == FINGERPRINT_NOTFOUND) {
    Serial.println("[FP] No match found");
    record_failed_attempt("fingerprint", "no_match");
  } else if (result == FINGERPRINT_NOFINGER) {
    // Spurious wakeup — ignore silently
  } else {
    Serial.printf("[FP] Error 0x%X\n", result);
  }
  return true;
}

// ── Core: image capture + match ───────────────────────────────────────────────
uint8_t fp_get_image_and_match() {
  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return FINGERPRINT_NOFINGER;
  if (p != FINGERPRINT_OK) {
    Serial.printf("[FP] getImage error: 0x%X\n", p); return p;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    Serial.printf("[FP] image2Tz error: 0x%X\n", p); return p;
  }

  p = finger.fingerSearch();
  return p;
}

// ── Enroll new fingerprint ────────────────────────────────────────────────────
// Call from app-triggered MQTT command (not from loop)
// Returns the new slot_key on success, empty string on failure
//
// Enrollment steps:
//   1. Find next free slot (1–200)
//   2. Capture image twice (for quality)
//   3. Create template from both images
//   4. Store in sensor
//   5. Generate complex slot_key and update cache
//
String fp_enroll(String user_id) {
  int slot = fp_find_free_slot();
  if (slot < 0) {
    Serial.println("[FP] No free slots available");
    return "";
  }
  Serial.printf("[FP] Enrolling user %s into slot %d\n",
                user_id.c_str(), slot);

  // ── Step 1: First capture ────────────────────────────────────────────────
  Serial.println("[FP] Place finger on sensor...");
  uint8_t p = FINGERPRINT_NOFINGER;
  unsigned long t0 = millis();
  while (p != FINGERPRINT_OK && millis() - t0 < 10000) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) { delay(100); continue; }
    if (p != FINGERPRINT_OK) {
      Serial.printf("[FP] Capture 1 error 0x%X\n", p); return "";
    }
  }
  if (p != FINGERPRINT_OK) { Serial.println("[FP] Timeout"); return ""; }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.printf("[FP] image2Tz(1) error 0x%X\n", p); return "";
  }

  Serial.println("[FP] Lift finger...");
  delay(1500);
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(100);

  // ── Step 2: Second capture ───────────────────────────────────────────────
  Serial.println("[FP] Place same finger again...");
  p = FINGERPRINT_NOFINGER;
  t0 = millis();
  while (p != FINGERPRINT_OK && millis() - t0 < 10000) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) { delay(100); continue; }
    if (p != FINGERPRINT_OK) {
      Serial.printf("[FP] Capture 2 error 0x%X\n", p); return "";
    }
  }
  if (p != FINGERPRINT_OK) { Serial.println("[FP] Timeout"); return ""; }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.printf("[FP] image2Tz(2) error 0x%X\n", p); return "";
  }

  // ── Step 3: Create model ─────────────────────────────────────────────────
  p = finger.createModel();
  if (p == FINGERPRINT_ENROLLMISMATCH) {
    Serial.println("[FP] Fingers didn't match — try again");
    return "";
  }
  if (p != FINGERPRINT_OK) {
    Serial.printf("[FP] createModel error 0x%X\n", p); return "";
  }

  // ── Step 4: Store template ───────────────────────────────────────────────
  p = finger.storeModel(slot);
  if (p != FINGERPRINT_OK) {
    Serial.printf("[FP] storeModel error 0x%X\n", p); return "";
  }

  // ── Step 5: Generate complex slot_key ────────────────────────────────────
  // Format: "FP" + 6-char hex built from slot + random entropy + millis()
  // Not sequential — looks like "FPA3C91B", "FP5E2D8F", "FP0B7C33"
  uint32_t entropy = (uint32_t)slot * 0x6B7F3D1B
                   ^ (uint32_t)esp_random()
                   ^ (uint32_t)(millis() & 0xFFFFFF);
  char slot_key[9];
  snprintf(slot_key, sizeof(slot_key), "FP%06X", entropy & 0xFFFFFF);

  // Update cache: fingerprint.slot_map[slot_key] = { slot, user_id }
  cache_add_fp_slot(slot, String(slot_key), user_id);
  cache_save();

  // Tell cloud to update shadow / lock document
  fp_publish_enroll_event(slot, String(slot_key), user_id);

  Serial.printf("[FP] Enrolled: slot=%d  slot_key=%s  user=%s\n",
                slot, slot_key, user_id.c_str());
  return String(slot_key);
}

// ── Delete fingerprint ────────────────────────────────────────────────────────
bool fp_delete(String slot_key) {
  int slot = cache_get_physical_slot(slot_key);
  if (slot < 0) {
    Serial.printf("[FP] slot_key %s not found in cache\n", slot_key.c_str());
    return false;
  }

  uint8_t p = finger.deleteModel(slot);
  if (p != FINGERPRINT_OK) {
    Serial.printf("[FP] deleteModel(%d) error 0x%X\n", slot, p);
    return false;
  }

  cache_remove_fp_slot(slot_key);
  cache_save();
  Serial.printf("[FP] Deleted slot_key=%s (slot=%d)\n", slot_key.c_str(), slot);
  return true;
}

// ── Find free slot (1–200) ────────────────────────────────────────────────────
int fp_find_free_slot() {
  // Get used slots from cache
  for (int s = FP_SLOT_MIN; s <= FP_SLOT_MAX; s++) {
    if (!cache_is_fp_slot_used(s)) return s;
  }
  return -1;  // all full
}

// ── Publish enroll event ──────────────────────────────────────────────────────
void fp_publish_enroll_event(int slot, String slot_key, String user_id) {
  StaticJsonDocument<256> doc;
  doc["event"]    = "fingerprint_enrolled";
  doc["slot"]     = slot;
  doc["slot_key"] = slot_key;
  doc["user_id"]  = user_id;
  doc["timestamp"]= get_iso_timestamp();
  String out; serializeJson(doc, out);
  if (mqtt.connected()) mqtt.publish(TOPIC_STATE, out.c_str(), false);
}

// ── Cache interface (implemented in control.ino) ──────────────────────────────
// cache_get_slot_key(int physical_slot)  → String slot_key
// cache_get_user_for_fp_slot(int slot)   → String user_id
// cache_get_physical_slot(String key)    → int slot
// cache_is_fp_slot_used(int slot)        → bool
// cache_add_fp_slot(slot, key, user_id)
// cache_remove_fp_slot(String key)
