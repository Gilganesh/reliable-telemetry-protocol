/*
 * sim_node.c -- Блок D, Рівень 1-2: симульований (третій) вузол.
 *
 * На відміну від sim_node_smoke_test.c (який НЕ використовує мережу),
 * ця програма публікує реальні пакети через СПРАВЖНІЙ MQTT у той самий
 * топік case24/uplink, що і реальні ESP32-плати (Блок B).
 *
 * НОВЕ (Крок 3-4 плану): sim_node тепер вміє
 *   - слати ALARM з надійною доставкою (ACK/retry через reliability.c) --
 *     підписується на свій downlink-топік і чекає підтвердження, так
 *     само, як це робитиме реальна плата;
 *   - симулювати втрату (--loss-percent) і дублювання (--duplicate-percent)
 *     пакетів "на джерелі" -- це стосується і телеметрії, і ALARM/retry
 *     (включно з повторними спробами!), так само реалістично, як і
 *     справжня погана мережа.
 *
 * Компіляція (Linux, після apt install libmosquitto-dev):
 *   cc -Wall -Wextra sim_node.c protocol.c reliability.c \
 *      -lmosquitto -o sim_node
 *   (protocol.h/protocol.c/reliability.h/reliability.c мають лежати
 *   поруч -- скрипт sync-shared.sh з Кроку 1 це робить автоматично)
 *
 * Компіляція (Mac, після brew install mosquitto):
 *   cc -Wall -Wextra sim_node.c protocol.c reliability.c \
 *      -I$(brew --prefix mosquitto)/include \
 *      -L$(brew --prefix mosquitto)/lib -lmosquitto -o sim_node
 *
 * Запуск (брокер mosquitto має вже працювати):
 *   ./sim_node                                   # localhost, звичайна телеметрія
 *   ./sim_node 192.168.1.50                      # брокер на іншій машині
 *   ./sim_node --send-alarm 192.168.1.50         # + одна ALARM з ACK/retry
 *   ./sim_node --loss-percent 20 --send-alarm 192.168.1.50
 *                                                 # + 20% втрат на кожній спробі
 *   ./sim_node --loss-percent 20 --duplicate-percent 5 192.168.1.50
 *
 * Прапорці можна ставити в будь-якому порядку, до чи після IP.
 * Зупинка: Ctrl+C.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <mosquitto.h>
#include "protocol.h"
#include "reliability.h"

/* Домовленість з командою: node_id 1, 2, 3 -- реальні плати ESP32
 * (Блок B), node_id 99 -- цей симульований вузол. */
#define SIM_NODE_ID 99

#define TELEMETRY_INTERVAL_MS 3000

/* ---- Параметри симуляції втрат/дублікатів (з --loss-percent / --duplicate-percent) ---- */
static int g_loss_percent = 0;
static int g_duplicate_percent = 0;

/* ---- Наскрізний лічильник sequence: ОДИН на весь вузол, спільний і
 * для TELEMETRY, і для ALARM -- протокол вимагає єдиної послідовності
 * на вузол, а не окремої для кожного типу повідомлення. ---- */
static uint32_t seq_counter = 0;

/* ---- Стан надійної доставки критичних повідомлень (Блок A, reliability.c) ---- */
static ReliableCtx g_reliable;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

static bool roll_percent(int percent) {
    if (percent <= 0) return false;
    if (percent >= 100) return true;
    return (rand() % 100) < percent;
}

/* Єдина точка, через яку sim_node реально публікує байти в MQTT --
 * і телеметрія, і ALARM (включно з retry-копіями) проходять через неї,
 * тому --loss-percent/--duplicate-percent впливають на все однаково
 * реалістично, як і справжня погана мережа вплинула б і на звичайні
 * дані, і на тривоги. */
static void transport_publish(struct mosquitto *mosq, const uint8_t *buf, int len) {
    if (roll_percent(g_loss_percent)) {
        printf("[LOSS-SIM] пакет НЕ відправлено (симуляція %d%% втрат)\n", g_loss_percent);
        return;
    }
    mosquitto_publish(mosq, NULL, "case24/uplink", len, buf, 0, false);

    if (roll_percent(g_duplicate_percent)) {
        printf("[DUP-SIM] дублюю щойно відправлений пакет (симуляція %d%% дублів)\n",
               g_duplicate_percent);
        mosquitto_publish(mosq, NULL, "case24/uplink", len, buf, 0, false);
    }
}

