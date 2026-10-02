#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <glob.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mosquitto.h>
#include <cjson/cJSON.h>
#include "protocol.h"

#define MAX_NODES 10
#define MAX_SEEN_ALARMS 16
#define SEQ_WINDOW 64
#define NODE_TIMEOUT_MS 15000
#define LOG_FILE_PATH "gateway_log.txt"

#define UDP_PORT      5005
#define UDP_MAX_PER_POLL 32
#define TCP_PORT      5006
#define UART_FRAME_GAP_MS 100
#define UART_BAUD     B115200

#define FRAME_MAX_SIZE (HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE)

typedef enum {
    ROUTE_MQTT,
    ROUTE_UDP,
    ROUTE_TCP,
    ROUTE_UART,
} RouteKind;

typedef struct {
    RouteKind kind;
    struct sockaddr_in udp_addr;
    int fd;
} ReplyRoute;

typedef struct {
    uint8_t buf[FRAME_MAX_SIZE];
    size_t  have;
    size_t  need;
    bool    header_done;
} FrameReader;

static void frame_reader_init(FrameReader *fr) {
    fr->have = 0;
    fr->need = HEADER_SIZE;
    fr->header_done = false;
}

static bool frame_prefix_plausible(const FrameReader *fr) {
    if (fr->have >= 1 && fr->buf[0] != PROTOCOL_VERSION) return false;
    if (fr->have >= 2 && fr->buf[1] > MSG_ACK) return false;
    if (fr->have >= HEADER_SIZE) {
        uint16_t payload_len;
        memcpy(&payload_len, fr->buf + 16, 2);
        if (payload_len > MAX_PAYLOAD_SIZE) return false;
    }
    return true;
}

static bool frame_reader_feed_byte(FrameReader *fr, uint8_t byte) {
    if (fr->have >= sizeof(fr->buf)) {
        frame_reader_init(fr);
    }
    fr->buf[fr->have++] = byte;

    while (fr->have > 0 && !frame_prefix_plausible(fr)) {
        memmove(fr->buf, fr->buf + 1, fr->have - 1);
        fr->have--;
    }
    if (fr->have >= HEADER_SIZE) {
        uint16_t payload_len;
        memcpy(&payload_len, fr->buf + 16, 2);
        fr->need = HEADER_SIZE + payload_len + CRC_SIZE;
        fr->header_done = true;
    } else {
        fr->header_done = false;
        fr->need = HEADER_SIZE;
    }
    return fr->header_done && fr->have == fr->need;
}

#define STATE_TOPIC "telemetry/gateway/state"

#define TELEMETRY_TOPIC "telemetry/gateway/telemetry"
#define DOWNLINK_PREFIX "telemetry/downlink/"
#define UPLINK_TOPIC "telemetry/uplink"

typedef struct {
    uint16_t node_id;
    int32_t  max_seq_seen;
    uint32_t received_count;
    uint32_t lost_count;
    uint32_t duplicate_count;
    uint64_t last_seen_ms;
    bool     was_online;

    uint32_t recent_alarms[MAX_SEEN_ALARMS];
    int alarm_idx;
    int alarm_filled;

    uint64_t seen_mask;

    RouteKind last_transport;
    ReplyRoute last_route;

    uint64_t last_ts_ms;
    int64_t  clock_offset_ms;
    bool     clock_valid;
    uint8_t  prov_acks;
    uint32_t prov_seq_wifi;
    uint32_t prov_seq_gw;
    uint64_t prov_last_ms;

    uint32_t ping_seq;
    uint64_t ping_sent_ms;
    uint64_t ping_next_ms;
    double   rtt_ms;
    uint64_t rtt_at_ms;
    int32_t  backlog;
    uint32_t buffer_dropped;
} NodeState;

NodeState nodes[MAX_NODES];
int node_count = 0;
struct mosquitto *mosq_global = NULL;

int g_udp_fd = -1;
int g_tcp_listen_fd = -1;
#define MAX_TCP_CLIENTS 4
#define TCP_IDLE_TIMEOUT_MS 20000
typedef struct {
    int fd;
    FrameReader reader;
    uint64_t last_rx_ms;
} TcpClient;
TcpClient g_tcp_clients[MAX_TCP_CLIENTS];
#define MAX_UART_PORTS 4
#define UART_PROBE_WARN_MS 15000
typedef struct {
    int fd;
    char path[256];
    FrameReader reader;
    uint64_t last_rx_ms;
    uint64_t opened_ms;
    bool responded;
    bool warned_silent;
} UartPort;
UartPort g_uart_ports[MAX_UART_PORTS];

static void uart_init(void) {
    for (int i = 0; i < MAX_UART_PORTS; i++) g_uart_ports[i].fd = -1;
}

static UartPort *uart_port_by_fd(int fd) {
    if (fd < 0) return NULL;
    for (int i = 0; i < MAX_UART_PORTS; i++)
        if (g_uart_ports[i].fd == fd) return &g_uart_ports[i];
    return NULL;
}

const char *route_kind_name(RouteKind k) {
    switch (k) {
        case ROUTE_MQTT: return "MQTT";
        case ROUTE_UDP:  return "UDP";
        case ROUTE_TCP:  return "TCP";
        case ROUTE_UART: return "UART";
    }
    return "?";
}

FILE *g_log_file = NULL;
uint32_t corrupted_count = 0;

uint64_t get_monotonic_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

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

NodeState* find_or_create_node(uint16_t node_id) {
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].node_id == node_id) return &nodes[i];
    }
    if (node_count < MAX_NODES) {
        NodeState *new_node = &nodes[node_count];
        memset(new_node, 0, sizeof(NodeState));
        new_node->node_id = node_id;
        new_node->max_seq_seen = -1;
        new_node->backlog = -1;
        new_node->was_online = true;
        node_count++;
        log_event("[STATUS] New node %u registered\n", node_id);
        return new_node;
    }
    static int last_rejected_id = -1;
    if (last_rejected_id != node_id) {
        last_rejected_id = node_id;
        log_event("[WARN] Node table full (MAX_NODES=%d), ignoring node %u\n",
                   MAX_NODES, node_id);
    }
    return NULL;
}

bool is_duplicate_alarm(NodeState *node, uint32_t seq) {
    for (int i = 0; i < node->alarm_filled; i++) {
        if (node->recent_alarms[i] == seq) return true;
    }
    return false;
}

void record_alarm(NodeState *node, uint32_t seq) {
    node->recent_alarms[node->alarm_idx] = seq;
    node->alarm_idx = (node->alarm_idx + 1) % MAX_SEEN_ALARMS;
    if (node->alarm_filled < MAX_SEEN_ALARMS) node->alarm_filled++;
}

void route_reply(const ReplyRoute *route, const uint8_t *data, int len) {
    switch (route->kind) {
        case ROUTE_UDP:
            sendto(g_udp_fd, data, len, 0,
                   (const struct sockaddr*)&route->udp_addr, sizeof(route->udp_addr));
            break;
        case ROUTE_TCP:
            send(route->fd, data, len, 0);
            break;
        case ROUTE_UART:
            write(route->fd, data, len);
            break;
        case ROUTE_MQTT:
            break;
    }
}

#define CONTROL_TOPIC "telemetry/gateway/control"
#define IMP_QUEUE 64

typedef struct {
    char     profile[24];
    int      loss;
    int      dup;
    int      corrupt;
    int      delay_ms;
    int      jitter_ms;
    uint16_t node;
    uint64_t blackout_until_ms;
    uint32_t rng;
    uint32_t dropped_up, dropped_down, duplicated, n_corrupted, delayed;
} Impair;
static Impair g_imp = { .profile = "good", .rng = 1 };

typedef struct {
    bool        used;
    bool        up;
    uint64_t    due_ms;
    uint8_t     buf[FRAME_MAX_SIZE];
    int         len;
    ReplyRoute  route;
    uint16_t    node_id;
} ImpItem;
static ImpItem g_imp_q[IMP_QUEUE];

void handle_packet_now(const uint8_t *raw, size_t raw_len, const ReplyRoute *route);
static bool tcp_fd_active(int fd);

