#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>

extern "C" {
  #include "protocol.h"
  #include "reliability.h"
  #include "packet_queue.h"
}

#ifdef NODE_LCD
  #ifndef NODE_I2C_SDA
    #define NODE_I2C_SDA 21
  #endif
  #ifndef NODE_I2C_SCL
    #define NODE_I2C_SCL 18
  #endif
  #include "node_lcd.h"
  bool sensor_display(char *line, size_t n);
  void lcd_flash(const char *msg);
  #define LCD_FLASH(msg) lcd_flash(msg)
#else
  #define LCD_FLASH(msg) ((void)0)
#endif

void sensor_setup();
void sensor_update();
bool sensor_payload(char *buf, size_t n);

void send_alarm();
void send_alarm_json(const char *json);

uint16_t MY_NODE_ID = 0;
char device_mac[18] = "";
bool id_confirmed = false;

#define SERVO_ENABLED false
#define SERVO_PIN     13
#if SERVO_ENABLED
  #include <ESP32Servo.h>
  Servo servo;
#endif

#define LINK_VIA_USB_CABLE 1
#define UART_RX_PIN 16
#define UART_TX_PIN 17
#define UART_BAUD   115200

#if LINK_VIA_USB_CABLE
  #define GatewaySerial Serial
  class NullPrint : public Print {
   public:
    size_t write(uint8_t) override { return 1; }
    size_t write(const uint8_t*, size_t n) override { return n; }
  };
  NullPrint DBG;
#else
  HardwareSerial GatewaySerial(2);
  #define DBG Serial
#endif

#define WIRE_TIMEOUT_MS   3000
#define UDP_LOCAL_PORT    12345
#define BUFFER_CAPACITY   50
#define FLUSH_INTERVAL_MS 50
#define FLUSH_RETRY_PAUSE_MS 1000
#define ALARM_QUEUE_CAP   8
#define ALARM_RETRY_COOLDOWN_MS 5000
#define TCP_KEEPALIVE_MS  5000
#define WIFI_PROBE_MS        2000
#define WIFI_LINK_TIMEOUT_MS 7000

Preferences prefs;
char wifi_ssid[64] = "";
char wifi_pass[96] = "";
char gw_ip_str[16] = "";
uint16_t udp_port = 5005;
uint16_t tcp_port = 5006;
bool wifi_configured = false;
bool gw_configured = false;
IPAddress gateway_ip;

WiFiUDP udp;
WiFiClient tcp_client;
bool udp_started = false;

unsigned long last_wire_rx = 0;
unsigned long last_tcp_rx = 0;
unsigned long last_udp_rx = 0;

uint32_t seq_counter = 0;
SensorPacket buffer_storage[BUFFER_CAPACITY];
PacketQueue tele_q = { buffer_storage, BUFFER_CAPACITY, 0, 0, 0 };
SensorPacket alarm_storage[ALARM_QUEUE_CAP];
PacketQueue alarm_q = { alarm_storage, ALARM_QUEUE_CAP, 0, 0, 0 };
SensorPacket alarm_inflight;
bool alarm_inflight_valid = false;
unsigned long alarm_retry_after = 0;
uint8_t sim_loss_percent = 0;
bool remote_alarm_active = false;
ReliableCtx reliable;
String serial_cmd_buffer = "";

struct FrameAssembler {
  uint8_t buf[HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE];
  size_t have = 0;
  size_t need = HEADER_SIZE;
  bool header_done = false;

  void reset() { have = 0; need = HEADER_SIZE; header_done = false; }

  unsigned long last_byte_ms = 0;

  bool plausible() {
    if (have >= 1 && buf[0] != PROTOCOL_VERSION) return false;
    if (have >= 2 && buf[1] > MSG_ACK) return false;
    if (have >= HEADER_SIZE) {
      uint16_t payload_len;
      memcpy(&payload_len, buf + 16, 2);
      if (payload_len > MAX_PAYLOAD_SIZE) return false;
    }
    return true;
  }

  bool feed(uint8_t b) {
    unsigned long now = millis();
    if (have > 0 && now - last_byte_ms > 100) reset();
    last_byte_ms = now;
    if (have >= sizeof(buf)) reset();
    buf[have++] = b;
    while (have > 0 && !plausible()) {
      memmove(buf, buf + 1, have - 1);
      have--;
    }
    if (have >= HEADER_SIZE) {
      uint16_t payload_len;
      memcpy(&payload_len, buf + 16, 2);
      need = HEADER_SIZE + payload_len + CRC_SIZE;
      header_done = true;
    } else {
      header_done = false;
      need = HEADER_SIZE;
    }
    return header_done && have == need;
  }
};
FrameAssembler uart_assembler;
FrameAssembler tcp_assembler;

