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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mosquitto.h>
#include <cjson/cJSON.h>
#include "protocol.h" // Підключаємо реальний протокол Блока А

#define MAX_NODES 10
#define MAX_SEEN_ALARMS 16
#define SEQ_WINDOW 64 // скільки останніх sequence пам'ятаємо для відрізнення дубліката від запізнілого пакета
#define NODE_TIMEOUT_MS 15000 // 15 с без повідомлень = вузол OFFLINE (плата шле раз на 5 с, тож 3 пропуски поспіль)
#define LOG_FILE_PATH "gateway_log.txt"

/* ==== 30.09 (3) -- три РІЗНІ транспорти для трьох фізичних плат ====
 * Мета: та сама протокольна логіка (protocol_unpack, дедуп, ACK) працює
 * НЕЗАЛЕЖНО від того, як байти прийшли -- MQTT (як і раніше, для
 * sim_node/сумісності), "сирий" UDP, "сирий" TCP, чи UART через дріт.
 * Це і є вимога кейсу "транспортний рівень замінний" -- зроблена не як
 * документ-намір, а як реальний код: чотири джерела, один
 * handle_packet(), один дашборд, один case24/gateway/state для веб. */
#define UDP_PORT      5005
#define TCP_PORT      5006
#define UART_DEVICE   "/dev/ttyUSB0"  /* USB-до-TTL перехідник; якщо інша
                                        * назва -- перевір `ls /dev/ttyUSB*`
                                        * після підключення й зміни тут. */
#define UART_FRAME_GAP_MS 100
#define UART_BAUD     B115200         /* має збігатися з Serial2.begin()
                                        * на платі, яка сидить на UART */

/* Максимальний розмір ОДНОГО кадру "на дроті": заголовок + весь можливий
 * payload + CRC. Використовується і для UDP-буфера, і для TCP/UART
 * складання кадру з потоку байтів. */
#define FRAME_MAX_SIZE (HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE)

typedef enum {
    ROUTE_MQTT,
    ROUTE_UDP,
    ROUTE_TCP,
    ROUTE_UART,
} RouteKind;

/* "Куди відповісти" -- заповнюється в момент прийому пакета (звідки б він
 * не прийшов), і використовується одразу ж для відправки ACK назад ТИМ
 * САМИМ каналом. Нічого зберігати довше не треба -- ACK завжди йде як
 * пряма відповідь на щойно отриманий критичний пакет. */
typedef struct {
    RouteKind kind;
    struct sockaddr_in udp_addr; /* дійсно лише для ROUTE_UDP */
    int fd;                      /* дійсно для ROUTE_TCP (fd клієнта) і ROUTE_UART */
} ReplyRoute;

/* Складання кадру з потоку байтів (TCP і UART -- байтовий потік, на
 * відміну від UDP, де межі датаграми й так збігаються з межами пакета).
 * Читаємо по одному байту: спершу назбируємо HEADER_SIZE байт, дістаємо
 * з них payload_len, тоді знаємо повний розмір кадру і чекаємо решту. */
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

/* Чи може fr->buf[0..have) бути початком справжнього кадру: версія протоколу,
 * відомий тип повідомлення, правдоподібний payload_len. Це дає самосинхронізацію:
 * сміття (завантажувач ESP32, підключення посеред кадру) відкидається побайтово,
 * а не з'їдає наступні справжні кадри і не рахується битим пакетом. */
static bool frame_prefix_plausible(const FrameReader *fr) {
    if (fr->have >= 1 && fr->buf[0] != PROTOCOL_VERSION) return false;
    if (fr->have >= 2 && fr->buf[1] > MSG_ACK) return false;
    if (fr->have >= HEADER_SIZE) {
        uint16_t payload_len;
        memcpy(&payload_len, fr->buf + 16, 2); /* offset payload_len у заголовку, див. protocol.h */
        if (payload_len > MAX_PAYLOAD_SIZE) return false;
    }
    return true;
}

/* Повертає true, якщо ПІСЛЯ цього байта кадр у fr->buf[0..fr->have) повний
 * і готовий для protocol_unpack(). Викликач має одразу забрати дані й
 * викликати frame_reader_init() перед наступним feed. */
