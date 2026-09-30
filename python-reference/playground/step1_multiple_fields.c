/*
 * Те саме, що playground/step1_multiple_fields.py, але на C.
 *
 * struct SensorHeader {
 *     uint8_t  node_id;
 *     uint8_t  msg_type;
 *     uint32_t sequence;
 * };
 *
 * Компіляція:  cc step1_multiple_fields.c -o step1
 * Запуск:      ./step1
 */
#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>   /* htonl() -- переворот байтів у мережевий порядок */

/* __attribute__((packed)) забирає padding, який компілятор інакше додав би
 * для вирівнювання полів у пам'яті (без цього sizeof міг би бути не 6,
 * а, наприклад, 8 байт -- компілятор "підрівнює" uint32_t до кратності 4). */
typedef struct __attribute__((packed)) {
    uint8_t  node_id;
    uint8_t  msg_type;
    uint32_t sequence;
} SensorHeader;

int main(void) {
    SensorHeader h;
    h.node_id  = 5;
    h.msg_type = 1;
    h.sequence = 42;

    printf("Запакували: node_id=%u msg_type=%u sequence=%u\n",
           h.node_id, h.msg_type, h.sequence);
    printf("sizeof(SensorHeader) = %zu байт\n\n", sizeof(h));

    /* ==== Варіант A: просто дивимось на байти структури "як є" ==== */
    unsigned char *raw_native = (unsigned char *)&h;
    printf("Варіант A -- сирі байти структури в пам'яті (native order):\n  ");
    for (size_t i = 0; i < sizeof(h); i++) printf("%02x", raw_native[i]);
    printf("\n");
    printf("  (на x86/більшості ESP32/STM32 це little-endian --\n");
    printf("   sequence=42=0x0000002A ляже як 2a 00 00 00, МОЛОДШИЙ байт першим)\n\n");

    /* ==== Варіант B: явно переводимо в мережевий порядок (big-endian) ====
     * Це саме те, що Python зробив автоматично через префікс "!" у форматі.
     * htonl = "host to network long" -- стандартна функція з <arpa/inet.h>,
     * яка розвертає байти 32-бітного числа під мережевий порядок. */
    SensorHeader h_net = h;
    h_net.sequence = htonl(h.sequence);

    unsigned char *raw_net = (unsigned char *)&h_net;
    printf("Варіант B -- ті самі дані, sequence переведено в мережевий порядок:\n  ");
    for (size_t i = 0; i < sizeof(h_net); i++) printf("%02x", raw_net[i]);
    printf("\n");
    printf("  (це і має збігтися байт-в-байт з Python: 05010000002a)\n\n");

    /* ==== Розпаковка "назад" ====
     * У C це навіть не окрема дія -- дані вже лежать як структура.
     * Якщо ж отримали сирі байти з мережі -- робимо навпаки: ntohl(). */
    uint32_t sequence_back = ntohl(h_net.sequence);
    printf("Розпакували назад (ntohl): node_id=%u msg_type=%u sequence=%u\n",
           h_net.node_id, h_net.msg_type, sequence_back);

    return 0;
}
