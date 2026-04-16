#include "rtreticulum/hal.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* Assumes FreeRTOS 10+ with configUSE_MUTEXES and
 * configSUPPORT_DYNAMIC_ALLOCATION enabled. Firmware overrides the weak
 * rt_hal_unix_micros_impl / rt_hal_log_write_impl / rt_hal_watchdog_feed
 * symbols to wire up RTC, UART/USB/RTT log sink, and the board watchdog. */

uint64_t rt_hal_millis(void) {
    return (uint64_t)xTaskGetTickCount() * (1000U / configTICK_RATE_HZ);
}

__attribute__((weak)) uint64_t rt_hal_unix_micros_impl(void) { return 0; }
uint64_t rt_hal_unix_micros(void) { return rt_hal_unix_micros_impl(); }

void rt_hal_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

struct ur_mutex { SemaphoreHandle_t h; };

ur_mutex_t* rt_hal_mutex_create(void) {
    SemaphoreHandle_t h = xSemaphoreCreateMutex();
    if (!h) return NULL;
    ur_mutex_t* m = (ur_mutex_t*)pvPortMalloc(sizeof(*m));
    if (!m) { vSemaphoreDelete(h); return NULL; }
    m->h = h;
    return m;
}

void rt_hal_mutex_destroy(ur_mutex_t* m) {
    if (!m) return;
    vSemaphoreDelete(m->h);
    vPortFree(m);
}

void rt_hal_mutex_lock(ur_mutex_t* m)   { xSemaphoreTake(m->h, portMAX_DELAY); }
void rt_hal_mutex_unlock(ur_mutex_t* m) { xSemaphoreGive(m->h); }

/* FreeRTOS recursive mutex requires configUSE_RECURSIVE_MUTEXES = 1. */
struct ur_recursive_mutex { SemaphoreHandle_t h; };

ur_recursive_mutex_t* rt_hal_recursive_mutex_create(void) {
    SemaphoreHandle_t h = xSemaphoreCreateRecursiveMutex();
    if (!h) return NULL;
    ur_recursive_mutex_t* m = (ur_recursive_mutex_t*)pvPortMalloc(sizeof(*m));
    if (!m) { vSemaphoreDelete(h); return NULL; }
    m->h = h;
    return m;
}

void rt_hal_recursive_mutex_destroy(ur_recursive_mutex_t* m) {
    if (!m) return;
    vSemaphoreDelete(m->h);
    vPortFree(m);
}

void rt_hal_recursive_mutex_lock(ur_recursive_mutex_t* m)   { xSemaphoreTakeRecursive(m->h, portMAX_DELAY); }
void rt_hal_recursive_mutex_unlock(ur_recursive_mutex_t* m) { xSemaphoreGiveRecursive(m->h); }

struct ur_task { TaskHandle_t h; };

ur_task_t* rt_hal_task_spawn(const char* name,
                             ur_task_fn  fn,
                             void*       arg,
                             size_t      stack_words,
                             int         priority) {
    ur_task_t* t = (ur_task_t*)pvPortMalloc(sizeof(*t));
    if (!t) return NULL;
    BaseType_t ok = xTaskCreate((TaskFunction_t)fn,
                                name ? name : "rtreticulum",
                                (uint16_t)stack_words,
                                arg,
                                (UBaseType_t)priority,
                                &t->h);
    if (ok != pdPASS) { vPortFree(t); return NULL; }
    return t;
}

__attribute__((weak)) void rt_hal_watchdog_feed(void) {}

__attribute__((weak)) int rt_hal_random_bytes(uint8_t* buf, size_t len) {
    /* Firmware MUST override this with a real TRNG. The default returns -1
     * so any caller that forgets to wire it up fails fast instead of using
     * predictable bytes. */
    (void)buf; (void)len;
    return -1;
}

__attribute__((weak)) void rt_hal_log_write_impl(const char* line, size_t len) {
    fwrite(line, 1, len, stdout);
}

void rt_hal_log_write(const char* line, size_t len) {
    rt_hal_log_write_impl(line, len);
}
