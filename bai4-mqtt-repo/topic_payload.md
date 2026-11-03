# Topic và payload

## Nơi triển khai

| Thành phần | Chạy ở đâu |
|---|---|
| Node (ESP32, mô hình nhà kính) | ESP32 DevKit, FreeRTOS: `sensor_task`, `safety_task` (core 1), `mqtt_task` (core 0) |
| Broker Mosquitto 2 | Docker Compose trên PC/RPi/VPS của nhóm (cổng 1883, có xác thực) |
| Máy quan sát | Máy khác trong cùng mạng: `mosquitto_sub` / MQTT Explorer |

## Topic

```
nhom01/node01/telemetry
```

`<nhóm>/<mã node>/<loại bản tin>` — không dùng tên chung chung như `test`. Dự phòng mở rộng: `nhom01/node01/status`, `nhom01/node01/cmd`.

## Payload

```json
{"device_id":"node01","ts":123456,"temp":27.4,"hum":68.5,"rain":false,"unit":"C","valid":true}
```

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `device_id` | string | Mã node gửi, trùng đoạn thứ 2 của topic |
| `ts` | số nguyên | `millis()` của ESP32 lúc đo (ms kể từ khi bật máy; không phải giờ thực) |
| `temp` | số / `null` | Nhiệt độ; `null` nếu DHT11 đọc lỗi |
| `hum` | số / `null` | Độ ẩm %RH; `null` nếu đọc lỗi |
| `rain` | bool / `null` | `true` = đang mưa; `null` nếu không đọc được |
| `unit` | string | Đơn vị của `temp` |
| `valid` | bool | Cờ chất lượng: `false` nếu có số đo lỗi |

Quy ước: **`null` = thiếu/lỗi, `0` = giá trị đo thật bằng 0**. Không bao giờ thay số đo lỗi bằng 0.

Ví dụ khi cảm biến lỗi:

```json
{"device_id":"node01","ts":130456,"temp":null,"hum":null,"rain":false,"unit":"C","valid":false}
```