static uint32_t imp_rand(void) {
    uint32_t x = g_imp.rng ? g_imp.rng : 1;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_imp.rng = x;
    return x;
}
static bool imp_chance(int pct) { return pct > 0 && (int)(imp_rand() % 100) < pct; }
static bool imp_blackout(uint64_t now) { return now < g_imp.blackout_until_ms; }
static bool imp_enabled(uint64_t now) {
    return g_imp.loss || g_imp.dup || g_imp.corrupt || g_imp.delay_ms || g_imp.jitter_ms || imp_blackout(now);
}
static bool imp_applies(uint16_t node_id) { return g_imp.node == 0 || g_imp.node == node_id; }
static bool imp_blackout_all(uint64_t now) { return imp_blackout(now) && g_imp.node == 0; }
static uint64_t imp_delay(void) {
    return (uint64_t)g_imp.delay_ms + (g_imp.jitter_ms > 0 ? imp_rand() % (uint32_t)(g_imp.jitter_ms + 1) : 0);
}

static bool imp_enqueue(bool up, const ReplyRoute *route, uint16_t node_id, const uint8_t *data, int len, uint64_t due) {
    if (len <= 0 || len > (int)sizeof(g_imp_q[0].buf)) return false;
    for (int i = 0; i < IMP_QUEUE; i++) {
        if (g_imp_q[i].used) continue;
        g_imp_q[i].used = true;
        g_imp_q[i].up = up;
        g_imp_q[i].due_ms = due;
        g_imp_q[i].len = len;
        g_imp_q[i].route = *route;
        g_imp_q[i].node_id = node_id;
        memcpy(g_imp_q[i].buf, data, (size_t)len);
        return true;
    }
    return false;
}

static void downlink_emit(const ReplyRoute *route, uint16_t node_id, const uint8_t *data, int len) {
    if (route->kind == ROUTE_MQTT) {
        char topic[64];
        snprintf(topic, sizeof(topic), DOWNLINK_PREFIX "%u", node_id);
        mosquitto_publish(mosq_global, NULL, topic, len, data, 0, false);
    } else {
        route_reply(route, data, len);
    }
}

static void downlink_send(const ReplyRoute *route, uint16_t node_id, const uint8_t *data, int len) {
    uint64_t now = get_monotonic_time_ms();
    if (imp_enabled(now) && imp_applies(node_id)) {
        if (imp_blackout(now) || imp_chance(g_imp.loss)) { g_imp.dropped_down++; return; }
        uint64_t d = imp_delay();
        if (d > 0 && imp_enqueue(false, route, node_id, data, len, now + d)) { g_imp.delayed++; return; }
    }
    downlink_emit(route, node_id, data, len);
}

void handle_packet(const uint8_t *raw, size_t raw_len, const ReplyRoute *route) {
    uint64_t now = get_monotonic_time_ms();
    uint16_t nid = 0;
    if (raw_len >= HEADER_SIZE) memcpy(&nid, raw + 2, 2);
    if (raw_len < HEADER_SIZE || raw_len > FRAME_MAX_SIZE || !imp_enabled(now) || !imp_applies(nid)) {
        handle_packet_now(raw, raw_len, route);
        return;
    }
    if (imp_blackout(now) || imp_chance(g_imp.loss)) { g_imp.dropped_up++; return; }

    uint8_t copy[FRAME_MAX_SIZE];
    memcpy(copy, raw, raw_len);
    if (imp_chance(g_imp.corrupt)) {
        copy[imp_rand() % raw_len] ^= (uint8_t)(1 + imp_rand() % 255);
        g_imp.n_corrupted++;
    }
    int copies = 1;
    if (imp_chance(g_imp.dup)) { copies = 2; g_imp.duplicated++; }
    for (int c = 0; c < copies; c++) {
        uint64_t d = imp_delay();
        if (d > 0 && imp_enqueue(true, route, nid, copy, (int)raw_len, now + d)) g_imp.delayed++;
        else handle_packet_now(copy, raw_len, route);
    }
}

static void impair_tick(uint64_t now) {
    for (int i = 0; i < IMP_QUEUE; i++) {
        if (!g_imp_q[i].used || g_imp_q[i].due_ms > now) continue;
        ImpItem it = g_imp_q[i];
        g_imp_q[i].used = false;
        if (it.route.kind == ROUTE_TCP && !tcp_fd_active(it.route.fd)) continue;
        if (it.route.kind == ROUTE_UART && !uart_port_by_fd(it.route.fd)) continue;
        if (it.up) handle_packet_now(it.buf, (size_t)it.len, &it.route);
        else downlink_emit(&it.route, it.node_id, it.buf, it.len);
    }
}

static void impair_reset(void) {
    uint32_t seed = g_imp.rng ? g_imp.rng : 1;
    memset(&g_imp, 0, sizeof(g_imp));
    snprintf(g_imp.profile, sizeof(g_imp.profile), "good");
    g_imp.rng = seed;
    memset(g_imp_q, 0, sizeof(g_imp_q));
}

static bool impair_set_profile(const char *name) {
    int loss = 0, dup = 0, corrupt = 0, delay = 0, jitter = 0;
    if (strcmp(name, "good") == 0) {
    } else if (strcmp(name, "lossy20") == 0) {
        loss = 20;
    } else if (strcmp(name, "delay") == 0) {
        delay = 200; jitter = 100;
    } else if (strcmp(name, "flaky") == 0) {
        loss = 10; dup = 10; corrupt = 5; delay = 100; jitter = 300;
    } else {
        return false;
    }
    uint32_t seed = g_imp.rng;
    uint16_t node = g_imp.node;
    impair_reset();
    g_imp.rng = seed;
    g_imp.node = node;
    snprintf(g_imp.profile, sizeof(g_imp.profile), "%s", name);
    g_imp.loss = loss; g_imp.dup = dup; g_imp.corrupt = corrupt;
    g_imp.delay_ms = delay; g_imp.jitter_ms = jitter;
    return true;
}

static int imp_json_int(cJSON *j, const char *key, int lo, int hi, int *out) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v)) return 0;
    double d = v->valuedouble;
    *out = (int)(d < lo ? lo : d > hi ? hi : d);
    return 1;
}

static void handle_control(const char *payload, int len) {
    char *js = malloc((size_t)len + 1);
    if (!js) return;
    memcpy(js, payload, (size_t)len);
    js[len] = '\0';
    cJSON *j = cJSON_Parse(js);
    free(js);
    cJSON *cmd = cJSON_IsObject(j) ? cJSON_GetObjectItemCaseSensitive(j, "cmd") : NULL;
    if (!cJSON_IsString(cmd) || strcmp(cmd->valuestring, "impair") != 0) {
        cJSON_Delete(j);
        return;
    }
    int v;
    cJSON *seed = cJSON_GetObjectItemCaseSensitive(j, "seed");
    if (cJSON_IsNumber(seed) && seed->valuedouble >= 1 && seed->valuedouble <= 4294967295.0)
        g_imp.rng = (uint32_t)seed->valuedouble;
    if (imp_json_int(j, "node", 0, 65535, &v)) g_imp.node = (uint16_t)v;
    cJSON *prof = cJSON_GetObjectItemCaseSensitive(j, "profile");
    if (cJSON_IsString(prof) && !impair_set_profile(prof->valuestring)) {
        log_event("[IMPAIR] Unknown profile \"%s\"\n", prof->valuestring);
        cJSON_Delete(j);
        return;
    }
    bool custom = false;
    if (imp_json_int(j, "loss", 0, 100, &v))      { g_imp.loss = v; custom = true; }
    if (imp_json_int(j, "dup", 0, 100, &v))       { g_imp.dup = v; custom = true; }
    if (imp_json_int(j, "corrupt", 0, 100, &v))   { g_imp.corrupt = v; custom = true; }
    if (imp_json_int(j, "delay_ms", 0, 5000, &v)) { g_imp.delay_ms = v; custom = true; }
    if (imp_json_int(j, "jitter_ms", 0, 5000, &v)){ g_imp.jitter_ms = v; custom = true; }
    if (custom && !cJSON_IsString(prof)) snprintf(g_imp.profile, sizeof(g_imp.profile), "custom");
    if (imp_json_int(j, "blackout_s", 0, 3600, &v)) {
        g_imp.blackout_until_ms = v > 0 ? get_monotonic_time_ms() + (uint64_t)v * 1000 : 0;
        if (v > 0)
            log_event("[IMPAIR] Link BLACKOUT for %d s (nodes: %s)\n", v, g_imp.node ? "selected" : "all");
        else
            log_event("[IMPAIR] Blackout cancelled\n");
    }
    log_event("[IMPAIR] profile=%s loss=%d%% dup=%d%% corrupt=%d%% delay=%d+0..%d ms node=%u\n",
               g_imp.profile, g_imp.loss, g_imp.dup, g_imp.corrupt, g_imp.delay_ms, g_imp.jitter_ms, g_imp.node);
    cJSON_Delete(j);
}