static bool frame_reader_feed_byte(FrameReader *fr, uint8_t byte) {
    if (fr->have >= sizeof(fr->buf)) {
        frame_reader_init(fr); /* переповнення -- явно биті дані, скидаємо */
    }
    fr->buf[fr->have++] = byte;

    while (fr->have > 0 && !frame_prefix_plausible(fr)) {
        memmove(fr->buf, fr->buf + 1, fr->have - 1); /* зсув на байт: шукаємо початок кадру далі */
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

// Топік, куди Gateway публікує СВІЙ вже оброблений стан (Блок E, веб-
// дашборд, читає ЛИШЕ звідси, а не парсить сирий case24/uplink сам --
// інакше виходять два незалежні "шлюзи" з розбіжною статистикою, а
// контракт (00-overview-shared-contract.md) віддає трекінг/dashboard/лог
// винятково Блоку C). Публікується разом з терміналним дашбордом.
#define STATE_TOPIC "case24/gateway/state"

// Сюди Gateway публікує КОЖЕН валідний (не дубльований) пакет вузла у вигляді
// JSON, незалежно від того, яким транспортом він прийшов (UDP/TCP/UART/MQTT).
// Веб бере значення сенсорів для графіків і БД ЛИШЕ звідси -- плати на
// UDP/TCP/UART у case24/uplink взагалі не потрапляють.
#define TELEMETRY_TOPIC "case24/gateway/telemetry"
#define DOWNLINK_PREFIX "case24/downlink/"

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
    int alarm_filled; // скільки комірок recent_alarms реально заповнено (щоб порожні нулі не збігались з seq=0)

    // Біт i = "пакет з sequence (max_seq_seen - i) уже отримано". Дозволяє
    // відрізнити справжній дублікат від запізнілого (reordered) пакета.
    uint64_t seen_mask;

    RouteKind last_transport; // яким каналом прийшов ОСТАННІЙ пакет цього вузла
    ReplyRoute last_route;    // куди слати команди з веба (downlink) для UDP/TCP/UART-вузла

    uint64_t last_ts_ms;      // timestamp_ms (millis плати) найновішого пакета -- для виявлення перезапуску
    // Автоналаштування вузла по UART (Wi-Fi + адреса шлюзу)
    uint8_t  prov_acks;       // біт0 -- wifi підтверджено, біт1 -- gw підтверджено
    uint32_t prov_seq_wifi;   // sequence наших CONFIG-пакетів (0 = ще не надсилали)
    uint32_t prov_seq_gw;
    uint64_t prov_last_ms;    // коли востаннє слали (для повторів)

    // Затримка: шлюз раз на PING_INTERVAL_MS шле вузлу CONFIG {"cmd":"ping"} і міряє
    // час до ACK на СВОЄМУ монотонному годиннику (годинники плат не синхронізуємо).
    uint32_t ping_seq;        // sequence пінга, що чекає на ACK (0 = нема)
    uint64_t ping_sent_ms;
    uint64_t ping_next_ms;    // коли слати наступний (0 = одразу)
    double   rtt_ms;          // ковзне середнє RTT
    uint64_t rtt_at_ms;       // коли отримано останній зразок (0 = ще не було)
    // Стан буфера на платі (поле backlog/dropped у payload найновішої телеметрії)
    int32_t  backlog;         // -1 = вузол не повідомляє
    uint32_t buffer_dropped;
} NodeState;

NodeState nodes[MAX_NODES];
int node_count = 0;
struct mosquitto *mosq_global = NULL;

// Дескриптори трьох "сирих" транспортів (окрім MQTT, який лишається на
// mosq_global). -1 означає "не піднявся" -- гейтвей просто пропускає цей
// канал у циклі опитування, решта транспортів працюють як і раніше.
int g_udp_fd = -1;
int g_tcp_listen_fd = -1;
// Кілька TCP-клієнтів одночасно: кожна плата, що має TCP як резервний
// канал, тримає своє з'єднання. Кадр складається окремо для кожного.
#define MAX_TCP_CLIENTS 4
#define TCP_IDLE_TIMEOUT_MS 20000 // плата шле раз на 5 с; мовчання довше = з'єднання мертве
typedef struct {
    int fd;
    FrameReader reader;
    uint64_t last_rx_ms;
} TcpClient;
TcpClient g_tcp_clients[MAX_TCP_CLIENTS];
// Кілька послідовних портів одночасно: шлюз сам знаходить USB-TTL перехідники
// (/dev/ttyUSB*, /dev/ttyACM*), шле на кожен пробні пінги і запам'ятовує як
// "наш" той порт, звідки прийшов валідний кадр протоколу.
#define MAX_UART_PORTS 4
#define UART_PROBE_WARN_MS 15000 // стільки порт мовчить -- повідомляємо, що там, схоже, не наша плата
typedef struct {
    int fd;                // -1 = слот вільний
    char path[256];        // справжній шлях (після realpath) -- для дедуплікації
    FrameReader reader;
    uint64_t last_rx_ms;   // коли востаннє прийшли байти (для скидання сміття між кадрами)
    uint64_t opened_ms;
    bool responded;        // з порту вже приходив валідний кадр
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
        new_node->backlog = -1;
        new_node->was_online = true; // щойно прийшов перший пакет -- вважаємо online
        node_count++;
        log_event("[СТАТУС] Новий вузол %u зареєстрований\n", node_id);
        return new_node;
    }
    static int last_rejected_id = -1; // щоб не спамити логом на кожен пакет
    if (last_rejected_id != node_id) {
        last_rejected_id = node_id;
        log_event("[ПОПЕРЕДЖЕННЯ] Таблиця вузлів заповнена (MAX_NODES=%d), вузол %u ігнорується\n",
                   MAX_NODES, node_id);
    }
    return NULL;
}

// Перевірка на наявність дублікатів ALARM
bool is_duplicate_alarm(NodeState *node, uint32_t seq) {
    for (int i = 0; i < node->alarm_filled; i++) {
        if (node->recent_alarms[i] == seq) return true;
    }
    return false;
}

// Фіксація нового sequence для ALARM
void record_alarm(NodeState *node, uint32_t seq) {
    node->recent_alarms[node->alarm_idx] = seq;
    node->alarm_idx = (node->alarm_idx + 1) % MAX_SEEN_ALARMS;
    if (node->alarm_filled < MAX_SEEN_ALARMS) node->alarm_filled++;
}

// Надсилає сирі байти НАЗАД тим самим каналом, звідки прийшов пакет.
// MQTT сюди не заходить -- для нього потрібен топік, а не сокет/fd, це
// лишається окремо в send_ack_via() нижче.
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
            break; // оброблюється окремо нижче (потрібен топік, не fd)
    }
}

// Формування та відправка ACK -- каналом, яким прийшов оригінальний
// критичний пакет (route). Для MQTT це, як і раніше, публікація в
// case24/downlink/<node_id>; для UDP/TCP/UART -- пряма відповідь route_reply().
void send_ack_via(uint16_t node_id, uint32_t seq, const ReplyRoute *route) {
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
    if (len <= 0) return;

    if (route->kind == ROUTE_MQTT) {
        char topic[64];
        snprintf(topic, sizeof(topic), "case24/downlink/%u", node_id);
        mosquitto_publish(mosq_global, NULL, topic, len, tx_buf, 0, false);
    } else {
        route_reply(route, tx_buf, len);
    }
}

// ЄДИНА обробка вхідного пакета -- незалежно від того, MQTT це, UDP, TCP
// чи UART. Саме це і є "транспорт замінний, логіка протоколу одна", а не
// просто документ-намір: протокол/дедуп/дашборд не знають і не питають,
// яким дротом прийшли байти.
// ==== Автоналаштування вузла по UART ====
// Коли вузол вийшов на зв'язок через UART-дріт, Gateway сам передає йому
// налаштування хоста, на якому працює: Wi-Fi (SSID/пароль) і власну IP з
// портами UDP/TCP. Плата зберігає їх у пам'яті й надалі тримає Wi-Fi
// резервним каналом: коли дріт відключили, дані йдуть по Wi-Fi.
// Налаштування беруться (по пріоритету): gateway.conf -> автовизначення
// (IP з мережевих інтерфейсів, SSID/пароль активного Wi-Fi через nmcli).
// Файл gateway.conf НЕ в git: пароль Wi-Fi не повинен потрапляти в репозиторій.
#define CONF_FILE_PATH "gateway.conf"
#define PROVISION_RETRY_MS 3000
#define UART_PING_MS 1000 // як часто шлемо "дріт живий" -- по ньому плата розуміє, що Pi на іншому кінці

typedef struct {
    char ssid[64];
    char pass[96];
    char ip[16];
    int  udp_port;
    int  tcp_port;
} ProvisionConfig;

static ProvisionConfig g_prov;
static bool g_prov_ready = false;
static uint32_t g_down_seq = 1; // sequence наших пакетів до плат; 0 зарезервовано як "не надсилали"

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
    if (!strncmp(name, "wl", 2)) return 3;                      // wlan0, wlp...
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

