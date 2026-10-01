#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <mosquitto.h>
#include <cjson/cJSON.h>
#include "protocol.h" // Підключаємо реальний протокол Блока А

#define MAX_NODES 10
#define MAX_SEEN_ALARMS 16
#define NODE_TIMEOUT_MS 5000 // 5 секунд без повідомлень = вузол OFFLINE
#define LOG_FILE_PATH "gateway_log.txt"

// Структура для відстеження стану кожного сенсорного вузла
typedef struct {
    uint16_t node_id;
    int32_t  max_seq_seen;
    uint32_t received_count;
    uint32_t lost_count;
    uint32_t duplicate_count;
    uint64_t last_seen_ms;
    bool     was_online; // для виявлення моменту переходу ONLINE<->OFFLINE (Крок 8)

    // Кільцевий буфер для дедуплікації критичних повідомлень
    uint32_t recent_alarms[MAX_SEEN_ALARMS];
    int alarm_idx;
} NodeState;

NodeState nodes[MAX_NODES];
int node_count = 0;
struct mosquitto *mosq_global = NULL;

// Крок 8: лог у файл (окрім екрана) + наскрізний лічильник битих пакетів.
FILE *g_log_file = NULL;
uint32_t corrupted_count = 0;

// Отримання монотонного часу (незалежного від системного годинника)
uint64_t get_monotonic_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

// Одна подія -> одразу і на екран (printf), і у файл (fprintf), з міткою
// часу, за форматом "простий текстовий файл, рядок на подію", як просив
// block-C-gateway.md для Рівня 3. fflush одразу -- щоб дані не загубились
// у буфері, якщо шлюз впаде посеред демо.
void log_event(const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_info);

    va_list args_stdout, args_file;
    va_start(args_stdout, fmt);
    va_copy(args_file, args_stdout);

    printf("[%s] ", ts);
    vprintf(fmt, args_stdout);
    va_end(args_stdout);

    if (g_log_file != NULL) {
        fprintf(g_log_file, "[%s] ", ts);
        vfprintf(g_log_file, fmt, args_file);
        fflush(g_log_file);
    }
    va_end(args_file);
}

// Пошук існуючого або створення нового запису вузла в пам'яті
NodeState* find_or_create_node(uint16_t node_id) {
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].node_id == node_id) return &nodes[i];
    }
    if (node_count < MAX_NODES) {
        NodeState *new_node = &nodes[node_count];
        memset(new_node, 0, sizeof(NodeState));
        new_node->node_id = node_id;
        new_node->max_seq_seen = -1; // -1 означає, що пакетів ще не було
        new_node->was_online = true; // щойно прийшов перший пакет -- вважаємо online
        node_count++;
        log_event("[СТАТУС] Новий вузол %u зареєстрований\n", node_id);
        return new_node;
    }
    return NULL;
}

// Перевірка на наявність дублікатів ALARM
bool is_duplicate_alarm(NodeState *node, uint32_t seq) {
    for (int i = 0; i < MAX_SEEN_ALARMS; i++) {
        if (node->recent_alarms[i] == seq) return true;
    }
    return false;
}

// Фіксація нового sequence для ALARM
void record_alarm(NodeState *node, uint32_t seq) {
    node->recent_alarms[node->alarm_idx] = seq;
    node->alarm_idx = (node->alarm_idx + 1) % MAX_SEEN_ALARMS;
}

// Формування та відправка ACK через реальний protocol_pack
void send_ack(uint16_t node_id, uint32_t seq) {
    SensorPacket ack_pkt = {0};
    ack_pkt.version = PROTOCOL_VERSION;
    ack_pkt.msg_type = MSG_ACK;
    ack_pkt.node_id = node_id;
    ack_pkt.sequence = seq;
    ack_pkt.timestamp_ms = get_monotonic_time_ms();
    ack_pkt.payload_len = 0;

    uint8_t tx_buf[256];
    // Отримуємо компактні байти (лише заголовок + CRC), а не всю структуру
    int len = protocol_pack(&ack_pkt, tx_buf, sizeof(tx_buf));

    if (len > 0) {
        char topic[64];
        snprintf(topic, sizeof(topic), "case24/downlink/%u", node_id);
        mosquitto_publish(mosq_global, NULL, topic, len, tx_buf, 0, false);
    }
}