void send_ack_via(uint16_t node_id, uint32_t seq, const ReplyRoute *route) {
    SensorPacket ack_pkt = {0};
    ack_pkt.version = PROTOCOL_VERSION;
    ack_pkt.msg_type = MSG_ACK;
    ack_pkt.node_id = node_id;
    ack_pkt.sequence = seq;
    ack_pkt.timestamp_ms = get_monotonic_time_ms();
    ack_pkt.payload_len = 0;

    uint8_t tx_buf[256];
    int len = protocol_pack(&ack_pkt, tx_buf, sizeof(tx_buf));
    if (len <= 0) return;

    downlink_send(route, node_id, tx_buf, len);
}

#define CONF_FILE_PATH "gateway.conf"
#define PROVISION_RETRY_MS 3000
#define UART_PING_MS 1000

typedef struct {
    char ssid[64];
    char pass[96];
    char ip[16];
    int  udp_port;
    int  tcp_port;
} ProvisionConfig;

static ProvisionConfig g_prov;
static bool g_prov_ready = false;
static uint32_t g_down_seq = 1;

static uint32_t next_down_seq(void) {
    uint32_t v = g_down_seq++;
    if (g_down_seq == 0) g_down_seq = 1;
    return v;
}

static void str_trim(char *s) {
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == '\n' || s[l-1] == '\r' || s[l-1] == ' ' || s[l-1] == '\t')) s[--l] = '\0';
    size_t lead = strspn(s, " \t");
    if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
}

static int iface_score(const char *name) {
    if (!strncmp(name, "lo", 2) || !strncmp(name, "docker", 6) ||
        !strncmp(name, "br-", 3) || !strncmp(name, "veth", 4)) return 0;
    if (!strncmp(name, "wl", 2)) return 3;
    if (!strncmp(name, "eth", 3) || !strncmp(name, "en", 2)) return 2;
    return 1;
}

static bool detect_own_ip(char *out, size_t n) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return false;
    int best = 0;
    for (struct ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        int sc = iface_score(a->ifa_name);
        if (sc > best) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, buf, sizeof(buf));
            snprintf(out, n, "%s", buf);
            best = sc;
        }
    }
    freeifaddrs(list);
    return best > 0;
}

static bool run_first_line(const char *cmd, char *out, size_t n) {
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    pclose(f);
    if (!ok) return false;
    str_trim(out);
    return out[0] != '\0';
}

static bool detect_wifi_ssid(char *out, size_t n) {
    FILE *f = popen("nmcli -t -f ACTIVE,SSID dev wifi 2>/dev/null", "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "yes:", 4) != 0) continue;
        size_t o = 0;
        for (const char *p = line + 4; *p && *p != '\n' && o + 1 < n; p++) {
            if (*p == '\\' && (p[1] == ':' || p[1] == '\\')) p++;
            out[o++] = *p;
        }
        out[o] = '\0';
        found = out[0] != '\0';
        break;
    }
    pclose(f);
    return found;
}

static bool detect_wifi_conn_name(char *out, size_t n) {
    FILE *f = popen("nmcli -t -f NAME,TYPE connection show --active 2>/dev/null", "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        str_trim(line);
        char *colon = strrchr(line, ':');
        if (!colon || strcmp(colon + 1, "802-11-wireless") != 0) continue;
        *colon = '\0';
        size_t o = 0;
        for (const char *p = line; *p && o + 1 < n; p++) {
            if (*p == '\\' && (p[1] == ':' || p[1] == '\\')) p++;
            out[o++] = *p;
        }
        out[o] = '\0';
        found = out[0] != '\0';
        break;
    }
    pclose(f);
    return found;
}

static bool detect_wifi_pass(const char *ssid, char *out, size_t n) {
    (void)ssid;
    char conn[128];
    if (!detect_wifi_conn_name(conn, sizeof(conn))) return false;
    if (strchr(conn, '\'') || strchr(conn, '\\')) return false;
    char cmd[320];
    snprintf(cmd, sizeof(cmd),
             "nmcli -s -g 802-11-wireless-security.psk connection show '%s' 2>/dev/null", conn);
    if (run_first_line(cmd, out, n)) return true;
    snprintf(cmd, sizeof(cmd),
             "sudo -n nmcli -s -g 802-11-wireless-security.psk connection show '%s' 2>/dev/null", conn);
    return run_first_line(cmd, out, n);
}

#define MAX_KNOWN_NETWORKS 8

static void build_provision_config(ProvisionConfig *out, bool *conf_other_network) {
    struct { char ssid[64]; char pass[96]; } nets[MAX_KNOWN_NETWORKS];
    int net_count = 0;
    char conf_ip[16] = "";
    memset(nets, 0, sizeof(nets));

    memset(out, 0, sizeof(*out));
    out->udp_port = UDP_PORT;
    out->tcp_port = TCP_PORT;
    *conf_other_network = false;

    FILE *f = fopen(CONF_FILE_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = line, *val = eq + 1;
            str_trim(key); str_trim(val);
            if (!strcmp(key, "wifi_ssid")) {
                if (net_count < MAX_KNOWN_NETWORKS) {
                    snprintf(nets[net_count].ssid, sizeof(nets[0].ssid), "%s", val);
                    net_count++;
                }
            } else if (!strcmp(key, "wifi_pass")) {
                if (net_count > 0) snprintf(nets[net_count-1].pass, sizeof(nets[0].pass), "%s", val);
            }
            else if (!strcmp(key, "gateway_ip")) snprintf(conf_ip, sizeof(conf_ip), "%s", val);
            else if (!strcmp(key, "udp_port"))   out->udp_port = atoi(val);
            else if (!strcmp(key, "tcp_port"))   out->tcp_port = atoi(val);
        }
        fclose(f);
    }

    if (!detect_own_ip(out->ip, sizeof(out->ip))) snprintf(out->ip, sizeof(out->ip), "%s", conf_ip);

    char active[64] = "";
    if (detect_wifi_ssid(active, sizeof(active))) {
        snprintf(out->ssid, sizeof(out->ssid), "%s", active);
        bool matched = false;
        for (int i = 0; i < net_count; i++) {
            if (strcmp(nets[i].ssid, active) == 0) {
                snprintf(out->pass, sizeof(out->pass), "%s", nets[i].pass);
                matched = true;
                break;
            }
        }
        if (!out->pass[0]) detect_wifi_pass(active, out->pass, sizeof(out->pass));
        if (!out->pass[0] && !matched && net_count > 0) *conf_other_network = true;
    } else if (net_count > 0) {
        snprintf(out->ssid, sizeof(out->ssid), "%s", nets[0].ssid);
        snprintf(out->pass, sizeof(out->pass), "%s", nets[0].pass);
    }
}

static bool prov_config_equal(const ProvisionConfig *a, const ProvisionConfig *b) {
    return strcmp(a->ssid, b->ssid) == 0 && strcmp(a->pass, b->pass) == 0 &&
           strcmp(a->ip, b->ip) == 0 && a->udp_port == b->udp_port && a->tcp_port == b->tcp_port;
}

static void log_provision_config(bool conf_other_network) {
    if (g_prov_ready) {
        log_event("[PROVISION] Nodes on UART will receive: gateway_ip=%s, UDP=%d, TCP=%d, Wi-Fi \"%s\", password %s\n",
                   g_prov.ip, g_prov.udp_port, g_prov.tcp_port, g_prov.ssid,
                   g_prov.pass[0] ? "set" : "NOT SET");
        if (conf_other_network)
            log_event("[PROVISION] Host is on network \"%s\", which is not listed in %s, and the system did not provide its password -- "
                       "add a wifi_ssid/wifi_pass pair for this network to %s\n", g_prov.ssid, CONF_FILE_PATH, CONF_FILE_PATH);
        else if (!g_prov.pass[0])
            log_event("[PROVISION] Wi-Fi password unknown -- set wifi_pass in %s unless the network is open\n", CONF_FILE_PATH);
    } else {
        log_event("[PROVISION] Could not detect Wi-Fi/IP -- node provisioning disabled. "
                   "Create %s (see gateway.conf.example)\n", CONF_FILE_PATH);
    }
}