// Виконує команду й повертає перший непорожній рядок виводу (без \n).
static bool run_first_line(const char *cmd, char *out, size_t n) {
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    pclose(f);
    if (!ok) return false;
    str_trim(out);
    return out[0] != '\0';
}

// SSID активної Wi-Fi мережі (NetworkManager). У режимі -t двокрапка й
// зворотна скісна в назві екрануються як "\:" і "\\".
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

// Ім'я активного Wi-Fi підключення NetworkManager. Воно не завжди збігається
// з SSID (профіль із Raspberry Pi Imager зветься "preconfigured").
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

// Пароль активної Wi-Fi мережі з профілю NetworkManager. Звичайному
// користувачеві паролі часто недоступні, тому пробуємо ще `sudo -n`
// (на Raspberry Pi OS sudo без пароля -- типове налаштування). Пароль іде
// лише в UART-кабель до плати, у лог і в git він не потрапляє.
static bool detect_wifi_pass(const char *ssid, char *out, size_t n) {
    (void)ssid;
    char conn[128];
    if (!detect_wifi_conn_name(conn, sizeof(conn))) return false;
    if (strchr(conn, '\'') || strchr(conn, '\\')) return false; // не складаємо shell-команду з дивних символів
    char cmd[320];
    snprintf(cmd, sizeof(cmd),
             "nmcli -s -g 802-11-wireless-security.psk connection show '%s' 2>/dev/null", conn);
    if (run_first_line(cmd, out, n)) return true;
    snprintf(cmd, sizeof(cmd),
             "sudo -n nmcli -s -g 802-11-wireless-security.psk connection show '%s' 2>/dev/null", conn);
    return run_first_line(cmd, out, n);
}

// Збирає актуальні налаштування. Мережа Pi може змінитися на ходу, тому
// ПОТОЧНІ дані (IP, активна Wi-Fi) мають пріоритет. gateway.conf може
// містити кілька мереж (пари wifi_ssid / wifi_pass): береться та, що збігається
// з активною Wi-Fi Pi. Якщо в файлі її нема -- пароль шукаємо в NetworkManager.
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
        // активну Wi-Fi визначити не вдалося (немає nmcli) -- беремо першу мережу з файлу
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
        log_event("[НАЛАШТУВАННЯ] Шлюз передаватиме вузлам по UART: gateway_ip=%s, UDP=%d, TCP=%d, Wi-Fi \"%s\", пароль %s\n",
                   g_prov.ip, g_prov.udp_port, g_prov.tcp_port, g_prov.ssid,
                   g_prov.pass[0] ? "заданий" : "НЕ ЗАДАНИЙ");
        if (conf_other_network)
            log_event("[НАЛАШТУВАННЯ] Pi зараз у мережі \"%s\", а в %s її нема і пароль із системи не отримано -- "
                       "додайте пару wifi_ssid/wifi_pass для цієї мережі в %s\n", g_prov.ssid, CONF_FILE_PATH, CONF_FILE_PATH);
        else if (!g_prov.pass[0])
            log_event("[НАЛАШТУВАННЯ] Пароль Wi-Fi не визначено -- впишіть wifi_pass у %s, якщо мережа не відкрита\n", CONF_FILE_PATH);
    } else {
        log_event("[НАЛАШТУВАННЯ] Не вдалося визначити Wi-Fi/IP -- автоналаштування вузлів вимкнено. "
                   "Створіть %s (див. gateway.conf.example)\n", CONF_FILE_PATH);
    }
}

// Скидає стан автоналаштування вузла -- наступний цикл uart_tick() надішле все знову.
static void provision_reset(NodeState *n) {
    n->prov_acks = 0;
    n->prov_seq_wifi = 0;
    n->prov_seq_gw = 0;
    n->prov_last_ms = 0;
}

// Визначення мережі (nmcli, sudo) може тривати секунди, тому воно йде у
// ФОНОВОМУ потоці раз на 10 с, щоб головний цикл не зупинявся (інакше він не
// читає порти й не шле "пінги", і плата вважає дріт мертвим). Потік лише
// будує конфіг і кладе його в g_prov_pending; логує й застосовує головний потік.
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
    else log_event("[НАЛАШТУВАННЯ] Не вдалося запустити фоновий потік визначення мережі\n");
}

// Викликається з головного циклу: забирає готовий результат фонового потоку.
// Мережа Pi змінилася -- плати на дроті отримують нові налаштування без
// перезапуску шлюзу.
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
    if (!first) log_event("[НАЛАШТУВАННЯ] Мережа Pi змінилася -- оновлюю налаштування вузлів\n");
    first = false;
    log_provision_config(other);
    for (int i = 0; i < node_count; i++) provision_reset(&nodes[i]);
}

// Пакує й пише у UART один кадр (шлюз -> плата). payload_json може бути NULL.
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
            log_event("[НАЛАШТУВАННЯ] Payload %zu байт > %d -- не надіслано (задовгий SSID/пароль?)\n", n, MAX_PAYLOAD_SIZE);
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

// Два невеликі CONFIG-пакети (разом SSID+пароль+IP не влізли б у 128 байт payload).
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
            log_event("[НАЛАШТУВАННЯ] Вузол %u прийняв налаштування (Wi-Fi + адреса шлюзу)\n", n->node_id);
        return;
    }
}

// Викликається з головного циклу: "пінг" дроту + повтори автоналаштування.
static void uart_tick(uint64_t now) {
    static uint64_t last_ping = 0;
    if (now - last_ping >= UART_PING_MS) {
        last_ping = now;
        uint32_t seq = next_down_seq();
        // Пінг іде на ВСІ відкриті порти: той, де є наша плата, відповість
        for (int i = 0; i < MAX_UART_PORTS; i++)
            uart_write_frame(g_uart_ports[i].fd, MSG_HEARTBEAT, 0, seq, NULL); // node_id 0 = усім
    }

    provision_refresh(now);
    if (!g_prov_ready) return;
    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        if (n->last_route.kind != ROUTE_UART || n->prov_acks == 3) continue;
        if (!uart_port_by_fd(n->last_route.fd)) continue; // порт, де бачили вузол, зник
        if (now - n->last_seen_ms > NODE_TIMEOUT_MS) continue; // вузол зараз не на дроті
        if (n->prov_last_ms != 0 && now - n->prov_last_ms < PROVISION_RETRY_MS) continue;
        n->prov_last_ms = now ? now : 1;
        provision_send(n);
        log_event("[НАЛАШТУВАННЯ] Надіслано налаштування вузлу %u по UART\n", n->node_id);
    }
}

