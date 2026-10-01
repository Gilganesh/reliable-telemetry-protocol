"""
web/app.py -- Блок E: веб-дашборд телеметрії (графіки + SQLite + MQTT).

Розподіл ролей (контракт: docs/team-blocks/00-overview-shared-contract.md):
  * Gateway (gateway.c) -- ЄДИНЕ джерело правди про received/lost/duplicate/
    online/corrupted. Веб лише читає його готовий стан з STATE_TOPIC і нічого
    з цього не рахує сам.
  * Веб читає UPLINK_TOPIC ТІЛЬКИ щоб зберегти значення сенсорів для графіків
    (SQLite). Лічильників тут нема.
  * Дублікати не потрапляють у базу: UNIQUE(node_id, sequence, ts_ms) +
    INSERT OR IGNORE. Повторна доставка того самого пакета (retry, буфер
    store-and-forward) ігнорується; перезапуск плати (sequence з нуля, інший
    ts_ms) -- ні.
  * ts_ms від вузла -- це millis() плати, а не годинник. Вісь часу графіків --
    received_at (час прийому на цьому ноутбуці).

Запуск:
    python3 app.py [IP_брокера] [Порт_брокера]
Змінні середовища:
    WEB_HOST (за замовчуванням 127.0.0.1 -- керування відкрите лише локально),
    WEB_PORT (5000), TELEMETRY_DB (web/telemetry.db), RETENTION_HOURS (24).
"""
import json
import os
import sqlite3
import sys
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt
from flask import Flask, jsonify, request, send_from_directory

from protocol_codec import CorruptPacketError, MsgType, Packet

STATE_TOPIC = "case24/gateway/state"
UPLINK_TOPIC = "case24/uplink"

BASE_DIR = Path(__file__).resolve().parent
DB_PATH = Path(os.environ.get("TELEMETRY_DB", BASE_DIR / "telemetry.db"))
RETENTION_HOURS = float(os.environ.get("RETENTION_HOURS", "24"))
MAX_HISTORY_POINTS = 600
MAX_EVENTS = 50

app = Flask(__name__, static_folder=str(BASE_DIR / "static"), static_url_path="/static")

nodes = {}
nodes_lock = threading.Lock()
corrupted_count = 0
gateway_state_at = 0.0  # wall-clock останнього повідомлення від Gateway

mqtt_state = {
    "client": None,
    "broker_host": "localhost",
    "broker_port": 1883,
    "connected": False,
    "last_error": "Ще не підключено",
}

# downlink sequence: стартуємо від поточного часу, щоб після перезапуску веб
# не повторював sequence, які вузол уже бачив
_seq_lock = threading.Lock()
_downlink_seq = int(time.time()) & 0x7FFFFFFF


def next_downlink_seq() -> int:
    global _downlink_seq
    with _seq_lock:
        _downlink_seq = (_downlink_seq + 1) & 0xFFFFFFFF
        return _downlink_seq


# ---------------------------------------------------------------------------
# SQLite
# ---------------------------------------------------------------------------

_db = None
_db_lock = threading.Lock()


def init_db():
    global _db
    _db = sqlite3.connect(str(DB_PATH), check_same_thread=False)
    _db.row_factory = sqlite3.Row
    with _db_lock:
        _db.execute("PRAGMA journal_mode=WAL")
        _db.execute(
            """
            CREATE TABLE IF NOT EXISTS samples (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                node_id     INTEGER NOT NULL,
                sequence    INTEGER NOT NULL,
                ts_ms       INTEGER NOT NULL,
                received_at REAL    NOT NULL,
                metric      TEXT    NOT NULL,
                value       REAL    NOT NULL,
                UNIQUE (node_id, sequence, ts_ms, metric)
            )
            """
        )
        _db.execute(
            "CREATE INDEX IF NOT EXISTS idx_samples_lookup "
            "ON samples(node_id, metric, received_at)"
        )
        _db.execute(
            """
            CREATE TABLE IF NOT EXISTS events (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                node_id     INTEGER NOT NULL,
                sequence    INTEGER NOT NULL,
                ts_ms       INTEGER NOT NULL,
                received_at REAL    NOT NULL,
                text        TEXT    NOT NULL,
                UNIQUE (node_id, sequence, ts_ms)
            )
            """
        )
        _db.commit()


