#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else crc = crc >> 1;
        }
    }
    return ~crc;
}

int main(void) {
    const char *test = "123456789";
    uint32_t result = crc32((const uint8_t *)test, strlen(test));
    printf("crc32(\"123456789\") = 0x%08X (очікуємо 0xCBF43926)\n", result);
    return 0;
}
