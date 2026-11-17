#!/usr/bin/env python3
"""Dashboard/API cho bài 6: gửi lệnh có cmd_id và kiểm tra ack."""

from __future__ import annotations

import json
import os
import sqlite3
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

import paho.mqtt.client as mqtt
from flask import Flask, jsonify, request, send_file

sys.stdout.reconfigure(encoding='utf-8')

APP_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = APP_ROOT / "greenhouse.db"
DASHBOARD_DIR = APP_ROOT / "dashboard"
COMMAND_QUEUE_PATH = APP_ROOT / "api" / "command_queue.jsonl"
ACK_QUEUE_PATH = APP_ROOT / "api" / "ack_queue.jsonl"

MQTT_BROKER = os.getenv("MQTT_BROKER_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_BROKER_PORT", "1883"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME", "iot_user")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "iot_password123")
TOPIC_COMMAND = "nhom01/node01/cmd"
TOPIC_ACK = "nhom01/node01/ack"
TIMEOUT_SECONDS = 5

ALLOWED_TOKENS = {
    "admin-demo": "admin",
    "operator-demo": "operator",
}

app = Flask(__name__)
app.config["JSON_SORT_KEYS"] = False

_stop_event = threading.Event()
_mqtt_client = None


def get_db_connection() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def ensure_db() -> None:
    with get_db_connection() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS command_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                cmd_id TEXT NOT NULL UNIQUE,
                device_id TEXT NOT NULL,
                action TEXT NOT NULL,
                request_user TEXT,
                status TEXT NOT NULL DEFAULT 'pending',
                result TEXT,
                reason TEXT,
                sent_at INTEGER NOT NULL,
                acked_at INTEGER,
                node_id TEXT,
                payload TEXT
            )
            """
        )
        conn.commit()


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%dT%H:%M:%S%z")


def record_command(cmd_id: str, device_id: str, action: str, request_user: str, status: str, result: str | None, reason: str | None, payload: dict | None):
    with get_db_connection() as conn:
        conn.execute(
            """
            INSERT OR REPLACE INTO command_history
            (cmd_id, device_id, action, request_user, status, result, reason, sent_at, acked_at, node_id, payload)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                cmd_id,
                device_id,
                action,
                request_user,
                status,
                result,
                reason,
                int(time.time()),
                None,
                None,
                json.dumps(payload or {}, ensure_ascii=False),
            ),
        )
        conn.commit()


def update_command_status(cmd_id: str, status: str, result: str | None = None, reason: str | None = None, node_id: str | None = None, acked_at: int | None = None):
    with get_db_connection() as conn:
        row = conn.execute("SELECT status FROM command_history WHERE cmd_id = ?", (cmd_id,)).fetchone()
        if row is None:
            return
        current = row["status"]
        if current in {"timeout", "denied"} and status not in {"timeout", "denied"}:
            return
        if current == "timeout" and status == "confirmed":
            reason = "Ack đến muộn sau timeout; không tự gửi lệnh mới"
            status = "timeout"
        if current == "denied" and status == "confirmed":
            return
        if acked_at is None:
            acked_at = int(time.time())
        conn.execute(
            """
            UPDATE command_history
            SET status = ?, result = ?, reason = ?, acked_at = ?, node_id = ?
            WHERE cmd_id = ?
            """,
            (status, result, reason, acked_at, node_id, cmd_id),
        )
        conn.commit()