static void provision_reset(NodeState *n) {
    n->prov_acks = 0;
    n->prov_seq_wifi = 0;
    n->prov_seq_gw = 0;
    n->prov_last_ms = 0;
}

static pthread_mutex_t g_prov_mtx = PTHREAD_MUTEX_INITIALIZER;
static ProvisionConfig g_prov_pending;
static bool g_prov_pending_other = false;
static bool g_prov_pending_valid = false;

static void *prov_worker(void *arg) {
    (void)arg;
    for (;;) {
        ProvisionConfig fresh;
        bool other = false;
        build_provision_config(&fresh, &other);
        pthread_mutex_lock(&g_prov_mtx);
        g_prov_pending = fresh;
        g_prov_pending_other = other;
        g_prov_pending_valid = true;
        pthread_mutex_unlock(&g_prov_mtx);
        sleep(10);
    }
    return NULL;
}

static void provision_start_worker(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, prov_worker, NULL) == 0) pthread_detach(t);
    else log_event("[PROVISION] Failed to start the network detection thread\n");
}

static void provision_refresh(uint64_t now) {
    (void)now;
    ProvisionConfig fresh;
    bool other = false, have = false;
    pthread_mutex_lock(&g_prov_mtx);
    if (g_prov_pending_valid) {
        fresh = g_prov_pending;
        other = g_prov_pending_other;
        g_prov_pending_valid = false;
        have = true;
    }
    pthread_mutex_unlock(&g_prov_mtx);
    if (!have) return;

    static bool first = true;
    if (!first && prov_config_equal(&fresh, &g_prov)) return;

    g_prov = fresh;
    g_prov_ready = g_prov.ssid[0] && g_prov.ip[0];
    if (!first) log_event("[PROVISION] Host network changed -- updating node settings\n");
    first = false;
    log_provision_config(other);
    for (int i = 0; i < node_count; i++) provision_reset(&nodes[i]);
}

static bool uart_write_frame(int fd, uint8_t type, uint16_t node_id, uint32_t seq, const char *payload_json) {
    if (fd < 0) return false;
    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = type;
    pkt.node_id = node_id;
    pkt.sequence = seq;
    pkt.timestamp_ms = get_monotonic_time_ms();
    if (payload_json) {
        size_t n = strlen(payload_json);
        if (n > MAX_PAYLOAD_SIZE) {
            log_event("[PROVISION] Payload of %zu bytes exceeds %d -- not sent (SSID/password too long?)\n", n, MAX_PAYLOAD_SIZE);
            return false;
        }
        memcpy(pkt.payload, payload_json, n);
        pkt.payload_len = (uint16_t)n;
    }
    uint8_t tx[256];
    int len = protocol_pack(&pkt, tx, sizeof(tx));
    if (len <= 0) return false;
    return write(fd, tx, len) == len;
}

static void provision_send(NodeState *n) {
    if (!(n->prov_acks & 1)) {
        if (n->prov_seq_wifi == 0) n->prov_seq_wifi = next_down_seq();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "cmd", "wifi");
        cJSON_AddStringToObject(o, "s", g_prov.ssid);
        cJSON_AddStringToObject(o, "p", g_prov.pass);
        char *js = cJSON_PrintUnformatted(o);
        if (js) { uart_write_frame(n->last_route.fd, MSG_CONFIG, n->node_id, n->prov_seq_wifi, js); free(js); }
        cJSON_Delete(o);
    }
    if (!(n->prov_acks & 2)) {
        if (n->prov_seq_gw == 0) n->prov_seq_gw = next_down_seq();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "cmd", "gw");
        cJSON_AddStringToObject(o, "ip", g_prov.ip);
        cJSON_AddNumberToObject(o, "udp", g_prov.udp_port);
        cJSON_AddNumberToObject(o, "tcp", g_prov.tcp_port);
        char *js = cJSON_PrintUnformatted(o);
        if (js) { uart_write_frame(n->last_route.fd, MSG_CONFIG, n->node_id, n->prov_seq_gw, js); free(js); }
        cJSON_Delete(o);
    }
}

static void provision_on_ack(const SensorPacket *ack) {
    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        if (n->node_id != ack->node_id) continue;
        uint8_t before = n->prov_acks;
        if (n->prov_seq_wifi != 0 && ack->sequence == n->prov_seq_wifi) n->prov_acks |= 1;
        if (n->prov_seq_gw != 0 && ack->sequence == n->prov_seq_gw) n->prov_acks |= 2;
        if (before != 3 && n->prov_acks == 3)
            log_event("[PROVISION] Node %u accepted settings (Wi-Fi + gateway address)\n", n->node_id);
        return;
    }
}

static void uart_tick(uint64_t now) {
    static uint64_t last_ping = 0;
    if (now - last_ping >= UART_PING_MS) {
        last_ping = now;
        uint32_t seq = next_down_seq();
        if (!imp_blackout_all(now))
            for (int i = 0; i < MAX_UART_PORTS; i++)
                uart_write_frame(g_uart_ports[i].fd, MSG_HEARTBEAT, 0, seq, NULL);
    }

    provision_refresh(now);
    if (!g_prov_ready) return;
    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        if (n->last_route.kind != ROUTE_UART || n->prov_acks == 3) continue;
        if (!uart_port_by_fd(n->last_route.fd)) continue;
        if (now - n->last_seen_ms > NODE_TIMEOUT_MS) continue;
        if (n->prov_last_ms != 0 && now - n->prov_last_ms < PROVISION_RETRY_MS) continue;
        n->prov_last_ms = now ? now : 1;
        provision_send(n);
        log_event("[PROVISION] Sent settings to node %u over UART\n", n->node_id);
    }
}

#define REGISTRY_FILE "node_registry.txt"

typedef struct {
    char     mac[24];
    uint16_t id;
} RegEntry;

static RegEntry g_reg[MAX_NODES];
static int g_reg_count = 0;

static void registry_save(void) {
    FILE *f = fopen(REGISTRY_FILE, "w");
    if (!f) {
        log_event("[ID] Failed to write %s: %s\n", REGISTRY_FILE, strerror(errno));
        return;
    }
    for (int i = 0; i < g_reg_count; i++) fprintf(f, "%s %u\n", g_reg[i].mac, g_reg[i].id);
    fclose(f);
}

static void registry_load(void) {
    FILE *f = fopen(REGISTRY_FILE, "r");
    if (!f) return;
    char mac[24];
    unsigned id;
    while (g_reg_count < MAX_NODES && fscanf(f, "%23s %u", mac, &id) == 2) {
        if (id == 0 || id > 65535) continue;
        snprintf(g_reg[g_reg_count].mac, sizeof(g_reg[0].mac), "%s", mac);
        g_reg[g_reg_count].id = (uint16_t)id;
        g_reg_count++;
    }
    fclose(f);
    if (g_reg_count > 0) log_event("[ID] Loaded node registry: %d entries (%s)\n", g_reg_count, REGISTRY_FILE);
}

static bool id_in_use(uint16_t id) {
    for (int i = 0; i < g_reg_count; i++) if (g_reg[i].id == id) return true;
    for (int i = 0; i < node_count; i++) if (nodes[i].node_id == id) return true;
    return false;
}

static uint16_t registry_assign(const char *mac, bool *is_new) {
    *is_new = false;
    for (int i = 0; i < g_reg_count; i++) {
        if (strcmp(g_reg[i].mac, mac) == 0) return g_reg[i].id;
    }
    if (g_reg_count >= MAX_NODES) return 0;
    uint16_t id = 1;
    while (id_in_use(id)) id++;
    snprintf(g_reg[g_reg_count].mac, sizeof(g_reg[0].mac), "%s", mac);
    g_reg[g_reg_count].id = id;
    g_reg_count++;
    registry_save();
    *is_new = true;
    return id;
}

static void send_frame_route(const ReplyRoute *route, uint8_t type, uint16_t node_id,
                             uint32_t seq, const char *payload_json) {
    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = type;
    pkt.node_id = node_id;
    pkt.sequence = seq;
    pkt.timestamp_ms = get_monotonic_time_ms();
    size_t n = payload_json ? strlen(payload_json) : 0;
    if (n > MAX_PAYLOAD_SIZE) return;
    if (n) memcpy(pkt.payload, payload_json, n);
    pkt.payload_len = (uint16_t)n;
    uint8_t tx[256];
    int len = protocol_pack(&pkt, tx, sizeof(tx));
    if (len <= 0) return;
    downlink_send(route, node_id, tx, len);
}