enum Channel { CH_NONE, CH_UART, CH_TCP, CH_UDP };

bool wire_alive() {
  return last_wire_rx != 0 && (millis() - last_wire_rx) < WIRE_TIMEOUT_MS;
}
bool wifi_up() { return wifi_configured && WiFi.status() == WL_CONNECTED; }
bool tcp_up()  { return wifi_up() && gw_configured && tcp_client.connected() &&
                        last_tcp_rx != 0 && (millis() - last_tcp_rx) < WIFI_LINK_TIMEOUT_MS; }
bool udp_up()  { return wifi_up() && gw_configured && udp_started &&
                        last_udp_rx != 0 && (millis() - last_udp_rx) < WIFI_LINK_TIMEOUT_MS; }

Channel active_channel() {
  if (wire_alive()) return CH_UART;
  if (tcp_up()) return CH_TCP;
  if (udp_up()) return CH_UDP;
  return CH_NONE;
}

const char* channel_name(Channel c) {
  switch (c) {
    case CH_UART: return "UART";
    case CH_TCP:  return "TCP";
    case CH_UDP:  return "UDP";
    default:      return "none";
  }
}

bool udp_send(const uint8_t *buf, int len) {
  if (!udp_up()) return false;
  udp.beginPacket(gateway_ip, udp_port);
  udp.write(buf, len);
  return udp.endPacket() == 1;
}

bool node_send(const uint8_t *buf, int len) {
  if (sim_loss_percent > 0 && random(100) < sim_loss_percent) {
    DBG.print("[LOSS-SIM] packet dropped (simulated loss ");
    DBG.print(sim_loss_percent);
    DBG.println("%)");
    return true;
  }
  switch (active_channel()) {
    case CH_UART:
      GatewaySerial.write(buf, len);
      return true;
    case CH_TCP:
      if (tcp_client.write(buf, len) == (size_t)len) return true;
      return udp_send(buf, len);
    case CH_UDP:
      return udp_send(buf, len);
    default:
      return false;
  }
}

bool transport_ready() { return active_channel() != CH_NONE; }

void esp32_reliable_send(void *userdata, const uint8_t *buf, int len) {
  (void)userdata;
  node_send(buf, len);
}

bool json_find_value(const char *js, const char *key, const char **val_start) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = js;
  while ((p = strstr(p, pat)) != NULL) {
    const char *q = p + strlen(pat);
    while (*q == ' ') q++;
    if (*q == ':') {
      q++;
      while (*q == ' ') q++;
      *val_start = q;
      return true;
    }
    p += 1;
  }
  return false;
}

bool json_get_string(const char *js, const char *key, char *out, size_t n) {
  const char *v;
  if (!json_find_value(js, key, &v) || *v != '"') return false;
  v++;
  size_t o = 0;
  while (*v && *v != '"') {
    char c = *v++;
    if (c == '\\' && *v) {
      char e = *v++;
      if (e == 'u' && strlen(v) >= 4) {
        char hex[5] = { v[0], v[1], v[2], v[3], 0 };
        long code = strtol(hex, NULL, 16);
        v += 4;
        c = (code < 0x80) ? (char)code : '?';
      } else {
        c = e;
      }
    }
    if (o + 1 < n) out[o++] = c;
  }
  out[o] = '\0';
  return *v == '"';
}

bool json_get_bool(const char *js, const char *key, bool *out) {
  const char *v;
  if (!json_find_value(js, key, &v)) return false;
  if (strncmp(v, "true", 4) == 0)  { *out = true;  return true; }
  if (strncmp(v, "false", 5) == 0) { *out = false; return true; }
  return false;
}

bool json_get_int(const char *js, const char *key, long *out) {
  const char *v;
  if (!json_find_value(js, key, &v)) return false;
  *out = strtol(v, NULL, 10);
  return true;
}

void wifi_start() {
  if (!wifi_configured) return;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(wifi_ssid, wifi_pass);
  DBG.print("[WIFI] Connecting to \"");
  DBG.print(wifi_ssid);
  DBG.println("\"...");
}

void apply_wifi_config(const char *ssid, const char *pass) {
  bool changed = strcmp(ssid, wifi_ssid) != 0 || strcmp(pass, wifi_pass) != 0 || !wifi_configured;
  snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", ssid);
  snprintf(wifi_pass, sizeof(wifi_pass), "%s", pass);
  wifi_configured = wifi_ssid[0] != '\0';
  if (changed) {
    prefs.putString("ssid", wifi_ssid);
    prefs.putString("pass", wifi_pass);
    DBG.println("[CONFIG] New Wi-Fi settings received from gateway and saved.");
    wifi_start();
  }
}

