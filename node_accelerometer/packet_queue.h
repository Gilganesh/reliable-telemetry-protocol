#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

/*
 * packet_queue.h -- кільцева черга SensorPacket фіксованої ємності.
 *
 * Навіщо окремий файл: на платі потрібні ДВІ черги з однаковою механікою --
 * буфер телеметрії (store-and-forward) і черга ALARM, що чекають на канал.
 * Тут вона написана один раз, без Arduino-залежностей, тож її можна
 * перевірити на ПК (tests/test_packet_queue.c).
 *
 * Header-only (static inline), щоб не залежати від extern "C" і порядку
 * компіляції в Arduino IDE. Пам'ять під елементи виділяє викликач (звичайний
 * масив), черга лише веде head/count -- жодного malloc.
 *
 * Політика переповнення вибирається функцією, якою кладуть:
 *   pq_push        -- у кінець; повна черга відкидає НАЙСТАРІШИЙ (свіжі дані важливіші)
 *   pq_push_front  -- на початок (повернення недоставленого); повна черга
 *                     відкидає НАЙНОВІШИЙ, щоб не втрачати те, що вже "в черзі першим"
 * Обидві повертають true, якщо через переповнення довелось щось відкинути
 * (лічильник dropped збільшується автоматично).
 */

#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

typedef struct {
    SensorPacket *items;   /* масив ємності capacity, належить викликачу */
    int capacity;
    int head;              /* індекс першого елемента */
    int count;
    uint32_t dropped;      /* скільки елементів відкинуто через переповнення */
} PacketQueue;

static inline void pq_init(PacketQueue *q, SensorPacket *storage, int capacity) {
    q->items = storage;
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    q->dropped = 0;
}

static inline bool pq_empty(const PacketQueue *q) { return q->count == 0; }

static inline bool pq_push(PacketQueue *q, const SensorPacket *pkt) {
    bool overflow = false;
    if (q->count == q->capacity) {
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->dropped++;
        overflow = true;
    }
    q->items[(q->head + q->count) % q->capacity] = *pkt;
    q->count++;
    return overflow;
}

static inline bool pq_push_front(PacketQueue *q, const SensorPacket *pkt) {
    bool overflow = false;
    if (q->count == q->capacity) {
        q->count--; /* відкидаємо хвіст (найновіший) */
        q->dropped++;
        overflow = true;
    }
    q->head = (q->head + q->capacity - 1) % q->capacity;
    q->items[q->head] = *pkt;
    q->count++;
    return overflow;
}

/* Перший елемент без видалення; NULL, якщо черга порожня. Вказівник чинний до
 * наступного push/pop. */
static inline SensorPacket *pq_peek(PacketQueue *q) {
    return q->count == 0 ? NULL : &q->items[q->head];
}

/* Видаляє перший елемент (out може бути NULL). false, якщо черга порожня. */
static inline bool pq_pop(PacketQueue *q, SensorPacket *out) {
    if (q->count == 0) return false;
    if (out) *out = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}

#endif /* PACKET_QUEUE_H */
