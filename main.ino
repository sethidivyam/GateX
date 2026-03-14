// ─────────────────────────────────────────────────────────────────────────────
//  main.ino — Smart Lock Entry Point
//  ESP32 | Arduino Framework
//  Handles: WiFi, AWS IoT MQTT, NTP, Shadow sync, main loop
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"   // rename .env → config.h and fill values
#include "certs.h"    // AWS_ROOT_CA, DEVICE_CERT, DEVICE_PRIVATE_KEY

// ── Forward declarations (defined in other .ino files) ───────────────────────
void      control_setup();
void      control_loop();
void      fp_setup();
bool      fp_loop();          // returns true if finger event handled
void      rfid_setup();
bool      rfid_loop();        // returns true if card event handled
void      cache_load();
void      cache_save();
void      queue_flush_online();
String    cache_get_user_for_fp_slot(uint16_t slot);
String    cache_get_user_for_rfid(String uid);
bool      cache_verify_pin_sha256(String sha256, String &out_pin_id,
                                  String &out_user_id);
void      publish_state(String event_type, String method,
                        String user_id, String detail,
                        bool success, String failure_reason);
void      publish_doorbell(String image_url);
void      log_event_offline(String event_type, String method,
                             String detail, bool success);

// ── Globals ──────────────────────────────────────────────────────────────────
WiFiClientSecure  net;
PubSubClient      mqtt(net);

bool    g_online          = false;
bool    g_lockout_active  = false;
unsigned long g_lockout_until_ms  = 0;
int     g_failed_attempts = 0;
unsigned long g_last_heartbeat    = 0;
unsigned long g_last_shadow_sync  = 0;
unsigned long g_last_doorbell_ms  = 0;

// Populated by cache_load()
extern int      g_relay_ms;
extern bool     g_offline_enabled;
extern JsonDocument g_cache;   // defined in control.ino

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] Smart Lock " LOCK_ID " | fw " FIRMWARE_VERSION);

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("[BOOT] SPIFFS mount failed — formatting");
    SPIFFS.format();
    SPIFFS.begin(true);
  }

  cache_load();
  control_setup();
  fp_setup();
  rfid_setup();

  wifi_connect();
  ntp_sync();
  mqtt_connect();
  shadow_request_sync();

  Serial.println("[BOOT] Ready.");
}

// ── Main loop ────────────────────────────────────────────────────────────────
void loop() {
  // Reconnect if needed
  if (WiFi.status() != WL_CONNECTED) wifi_connect();
  if (!mqtt.connected())             mqtt_connect();
  mqtt.loop();

  // Peripheral handlers
  fp_loop();
  rfid_loop();
  control_loop();

  // Periodic heartbeat
  if (millis() - g_last_heartbeat > HEARTBEAT_INTERVAL_MS) {
    g_last_heartbeat = millis();
    publish_heartbeat();
  }

  // Periodic shadow re-sync
  if (millis() - g_last_shadow_sync > SHADOW_SYNC_INTERVAL_MS) {
    g_last_shadow_sync = millis();
    shadow_request_sync();
  }

  // Flush offline queue if online
  if (g_online) queue_flush_online();
}

// ── WiFi ─────────────────────────────────────────────────────────────────────
void wifi_connect() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < WIFI_RETRY_LIMIT) {
    delay(WIFI_RETRY_DELAY_MS);
    Serial.print(".");
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected — IP: %s\n",
                  WiFi.localIP().toString().c_str());
    g_online = true;
  } else {
    Serial.println("\n[WiFi] Failed — running offline");
    g_online = false;
  }
}

// ── NTP ──────────────────────────────────────────────────────────────────────
void ntp_sync() {
  if (!g_online) return;
  configTzTime(NTP_TIMEZONE, NTP_SERVER1, NTP_SERVER2);
  Serial.print("[NTP] Syncing time");
  struct tm t;
  int retries = 0;
  while (!getLocalTime(&t) && retries < 10) {
    delay(500); Serial.print("."); retries++;
  }
  Serial.printf("\n[NTP] Time: %04d-%02d-%02d %02d:%02d:%02d\n",
    t.tm_year+1900, t.tm_mon+1, t.tm_mday,
    t.tm_hour, t.tm_min, t.tm_sec);
}