def numeric_fields(payload: dict) -> dict:
    """Лише числові поля payload (bool і рядки пропускаємо)."""
    out = {}
    for key, val in payload.items():
        if isinstance(val, bool) or not isinstance(val, (int, float)):
            continue
        if val != val or val in (float("inf"), float("-inf")):
            continue
        out[str(key)[:32]] = float(val)
    return out


def store_sample(node_id: int, pkt: Packet, payload: dict):
    metrics = numeric_fields(payload)
    if not metrics:
        return
    now = time.time()
    with _db_lock:
        _db.executemany(
            "INSERT OR IGNORE INTO samples "
            "(node_id, sequence, ts_ms, received_at, metric, value) VALUES (?,?,?,?,?,?)",
            [(node_id, pkt.sequence, pkt.timestamp_ms, now, m, v) for m, v in metrics.items()],
        )
        _db.commit()


def store_event(node_id: int, pkt: Packet, text: str):
    with _db_lock:
        _db.execute(
            "INSERT OR IGNORE INTO events (node_id, sequence, ts_ms, received_at, text) "
            "VALUES (?,?,?,?,?)",
            (node_id, pkt.sequence, pkt.timestamp_ms, time.time(), text[:200]),
        )
        _db.commit()


def prune_old():
    cutoff = time.time() - RETENTION_HOURS * 3600
    with _db_lock:
        _db.execute("DELETE FROM samples WHERE received_at < ?", (cutoff,))
        _db.execute("DELETE FROM events WHERE received_at < ?", (cutoff,))
        _db.commit()


def pruner_loop():
    while True:
        time.sleep(300)
        try:
            prune_old()
        except Exception as e:
            print(f"[DB] помилка очищення: {e}")


def fetch_history(node_id: int, metric: str, since_s: float):
    since = time.time() - since_s
    with _db_lock:
        rows = _db.execute(
            """
            SELECT received_at, value FROM samples
            WHERE node_id = ? AND metric = ? AND received_at >= ?
            ORDER BY received_at DESC LIMIT ?
            """,
            (node_id, metric, since, MAX_HISTORY_POINTS),
        ).fetchall()
    return [{"t": int(r["received_at"] * 1000), "v": r["value"]} for r in reversed(rows)]


def fetch_metrics():
    with _db_lock:
        rows = _db.execute(
            "SELECT node_id, metric FROM samples GROUP BY node_id, metric ORDER BY node_id, metric"
        ).fetchall()
    result = {}
    for r in rows:
        result.setdefault(r["node_id"], []).append(r["metric"])
    return result


def fetch_events():
    with _db_lock:
        rows = _db.execute(
            "SELECT node_id, received_at, text FROM events ORDER BY id DESC LIMIT ?",
            (MAX_EVENTS,),
        ).fetchall()
    return [{"node_id": r["node_id"], "t": int(r["received_at"] * 1000), "text": r["text"]} for r in rows]


# ---------------------------------------------------------------------------
# Nodes / MQTT
# ---------------------------------------------------------------------------

def get_or_create_node(node_id: int, ip_address: str = "—", name: str = ""):
    if node_id not in nodes:
        nodes[node_id] = {
            "node_id": node_id,
            "name": name or f"Вузол #{node_id}",
            "ip_address": ip_address,
            "max_seq_seen": -1,
            "received_count": 0,
            "lost_count": 0,
            "duplicate_count": 0,
            "loss_rate": 0.0,
            "online": False,
            "transport": "?",
            "last_seen_ms_ago": None,
            "alarm_active": False,
        }
    return nodes[node_id]