void apply_gw_config(const char *ip, long udp_p, long tcp_p) {
  IPAddress parsed;
  if (!parsed.fromString(ip)) {
    DBG.println("[CONFIG] Gateway sent an invalid IP, ignoring.");
    return;
  }
  bool changed = !gw_configured || strcmp(ip, gw_ip_str) != 0 ||
                 (udp_p > 0 && udp_p != udp_port) || (tcp_p > 0 && tcp_p != tcp_port);
  snprintf(gw_ip_str, sizeof(gw_ip_str), "%s", ip);
  gateway_ip = parsed;
  if (udp_p > 0) udp_port = (uint16_t)udp_p;
  if (tcp_p > 0) tcp_port = (uint16_t)tcp_p;
  gw_configured = true;
  if (changed) {
    prefs.putString("gw", gw_ip_str);
    prefs.putUShort("udp", udp_port);
    prefs.putUShort("tcp", tcp_port);
    tcp_client.stop();
    DBG.print("[CONFIG] Gateway address: ");
    DBG.print(gw_ip_str);
    DBG.print(" (UDP ");
    DBG.print(udp_port);
    DBG.print(", TCP ");
    DBG.print(tcp_port);
    DBG.println("), saved.");
  }
}

void send_ack_to_gateway(uint32_t seq) {
  SensorPacket ack = {};
  ack.version = PROTOCOL_VERSION;
  ack.msg_type = MSG_ACK;
  ack.node_id = MY_NODE_ID;
  ack.sequence = seq;
  ack.timestamp_ms = millis();
  uint8_t tx[64];
  int len = protocol_pack(&ack, tx, sizeof(tx));
  if (len > 0) node_send(tx, len);
}

void send_hello() {
  SensorPacket h = {};
  h.version = PROTOCOL_VERSION;
  h.msg_type = MSG_HEARTBEAT;
  h.node_id = 0;
  h.sequence = 0;
  h.timestamp_ms = millis();
  snprintf((char*)h.payload, MAX_PAYLOAD_SIZE, "{\"mac\":\"%s\",\"id\":%u}", device_mac, MY_NODE_ID);
  h.payload_len = strlen((char*)h.payload);
  uint8_t tx[128];
  int len = protocol_pack(&h, tx, sizeof(tx));
  if (len > 0) node_send(tx, len);
}

void send_wifi_probe() {
  if (!wifi_up() || !gw_configured) return;
  SensorPacket h = {};
  h.version = PROTOCOL_VERSION;
  h.msg_type = MSG_HEARTBEAT;
  h.node_id = 0;
  h.sequence = 0;
  h.timestamp_ms = millis();
  snprintf((char*)h.payload, MAX_PAYLOAD_SIZE, "{\"mac\":\"%s\",\"id\":%u%s}",
           device_mac, MY_NODE_ID, id_confirmed ? ",\"probe\":1" : "");
  h.payload_len = strlen((char*)h.payload);
  uint8_t tx[128];
  int len = protocol_pack(&h, tx, sizeof(tx));
  if (len <= 0) return;
  if (tcp_client.connected()) tcp_client.write(tx, len);
  if (udp_started) {
    udp.beginPacket(gateway_ip, udp_port);
    udp.write(tx, len);
    udp.endPacket();
  }
}

