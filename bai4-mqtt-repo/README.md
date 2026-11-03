# Bài 4 — Gửi dữ liệu qua MQTT

Họ tên: ... · MSSV: ... · Nhóm: nhom01

## File trong repo này

- `docker-compose.yml` — cấu hình broker (đã bỏ bí mật)
- `mosquitto/config/mosquitto.conf` — cấu hình broker (bắt buộc đăng nhập)
- `setup_broker.sh`, `.env.example` — sinh tài khoản broker từ biến môi trường vào volume Docker `mosq_auth` (`.env` nằm trong `.gitignore`, không có mật khẩu trong repo)
- `firmware/` — code FreeRTOS cho ESP32 (PlatformIO; sao chép `include/config.example.h` thành `config.h`)
- `topic_payload.md` — mô tả topic và cấu trúc payload
- `task_queue.png` — sơ đồ task/queue
- `dashboard.html` — trang xem số đo trực quan (mở bằng trình duyệt, kết nối WebSocket cổng 9001; chỉ để quan sát)
- `tools/check_log.py` — đối chiếu số bản tin trong log với chu kỳ gửi
- `log_10min.txt` — log subscribe liên tục 10 phút
- `log_disconnect.txt` — log thử ngắt/nối lại broker

## Cách chạy lại

```bash
cp .env.example .env            # điền mật khẩu
./setup_broker.sh               # tạo tài khoản trong volume mosq_auth (Windows: Git Bash/WSL, hoặc xem lệnh dưới)
docker compose up -d
# máy quan sát:
mosquitto_sub -h <IP_BROKER> -u observer -P <mật khẩu> -t "nhom01/node01/telemetry" -v | tee log_10min.txt
python3 tools/check_log.py log_10min.txt --period 2
```

Trên Windows PowerShell (không có bash), thay bước `setup_broker.sh` bằng:

```powershell
docker run --rm -v mosq_auth:/auth eclipse-mosquitto:2 sh -c "mosquitto_passwd -b -c /auth/passwd <user_esp> '<mat_khau_esp>' && mosquitto_passwd -b /auth/passwd observer '<mat_khau_quan_sat>' && chown 1883:1883 /auth/passwd && chmod 0640 /auth/passwd"
```

## 4.1 Payload thực tế

TODO: dán MỘT payload thật lấy từ `log_10min.txt` (không dùng mẫu), ví dụ dạng:

```json
{"device_id":"node01","ts":...,"temp":...,"hum":...,"rain":...,"unit":"C","valid":true}
```

Giải thích: `device_id` định danh node (trùng topic); `ts` là `millis()` của ESP32 (ms từ lúc bật máy); `temp`/`hum` là số đo DHT11, `null` khi đọc lỗi (khác `0` là giá trị thật); `rain` là `true/false/null` từ cảm biến mưa; `unit` là đơn vị nhiệt độ; `valid` là cờ chất lượng, `false` nếu có số đo lỗi. Chi tiết ở `topic_payload.md`.

## 4.2 Task và queue

Firmware có ba task (xem `task_queue.png`):

- `sensor_task` (core 1, ưu tiên 2): mỗi 2000 ms đọc DHT11 và cảm biến mưa, cập nhật nhiệt độ dùng chung (mutex, timeout 5–10 ms) và đẩy một bản ghi vào queue.
- `mqtt_task` (core 0, ưu tiên 1): giữ WiFi/MQTT, tự kết nối lại (thử lại sau 3–5 s), lấy bản ghi từ queue và publish.
- `safety_task` (core 1, ưu tiên 3): chu kỳ 200 ms với `vTaskDelayUntil`, chạy FSM an toàn (NORMAL / VENT_OPEN / RAIN_CLOSED / SENSOR_FAIL), đọc cảm biến mưa trực tiếp và không gọi bất kỳ hàm mạng nào.

Queue: `xQueueCreate(10, sizeof(Sample))`. Chính sách khi đầy: **bỏ bản ghi cũ nhất rồi ghi bản mới** — số đo mới phản ánh trạng thái hiện tại hơn, và `sensor_task` ghi với timeout 0 nên không bao giờ bị chặn. Mỗi lần bỏ đều in `[QUEUE] FULL -> drop oldest` và cộng vào bộ đếm `dropped`.

Task an toàn không được chờ vô hạn vào MQTT vì một lệnh mạng có thể treo bao lâu tùy ý (mất WiFi, broker chết, TCP timeout). Nếu vòng an toàn phụ thuộc vào đó, mái/cửa sẽ không phản ứng kịp khi trời mưa hay quá nhiệt. Vì thế task an toàn có chu kỳ riêng, ưu tiên cao nhất, chỉ dùng mutex với timeout ngắn, và khi mất cảm biến thì tự chuyển về trạng thái an toàn.

Thí nghiệm nghẽn queue: đặt `DEMO_MQTT_SLOW_MS` (ví dụ 8000) trong `config.h`, nạp lại firmware.

TODO: ghi kết quả thật — số bản ghi bị bỏ/giữ, và dòng heartbeat `[SAFETY] hb` vẫn đều ~1 s trong lúc queue đầy.

## 4.3 Thí nghiệm ngắt kết nối

TODO (điền từ `log_disconnect.txt`):

- Thời điểm `docker compose stop broker`: ...; khởi động lại: ... (cách nhau 30 s).
- Bằng chứng vòng an toàn vẫn chạy: các dòng `[SAFETY] hb ... mqtt=DOWN` vẫn đều đặn trong lúc mất kết nối.
- Kết quả phục hồi: dòng `[MQTT] connected` xuất hiện lúc ..., sau ... giây kể từ khi broker sống lại; các bản tin tiếp tục đến máy quan sát.

## 4.4 Đóng góp cá nhân

TODO: nêu việc em tự làm, dẫn commit/log cụ thể; ghi rõ phần được bạn hỗ trợ (và ai hỗ trợ). Nếu dùng công cụ AI để tạo khung code/tài liệu, cũng ghi rõ phần đó và phần em tự kiểm tra, chỉnh sửa.
