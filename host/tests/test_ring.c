/*
 * Host-side regression test for main/can_ring.c.
 *
 * The ring is compiled with tiny ESP-IDF shims (mutex/heap) so the overflow
 * policy can be checked deterministically: pushing into a full ring must drop
 * the OLDEST record. The old code advanced s_head before writing and therefore
 * overwrote the second-oldest record, leaving the stale oldest one in place.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can_ring.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    pthread_mutex_t mutex;
} shim_mutex_t;

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    shim_mutex_t *m = malloc(sizeof(*m));
    if (!m) {
        return NULL;
    }
    if (pthread_mutex_init(&m->mutex, NULL) != 0) {
        free(m);
        return NULL;
    }
    return (SemaphoreHandle_t)m;
}

int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks)
{
    (void)ticks;
    return pthread_mutex_lock(&((shim_mutex_t *)sem)->mutex) == 0 ? pdTRUE : pdFALSE;
}

int xSemaphoreGive(SemaphoreHandle_t sem)
{
    return pthread_mutex_unlock(&((shim_mutex_t *)sem)->mutex) == 0 ? pdTRUE : pdFALSE;
}

static int s_failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            s_failures++;                                                            \
        }                                                                            \
    } while (0)

static void push_id(uint32_t id)
{
    linsight_can_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.id = id;
    rec.dlc = 1;
    rec.data[0] = (uint8_t)id;
    can_ring_push(&rec);
}

int main(void)
{
    if (can_ring_init(8) != ESP_OK) {
        fprintf(stderr, "FAIL: can_ring_init\n");
        return 1;
    }

    /* Overrun by 12: only the last 8 pushes may survive, oldest first. */
    for (uint32_t i = 0; i < 20; i++) {
        push_id(i);
    }

    can_ring_info_t info;
    can_ring_get_info(&info);
    CHECK(info.cap == 8);
    CHECK(info.used == 8);
    CHECK(info.dropped == 12);

    linsight_can_record_t out[8];
    uint32_t n = can_ring_pop(out, 8);
    CHECK(n == 8);
    for (uint32_t i = 0; i < n; i++) {
        CHECK(out[i].id == 12 + i);
        CHECK(out[i].data[0] == (uint8_t)(12 + i));
        CHECK(out[i].seq == (uint16_t)(12 + i));
    }
    CHECK(can_ring_pop(out, 8) == 0);

    /* Ordinary (non-full) ordering after a drain. */
    for (uint32_t i = 20; i < 24; i++) {
        push_id(i);
    }
    n = can_ring_pop(out, 2);
    CHECK(n == 2);
    CHECK(out[0].id == 20);
    CHECK(out[1].id == 21);
    n = can_ring_pop(out, 8);
    CHECK(n == 2);
    CHECK(out[0].id == 22);
    CHECK(out[1].id == 23);
    CHECK(can_ring_pop(out, 1) == 0);

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("ring: all checks passed\n");
    return 0;
}
