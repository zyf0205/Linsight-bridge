#include "can_ring.h"

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static linsight_can_record_t *s_buf;
static uint32_t s_cap;
static uint32_t s_head;  /* next write index */
static uint32_t s_count; /* records stored */
static uint32_t s_stored;
static uint32_t s_dropped;
static SemaphoreHandle_t s_lock;

esp_err_t can_ring_init(uint32_t capacity_records)
{
    if (capacity_records == 0 || s_buf) {
        return ESP_ERR_INVALID_ARG;
    }

    s_buf = heap_caps_malloc((size_t)capacity_records * sizeof(*s_buf),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        return ESP_ERR_NO_MEM;
    }

    s_cap = capacity_records;
    s_head = 0;
    s_count = 0;
    s_stored = 0;
    s_dropped = 0;
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void can_ring_push(linsight_can_record_t *rec)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_count == s_cap) {
        /* Full: s_head points at the oldest record (head == tail when full),
         * so drop it in place. Advancing head here would overwrite the
         * second-oldest record and keep the stale oldest one instead. */
        s_dropped++;
        s_count--;
    }

    rec->seq = (uint16_t)(s_stored & 0xFFFFu);
    s_buf[s_head] = *rec;
    s_head = (s_head + 1) % s_cap;
    s_count++;
    s_stored++;

    xSemaphoreGive(s_lock);
}

uint32_t can_ring_pop(linsight_can_record_t *out, uint32_t max)
{
    if (max == 0) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    const uint32_t n = s_count < max ? s_count : max;
    uint32_t idx = s_head + s_cap - s_count; /* oldest record; never underflows */
    for (uint32_t i = 0; i < n; i++) {
        if (idx >= s_cap) {
            idx -= s_cap;
        }
        out[i] = s_buf[idx];
        idx++;
    }
    s_count -= n;

    xSemaphoreGive(s_lock);
    return n;
}

void can_ring_get_info(can_ring_info_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->used = s_count;
    out->cap = s_cap;
    out->dropped = s_dropped;
    xSemaphoreGive(s_lock);
}

void can_ring_reset_dropped(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dropped = 0;
    xSemaphoreGive(s_lock);
}