String get_iso_timestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "1970-01-01T00:00:00Z";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

unsigned long get_epoch() {
  return (unsigned long)time(nullptr);
}

// ── MQTT ─────────────────────────────────────────────────────────────────────
void mqtt_connect() {
  if (!g_online) return;

  net.setCACert(AWS_ROOT_CA);
  net.setCertificate(DEVICE_CERT);
  net.setPrivateKey(DEVICE_PRIVATE_KEY);

  mqtt.setServer(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
  mqtt.setBufferSize(2048);
  mqtt.setCallback(mqtt_on_message);

  // Last Will Testament — device offline event
  String lwt = build_lwt_payload();
  Serial.print("[MQTT] Connecting");
  int retries = 0;
  while (!mqtt.connected() && retries < 5) {
    bool ok = mqtt.connect(
      THING_NAME,
      nullptr, nullptr,
      TOPIC_STATE, 1, true, lwt.c_str()
    );
    if (ok) break;
    Serial.printf("  err=%d, retry %d\n", mqtt.state(), retries+1);
    delay(2000);
    retries++;
  }

  if (mqtt.connected()) {
    Serial.println("\n[MQTT] Connected");
    mqtt.subscribe(TOPIC_CMD,          1);
    mqtt.subscribe(TOPIC_SHADOW_DELTA, 1);
    mqtt.subscribe("$aws/things/" THING_NAME "/shadow/get/accepted", 1);
    publish_device_online();
  } else {
    Serial.println("\n[MQTT] Failed — offline mode");
    g_online = false;
  }
}

void mqtt_on_message(char *topic, byte *payload, unsigned int len) {
  String t(topic);
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  Serial.printf("[MQTT] ← %s\n", t.c_str());

  if (t == TOPIC_CMD)                                      handle_cmd(msg);
  else if (t.endsWith("/shadow/update/delta"))             handle_shadow_delta(msg);
  else if (t.endsWith("/shadow/get/accepted"))             handle_shadow_accepted(msg);
}

// ── MQTT incoming handlers ────────────────────────────────────────────────────
void handle_cmd(String &msg) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, msg)) { Serial.println("[CMD] parse error"); return; }

  String action     = doc["action"]       | "";
  String req_id     = doc["request_id"]   | "";
  String req_by     = doc["requested_by"] | "";
  int    relay_ms_o = doc["relay_ms"]     | g_relay_ms;

  if (action == "open") {
    if (g_lockout_active && millis() < g_lockout_until_ms) {
      Serial.println("[CMD] Lockout active — denied");
      publish_state("failed_attempt","app",req_by,req_id,false,"lockout_active");
      return;
    }
    Serial.printf("[CMD] Open requested by %s\n", req_by.c_str());
    trigger_relay(relay_ms_o);
    g_failed_attempts = 0;
    publish_state("door_opened","app",req_by,req_id,true,"");
  }
}

void handle_shadow_delta(String &msg) {
  Serial.println("[Shadow] Delta received — syncing cache");
  // Delta means cloud has newer cache_version
  shadow_request_sync();
}

void handle_shadow_accepted(String &msg) {
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, msg)) return;

  JsonObject state = doc["state"]["desired"];
  if (state.isNull()) state = doc["state"]["reported"];
  if (state.isNull()) return;

  // Update cache from shadow
  int cloud_version = state["cache_version"] | 0;
  int local_version = g_cache["cache_version"] | 0;
  if (cloud_version > local_version) {
    Serial.printf("[Shadow] Version %d → %d, updating cache\n",
                  local_version, cloud_version);
    // Merge relevant fields from shadow into g_cache
    if (state.containsKey("relay_ms"))
      g_cache["relay_ms"] = state["relay_ms"];
    if (state.containsKey("offline_enabled"))
      g_cache["offline_enabled"] = state["offline_enabled"];
    if (state.containsKey("fingerprint"))
      g_cache["fingerprint"] = state["fingerprint"];
    if (state.containsKey("rfid"))
      g_cache["rfid"] = state["rfid"];
    if (state.containsKey("pins"))
      g_cache["pins"] = state["pins"];
    if (state.containsKey("lockout"))
      g_cache["lockout"] = state["lockout"];

    g_cache["cache_version"] = cloud_version;
    g_cache["synced_at"]     = get_iso_timestamp();
    cache_save();
    Serial.println("[Shadow] Cache updated & saved to SPIFFS");
  } else {
    Serial.println("[Shadow] Cache already up to date");
  }
  g_last_shadow_sync = millis();
}