void handle_config_packet(const SensorPacket *pkt) {
  char js[MAX_PAYLOAD_SIZE + 1];
  memcpy(js, pkt->payload, pkt->payload_len);
  js[pkt->payload_len] = '\0';

  char cmd[16];
  if (!json_get_string(js, "cmd", cmd, sizeof(cmd))) {
    DBG.println("[CONFIG] CONFIG without cmd field, ignoring.");
    return;
  }
  if (strcmp(cmd, "id") == 0) {
    char mac[18];
    long id = 0;
    if (!json_get_string(js, "mac", mac, sizeof(mac)) || strcmp(mac, device_mac) != 0) return;
    if (!json_get_int(js, "id", &id) || id <= 0 || id > 65535) return;
    if (MY_NODE_ID != (uint16_t)id) {
      DBG.print("[ID] Gateway assigned node_id=");
      DBG.print(id);
      DBG.print(MY_NODE_ID ? " (was " : " (new board");
      if (MY_NODE_ID) { DBG.print(MY_NODE_ID); DBG.print(", reassigned)"); } else DBG.print(")");
      DBG.println();
      MY_NODE_ID = (uint16_t)id;
      prefs.putUShort("nid", MY_NODE_ID);
    }
    id_confirmed = true;
  } else if (strcmp(cmd, "servo") == 0) {
    long angle = 90;
    json_get_int(js, "angle", &angle);
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
#if SERVO_ENABLED
    servo.write((int)angle);
    DBG.print("[SERVO] Angle: ");
    DBG.println(angle);
#else
    DBG.print("[SERVO] Command received (angle=");
    DBG.print(angle);
    DBG.println("), but SERVO_ENABLED is false on this board");
#endif
    send_ack_to_gateway(pkt->sequence);
  } else if (strcmp(cmd, "wifi") == 0) {
    char ssid[64], pass[96];
    if (!json_get_string(js, "s", ssid, sizeof(ssid))) return;
    if (!json_get_string(js, "p", pass, sizeof(pass))) pass[0] = '\0';
    apply_wifi_config(ssid, pass);
    send_ack_to_gateway(pkt->sequence);
  } else if (strcmp(cmd, "gw") == 0) {
    char ip[16];
    long u = 0, t = 0;
    if (!json_get_string(js, "ip", ip, sizeof(ip))) return;
    json_get_int(js, "udp", &u);
    json_get_int(js, "tcp", &t);
    apply_gw_config(ip, u, t);
    send_ack_to_gateway(pkt->sequence);
  } else if (strcmp(cmd, "fire_alarm") == 0) {
    send_ack_to_gateway(pkt->sequence);
    send_alarm();
  } else if (strcmp(cmd, "ping") == 0) {
    send_ack_to_gateway(pkt->sequence);
  } else if (strcmp(cmd, "simulate_loss") == 0) {
    long pct = 0;
    json_get_int(js, "loss_percent", &pct);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    send_ack_to_gateway(pkt->sequence);
    sim_loss_percent = (uint8_t)pct;
    DBG.print("[CONFIG] Simulated outbound loss: ");
    DBG.print(pct);
    DBG.println("%");
  } else {
    DBG.print("[CONFIG] Unknown command: ");
    DBG.println(cmd);
  }
}

void handle_remote_alarm(const SensorPacket *pkt) {
  char js[MAX_PAYLOAD_SIZE + 1];
  memcpy(js, pkt->payload, pkt->payload_len);
  js[pkt->payload_len] = '\0';

  bool active = true;
  json_get_bool(js, "active", &active);
  send_ack_to_gateway(pkt->sequence);
  remote_alarm_active = active;
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, active ? HIGH : LOW);
#endif
  DBG.print("[ALARM] Remote alarm: ");
  DBG.println(active ? "ON" : "off");
  LCD_FLASH(active ? "WEB ALARM ON" : "Web alarm off");
}

void handle_downlink_bytes(const uint8_t *raw, size_t len, Channel src) {
  SensorPacket pkt;
  int rc = protocol_unpack(raw, len, &pkt);
  if (rc != PROTO_OK) {
    DBG.print("[WARN] Corrupted frame from gateway, code=");
    DBG.println(rc);
    return;
  }
  if (src == CH_UART) last_wire_rx = millis();
  else if (src == CH_TCP) last_tcp_rx = millis();
  else if (src == CH_UDP) last_udp_rx = millis();

  if (pkt.node_id != 0 && pkt.node_id != MY_NODE_ID) return;

  if (pkt.msg_type == MSG_ACK) {
    DBG.print("[ACK] Acknowledged sequence=");
    DBG.println(pkt.sequence);
    reliable_on_ack_received(&reliable, pkt.sequence);
  } else if (pkt.msg_type == MSG_CONFIG) {
    handle_config_packet(&pkt);
  } else if (pkt.msg_type == MSG_ALARM) {
    handle_remote_alarm(&pkt);
  }
}

void poll_downlink() {
  while (GatewaySerial.available()) {
    if (uart_assembler.feed((uint8_t)GatewaySerial.read())) {
      handle_downlink_bytes(uart_assembler.buf, uart_assembler.have, CH_UART);
      uart_assembler.reset();
    }
  }
  if (udp_started) {
    int packet_size = udp.parsePacket();
    if (packet_size > 0) {
      uint8_t buf[HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE];
      int len = udp.read(buf, sizeof(buf));
      if (len > 0) handle_downlink_bytes(buf, (size_t)len, CH_UDP);
    }
  }
  if (tcp_client.connected()) {
    while (tcp_client.available()) {
      if (tcp_assembler.feed((uint8_t)tcp_client.read())) {
        handle_downlink_bytes(tcp_assembler.buf, tcp_assembler.have, CH_TCP);
        tcp_assembler.reset();
      }
    }
  }
}

