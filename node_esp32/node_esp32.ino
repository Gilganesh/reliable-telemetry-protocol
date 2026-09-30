/*
 * node_esp32.ino -- Блок B, Рівні 1-3: WiFi+MQTT bring-up, реальний
 * протокол (Блок A), буфер store-and-forward.
 *
 * ПОВНИЙ, заповнений скетч -- нічого дороблювати, окрім 4 налаштувань
 * нижче (WiFi, IP брокера, свій node_id).
 *
 * ЧЕСНО: цей файл НЕ скомпільований і НЕ перевірений на реальному ESP32
 * (в середовищі, де я працюю, немає ESP32/Arduino-тулчейну -- лише C-код
 * протоколу я можу компілювати й тестувати напряму). Логіка звірена
 * вручну з документацією PubSubClient і ESP32 WiFi API, але перший
 * реальний запуск на платі -- це і є твоя перевірка. Якщо щось не
 * компілюється -- скинь мені точний текст помилки, розберемо разом.
 *
 * Перед прошивкою в Arduino IDE:
 *   1. File -> Preferences -> Additional Board Manager URLs, додати:
 *      https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
 *   2. Tools -> Board -> Boards Manager, знайти "esp32" (Espressif
 *      Systems), встановити.
 *   3. Tools -> Manage Libraries, знайти "PubSubClient" (автор:
 *      Nick O'Leary), встановити.
 *   4. Створити папку зі скетчем з таким же іменем, як цей файл:
 *      node_esp32/node_esp32.ino -- і покласти в ЦЮ ЖЕ папку файли
 *      protocol.h і protocol.c (скопіювати з protocol/ у репозиторії).
 *      Arduino IDE компілює всі .h/.c файли в папці скетчу разом з .ino.
 */
#include <WiFi.h>
#include <PubSubClient.h>
extern "C" {
  #include "protocol.h"   /* Блок A: SensorPacket, protocol_pack/unpack */
}

/* ==================== НАЛАШТУВАННЯ -- зміни тут ==================== */
const char* ssid       = "ІМ'Я_ВАШОГО_WIFI";
const char* password   = "ПАРОЛЬ_ВАШОГО_WIFI";
const char* mqtt_server = "192.168.1.X";  /* IP ноутбука Блоку C (Gateway) */
uint16_t MY_NODE_ID = 1;                  /* 1 для першої плати, 2 для другої */
/* ==================================================================== */

#define BUFFER_CAPACITY 20

WiFiClient espClient;
PubSubClient client(espClient);
uint32_t seq_counter = 0;

/* Кільцевий буфер для store-and-forward (критерій приймання №4):
 * якщо MQTT недоступний -- пакет не втрачається, а зберігається тут,
 * і вивантажується одразу після відновлення зв'язку. */
SensorPacket buffer[BUFFER_CAPACITY];
int buffer_count = 0;

void setup_wifi() {
  Serial.print("Підключення до WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi підключено, IP плати: ");
  Serial.println(WiFi.localIP());
}

/* Викликається бібліотекою автоматично, коли приходить повідомлення
 * з топіка, на який ми підписані (downlink -- ACK від Gateway). */
void on_mqtt_message(char* topic, uint8_t* payload, unsigned int length) {
  SensorPacket pkt;
  int rc = protocol_unpack(payload, length, &pkt);
  if (rc != PROTO_OK) {
    Serial.print("[WARN] Пошкоджений downlink-пакет, код=");
    Serial.println(rc);
    return;
  }
  if (pkt.msg_type == MSG_ACK) {
    Serial.print("[ACK] Підтверджено sequence=");
    Serial.println(pkt.sequence);
    /* Рівень 4 (ACK/retry, після цього рівня): тут треба буде
     * встановити прапорець "ack прийшов для цього sequence", який
     * читає функція send_critical() з ack-retry-algorithm.md. Поки
     * (Рівні 1-3) просто друкуємо -- retry ще не реалізовано. */
  }
}

void flush_buffer() {
  if (buffer_count == 0) return;
  Serial.print("[BUFFER] Вивантажую накопичені пакети: ");
  Serial.println(buffer_count);
  for (int i = 0; i < buffer_count; i++) {
    uint8_t tx_buf[256];
    int packed_len = protocol_pack(&buffer[i], tx_buf, sizeof(tx_buf));
    if (packed_len > 0) {
      client.publish("case24/uplink", tx_buf, packed_len);
    }
  }
  buffer_count = 0;
}

void reconnect_mqtt() {
  while (!client.connected()) {
    Serial.print("Підключення до MQTT-брокера...");
    String client_id = "esp32-node-" + String(MY_NODE_ID);
    if (client.connect(client_id.c_str())) {
      Serial.println(" підключено!");
      char sub_topic[64];
      snprintf(sub_topic, sizeof(sub_topic), "case24/downlink/%u", MY_NODE_ID);
      client.subscribe(sub_topic);
      flush_buffer();   /* щойно з'явився зв'язок -- вивантажуємо буфер */
    } else {
      Serial.print(" не вдалося, код=");
      Serial.print(client.state());
      Serial.println(" -- повторна спроба через 2с");
      delay(2000);
    }
  }
}

void buffer_packet(const SensorPacket* pkt) {
  if (buffer_count < BUFFER_CAPACITY) {
    buffer[buffer_count++] = *pkt;
  } else {
    /* Буфер переповнено -- викидаємо найстаріший, зсуваючи масив.
     * Для 20 слотів і телеметрії кожні 5с це ~100с запасу. */
    for (int i = 1; i < BUFFER_CAPACITY; i++) buffer[i - 1] = buffer[i];
    buffer[BUFFER_CAPACITY - 1] = *pkt;
  }
  Serial.print("[BUFFER] Немає зв'язку -- пакет збережено локально. У буфері: ");
  Serial.println(buffer_count);
}

void send_telemetry() {
  SensorPacket pkt = {0};
  pkt.version = PROTOCOL_VERSION;
  pkt.msg_type = MSG_TELEMETRY;
  pkt.node_id = MY_NODE_ID;
  pkt.sequence = seq_counter++;
  pkt.timestamp_ms = millis();

  /* TODO (не зараз, пізніше): тут буде читання РЕАЛЬНОГО датчика.
   * Зараз -- заглушкові дані, щоб перевірити канал end-to-end. */
  snprintf((char*)pkt.payload, MAX_PAYLOAD_SIZE, "{\"t\":%.1f,\"h\":%.1f}", 23.5, 55.0);
  pkt.payload_len = strlen((char*)pkt.payload);

  if (!client.connected()) {
    buffer_packet(&pkt);
    return;
  }

  uint8_t tx_buf[256];
  int packed_len = protocol_pack(&pkt, tx_buf, sizeof(tx_buf));
  if (packed_len > 0) {
    client.publish("case24/uplink", tx_buf, packed_len);
    Serial.print("[TX] seq=");
    Serial.print(pkt.sequence);
    Serial.print(" -> case24/uplink (");
    Serial.print(packed_len);
    Serial.println(" байт)");
  }
}

void setup() {
  Serial.begin(115200);
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(on_mqtt_message);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
  }
  if (!client.connected()) {
    reconnect_mqtt();
  }
  client.loop();   /* ОБОВ'ЯЗКОВО викликати щоразу -- інакше вхідні ACK не обробляються */

  static unsigned long last_send = 0;
  if (millis() - last_send > 5000) {
    last_send = millis();
    send_telemetry();
  }
}
