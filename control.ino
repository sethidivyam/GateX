// ─────────────────────────────────────────────────────────────────────────────
//  control.ino — Hardware Control + SPIFFS Cache + Offline Queue
//  Handles: relay, door sensor (reed switch), tamper, doorbell button,
//           battery ADC, SPIFFS cache load/save, offline event queue
// ─────────────────────────────────────────────────────────────────────────────

#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>   // ESP32 hardware SHA256

// ── Cache document (shared across all .ino files) ────────────────────────────
JsonDocument g_cache;           // loaded from SPIFFS on boot
int     g_relay_ms        = RELAY_DURATION_MS;
bool    g_offline_enabled = true;

// ── Door state tracking ───────────────────────────────────────────────────────
typedef enum {
  DOOR_CLOSED  = 0,
  DOOR_OPEN    = 1,
  DOOR_AJAR    = 2,   // relay done but sensor still open
  DOOR_UNKNOWN = 3
} DoorState;

static DoorState  g_door_state       = DOOR_UNKNOWN;
static DoorState  g_door_state_prev  = DOOR_UNKNOWN;
static unsigned long g_door_opened_at = 0;
static bool       g_ajar_alerted     = false;

// ── Relay state ───────────────────────────────────────────────────────────────
static bool       g_relay_running = false;
static unsigned long g_relay_end_ms = 0;

// ── Tamper ────────────────────────────────────────────────────────────────────
static unsigned long g_tamper_last_ms = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  CONTROL SETUP
// ─────────────────────────────────────────────────────────────────────────────
void control_setup() {
  // Relay
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? LOW : HIGH);  // off

  // Door sensor
  pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);

  // Doorbell
  pinMode(DOORBELL_PIN, INPUT_PULLUP);

  // Tamper
  pinMode(TAMPER_PIN, INPUT_PULLUP);

  // LED
  pinMode(LED_PIN, OUTPUT);
  led_blink(3, 80);  // 3 quick blinks = booted

  Serial.println("[CTL] Control pins initialised");
}

// ─────────────────────────────────────────────────────────────────────────────
//  CONTROL LOOP — called every main loop() tick
// ─────────────────────────────────────────────────────────────────────────────
void control_loop() {
  relay_loop();
  door_sensor_loop();
  doorbell_loop();
  tamper_loop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  RELAY
// ─────────────────────────────────────────────────────────────────────────────
void trigger_relay(int duration_ms) {
  if (g_relay_running) return;  // already open, ignore

  Serial.printf("[CTL] Relay ON — %d ms\n", duration_ms);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? HIGH : LOW);
  g_relay_running = true;
  g_relay_end_ms  = millis() + duration_ms;
  led_on();
}

void relay_loop() {
  if (!g_relay_running) return;
  if (millis() >= g_relay_end_ms) {
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_HIGH ? LOW : HIGH);
    g_relay_running = false;
    led_off();
    Serial.println("[CTL] Relay OFF");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  DOOR SENSOR (Reed Switch)
// ─────────────────────────────────────────────────────────────────────────────
void door_sensor_loop() {
  bool raw_closed = (digitalRead(DOOR_SENSOR_PIN) == DOOR_CLOSED_STATE);
  DoorState new_state;

  if (raw_closed) {
    new_state = DOOR_CLOSED;
  } else {
    // Door is open — is it ajar (relay already done)?
    new_state = (g_door_state == DOOR_OPEN && !g_relay_running)
                ? DOOR_AJAR : DOOR_OPEN;
  }

  if (new_state == g_door_state_prev) {
    // Same state — check ajar timeout
    if (g_door_state == DOOR_OPEN || g_door_state == DOOR_AJAR) {
      if (!g_ajar_alerted &&
          millis() - g_door_opened_at > (DOOR_OPEN_ALERT_SEC * 1000UL)) {
        g_ajar_alerted = true;
        Serial.println("[CTL] Door left open — alerting");
        publish_state("door_ajar","auto","","",true,"");
      }
    }
    return;
  }

  // State changed
  DoorState prev = g_door_state;
  g_door_state      = new_state;
  g_door_state_prev = new_state;

  if (new_state == DOOR_OPEN) {
    g_door_opened_at = millis();
    g_ajar_alerted   = false;
    Serial.println("[CTL] Door OPEN");
    // door_opened event is published from the trigger source (fp/rfid/cmd)
    // Here we just track state
  } else if (new_state == DOOR_CLOSED) {
    Serial.println("[CTL] Door CLOSED");
    g_ajar_alerted = false;
    publish_door_closed_event(prev);
  } else if (new_state == DOOR_AJAR) {
    Serial.println("[CTL] Door AJAR — relay done, door still open");
  }
}

void publish_door_closed_event(DoorState prev) {
  String before = door_state_str(prev);
  StaticJsonDocument<256> doc;
  doc["event"]             = "door_closed";
  doc["door_state_before"] = before;
  doc["door_state_after"]  = "closed";
  doc["timestamp"]         = get_iso_timestamp();
  String out; serializeJson(doc, out);
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_STATE, out.c_str(), false);
  } else {
    log_event_offline("door_closed","auto","",true);
  }
}