void stamp_buffer_stats(SensorPacket *pkt, int backlog) {
  if (pkt->msg_type != MSG_TELEMETRY || pkt->payload_len == 0 || pkt->payload_len >= MAX_PAYLOAD_SIZE) return;
  pkt->payload[pkt->payload_len] = '\0';
  char *old = strstr((char*)pkt->payload, ",\"backlog\":");
  if (old) {
    *old = '}';
    pkt->payload[old - (char*)pkt->payload + 1] = '\0';
    pkt->payload_len = (uint16_t)(old - (char*)pkt->payload + 1);
  }
  if (pkt->payload[pkt->payload_len - 1] != '}') return;
  int base = pkt->payload_len - 1;
  int n = snprintf((char*)pkt->payload + base, MAX_PAYLOAD_SIZE - base,
                   ",\"backlog\":%d,\"dropped\":%lu}", backlog, (unsigned long)tele_q.dropped);
  if (n > 0 && base + n < MAX_PAYLOAD_SIZE) {
    pkt->payload_len = (uint16_t)(base + n);
  } else {
    pkt->payload[base] = '}';
    pkt->payload[base + 1] = '\0';
  }
}

void buffer_packet(const SensorPacket* pkt) {
  if (pq_push(&tele_q, pkt)) {
    DBG.print("[BUFFER] Overflow, oldest packet dropped. Dropped total: ");
    DBG.println(tele_q.dropped);
  }
  DBG.print("[BUFFER] Packet stored locally. Buffered: ");
  DBG.println(tele_q.count);
}

void flush_buffer_step() {
  static bool flushing = false;
  static unsigned long pause_until = 0;
  SensorPacket *head = pq_peek(&tele_q);
  if (!head) return;
  if (pause_until != 0 && (long)(millis() - pause_until) < 0) return;
  if (!flushing) {
    DBG.print("[BUFFER] Link is up (");
    DBG.print(channel_name(active_channel()));
    DBG.print("), flushing buffered packets: ");
    DBG.println(tele_q.count);
    flushing = true;
  }

  if (MY_NODE_ID != 0) head->node_id = MY_NODE_ID;
  stamp_buffer_stats(head, tele_q.count - 1);

  uint8_t tx_buf[256];
  int packed_len = protocol_pack(head, tx_buf, sizeof(tx_buf));
  if (packed_len <= 0 || node_send(tx_buf, packed_len)) {
    pq_pop(&tele_q, NULL);
    pause_until = 0;
    if (pq_empty(&tele_q)) {
      DBG.println("[BUFFER] Flush complete.");
      flushing = false;
    }
  } else {
    DBG.println("[BUFFER] Send failed, retrying in 1 s.");
    flushing = false;
    pause_until = millis() + FLUSH_RETRY_PAUSE_MS;
    if (pause_until == 0) pause_until = 1;
  }
}

void alarm_enqueue(const SensorPacket* pkt) {
  if (pq_push(&alarm_q, pkt)) {
    DBG.print("[ALARM] Queue full, oldest ALARM dropped. Dropped total: ");
    DBG.println(alarm_q.dropped);
  }
}

void alarm_requeue_front(const SensorPacket* pkt) {
  if (pq_push_front(&alarm_q, pkt)) {
    DBG.print("[ALARM] Queue full, newest ALARM dropped. Dropped total: ");
    DBG.println(alarm_q.dropped);
  }
}

void alarm_pump() {
  if (pq_empty(&alarm_q) || reliable_is_busy(&reliable) || reliable.pending_success || !transport_ready()) return;
  if (alarm_retry_after != 0 && (long)(millis() - alarm_retry_after) < 0) return;

  SensorPacket pkt = *pq_peek(&alarm_q);
  if (MY_NODE_ID != 0) pkt.node_id = MY_NODE_ID;
  pq_pop(&alarm_q, NULL);
  if (!reliable_send_critical(&reliable, &pkt, millis())) {
    DBG.println("[ALARM] Failed to pack queued ALARM, skipping.");
    return;
  }
  alarm_inflight = pkt;
  alarm_inflight_valid = true;
  alarm_retry_after = 0;
  DBG.print("[ALARM] Sending sequence=");
  DBG.print(pkt.sequence);
  DBG.print(", still queued: ");
  DBG.println(alarm_q.count);
}

void send_telemetry() {
  SensorPacket pkt = {};
  pkt.version = PROTOCOL_VERSION;
  pkt.msg_type = MSG_TELEMETRY;
  pkt.node_id = MY_NODE_ID;
  pkt.timestamp_ms = millis();

  if (!sensor_payload((char*)pkt.payload, MAX_PAYLOAD_SIZE)) {
    DBG.println("[SENSOR] Measurement failed, telemetry packet skipped.");
    return;
  }
  pkt.payload_len = strlen((char*)pkt.payload);
  pkt.sequence = seq_counter++;

  if (!transport_ready() || !pq_empty(&tele_q)) {
    buffer_packet(&pkt);
    return;
  }

  stamp_buffer_stats(&pkt, 0);
  uint8_t tx_buf[256];
  int packed_len = protocol_pack(&pkt, tx_buf, sizeof(tx_buf));
  if (packed_len > 0) {
    Channel ch = active_channel();
    if (node_send(tx_buf, packed_len)) {
      DBG.print("[TX] seq=");
      DBG.print(pkt.sequence);
      DBG.print(" -> ");
      DBG.print(channel_name(ch));
      DBG.print(" (");
      DBG.print(packed_len);
      DBG.println(" bytes)");
    } else {
      buffer_packet(&pkt);
    }
  }
}

