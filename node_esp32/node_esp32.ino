/*
 * node_esp32.ino -- Блок B: вузол з УСІМА каналами зв'язку одночасно.
 *
 * Плата не обирає транспорт при прошивці. Вона тримає одночасно:
 *   UART (дріт до Pi через USB-TTL) -- ОСНОВНИЙ, поки дріт живий;
 *   TCP  (Wi-Fi, сирий сокет)       -- резервний №1;
 *   UDP  (Wi-Fi, датаграми)         -- резервний №2.
 * Пріоритет: UART > TCP > UDP. Якщо жодного немає -- пакети йдуть у
 * локальний буфер (store-and-forward) і вивантажуються, щойно канал
 * з'явився.
 *
 * Налаштування Wi-Fi плата НЕ питає в людини. Шлюз на Pi, побачивши плату
 * на UART, сам передає їй двома CONFIG-пакетами:
 *   {"cmd":"wifi","s":"<SSID>","p":"<пароль>"}
 *   {"cmd":"gw","ip":"<IP шлюзу>","udp":5005,"tcp":5006}
 * Плата зберігає їх у енергонезалежній пам'яті (NVS), підтверджує кожен
 * пакет через ACK і далі тримає Wi-Fi у фоні. Після перезапуску плата
 * бере збережені налаштування, тож може працювати по Wi-Fi і без дроту.
 * (Пароль лежить у NVS плати відкритим текстом -- для демо це прийнятно.)
 *
 * "Дріт живий" = за останні WIRE_TIMEOUT_MS плата отримала хоча б один
 * валідний кадр від шлюзу по UART (шлюз шле "пінг" щосекунди).
 * Відключили дріт -- за кілька секунд плата сама переходить на Wi-Fi.
 * Втрата/дублікати під час перемикання лишаються видимими в лічильниках
 * шлюзу (sequence наскрізний, один на всі канали).
 *
 * ПОТРІБНА БІБЛІОТЕКА (Arduino IDE -> Tools -> Manage Libraries):
 *   "MPU9250" автора hideakitai.
 *
 * node_id ПЛАТА НЕ ПРОПИСУЄ -- його призначає шлюз за старшинством: хто
 * першим з'явився на шлюзі, той отримує менший id (1, 2, 3...). Плата
 * ідентифікує себе MAC-адресою (HELLO), шлюз повертає id, плата зберігає
 * його в NVS. Реєстр веде шлюз, тож після перезапуску плати id той самий.
 * Поки id не призначено, телеметрія не шлеться (немає від чийого імені).
 *
 * Серво: лише на ОДНІЙ платі, де воно фізично підключене, постав
 * SERVO_ENABLED true (бібліотека "ESP32Servo"); на решті команда servo
 * просто підтверджується й логується.
 *
 * Поруч з .ino мають лежати protocol.h/.c і reliability.h/.c.
 */
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <MPU9250.h>

extern "C" {
  #include "protocol.h"     /* SensorPacket, protocol_pack/unpack */
  #include "reliability.h"  /* ACK/retry для MSG_ALARM */
}

// node_id призначає шлюз (0 = ще не призначено). Зберігається в NVS.
uint16_t MY_NODE_ID = 0;
char device_mac[18] = "";           // унікальна ідентичність плати (eFuse MAC)
bool id_confirmed = false;          // шлюз підтвердив id у цій сесії

// ---- Серво (лише на одній платі в мережі) ----
#define SERVO_ENABLED false
#define SERVO_PIN     18
#if SERVO_ENABLED
  #include <ESP32Servo.h>
  Servo servo;
#endif

// ---- UART до шлюзу ----
// LINK_VIA_USB_CABLE 1: зв'язок зі шлюзом іде прямо через USB-кабель плати
//   (вбудований USB-UART адаптер = UART0 = Serial). Кабель з'єднує плату з
//   Pi, жодних пінів і перехідників. УВАГА: Serial тоді зайнятий каналом, тож
//   текстові логи й команди (alarm/status) у Serial Monitor недоступні
//   (логи гасяться). Для налагодження поставте 0 і підключіть кабель до ПК.
// LINK_VIA_USB_CABLE 0: окремий UART2 на пінах GPIO16/17 (через USB-TTL
//   перехідник до Pi); Serial лишається вільним для логів і команд.
#define LINK_VIA_USB_CABLE 1
#define UART_RX_PIN 16              // лише для режиму 0
#define UART_TX_PIN 17              // лише для режиму 0
#define UART_BAUD   115200          // має збігатися з UART_BAUD у gateway.c