/* Обгортка з сигнатурою reliable_send_fn -- саме її викликає reliability.c
 * і для першої спроби, і для кожного retry (ТОЙ САМИЙ tx_buf щоразу). */
static void sim_reliable_send(void *userdata, const uint8_t *buf, int len) {
    struct mosquitto *mosq = (struct mosquitto *)userdata;
    transport_publish(mosq, buf, len);
}

/* Викликається бібліотекою mosquitto, коли приходить повідомлення в
 * топіку, на який ми підписані (case24/downlink/99) -- тобто ACK від
 * Gateway. */
static void on_downlink_message(struct mosquitto *mosq, void *userdata,
                                 const struct mosquitto_message *msg) {
    (void)mosq;
    (void)userdata;

    SensorPacket pkt = {0};
    int rc = protocol_unpack((const uint8_t *)msg->payload, (size_t)msg->payloadlen, &pkt);
    if (rc != PROTO_OK) {
        printf("[WARN] Пошкоджений downlink-пакет, код=%d\n", rc);
        return;
    }
    if (pkt.msg_type == MSG_ACK) {
        printf("[ACK] Отримано підтвердження sequence=%u\n", pkt.sequence);
        reliable_on_ack_received(&g_reliable, pkt.sequence);
    }
}

/* Стан зв'язку з брокером. mosquitto_loop() сам не перепідключається, а сесія
 * clean=true губить підписку, тому підписуємось у on_connect, а після обриву
 * main() перепідключає вручну. */
static bool g_connected = false;

static void on_connect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)userdata;
    if (rc != 0) {
        printf("[MQTT] Брокер відхилив підключення (код %d)\n", rc);
        return;
    }
    char topic[32];
    snprintf(topic, sizeof(topic), "case24/downlink/%d", SIM_NODE_ID);
    mosquitto_subscribe(mosq, NULL, topic, 0);
    g_connected = true;
    printf("[MQTT] Підключено до брокера\n");
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq; (void)userdata;
    if (g_connected) printf("[MQTT] З'єднання з брокером втрачено (код %d), перепідключаюсь\n", rc);
    g_connected = false;
}

static void send_one_telemetry(struct mosquitto *mosq) {
    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = MSG_TELEMETRY;
    pkt.node_id = SIM_NODE_ID;
    pkt.sequence = seq_counter++;
    pkt.timestamp_ms = now_ms();

    double t = 20.0 + (rand() % 100) / 10.0;
    double h = 40.0 + (rand() % 200) / 10.0;
    int n = snprintf((char *)pkt.payload, MAX_PAYLOAD_SIZE, "{\"t\":%.1f,\"h\":%.1f}", t, h);
    pkt.payload_len = (uint16_t)n;

    uint8_t tx_buf[RELIABLE_MAX_PACKET];
    int packed_len = protocol_pack(&pkt, tx_buf, sizeof(tx_buf));
    if (packed_len > 0) {
        transport_publish(mosq, tx_buf, packed_len);
        printf("[TX] seq=%u payload=%s (%d байт на дроті)\n",
               pkt.sequence, pkt.payload, packed_len);
    } else {
        printf("[WARN] protocol_pack повернув помилку %d\n", packed_len);
    }
}

/* Ініціює ОДНУ надійну відправку ALARM. Сама відправка (і всі retry)
 * відбувається асинхронно через reliable_tick() в основному циклі. */
static void send_one_alarm(uint64_t now) {
    if (reliable_is_busy(&g_reliable)) {
        printf("[ALARM] Попередня критична відправка ще активна, пропускаю.\n");
        return;
    }

    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = MSG_ALARM;
    pkt.node_id = SIM_NODE_ID;
    pkt.sequence = seq_counter++;
    pkt.timestamp_ms = now;

    const char *json = "{\"alarm\":\"gas_detected\"}";
    pkt.payload_len = (uint16_t)strlen(json);
    memcpy(pkt.payload, json, pkt.payload_len);

    printf("[ALARM] Ініціюю надійну відправку sequence=%u...\n", pkt.sequence);
    bool started = reliable_send_critical(&g_reliable, &pkt, now);
    if (!started) {
        printf("[WARN] Не вдалось почати відправку ALARM.\n");
    }
}

