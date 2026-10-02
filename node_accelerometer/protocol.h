#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* ==== Налаштування протоколу (див. 00-overview-shared-contract.md) ==== */
#define PROTOCOL_VERSION   1
#define MAX_PAYLOAD_SIZE   128

/* Розмір заголовка НА ДРОТІ (без payload, без crc):
 * version(1) + msg_type(1) + node_id(2) + sequence(4)
 * + timestamp_ms(8) + payload_len(2) = 18 байт */
#define HEADER_SIZE 18
#define CRC_SIZE    4

/* ==== Типи повідомлень ==== */
typedef enum {
    MSG_HEARTBEAT = 0,  /* best-effort, без ACK */
    MSG_TELEMETRY = 1,  /* best-effort, без ACK */
    MSG_ALARM     = 2,  /* критичне, потребує ACK/retry */
    MSG_CONFIG    = 3,  /* критичне, потребує ACK/retry */
    MSG_ACK       = 4,  /* підтвердження критичного повідомлення */
} MessageType;

/* ==== Пакет: зручне представлення В ПАМ'ЯТІ ====
 *
 * Це НЕ те саме, що байти "на дроті" -- payload тут завжди займає
 * MAX_PAYLOAD_SIZE байт (щоб можна було просто оголосити змінну на
 * стеку), але реально використовується лише перші payload_len байт.
 * protocol_pack() сам вирізає лише потрібну частину при серіалізації. */
typedef struct {
    uint8_t  version;
    uint8_t  msg_type;
    uint16_t node_id;
    uint32_t sequence;
    uint64_t timestamp_ms;
    uint16_t payload_len;
    uint8_t  payload[MAX_PAYLOAD_SIZE];
} SensorPacket;

/* ==== Коди результату ==== */
#define PROTO_OK                   0
#define PROTO_ERR_TOO_SHORT       -1  /* raw_len замалий навіть для заголовка+CRC */
#define PROTO_ERR_BAD_CRC         -2  /* CRC не збігається -- пакет пошкоджений */
#define PROTO_ERR_UNKNOWN_TYPE    -3  /* msg_type не входить у перелік MessageType */
#define PROTO_ERR_LEN_MISMATCH    -4  /* payload_len у заголовку != фактичний залишок байт */
#define PROTO_ERR_PAYLOAD_TOO_BIG -5  /* payload_len > MAX_PAYLOAD_SIZE */
#define PROTO_ERR_BUF_TOO_SMALL   -6  /* out_buf замалий для protocol_pack */

/* Стандартний CRC-32 (той самий алгоритм, що і zlib.crc32 в Python,
 * поліном 0xEDB88320). Перевірено на еталонному векторі:
 * crc32("123456789") == 0xCBF43926. */
uint32_t crc32(const uint8_t *data, size_t len);

/* Серіалізує pkt в out_buf (сирі байти, готові для відправки).
 * Повертає ПОЗИТИВНУ кількість записаних байт при успіху,
 * або один з кодів помилок вище (від'ємне число) при проблемі. */
int protocol_pack(const SensorPacket *pkt, uint8_t *out_buf, size_t out_buf_size);

/* Розбирає raw (довжини raw_len) назад у out_pkt.
 * Повертає PROTO_OK (0) при успіху, інакше код помилки.
 * Гарантія: НІКОЛИ не читає та не пише за межі виділеної пам'яті,
 * навіть якщо raw -- це повне сміття чи обірваний пакет. */
int protocol_unpack(const uint8_t *raw, size_t raw_len, SensorPacket *out_pkt);

#endif /* PROTOCOL_H */