#if LINK_VIA_USB_CABLE
  #define GatewaySerial Serial
  // Приймач логів, що нічого не друкує (інакше текст псував би кадри протоколу)
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

#define WIRE_TIMEOUT_MS   3000      // стільки без кадрів від шлюзу = дріт мертвий
#define UDP_LOCAL_PORT    12345     // з нього плата і шле UDP, і слухає ACK
#define BUFFER_CAPACITY   50
#define TCP_KEEPALIVE_MS  5000      // щоб шлюз не закрив "тихе" резервне TCP

// ---- Налаштування, що приходять від шлюзу (зберігаються в NVS) ----
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

unsigned long last_wire_rx = 0;     // millis() останнього валідного кадру по UART

MPU9250 mpu;
uint32_t seq_counter = 0;           // наскрізний sequence для TELEMETRY і ALARM
SensorPacket buffer[BUFFER_CAPACITY];
int buffer_count = 0;
bool imu_ok = false;
ReliableCtx reliable;
String serial_cmd_buffer = "";

// Складання кадру з байтового потоку (UART/TCP): спершу HEADER_SIZE байт,
// з них payload_len, тоді повний розмір. UDP зберігає межі датаграм сам.
struct FrameAssembler {
  uint8_t buf[HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE];
  size_t have = 0;
  size_t need = HEADER_SIZE;
  bool header_done = false;

  void reset() { have = 0; need = HEADER_SIZE; header_done = false; }

  bool feed(uint8_t b) {
    if (have >= sizeof(buf)) reset();
    buf[have++] = b;
    if (!header_done && have == HEADER_SIZE) {
      uint16_t payload_len;
      memcpy(&payload_len, buf + 16, 2);
      need = HEADER_SIZE + payload_len + CRC_SIZE;
      header_done = true;
      if (need > sizeof(buf)) { reset(); return false; }
    }
    return header_done && have == need;
  }
};
FrameAssembler uart_assembler;
FrameAssembler tcp_assembler;

// ================== КАНАЛИ ==================
enum Channel { CH_NONE, CH_UART, CH_TCP, CH_UDP };

bool wire_alive() {
  return last_wire_rx != 0 && (millis() - last_wire_rx) < WIRE_TIMEOUT_MS;
}
bool wifi_up() { return wifi_configured && WiFi.status() == WL_CONNECTED; }
bool tcp_up()  { return wifi_up() && gw_configured && tcp_client.connected(); }
bool udp_up()  { return wifi_up() && gw_configured && udp_started; }

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
    default:      return "немає каналу";
  }
}

bool udp_send(const uint8_t *buf, int len) {
  if (!udp_up()) return false;
  udp.beginPacket(gateway_ip, udp_port);
  udp.write(buf, len);
  return udp.endPacket() == 1;
}

// Єдине місце, де транспорт торкається протокольного коду. Обирає
// найкращий доступний канал; якщо TCP-запис не вдався -- пробує UDP.
bool node_send(const uint8_t *buf, int len) {
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

// Сигнатура, якої вимагає reliability.c (і перша спроба, і кожен retry).
void esp32_reliable_send(void *userdata, const uint8_t *buf, int len) {
  (void)userdata;
  node_send(buf, len);
}

// ================== ПРИЙОМ НАЛАШТУВАНЬ ВІД ШЛЮЗУ ==================
// Мінімальний розбір JSON для наших двох CONFIG-пакетів. Знаходить
// "key":"значення" з екрануванням \" \\ \/ і \u00XX (ASCII).
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
    p += 1; // це було значення, а не ключ -- шукаємо далі
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
        c = e; // \" \\ \/
      }
    }
    if (o + 1 < n) out[o++] = c;
  }
  out[o] = '\0';
  return *v == '"';
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
  DBG.print("[WIFI] Підключаюсь до \"");
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
    DBG.println("[CONFIG] Отримано нові налаштування Wi-Fi від шлюзу, збережено.");
    wifi_start();
  }
}

void apply_gw_config(const char *ip, long udp_p, long tcp_p) {
  IPAddress parsed;
  if (!parsed.fromString(ip)) {
    DBG.println("[CONFIG] Шлюз надіслав некоректний IP, ігнорую.");
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
    tcp_client.stop(); // перепідключиться вже за новою адресою
    DBG.print("[CONFIG] Адреса шлюзу від Pi: ");
    DBG.print(gw_ip_str);
    DBG.print(" (UDP ");
    DBG.print(udp_port);
    DBG.print(", TCP ");
    DBG.print(tcp_port);
    DBG.println("), збережено.");
  }
}