static void node_seq_reset(NodeState *n) {
    n->max_seq_seen = -1;
    n->seen_mask = 0;
    n->alarm_idx = 0;
    n->alarm_filled = 0;
    n->last_ts_ms = 0;
    n->clock_valid = false;
    provision_reset(n);
}

static void handle_hello(const SensorPacket *pkt, const ReplyRoute *route) {
    if (route->kind == ROUTE_MQTT) return;
    char js[MAX_PAYLOAD_SIZE + 1];
    memcpy(js, pkt->payload, pkt->payload_len);
    js[pkt->payload_len] = '\0';

    cJSON *j = cJSON_Parse(js);
    cJSON *mac_j = cJSON_IsObject(j) ? cJSON_GetObjectItemCaseSensitive(j, "mac") : NULL;
    if (!cJSON_IsString(mac_j) || strlen(mac_j->valuestring) < 8 || strlen(mac_j->valuestring) >= sizeof(g_reg[0].mac)) {
        log_event("[ID] HELLO without a valid mac rejected (link=%s)\n", route_kind_name(route->kind));
        cJSON_Delete(j);
        return;
    }
    cJSON *had_j = cJSON_GetObjectItemCaseSensitive(j, "id");
    unsigned had = cJSON_IsNumber(had_j) ? (unsigned)had_j->valuedouble : 0;
    cJSON *probe_j = cJSON_GetObjectItemCaseSensitive(j, "probe");
    bool is_probe = cJSON_IsNumber(probe_j) && probe_j->valuedouble != 0;

    bool is_new;
    uint16_t id = registry_assign(mac_j->valuestring, &is_new);
    if (id == 0) {
        log_event("[ID] Registry full (%d boards), board %s got no id\n", MAX_NODES, mac_j->valuestring);
        cJSON_Delete(j);
        return;
    }
    if (is_new)
        log_event("[ID] New board %s (link=%s) -> node_id=%u\n", mac_j->valuestring, route_kind_name(route->kind), id);
    if (had != 0 && had != id)
        log_event("[ID] Board %s had id=%u, reassigned to %u\n", mac_j->valuestring, had, id);

    for (int i = 0; !is_probe && i < node_count; i++) {
        if (nodes[i].node_id == id && nodes[i].max_seq_seen != -1) {
            log_event("[STATUS] Node %u restarted (HELLO from %s), resetting sequence tracking\n",
                       id, mac_j->valuestring);
            node_seq_reset(&nodes[i]);
            break;
        }
    }

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "cmd", "id");
    cJSON_AddStringToObject(reply, "mac", mac_j->valuestring);
    cJSON_AddNumberToObject(reply, "id", id);
    char *rs = cJSON_PrintUnformatted(reply);
    if (rs) {
        send_frame_route(route, MSG_CONFIG, 0, next_down_seq(), rs);
        free(rs);
    }
    cJSON_Delete(reply);
    cJSON_Delete(j);
}

#define CLOCK_LEAK_MS 2

static int64_t sample_age_ms(NodeState *n, int64_t delta_ms) {
    if (!n->clock_valid) {
        n->clock_offset_ms = delta_ms;
        n->clock_valid = true;
        return 0;
    }
    int64_t relaxed = n->clock_offset_ms + CLOCK_LEAK_MS;
    n->clock_offset_ms = delta_ms < relaxed ? delta_ms : relaxed;
    return delta_ms - n->clock_offset_ms;
}

static void publish_telemetry(const SensorPacket *pkt, const char *payload_str, RouteKind kind, NodeState *node, int64_t age_ms) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "node_id", pkt->node_id);
    cJSON_AddNumberToObject(root, "sequence", pkt->sequence);
    cJSON_AddNumberToObject(root, "ts_ms", (double)pkt->timestamp_ms);
    cJSON_AddNumberToObject(root, "type", pkt->msg_type);
    cJSON_AddStringToObject(root, "transport", route_kind_name(kind));
    cJSON_AddNumberToObject(root, "age_ms", (double)age_ms);

    cJSON *payload = pkt->payload_len > 0 ? cJSON_Parse(payload_str) : NULL;
    if (payload) {
        if (node && cJSON_IsObject(payload) && (int32_t)pkt->sequence == node->max_seq_seen) {
            cJSON *bl = cJSON_GetObjectItemCaseSensitive(payload, "backlog");
            cJSON *dr = cJSON_GetObjectItemCaseSensitive(payload, "dropped");
            if (cJSON_IsNumber(bl) && bl->valuedouble >= 0 && bl->valuedouble < 1e6) node->backlog = (int32_t)bl->valuedouble;
            if (cJSON_IsNumber(dr) && dr->valuedouble >= 0 && dr->valuedouble < 4e9) node->buffer_dropped = (uint32_t)dr->valuedouble;
        }
        cJSON_AddItemToObject(root, "payload", payload);
    } else if (pkt->payload_len > 0) {
        cJSON_AddStringToObject(root, "payload_raw", payload_str);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        mosquitto_publish(mosq_global, NULL, TELEMETRY_TOPIC, (int)strlen(json_str), json_str, 0, false);
        free(json_str);
    }
    cJSON_Delete(root);
}

static void latency_on_ack(const SensorPacket *ack);

void handle_packet_now(const uint8_t *raw, size_t raw_len, const ReplyRoute *route) {
    SensorPacket pkt = {0};

    int status = protocol_unpack(raw, raw_len, &pkt);

    if (status == PROTO_ERR_BAD_CRC) {
        corrupted_count++;
        log_event("[ERROR] Corrupted packet (PROTO_ERR_BAD_CRC), link=%s, rejected. "
                   "Corrupted total: %u\n", route_kind_name(route->kind), corrupted_count);
        return;
    } else if (status != PROTO_OK) {
        corrupted_count++;
        log_event("[WARN] Unpack error (code %d), link=%s, rejected. "
                   "Corrupted total: %u\n", status, route_kind_name(route->kind), corrupted_count);
        return;
    }

    if (pkt.msg_type == MSG_ACK) {
        provision_on_ack(&pkt);
        latency_on_ack(&pkt);
        return;
    }

    if (pkt.msg_type == MSG_HEARTBEAT && pkt.node_id == 0) {
        handle_hello(&pkt, route);
        return;
    }
    if (pkt.node_id == 0) return;

    NodeState *node = find_or_create_node(pkt.node_id);
    if (!node) return;

    uint64_t t_now = get_monotonic_time_ms();
    if (route->kind == ROUTE_UART && node->last_seen_ms != 0 &&
        (node->last_route.kind != ROUTE_UART || node->last_route.fd != route->fd ||
         t_now - node->last_seen_ms > NODE_TIMEOUT_MS)) {
        log_event("[PROVISION] Node %u is back on UART, re-provisioning\n", pkt.node_id);
        provision_reset(node);
    }
    node->last_seen_ms = t_now;
    node->last_transport = route->kind;
    node->last_route = *route;

    bool is_seq_duplicate = false;
    bool critical = pkt.msg_type == MSG_ALARM || pkt.msg_type == MSG_CONFIG;
    bool ts_restart = !critical && node->max_seq_seen != -1 && pkt.timestamp_ms + 30000 < node->last_ts_ms;
    if (node->max_seq_seen == -1) {
        node->max_seq_seen = (int32_t)pkt.sequence;
        node->seen_mask = 1;
        node->received_count++;
        node->last_ts_ms = pkt.timestamp_ms;
    } else {
        int64_t diff = (int64_t)pkt.sequence - (int64_t)node->max_seq_seen;
        if (diff > 0) {
            node->lost_count += (uint32_t)(diff - 1);
            node->seen_mask = (diff >= SEQ_WINDOW) ? 1 : ((node->seen_mask << diff) | 1);
            node->max_seq_seen = (int32_t)pkt.sequence;
            node->received_count++;
            node->last_ts_ms = pkt.timestamp_ms;
        } else if (-diff < SEQ_WINDOW && !ts_restart) {
            uint64_t bit = 1ULL << (-diff);
            if (node->seen_mask & bit) {
                is_seq_duplicate = true;
                node->duplicate_count++;
            } else {
                node->seen_mask |= bit;
                node->received_count++;
                if (node->lost_count > 0) node->lost_count--;
                log_event("[ORDER] Node %u: seq %u arrived late (out of order)\n",
                           pkt.node_id, pkt.sequence);
            }
        } else if (critical) {
            if (is_duplicate_alarm(node, pkt.sequence)) {
                is_seq_duplicate = true;
                node->duplicate_count++;
            } else {
                node->received_count++;
                if (node->lost_count > 0) node->lost_count--;
                log_event("[ORDER] Node %u: critical seq %u arrived outside the window (late retry)\n",
                           pkt.node_id, pkt.sequence);
            }
        } else {
            log_event("[STATUS] Node %u appears to have restarted (seq %u after %d, timestamp %llu ms), resetting sequence tracking\n",
                       pkt.node_id, pkt.sequence, node->max_seq_seen, (unsigned long long)pkt.timestamp_ms);
            node_seq_reset(node);
            node->max_seq_seen = (int32_t)pkt.sequence;
            node->seen_mask = 1;
            node->received_count++;
            node->last_ts_ms = pkt.timestamp_ms;
        }
    }

    char tmp[MAX_PAYLOAD_SIZE + 1];
    memcpy(tmp, pkt.payload, pkt.payload_len);
    tmp[pkt.payload_len] = '\0';

    if (!is_seq_duplicate) {
        int64_t age_ms = sample_age_ms(node, (int64_t)t_now - (int64_t)pkt.timestamp_ms);
        publish_telemetry(&pkt, tmp, route->kind, node, age_ms);
    }

    if (pkt.msg_type == MSG_ALARM || pkt.msg_type == MSG_CONFIG) {
        if (!is_duplicate_alarm(node, pkt.sequence)) {
            log_event("[EVENT] Node %u sent ALARM/CONFIG (seq %u, link=%s) -- new event\n",
                       pkt.node_id, pkt.sequence, route_kind_name(route->kind));
            record_alarm(node, pkt.sequence);
        } else {
            log_event("[EVENT] Node %u repeated ALARM/CONFIG (seq %u, link=%s) -- "
                       "deduplicated, no second event\n",
                       pkt.node_id, pkt.sequence, route_kind_name(route->kind));
        }
        send_ack_via(pkt.node_id, pkt.sequence, route);
    }
}

