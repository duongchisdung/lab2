// CSE3069 - Bài 4: ESP32 + FreeRTOS + MQTT
// 3 task:
//   sensor_task (core 1) : đọc DHT11 + cảm biến mưa, đẩy bản ghi vào queue
//   mqtt_task   (core 0) : giữ WiFi/MQTT, gửi mẫu mới nhất theo chu kỳ cấu hình
//   safety_task (core 1) : FSM an toàn, chu kỳ riêng, KHÔNG chạm mạng/MQTT
//
// Queue giới hạn 10 bản ghi. Chính sách khi đầy: BỎ BẢN GHI CŨ NHẤT rồi ghi bản mới
// (số đo mới có giá trị hơn số đo cũ; sensor_task không bao giờ bị chặn).

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include "config.h"

#define QUEUE_LEN 10

struct Sample {
  uint32_t ts;      // millis() lúc đo
  float    temp;    // NAN nếu lỗi
  float    hum;     // NAN nếu lỗi
  int8_t   rain;    // 1 / 0 ; -1 = không đọc được
  bool     valid;   // true nếu cả DHT đọc được
};

// Trạng thái dùng chung cho task an toàn (ghi bởi sensor_task, đọc bởi safety_task)
struct Shared {
  volatile float temp;
  volatile bool  tempOk;
  volatile uint32_t lastUpdate;
};

static QueueHandle_t   qSamples;
static SemaphoreHandle_t mtx;
static Shared shared = {NAN, false, 0};

static DHT dht(PIN_DHT, DHT_TYPE);

static inline bool rainNow() {
  int v = digitalRead(PIN_RAIN_DO);
  return RAIN_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}
#if MQTT_ENABLED
static WiFiClient   wifi;
static PubSubClient mqtt(wifi);
#endif

// Thống kê cho log thí nghiệm
static volatile uint32_t statProduced = 0, statDropped = 0, statPublished = 0;
#if MQTT_ENABLED
static volatile uint32_t mqttLastLoopMs = 0;

static const char* TOPIC = GROUP_ID "/" DEVICE_ID "/telemetry";
#endif

// ---------------------------------------------------------------- sensor_task
static void sensor_task(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    Sample s;
    s.ts   = millis();
    s.temp = dht.readTemperature();
    s.hum  = dht.readHumidity();
    s.valid = !(isnan(s.temp) || isnan(s.hum));
    s.rain = rainNow() ? 1 : 0;

    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(10))) {
      shared.temp = s.temp;
      shared.tempOk = !isnan(s.temp);
      shared.lastUpdate = s.ts;
      xSemaphoreGive(mtx);
    }

    if (!s.valid) {
      Serial.println("[SENSOR] DHT read failed");
    } else {
      statProduced++;
#if MQTT_ENABLED
      // Không chờ: nếu đầy thì bỏ bản cũ nhất rồi ghi bản mới.
      if (xQueueSend(qSamples, &s, 0) != pdTRUE) {
        Sample old;
        xQueueReceive(qSamples, &old, 0);   // vứt bản cũ nhất
        statDropped++;
        xQueueSend(qSamples, &s, 0);
        Serial.printf("[QUEUE] FULL -> drop oldest ts=%lu | dropped=%lu\n",
                      (unsigned long)old.ts, (unsigned long)statDropped);
      }
#else
      Serial.printf("[SENSOR] t=%.1f C hum=%.1f%% rain=%s\n",
                    s.temp, s.hum, s.rain ? "YES" : "NO");
#endif
    }
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
  }
}

#if MQTT_ENABLED
// ----------------------------------------------------------------- mqtt_task
static void fmtFloat(char* out, size_t n, float v, bool ok) {
  if (ok && !isnan(v)) snprintf(out, n, "%.1f", v);
  else                 snprintf(out, n, "null");   // null = lỗi/thiếu, khác 0 thật
}

static bool publishSample(const Sample& s) {
  char t[12], h[12], r[8], buf[200];
  fmtFloat(t, sizeof t, s.temp, s.valid);
  fmtFloat(h, sizeof h, s.hum,  s.valid);
  if (s.rain < 0) snprintf(r, sizeof r, "null");
  else            snprintf(r, sizeof r, s.rain ? "true" : "false");
  snprintf(buf, sizeof buf,
    "{\"device_id\":\"%s\",\"ts\":%lu,\"temp\":%s,\"hum\":%s,\"rain\":%s,"
    "\"unit\":\"C\",\"valid\":%s}",
    DEVICE_ID, (unsigned long)s.ts, t, h, r, s.valid ? "true" : "false");
  return mqtt.publish(TOPIC, buf);
}

