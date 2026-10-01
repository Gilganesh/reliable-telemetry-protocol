/*
 * node_esp32.ino -- Блок B, Рівні 1-4 (ЗЛИТА версія, 30.09)
 *
 * Ця версія об'єднує дві незалежні гілки розвитку, які команда
 * принесла паралельно:
 *   1. "node_esp32_buffer" -- виправлення РЕАЛЬНОГО бага: старий
 *      reconnect_mqtt()/setup_wifi() були БЛОКУЮЧИМИ (while-цикл з
 *      delay всередині), тому під час реального обриву зв'язку весь
 *      loop() зависав усередині них і send_telemetry() просто НЕ
 *      викликався -- буфер (Критерій приймання №4) міг не наповнюватись
 *      так, як очікується під час демо. Тепер перепідключення до WiFi
 *      і транспорту -- НЕБЛОКУЮЧЕ (перевірка раз на кілька секунд через
 *      millis(), без delay/while), loop() продовжує працювати і
 *      телеметрія продовжує генеруватись (і буферизуватись) навіть
 *      поки зв'язку немає.
 *   2. "Node_esp32_IMU" -- реальний сенсор MPU9250 (10-DOF IMU,
 *      бібліотека "MPU9250" авторства hideakitai) замість фейкових
 *      температури/вологості: payload телеметрії тепер реальні
 *      roll/pitch/yaw з фізичного давача.
 *
 * Обидві гілки лишали ACK/retry (reliability.c, Блок A) -- лишено.
 *
 * 30.09 (2) -- ФІКС ЗАВИСАННЯ ПЛАТИ (Вузол 1 зависав повністю, без
 * жодного виводу в Serial, після тривалого офлайну):
 *   1. Wire.setTimeOut(1000) -- без цього зависла I2C-шина (просідання
 *      живлення IMU під час передачі Wi-Fi, слабкий контакт SDA/SCL)
 *      блокує mpu.update() НАЗАВЖДИ, а з ним і весь loop(), і Serial.
 *   2. imu_ok -- mpu.update() більше не викликається, якщо IMU не
 *      відповіла при старті.
 *   3. MQTT client_id тепер char[], а не String -- у неблокуючому
 *      reconnect-циклі (кожні 2с, поки офлайн) повторне String-
 *      конкатенування довго міг фрагментувати heap.
 *
 * 30.09 (3) -- ТРИ РІЗНІ ТРАНСПОРТИ, ОДИН ПРОТОКОЛ. Кейс вимагає, щоб
 * архітектура дозволяла замінити транспортний рівень (UDP/TCP/UART/
 * імітований канал) без переписування логіки протоколу. Замість трьох
 * окремих .ino-файлів, які з часом розійдуться -- ОДИН файл, і перед
 * прошивкою кожної плати змінюється лише NODE_TRANSPORT нижче (так само,
 * як MY_NODE_ID). protocol.c/reliability.c/буфер/IMU -- спільні для всіх
 * трьох режимів, не знають і не питають, яким дротом підуть байти.
 *
 *   NODE_TRANSPORT_UDP  -- WiFiUDP, датаграми напряму на IP шлюзу:UDP_PORT.
 *   NODE_TRANSPORT_TCP  -- WiFiClient (сирий TCP-сокет, БЕЗ MQTT-обгортки),
 *                          з'єднання тримається, як раніше тримався MQTT.
 *   NODE_TRANSPORT_UART -- пряма дротова лінія (апаратний UART2, НЕ той
 *                          самий Serial, що йде в USB/Serial Monitor) до
 *                          USB-TTL перехідника, підключеного до Pi. Ця
 *                          плата НЕ використовує WiFi взагалі -- фізично
 *                          прив'язана дротом до шлюзу, тому мусить стояти
 *                          поруч з Pi на демо.
 *
 * ПОТРІБНА БІБЛІОТЕКА (Arduino IDE -> Tools -> Manage Libraries):
 *   "MPU9250" автора hideakitai (шукати саме цю назву автора --
 *   є кілька бібліотек зі схожою назвою для інших давачів).
 *
 * ВАЖЛИВО ПЕРЕД ПРОШИВКОЮ КОЖНОЇ ПЛАТИ:
 *   1. Постав правильний MY_NODE_ID нижче (кожен фізичний вузол мережі
 *      МАЄ мати унікальний ID).
 *   2. Постав правильний NODE_TRANSPORT нижче (UDP / TCP / UART --
 *      відповідно до того, яким каналом ЦЯ плата йде на демо).
 *
 * ВАЖЛИВО перед прошивкою: поклади поруч із цим .ino файлом (в ту саму
 * папку) protocol.h, protocol.c, reliability.h, reliability.c.
 */
