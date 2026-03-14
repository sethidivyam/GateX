// ─────────────────────────────────────────────
//  .env — Smart Lock Configuration
//  Copy this to config.h and fill your values
// ─────────────────────────────────────────────

// ── Device Identity ──────────────────────────
#define LOCK_ID           "LOCK-x7y8z9"
#define FIRMWARE_VERSION  "2.1.3"
#define HARDWARE_REV      "v2"

// ── WiFi ─────────────────────────────────────
#define WIFI_SSID         "YourWiFiSSID"
#define WIFI_PASSWORD     "YourWiFiPassword"
#define WIFI_RETRY_LIMIT  10
#define WIFI_RETRY_DELAY_MS 1000

// ── AWS IoT Core ─────────────────────────────
#define AWS_IOT_ENDPOINT  "xxxxxx-ats.iot.ap-south-1.amazonaws.com"
#define AWS_IOT_PORT      8883
#define THING_NAME        "lock-x7y8z9"

// Certificates go in certs.h
// AWS Root CA, Device Cert, Device Private Key

// ── MQTT Topics ───────────────────────────────
#define TOPIC_CMD         "home/" LOCK_ID "/cmd"
#define TOPIC_STATE       "home/" LOCK_ID "/state"
#define TOPIC_DOORBELL    "home/" LOCK_ID "/doorbell"
#define TOPIC_SHADOW_GET  "$aws/things/" THING_NAME "/shadow/get"
#define TOPIC_SHADOW_UPD  "$aws/things/" THING_NAME "/shadow/update"
#define TOPIC_SHADOW_DELTA "$aws/things/" THING_NAME "/shadow/update/delta"

// ── Hardware Pins ─────────────────────────────
//   Fingerprint R503 (UART2)
#define FP_RX_PIN         16
#define FP_TX_PIN         17
#define FP_WAKEUP_PIN     4    // R503 WAKEUP/TOUCH pin
#define FP_BAUD           57600

//   RFID RC522 (SPI)
#define RFID_SS_PIN       5
#define RFID_RST_PIN      27
#define RFID_SCK_PIN      18
#define RFID_MISO_PIN     19
#define RFID_MOSI_PIN     23

//   Relay
#define RELAY_PIN         26
#define RELAY_ACTIVE_HIGH true   // false if relay triggers on LOW

//   Door Reed Switch (magnetic sensor)
#define DOOR_SENSOR_PIN   25
#define DOOR_CLOSED_STATE LOW    // LOW when door is closed (NO reed switch)

//   Doorbell button
#define DOORBELL_PIN      33
#define DOORBELL_DEBOUNCE_MS 50

//   Tamper vibration sensor
#define TAMPER_PIN        32
#define TAMPER_DEBOUNCE_MS 200

//   Status LED (optional)
#define LED_PIN           2
#define LED_ACTIVE_HIGH   true

// ── Behaviour ────────────────────────────────
#define RELAY_DURATION_MS         2000   // door open time
#define DOOR_OPEN_ALERT_SEC       30     // alert if open > 30s
#define FAILED_ATTEMPT_LOCKOUT    5      // attempts before lockout
#define LOCKOUT_DURATION_SEC      300    // 5 min lockout
#define DOORBELL_COOLDOWN_MS      10000  // 10s spam guard
#define HEARTBEAT_INTERVAL_MS     30000  // state publish every 30s
#define SHADOW_SYNC_INTERVAL_MS   300000 // re-pull shadow every 5 min
#define OFFLINE_QUEUE_MAX         50     // max buffered offline events

// ── SPIFFS Cache ──────────────────────────────
#define CACHE_FILE        "/cache.json"
#define CERT_CA_FILE      "/ca.crt"
#define CERT_DEVICE_FILE  "/device.crt"
#define CERT_KEY_FILE     "/device.key"

// ── Fingerprint ID range ─────────────────────
// R503 supports 1–200 template slots physically
// We map slot → complex ID stored in cache
#define FP_SLOT_MIN       1
#define FP_SLOT_MAX       200

// ── NTP ───────────────────────────────────────
#define NTP_SERVER1       "pool.ntp.org"
#define NTP_SERVER2       "time.google.com"
#define NTP_TIMEZONE      "IST-5:30"     // POSIX tz string for Asia/Kolkata