static void mqtt_task(void*) {
  uint32_t nextTry = 0;
  uint32_t lastPublishMs = millis();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(2);       // connect/đọc không treo quá ~2 s
  mqtt.setKeepAlive(10);
  wifi.setTimeout(2);
  WiFi.mode(WIFI_STA);

  for (;;) {
    mqttLastLoopMs = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (millis() >= nextTry) {
        Serial.println("[MQTT] WiFi lost -> reconnecting");
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        nextTry = millis() + 5000;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (!mqtt.connected()) {
      if (millis() >= nextTry) {
        Serial.printf("[MQTT] connecting to %s:%d ...\n", MQTT_HOST, MQTT_PORT);
        String cid = String(DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)) {
          Serial.println("[MQTT] connected");
        } else {
          Serial.printf("[MQTT] connect failed rc=%d, retry in 3s\n", mqtt.state());
          nextTry = millis() + 3000;   // backoff đơn giản
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    mqtt.loop();

    if ((uint32_t)(millis() - lastPublishMs) >= TELEMETRY_PERIOD_MS) {
      Sample s;
      bool haveSample = false;
      while (xQueueReceive(qSamples, &s, 0) == pdTRUE) {
        haveSample = true; // drain queue to publish the newest reading
      }
      if (haveSample) {
        if (DEMO_MQTT_SLOW_MS > 0) vTaskDelay(pdMS_TO_TICKS(DEMO_MQTT_SLOW_MS)); // thử tải
        lastPublishMs = millis();
        if (publishSample(s)) {
          statPublished++;
        } else {
          // publish lỗi (mất kết nối giữa chừng): trả bản ghi lại đầu hàng đợi
          xQueueSendToFront(qSamples, &s, 0);
          vTaskDelay(pdMS_TO_TICKS(200));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
#endif

// --------------------------------------------------------------- safety_task
enum SafetyState { S_NORMAL, S_VENT_OPEN, S_RAIN_CLOSED, S_SENSOR_FAIL };
static const char* STATE_NAME[] = {"NORMAL", "VENT_OPEN", "RAIN_CLOSED", "SENSOR_FAIL"};

static bool ventOpen = false;          // giả định mái đang đóng khi khởi động
static uint32_t motorStopAt = 0;

static void motorStop() { digitalWrite(PIN_MOTOR1_A, LOW); digitalWrite(PIN_MOTOR1_B, LOW); }

// Quay motor một khoảng MOTOR_RUN_MS theo hướng mở/đóng rồi tự dừng (không chặn task)
static void setVent(bool open) {
  if (open == ventOpen) return;
  ventOpen = open;
  digitalWrite(PIN_MOTOR1_A, open ? HIGH : LOW);
  digitalWrite(PIN_MOTOR1_B, open ? LOW : HIGH);
  motorStopAt = millis() + MOTOR_RUN_MS;
}

static void safety_task(void*) {
  SafetyState st = S_NORMAL;
  uint32_t lastHb = 0;
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    float temp = NAN; bool ok = false; uint32_t upd = 0;
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(5))) {
      temp = shared.temp; ok = shared.tempOk; upd = shared.lastUpdate;
      xSemaphoreGive(mtx);
    }
    bool rain = rainNow();                               // đọc thẳng, không qua mạng
    if (motorStopAt && (int32_t)(millis() - motorStopAt) >= 0) { motorStop(); motorStopAt = 0; }
    bool stale = (millis() - upd) > (3 * SENSOR_PERIOD_MS);

    SafetyState next = st;
    if (rain)                         next = S_RAIN_CLOSED;     // mưa: luôn đóng
    else if (!ok || stale)            next = S_SENSOR_FAIL;     // mất cảm biến: về trạng thái an toàn (đóng)
    else if (st == S_VENT_OPEN)       next = (temp <= TEMP_VENT_OFF_C) ? S_NORMAL : S_VENT_OPEN;
    else                              next = (temp >= TEMP_VENT_ON_C)  ? S_VENT_OPEN : S_NORMAL;

    if (next != st) {
      Serial.printf("[SAFETY] %s -> %s (temp=%.1f rain=%d)\n",
                    STATE_NAME[st], STATE_NAME[next], temp, rain);
      st = next;
    }
    setVent(st == S_VENT_OPEN);
    digitalWrite(PIN_LED_STATE, st == S_NORMAL ? LOW : HIGH);

    if (millis() - lastHb >= HEARTBEAT_MS) {
      lastHb = millis();
      // Heartbeat confirms the safety task is still running.
#if MQTT_ENABLED
      Serial.printf("[SAFETY] hb t=%lu state=%s mqtt=%s q=%u pub=%lu drop=%lu mqttLoopAgo=%lums\n",
                    (unsigned long)millis(), STATE_NAME[st],
                    mqtt.connected() ? "UP" : "DOWN",
                    (unsigned)uxQueueMessagesWaiting(qSamples),
                    (unsigned long)statPublished, (unsigned long)statDropped,
                    (unsigned long)(millis() - mqttLastLoopMs));
#else
      Serial.printf("[SAFETY] hb t=%lu state=%s mode=TEST q=%u\n",
                    (unsigned long)millis(), STATE_NAME[st],
                    (unsigned)uxQueueMessagesWaiting(qSamples));
#endif
    }
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SAFETY_PERIOD_MS));
  }
}

// --------------------------------------------------------------------- setup
void setup() {
  Serial.begin(115200);
  pinMode(PIN_RAIN_DO, INPUT);
  pinMode(PIN_MOTOR1_A, OUTPUT);
  pinMode(PIN_MOTOR1_B, OUTPUT);
  pinMode(PIN_LED_STATE, OUTPUT);
  motorStop();                          // không quay motor lúc khởi động
  dht.begin();

  qSamples = xQueueCreate(QUEUE_LEN, sizeof(Sample));
  mtx = xSemaphoreCreateMutex();

  //                 hàm          tên      stack  tham số  ưu tiên  handle  core
  xTaskCreatePinnedToCore(safety_task, "safety", 4096, nullptr, 3, nullptr, 1); // ưu tiên cao nhất
  xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, nullptr, 2, nullptr, 1);
#if MQTT_ENABLED
  xTaskCreatePinnedToCore(mqtt_task,   "mqtt",   8192, nullptr, 1, nullptr, 0);
  Serial.printf("[BOOT] topic=%s queue=%d slow=%dms\n", TOPIC, QUEUE_LEN, DEMO_MQTT_SLOW_MS);
#else
  Serial.println("[BOOT] TEST MODE: MQTT disabled; sensor readings print to Serial only");
#endif
}

void loop() { vTaskDelete(nullptr); }   // mọi việc nằm trong các task