// ==== Автопризначення node_id за старшинством ====
// Плата не має "своєї" node_id: вона шле HELLO (HEARTBEAT з node_id=0 і
// payload {"mac":"...","id":<збережений або 0>}). Шлюз веде реєстр
// MAC -> node_id: хто з'явився першим, той отримує найменший вільний id
// (1, 2, 3...). Реєстр зберігається у файлі, тож id не міняються після
// перезапуску шлюзу. Якщо плата мала вже прописаний id, що не збігається з
// реєстром (наприклад, такий самий, як у іншої плати), вона перезаписує його
// на призначений. Шлюз відповідає на кожен HELLO (відповідь ідемпотентна),
// плата повторює HELLO, поки не отримає id.
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
        log_event("[ID] Не вдалося записати %s: %s\n", REGISTRY_FILE, strerror(errno));
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
    if (g_reg_count > 0) log_event("[ID] Завантажено реєстр плат: %d шт. (%s)\n", g_reg_count, REGISTRY_FILE);
}

static bool id_in_use(uint16_t id) {
    for (int i = 0; i < g_reg_count; i++) if (g_reg[i].id == id) return true;
    for (int i = 0; i < node_count; i++) if (nodes[i].node_id == id) return true; // вузли без HELLO (sim_node)
    return false;
}

// Повертає id для MAC (існуючий або новий найменший вільний); 0 -- реєстр заповнений.
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

// Пакує кадр і відправляє ТИМ КАНАЛОМ, звідки прийшов запит (UART/UDP/TCP).
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
    if (route->kind == ROUTE_MQTT) {
        // MQTT-вузол слухає свій downlink-топік (потрібен node_id != 0)
        char topic[64];
        snprintf(topic, sizeof(topic), DOWNLINK_PREFIX "%u", node_id);
        mosquitto_publish(mosq_global, NULL, topic, len, tx, 0, false);
    } else {
        route_reply(route, tx, len);
    }
}

// Скидає облік sequence/дедуплікації вузла (плата перезапустилась і почала
// sequence з нуля). Лічильники received/lost/duplicate лишаються.
static void node_seq_reset(NodeState *n) {
    n->max_seq_seen = -1;
    n->seen_mask = 0;
    n->alarm_idx = 0;
    n->alarm_filled = 0;
    n->last_ts_ms = 0;
    // Після перезапуску плата могла втратити налаштування -- шлемо знову
    provision_reset(n);
}

static void handle_hello(const SensorPacket *pkt, const ReplyRoute *route) {
    if (route->kind == ROUTE_MQTT) return; // MQTT-вузли id не отримують (sim_node має свій)
    char js[MAX_PAYLOAD_SIZE + 1];
    memcpy(js, pkt->payload, pkt->payload_len);
    js[pkt->payload_len] = '\0';

    cJSON *j = cJSON_Parse(js);
    cJSON *mac_j = cJSON_IsObject(j) ? cJSON_GetObjectItemCaseSensitive(j, "mac") : NULL;
    if (!cJSON_IsString(mac_j) || strlen(mac_j->valuestring) < 8 || strlen(mac_j->valuestring) >= sizeof(g_reg[0].mac)) {
        log_event("[ID] HELLO без коректного mac, відхилено (канал=%s)\n", route_kind_name(route->kind));
        cJSON_Delete(j);
        return;
    }
    cJSON *had_j = cJSON_GetObjectItemCaseSensitive(j, "id");
    unsigned had = cJSON_IsNumber(had_j) ? (unsigned)had_j->valuedouble : 0;

    bool is_new;
    uint16_t id = registry_assign(mac_j->valuestring, &is_new);
    if (id == 0) {
        log_event("[ID] Реєстр заповнений (%d плат), плата %s id не отримала\n", MAX_NODES, mac_j->valuestring);
        cJSON_Delete(j);
        return;
    }
    if (is_new)
        log_event("[ID] Нова плата %s (канал=%s) -> node_id=%u\n", mac_j->valuestring, route_kind_name(route->kind), id);
    if (had != 0 && had != id)
        log_event("[ID] Плата %s мала id=%u, переписано на %u\n", mac_j->valuestring, had, id);

    // HELLO шле лише плата, що щойно завантажилась (id ще не підтверджено в цій
    // сесії). Це надійніша ознака перезапуску, ніж евристика за sequence/timestamp:
    // та плутає запізнілий ALARM із перезапуском.
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].node_id == id && nodes[i].max_seq_seen != -1) {
            log_event("[СТАТУС] Вузол %u перезапустився (HELLO від %s). Скидаю відстеження sequence\n",
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
        send_frame_route(route, MSG_CONFIG, 0, next_down_seq(), rs); // node_id=0: адресат визначається за mac
        free(rs);
    }
    cJSON_Delete(reply);
    cJSON_Delete(j);
}

