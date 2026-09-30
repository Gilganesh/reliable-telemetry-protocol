# Блок C — Gateway (C, на ноутбуці)

> Перед цим прочитай `00-overview-shared-contract.md`.

## Твоя зона відповідальності

Програма на C, що піднімає (чи просто використовує вже запущений)
MQTT-брокер, підписується на дані від усіх вузлів, розпаковує пакети
(`protocol.c` з Блоку A), трекає кожен вузол окремо, показує dashboard і
пише лог. Плюс — відправка ACK на критичні повідомлення.

## Рівень 1 — Bring-up: підписка і сирі байти

### Завдання
1. Встанови `libmosquitto`: `brew install mosquitto` (Mac, ставить і
   брокер, і бібліотеку з заголовками) або `apt install
   libmosquitto-dev` (Linux).
2. Піднімі брокер (`mosquitto -v` в окремому терміналі, лишити працювати).
3. Мінімальна C-програма з `libmosquitto`: підключення до
   `localhost:1883`, підписка на `case24/uplink`, callback-функція, яка
   просто друкує розмір і hex вхідних байтів кожного повідомлення. Базова
   структура коду (API `libmosquitto`, спрощено):
   ```c
   #include <mosquitto.h>

   void on_message(struct mosquitto *mosq, void *userdata,
                    const struct mosquitto_message *msg) {
       printf("Отримано %d байт з топіка %s\n", msg->payloadlen, msg->topic);
       // тут поки просто друк hex, розбір -- на Рівні 2
   }

   int main(void) {
       mosquitto_lib_init();
       struct mosquitto *mosq = mosquitto_new(NULL, true, NULL);
       mosquitto_message_callback_set(mosq, on_message);
       mosquitto_connect(mosq, "localhost", 1883, 60);
       mosquitto_subscribe(mosq, NULL, "case24/uplink", 0);
       mosquitto_loop_forever(mosq, -1, 1);  // блокуючий цикл
       return 0;
   }
   ```
   Компіляція (Mac, шляхи можуть відрізнятись — перевір `brew info
   mosquitto` для точних шляхів): `cc gateway.c -o gateway -lmosquitto`.
4. Перевір разом із Блоком B (Рівень 1 там) — тестові текстові рядки з
   ESP32 мають з'являтись у твоїй програмі.

### Готово, коли
- Твоя C-програма (не `mosquitto_sub`, а власний код) бачить повідомлення
  від реальної плати ESP32.

## Рівень 2 — Розбір реального пакета

### Завдання
1. Підключи `protocol.c`/`protocol.h` (Блок A). У `on_message` виклич
   `protocol_unpack(msg->payload, msg->payloadlen, &pkt)`.
2. Якщо повернуло помилку — це критерій приймання №5 ("пошкоджений пакет"):
   залогуй і **не крашся**, `return` з callback-функції і чекай наступне
   повідомлення. Порахуй кількість таких випадків (глобальний лічильник).
3. Якщо все ок — виведи розібрані поля: `node_id`, `msg_type`, `sequence`,
   payload.

### Готово, коли
- Реальний пакет з ESP32 (Блок B, Рівень 2) розбирається правильно.
- Тест: подай у `protocol_unpack` напряму (не через MQTT, просто викликом
  функції в тестовому файлі) зіпсовані байти — переконайся, що помилка
  повертається, а не крах програми.

## Рівень 3 — Трекінг вузлів, dashboard, лог

### Завдання
1. Заведи структуру (аналог Python-версії `NodeState`):
   ```c
   typedef struct {
       uint16_t node_id;
       int32_t  max_seq_seen;   // -1 = ще не бачили
       uint32_t received_count;
       uint32_t lost_count;
       uint32_t duplicate_count;
       uint64_t last_seen_ms;   // час останнього пакета (для online/offline)
   } NodeState;

   NodeState nodes[MAX_NODES];  // простий масив, шукати по node_id лінійно -- нормально для 3-10 вузлів
   int node_count;
   ```
2. Логіка gap/duplicate — та сама, що вже перевірена в Python-версії
   (`gateway/node_state.py`, метод `record_packet`): якщо `sequence` більше
   за `max_seq_seen` — новий пакет (різниця більша за 1 — це "gap",
   пропущені пакети); якщо `sequence` не більше — дублікат.
3. Dashboard — просто `printf()` таблиці раз на N секунд (не обов'язково
   складний UI, головне — видно всі вузли одночасно, окремо, з їхніми
   лічильниками). Online/offline — за `(поточний_час - last_seen_ms) >
   timeout`.
4. Лог — простий текстовий файл (`fprintf` рядок на подію), не обов'язково
   JSON, якщо це ускладнює C-код без бібліотеки — головне, щоб Блок D міг
   з нього витягнути потрібні числа для звіту.

### Готово, коли
- 3 вузли одночасно (2 реальних + симульований від Блоку D) видно в
  dashboard окремо, з правильними лічильниками. Критерій приймання №1.

## Рівень 4 — ACK на критичні повідомлення

### Завдання
1. Коли `msg_type == MSG_ALARM` чи `MSG_CONFIG` — зібрати ACK-пакет
   (`protocol_pack` з `msg_type = MSG_ACK`, той самий `sequence`, що і в
   отриманому повідомленні) і опублікувати в `case24/downlink/<node_id>`.
2. Дедуплікація бізнес-подій: якщо ALARM з тим самим `(node_id, sequence)`
   вже було оброблено раніше — не рахувати як нову подію вдруге (окремий
   набір "вже бачені критичні (node_id, sequence)", можна простий масив
   останніх N пар).

### Готово, коли
- Повторна доставка того самого ALARM (той самий sequence, надісланий
  ще раз Блоком B при retry) не створює другого запису тривоги.