// ── Shadow ───────────────────────────────────────────────────────────────────
void shadow_request_sync() {
  if (!mqtt.connected()) return;
  mqtt.publish(TOPIC_SHADOW_GET, "{}");
  Serial.println("[Shadow] Requested sync");
}

// ── Publish helpers ───────────────────────────────────────────────────────────
void publish_state(String event_type, String method,
                   String user_id,    String detail,
                   bool success,      String failure_reason) {
  StaticJsonDocument<512> doc;
  doc["event"]           = event_type;
  doc["method"]          = method;
  doc["user_id"]         = user_id.length() ? user_id : (const char*)nullptr;
  doc["detail"]          = detail;
  doc["success"]         = success;
  doc["failure_reason"]  = failure_reason.length() ? failure_reason
                                                     : (const char*)nullptr;
  doc["relay_ms"]        = g_relay_ms;
  doc["battery_percent"] = read_battery_percent();
  doc["wifi_signal_dbm"] = WiFi.RSSI();
  doc["timestamp"]       = get_iso_timestamp();

  String out;
  serializeJson(doc, out);

  if (mqtt.connected()) {
    mqtt.publish(TOPIC_STATE, out.c_str(), false);
    Serial.printf("[PUB] state: %s\n", event_type.c_str());
  } else {
    log_event_offline(event_type, method, detail, success);
  }
}

void publish_heartbeat() {
  StaticJsonDocument<256> doc;
  doc["event"]           = "heartbeat";
  doc["online"]          = true;
  doc["battery_percent"] = read_battery_percent();
  doc["wifi_signal_dbm"] = WiFi.RSSI();
  doc["timestamp"]       = get_iso_timestamp();
  String out; serializeJson(doc, out);
  if (mqtt.connected()) mqtt.publish(TOPIC_STATE, out.c_str(), false);
}

void publish_device_online() {
  publish_state("device_online","auto","","",true,"");
}

void publish_doorbell(String image_url) {
  StaticJsonDocument<256> doc;
  doc["event"]     = "doorbell";
  doc["image_url"] = image_url;
  doc["timestamp"] = get_iso_timestamp();
  String out; serializeJson(doc, out);
  if (mqtt.connected()) mqtt.publish(TOPIC_DOORBELL, out.c_str(), false);
}

String build_lwt_payload() {
  StaticJsonDocument<128> doc;
  doc["event"]     = "device_offline";
  doc["lock_id"]   = LOCK_ID;
  doc["timestamp"] = "unknown";  // LWT sent by broker, no real timestamp
  String out; serializeJson(doc, out);
  return out;
}

// ── Lockout ───────────────────────────────────────────────────────────────────
void record_failed_attempt(String method, String detail) {
  g_failed_attempts++;
  Serial.printf("[Auth] Failed attempt %d/%d — %s %s\n",
    g_failed_attempts, FAILED_ATTEMPT_LOCKOUT,
    method.c_str(), detail.c_str());

  publish_state("failed_attempt", method, "", detail, false,
                "invalid_credential");

  if (g_failed_attempts >= FAILED_ATTEMPT_LOCKOUT) {
    g_lockout_active   = true;
    g_lockout_until_ms = millis() + (LOCKOUT_DURATION_SEC * 1000UL);
    Serial.printf("[Auth] LOCKOUT — %d sec\n", LOCKOUT_DURATION_SEC);
    publish_state("lockout_started","auto","","",true,"");
  }
}

void check_lockout_expiry() {
  if (g_lockout_active && millis() >= g_lockout_until_ms) {
    g_lockout_active  = false;
    g_failed_attempts = 0;
    Serial.println("[Auth] Lockout expired");
    publish_state("lockout_ended","auto","","",true,"");
  }
}
