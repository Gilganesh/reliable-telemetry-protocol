#ifndef RELIABILITY_H
#define RELIABILITY_H

/*
 * reliability.h -- Блок A, "Рівень 2": спільна логіка ACK/retry для
 * критичних повідомлень (MSG_ALARM / MSG_CONFIG).
 *
 * Навіщо цей файл існує окремо від protocol.c:
 * ack-retry-algorithm.md прямо попереджає, що Блок B (ESP32) і Блок D
 * (sim_node) НЕ повинні кожен окремо писати свою версію циклу
 * "відправив -> чекаю ACK -> не дочекався -> повторюю", бо дві незалежні
 * реалізації легко розійдуться в деталях (наприклад, одна змінить
 * sequence між спробами, інша ні) -- і тоді поведінка на демо буде
 * різною для реального вузла і для симуляції. Цей файл -- ОДНА спільна
 * реалізація псевдокоду з ack-retry-algorithm.md, яку підключають обидва
 * блоки, так само як вони вже підключають protocol.h.
 *
 * Як підключити (як ESP32, так і sim_node):
 *   1. Поклади reliability.h і reliability.c в ту саму папку, де вже
 *      лежать protocol.h/protocol.c (в ESP32-скетчі -- обов'язково,
 *      Arduino компілює всі .c/.h з папки скетча).
 *   2. На ESP32 підключай так само, як protocol.h:
 *        extern "C" {
 *          #include "protocol.h"
 *          #include "reliability.h"
 *        }
 *   3. Реалізуй ОДНУ функцію-обгортку навколо свого способу публікації
 *      (client.publish(...) на ESP32, mosquitto_publish(...) у sim_node)
 *      з сигнатурою reliable_send_fn -- див. приклади нижче.
 *   4. Дивись докладний приклад використання в коментарі під
 *      оголошеннями функцій.
 *
 * Цей файл НІЧОГО сам не друкує (ні Serial.print, ні printf) -- логування
 * "спроба N з M" / "RETRY ВИЧЕРПАНО" (обов'язкове, критерій приймання №2)
 * лишається за викликачем, який читає ReliableCtx.attempts/.sequence і
 * результат reliable_tick(). Так само модуль нічого не знає про WiFi/MQTT
 * -- лише викликає send_fn, який йому дали при ініціалізації.
 */

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

/* Ті самі константи, що і в docs/team-blocks/ack-retry-algorithm.md --
 * якщо їх колись треба буде змінити, міняти тут ОДИН раз для всіх. */
#define ACK_TIMEOUT_MS   2000   /* скільки чекати ACK, перш ніж повторити */
#define MAX_RETRIES      3      /* скільки РАЗ повторити (без першої спроби) */

/* Максимальний розмір спакованого пакета "на дроті" -- заголовок + CRC +
 * весь можливий payload. Той самий розрахунок, що і tx_buf[256] в
 * існуючому коді Блоку B/D, просто виражений через константи Блоку A. */
#define RELIABLE_MAX_PACKET (HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE)

typedef enum {
    RELIABLE_IDLE = 0,      /* немає активної критичної відправки */
    RELIABLE_WAITING,       /* пакет відправлено, чекаємо ACK або дедлайну */
    RELIABLE_SUCCESS,       /* ЩОЙНО підтверджено -- одноразовий сигнал від tick() */
    RELIABLE_EXHAUSTED,     /* ЩОЙНО вичерпано retries -- одноразовий сигнал від tick() */
} ReliableStatus;

/*
 * Функція, яку надає викликач: "візьми ці байти й реально відправ".
 * userdata -- те саме значення, що передане в reliable_init() (щоб не
 * змушувати ESP32/sim_node тримати глобальні змінні заради callback'у).
 *
 * Приклад для ESP32 (client -- глобальний PubSubClient):
 *   void esp32_send(void *userdata, const uint8_t *buf, int len) {
 *     (void)userdata;
 *     client.publish("case24/uplink", buf, len);
 *   }
 *
 * Приклад для sim_node (mosq -- struct mosquitto*, переданий як userdata):
 *   void sim_send(void *userdata, const uint8_t *buf, int len) {
 *     struct mosquitto *mosq = (struct mosquitto *)userdata;
 *     mosquitto_publish(mosq, NULL, "case24/uplink", len, buf, 0, false);
 *   }
 */
