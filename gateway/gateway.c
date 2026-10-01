#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mosquitto.h>
#include <cjson/cJSON.h>
#include "protocol.h" // Підключаємо реальний протокол Блока А

#define MAX_NODES 10
#define MAX_SEEN_ALARMS 16
#define SEQ_WINDOW 64 // скільки останніх sequence пам'ятаємо для відрізнення дубліката від запізнілого пакета
#define NODE_TIMEOUT_MS 5000 // 5 секунд без повідомлень = вузол OFFLINE
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

/* Повертає true, якщо ПІСЛЯ цього байта кадр у fr->buf[0..fr->have) повний
 * і готовий для protocol_unpack(). Викликач має одразу забрати дані й
 * викликати frame_reader_init() перед наступним feed. */
static bool frame_reader_feed_byte(FrameReader *fr, uint8_t byte) {
    if (fr->have >= sizeof(fr->buf)) {
        frame_reader_init(fr); /* переповнення -- явно биті дані, скидаємо */
    }
    fr->buf[fr->have++] = byte;

    if (!fr->header_done && fr->have == HEADER_SIZE) {
        uint16_t payload_len;
        memcpy(&payload_len, fr->buf + 16, 2); /* offset payload_len у заголовку, див. protocol.h */
        fr->need = HEADER_SIZE + payload_len + CRC_SIZE;
        fr->header_done = true;
        if (fr->need > sizeof(fr->buf)) {
            frame_reader_init(fr); /* неможливий payload_len -- сміття на лінії */
            return false;
        }
    }
    return fr->header_done && fr->have == fr->need;
}

// Топік, куди Gateway публікує СВІЙ вже оброблений стан (Блок E, веб-
// дашборд, читає ЛИШЕ звідси, а не парсить сирий case24/uplink сам --
// інакше виходять два незалежні "шлюзи" з розбіжною статистикою, а
// контракт (00-overview-shared-contract.md) віддає трекінг/dashboard/лог
// винятково Блоку C). Публікується разом з терміналним дашбордом.
#define STATE_TOPIC "case24/gateway/state"

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
} NodeState;

NodeState nodes[MAX_NODES];
int node_count = 0;
struct mosquitto *mosq_global = NULL;

// Дескриптори трьох "сирих" транспортів (окрім MQTT, який лишається на
// mosq_global). -1 означає "не піднявся" -- гейтвей просто пропускає цей
// канал у циклі опитування, решта транспортів працюють як і раніше.
int g_udp_fd = -1;
int g_tcp_listen_fd = -1;
int g_tcp_client_fd = -1;
int g_uart_fd = -1;
FrameReader g_tcp_reader;
FrameReader g_uart_reader;

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

    // Трекінг вузла
    NodeState *node = find_or_create_node(pkt.node_id);
    if (!node) return;

    node->last_seen_ms = get_monotonic_time_ms();
    node->last_transport = route->kind;

    // Аналіз втрат, дублікатів і порядку (вікно SEQ_WINDOW останніх sequence).
    if (node->max_seq_seen == -1) {
        node->max_seq_seen = (int32_t)pkt.sequence;
        node->seen_mask = 1;
        node->received_count++;
    } else {
        int64_t diff = (int64_t)pkt.sequence - (int64_t)node->max_seq_seen;
        if (diff > 0) {
            // Новий найвищий sequence; усе, що пропущено між ними, -- втрати
            // (якщо воно потім прийде запізно, лічильник втрат зменшиться).
            node->lost_count += (uint32_t)(diff - 1);
            node->seen_mask = (diff >= SEQ_WINDOW) ? 1 : ((node->seen_mask << diff) | 1);
            node->max_seq_seen = (int32_t)pkt.sequence;
            node->received_count++;
        } else if (-diff < SEQ_WINDOW) {
            uint64_t bit = 1ULL << (-diff);
            if (node->seen_mask & bit) {
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
        } else {
            // sequence відстає від максимуму більше ніж на вікно -- це не
            // дублікат, а перезапуск вузла (sequence пішов з нуля).
            log_event("[СТАТУС] Вузол %u, схоже, перезапустився (Seq %u після %d). Скидаю відстеження sequence\n",
                       pkt.node_id, pkt.sequence, node->max_seq_seen);
            node->max_seq_seen = (int32_t)pkt.sequence;
            node->seen_mask = 1;
            node->alarm_idx = 0;
            node->alarm_filled = 0;
            node->received_count++;
        }
    }

    // Парсинг JSON-корисного навантаження (у локальний буфер +1 байт для
    // '\0' -- НЕ пишемо термінатор у сам pkt.payload, бо payload_len може
    // дорівнювати MAX_PAYLOAD_SIZE і це був би вихід за межі масиву).
    if (pkt.payload_len > 0) {
        char tmp[MAX_PAYLOAD_SIZE + 1];
        memcpy(tmp, pkt.payload, pkt.payload_len);
        tmp[pkt.payload_len] = '\0';
        cJSON *json = cJSON_Parse(tmp);
        if (json) {
            cJSON_Delete(json);
        }
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
void on_mqtt_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    (void)mosq; (void)userdata;
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
    if (listen(g_tcp_listen_fd, 1) < 0) {
        fprintf(stderr, "[TCP] listen() не вдався: %s\n", strerror(errno));
        close(g_tcp_listen_fd);
        g_tcp_listen_fd = -1;
        return;
    }
    int flags = fcntl(g_tcp_listen_fd, F_GETFL, 0);
    fcntl(g_tcp_listen_fd, F_SETFL, flags | O_NONBLOCK);
    frame_reader_init(&g_tcp_reader);
    log_event("[TCP] Слухаю на порту %d\n", TCP_PORT);
}

void poll_tcp(void) {
    if (g_tcp_listen_fd < 0) return;

    if (g_tcp_client_fd < 0) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(g_tcp_listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            g_tcp_client_fd = fd;
            frame_reader_init(&g_tcp_reader);
            log_event("[TCP] Вузол підключився з %s (fd=%d)\n",
                       inet_ntoa(client_addr.sin_addr), fd);
        }
        return; // ще нема з ким читати цього циклу
    }

    uint8_t rx[256];
    ssize_t n = recv(g_tcp_client_fd, rx, sizeof(rx), 0);
    if (n > 0) {
        ReplyRoute route = { .kind = ROUTE_TCP, .fd = g_tcp_client_fd };
        for (ssize_t i = 0; i < n; i++) {
            if (frame_reader_feed_byte(&g_tcp_reader, rx[i])) {
                handle_packet(g_tcp_reader.buf, g_tcp_reader.have, &route);
                frame_reader_init(&g_tcp_reader);
            }
        }
    } else if (n == 0) {
        log_event("[TCP] Вузол (fd=%d) відключився\n", g_tcp_client_fd);
        close(g_tcp_client_fd);
        g_tcp_client_fd = -1;
    }
    // n < 0 (EAGAIN) -- просто немає нових байт зараз.
}

