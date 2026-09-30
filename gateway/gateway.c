#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <mosquitto.h>
#include <cjson/cJSON.h>
#include "protocol.h" // Підключаємо реальний протокол Блока А

#define MAX_NODES 10
#define MAX_SEEN_ALARMS 16
#define NODE_TIMEOUT_MS 5000 // 5 секунд без повідомлень = вузол OFFLINE

// Структура для відстеження стану кожного сенсорного вузла
typedef struct {
    uint16_t node_id;
    int32_t  max_seq_seen;
    uint32_t received_count;
    uint32_t lost_count;
    uint32_t duplicate_count;
    uint64_t last_seen_ms;
    
    // Кільцевий буфер для дедуплікації критичних повідомлень
    uint32_t recent_alarms[MAX_SEEN_ALARMS];
    int alarm_idx;
} NodeState;

NodeState nodes[MAX_NODES];
int node_count = 0;
struct mosquitto *mosq_global = NULL;

// Отримання монотонного часу (незалежного від системного годинника)
uint64_t get_monotonic_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
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
        node_count++;
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

// ВИПРАВЛЕНО: Формування та відправка ACK через реальний protocol_pack
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

// ВИПРАВЛЕНО: Обробка через реальний protocol_unpack
void on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    SensorPacket pkt = {0};
    
    // Розпакування та строга перевірка CRC32
    int status = protocol_unpack((const uint8_t*)msg->payload, msg->payloadlen, &pkt);
    
    if (status == PROTO_ERR_BAD_CRC) {
        printf("[ПОМИЛКА] Пакет битий (PROTO_ERR_BAD_CRC)! Відхилено.\n");
        return; 
    } else if (status != PROTO_OK) {
        printf("[ПОПЕРЕДЖЕННЯ] Помилка розпакування (Код: %d). Відхилено.\n", status);
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
    
    // Обробка критичних подій 
    if (pkt.msg_type == MSG_ALARM || pkt.msg_type == MSG_CONFIG) {
        if (!is_duplicate_alarm(node, pkt.sequence)) {
            printf("[ПОДІЯ] Вузол %u надіслав ALARM/CONFIG (Seq: %u)\n", pkt.node_id, pkt.sequence);
            record_alarm(node, pkt.sequence);
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
        
        double loss_rate = 0.0;
        if ((n->received_count + n->lost_count) > 0) {
            loss_rate = ((double)n->lost_count / (n->received_count + n->lost_count)) * 100.0;
        }

        printf("%-5u | %-7s | %-6u | %-6u | %-6u | %-10d (Loss: %.1f%%)\n",
               n->node_id, online ? "ONLINE" : "OFFLINE",
               n->received_count, n->lost_count, n->duplicate_count,
               n->max_seq_seen, loss_rate);
    }
    printf("======================================================\n");
}

int main(void) {
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
    return 0;
}