// Підтвердження CONFIG-пакета шлюзу: ACK з тим самим sequence.
void send_ack_to_gateway(uint32_t seq) {
  SensorPacket ack = {0};
  ack.version = PROTOCOL_VERSION;
  ack.msg_type = MSG_ACK;
  ack.node_id = MY_NODE_ID;
  ack.sequence = seq;
  ack.timestamp_ms = millis();
  uint8_t tx[64];
  int len = protocol_pack(&ack, tx, sizeof(tx));
  if (len > 0) node_send(tx, len);
}

// Запит id у шлюзу: HEARTBEAT з node_id=0 і нашим MAC (+ збережений id, якщо був).
void send_hello() {
  SensorPacket h = {0};
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

void handle_config_packet(const SensorPacket *pkt) {
  char js[MAX_PAYLOAD_SIZE + 1];
  memcpy(js, pkt->payload, pkt->payload_len);
  js[pkt->payload_len] = '\0';

  char cmd[16];
  if (!json_get_string(js, "cmd", cmd, sizeof(cmd))) {
    DBG.println("[CONFIG] CONFIG без поля cmd, ігнорую.");
    return;
  }
  if (strcmp(cmd, "id") == 0) {
    char mac[18];
    long id = 0;
    if (!json_get_string(js, "mac", mac, sizeof(mac)) || strcmp(mac, device_mac) != 0) return; // не нам
    if (!json_get_int(js, "id", &id) || id <= 0 || id > 65535) return;
    if (MY_NODE_ID != (uint16_t)id) {
      DBG.print("[ID] Шлюз призначив node_id=");
      DBG.print(id);
      DBG.print(MY_NODE_ID ? " (було " : " (нова плата");
      if (MY_NODE_ID) { DBG.print(MY_NODE_ID); DBG.print(", переписано)"); } else DBG.print(")");
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
    DBG.print("[SERVO] Кут: ");
    DBG.println(angle);
#else
    DBG.print("[SERVO] Команда отримана (angle=");
    DBG.print(angle);
    DBG.println("), але SERVO_ENABLED=false на цій платі");
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
  } else {
    // servo / simulate_loss з веб-інтерфейсу: на платі ще не реалізовано
    DBG.print("[CONFIG] Невідома команда: ");
    DBG.println(cmd);
  }
}

// ================== ПРИЙОМ ВІД ШЛЮЗУ ==================
void handle_downlink_bytes(const uint8_t *raw, size_t len, bool from_wire) {
  SensorPacket pkt;
  int rc = protocol_unpack(raw, len, &pkt);
  if (rc != PROTO_OK) {
    DBG.print("[WARN] Пошкоджений кадр від шлюзу, код=");
    DBG.println(rc);
    return;
  }
  if (from_wire) last_wire_rx = millis(); // будь-який валідний кадр = дріт живий

  if (pkt.node_id != 0 && pkt.node_id != MY_NODE_ID) return; // чужий вузол; 0 = усім

  if (pkt.msg_type == MSG_ACK) {
    DBG.print("[ACK] Підтвердження sequence=");
    DBG.println(pkt.sequence);
    reliable_on_ack_received(&reliable, pkt.sequence);
  } else if (pkt.msg_type == MSG_CONFIG) {
    handle_config_packet(&pkt);
  }
  // MSG_HEARTBEAT від шлюзу ("пінг" дроту) -- потрібен лише заради last_wire_rx
}

void poll_downlink() {
  while (GatewaySerial.available()) {
    if (uart_assembler.feed((uint8_t)GatewaySerial.read())) {
      handle_downlink_bytes(uart_assembler.buf, uart_assembler.have, true);
      uart_assembler.reset();
    }
  }
  if (udp_started) {
    int packet_size = udp.parsePacket();
    if (packet_size > 0) {
      uint8_t buf[HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE];
      int len = udp.read(buf, sizeof(buf));
      if (len > 0) handle_downlink_bytes(buf, (size_t)len, false);
    }
  }
  if (tcp_client.connected()) {
    while (tcp_client.available()) {
      if (tcp_assembler.feed((uint8_t)tcp_client.read())) {
        handle_downlink_bytes(tcp_assembler.buf, tcp_assembler.have, false);
        tcp_assembler.reset();
      }
    }
  }
}

// ================== БУФЕР STORE-AND-FORWARD ==================
void buffer_packet(const SensorPacket* pkt) {
  if (buffer_count < BUFFER_CAPACITY) {
    buffer[buffer_count++] = *pkt;
  } else {
    for (int i = 1; i < BUFFER_CAPACITY; i++) buffer[i - 1] = buffer[i];
    buffer[BUFFER_CAPACITY - 1] = *pkt;
  }
  DBG.print("[BUFFER] Немає каналу -- пакет збережено локально. У буфері: ");
  DBG.println(buffer_count);
}

void flush_buffer() {
  if (buffer_count == 0) return;
  DBG.print("[BUFFER] Канал є (");
  DBG.print(channel_name(active_channel()));
  DBG.print("). Вивантажую накопичені пакети: ");
  DBG.println(buffer_count);

  int sent = 0;
  for (int i = 0; i < buffer_count; i++) {
    uint8_t tx_buf[256];
    int packed_len = protocol_pack(&buffer[i], tx_buf, sizeof(tx_buf));
    if (packed_len > 0) {
      if (node_send(tx_buf, packed_len)) {
        sent++;
        delay(50); // пауза між пакетами для стабільності приймача
      } else {
        DBG.println("[BUFFER] Помилка відправки, перериваю вивантаження.");
        break;
      }
    }
  }
  if (sent < buffer_count) {
    int remaining = buffer_count - sent;
    for (int i = 0; i < remaining; i++) buffer[i] = buffer[sent + i];
    buffer_count = remaining;
  } else {
    buffer_count = 0;
  }
  DBG.print("[BUFFER] Вивантаження завершено. Залишок: ");
  DBG.println(buffer_count);
}

// ================== ТЕЛЕМЕТРІЯ І ALARM ==================
void send_telemetry() {
  SensorPacket pkt = {0};
  pkt.version = PROTOCOL_VERSION;
  pkt.msg_type = MSG_TELEMETRY;
  pkt.node_id = MY_NODE_ID;
  pkt.sequence = seq_counter++;
  pkt.timestamp_ms = millis();

  snprintf((char*)pkt.payload, MAX_PAYLOAD_SIZE,
           "{\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
           mpu.getRoll(), mpu.getPitch(), mpu.getYaw());
  pkt.payload_len = strlen((char*)pkt.payload);

  if (!transport_ready()) {
    buffer_packet(&pkt);
    return;
  }

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
      DBG.println(" байт)");
    } else {
      buffer_packet(&pkt);
    }
  }
}