static bool tcp_fd_active(int fd) {
    if (fd < 0) return false;
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (g_tcp_clients[i].fd == fd) return true;
    }
    return false;
}

void forward_downlink(const struct mosquitto_message *msg) {
    SensorPacket pkt = {0};
    if (protocol_unpack((const uint8_t*)msg->payload, (size_t)msg->payloadlen, &pkt) != PROTO_OK) return;
    if (pkt.msg_type == MSG_ACK) return;

    for (int i = 0; i < node_count; i++) {
        if (nodes[i].node_id != pkt.node_id) continue;
        ReplyRoute route = nodes[i].last_route;
        if (route.kind == ROUTE_MQTT) return;
        if (route.kind == ROUTE_UART && !uart_port_by_fd(route.fd)) route.fd = -1;
        if (route.kind == ROUTE_TCP && !tcp_fd_active(route.fd)) route.fd = -1;
        if ((route.kind == ROUTE_TCP || route.kind == ROUTE_UART) && route.fd < 0) {
            log_event("[DOWNLINK] Node %u: link %s is down, web command not delivered\n",
                       pkt.node_id, route_kind_name(route.kind));
            return;
        }
        downlink_send(&route, pkt.node_id, (const uint8_t*)msg->payload, msg->payloadlen);
        log_event("[DOWNLINK] Web command (type %u, seq %u) -> node %u via %s\n",
                   pkt.msg_type, pkt.sequence, pkt.node_id, route_kind_name(route.kind));
        return;
    }
    log_event("[DOWNLINK] Dropped command for unknown node %u\n", pkt.node_id);
}

#define MQTT_HOST "127.0.0.1"
#define MQTT_PORT 1883
#define MQTT_RETRY_MS 1000
static bool g_mqtt_connected = false;

static void on_mqtt_connect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)userdata;
    if (rc != 0) {
        log_event("[MQTT] Broker refused connection (code %d), retrying\n", rc);
        return;
    }
    mosquitto_subscribe(mosq, NULL, UPLINK_TOPIC, 0);
    mosquitto_subscribe(mosq, NULL, DOWNLINK_PREFIX "+", 0);
    mosquitto_subscribe(mosq, NULL, CONTROL_TOPIC, 0);
    g_mqtt_connected = true;
    log_event("[MQTT] Connected to broker %s:%d\n", MQTT_HOST, MQTT_PORT);
}

static void on_mqtt_disconnect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq; (void)userdata;
    if (g_mqtt_connected)
        log_event("[MQTT] Lost connection to broker (code %d), reconnecting\n", rc);
    g_mqtt_connected = false;
}

static void mqtt_maintain(uint64_t now) {
    static uint64_t last_try = 0;
    static int failures = 0;

    int rc = mosquitto_loop(mosq_global, 0, 1);
    if (rc != MOSQ_ERR_SUCCESS) g_mqtt_connected = false;
    if (g_mqtt_connected) { failures = 0; return; }

    if (last_try != 0 && now - last_try < MQTT_RETRY_MS) return;
    last_try = now ? now : 1;
    if (mosquitto_reconnect_async(mosq_global) != MOSQ_ERR_SUCCESS && ++failures == 3) {
        log_event("[MQTT] Broker %s:%d unreachable -- running without MQTT, retrying every %d s\n",
                   MQTT_HOST, MQTT_PORT, MQTT_RETRY_MS / 1000);
    }
}

#define PING_INTERVAL_MS 5000
#define RTT_STALE_MS 20000
#define RTT_EWMA_ALPHA 0.3

static void ping_tick(uint64_t now) {
    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        if (now - n->last_seen_ms >= NODE_TIMEOUT_MS) { n->ping_seq = 0; continue; }
        if (n->ping_next_ms != 0 && now < n->ping_next_ms) continue;

        ReplyRoute route = n->last_route;
        if (route.kind == ROUTE_UART && !uart_port_by_fd(route.fd)) continue;
        if (route.kind == ROUTE_TCP && !tcp_fd_active(route.fd)) continue;
        if (route.kind == ROUTE_MQTT && !g_mqtt_connected) continue;

        n->ping_next_ms = now + PING_INTERVAL_MS;
        n->ping_seq = next_down_seq();
        n->ping_sent_ms = now;
        send_frame_route(&route, MSG_CONFIG, n->node_id, n->ping_seq, "{\"cmd\":\"ping\"}");
    }
}

static void latency_on_ack(const SensorPacket *ack) {
    uint64_t now = get_monotonic_time_ms();
    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        if (n->node_id != ack->node_id || n->ping_seq == 0 || ack->sequence != n->ping_seq) continue;
        double rtt = (double)(now - n->ping_sent_ms);
        n->rtt_ms = n->rtt_at_ms == 0 ? rtt : RTT_EWMA_ALPHA * rtt + (1.0 - RTT_EWMA_ALPHA) * n->rtt_ms;
        n->rtt_at_ms = now;
        n->ping_seq = 0;
        return;
    }
}

void on_mqtt_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    (void)mosq; (void)userdata;
    if (strcmp(msg->topic, CONTROL_TOPIC) == 0) {
        handle_control((const char*)msg->payload, msg->payloadlen);
        return;
    }
    if (strncmp(msg->topic, DOWNLINK_PREFIX, strlen(DOWNLINK_PREFIX)) == 0) {
        forward_downlink(msg);
        return;
    }
    ReplyRoute route = { .kind = ROUTE_MQTT };
    handle_packet((const uint8_t*)msg->payload, (size_t)msg->payloadlen, &route);
}

void setup_udp(void) {
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) {
        fprintf(stderr, "[UDP] Failed to create socket: %s\n", strerror(errno));
        return;
    }
    int opt = 1;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(UDP_PORT);

    if (bind(g_udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[UDP] Failed to bind port %d: %s\n", UDP_PORT, strerror(errno));
        close(g_udp_fd);
        g_udp_fd = -1;
        return;
    }
    int flags = fcntl(g_udp_fd, F_GETFL, 0);
    fcntl(g_udp_fd, F_SETFL, flags | O_NONBLOCK);
    log_event("[UDP] Listening on port %d\n", UDP_PORT);
}

