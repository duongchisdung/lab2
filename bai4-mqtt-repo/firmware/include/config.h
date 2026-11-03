// Sao chép file này thành config.h (config.h nằm trong .gitignore) rồi điền giá trị thật.
#pragma once

#define WIFI_SSID      "VJU Office"
#define WIFI_PASS      "VJuOffice@2023"

#define MQTT_HOST      "172.16.50.112"   // IP PC/RPi/VPS chạy broker
#define MQTT_PORT      1883
#define MQTT_USER      "esp32_node01"
#define MQTT_PASS      "VJuOffice@2023"

#define GROUP_ID       "nhom01"
#define DEVICE_ID      "node01"
#define MQTT_ENABLED   0      // 0: chỉ đo và in Serial, không kết nối/gửi MQTT

// ---- Chân GPIO (ĐỔI cho khớp mô hình nhà kính của nhóm) ----
// Theo sơ đồ mạch ESP32-DEVKITC-32E của nhóm:
#define PIN_DHT        14     // DATA_RH -> IO14, cảm biến DHT11
#define DHT_TYPE       DHT11
#define PIN_RAIN_DO    39     // GIẢ ĐỊNH: ADC_IN (SENSOR_VN, IO39). Đổi nếu cảm biến mưa nối chân khác
#define RAIN_ACTIVE_LOW 1     // 1: module mưa xuất LOW khi có mưa
#define PIN_MOTOR1_A   5      // MOTOR1_INT1 (IO5)  - mái/cửa thông gió
#define PIN_MOTOR1_B   17     // MOTOR1_INT2 (IO17)
#define MOTOR_RUN_MS   3000   // thời gian quay mỗi lần mở/đóng (không có công tắc hành trình)
#define PIN_LED_STATE  26     // LED_R (IO26)

// ---- Chu kỳ ----
#define SENSOR_PERIOD_MS   2000   // chu kỳ đọc cảm biến
#define TELEMETRY_PERIOD_MS 60000 // chu kỳ gửi MQTT: 1 phút
#define SAFETY_PERIOD_MS    200   // chu kỳ an toàn, KHÔNG phụ thuộc MQTT
#define HEARTBEAT_MS       1000

// ---- Ngưỡng an toàn (FSM Bài 3 — chỉnh theo bài của nhóm) ----
#define TEMP_VENT_ON_C     32.0f
#define TEMP_VENT_OFF_C    29.0f   // trễ (hysteresis) để không đóng/mở liên tục

// ---- Thí nghiệm nghẽn queue: đặt >0 để làm task MQTT chậm có chủ đích ----
#define DEMO_MQTT_SLOW_MS  0       // vd. 8000 khi chạy thử tải