// Обробка через реальний protocol_unpack
void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    SensorPacket pkt = {0};

    // Розпакування та строга перевірка CRC32
    int status = protocol_unpack((const uint8_t*)msg->payload, msg->payloadlen, &pkt);

    if (status == PROTO_ERR_BAD_CRC) {
        corrupted_count++;
        log_event("[ПОМИЛКА] Пакет битий (PROTO_ERR_BAD_CRC)! Відхилено. "
                   "Разом битих пакетів: %u\n", corrupted_count);
        return;
    } else if (status != PROTO_OK) {
        corrupted_count++;
        log_event("[ПОПЕРЕДЖЕННЯ] Помилка розпакування (код: %d). Відхилено. "
                   "Разом битих пакетів: %u\n", status, corrupted_count);
        return;
    }

    // Трекінг вузла
    NodeState *node = find_or_create_node(pkt.node_id);
    if (!node) return;

    node->last_seen_ms = get_monotonic_time_ms();
    node->received_count++;

    // Аналіз втрат та дублікатів
    if (node->max_seq_seen == -1) {
        node->max_seq_seen = pkt.sequence;
    } else {
        if ((int32_t)pkt.sequence > node->max_seq_seen) {
            node->lost_count += (pkt.sequence - node->max_seq_seen - 1);
            node->max_seq_seen = pkt.sequence;
        } else {
            node->duplicate_count++;
        }
    }

    // Парсинг JSON-корисного навантаження
    if (pkt.payload_len > 0) {
        pkt.payload[pkt.payload_len] = '\0';
        cJSON *json = cJSON_Parse((const char*)pkt.payload);
        if (json) {
            cJSON_Delete(json);
        }
    }

    // Обробка критичних подій -- тепер логуємо ОБИДВА випадки: і нову
    // подію, і задедупліковану, бо саме другий випадок і є доказом
    // критерію №3 (повторний пакет не створив дублікат бізнес-події).
    if (pkt.msg_type == MSG_ALARM || pkt.msg_type == MSG_CONFIG) {
        if (!is_duplicate_alarm(node, pkt.sequence)) {
            log_event("[ПОДІЯ] Вузол %u надіслав ALARM/CONFIG (Seq: %u) -- НОВА подія\n",
                       pkt.node_id, pkt.sequence);
            record_alarm(node, pkt.sequence);
        } else {
            log_event("[ПОДІЯ] Вузол %u повторив ALARM/CONFIG (Seq: %u) -- "
                       "ДЕДУПЛІКОВАНО, друга подія НЕ створена\n",
                       pkt.node_id, pkt.sequence);
        }
        send_ack(pkt.node_id, pkt.sequence);
    }
}

void print_dashboard() {
    uint64_t now = get_monotonic_time_ms();
    printf("\n--- Дашборд (Вузлів: %d) ---\n", node_count);
    printf("%-5s | %-7s | %-6s | %-6s | %-6s | %-10s\n",
           "Вузол", "Статус", "Отр.", "Втрат", "Дубл.", "Max Seq");
    printf("------------------------------------------------------\n");

    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        bool online = (now - n->last_seen_ms) < NODE_TIMEOUT_MS;

        // Крок 8: фіксуємо МОМЕНТ переходу ONLINE<->OFFLINE у лог --
        // саме ці мітки часу потрібні для звіту про 30-секундний розрив
        // (критерій приймання №4): коли вузол "зник" і коли "повернувся".
        if (online != n->was_online) {
            if (online) {
                log_event("[СТАТУС] Вузол %u знову ONLINE (зв'язок відновлено)\n", n->node_id);
            } else {
                log_event("[СТАТУС] Вузол %u перейшов у OFFLINE (немає повідомлень > %dмс)\n",
                           n->node_id, NODE_TIMEOUT_MS);
            }
            n->was_online = online;
        }

        double loss_rate = 0.0;
        if ((n->received_count + n->lost_count) > 0) {
            loss_rate = ((double)n->lost_count / (n->received_count + n->lost_count)) * 100.0;
        }

        printf("%-5u | %-7s | %-6u | %-6u | %-6u | %-10d (Loss: %.1f%%)\n",
               n->node_id, online ? "ONLINE" : "OFFLINE",
               n->received_count, n->lost_count, n->duplicate_count,
               n->max_seq_seen, loss_rate);
    }
    printf("Всього битих пакетів з початку роботи: %u\n", corrupted_count);
    printf("======================================================\n");
}

int main(void) {
    g_log_file = fopen(LOG_FILE_PATH, "a");
    if (g_log_file != NULL) {
        time_t start_t = time(NULL);
        fprintf(g_log_file, "\n===== Новий запуск Gateway: %s", ctime(&start_t)); /* ctime сам додає \n */
        fflush(g_log_file);
        printf("Лог подій пишеться у файл: %s\n", LOG_FILE_PATH);
    } else {
        fprintf(stderr, "[УВАГА] Не вдалося відкрити %s для запису -- лог буде тільки на екрані.\n",
                LOG_FILE_PATH);
    }

    mosquitto_lib_init();
    mosq_global = mosquitto_new("GatewayClient", true, NULL);
    mosquitto_message_callback_set(mosq_global, on_message);

    if (mosquitto_connect(mosq_global, "localhost", 1883, 60) != 0) {
        fprintf(stderr, "Помилка підключення до MQTT-брокера.\n");
        return 1;
    }

    mosquitto_subscribe(mosq_global, NULL, "case24/uplink", 0);
    printf("Шлюз успішно запущено. Очікування телеметрії...\n");

    uint64_t last_dash_ms = get_monotonic_time_ms();

    while (1) {
        mosquitto_loop(mosq_global, 100, 1);

        uint64_t now = get_monotonic_time_ms();
        if (now - last_dash_ms >= 2000) {
            print_dashboard();
            last_dash_ms = now;
        }
    }

    mosquitto_destroy(mosq_global);
    mosquitto_lib_cleanup();
    if (g_log_file != NULL) {
        fclose(g_log_file);
    }
    return 0;
}