// Публікує пакет у TELEMETRY_TOPIC. Payload, якщо це валідний JSON, іде як
// вкладений об'єкт; інакше -- сирим рядком у "payload_raw".
static void publish_telemetry(const SensorPacket *pkt, const char *payload_str, RouteKind kind, NodeState *node) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "node_id", pkt->node_id);
    cJSON_AddNumberToObject(root, "sequence", pkt->sequence);
    cJSON_AddNumberToObject(root, "ts_ms", (double)pkt->timestamp_ms);
    cJSON_AddNumberToObject(root, "type", pkt->msg_type);
    cJSON_AddStringToObject(root, "transport", route_kind_name(kind));

    cJSON *payload = pkt->payload_len > 0 ? cJSON_Parse(payload_str) : NULL;
    if (payload) {
        // Стан буфера беремо лише з найновішого пакета: запізнілі з буфера описують минуле
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

void handle_packet(const uint8_t *raw, size_t raw_len, const ReplyRoute *route) {
    SensorPacket pkt = {0};

    // Розпакування та строга перевірка CRC32
    int status = protocol_unpack(raw, raw_len, &pkt);

    if (status == PROTO_ERR_BAD_CRC) {
        corrupted_count++;
        log_event("[ПОМИЛКА] Пакет битий (PROTO_ERR_BAD_CRC), канал=%s! Відхилено. "
                   "Разом битих пакетів: %u\n", route_kind_name(route->kind), corrupted_count);
        return;
    } else if (status != PROTO_OK) {
        corrupted_count++;
        log_event("[ПОПЕРЕДЖЕННЯ] Помилка розпакування (код: %d), канал=%s. Відхилено. "
                   "Разом битих пакетів: %u\n", status, route_kind_name(route->kind), corrupted_count);
        return;
    }

    // ACK від вузла (підтвердження наших CONFIG-пакетів автоналаштування) --
    // це не телеметрія, у облік sequence/втрат не потрапляє.
    if (pkt.msg_type == MSG_ACK) {
        provision_on_ack(&pkt);
        latency_on_ack(&pkt);
        return;
    }

    // HELLO від плати без id: HEARTBEAT з node_id=0 -- запит на призначення id.
    if (pkt.msg_type == MSG_HEARTBEAT && pkt.node_id == 0) {
        handle_hello(&pkt, route);
        return;
    }
    if (pkt.node_id == 0) return; // node_id=0 зарезервовано; інші пакети без id не приймаємо

    // Трекінг вузла
    NodeState *node = find_or_create_node(pkt.node_id);
    if (!node) return;

    // Дріт утикнули знову (вузол з'явився на UART після того, як був деінде або мовчав):
    // налаштування Wi-Fi могли застаріти -- перенастроюємо.
    uint64_t t_now = get_monotonic_time_ms();
    if (route->kind == ROUTE_UART && node->last_seen_ms != 0 &&
        (node->last_route.kind != ROUTE_UART || node->last_route.fd != route->fd ||
         t_now - node->last_seen_ms > NODE_TIMEOUT_MS)) {
        log_event("[НАЛАШТУВАННЯ] Вузол %u знову на UART -- перенастроюю\n", pkt.node_id);
        provision_reset(node);
    }
    node->last_seen_ms = t_now;
    node->last_transport = route->kind;
    node->last_route = *route;

    // Аналіз втрат, дублікатів і порядку (вікно SEQ_WINDOW останніх sequence).
    bool is_seq_duplicate = false;
    // ALARM/CONFIG плата може повторювати дуже пізно (черга на платі, retry після
    // розриву), тож їхні старі sequence/timestamp -- не ознака перезапуску.
    bool critical = pkt.msg_type == MSG_ALARM || pkt.msg_type == MSG_CONFIG;
    // Перезапуск плати: millis() пішов з нуля, тобто timestamp різко впав.
    // Лише за sequence це видно не одразу (поки не набіжить SEQ_WINDOW пакетів).
    // Основний сигнал перезапуску -- HELLO (див. handle_hello), це запасна евристика.
    bool ts_restart = !critical && node->max_seq_seen != -1 && pkt.timestamp_ms + 30000 < node->last_ts_ms;
    if (node->max_seq_seen == -1) {
        node->max_seq_seen = (int32_t)pkt.sequence;
        node->seen_mask = 1;
        node->received_count++;
        node->last_ts_ms = pkt.timestamp_ms;
    } else {
        int64_t diff = (int64_t)pkt.sequence - (int64_t)node->max_seq_seen;
        if (diff > 0) {
            // Новий найвищий sequence; усе, що пропущено між ними, -- втрати
            // (якщо воно потім прийде запізно, лічильник втрат зменшиться).
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
                // Запізнілий пакет (порушення порядку): не дублікат, і раніше
                // він був зарахований як втрачений.
                node->seen_mask |= bit;
                node->received_count++;
                if (node->lost_count > 0) node->lost_count--;
                log_event("[ПОРЯДОК] Вузол %u: пакет Seq %u прийшов із запізненням (порушення порядку)\n",
                           pkt.node_id, pkt.sequence);
            }
        } else if (critical) {
            // ALARM/CONFIG відстає від максимуму більше ніж на вікно: це запізніле
            // повторення, а не перезапуск. Обліку sequence не чіпаємо, дублікат
            // визначаємо за списком уже бачених критичних sequence.
            if (is_duplicate_alarm(node, pkt.sequence)) {
                is_seq_duplicate = true;
                node->duplicate_count++;
            } else {
                node->received_count++;
                if (node->lost_count > 0) node->lost_count--;
                log_event("[ПОРЯДОК] Вузол %u: критичний пакет Seq %u прийшов за межами вікна (запізнілий)\n",
                           pkt.node_id, pkt.sequence);
            }
        } else {
            // sequence відстає від максимуму більше ніж на вікно -- це не
            // дублікат, а перезапуск вузла (sequence пішов з нуля).
            log_event("[СТАТУС] Вузол %u, схоже, перезапустився (Seq %u після %d, timestamp %llu мс). Скидаю відстеження sequence\n",
                       pkt.node_id, pkt.sequence, node->max_seq_seen, (unsigned long long)pkt.timestamp_ms);
            node_seq_reset(node);
            node->max_seq_seen = (int32_t)pkt.sequence;
            node->seen_mask = 1;
            node->received_count++;
            node->last_ts_ms = pkt.timestamp_ms;
        }
    }

    // Парсинг JSON-корисного навантаження (у локальний буфер +1 байт для
    // '\0' -- НЕ пишемо термінатор у сам pkt.payload, бо payload_len може
    // дорівнювати MAX_PAYLOAD_SIZE і це був би вихід за межі масиву).
    char tmp[MAX_PAYLOAD_SIZE + 1];
    memcpy(tmp, pkt.payload, pkt.payload_len);
    tmp[pkt.payload_len] = '\0';

    // Дублі не віддаємо вебу -- у БД вони теж не потрібні.
    if (!is_seq_duplicate && pkt.msg_type != MSG_ACK) {
        publish_telemetry(&pkt, tmp, route->kind, node);
    }

    // Обробка критичних подій -- тепер логуємо ОБИДВА випадки: і нову
    // подію, і задедупліковану, бо саме другий випадок і є доказом
    // критерію №3 (повторний пакет не створив дублікат бізнес-події).
    if (pkt.msg_type == MSG_ALARM || pkt.msg_type == MSG_CONFIG) {
        if (!is_duplicate_alarm(node, pkt.sequence)) {
            log_event("[ПОДІЯ] Вузол %u надіслав ALARM/CONFIG (Seq: %u, канал=%s) -- НОВА подія\n",
                       pkt.node_id, pkt.sequence, route_kind_name(route->kind));
            record_alarm(node, pkt.sequence);
        } else {
            log_event("[ПОДІЯ] Вузол %u повторив ALARM/CONFIG (Seq: %u, канал=%s) -- "
                       "ДЕДУПЛІКОВАНО, друга подія НЕ створена\n",
                       pkt.node_id, pkt.sequence, route_kind_name(route->kind));
        }
        send_ack_via(pkt.node_id, pkt.sequence, route);
    }
}

// Колбек MQTT -- лише розпаковує ReplyRoute{ROUTE_MQTT} і передає в
// спільний handle_packet(). Лишається для sim_node і для сумісності --
// фізичні плати тепер ідуть через UDP/TCP/UART нижче.
static bool tcp_fd_active(int fd) {
    if (fd < 0) return false;
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (g_tcp_clients[i].fd == fd) return true;
    }
    return false;
}

