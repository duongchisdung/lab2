#!/usr/bin/env python3
"""Đối chiếu số bản tin trong log subscribe với chu kỳ gửi.

Dùng:  python3 tools/check_log.py log_10min.txt --period 2
Log là đầu ra của:  mosquitto_sub -t "nhom01/node01/telemetry" -v | tee log_10min.txt
Dòng có dạng:  nhom01/node01/telemetry {"device_id":...,"ts":123456,...}
"""
import argparse, json, sys

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("--period", type=float, default=2.0, help="chu kỳ gửi (giây)")
a = ap.parse_args()

rows = []
for line in open(a.log, encoding="utf-8"):
    line = line.strip()
    if "{" not in line:
        continue
    try:
        rows.append(json.loads(line[line.index("{"):]))
    except json.JSONDecodeError:
        print("bỏ qua dòng hỏng:", line[:80], file=sys.stderr)

if len(rows) < 2:
    sys.exit("Chưa đủ bản tin để đối chiếu")

ts = [r["ts"] for r in rows]
span = (ts[-1] - ts[0]) / 1000
expected = int(span / a.period) + 1
gaps = [(ts[i - 1], ts[i]) for i in range(1, len(ts)) if ts[i] - ts[i - 1] > 1.5 * a.period * 1000]
nulls = sum(1 for r in rows if r.get("temp") is None)

print(f"Số bản tin nhận : {len(rows)}")
print(f"Khoảng thời gian: {span:.0f} s ({span/60:.1f} phút)")
print(f"Số bản tin dự kiến: {expected}  -> thiếu {max(0, expected-len(rows))}")
print(f"Bản tin có temp=null: {nulls}")
print(f"Khoảng trống > 1.5 chu kỳ: {len(gaps)}")
for g0, g1 in gaps:
    print(f"  ts {g0} -> {g1}  (trống {(g1-g0)/1000:.1f} s)")
print("ĐẠT ≥ 10 phút" if span >= 600 else "CHƯA đủ 10 phút")