String door_state_str(DoorState s) {
  switch(s) {
    case DOOR_CLOSED:  return "closed";
    case DOOR_OPEN:    return "open";
    case DOOR_AJAR:    return "ajar";
    default:           return "unknown";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  DOORBELL BUTTON
// ─────────────────────────────────────────────────────────────────────────────
static bool g_doorbell_prev = HIGH;
static unsigned long g_doorbell_debounce = 0;

void doorbell_loop() {
  bool cur = digitalRead(DOORBELL_PIN);
  if (cur == g_doorbell_prev) return;

  if (millis() - g_doorbell_debounce < DOORBELL_DEBOUNCE_MS) {
    g_doorbell_prev = cur;
    return;
  }
  g_doorbell_debounce = millis();
  g_doorbell_prev = cur;

  if (cur == LOW) {  // button pressed (active LOW with pullup)
    if (millis() - g_last_doorbell_ms < DOORBELL_COOLDOWN_MS) {
      Serial.println("[CTL] Doorbell cooldown active");
      return;
    }
    g_last_doorbell_ms = millis();
    Serial.println("[CTL] Doorbell pressed");
    led_blink(2, 150);
    // Camera capture + S3 upload would happen here via HTTP
    // For now publish without image URL
    String image_url = "";  // replace with actual S3 upload result
    if (mqtt.connected()) {
      publish_doorbell(image_url);
    } else {
      log_event_offline("doorbell","button","",true);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  TAMPER SENSOR
// ─────────────────────────────────────────────────────────────────────────────
static bool g_tamper_prev = HIGH;

void tamper_loop() {
  bool cur = digitalRead(TAMPER_PIN);
  if (cur == g_tamper_prev) return;

  if (millis() - g_tamper_last_ms < TAMPER_DEBOUNCE_MS) {
    g_tamper_prev = cur;
    return;
  }
  g_tamper_last_ms = millis();
  g_tamper_prev = cur;

  if (cur == LOW) {  // tamper triggered
    Serial.println("[CTL] TAMPER DETECTED");
    led_blink(5, 50);
    publish_state("tamper","auto","","",true,"");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  BATTERY ADC (optional — if battery connected to ADC pin)
// ─────────────────────────────────────────────────────────────────────────────
// Assumes 18650 cell via voltage divider on ADC pin
// Tune BATT_FULL_MV and BATT_EMPTY_MV to your cell chemistry
#define BATT_ADC_PIN   34
#define BATT_FULL_MV   4200
#define BATT_EMPTY_MV  3000
#define BATT_R1        100   // kΩ top resistor
#define BATT_R2        100   // kΩ bottom resistor
#define ADC_REF_MV     3300
#define ADC_MAX        4095

int read_battery_percent() {
  int raw = analogRead(BATT_ADC_PIN);
  int vdiv_mv = (raw * ADC_REF_MV) / ADC_MAX;
  int batt_mv = vdiv_mv * (BATT_R1 + BATT_R2) / BATT_R2;
  int pct = map(batt_mv, BATT_EMPTY_MV, BATT_FULL_MV, 0, 100);
  return constrain(pct, 0, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
//  LED HELPERS
// ─────────────────────────────────────────────────────────────────────────────
void led_on()  { digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? HIGH : LOW); }
void led_off() { digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? LOW : HIGH);  }
void led_blink(int times, int ms) {
  for (int i = 0; i < times; i++) {
    led_on(); delay(ms); led_off(); delay(ms);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SHA256 HELPER (for offline PIN verify)
// ─────────────────────────────────────────────────────────────────────────────
String sha256_hex(String input) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx,
    (const unsigned char*)input.c_str(), input.length());
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  String hex = "";
  for (int i = 0; i < 32; i++) {
    if (hash[i] < 0x10) hex += "0";
    hex += String(hash[i], HEX);
  }
  return hex;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CACHE — SPIFFS load / save
// ─────────────────────────────────────────────────────────────────────────────
void cache_load() {
  if (!SPIFFS.exists(CACHE_FILE)) {
    Serial.println("[Cache] No cache file — using defaults");
    g_cache["lock_id"]         = LOCK_ID;
    g_cache["cache_version"]   = 0;
    g_cache["relay_ms"]        = RELAY_DURATION_MS;
    g_cache["offline_enabled"] = true;
    g_cache.createNestedObject("fingerprint")["slot_map"];
    g_cache.createNestedObject("rfid")["uid_map"];
    g_cache.createNestedArray("pins");
    g_cache.createNestedArray("offline_queue");
    cache_save();
    return;
  }

  File f = SPIFFS.open(CACHE_FILE, "r");
  DeserializationError err = deserializeJson(g_cache, f);
  f.close();

  if (err) {
    Serial.printf("[Cache] Parse error: %s — resetting\n", err.c_str());
    SPIFFS.remove(CACHE_FILE);
    cache_load();
    return;
  }

  g_relay_ms        = g_cache["relay_ms"]        | RELAY_DURATION_MS;
  g_offline_enabled = g_cache["offline_enabled"] | true;

  int ver = g_cache["cache_version"] | 0;
  Serial.printf("[Cache] Loaded v%d from SPIFFS\n", ver);
}

void cache_save() {
  File f = SPIFFS.open(CACHE_FILE, "w");
  if (!f) { Serial.println("[Cache] SPIFFS write failed"); return; }
  serializeJson(g_cache, f);
  f.close();
  Serial.println("[Cache] Saved to SPIFFS");
}

// ─────────────────────────────────────────────────────────────────────────────
//  CACHE — Fingerprint helpers
// ─────────────────────────────────────────────────────────────────────────────
// slot_map in cache: { "FPA3C91B": { "slot": 1, "user_id": "USR-xxx" }, ... }

String cache_get_slot_key(uint16_t physical_slot) {
  JsonObject slot_map = g_cache["fingerprint"]["slot_map"];
  for (JsonPair kv : slot_map) {
    if (kv.value()["slot"] == physical_slot) {
      return String(kv.key().c_str());
    }
  }
  return "";
}

String cache_get_user_for_fp_slot(uint16_t physical_slot) {
  JsonObject slot_map = g_cache["fingerprint"]["slot_map"];
  for (JsonPair kv : slot_map) {
    if (kv.value()["slot"] == physical_slot) {
      return kv.value()["user_id"] | "";
    }
  }
  return "";
}

int cache_get_physical_slot(String slot_key) {
  if (g_cache["fingerprint"]["slot_map"].containsKey(slot_key.c_str())) {
    return g_cache["fingerprint"]["slot_map"][slot_key.c_str()]["slot"] | -1;
  }
  return -1;
}

bool cache_is_fp_slot_used(int physical_slot) {
  JsonObject slot_map = g_cache["fingerprint"]["slot_map"];
  for (JsonPair kv : slot_map) {
    if (kv.value()["slot"] == physical_slot) return true;
  }
  return false;
}

void cache_add_fp_slot(int slot, String slot_key, String user_id) {
  JsonObject entry = g_cache["fingerprint"]["slot_map"]
                              .createNestedObject(slot_key.c_str());
  entry["slot"]    = slot;
  entry["user_id"] = user_id;
}

void cache_remove_fp_slot(String slot_key) {
  g_cache["fingerprint"]["slot_map"].remove(slot_key.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  CACHE — RFID helpers
// ─────────────────────────────────────────────────────────────────────────────
// uid_map: { "A3:F2:11:CC": { "uid_key": "RF8B4C1A", "user_id": "USR-xxx" }, ... }

String cache_get_user_for_rfid(String uid) {
  if (!g_cache["rfid"]["uid_map"].containsKey(uid.c_str())) return "";
  return g_cache["rfid"]["uid_map"][uid.c_str()]["user_id"] | "";
}

void cache_add_rfid(String uid, String uid_key, String user_id) {
  JsonObject entry = g_cache["rfid"]["uid_map"]
                              .createNestedObject(uid.c_str());
  entry["uid_key"] = uid_key;
  entry["user_id"] = user_id;
}

bool cache_remove_rfid(String uid_key) {
  JsonObject uid_map = g_cache["rfid"]["uid_map"];
  for (JsonPair kv : uid_map) {
    if (kv.value()["uid_key"] == uid_key) {
      uid_map.remove(kv.key());
      return true;
    }
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CACHE — PIN verify (offline)
// ─────────────────────────────────────────────────────────────────────────────
// Returns true if a matching, valid, non-expired pin found
bool cache_verify_pin_sha256(String input_pin,
                              String &out_pin_id,
                              String &out_user_id) {
  if (!g_offline_enabled) return false;

  String input_hash = sha256_hex(input_pin);
  unsigned long now = get_epoch();
  struct tm t; getLocalTime(&t);
  int cur_day_of_week = t.tm_wday;       // 0=Sun
  int cur_min = t.tm_hour * 60 + t.tm_min;

  JsonArray pins = g_cache["pins"].as<JsonArray>();
  for (JsonObject pin : pins) {
    if (!pin["active"]) continue;

    // Expired?
    unsigned long exp = pin["expires_at"] | 0UL;
    if (exp != 0 && now > exp) continue;

    // Hash match?
    if (String(pin["sha256"] | "") != input_hash) continue;

    // Schedule check (scheduled type)
    if (String(pin["type"] | "") == "scheduled") {
      JsonObject sched = pin["schedule"];
      if (sched.isNull()) continue;

      int from_min = sched["from_min"] | 0;
      int to_min   = sched["to_min"]   | 1440;

      // Days: array of ints [1,2,3,4,5,6]
      bool day_ok = false;
      for (int d : sched["days"].as<JsonArray>()) {
        if (d == cur_day_of_week) { day_ok = true; break; }
      }
      if (!day_ok) {
        Serial.printf("[PIN] Schedule day mismatch (today=%d)\n",
                      cur_day_of_week);
        continue;
      }

      if (cur_min < from_min || cur_min > to_min) {
        Serial.printf("[PIN] Schedule time mismatch (%d not in %d-%d)\n",
                      cur_min, from_min, to_min);
        continue;
      }
    }

    // max_uses check
    int max_uses  = pin["max_uses"]        | 0;
    int local_cnt = pin["local_use_count"] | 0;
    if (max_uses > 0 && local_cnt >= max_uses) {
      Serial.println("[PIN] Max uses reached (offline count)");
      continue;
    }

    // ✓ Valid pin found
    out_pin_id  = String(pin["pin_id"]  | "");
    out_user_id = String(pin["user_id"] | "");

    // Increment local use count for temporary/scheduled
    String type = String(pin["type"] | "");
    if (type == "temporary" || type == "scheduled") {
      pin["local_use_count"] = local_cnt + 1;
      cache_save();
    }

    return true;
  }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  OFFLINE EVENT QUEUE
// ─────────────────────────────────────────────────────────────────────────────
void log_event_offline(String event_type, String method,
                       String detail, bool success) {
  JsonArray q = g_cache["offline_queue"].as<JsonArray>();

  // Trim if at limit
  while ((int)q.size() >= OFFLINE_QUEUE_MAX) q.remove(0);

  JsonObject entry = q.createNestedObject();
  entry["log_id"]     = "LOG-offl-" + String(millis());
  entry["timestamp"]  = get_epoch();
  entry["event_type"] = event_type;
  entry["method"]     = method;
  entry["detail"]     = detail;
  entry["success"]    = success;

  cache_save();
  Serial.printf("[Queue] Buffered offline event: %s\n", event_type.c_str());
}

void queue_flush_online() {
  if (!mqtt.connected()) return;
  JsonArray q = g_cache["offline_queue"].as<JsonArray>();
  if (q.size() == 0) return;

  Serial.printf("[Queue] Flushing %d offline events\n", (int)q.size());

  for (JsonObject entry : q) {
    StaticJsonDocument<256> doc;
    doc["event"]         = entry["event_type"];
    doc["method"]        = entry["method"];
    doc["detail"]        = entry["detail"];
    doc["success"]       = entry["success"];
    doc["offline_event"] = true;
    doc["synced_at"]     = get_iso_timestamp();

    // Convert stored epoch to ISO
    unsigned long ts = entry["timestamp"] | 0UL;
    char iso[25];
    struct tm *t = gmtime((time_t*)&ts);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", t);
    doc["timestamp"] = iso;

    String out; serializeJson(doc, out);
    mqtt.publish(TOPIC_STATE, out.c_str(), false);
    delay(50);  // brief gap between publishes
  }

  // Clear queue and save
  g_cache["offline_queue"] = g_cache.createNestedArray("offline_queue");
  cache_save();
  Serial.println("[Queue] Flush complete");
}