// Команди з веба (ALARM/CONFIG) приходять в MQTT-топік case24/downlink/<id>.
// Плата на UDP/TCP/UART цього топіка не слухає, тому Gateway сам пересилає
// кадр тим каналом, яким вузол останній раз вийшов на зв'язок. Для MQTT-вузлів
// (sim_node) нічого не робимо -- вони підписані на топік самі. Власні ACK
// Gateway, які теж ідуть у цей топік, ігноруємо (msg_type == ACK).
void forward_downlink(const struct mosquitto_message *msg) {
    SensorPacket pkt = {0};
    if (protocol_unpack((const uint8_t*)msg->payload, (size_t)msg->payloadlen, &pkt) != PROTO_OK) return;
    if (pkt.msg_type == MSG_ACK) return;

    for (int i = 0; i < node_count; i++) {
        if (nodes[i].node_id != pkt.node_id) continue;
        ReplyRoute route = nodes[i].last_route;
        if (route.kind == ROUTE_MQTT) return;
        // fd для TCP міг змінитись після перепідключення -- беремо поточний
        if (route.kind == ROUTE_UART && !uart_port_by_fd(route.fd)) route.fd = -1;
        if (route.kind == ROUTE_TCP && !tcp_fd_active(route.fd)) route.fd = -1;
        if ((route.kind == ROUTE_TCP || route.kind == ROUTE_UART) && route.fd < 0) {
            log_event("[DOWNLINK] Вузол %u: канал %s не активний, команду з веба не доставлено\n",
                       pkt.node_id, route_kind_name(route.kind));
            return;
        }
        route_reply(&route, (const uint8_t*)msg->payload, msg->payloadlen);
        log_event("[DOWNLINK] Команда з веба (тип %u, Seq %u) -> вузол %u через %s\n",
                   pkt.msg_type, pkt.sequence, pkt.node_id, route_kind_name(route.kind));
        return;
    }
    log_event("[DOWNLINK] Команду для невідомого вузла %u відкинуто\n", pkt.node_id);
}

// ---- З'єднання з MQTT-брокером ----
// mosquitto_loop() у неблокуючому режимі сам не перепідключається, а сесія clean=true
// втрачає підписки, тож підписуємось у on_connect, а перепідключення робить
// mqtt_maintain() з головного циклу. Без брокера шлюз не падає: UDP/TCP/UART працюють.
// Саме IPv4-адреса, не "localhost": асинхронне підключення пробує лише першу адресу,
// а "localhost" на macOS спершу дає IPv6 (::1), на якому брокер не слухає.
#define MQTT_HOST "127.0.0.1"
#define MQTT_PORT 1883
#define MQTT_RETRY_MS 1000
static bool g_mqtt_connected = false;

static void on_mqtt_connect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)userdata;
    if (rc != 0) {
        log_event("[MQTT] Брокер відхилив підключення (код %d), пробую знову\n", rc);
        return;
    }
    mosquitto_subscribe(mosq, NULL, "case24/uplink", 0);
    mosquitto_subscribe(mosq, NULL, DOWNLINK_PREFIX "+", 0);
    g_mqtt_connected = true;
    log_event("[MQTT] Підключено до брокера %s:%d\n", MQTT_HOST, MQTT_PORT);
}

static void on_mqtt_disconnect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq; (void)userdata;
    if (g_mqtt_connected)
        log_event("[MQTT] З'єднання з брокером втрачено (код %d), перепідключаюсь\n", rc);
    g_mqtt_connected = false;
}

// Викликається щоразу в головному циклі: крутить MQTT і, якщо зв'язку немає,
// раз на MQTT_RETRY_MS пробує підключитись (без блокування циклу).
static void mqtt_maintain(uint64_t now) {
    static uint64_t last_try = 0;
    static int failures = 0;

    int rc = mosquitto_loop(mosq_global, 0, 1); // неблокуюче -- решта каналів теж мають встигати
    if (rc != MOSQ_ERR_SUCCESS) g_mqtt_connected = false;
    if (g_mqtt_connected) { failures = 0; return; }

    if (last_try != 0 && now - last_try < MQTT_RETRY_MS) return;
    last_try = now ? now : 1;
    // Перша спроба після connect_async часто повертає помилку -- попереджаємо з другої
    if (mosquitto_reconnect_async(mosq_global) != MOSQ_ERR_SUCCESS && ++failures == 3) {
        log_event("[MQTT] Брокер %s:%d недоступний -- шлюз працює без MQTT і пробує знову кожні %d с\n",
                   MQTT_HOST, MQTT_PORT, MQTT_RETRY_MS / 1000);
    }
}

// ---- Вимірювання затримки (RTT) ----
// Раз на PING_INTERVAL_MS кожному online-вузлу йде CONFIG {"cmd":"ping"}; вузол
// відповідає ACK з тим самим sequence. RTT = час між відправкою і ACK за годинником
// шлюзу, тому синхронізація годинників не потрібна. Роздільність ~10-20 мс (цикл шлюзу).
// Вузол, що не відповів, просто не дає зразка (разом із втратами це видно в loss rate).
#define PING_INTERVAL_MS 5000
#define RTT_STALE_MS 20000     // зразок старший за це -- у стані latency_ms = null
#define RTT_EWMA_ALPHA 0.3

static void ping_tick(uint64_t now) {
    for (int i = 0; i < node_count; i++) {
        NodeState *n = &nodes[i];
        if (now - n->last_seen_ms >= NODE_TIMEOUT_MS) { n->ping_seq = 0; continue; } // OFFLINE
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
        n->ping_seq = 0; // запізнілий дубль ACK вдруге не рахуємо
        return;
    }
}

void on_mqtt_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    (void)mosq; (void)userdata;
    if (strncmp(msg->topic, DOWNLINK_PREFIX, strlen(DOWNLINK_PREFIX)) == 0) {
        forward_downlink(msg);
        return;
    }
    ReplyRoute route = { .kind = ROUTE_MQTT };
    handle_packet((const uint8_t*)msg->payload, (size_t)msg->payloadlen, &route);
}

// ---- UDP: підняти сокет і опитати його (неблокуюче) ----
void setup_udp(void) {
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) {
        fprintf(stderr, "[UDP] Не вдалося створити сокет: %s\n", strerror(errno));
        return;
    }
    int opt = 1;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(UDP_PORT);

    if (bind(g_udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[UDP] Не вдалося прив'язати порт %d: %s\n", UDP_PORT, strerror(errno));
        close(g_udp_fd);
        g_udp_fd = -1;
        return;
    }
    int flags = fcntl(g_udp_fd, F_GETFL, 0);
    fcntl(g_udp_fd, F_SETFL, flags | O_NONBLOCK);
    log_event("[UDP] Слухаю на порту %d\n", UDP_PORT);
}

