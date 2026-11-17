#!/usr/bin/env python3
"""Mô phỏng node nhà kính: nhận lệnh và publish ACK."""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

sys.stdout.reconfigure(encoding='utf-8')

import paho.mqtt.client as mqtt

APP_ROOT = Path(__file__).resolve().parent.parent
COMMAND_QUEUE_PATH = APP_ROOT / "api" / "command_queue.jsonl"
ACK_QUEUE_PATH = APP_ROOT / "api" / "ack_queue.jsonl"

MQTT_BROKER = os.getenv("MQTT_BROKER_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_BROKER_PORT", "1883"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME", "iot_user")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "iot_password123")
TOPIC_COMMAND = "nhom01/node01/cmd"
TOPIC_ACK = "nhom01/node01/ack"
NODE_ID = os.getenv("NODE_ID", "node01")
BLOCK_ACK = os.getenv("BLOCK_ACK", "0").lower() in {"1", "true", "yes"}
REJECT_MODE = os.getenv("REJECT_MODE", "0").lower() in {"1", "true", "yes"}

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="mock_node_greenhouse")
client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)


def append_jsonl(path: Path, payload: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(payload, ensure_ascii=False) + "\n")


def publish_ack(cmd_id: str, result: str, reason: str | None = None, action: str | None = None):
    payload = {
        "cmd_id": cmd_id,
        "node_id": NODE_ID,
        "result": result,
        "action": action,
        "reason": reason or ("Thực thi thành công" if result == "success" else "Từ chối vì an toàn"),
    }
    if client.is_connected():
        client.publish(TOPIC_ACK, json.dumps(payload, ensure_ascii=False), qos=1)
    else:
        append_jsonl(ACK_QUEUE_PATH, payload)
    print(f"[ACK] cmd_id={cmd_id} result={result} action={action} reason={payload['reason']}")


def process_command(payload: dict):
    cmd_id = payload.get("cmd_id")
    action = payload.get("action", "pump_on")
    if not cmd_id:
        print("[CMD] Lệnh không có cmd_id, bỏ qua")
        return
    print(f"[CMD] nhận lệnh: cmd_id={cmd_id} action={action}")
    if BLOCK_ACK:
        print(f"[SIM] chặn ACK để test timeout cho cmd_id={cmd_id}")
        return
    time.sleep(0.5)
    if REJECT_MODE:
        publish_ack(cmd_id, "rejected", "Node từ chối vì an toàn", action)
        return
    publish_ack(cmd_id, "success", "Thực thi thành công", action)


def process_queue_file():
    seen = set()
    while True:
        if not COMMAND_QUEUE_PATH.exists():
            time.sleep(0.4)
            continue
        try:
            with open(COMMAND_QUEUE_PATH, "r", encoding="utf-8") as f:
                lines = f.read().splitlines()
        except OSError:
            time.sleep(0.4)
            continue
        for line in lines:
            if not line.strip() or line in seen:
                continue
            seen.add(line)
            try:
                payload = json.loads(line)
            except Exception:
                continue
            process_command(payload)
        time.sleep(0.4)


def on_command(client, userdata, message):
    payload = json.loads(message.payload.decode("utf-8"))
    process_command(payload)


print(f"[NODE] Mock node {NODE_ID} đang lắng nghe queue local: {COMMAND_QUEUE_PATH}")
print(f"[NODE] BLOCK_ACK={BLOCK_ACK}, REJECT_MODE={REJECT_MODE}")
try:
    process_queue_file()
except KeyboardInterrupt:
    print("[NODE] Ngắt kết nối mock node")