#include <Wire.h>
#include "MPU9250.h"

extern "C" {
  #include "protocol.h"     /* Блок A: SensorPacket, protocol_pack/unpack */
  #include "reliability.h"  /* Блок A: ACK/retry для MSG_ALARM/MSG_CONFIG */
}

// ==== ВИБІР ТРАНСПОРТУ (постав перед прошивкою КОЖНОЇ плати) ====
#define NODE_TRANSPORT_UDP  1
#define NODE_TRANSPORT_TCP  2
#define NODE_TRANSPORT_UART 3
#define NODE_TRANSPORT NODE_TRANSPORT_UDP   // <-- ЗМІНИ ТУТ: UDP / TCP / UART

// !!! ЗМІНИТИ ПЕРЕД ПРОШИВКОЮ КОЖНОЇ ПЛАТИ -- унікальний ID вузла !!!
uint16_t MY_NODE_ID     = 1;

#if NODE_TRANSPORT == NODE_TRANSPORT_UDP
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #define GATEWAY_PORT 5005          // має збігатися з UDP_PORT у gateway.c
  #define UDP_LOCAL_PORT 12345       // порт, з якого плата і шле, і слухає ACK
  WiFiUDP udp;
#elif NODE_TRANSPORT == NODE_TRANSPORT_TCP
  #include <WiFi.h>
  #define GATEWAY_PORT 5006          // має збігатися з TCP_PORT у gateway.c
  WiFiClient tcp_client;
#elif NODE_TRANSPORT == NODE_TRANSPORT_UART
  // Апаратний UART2 -- ОКРЕМИЙ від Serial (USB/Serial Monitor), тому
  // консоль для команди "alarm" і для діагностики лишається вільною.
  // GPIO16/17 -- дефолтні піни UART2 на більшості ESP32 DevKit-плат; якщо
  // у твоєї плати вони зайняті під щось інше -- зміни тут.
  #define UART_RX_PIN 16
  #define UART_TX_PIN 17
  #define UART_BAUD   115200         // має збігатися з UART_BAUD у gateway.c
  HardwareSerial GatewaySerial(2);
#endif

#if NODE_TRANSPORT != NODE_TRANSPORT_UART
// --- НАЛАШТУВАННЯ WiFi ТА АДРЕСИ ШЛЮЗУ (питаються через Serial при старті) ---
char wifi_ssid[64]      = "";
char wifi_password[64]  = "";
char gateway_host[16]   = ""; // IP Raspberry Pi (той самий шлюз, що і для MQTT)
IPAddress gateway_ip;
#endif

#define BUFFER_CAPACITY 50

MPU9250 mpu;                // Об'єкт IMU (бібліотека hideakitai)
uint32_t seq_counter = 0;   // наскрізний sequence -- спільний для TELEMETRY і ALARM

SensorPacket buffer[BUFFER_CAPACITY];
int buffer_count = 0;

// true, якщо IMU відповіла при старті. НЕ гарантує, що шина I2C не
// "зависне" пізніше (просідання живлення, слабкий контакт) -- від цього
// захищає Wire.setTimeOut() нижче, а цей прапорець лише не дає читати
// сенсор, якого не було виявлено взагалі.
bool imu_ok = false;