void poll_udp(void) {
    if (g_udp_fd < 0) return;

    uint8_t buf[FRAME_MAX_SIZE];
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);

    // UDP зберігає межі датаграми -- на відміну від TCP/UART, тут не
    // потрібне збирання кадру по байтах: одна recvfrom() = один пакет.
    ssize_t n = recvfrom(g_udp_fd, buf, sizeof(buf), 0,
                          (struct sockaddr*)&sender, &sender_len);
    if (n > 0) {
        ReplyRoute route = { .kind = ROUTE_UDP, .udp_addr = sender };
        handle_packet(buf, (size_t)n, &route);
    }
    // n < 0 (EAGAIN/EWOULDBLOCK) -- просто немає нових даних, це норма.
}

// ---- TCP: слухати, приймати ОДНЕ з'єднання за раз, читати з фреймінгом ----
void setup_tcp(void) {
    g_tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp_listen_fd < 0) {
        fprintf(stderr, "[TCP] Не вдалося створити сокет: %s\n", strerror(errno));
        return;
    }
    int opt = 1;
    setsockopt(g_tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(TCP_PORT);

    if (bind(g_tcp_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[TCP] Не вдалося прив'язати порт %d: %s\n", TCP_PORT, strerror(errno));
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
        return;
    }
    if (listen(g_tcp_listen_fd, MAX_TCP_CLIENTS) < 0) {
        fprintf(stderr, "[TCP] listen() не вдався: %s\n", strerror(errno));
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
        return;
    }
    int flags = fcntl(g_tcp_listen_fd, F_GETFL, 0);
    fcntl(g_tcp_listen_fd, F_SETFL, flags | O_NONBLOCK);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) g_tcp_clients[i].fd = -1;
    log_event("[TCP] Слухаю на порту %d\n", TCP_PORT);
}

static void tcp_client_close(TcpClient *c, const char *why) {
    log_event("[TCP] Вузол (fd=%d) відключено: %s\n", c->fd, why);
    close(c->fd);
    c->fd = -1;
}

void poll_tcp(void) {
    if (g_tcp_listen_fd < 0) return;
    uint64_t now = get_monotonic_time_ms();

    // Приймаємо всі нові з'єднання, що чекають
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
            log_event("[TCP] Забагато клієнтів (макс. %d), з'єднання з %s відхилено\n",
                       MAX_TCP_CLIENTS, inet_ntoa(client_addr.sin_addr));
            close(fd);
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        g_tcp_clients[slot].fd = fd;
        g_tcp_clients[slot].last_rx_ms = now;
        frame_reader_init(&g_tcp_clients[slot].reader);
        log_event("[TCP] Вузол підключився з %s (fd=%d)\n", inet_ntoa(client_addr.sin_addr), fd);
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
            tcp_client_close(cl, "закрито вузлом");
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            tcp_client_close(cl, strerror(errno));
        } else if (now - cl->last_rx_ms > TCP_IDLE_TIMEOUT_MS) {
            tcp_client_close(cl, "немає даних (обрив без FIN)");
        }
    }
}

// Відкриває порт у сирому режимі 8N1 115200. Повертає fd або -1.
static int uart_open_configured(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) { close(fd); return -1; }

    cfsetospeed(&tty, UART_BAUD);
    cfsetispeed(&tty, UART_BAUD);

    tty.c_cflag &= ~PARENB;    // без парності
    tty.c_cflag &= ~CSTOPB;    // 1 стоп-біт
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;        // 8 біт даних
    tty.c_cflag &= ~CRTSCTS;   // без апаратного flow control
    tty.c_cflag |= (CREAD | CLOCAL);
    tty.c_cflag &= ~HUPCL;     // не смикати DTR/RTS при закритті порту (вони скидають ESP32)

    tty.c_lflag &= ~(ICANON | IEXTEN);    // сирий режим: без по-рядкової обробки і службових символів (DISCARD, LNEXT)
    tty.c_lflag &= ~(ECHO | ECHOE | ISIG);

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { close(fd); return -1; }

    // USB-UART адаптер плати: DTR/RTS підключені до EN/GPIO0, і відкриття порту
    // їх активує (плата перезавантажується). Знімаємо обидва, щоб далі вона працювала.
    int modem_bits = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &modem_bits);
    return fd;
}

static void uart_close_port(UartPort *p, const char *why) {
    log_event("[UART] %s: %s -- порт закрито\n", p->path, why);
    // Вузли, яких чули через цей порт, більше не мають дроту (і fd може дістатися іншому порту).
    for (int i = 0; i < node_count; i++)
        if (nodes[i].last_route.kind == ROUTE_UART && nodes[i].last_route.fd == p->fd)
            nodes[i].last_route.fd = -1;
    close(p->fd);
    p->fd = -1;
}

// Шукає послідовні порти й відкриває нові. Номер ttyUSBx Linux видає за порядком
// появи пристроїв і він "скаче", тому кожен порт ідентифікуємо за realpath, а які
// з них наші -- вирішує відповідь на пробний пінг, а не назва.
//   GATEWAY_UART='/dev/serial/by-id/usb-FTDI*' -- шукати лише за цим шаблоном/шляхом
//   не задано -- усі /dev/ttyUSB* і /dev/ttyACM*
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
            if (!slot) break; // усі слоти зайняті

            int fd = uart_open_configured(real);
            if (fd < 0) {
                if (first) fprintf(stderr, "[UART] Не вдалося відкрити %s (%s)\n", real, strerror(errno));
                continue;
            }
            slot->fd = fd;
            snprintf(slot->path, sizeof(slot->path), "%s", real);
            frame_reader_init(&slot->reader);
            slot->last_rx_ms = 0;
            slot->opened_ms = get_monotonic_time_ms();
            slot->responded = false;
            slot->warned_silent = false;
            log_event("[UART] Знайдено порт %s (115200 8N1) -- шлю пробні пінги, чекаю відповіді плати\n", real);
        }
        globfree(&g);
    }

    if (first) {
        bool any = false;
        for (int j = 0; j < MAX_UART_PORTS; j++) any = any || g_uart_ports[j].fd >= 0;
        if (!any)
            fprintf(stderr,
                    "[UART] Послідовних портів не знайдено (/dev/ttyUSB*, /dev/ttyACM*) -- UART-вузли недоступні. "
                    "Шлюз шукає знову кожні 2 с, решта каналів працює.\n");
    }
}