def get_history(limit: int = 10):
    with get_db_connection() as conn:
        rows = conn.execute(
            """
            SELECT cmd_id, device_id, action, request_user, status, result, reason, sent_at, acked_at, node_id, payload
            FROM command_history
            ORDER BY sent_at DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
    return [dict(row) for row in rows]


def get_latest_telemetry():
    with get_db_connection() as conn:
        row = conn.execute(
            """
            SELECT device_id, ts, temp, hum, rain, valid, suspect, fsm_state, pump, received_at
            FROM telemetry
            ORDER BY ts DESC
            LIMIT 1
            """
        ).fetchone()
    if row is None:
        return {
            "device_id": None,
            "ts": None,
            "temp": None,
            "hum": None,
            "rain": None,
            "valid": False,
            "suspect": False,
            "fsm_state": "UNKNOWN",
            "pump": 0,
            "received_at": None,
        }
    data = dict(row)
    data["stale"] = data["received_at"] is not None and (int(time.time()) - int(data["received_at"])) > 30
    return data


def handle_ack_message(payload: dict):
    cmd_id = payload.get("cmd_id")
    if not cmd_id:
        return
    if payload.get("result") == "success":
        status = "confirmed"
        result = "success"
        reason = "Ack khớp cmd_id, node đúng, thực thi thành công"
        node_id = payload.get("node_id") or "node01"
    elif payload.get("result") == "rejected":
        status = "denied"
        result = "rejected"
        reason = payload.get("reason") or "Node từ chối lệnh vì an toàn"
        node_id = payload.get("node_id") or "node01"
    else:
        status = "denied"
        result = payload.get("result") or "error"
        reason = payload.get("reason") or "Ack không hợp lệ"
        node_id = payload.get("node_id") or "node01"

    with get_db_connection() as conn:
        row = conn.execute("SELECT status FROM command_history WHERE cmd_id = ?", (cmd_id,)).fetchone()
        if row is None:
            return
        if row["status"] in {"timeout", "denied"}:
            if row["status"] == "timeout":
                reason = "Ack đến muộn sau timeout; không tự chấp nhận lại"
            return

    update_command_status(cmd_id, status, result=result, reason=reason, node_id=node_id, acked_at=int(time.time()))


def timeout_scan():
    while not _stop_event.is_set():
        with get_db_connection() as conn:
            rows = conn.execute(
                "SELECT cmd_id, sent_at, status FROM command_history WHERE status = 'pending'"
            ).fetchall()
        now = int(time.time())
        for row in rows:
            if now - int(row["sent_at"]) >= TIMEOUT_SECONDS:
                update_command_status(
                    row["cmd_id"],
                    "timeout",
                    result="timeout",
                    reason="Không nhận ack trong 5 giây; trạng thái quá hạn",
                    node_id=None,
                    acked_at=now,
                )
        time.sleep(0.5)


def append_jsonl(path: Path, payload: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(payload, ensure_ascii=False) + "\n")


def poll_local_ack_queue():
    seen = set()
    while not _stop_event.is_set():
        if not ACK_QUEUE_PATH.exists():
            time.sleep(0.3)
            continue
        try:
            with open(ACK_QUEUE_PATH, "r", encoding="utf-8") as f:
                lines = f.read().splitlines()
        except OSError:
            time.sleep(0.3)
            continue
        for line in lines:
            if not line.strip():
                continue
            if line in seen:
                continue
            seen.add(line)
            try:
                payload = json.loads(line)
            except Exception:
                continue
            handle_ack_message(payload)
        time.sleep(0.3)


def on_mqtt_message(client, userdata, message):
    try:
        payload = json.loads(message.payload.decode("utf-8"))
    except Exception:
        return
    handle_ack_message(payload)


def start_mqtt():
    global _mqtt_client
    if _mqtt_client is not None:
        return
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="dashboard_api")
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.on_message = on_mqtt_message
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=30)
    client.subscribe(TOPIC_ACK, qos=1)
    client.loop_start()
    _mqtt_client = client


@app.route("/")
def index():
    return send_file(DASHBOARD_DIR / "index.html")


@app.route("/dashboard/<path:filename>")
def static_dashboard(filename: str):
    return send_file(DASHBOARD_DIR / filename)


@app.route("/api/health")
def api_health():
    return jsonify({"status": "ok", "broker": MQTT_BROKER, "mqtt_topic": TOPIC_COMMAND, "timeout_seconds": TIMEOUT_SECONDS})


@app.route("/api/latest")
def api_latest():
    return jsonify(get_latest_telemetry())


@app.route("/api/commands")
def api_commands():
    limit = max(1, min(20, int(request.args.get("limit", "10"))))
    return jsonify({"commands": get_history(limit)})


@app.route("/api/command", methods=["POST"])
def api_command():
    global _mqtt_client
    data = request.get_json(silent=True) or {}
    token = request.headers.get("Authorization", "")
    if token.lower().startswith("bearer "):
        token = token.split(" ", 1)[1].strip()
    if not token:
        token = str(data.get("token") or "")

    allowed_user = ALLOWED_TOKENS.get(token)
    if not allowed_user:
        return jsonify({
            "status": "denied",
            "result": "rejected",
            "reason": "Tài khoản không hợp lệ hoặc token sai",
            "cmd_id": None,
            "node_received": False,
        }), 403

    device_id = str(data.get("device_id") or "node01")
    action = str(data.get("action") or "pump_on")
    cmd_id = str(uuid.uuid4())

    record_command(
        cmd_id=cmd_id,
        device_id=device_id,
        action=action,
        request_user=allowed_user,
        status="pending",
        result=None,
        reason="Đang chờ ACK",
        payload={"device_id": device_id, "action": action, "token_user": allowed_user},
    )

    mqtt_publish = {
        "cmd_id": cmd_id,
        "device_id": device_id,
        "action": action,
        "issued_at": int(time.time()),
        "token_user": allowed_user,
    }
    if _mqtt_client is not None:
        try:
            _mqtt_client.publish(TOPIC_COMMAND, json.dumps(mqtt_publish, ensure_ascii=False), qos=1)
        except Exception:
            _mqtt_client = None
    if _mqtt_client is None:
        append_jsonl(COMMAND_QUEUE_PATH, mqtt_publish)

    return jsonify({
        "cmd_id": cmd_id,
        "status": "waiting",
        "result": None,
        "device_id": device_id,
        "action": action,
        "message": "Lệnh đã được gửi xuống node; chờ ACK từ thiết bị",
        "timeout_seconds": TIMEOUT_SECONDS,
        "node_received": True,
    }), 202


if __name__ == "__main__":
    ensure_db()
    try:
        start_mqtt()
    except Exception:
        _mqtt_client = None
    timeout_thread = threading.Thread(target=timeout_scan, daemon=True)
    timeout_thread.start()
    poll_thread = threading.Thread(target=poll_local_ack_queue, daemon=True)
    poll_thread.start()
    app.run(host="0.0.0.0", port=5000, debug=False)