// ---- UART: відкрити послідовний порт (термінально, 8N1), читати з фреймінгом ----
void setup_uart(void) {
    g_uart_fd = open(UART_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_uart_fd < 0) {
        fprintf(stderr,
                "[UART] Не вдалося відкрити %s (%s) -- UART-вузол недоступний. "
                "Перевір, що USB-TTL перехідник підключений (`ls /dev/ttyUSB*`), "
                "і що UART_DEVICE вище відповідає реальному шляху. Гейтвей "
                "продовжує працювати без UART.\n", UART_DEVICE, strerror(errno));
        return;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(g_uart_fd, &tty) != 0) {
        fprintf(stderr, "[UART] tcgetattr: %s\n", strerror(errno));
        close(g_uart_fd);
        g_uart_fd = -1;
        return;
    }

    cfsetospeed(&tty, UART_BAUD);
    cfsetispeed(&tty, UART_BAUD);

    tty.c_cflag &= ~PARENB;    // без парності
    tty.c_cflag &= ~CSTOPB;    // 1 стоп-біт
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;        // 8 біт даних
    tty.c_cflag &= ~CRTSCTS;   // без апаратного flow control
    tty.c_cflag |= (CREAD | CLOCAL);

    tty.c_lflag &= ~ICANON;    // сирий (не по-рядковий) режим
    tty.c_lflag &= ~(ECHO | ECHOE | ISIG);

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);

    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(g_uart_fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "[UART] tcsetattr: %s\n", strerror(errno));
        close(g_uart_fd);
        g_uart_fd = -1;
        return;
    }

    frame_reader_init(&g_uart_reader);
    log_event("[UART] Слухаю на %s (115200 8N1)\n", UART_DEVICE);
}

void poll_uart(void) {
    if (g_uart_fd < 0) return;

    uint8_t rx[256];
    ssize_t n = read(g_uart_fd, rx, sizeof(rx));
    if (n > 0) {
        ReplyRoute route = { .kind = ROUTE_UART, .fd = g_uart_fd };
        for (ssize_t i = 0; i < n; i++) {
            if (frame_reader_feed_byte(&g_uart_reader, rx[i])) {
                handle_packet(g_uart_reader.buf, g_uart_reader.have, &route);
                frame_reader_init(&g_uart_reader);
            }
        }
    }
    // n <= 0 -- немає нових байт зараз (O_NONBLOCK), норма.
}

void print_dashboard() {
    uint64_t now = get_monotonic_time_ms();
    printf("\n--- Дашборд (Вузлів: %d) ---\n", node_count);
    printf("%-5s | %-7s | %-6s | %-6s | %-6s | %-6s | %-10s\n",
           "Вузол", "Статус", "Канал", "Отр.", "Втрат", "Дубл.", "Max Seq");
    printf("--------------------------------------------------------------\n");

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

        printf("%-5u | %-7s | %-6s | %-6u | %-6u | %-6u | %-10d (Loss: %.1f%%)\n",
               n->node_id, online ? "ONLINE" : "OFFLINE", route_kind_name(n->last_transport),
               n->received_count, n->lost_count, n->duplicate_count,
               n->max_seq_seen, loss_rate);
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
    mosquitto_message_callback_set(mosq_global, on_mqtt_message);

    if (mosquitto_connect(mosq_global, "localhost", 1883, 60) != 0) {
        fprintf(stderr, "Помилка підключення до MQTT-брокера.\n");
        return 1;
    }

    mosquitto_subscribe(mosq_global, NULL, "case24/uplink", 0);

    // 30.09 (3) -- три додаткові "сирі" транспорти поряд з MQTT. Кожен
    // піднімається незалежно: якщо, наприклад, UART-перехідник не
    // підключений, setup_uart() лише друкує попередження і gateway
    // продовжує працювати з рештою каналів (MQTT/UDP/TCP).
    setup_udp();
    setup_tcp();
    setup_uart();

    printf("Шлюз успішно запущено. Очікування телеметрії (MQTT/UDP/TCP/UART)...\n");

    uint64_t last_dash_ms = get_monotonic_time_ms();

    while (1) {
        mosquitto_loop(mosq_global, 0, 1); // неблокуюче -- решта каналів теж мають встигати
        poll_udp();
        poll_tcp();
        poll_uart();

        uint64_t now = get_monotonic_time_ms();
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