#define UART_SCAN_MS 2000 // як часто шукаємо нові порти (перехідник могли встромити пізніше)

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

        uint8_t rx[256];
        ssize_t n = read(p->fd, rx, sizeof(rx));
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            char why[96];
            snprintf(why, sizeof(why), "порт зник (%s)", strerror(errno));
            uart_close_port(p, why);
            continue;
        }
        if (n > 0) {
            // Кадр плата пише одним блоком (~15 мс на 115200). Пауза довша за
            // UART_FRAME_GAP_MS всередині недоскладеного кадру означає сміття (завантажувач
            // ESP32 друкує текст при скиданні) -- скидаємо складання, щоб наступний
            // справжній кадр не з'їхав на зміщення.
            if (p->reader.have > 0 && now_ms - p->last_rx_ms > UART_FRAME_GAP_MS)
                frame_reader_init(&p->reader);
            p->last_rx_ms = now_ms;
            ReplyRoute route = { .kind = ROUTE_UART, .fd = p->fd };
            for (ssize_t i = 0; i < n; i++) {
                if (frame_reader_feed_byte(&p->reader, rx[i])) {
                    SensorPacket probe;
                    if (!p->responded && protocol_unpack(p->reader.buf, p->reader.have, &probe) == PROTO_OK) {
                        p->responded = true;
                        log_event("[UART] %s: пристрій відповів валідним кадром -- це наш сенсор\n", p->path);
                    }
                    handle_packet(p->reader.buf, p->reader.have, &route);
                    frame_reader_init(&p->reader);
                }
            }
        }
        if (!p->responded && !p->warned_silent && now_ms - p->opened_ms > UART_PROBE_WARN_MS) {
            p->warned_silent = true;
            log_event("[UART] %s: немає відповіді на пінги %d с -- схоже, не наша плата (порт лишається відкритим)\n",
                      p->path, UART_PROBE_WARN_MS / 1000);
        }
    }
}

void print_dashboard() {
    uint64_t now = get_monotonic_time_ms();
    printf("\n--- Дашборд (Вузлів: %d) ---\n", node_count);
    printf("%-5s | %-7s | %-6s | %-6s | %-6s | %-6s | %-8s | %-7s | %-10s\n",
           "Вузол", "Статус", "Канал", "Отр.", "Втрат", "Дубл.", "RTT мс", "Backlog", "Max Seq");
    printf("----------------------------------------------------------------------------------\n");

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

        char rtt_s[16] = "-", bl_s[16] = "-";
        if (n->rtt_at_ms != 0 && now - n->rtt_at_ms < RTT_STALE_MS) snprintf(rtt_s, sizeof(rtt_s), "%.0f", n->rtt_ms);
        if (n->backlog >= 0) snprintf(bl_s, sizeof(bl_s), "%d", n->backlog);
        printf("%-5u | %-7s | %-6s | %-6u | %-6u | %-6u | %-8s | %-7s | %-10d (Loss: %.1f%%)\n",
               n->node_id, online ? "ONLINE" : "OFFLINE", route_kind_name(n->last_transport),
               n->received_count, n->lost_count, n->duplicate_count,
               rtt_s, bl_s, n->max_seq_seen, loss_rate);
    }
    printf("Всього битих пакетів з початку роботи: %u\n", corrupted_count);
    printf("======================================================\n");
}

// Публікує оброблений стан (те саме, що показує термінальний дашборд, і
// нічого більше) у STATE_TOPIC як JSON. Це ЄДИНИЙ канал, звідки веб-
// інтерфейс (Блок E) має брати received/lost/duplicate/online -- він НЕ
// повинен сам рахувати це з сирого case24/uplink, інакше виходять два
// незалежні шлюзи з можливо різними цифрами. Веб лишається "вікном" у
// стан Gateway, як і намальовано в архітектурі кейсу
// (Sensor Node -> ... -> Gateway -> Event Store / Dashboard).
//
// НЕ чіпає n->was_online -- це виключно відповідальність print_dashboard()
// (там же і лог переходів ONLINE<->OFFLINE), щоб не було двох місць, які
// одночасно мутують один стан.
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
        // Скільки мс тому прийшов останній пакет -- монотонний час, тому
        // порівнянний лише сам із собою (не з wall-clock веб-сторони).
        // Веб показує це як "X.Xс тому", а не намагається звести годинники.
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
        fprintf(g_log_file, "\n===== Новий запуск Gateway: %s", ctime(&start_t)); /* ctime сам додає \n */
        fflush(g_log_file);
        printf("Лог подій пишеться у файл: %s\n", LOG_FILE_PATH);
    } else {
        fprintf(stderr, "[УВАГА] Не вдалося відкрити %s для запису -- лог буде тільки на екрані.\n",
                LOG_FILE_PATH);
    }

    signal(SIGPIPE, SIG_IGN); // запис у закритий TCP/UART не повинен вбивати шлюз

    mosquitto_lib_init();
    mosq_global = mosquitto_new("GatewayClient", true, NULL);
    if (!mosq_global) {
        fprintf(stderr, "Не вдалося створити MQTT-клієнта.\n");
        return 1;
    }
    mosquitto_connect_callback_set(mosq_global, on_mqtt_connect);
    mosquitto_disconnect_callback_set(mosq_global, on_mqtt_disconnect);
    mosquitto_message_callback_set(mosq_global, on_mqtt_message);
    // connect_async лише запам'ятовує адресу брокера; саме підключення робить
    // mqtt_maintain() (reconnect_async) -- так само на старті й після обриву.
    mosquitto_connect_async(mosq_global, MQTT_HOST, MQTT_PORT, 60);

    // 30.09 (3) -- три додаткові "сирі" транспорти поряд з MQTT. Кожен
    // піднімається незалежно: якщо, наприклад, UART-перехідник не
    // підключений, UART-сканер лише друкує попередження і gateway
    // продовжує працювати з рештою каналів (MQTT/UDP/TCP).
    setup_udp();
    setup_tcp();
    uart_init();
    g_down_seq = ((uint32_t)time(NULL) & 0x7FFFFFFF) | 1;
    registry_load();
    provision_start_worker();

    printf("Шлюз успішно запущено. Очікування телеметрії (MQTT/UDP/TCP/UART)...\n");

    uint64_t last_dash_ms = get_monotonic_time_ms();

    while (1) {
        uint64_t now = get_monotonic_time_ms();
        mqtt_maintain(now);
        poll_udp();
        poll_tcp();
        poll_uart();

        now = get_monotonic_time_ms();
        uart_tick(now);
        ping_tick(now);
        if (now - last_dash_ms >= 2000) {
            print_dashboard();
            publish_gateway_state(); // те саме, що дашборд, але для веб (Блок E)
            last_dash_ms = now;
        }

        usleep(10000); // 10мс -- щоб порожній цикл не вантажив CPU на 100%
    }

    mosquitto_destroy(mosq_global);
    mosquitto_lib_cleanup();
    if (g_log_file != NULL) {
        fclose(g_log_file);
    }
    return 0;
}