int main(int argc, char **argv) {
    const char *broker_host = "localhost";
    bool send_alarm_once = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--loss-percent") == 0 && i + 1 < argc) {
            g_loss_percent = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duplicate-percent") == 0 && i + 1 < argc) {
            g_duplicate_percent = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--send-alarm") == 0) {
            send_alarm_once = true;
        } else {
            broker_host = argv[i];
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* рядок одразу в лог, навіть коли stdout перенаправлено у файл */
    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("SimNode99", true, NULL);
    if (!mosq) {
        fprintf(stderr, "Не вдалося створити MQTT клієнта\n");
        return 1;
    }

    reliable_init(&g_reliable, sim_reliable_send, mosq);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_message_callback_set(mosq, on_downlink_message);

    if (mosquitto_connect(mosq, broker_host, 1883, 60) != MOSQ_ERR_SUCCESS) {
        /* Не виходимо: основний цикл сам перепідключиться, коли брокер з'явиться */
        fprintf(stderr, "Не вдалося підключитись до брокера %s:1883 "
                        "(чи запущений mosquitto?) -- пробую знову кожну секунду\n", broker_host);
    }

    printf("sim_node запущено, node_id=%d, брокер=%s:1883.\n", SIM_NODE_ID, broker_host);
    printf("Телеметрія кожні %dмс. loss=%d%%, duplicate=%d%%. %s\n",
           TELEMETRY_INTERVAL_MS, g_loss_percent, g_duplicate_percent,
           send_alarm_once ? "Надішлю 1 ALARM невдовзі після старту." : "");
    printf("Ctrl+C для зупинки.\n");

    uint64_t last_telemetry_ms = 0;
    uint64_t start_ms = now_ms();
    uint64_t last_reconnect_ms = 0;
    bool alarm_pending = send_alarm_once;

    while (1) {
        uint64_t now = now_ms();

        if (now - last_telemetry_ms >= TELEMETRY_INTERVAL_MS) {
            last_telemetry_ms = now;
            send_one_telemetry(mosq);
        }

        /* Тривогу шлемо трохи згодом після старту (щоб з'єднання й
         * підписка вже точно встигли встановитись), рівно один раз. */
        if (alarm_pending && g_connected && now - start_ms > 1500) {
            alarm_pending = false;
            send_one_alarm(now);
        }

        ReliableStatus st = reliable_tick(&g_reliable, now);
        if (st == RELIABLE_SUCCESS) {
            printf("[ALARM] seq=%u ДОСТАВЛЕНО, спроб=%d\n",
                   g_reliable.sequence, g_reliable.attempts);
        } else if (st == RELIABLE_EXHAUSTED) {
            printf("[ALARM] seq=%u: RETRY ВИЧЕРПАНО, НЕ доставлено (спроб=%d)\n",
                   g_reliable.sequence, g_reliable.attempts);
        }

        /* timeout=100мс -- це і є "серцебиття" циклу: досить часто, щоб
         * не проґавити дедлайн ACK_TIMEOUT_MS (2000мс) чи вхідний ACK,
         * і НЕ блокує довше, ніж на 100мс (на відміну від старого
         * sleep(3), який ловив вхідні повідомлення лише раз на 3с). */
        int lrc = mosquitto_loop(mosq, 100, 1);
        if (lrc != MOSQ_ERR_SUCCESS) {
            g_connected = false;
            /* Брокер впав/перезапустився: пробуємо знову раз на секунду.
             * mosquitto_reconnect блокуючий, але перебирає всі адреси
             * (на відміну від async, який пробує лише першу, напр. ::1). */
            if (now - last_reconnect_ms >= 1000) {
                last_reconnect_ms = now;
                mosquitto_reconnect(mosq);
            }
            usleep(100000); /* mosquitto_loop повертається одразу -- не крутимо CPU */
        }
    }

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
