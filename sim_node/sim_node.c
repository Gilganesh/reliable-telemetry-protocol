#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <mosquitto.h>
#include "protocol.h"
#include "reliability.h"

#define DEFAULT_NODE_ID 99
#define BROKER_PORT 1883
#define TELEMETRY_INTERVAL_MS 3000
#define UPLINK_TOPIC "telemetry/uplink"
#define DOWNLINK_PREFIX "telemetry/downlink/"

static uint16_t g_node_id = DEFAULT_NODE_ID;
static int g_loss_percent = 0;
static int g_duplicate_percent = 0;
static uint32_t seq_counter = 0;
static ReliableCtx g_reliable;
static bool g_connected = false;
static uint64_t g_boot_ms = 0;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t uptime_ms(void) {
    return now_ms() - g_boot_ms;
}

static bool roll_percent(int percent) {
    if (percent <= 0) return false;
    if (percent >= 100) return true;
    return (rand() % 100) < percent;
}

static void transport_publish(struct mosquitto *mosq, const uint8_t *buf, int len) {
    if (roll_percent(g_loss_percent)) {
        printf("[LOSS-SIM] packet dropped (simulated loss %d%%)\n", g_loss_percent);
        return;
    }
    mosquitto_publish(mosq, NULL, UPLINK_TOPIC, len, buf, 0, false);

    if (roll_percent(g_duplicate_percent)) {
        printf("[DUP-SIM] packet duplicated (simulated duplication %d%%)\n", g_duplicate_percent);
        mosquitto_publish(mosq, NULL, UPLINK_TOPIC, len, buf, 0, false);
    }
}

static void sim_reliable_send(void *userdata, const uint8_t *buf, int len) {
    transport_publish((struct mosquitto *)userdata, buf, len);
}

static bool payload_is_ping(const SensorPacket *pkt) {
    char js[MAX_PAYLOAD_SIZE + 1];
    memcpy(js, pkt->payload, pkt->payload_len);
    js[pkt->payload_len] = '\0';
    return strstr(js, "\"ping\"") != NULL;
}

static void send_ack(struct mosquitto *mosq, uint32_t seq) {
    SensorPacket ack = {0};
    ack.version = PROTOCOL_VERSION;
    ack.msg_type = MSG_ACK;
    ack.node_id = g_node_id;
    ack.sequence = seq;
    ack.timestamp_ms = uptime_ms();
    uint8_t tx[RELIABLE_MAX_PACKET];
    int len = protocol_pack(&ack, tx, sizeof(tx));
    if (len > 0) transport_publish(mosq, tx, len);
}

static void on_downlink_message(struct mosquitto *mosq, void *userdata,
                                const struct mosquitto_message *msg) {
    (void)userdata;
    SensorPacket pkt = {0};
    int rc = protocol_unpack((const uint8_t *)msg->payload, (size_t)msg->payloadlen, &pkt);
    if (rc != PROTO_OK) {
        printf("[WARN] Corrupted downlink packet, code=%d\n", rc);
        return;
    }
    if (pkt.msg_type == MSG_ACK) {
        printf("[ACK] Acknowledged sequence=%u\n", pkt.sequence);
        reliable_on_ack_received(&g_reliable, pkt.sequence);
    } else if (pkt.msg_type == MSG_CONFIG && pkt.node_id == g_node_id && payload_is_ping(&pkt)) {
        send_ack(mosq, pkt.sequence);
    }
}

static void on_connect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)userdata;
    if (rc != 0) {
        printf("[MQTT] Broker refused connection (code %d)\n", rc);
        return;
    }
    char topic[48];
    snprintf(topic, sizeof(topic), DOWNLINK_PREFIX "%u", g_node_id);
    mosquitto_subscribe(mosq, NULL, topic, 0);
    g_connected = true;
    printf("[MQTT] Connected to broker\n");
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq; (void)userdata;
    if (g_connected) printf("[MQTT] Lost connection to broker (code %d), reconnecting\n", rc);
    g_connected = false;
}

static void send_one_telemetry(struct mosquitto *mosq) {
    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = MSG_TELEMETRY;
    pkt.node_id = g_node_id;
    pkt.sequence = seq_counter++;
    pkt.timestamp_ms = uptime_ms();

    double t = 20.0 + (rand() % 100) / 10.0;
    double h = 40.0 + (rand() % 200) / 10.0;
    int n = snprintf((char *)pkt.payload, MAX_PAYLOAD_SIZE,
                     "{\"temperature\":%.1f,\"humidity\":%.1f}", t, h);
    pkt.payload_len = (uint16_t)n;

    uint8_t tx_buf[RELIABLE_MAX_PACKET];
    int packed_len = protocol_pack(&pkt, tx_buf, sizeof(tx_buf));
    if (packed_len > 0) {
        transport_publish(mosq, tx_buf, packed_len);
        printf("[TX] seq=%u payload=%s (%d bytes)\n", pkt.sequence, pkt.payload, packed_len);
    } else {
        printf("[WARN] protocol_pack failed with %d\n", packed_len);
    }
}