void send_alarm() {
  send_alarm_json("{\"alarm\":\"critical_event\"}");
}

void send_alarm_json(const char *json) {
  if (MY_NODE_ID == 0) {
    DBG.println("[ALARM] node_id not assigned by gateway yet.");
    return;
  }
  size_t json_len = strlen(json);
  if (json_len > MAX_PAYLOAD_SIZE) {
    DBG.println("[ALARM] Payload too long, alarm not sent.");
    return;
  }
  SensorPacket pkt = {};
  pkt.version = PROTOCOL_VERSION;
  pkt.msg_type = MSG_ALARM;
  pkt.node_id = MY_NODE_ID;
  pkt.sequence = seq_counter++;
  pkt.timestamp_ms = millis();

  pkt.payload_len = (uint16_t)json_len;
  memcpy(pkt.payload, json, json_len);

  DBG.print("[ALARM] ALARM sequence=");
  DBG.print(pkt.sequence);
  DBG.println(transport_ready() && !reliable_is_busy(&reliable)
                  ? " -- sending"
                  : " -- queued (no link or busy)");
  alarm_enqueue(&pkt);
  alarm_pump();
}

void print_status() {
  DBG.print("[STATUS] node_id=");
  DBG.print(MY_NODE_ID);
  DBG.print(id_confirmed ? " (confirmed)" : " (unconfirmed)");
  DBG.print(" | link=");
  DBG.print(channel_name(active_channel()));
  DBG.print(" | wire=");
  DBG.print(wire_alive() ? "up" : "down");
  DBG.print(" | Wi-Fi=");
  DBG.print(wifi_configured ? (WiFi.status() == WL_CONNECTED ? "connected" : "connecting") : "not configured");
  DBG.print(" | gateway=");
  DBG.print(gw_configured ? gw_ip_str : "not configured");
  DBG.print(" | TCP=");
  DBG.print(tcp_client.connected() ? (tcp_up() ? "up" : "connected, no reply") : "down");
  DBG.print(" | UDP=");
  DBG.print(udp_up() ? "up" : "no reply");
  DBG.print(" | buffer=");
  DBG.print(tele_q.count);
  DBG.print(" (dropped ");
  DBG.print(tele_q.dropped);
  DBG.print(") | remote ALARM=");
  DBG.print(remote_alarm_active ? "on" : "off");
  DBG.print(" | simulated loss=");
  DBG.print(sim_loss_percent);
  DBG.print("% | queued ALARMs=");
  DBG.print(alarm_q.count + (alarm_inflight_valid ? 1 : 0));
  DBG.print(" (dropped ");
  DBG.print(alarm_q.dropped);
  DBG.println(")");
}

void check_serial_commands() {
#if !LINK_VIA_USB_CABLE
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serial_cmd_buffer.length() > 0) {
        serial_cmd_buffer.trim();
        serial_cmd_buffer.toLowerCase();
        if (serial_cmd_buffer == "alarm") {
          send_alarm();
        } else if (serial_cmd_buffer == "status") {
          print_status();
        } else if (serial_cmd_buffer == "forget") {
          prefs.clear();
          DBG.println("[CONFIG] Settings erased. Reboot the board and connect it to the gateway over UART.");
        } else {
          DBG.print("[SERIAL] Unknown command: ");
          DBG.println(serial_cmd_buffer);
        }
        serial_cmd_buffer = "";
      }
    } else {
      serial_cmd_buffer += c;
    }
  }
#endif
}

void load_saved_config() {
  prefs.begin("node", false);
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  String g = prefs.getString("gw", "");
  udp_port = prefs.getUShort("udp", 5005);
  tcp_port = prefs.getUShort("tcp", 5006);
  MY_NODE_ID = prefs.getUShort("nid", 0);
  s.toCharArray(wifi_ssid, sizeof(wifi_ssid));
  p.toCharArray(wifi_pass, sizeof(wifi_pass));
  g.toCharArray(gw_ip_str, sizeof(gw_ip_str));
  wifi_configured = wifi_ssid[0] != '\0';
  gw_configured = gw_ip_str[0] != '\0' && gateway_ip.fromString(gw_ip_str);
}