void send_alarm() {
  if (MY_NODE_ID == 0) {
    DBG.println("[ALARM] node_id ще не призначено шлюзом.");
    return;
  }
  if (!transport_ready()) {
    DBG.println("[ALARM] Немає каналу -- ALARM не відправлено.");
    return;
  }
  if (reliable_is_busy(&reliable)) {
    DBG.println("[ALARM] Попередня критична відправка ще активна, зачекай.");
    return;
  }

  SensorPacket pkt = {0};
  pkt.version = PROTOCOL_VERSION;
  pkt.msg_type = MSG_ALARM;
  pkt.node_id = MY_NODE_ID;
  pkt.sequence = seq_counter++;
  pkt.timestamp_ms = millis();

  const char* json = "{\"alarm\":\"critical_event\"}";
  pkt.payload_len = strlen(json);
  memcpy(pkt.payload, json, pkt.payload_len);

  DBG.print("[ALARM] Ініціюю надійну відправку sequence=");
  DBG.println(pkt.sequence);
  reliable_send_critical(&reliable, &pkt, millis());
}

void print_status() {
  DBG.print("[STATUS] node_id=");
  DBG.print(MY_NODE_ID);
  DBG.print(id_confirmed ? " (підтверджено)" : " (не підтверджено)");
  DBG.print(" | канал=");
  DBG.print(channel_name(active_channel()));
  DBG.print(" | дріт=");
  DBG.print(wire_alive() ? "живий" : "немає");
  DBG.print(" | Wi-Fi=");
  DBG.print(wifi_configured ? (WiFi.status() == WL_CONNECTED ? "підключено" : "підключаюсь") : "не налаштовано");
  DBG.print(" | шлюз=");
  DBG.print(gw_configured ? gw_ip_str : "не налаштовано");
  DBG.print(" | TCP=");
  DBG.print(tcp_client.connected() ? "так" : "ні");
  DBG.print(" | буфер=");
  DBG.println(buffer_count);
}

// Команди з Serial Monitor: "alarm" -- критична подія з ACK/retry,
// "status" -- поточний стан каналів, "forget" -- стерти збережені налаштування.
void check_serial_commands() {
#if LINK_VIA_USB_CABLE
  return; // Serial зайнятий каналом зі шлюзом -- команди з консолі недоступні
#endif
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
          DBG.println("[CONFIG] Налаштування стерто. Перезавантаж плату і підключи дріт до Pi.");
        } else {
          DBG.print("[SERIAL] Невідома команда: ");
          DBG.println(serial_cmd_buffer);
        }
        serial_cmd_buffer = "";
      }
    } else {
      serial_cmd_buffer += c;
    }
  }
}