// --- Надійна доставка критичних повідомлень (Блок A, reliability.c) ---
ReliableCtx reliable;

// Складання кадру з байтового потоку (TCP/UART) -- той самий підхід, що
// і FrameReader у gateway.c: спершу HEADER_SIZE байт, дістати з них
// payload_len, тоді знати повний розмір кадру. UDP цього не потребує --
// WiFiUDP і так зберігає межі датаграми (один parsePacket() = один пакет).
#if NODE_TRANSPORT == NODE_TRANSPORT_TCP || NODE_TRANSPORT == NODE_TRANSPORT_UART
struct FrameAssembler {
  uint8_t buf[HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE];
  size_t have = 0;
  size_t need = HEADER_SIZE;
  bool header_done = false;

  void reset() { have = 0; need = HEADER_SIZE; header_done = false; }

  // true, якщо ПІСЛЯ цього байта кадр у buf[0..have) повний.
  bool feed(uint8_t b) {
    if (have >= sizeof(buf)) reset();
    buf[have++] = b;
    if (!header_done && have == HEADER_SIZE) {
      uint16_t payload_len;
      memcpy(&payload_len, buf + 16, 2); // offset payload_len у заголовку
      need = HEADER_SIZE + payload_len + CRC_SIZE;
      header_done = true;
      if (need > sizeof(buf)) { reset(); return false; }
    }
    return header_done && have == need;
  }
};
FrameAssembler downlink_assembler;
#endif

// Обгортка над реальною відправкою -- єдине місце, де transport-логіка
// торкається протокольного коду (esp32_reliable_send, send_telemetry,
// flush_buffer). Повертає true, якщо байти пішли (best-effort для
// UDP/UART -- вони не підтверджують доставку на цьому рівні; для TCP --
// чи вдалось записати в сокет).
bool node_send(const uint8_t *buf, int len) {
#if NODE_TRANSPORT == NODE_TRANSPORT_UDP
  if (WiFi.status() != WL_CONNECTED) return false;
  udp.beginPacket(gateway_ip, GATEWAY_PORT);
  udp.write(buf, len);
  return udp.endPacket() == 1;
#elif NODE_TRANSPORT == NODE_TRANSPORT_TCP
  if (!tcp_client.connected()) return false;
  return tcp_client.write(buf, len) == (size_t)len;
#elif NODE_TRANSPORT == NODE_TRANSPORT_UART
  GatewaySerial.write(buf, len);
  return true; // пряма дротова лінія -- з погляду плати завжди "готова"
#endif
}

// Чи готовий транспорт до відправки ПРЯМО ЗАРАЗ (від цього залежить,
// чи йде пакет одразу, чи в локальний буфер -- Критерій приймання №4).
bool transport_ready() {
#if NODE_TRANSPORT == NODE_TRANSPORT_UDP
  return WiFi.status() == WL_CONNECTED;
#elif NODE_TRANSPORT == NODE_TRANSPORT_TCP
  return WiFi.status() == WL_CONNECTED && tcp_client.connected();
#elif NODE_TRANSPORT == NODE_TRANSPORT_UART
  return true; // дріт або є, або плата взагалі не отримує живлення
#endif
}

// Обгортка над відправкою з сигнатурою, якої вимагає reliability.c.
// Викликається і для першої спроби, і для кожного retry (ті самі байти).
void esp32_reliable_send(void *userdata, const uint8_t *buf, int len) {
  (void)userdata;
  node_send(buf, len);
}

// Буфер для нечутливого до блокувань читання команд з Serial Monitor.
String serial_cmd_buffer = "";

#if NODE_TRANSPORT != NODE_TRANSPORT_UART
void setup_wifi() {
  Serial.println("\n=================================");
  Serial.print("Підключення до WiFi: ");
  Serial.println(wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);

  // Обмежена за часом спроба лише тут, при старті (щоб не сидіти
  // вічно, якщо мережа недоступна на старті) -- подальші спроби вже
  // неблокуючі, дивись loop().
  unsigned long start_attempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start_attempt < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi підключено, IP плати: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Не вдалося підключитися до Wi-Fi під час старту, продовжимо у фоновому режимі.");
  }
}
#endif

