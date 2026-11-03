// Thử DHT11 KHÔNG dùng thư viện: tự đọc theo giao thức một dây và báo dừng ở bước nào.
// Nạp bằng env "dht_test". Mở Serial Monitor 115200.
#include <Arduino.h>

#define PIN_DHT 14   // DATA_RH -> IO14 (theo sơ đồ)

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Chờ trong khi chân còn ở mức `level`; trả về thời gian (us) hoặc -1 nếu quá hạn.
static int waitWhile(int level, uint32_t timeoutUs) {
  uint32_t t = micros();
  while (digitalRead(PIN_DHT) == level) {
    if (micros() - t > timeoutUs) return -1;
  }
  return (int)(micros() - t);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n[TEST] DHT11 tu doc, chan IO14");
}

void loop() {
  uint8_t b[5] = {0};
  int stage = 0, tResp = 0, tLow = 0, tHigh = 0;
  int lows[40], highs[40];

  pinMode(PIN_DHT, INPUT_PULLUP);
  delay(50);
  int idle = digitalRead(PIN_DHT);

  // Tín hiệu bắt đầu: kéo thấp >= 18 ms rồi thả
  pinMode(PIN_DHT, OUTPUT);
  digitalWrite(PIN_DHT, LOW);
  delay(20);

  portENTER_CRITICAL(&mux);
  pinMode(PIN_DHT, INPUT_PULLUP);
  tResp = waitWhile(HIGH, 120);                       // chờ cảm biến kéo thấp
  if (tResp < 0) stage = 1;
  else {
    tLow = waitWhile(LOW, 150);                       // ~80 us thấp
    if (tLow < 0) stage = 2;
    else {
      tHigh = waitWhile(HIGH, 150);                   // ~80 us cao
      if (tHigh < 0) stage = 3;
      else {
        for (int i = 0; i < 40 && stage == 0; i++) {
          lows[i]  = waitWhile(LOW, 120);             // ~50 us thấp
          highs[i] = (lows[i] < 0) ? -1 : waitWhile(HIGH, 150);   // 26-28 us = bit 0, ~70 us = bit 1
          if (lows[i] < 0 || highs[i] < 0) { stage = 4 + i; break; }
          b[i / 8] <<= 1;
          if (highs[i] > lows[i]) b[i / 8] |= 1;
        }
      }
    }
  }
  portEXIT_CRITICAL(&mux);

  Serial.printf("[TEST] muc roi=%d  ", idle);
  if (stage == 1)      Serial.println("LOI buoc 1: cam bien KHONG tra loi (khong co nguon / sai chan / hong / chua cam)");
  else if (stage == 2) Serial.println("LOI buoc 2: cam bien tra loi nhung bi ket o muc thap");
  else if (stage == 3) Serial.println("LOI buoc 3: cam bien bi ket o muc cao");
  else if (stage >= 4) Serial.printf("LOI o bit %d (tin hieu dut giua chung)\n", stage - 4);
  else {
    uint8_t sum = b[0] + b[1] + b[2] + b[3];
    Serial.printf("bytes=%02X %02X %02X %02X %02X  checksum=%s\n", b[0], b[1], b[2], b[3], b[4], sum == b[4] ? "DUNG" : "SAI");
    if (sum == b[4]) {
      Serial.printf("[TEST] DHT11: T=%d.%d C  H=%d.%d %%\n", b[2], b[3], b[0], b[1]);
      Serial.printf("[TEST] (neu la DHT22 thi: T=%.1f C  H=%.1f %%)\n",
                    (((b[2] & 0x7F) << 8) | b[3]) / 10.0f, ((b[0] << 8) | b[1]) / 10.0f);
    }
  }
  delay(2500);
}