void poll_udp(void) {
    if (g_udp_fd < 0) return;

    uint8_t buf[FRAME_MAX_SIZE];
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);

    for (int i = 0; i < UDP_MAX_PER_POLL; i++) {
        sender_len = sizeof(sender);
        ssize_t n = recvfrom(g_udp_fd, buf, sizeof(buf), 0,
                              (struct sockaddr*)&sender, &sender_len);
        if (n <= 0) break;
        ReplyRoute route = { .kind = ROUTE_UDP, .udp_addr = sender };
        handle_packet(buf, (size_t)n, &route);
    }
}

void setup_tcp(void) {
    g_tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_listen_fd < 0) {
        fprintf(stderr, "[TCP] Failed to create socket: %s\n", strerror(errno));
        return;
    }
    int opt = 1;
    setsockopt(g_tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(TCP_PORT);

    if (bind(g_tcp_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[TCP] Failed to bind port %d: %s\n", TCP_PORT, strerror(errno));
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
        return;
    }
    if (listen(g_tcp_listen_fd, MAX_TCP_CLIENTS) < 0) {
        fprintf(stderr, "[TCP] listen() failed: %s\n", strerror(errno));
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
        return;
    }
    int flags = fcntl(g_tcp_listen_fd, F_GETFL, 0);
    fcntl(g_tcp_listen_fd, F_SETFL, flags | O_NONBLOCK);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) g_tcp_clients[i].fd = -1;
    log_event("[TCP] Listening on port %d\n", TCP_PORT);
}

static void tcp_client_close(TcpClient *c, const char *why) {
    log_event("[TCP] Client (fd=%d) disconnected: %s\n", c->fd, why);
    for (int i = 0; i < node_count; i++)
        if (nodes[i].last_route.kind == ROUTE_TCP && nodes[i].last_route.fd == c->fd)
            nodes[i].last_route.fd = -1;
    close(c->fd);
    c->fd = -1;
}

void poll_tcp(void) {
    if (g_tcp_listen_fd < 0) return;
    uint64_t now = get_monotonic_time_ms();

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(g_tcp_listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (fd < 0) break;
        int slot = -1;
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (g_tcp_clients[i].fd < 0) { slot = i; break; }
        }
        if (slot < 0) {
            log_event("[TCP] Too many clients (max %d), rejected connection from %s\n",
                       MAX_TCP_CLIENTS, inet_ntoa(client_addr.sin_addr));
            close(fd);
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        g_tcp_clients[slot].fd = fd;
        g_tcp_clients[slot].last_rx_ms = now;
        frame_reader_init(&g_tcp_clients[slot].reader);
        log_event("[TCP] Client connected from %s (fd=%d)\n", inet_ntoa(client_addr.sin_addr), fd);
    }

    for (int c = 0; c < MAX_TCP_CLIENTS; c++) {
        TcpClient *cl = &g_tcp_clients[c];
        if (cl->fd < 0) continue;

        uint8_t rx[256];
        ssize_t n = recv(cl->fd, rx, sizeof(rx), 0);
        if (n > 0) {
            cl->last_rx_ms = now;
            ReplyRoute route = { .kind = ROUTE_TCP, .fd = cl->fd };
            for (ssize_t i = 0; i < n; i++) {
                if (frame_reader_feed_byte(&cl->reader, rx[i])) {
                    handle_packet(cl->reader.buf, cl->reader.have, &route);
                    frame_reader_init(&cl->reader);
                }
            }
        } else if (n == 0) {
            tcp_client_close(cl, "closed by peer");
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            tcp_client_close(cl, strerror(errno));
        } else if (now - cl->last_rx_ms > TCP_IDLE_TIMEOUT_MS) {
            tcp_client_close(cl, "idle timeout (connection lost without FIN)");
        }
    }
}

static int uart_open_configured(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }

    cfsetospeed(&tty, UART_BAUD);
    cfsetispeed(&tty, UART_BAUD);

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= (CREAD | CLOCAL);
    tty.c_cflag &= ~HUPCL;

    tty.c_lflag &= ~(ICANON | IEXTEN);
    tty.c_lflag &= ~(ECHO | ECHOE | ISIG);

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { close(fd); return -1; }

    int modem_bits = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &modem_bits);
    return fd;
}

static void uart_close_port(UartPort *p, const char *why) {
    log_event("[UART] %s: %s, port closed\n", p->path, why);
    for (int i = 0; i < node_count; i++)
        if (nodes[i].last_route.kind == ROUTE_UART && nodes[i].last_route.fd == p->fd)
            nodes[i].last_route.fd = -1;
    close(p->fd);
    p->fd = -1;
}

static void uart_scan(bool first) {
    const char *env = getenv("GATEWAY_UART");
    const char *defaults[] = { "/dev/ttyUSB*", "/dev/ttyACM*" };
    const char **pats = defaults;
    size_t npats = sizeof(defaults) / sizeof(defaults[0]);
    const char *one[1];
    if (env && *env) { one[0] = env; pats = one; npats = 1; }

    for (size_t i = 0; i < npats; i++) {
        glob_t g;
        if (glob(pats[i], 0, NULL, &g) != 0) continue;
        for (size_t k = 0; k < g.gl_pathc; k++) {
            char real[256];
            if (!realpath(g.gl_pathv[k], real)) continue;

            bool known = false;
            UartPort *slot = NULL;
            for (int j = 0; j < MAX_UART_PORTS; j++) {
                if (g_uart_ports[j].fd >= 0 && strcmp(g_uart_ports[j].path, real) == 0) known = true;
                if (g_uart_ports[j].fd < 0 && !slot) slot = &g_uart_ports[j];
            }
            if (known) continue;
            if (!slot) break;

            int fd = uart_open_configured(real);
            if (fd < 0) {
                if (first) fprintf(stderr, "[UART] Failed to open %s (%s)\n", real, strerror(errno));
                continue;
            }
            slot->fd = fd;
            snprintf(slot->path, sizeof(slot->path), "%s", real);
            frame_reader_init(&slot->reader);
            slot->last_rx_ms = 0;
            slot->opened_ms = get_monotonic_time_ms();
            slot->responded = false;
            slot->warned_silent = false;
            log_event("[UART] Found port %s (115200 8N1), probing for a node\n", real);
        }
        globfree(&g);
    }

    if (first) {
        bool any = false;
        for (int j = 0; j < MAX_UART_PORTS; j++) any = any || g_uart_ports[j].fd >= 0;
        if (!any)
            fprintf(stderr,
                    "[UART] No serial ports found (/dev/ttyUSB*, /dev/ttyACM*). "
                    "Rescanning every 2 s, other links keep working.\n");
    }
}

#define UART_SCAN_MS 2000

void poll_uart(void) {
    uint64_t now_ms = get_monotonic_time_ms();
    static uint64_t last_scan_ms = 0;
    static bool first_scan = true;
    if (first_scan || now_ms - last_scan_ms >= UART_SCAN_MS) {
        last_scan_ms = now_ms;
        uart_scan(first_scan);
        first_scan = false;
    }

    for (int pi = 0; pi < MAX_UART_PORTS; pi++) {
        UartPort *p = &g_uart_ports[pi];
        if (p->fd < 0) continue;

        struct pollfd pfd = { .fd = p->fd, .events = POLLIN };
        struct stat st;
        if ((poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) ||
            stat(p->path, &st) != 0) {
            uart_close_port(p, "device unplugged");
            continue;
        }

        uint8_t rx[256];
        ssize_t n = read(p->fd, rx, sizeof(rx));
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            char why[96];
            snprintf(why, sizeof(why), "port gone (%s)", strerror(errno));
            uart_close_port(p, why);
            continue;
        }
        if (n > 0) {
            if (p->reader.have > 0 && now_ms - p->last_rx_ms > UART_FRAME_GAP_MS)
                frame_reader_init(&p->reader);
            p->last_rx_ms = now_ms;
            ReplyRoute route = { .kind = ROUTE_UART, .fd = p->fd };
            for (ssize_t i = 0; i < n; i++) {
                if (frame_reader_feed_byte(&p->reader, rx[i])) {
                    SensorPacket probe;
                    if (!p->responded && protocol_unpack(p->reader.buf, p->reader.have, &probe) == PROTO_OK) {
                        p->responded = true;
                        log_event("[UART] %s: received a valid frame, node detected\n", p->path);
                    }
                    handle_packet(p->reader.buf, p->reader.have, &route);
                    frame_reader_init(&p->reader);
                }
            }
        }
        if (!p->responded && !p->warned_silent && now_ms > p->opened_ms &&
            now_ms - p->opened_ms > UART_PROBE_WARN_MS) {
            p->warned_silent = true;
            log_event("[UART] %s: no response to probes for %d s, probably not a node (port stays open)\n",
                      p->path, UART_PROBE_WARN_MS / 1000);
        }
    }
}