// ================== SETUP / LOOP ==================
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

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setTimeOut(1000);      // без цього завислий I2C блокує весь loop()
  delay(2000);

  DBG.println("\nІніціалізація IMU MPU9250...");
  imu_ok = mpu.setup(0x68);
  DBG.println(imu_ok ? "[OK] IMU успішно підключено."
                        : "[ПОМИЛКА] IMU не знайдено! Телеметрія піде з нульовими roll/pitch/yaw.");

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
    DBG.println("[CONFIG] Знайдено збережені налаштування -- Wi-Fi стартує у фоні.");
    wifi_start();
  } else {
    DBG.println("[CONFIG] Налаштувань Wi-Fi ще немає: підключи дріт до Pi (UART), шлюз передасть їх сам.");
  }

  reliable_init(&reliable, esp32_reliable_send, NULL);

  DBG.print("Готово. MAC=");
  DBG.print(device_mac);
  DBG.print(", node_id=");
  DBG.print(MY_NODE_ID ? String(MY_NODE_ID) : String("очікую від шлюзу"));
  DBG.println(". Команди в Serial Monitor: alarm, status, forget.");
}

void loop() {
  // --- Wi-Fi у фоні (неблокуюче) ---
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
    DBG.print("[WIFI] Підключено, IP плати: ");
    DBG.println(WiFi.localIP());
    if (!udp_started) udp_started = udp.begin(UDP_LOCAL_PORT);
  }
  if (!now_wifi && was_wifi) {
    DBG.println("[WIFI] Зв'язок втрачено.");
    tcp_client.stop();
  }
  was_wifi = now_wifi;

  // --- TCP-з'єднання з шлюзом (резервний канал) ---
  if (wifi_up() && gw_configured && !tcp_client.connected()) {
    static unsigned long last_tcp_attempt = 0;
    if (millis() - last_tcp_attempt > 3000) {
      last_tcp_attempt = millis();
      tcp_assembler.reset();
      if (tcp_client.connect(gateway_ip, tcp_port, 1500)) {
        DBG.println("[TCP] Підключено до шлюзу (резервний канал).");
      }
    }
  }

  // Поки TCP не основний, шлемо по ньому службовий ACK з невикористаним
  // sequence -- шлюз бачить трафік і не закриває "тихе" з'єднання.
  if (tcp_client.connected() && active_channel() != CH_TCP) {
    static unsigned long last_keepalive = 0;
    if (millis() - last_keepalive >= TCP_KEEPALIVE_MS) {
      last_keepalive = millis();
      SensorPacket ka = {0};
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

  poll_downlink();

  if (imu_ok) mpu.update();

  check_serial_commands();

  ReliableStatus rst = reliable_tick(&reliable, millis());
  if (rst == RELIABLE_SUCCESS) {
    DBG.print("[ALARM] seq=");
    DBG.print(reliable.sequence);
    DBG.print(" ДОСТАВЛЕНО, спроб=");
    DBG.println(reliable.attempts);
  } else if (rst == RELIABLE_EXHAUSTED) {
    DBG.print("[ALARM] seq=");
    DBG.print(reliable.sequence);
    DBG.print(": RETRY ВИЧЕРПАНО, НЕ доставлено, спроб=");
    DBG.println(reliable.attempts);
  }

  // Щойно з'явився будь-який канал -- вивантажуємо накопичене (раз на секунду)
  static unsigned long last_flush_check = 0;
  if (buffer_count > 0 && transport_ready() && millis() - last_flush_check > 1000) {
    last_flush_check = millis();
    flush_buffer();
  }

  // Поки шлюз не підтвердив id, просимо його (раз на 2 с, коли є канал)
  static unsigned long last_hello = 0;
  if (!id_confirmed && transport_ready() && millis() - last_hello >= 2000) {
    last_hello = millis();
    send_hello();
  }

  // Телеметрія кожні 5 секунд (лише коли є node_id)
  static unsigned long last_send = 0;
  if (millis() - last_send >= 5000) {
    last_send = millis();
    if (MY_NODE_ID != 0) send_telemetry();
  }

  // Перемикання каналу -- видно в Serial
  static Channel last_channel = CH_NONE;
  Channel ch = active_channel();
  if (ch != last_channel) {
    DBG.print("[КАНАЛ] ");
    DBG.print(channel_name(last_channel));
    DBG.print(" -> ");
    DBG.println(channel_name(ch));
    last_channel = ch;
  }
}