static void send_one_alarm(uint64_t now) {
    if (reliable_is_busy(&g_reliable)) {
        printf("[ALARM] Previous critical send still in progress, skipping.\n");
        return;
    }

    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = MSG_ALARM;
    pkt.node_id = g_node_id;
    pkt.sequence = seq_counter++;
    pkt.timestamp_ms = uptime_ms();

    const char *json = "{\"alarm\":\"gas_detected\"}";
    pkt.payload_len = (uint16_t)strlen(json);
    memcpy(pkt.payload, json, pkt.payload_len);

    printf("[ALARM] Sending sequence=%u with ACK/retry...\n", pkt.sequence);
    if (!reliable_send_critical(&g_reliable, &pkt, now)) {
        printf("[WARN] Failed to start ALARM delivery.\n");
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options] [broker_host]\n"
            "  --node-id N              node id to report (default %d)\n"
            "  --loss-percent P         drop P%% of outgoing packets\n"
            "  --duplicate-percent P    duplicate P%% of outgoing packets\n"
            "  --send-alarm             send one ALARM with ACK/retry shortly after start\n",
            prog, DEFAULT_NODE_ID);
}

int main(int argc, char **argv) {
    const char *broker_host = "127.0.0.1";
    bool send_alarm_once = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
            int id = atoi(argv[++i]);
            if (id < 1 || id > 65535) {
                fprintf(stderr, "--node-id must be in 1..65535\n");
                return 2;
            }
            g_node_id = (uint16_t)id;
        } else if (strcmp(argv[i], "--loss-percent") == 0 && i + 1 < argc) {
            g_loss_percent = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duplicate-percent") == 0 && i + 1 < argc) {
            g_duplicate_percent = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--send-alarm") == 0) {
            send_alarm_once = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || argv[i][0] == '-') {
            usage(argv[0]);
            return argv[i][1] == 'h' || strcmp(argv[i], "--help") == 0 ? 0 : 2;
        } else {
            broker_host = argv[i];
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    g_boot_ms = now_ms();
    srand((unsigned)time(NULL) ^ g_node_id);
    mosquitto_lib_init();

    char client_id[32];
    snprintf(client_id, sizeof(client_id), "sim-node-%u", g_node_id);
    struct mosquitto *mosq = mosquitto_new(client_id, true, NULL);
    if (!mosq) {
        fprintf(stderr, "Failed to create MQTT client\n");
        return 1;
    }

    reliable_init(&g_reliable, sim_reliable_send, mosq);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_message_callback_set(mosq, on_downlink_message);

    if (mosquitto_connect(mosq, broker_host, BROKER_PORT, 60) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Cannot connect to broker %s:%d (is mosquitto running?), retrying every second\n",
                broker_host, BROKER_PORT);
    }

    printf("sim_node started: node_id=%u, broker=%s:%d, telemetry every %d ms, loss=%d%%, duplicate=%d%%%s\n",
           g_node_id, broker_host, BROKER_PORT, TELEMETRY_INTERVAL_MS, g_loss_percent,
           g_duplicate_percent, send_alarm_once ? ", one ALARM after start" : "");

    uint64_t last_telemetry_ms = 0;
    uint64_t start_ms = now_ms();
    uint64_t last_reconnect_ms = 0;
    bool alarm_pending = send_alarm_once;

    while (1) {
        uint64_t now = now_ms();

        if (last_telemetry_ms == 0 || now - last_telemetry_ms >= TELEMETRY_INTERVAL_MS) {
            last_telemetry_ms = now;
            send_one_telemetry(mosq);
        }

        if (alarm_pending && g_connected && now - start_ms > 1500) {
            alarm_pending = false;
            send_one_alarm(now);
        }

        ReliableStatus st = reliable_tick(&g_reliable, now);
        if (st == RELIABLE_SUCCESS) {
            printf("[ALARM] seq=%u delivered, attempts=%d\n", g_reliable.sequence, g_reliable.attempts);
        } else if (st == RELIABLE_EXHAUSTED) {
            printf("[ALARM] seq=%u: retries exhausted, NOT delivered (attempts=%d)\n",
                   g_reliable.sequence, g_reliable.attempts);
        }

        int lrc = mosquitto_loop(mosq, 100, 1);
        if (lrc != MOSQ_ERR_SUCCESS) {
            g_connected = false;
            if (now - last_reconnect_ms >= 1000) {
                last_reconnect_ms = now;
                mosquitto_reconnect(mosq);
            }
            usleep(100000);
        }
    }

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}