#ifdef NODE_LCD
Lcd1602 lcd;
char lcd_shown[LCD_ROWS][LCD_COLS + 1] = {"", ""};
char lcd_flash_text[LCD_COLS + 1] = "";
unsigned long lcd_flash_until = 0;
unsigned long lcd_next_refresh = 0;
unsigned long lcd_next_probe = 0;

#define LCD_REFRESH_MS 250
#define LCD_PROBE_MS   5000
#define LCD_FLASH_MS   3000
#define LCD_PAGE_MS    3000

void lcd_message(const char *line1, const char *line2) {
  if (!lcd.ready()) return;
  lcd.printLine(0, line1);
  lcd.printLine(1, line2);
  snprintf(lcd_shown[0], sizeof(lcd_shown[0]), "%s", line1);
  snprintf(lcd_shown[1], sizeof(lcd_shown[1]), "%s", line2);
  lcd_next_refresh = millis() + 1500;
}

void lcd_init() {
  if (lcd.ready()) return;
  if (!lcd.begin()) {
    DBG.println("[LCD] Display not found on I2C (0x27 / 0x3F), continuing without it.");
    lcd_next_probe = millis() + LCD_PROBE_MS;
    return;
  }
  char msg[40];
  snprintf(msg, sizeof(msg), "[LCD] Display found at 0x%02X.", lcd.address());
  DBG.println(msg);
  lcd_message("Telemetry node", "Starting...");
}

void lcd_flash(const char *msg) {
  snprintf(lcd_flash_text, sizeof(lcd_flash_text), "%s", msg);
  lcd_flash_until = millis() + LCD_FLASH_MS;
  if (lcd_flash_until == 0) lcd_flash_until = 1;
}

void lcd_compose(char *line1, char *line2) {
  Channel ch = active_channel();
  const char *link = ch == CH_NONE ? "NO LINK" : channel_name(ch);
  char id[8];
  if (MY_NODE_ID != 0) snprintf(id, sizeof(id), "%u", MY_NODE_ID);
  else snprintf(id, sizeof(id), "---");
  snprintf(line1, LCD_COLS + 1, "%-7s Node %-3s", link, id);

  if (lcd_flash_until != 0 && (long)(millis() - lcd_flash_until) < 0) {
    snprintf(line2, LCD_COLS + 1, "%s", lcd_flash_text);
    return;
  }

  char info[LCD_COLS + 1];
  if (!sensor_display(info, sizeof(info)))
    snprintf(info, sizeof(info), "Buf:%d Drop:%lu", tele_q.count, (unsigned long)tele_q.dropped);

  int queued_alarms = alarm_q.count + (alarm_inflight_valid ? 1 : 0);
  bool backlog = tele_q.count > 0 || queued_alarms > 0;
  if (backlog && ((millis() / LCD_PAGE_MS) & 1))
    snprintf(line2, LCD_COLS + 1, "BUF %d ALM %d", tele_q.count, queued_alarms);
  else
    snprintf(line2, LCD_COLS + 1, "%s", info);
}

void lcd_service() {
  unsigned long now = millis();
  if (!lcd.ready()) {
    if ((long)(now - lcd_next_probe) < 0) return;
    lcd_next_probe = now + LCD_PROBE_MS;
    if (lcd.begin()) {
      lcd_shown[0][0] = '\0';
      lcd_shown[1][0] = '\0';
    }
    return;
  }
  if ((long)(now - lcd_next_refresh) < 0) return;
  lcd_next_refresh = now + LCD_REFRESH_MS;

  char lines[LCD_ROWS][LCD_COLS + 1];
  lcd_compose(lines[0], lines[1]);
  for (uint8_t row = 0; row < LCD_ROWS; row++) {
    if (strcmp(lines[row], lcd_shown[row]) == 0) continue;
    lcd.printLine(row, lines[row]);
    if (lcd.ready()) snprintf(lcd_shown[row], sizeof(lcd_shown[row]), "%s", lines[row]);
    return;
  }
}
#endif