// Спільна обробка вхідного (downlink) кадру -- незалежно від того, UDP
// це, TCP чи UART. MSG_ACK -- підтвердження критичної відправки;
// MSG_CONFIG (керування з веб-інтерфейсу) -- ще НЕ оброблюється.
void handle_downlink_bytes(const uint8_t *raw, size_t len) {
  SensorPacket pkt;
  int rc = protocol_unpack(raw, len, &pkt);
  if (rc != PROTO_OK) {
    Serial.print("[WARN] Пошкоджений downlink-пакет, код=");
    Serial.println(rc);
    return;
  }
  if (pkt.msg_type == MSG_ACK) {
    Serial.print("[ACK] Отримано підтвердження sequence=");
    Serial.println(pkt.sequence);
    reliable_on_ack_received(&reliable, pkt.sequence);
  }
  // MSG_CONFIG -- коли підключимо реальну симуляцію втрат/серво на платі,
  // тут з'явиться гілка "if (pkt.msg_type == MSG_CONFIG) { ... }".
}

// Перевіряє, чи прийшло щось на downlink (ACK/CONFIG), і передає в
// handle_downlink_bytes(). Викликається щоразу в loop().
void poll_downlink() {
#if NODE_TRANSPORT == NODE_TRANSPORT_UDP
  int packet_size = udp.parsePacket();
  if (packet_size > 0) {
    uint8_t buf[HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE];
    int len = udp.read(buf, sizeof(buf));
    if (len > 0) handle_downlink_bytes(buf, (size_t)len);
  }
#elif NODE_TRANSPORT == NODE_TRANSPORT_TCP
  while (tcp_client.available()) {
    if (downlink_assembler.feed((uint8_t)tcp_client.read())) {
      handle_downlink_bytes(downlink_assembler.buf, downlink_assembler.have);
      downlink_assembler.reset();
    }
  }
#elif NODE_TRANSPORT == NODE_TRANSPORT_UART
  while (GatewaySerial.available()) {
    if (downlink_assembler.feed((uint8_t)GatewaySerial.read())) {
      handle_downlink_bytes(downlink_assembler.buf, downlink_assembler.have);
      downlink_assembler.reset();
    }
  }
#endif
}

void buffer_packet(const SensorPacket* pkt) {
  if (buffer_count < BUFFER_CAPACITY) {
    buffer[buffer_count++] = *pkt;
  } else {
    for (int i = 1; i < BUFFER_CAPACITY; i++) {
      buffer[i - 1] = buffer[i];
    }
    buffer[BUFFER_CAPACITY - 1] = *pkt;
  }
  Serial.print("[BUFFER] Немає зв'язку -- пакет збережено локально. У буфері: ");
  Serial.println(buffer_count);
}

void flush_buffer() {
  if (buffer_count == 0) return;
  Serial.print("[BUFFER] Зв'язок відновлено! Вивантажую накопичені пакети: ");
  Serial.println(buffer_count);

  int sent = 0;
  for (int i = 0; i < buffer_count; i++) {
    uint8_t tx_buf[256];
    int packed_len = protocol_pack(&buffer[i], tx_buf, sizeof(tx_buf));
    if (packed_len > 0) {
      if (node_send(tx_buf, packed_len)) {
        sent++;
        delay(50); // невелика пауза між пакетами для стабільності приймача
      } else {
        Serial.println("[BUFFER] Помилка відправки буферизованого пакета, перериваю вивантаження.");
        break;
      }
    }
  }

  if (sent < buffer_count) {
    int remaining = buffer_count - sent;
    for (int i = 0; i < remaining; i++) {
      buffer[i] = buffer[sent + i];
    }
    buffer_count = remaining;
  } else {
    buffer_count = 0;
  }
  Serial.print("[BUFFER] Вивантаження завершено. Залишок у буфері: ");
  Serial.println(buffer_count);
}