def on_gateway_state(msg):
    global corrupted_count, gateway_state_at
    try:
        data = json.loads(msg.payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return
    if not isinstance(data, dict):
        return
    with nodes_lock:
        gateway_state_at = time.time()
        corrupted_count = int(data.get("corrupted_count", corrupted_count) or 0)
        for gw in data.get("nodes", []):
            node_id = gw.get("node_id")
            if not isinstance(node_id, int):
                continue
            node = get_or_create_node(node_id)
            for key in ("online", "transport", "received_count", "lost_count",
                        "duplicate_count", "max_seq_seen", "loss_rate", "last_seen_ms_ago"):
                if key in gw:
                    node[key] = gw[key]


def on_uplink(msg):
    """Тільки збереження даних для графіків. Битий пакет мовчки пропускаємо --
    Gateway його вже порахував (corrupted_count) і записав у свій лог."""
    try:
        pkt = Packet.unpack(msg.payload)
    except CorruptPacketError:
        return

    if pkt.msg_type not in (MsgType.TELEMETRY, MsgType.HEARTBEAT, MsgType.ALARM):
        return

    try:
        payload = json.loads(pkt.payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        payload = {}
    if not isinstance(payload, dict):
        payload = {}

    with nodes_lock:
        node = get_or_create_node(pkt.node_id)
        if "ip" in payload:
            node["ip_address"] = str(payload["ip"])[:45]
        if pkt.msg_type == MsgType.ALARM:
            node["alarm_active"] = True

    try:
        if pkt.msg_type == MsgType.ALARM:
            store_event(pkt.node_id, pkt, json.dumps(payload, ensure_ascii=False))
        else:
            store_sample(pkt.node_id, pkt, payload)
    except sqlite3.Error as e:
        print(f"[DB] помилка запису: {e}")


def on_message(client, userdata, msg):
    if msg.topic == STATE_TOPIC:
        on_gateway_state(msg)
    elif msg.topic == UPLINK_TOPIC:
        on_uplink(msg)


def _rc_failed(rc) -> bool:
    # paho 2.x передає ReasonCode, paho 1.x -- int
    return rc.is_failure if hasattr(rc, "is_failure") else rc != 0


def on_connect(client, userdata, flags, rc, properties=None):
    if _rc_failed(rc):
        mqtt_state["connected"] = False
        mqtt_state["last_error"] = f"Брокер відхилив підключення ({rc})"
        print(f"[MQTT ERROR] {mqtt_state['last_error']}")
        return
    mqtt_state["connected"] = True
    mqtt_state["last_error"] = "Успішно підключено"
    client.subscribe([(STATE_TOPIC, 0), (UPLINK_TOPIC, 0)])
    print(f"[MQTT] Підключено до {mqtt_state['broker_host']}:{mqtt_state['broker_port']}")


def on_disconnect(client, userdata, *args):
    mqtt_state["connected"] = False
    mqtt_state["last_error"] = "З'єднання з брокером втрачено, перепідключення..."


def make_client():
    try:
        # paho-mqtt 2.x: сигнатури on_connect/on_disconnect тримаємо
        # сумісними з обома версіями (див. вище)
        return mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        return mqtt.Client()  # paho-mqtt 1.x


def start_mqtt_client(host: str, port: int = 1883):
    old = mqtt_state["client"]
    if old is not None:
        try:
            old.loop_stop()
            old.disconnect()
        except Exception:
            pass

    client = make_client()
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=10)

    mqtt_state.update(client=client, broker_host=host, broker_port=port, connected=False,
                      last_error="Підключення...")
    try:
        # connect_async + loop_start: якщо брокера ще нема, paho сам
        # перепробовує, а не падає з винятком
        client.connect_async(host, port, 60)
        client.loop_start()
        return True, "Підключення ініційовано"
    except Exception as e:
        mqtt_state["last_error"] = f"Не вдалося запустити клієнт для {host}:{port} -> {e}"
        print(f"[MQTT EXCEPTION] {mqtt_state['last_error']}")
        return False, mqtt_state["last_error"]


def publish_packet(node_id: int, msg_type: int, payload_dict: dict) -> bool:
    client = mqtt_state["client"]
    if client is None or not mqtt_state["connected"]:
        return False
    pkt = Packet(
        version=1,
        msg_type=msg_type,
        node_id=node_id,
        sequence=next_downlink_seq(),
        timestamp_ms=int(time.time() * 1000),
        payload=json.dumps(payload_dict).encode("utf-8"),
    )
    res = client.publish(f"case24/downlink/{node_id}", pkt.pack())
    return res.rc == mqtt.MQTT_ERR_SUCCESS


# ---------------------------------------------------------------------------
# API
# ---------------------------------------------------------------------------

def body() -> dict:
    data = request.get_json(silent=True)
    return data if isinstance(data, dict) else {}


def bad(msg, code=400):
    return jsonify({"error": msg}), code


@app.route("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


@app.route("/api/state")
def api_state():
    with nodes_lock:
        rows = []
        for n in sorted(nodes.values(), key=lambda x: x["node_id"]):
            ago = n["last_seen_ms_ago"]
            rows.append({**n, "last_seen_s": None if ago is None else round(ago / 1000, 1)})
        gateway_age = round(time.time() - gateway_state_at, 1) if gateway_state_at else None
        corrupted = corrupted_count
    # якщо Gateway замовк -- його "online" застаріло, дашборд має про це сказати
    return jsonify({
        "nodes": rows,
        "corrupted_count": corrupted,
        "gateway_age_s": gateway_age,
        "mqtt_config": {
            "host": mqtt_state["broker_host"],
            "port": mqtt_state["broker_port"],
            "connected": mqtt_state["connected"],
            "last_error": mqtt_state["last_error"],
        },
    })


@app.route("/api/metrics")
def api_metrics():
    return jsonify(fetch_metrics())


@app.route("/api/history")
def api_history():
    try:
        node_id = int(request.args["node_id"])
        since_s = min(max(float(request.args.get("since_s", 300)), 10), RETENTION_HOURS * 3600)
    except (KeyError, ValueError):
        return bad("потрібні node_id та since_s")
    metric = request.args.get("metric", "")
    if not metric:
        return bad("потрібен metric")
    return jsonify({"node_id": node_id, "metric": metric, "points": fetch_history(node_id, metric, since_s)})


@app.route("/api/events")
def api_events():
    return jsonify(fetch_events())


@app.route("/api/nodes/add", methods=["POST"])
def api_add_node():
    data = body()
    try:
        node_id = int(data.get("node_id"))
    except (TypeError, ValueError):
        return bad("node_id має бути числом")
    if not 0 < node_id < 65536:
        return bad("node_id має бути в межах 1..65535")
    name = str(data.get("name", "")).strip()[:40]
    ip = str(data.get("ip_address", "")).strip()[:45] or "—"
    with nodes_lock:
        node = get_or_create_node(node_id, ip_address=ip, name=name)
        if name:
            node["name"] = name
        if ip != "—":
            node["ip_address"] = ip
    return jsonify({"success": True, "node_id": node_id})


@app.route("/api/config/broker", methods=["POST"])
def api_config_broker():
    data = body()
    host = str(data.get("host", "localhost")).strip() or "localhost"
    try:
        port = int(data.get("port", 1883))
    except (TypeError, ValueError):
        return bad("port має бути числом")
    if not 0 < port < 65536:
        return bad("port поза діапазоном")
    ok, msg = start_mqtt_client(host, port)
    return jsonify({"success": ok, "message": msg})


@app.route("/api/servo/<int:node_id>", methods=["POST"])
def api_servo(node_id):
    angle = body().get("angle")
    if isinstance(angle, bool) or not isinstance(angle, (int, float)) or not 0 <= angle <= 180:
        return bad("потрібен angle 0..180")
    return jsonify({"sent": publish_packet(node_id, MsgType.CONFIG, {"cmd": "servo", "angle": angle})})


@app.route("/api/simulate-loss/<int:node_id>", methods=["POST"])
def api_simulate_loss(node_id):
    percent = body().get("loss_percent")
    if isinstance(percent, bool) or not isinstance(percent, (int, float)) or not 0 <= percent <= 100:
        return bad("потрібен loss_percent 0..100")
    return jsonify({"sent": publish_packet(
        node_id, MsgType.CONFIG, {"cmd": "simulate_loss", "loss_percent": percent})})


@app.route("/api/alarm/<int:node_id>", methods=["POST"])
def api_alarm(node_id):
    state = bool(body().get("state", True))
    with nodes_lock:
        if node_id in nodes:
            nodes[node_id]["alarm_active"] = state
    ok = publish_packet(node_id, MsgType.ALARM, {"cmd": "alarm", "active": state})
    return jsonify({"sent": ok, "node_id": node_id, "active": state})


@app.route("/api/alarm/all", methods=["POST"])
def api_alarm_all():
    state = bool(body().get("state", True))
    with nodes_lock:
        ids = list(nodes)
        for node_id in ids:
            nodes[node_id]["alarm_active"] = state
    # публікуємо поза замком, щоб мережа не блокувала решту API
    sent = [publish_packet(i, MsgType.ALARM, {"cmd": "alarm", "active": state}) for i in ids]
    return jsonify({"sent": all(sent) if sent else False, "active": state, "nodes": len(ids)})


if __name__ == "__main__":
    init_db()
    threading.Thread(target=pruner_loop, daemon=True).start()
    broker_host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    broker_port = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
    start_mqtt_client(broker_host, broker_port)
    app.run(host=os.environ.get("WEB_HOST", "127.0.0.1"),
            port=int(os.environ.get("WEB_PORT", "5000")),
            debug=False, threaded=True)