typedef void (*reliable_send_fn)(void *userdata, const uint8_t *buf, int len);

typedef struct {
    ReliableStatus status;
    uint8_t  tx_buf[RELIABLE_MAX_PACKET]; /* той самий байти на кожну спробу */
    int      tx_len;
    uint32_t sequence;
    int      attempts;        /* скільки разів вже реально відправлено (>=1) */
    uint64_t deadline_ms;
    bool     pending_success; /* внутрішній прапорець для одноразового SUCCESS */
    reliable_send_fn send_fn;
    void     *userdata;
} ReliableCtx;

/* Викликати ОДИН раз при старті (setup() на ESP32, початок main() у sim_node). */
void reliable_init(ReliableCtx *ctx, reliable_send_fn send_fn, void *userdata);

/*
 * Почати надійну відправку критичного пакета. pkt->msg_type МАЄ бути
 * MSG_ALARM або MSG_CONFIG (для TELEMETRY/HEARTBEAT просто публікуй
 * напряму, як і раніше, -- цей механізм для них не потрібен).
 *
 * pkt->sequence МАЄ бути взятий з того самого наскрізного лічильника
 * (seq_counter), яким нумерується TELEMETRY -- протокол вимагає ЄДИНОЇ
 * послідовності на вузол, а не окремої для критичних повідомлень.
 *
 * Повертає false, якщо: pkt має не той msg_type, protocol_pack не зміг
 * запакувати пакет, АБО попередня критична відправка ще не завершилась
 * (див. reliable_is_busy) -- у поточній версії підтримується лише ОДНА
 * активна критична відправка одночасно, черги нема. Для хакатон-демо
 * (одна тривога за раз) цього достатньо; якщо знадобиться кілька
 * одночасних ALARM -- це вже розширення на майбутнє, не Рівень 2.
 */
bool reliable_send_critical(ReliableCtx *ctx, const SensorPacket *pkt, uint64_t now_ms);

/*
 * Викликати з callback'у вхідних downlink-повідомлень щоразу, коли
 * прийшов розпакований пакет з msg_type == MSG_ACK (після
 * protocol_unpack). Якщо ack_sequence збігається з тим, що зараз
 * очікується -- наступний виклик reliable_tick() поверне
 * RELIABLE_SUCCESS. Якщо ACK "старий" (від попередньої, вже завершеної
 * відправки) чи ні на що не очікується -- просто ігнорується.
 */
void reliable_on_ack_received(ReliableCtx *ctx, uint32_t ack_sequence);

/*
 * Викликати на КОЖНІй ітерації головного циклу (так само часто, як
 * client.loop() на ESP32 чи mosquitto_loop() у sim_node -- НЕ рідше,
 * інакше можна пропустити дедлайн чи ACK). Сама вирішує, чи час
 * повторити відправку (тим самим tx_buf, той самий sequence), чи час
 * здатися.
 *
 * Повертає:
 *   RELIABLE_IDLE      -- нема активної критичної відправки, нічого робити
 *   RELIABLE_WAITING    -- ще чекаємо ACK (можливо, щойно відправили retry)
 *   RELIABLE_SUCCESS    -- ЩОЙНО підтверджено (лунає РІВНО ОДИН раз);
 *                          виклич тут свій Serial.print/printf з
 *                          ctx->sequence і ctx->attempts
 *   RELIABLE_EXHAUSTED  -- ЩОЙНО вичерпано MAX_RETRIES (лунає РІВНО ОДИН
 *                          раз); тут ОБОВ'ЯЗКОВО залогувати -- це
 *                          критерій приймання №2
 */
ReliableStatus reliable_tick(ReliableCtx *ctx, uint64_t now_ms);

/* true, якщо зараз є активна критична відправка (чекає ACK чи retry) --
 * зручно перевірити перед тим, як пробувати відправити нову тривогу. */
bool reliable_is_busy(const ReliableCtx *ctx);

#endif /* RELIABILITY_H */