void send_telemetry() {
  SensorPacket pkt = {0};
  pkt.version = PROTOCOL_VERSION;
  pkt.msg_type = MSG_TELEMETRY;
  pkt.node_id = MY_NODE_ID;
  pkt.sequence = seq_counter++;
  pkt.timestamp_ms = millis();

  // Реальні дані з IMU (MPU9250) замість фейкових t/h.
  snprintf((char*)pkt.payload, MAX_PAYLOAD_SIZE,
           "{\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
           mpu.getRoll(), mpu.getPitch(), mpu.getYaw());
  pkt.payload_len = strlen((char*)pkt.payload);

  // Якщо транспорт не готовий -- одразу у буфер!
  if (!transport_ready()) {
    buffer_packet(&pkt);
    return;
  }

  uint8_t tx_buf[256];
  int packed_len = protocol_pack(&pkt, tx_buf, sizeof(tx_buf));
  if (packed_len > 0) {
    bool success = node_send(tx_buf, packed_len);
    if (!success) {
      buffer_packet(&pkt);
    } else {
      Serial.print("[TX] seq=");
      Serial.print(pkt.sequence);
      Serial.print(" -> шлюз (");
      Serial.print(packed_len);
      Serial.println(" байт)");
    }
  }
}

// Ініціює ОДНУ надійну відправку ALARM. Сама відправка (і всі retry)
// відбувається асинхронно через reliable_tick() в loop().
void send_alarm() {
  if (!transport_ready()) {
    Serial.println("[ALARM] Транспорт не готовий -- ALARM не відправлено.");
    return;
  }
  if (reliable_is_busy(&reliable)) {
    Serial.println("[ALARM] Попередня критична відправка ще активна, зачекай.");
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

  Serial.print("[ALARM] Ініціюю надійну відправку sequence=");
  Serial.println(pkt.sequence);
  reliable_send_critical(&reliable, &pkt, millis());
}

// Нечутливе до блокувань читання команд з Serial Monitor. Введи "alarm"
// і натисни Enter -- це відправить одну критичну подію з ACK/retry.
// Працює однаково в усіх трьох режимах транспорту -- це завжди основний
// USB-Serial, навіть коли плата на UART-транспорті (той -- окремий,
// апаратний UART2, не займає цю консоль).
void check_serial_commands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serial_cmd_buffer.length() > 0) {
        serial_cmd_buffer.trim();
        serial_cmd_buffer.toLowerCase();
        if (serial_cmd_buffer == "alarm") {
          send_alarm();
        } else {
          Serial.print("[SERIAL] Невідома команда: ");
          Serial.println(serial_cmd_buffer);
        }
        serial_cmd_buffer = "";
      }
    } else {
      serial_cmd_buffer += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();               // Ініціалізація I2C для IMU
  Wire.setTimeOut(1000);      // КРИТИЧНО: без цього I2C-транзакція, що не
                              // отримала відповіді від давача (обрив дроту,
                              // просідання живлення під час TX Wi-Fi), може
                              // блокувати шину НАЗАВЖДИ -- а разом з нею і
                              // mpu.update() у loop(), тобто ВЕСЬ loop() і
                              // Serial теж (це і є "плата зависла, нічого
                              // не виводить" без жодного повідомлення).
  delay(2000);                // Дати час платі й давачу на старт

  Serial.println("\nІніціалізація IMU MPU9250...");
  imu_ok = mpu.setup(0x68);
  if (!imu_ok) {
    Serial.println("[ПОМИЛКА] IMU не знайдено! Перевірте підключення (SDA/SCL). Телеметрія піде з нульовими roll/pitch/yaw, вузол продовжить працювати.");
  } else {
    Serial.println("[OK] IMU успішно підключено.");
  }

#if NODE_TRANSPORT == NODE_TRANSPORT_UART
  // UART-режим: жодного WiFi -- пряма дротова лінія до Pi через USB-TTL
  // перехідник. Serial (USB) лишається вільним для діагностики/"alarm".
  GatewaySerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.println("\n=================================");
  Serial.println("Режим транспорту: UART (пряма дротова лінія до шлюзу)");
  Serial.print("UART2: RX=GPIO"); Serial.print(UART_RX_PIN);
  Serial.print(", TX=GPIO"); Serial.print(UART_TX_PIN);
  Serial.print(", "); Serial.print(UART_BAUD); Serial.println(" 8N1");
  Serial.println("=================================\n");
#else
  // Очищаємо буфер Serial від стартового сміття перед опитуванням
  while (Serial.available() > 0) { Serial.read(); delay(10); }

  // 1. SSID
  Serial.println("\n=================================");
  Serial.println("1. Введіть назву Wi-Fi мережі (SSID):");
  Serial.println("=================================");
  while (true) {
    if (Serial.available() > 0) {
      String input_ssid = Serial.readStringUntil('\n');
      input_ssid.trim();
      if (input_ssid.length() > 0 && input_ssid.length() < sizeof(wifi_ssid)) {
        input_ssid.toCharArray(wifi_ssid, sizeof(wifi_ssid));
        break;
      }
    }
    delay(50);
  }
  Serial.print("Записано SSID: ");
  Serial.println(wifi_ssid);

  while (Serial.available() > 0) { Serial.read(); delay(10); }

  // 2. PASSWORD
  Serial.println("\n=================================");
  Serial.print("2. Введіть пароль для Wi-Fi '");
  Serial.print(wifi_ssid);
  Serial.println("':");
  Serial.println("=================================");
  while (true) {
    if (Serial.available() > 0) {
      String pass = Serial.readStringUntil('\n');
      pass.trim();
      // Дозволяємо порожній пароль, якщо мережа відкрита.
      if (pass.length() < sizeof(wifi_password)) {
        pass.toCharArray(wifi_password, sizeof(wifi_password));
        break;
      }
    }
    delay(50);
  }
  Serial.print("Записано пароль: ");
  Serial.println(wifi_password);

  while (Serial.available() > 0) { Serial.read(); delay(10); }

  // 3. IP шлюзу (той самий Raspberry Pi, що і для MQTT -- просто інший порт)
  Serial.println("\n=================================");
  Serial.println("3. Введіть IP-адресу шлюзу (Raspberry Pi):");
  Serial.println("=================================");
  while (true) {
    if (Serial.available() > 0) {
      String ip = Serial.readStringUntil('\n');
      ip.trim();
      if (ip.length() > 0 && ip.length() < sizeof(gateway_host)) {
        ip.toCharArray(gateway_host, sizeof(gateway_host));
        break;
      }
    }
    delay(50);
  }
  Serial.print("Використовую шлюз: ");
  Serial.print(gateway_host);
  Serial.print(":");
  Serial.println(GATEWAY_PORT);
  Serial.println("=================================\n");

  if (!gateway_ip.fromString(gateway_host)) {
    Serial.println("[ПОМИЛКА] Не вдалося розпізнати IP-адресу шлюзу!");
  }

  setup_wifi();

  #if NODE_TRANSPORT == NODE_TRANSPORT_UDP
    udp.begin(UDP_LOCAL_PORT); // той самий локальний порт і для send, і для ACK
    Serial.println("Режим транспорту: UDP");
  #elif NODE_TRANSPORT == NODE_TRANSPORT_TCP
    Serial.println("Режим транспорту: TCP (сирий сокет, без MQTT)");
  #endif
#endif

  reliable_init(&reliable, esp32_reliable_send, NULL);

  Serial.print("Готово. Вузол ID=");
  Serial.print(MY_NODE_ID);
  Serial.println(". Введи \"alarm\" в цьому Serial Monitor і натисни Enter,");
  Serial.println("щоб надіслати критичну подію з ACK/retry.");
}