void node_setup() {
  Serial.begin(115200);
  delay(2000);

  sensor_setup();

#if !LINK_VIA_USB_CABLE
  GatewaySerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
#endif

  uint64_t efuse = ESP.getEfuseMac();
  snprintf(device_mac, sizeof(device_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           (uint8_t)(efuse), (uint8_t)(efuse >> 8), (uint8_t)(efuse >> 16),
           (uint8_t)(efuse >> 24), (uint8_t)(efuse >> 32), (uint8_t)(efuse >> 40));
#if SERVO_ENABLED
  servo.attach(SERVO_PIN);
  servo.write(90);
#endif

  load_saved_config();
  WiFi.mode(WIFI_STA);
  if (wifi_configured) {
    DBG.println("[CONFIG] Saved settings found, starting Wi-Fi in background.");
    wifi_start();
  } else {
    DBG.println("[CONFIG] No Wi-Fi settings yet: connect the board to the gateway over UART to provision it.");
  }

  reliable_init(&reliable, esp32_reliable_send, NULL);

  DBG.print("Ready. MAC=");
  DBG.print(device_mac);
  DBG.print(", node_id=");
  DBG.print(MY_NODE_ID ? String(MY_NODE_ID) : String("waiting for gateway"));
  DBG.println(". Serial commands: alarm, status, forget.");
}

void node_loop() {
  static bool was_wifi = false;
  bool now_wifi = wifi_up();
  if (wifi_configured && !now_wifi) {
    static unsigned long last_attempt = 0;
    if (millis() - last_attempt > 8000) {
      last_attempt = millis();
      WiFi.disconnect();
      WiFi.begin(wifi_ssid, wifi_pass);
    }
  }
  if (now_wifi && !was_wifi) {
    DBG.print("[WIFI] Connected, IP: ");
    DBG.println(WiFi.localIP());
    if (!udp_started) udp_started = udp.begin(UDP_LOCAL_PORT);
  }
  if (!now_wifi && was_wifi) {
    DBG.println("[WIFI] Connection lost.");
    tcp_client.stop();
    last_tcp_rx = 0;
    last_udp_rx = 0;
  }
  was_wifi = now_wifi;

  if (wifi_up() && gw_configured && !tcp_client.connected()) {
    static unsigned long last_tcp_attempt = 0;
    if (millis() - last_tcp_attempt > 3000) {
      last_tcp_attempt = millis();
      tcp_assembler.reset();
      if (tcp_client.connect(gateway_ip, tcp_port, 1500)) {
        last_tcp_rx = 0;
        DBG.println("[TCP] Connected to gateway (backup link).");
      }
    }
  }

  if (tcp_client.connected() && active_channel() != CH_TCP) {
    static unsigned long last_keepalive = 0;
    if (millis() - last_keepalive >= TCP_KEEPALIVE_MS) {
      last_keepalive = millis();
      SensorPacket ka = {};
      ka.version = PROTOCOL_VERSION;
      ka.msg_type = MSG_ACK;
      ka.node_id = MY_NODE_ID;
      ka.sequence = 0xFFFFFFFF;
      ka.timestamp_ms = millis();
      uint8_t tx[64];
      int len = protocol_pack(&ka, tx, sizeof(tx));
      if (len > 0) tcp_client.write(tx, len);
    }
  }

  static unsigned long last_probe = 0;
  if (millis() - last_probe >= WIFI_PROBE_MS) {
    last_probe = millis();
    send_wifi_probe();
  }

  poll_downlink();

  sensor_update();

  check_serial_commands();

  ReliableStatus rst = reliable_tick(&reliable, millis());
  if (rst == RELIABLE_SUCCESS) {
    DBG.print("[ALARM] seq=");
    DBG.print(reliable.sequence);
    DBG.print(" delivered, attempts=");
    DBG.println(reliable.attempts);
    LCD_FLASH("ALARM delivered");
    alarm_inflight_valid = false;
  } else if (rst == RELIABLE_EXHAUSTED) {
    DBG.print("[ALARM] seq=");
    DBG.print(reliable.sequence);
    DBG.print(": retries exhausted, NOT delivered, attempts=");
    DBG.print(reliable.attempts);
    DBG.println(" -- requeued, will retry later");
    LCD_FLASH("ALARM FAILED");
    if (alarm_inflight_valid) {
      alarm_requeue_front(&alarm_inflight);
      alarm_inflight_valid = false;
    }
    alarm_retry_after = millis() + ALARM_RETRY_COOLDOWN_MS;
    if (alarm_retry_after == 0) alarm_retry_after = 1;
  }
  alarm_pump();

  static unsigned long last_flush_step = 0;
  if (!pq_empty(&tele_q) && transport_ready() && millis() - last_flush_step >= FLUSH_INTERVAL_MS) {
    last_flush_step = millis();
    flush_buffer_step();
  }

  static unsigned long last_hello = 0;
  if (!id_confirmed && transport_ready() && millis() - last_hello >= 2000) {
    last_hello = millis();
    send_hello();
  }

  static unsigned long last_send = 0;
  if (millis() - last_send >= 5000) {
    last_send = millis();
    if (MY_NODE_ID != 0) send_telemetry();
  }

  static Channel last_channel = CH_NONE;
  Channel ch = active_channel();
  if (ch != last_channel) {
    DBG.print("[LINK] ");
    DBG.print(channel_name(last_channel));
    DBG.print(" -> ");
    DBG.println(channel_name(ch));
    last_channel = ch;
  }

#ifdef NODE_LCD
  lcd_service();
#endif
}