void print_dashboard() {
    uint64_t now = get_monotonic_time_ms();
    printf("\n--- Dashboard (nodes: %d) ---\n", node_count);
    printf("%-5s | %-7s | %-6s | %-6s | %-6s | %-6s | %-8s | %-7s | %-10s\n",
           "Node", "Status", "Link", "Recv", "Lost", "Dup", "RTT ms", "Backlog", "Max Seq");
    printf("----------------------------------------------------------------------------------\n");

    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        bool online = (now - n->last_seen_ms) < NODE_TIMEOUT_MS;

        if (online != n->was_online) {
            if (online) {
                log_event("[STATUS] Node %u is ONLINE again\n", n->node_id);
            } else {
                log_event("[STATUS] Node %u went OFFLINE (no messages for > %d ms)\n",
                           n->node_id, NODE_TIMEOUT_MS);
            }
            n->was_online = online;
        }

        double loss_rate = 0.0;
        if ((n->received_count + n->lost_count) > 0) {
            loss_rate = ((double)n->lost_count / (n->received_count + n->lost_count)) * 100.0;
        }

        char rtt_s[16] = "-", bl_s[16] = "-";
        if (n->rtt_at_ms != 0 && now - n->rtt_at_ms < RTT_STALE_MS) snprintf(rtt_s, sizeof(rtt_s), "%.0f", n->rtt_ms);
        if (n->backlog >= 0) snprintf(bl_s, sizeof(bl_s), "%d", n->backlog);
        printf("%-5u | %-7s | %-6s | %-6u | %-6u | %-6u | %-8s | %-7s | %-10d (Loss: %.1f%%)\n",
               n->node_id, online ? "ONLINE" : "OFFLINE", route_kind_name(n->last_transport),
               n->received_count, n->lost_count, n->duplicate_count,
               rtt_s, bl_s, n->max_seq_seen, loss_rate);
    }
    printf("Corrupted packets since start: %u\n", corrupted_count);
    if (imp_enabled(get_monotonic_time_ms()))
        printf("Impairment: %s | loss %d%% dup %d%% corrupt %d%% delay %d+%d ms | dropped up %u, dropped down %u, duplicated %u, corrupted %u\n",
               g_imp.profile, g_imp.loss, g_imp.dup, g_imp.corrupt, g_imp.delay_ms, g_imp.jitter_ms,
               g_imp.dropped_up, g_imp.dropped_down, g_imp.duplicated, g_imp.n_corrupted);
    printf("======================================================\n");
}

void publish_gateway_state(void) {
    uint64_t now = get_monotonic_time_ms();
    cJSON *root = cJSON_CreateObject();
    cJSON *nodes_json = cJSON_CreateArray();

    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        bool online = (now - n->last_seen_ms) < NODE_TIMEOUT_MS;

        double loss_rate = 0.0;
        if ((n->received_count + n->lost_count) > 0) {
            loss_rate = ((double)n->lost_count / (n->received_count + n->lost_count)) * 100.0;
        }

        cJSON *node_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(node_json, "node_id", n->node_id);
        cJSON_AddBoolToObject(node_json, "online", online);
        cJSON_AddStringToObject(node_json, "transport", route_kind_name(n->last_transport));
        cJSON_AddNumberToObject(node_json, "received_count", n->received_count);
        cJSON_AddNumberToObject(node_json, "lost_count", n->lost_count);
        cJSON_AddNumberToObject(node_json, "duplicate_count", n->duplicate_count);
        cJSON_AddNumberToObject(node_json, "max_seq_seen", n->max_seq_seen);
        cJSON_AddNumberToObject(node_json, "loss_rate", loss_rate);
        cJSON_AddNumberToObject(node_json, "last_seen_ms_ago", (double)(now - n->last_seen_ms));
        bool rtt_fresh = n->rtt_at_ms != 0 && now - n->rtt_at_ms < RTT_STALE_MS;
        if (rtt_fresh) cJSON_AddNumberToObject(node_json, "latency_ms", n->rtt_ms);
        else           cJSON_AddNullToObject(node_json, "latency_ms");
        if (n->backlog >= 0) cJSON_AddNumberToObject(node_json, "backlog", n->backlog);
        else                 cJSON_AddNullToObject(node_json, "backlog");
        cJSON_AddNumberToObject(node_json, "buffer_dropped", n->buffer_dropped);
        cJSON_AddItemToArray(nodes_json, node_json);
    }

    cJSON_AddItemToObject(root, "nodes", nodes_json);
    cJSON_AddNumberToObject(root, "corrupted_count", corrupted_count);

    cJSON *imp = cJSON_CreateObject();
    cJSON_AddStringToObject(imp, "profile", g_imp.profile);
    cJSON_AddBoolToObject(imp, "active", imp_enabled(now));
    cJSON_AddNumberToObject(imp, "loss", g_imp.loss);
    cJSON_AddNumberToObject(imp, "dup", g_imp.dup);
    cJSON_AddNumberToObject(imp, "corrupt", g_imp.corrupt);
    cJSON_AddNumberToObject(imp, "delay_ms", g_imp.delay_ms);
    cJSON_AddNumberToObject(imp, "jitter_ms", g_imp.jitter_ms);
    cJSON_AddNumberToObject(imp, "node", g_imp.node);
    cJSON_AddNumberToObject(imp, "blackout_left_s", imp_blackout(now) ? (double)(g_imp.blackout_until_ms - now) / 1000.0 : 0);
    cJSON_AddNumberToObject(imp, "dropped_up", g_imp.dropped_up);
    cJSON_AddNumberToObject(imp, "dropped_down", g_imp.dropped_down);
    cJSON_AddNumberToObject(imp, "duplicated", g_imp.duplicated);
    cJSON_AddNumberToObject(imp, "corrupted", g_imp.n_corrupted);
    cJSON_AddNumberToObject(imp, "delayed", g_imp.delayed);
    cJSON_AddItemToObject(root, "impairment", imp);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str != NULL) {
        mosquitto_publish(mosq_global, NULL, STATE_TOPIC, (int)strlen(json_str), json_str, 0, false);
        free(json_str);
    }
    cJSON_Delete(root);
}

int main(void) {
    g_log_file = fopen(LOG_FILE_PATH, "a");
    if (g_log_file != NULL) {
        time_t start_t = time(NULL);
        fprintf(g_log_file, "\n===== Gateway started: %s", ctime(&start_t));
        fflush(g_log_file);
        printf("Event log: %s\n", LOG_FILE_PATH);
    } else {
        fprintf(stderr, "[WARN] Cannot open %s for writing, logging to stdout only.\n",
                LOG_FILE_PATH);
    }

    signal(SIGPIPE, SIG_IGN);

    mosquitto_lib_init();
    mosq_global = mosquitto_new("GatewayClient", true, NULL);
    if (!mosq_global) {
        fprintf(stderr, "Failed to create MQTT client.\n");
        return 1;
    }
    mosquitto_connect_callback_set(mosq_global, on_mqtt_connect);
    mosquitto_disconnect_callback_set(mosq_global, on_mqtt_disconnect);
    mosquitto_message_callback_set(mosq_global, on_mqtt_message);
    mosquitto_connect_async(mosq_global, MQTT_HOST, MQTT_PORT, 60);

    setup_udp();
    setup_tcp();
    uart_init();
    g_down_seq = ((uint32_t)time(NULL) & 0x7FFFFFFF) | 1;
    registry_load();
    provision_start_worker();

    printf("Gateway started, waiting for telemetry (MQTT/UDP/TCP/UART)...\n");

    uint64_t last_dash_ms = get_monotonic_time_ms();

    while (1) {
        uint64_t now = get_monotonic_time_ms();
        mqtt_maintain(now);
        poll_udp();
        poll_tcp();
        poll_uart();

        now = get_monotonic_time_ms();
        impair_tick(now);
        uart_tick(now);
        ping_tick(now);
        if (now - last_dash_ms >= 2000) {
            print_dashboard();
            publish_gateway_state();
            last_dash_ms = now;
        }

        usleep(10000);
    }

    mosquitto_destroy(mosq_global);
    mosquitto_lib_cleanup();
    if (g_log_file != NULL) {
        fclose(g_log_file);
    }
    return 0;
}