void loop() {
#if NODE_TRANSPORT == NODE_TRANSPORT_UDP
  // 1. Неблокуюча перевірка та автоперепідключення до Wi-Fi. UDP сам по
  //    собі без стану з'єднання -- досить, щоб WiFi був живий. Буфер
  //    вивантажуємо РІВНО ОДИН РАЗ на переході "не було WiFi" -> "є WiFi"
  //    (was_connected перевіряє саме цей перехід, не просто "зараз є WiFi").
  static bool udp_was_connected = false;
  bool udp_now_connected = (WiFi.status() == WL_CONNECTED);
  if (!udp_now_connected) {
    static unsigned long last_wifi_attempt = 0;
    if (millis() - last_wifi_attempt > 3000) {
      last_wifi_attempt = millis();
      Serial.println("[WIFI] Зв'язок втрачено або відсутній. Пробую перепідключитися...");
      WiFi.disconnect();
      WiFi.begin(wifi_ssid, wifi_password);
    }
  } else if (!udp_was_connected) {
    flush_buffer(); // щойно (пере)з'явився WiFi -- вивантажуємо накопичене
  }
  udp_was_connected = udp_now_connected;
#elif NODE_TRANSPORT == NODE_TRANSPORT_TCP
  // 1. Неблокуюче перепідключення Wi-Fi, потім TCP-сокета -- той самий
  //    патерн, що раніше був для WiFi+MQTT, просто без MQTT-обгортки.
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long last_wifi_attempt = 0;
    if (millis() - last_wifi_attempt > 3000) {
      last_wifi_attempt = millis();
      Serial.println("[WIFI] Зв'язок втрачено або відсутній. Пробую перепідключитися...");
      WiFi.disconnect();
      WiFi.begin(wifi_ssid, wifi_password);
    }
  } else if (!tcp_client.connected()) {
    static unsigned long last_tcp_attempt = 0;
    if (millis() - last_tcp_attempt > 2000) {
      last_tcp_attempt = millis();
      Serial.print("[TCP] Підключення до шлюзу...");
      downlink_assembler.reset();
      if (tcp_client.connect(gateway_ip, GATEWAY_PORT)) {
        Serial.println(" підключено!");
        flush_buffer();
      } else {
        Serial.println(" не вдалося.");
      }
    }
  }
