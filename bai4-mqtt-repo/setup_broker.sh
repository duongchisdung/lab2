#!/usr/bin/env bash
# Sinh file passwd từ biến môi trường trong .env, lưu vào volume Docker "mosq_auth".
# (Không dùng thư mục bind-mount để tránh lỗi quyền đọc file trên Windows/WSL.)
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -f .env ]; then
  echo "Thiếu .env — chạy: cp .env.example .env rồi điền mật khẩu" >&2
  exit 1
fi
set -a; . ./.env; set +a

docker run --rm -v mosq_auth:/auth -e U1="$MQTT_USER" -e P1="$MQTT_PASS" \
  -e U2="$OBSERVER_USER" -e P2="$OBSERVER_PASS" eclipse-mosquitto:2 sh -c '
    mosquitto_passwd -b -c /auth/passwd "$U1" "$P1" &&
    mosquitto_passwd -b /auth/passwd "$U2" "$P2" &&
    chown 1883:1883 /auth/passwd && chmod 0640 /auth/passwd'
echo "Đã tạo tài khoản trong volume mosq_auth. Chạy tiếp: docker compose up -d"