#endif
  // NODE_TRANSPORT_UART -- немає ні WiFi, ні "з'єднання" для перевірки:
  // дріт або є, або плата взагалі не отримує живлення/даних.

  poll_downlink();

  // Постійно оновлюємо дані з IMU для коректної роботи фільтра орієнтації
  // (незалежно від стану транспорту -- давач має оновлюватись завжди).
  // Викликаємо лише якщо IMU взагалі відповіла при старті -- інакше це
  // гарантовано або "сміття", або (без Wire.setTimeOut вище) залипання.
  if (imu_ok) {
    mpu.update();
  }

  check_serial_commands();

  ReliableStatus rst = reliable_tick(&reliable, millis());
  if (rst == RELIABLE_SUCCESS) {
    Serial.print("[ALARM] seq=");
    Serial.print(reliable.sequence);
    Serial.print(" ДОСТАВЛЕНО, спроб=");
    Serial.println(reliable.attempts);
  } else if (rst == RELIABLE_EXHAUSTED) {
    Serial.print("[ALARM] seq=");
    Serial.print(reliable.sequence);
    Serial.print(": RETRY ВИЧЕРПАНО, НЕ доставлено, спроб=");
    Serial.println(reliable.attempts);
  }

  // 3. Генерація та відправка телеметрії кожні 5 секунд -- якщо транспорт
  //    не готовий, send_telemetry() сама збереже пакет у буфер.
  static unsigned long last_send = 0;
  if (millis() - last_send >= 5000) {
    last_send = millis();
    send_telemetry();
  }
}
