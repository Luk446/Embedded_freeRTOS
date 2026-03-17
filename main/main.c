#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "monitor.h"
#include "esp_task_wdt.h"

#define PIN_SYNC 13
#define PIN_IN_A 14
#define PIN_IN_B 25
#define PIN_IN_S 26
#define PIN_IN_MODE 27

#define ACK_A 16
#define ACK_B 17
#define ACK_AGG 18
#define ACK_C 19
#define ACK_D 21
#define ACK_S 22

#define PERIOD_A_US 10000LL
#define PERIOD_B_US 20000LL
#define PERIOD_AGG_US 20000LL
#define PERIOD_C_US 50000LL
#define PERIOD_D_US 50000LL

#define SPORADIC_SLACK_GUARD_US 3000LL
#define AGG_FALLBACK_TOKEN 0xDEADBEEFu

#if CONFIG_IDF_TARGET_ESP32C3
#define BUDGET_A_CYCLES 448000u
#define BUDGET_B_CYCLES 640000u
#define BUDGET_AGG_CYCLES 320000u
#define BUDGET_C_CYCLES 1120000u
#define BUDGET_D_CYCLES 640000u
#define BUDGET_S_CYCLES 400000u
#else
#define BUDGET_A_CYCLES 672000u
#define BUDGET_B_CYCLES 960000u
#define BUDGET_AGG_CYCLES 480000u
#define BUDGET_C_CYCLES 1680000u
#define BUDGET_D_CYCLES 960000u
#define BUDGET_S_CYCLES 600000u
#endif

static const char *TAG = "assignment2";

uint32_t WorkKernel(uint32_t budget_cycles, uint32_t seed);

typedef struct {
    uint32_t idx_a;
    uint32_t idx_b;
    uint32_t idx_agg;
    uint32_t idx_c;
    uint32_t idx_d;
    uint32_t idx_s;
    uint32_t last_edge_count_a;
    uint32_t last_edge_count_b;
    int64_t next_a_us;
    int64_t next_b_us;
    int64_t next_agg_us;
    int64_t next_c_us;
    int64_t next_d_us;
    bool token_a_valid;
    bool token_b_valid;
    uint32_t token_a;
    uint32_t token_b;
} sched_state_t;

static sched_state_t g_state;

static volatile uint32_t g_edge_count_a = 0;
static volatile uint32_t g_edge_count_b = 0;
static volatile uint32_t g_pending_s = 0;
static volatile uint32_t g_sync_time_us = 0;
static volatile bool g_sync_seen = false;
static volatile bool g_schedule_started = false;

static inline void ack_high(gpio_num_t pin)
{
    gpio_set_level(pin, 1);
}

static inline void ack_low(gpio_num_t pin)
{
    gpio_set_level(pin, 0);
}

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

static void IRAM_ATTR isr_in_a(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&g_edge_count_a, 1u, __ATOMIC_RELAXED);
}

static void IRAM_ATTR isr_in_b(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&g_edge_count_b, 1u, __ATOMIC_RELAXED);
}

static void IRAM_ATTR isr_in_s(void *arg)
{
    (void)arg;

    if (!__atomic_load_n(&g_schedule_started, __ATOMIC_RELAXED)) {
        return;
    }

    __atomic_fetch_add(&g_pending_s, 1u, __ATOMIC_RELAXED);
    notifySRelease();
}

static void IRAM_ATTR isr_sync(void *arg)
{
    (void)arg;

    if (__atomic_load_n(&g_sync_seen, __ATOMIC_RELAXED)) {
        return;
    }

    __atomic_store_n(&g_sync_time_us, (uint32_t)esp_timer_get_time(), __ATOMIC_RELAXED);
    __atomic_store_n(&g_sync_seen, true, __ATOMIC_RELEASE);
}

static void gpio_init(void)
{
    const gpio_config_t input_conf = {
        .pin_bit_mask = (1ULL << PIN_SYNC) |
                        (1ULL << PIN_IN_A) |
                        (1ULL << PIN_IN_B) |
                        (1ULL << PIN_IN_S) |
                        (1ULL << PIN_IN_MODE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    const gpio_config_t output_conf = {
        .pin_bit_mask = (1ULL << ACK_A) |
                        (1ULL << ACK_B) |
                        (1ULL << ACK_AGG) |
                        (1ULL << ACK_C) |
                        (1ULL << ACK_D) |
                        (1ULL << ACK_S),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&input_conf));
    ESP_ERROR_CHECK(gpio_config(&output_conf));

    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_SYNC, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_S, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_MODE, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_A, GPIO_FLOATING));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_B, GPIO_FLOATING));

    ack_low(ACK_A);
    ack_low(ACK_B);
    ack_low(ACK_AGG);
    ack_low(ACK_C);
    ack_low(ACK_D);
    ack_low(ACK_S);

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_SYNC, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_IN_A, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_IN_B, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_IN_S, GPIO_INTR_POSEDGE));

    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_SYNC, isr_sync, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_A, isr_in_a, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_B, isr_in_b, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_S, isr_in_s, NULL));
}

static int64_t wait_for_sync_rising_edge(void)
{
    __atomic_store_n(&g_sync_seen, false, __ATOMIC_RELEASE);

    while (gpio_get_level(PIN_SYNC) != 0) {
    }

    while (!__atomic_load_n(&g_sync_seen, __ATOMIC_ACQUIRE)) {
    }

    return (int64_t)__atomic_load_n(&g_sync_time_us, __ATOMIC_ACQUIRE);
}

static void reset_schedule_state(int64_t t0_us)
{
    g_state.idx_a = 0;
    g_state.idx_b = 0;
    g_state.idx_agg = 0;
    g_state.idx_c = 0;
    g_state.idx_d = 0;
    g_state.idx_s = 0;
    g_state.last_edge_count_a = __atomic_load_n(&g_edge_count_a, __ATOMIC_RELAXED);
    g_state.last_edge_count_b = __atomic_load_n(&g_edge_count_b, __ATOMIC_RELAXED);
    g_state.next_a_us = t0_us;
    g_state.next_b_us = t0_us;
    g_state.next_agg_us = t0_us;
    g_state.next_c_us = t0_us;
    g_state.next_d_us = t0_us;
    g_state.token_a_valid = false;
    g_state.token_b_valid = false;
    g_state.token_a = 0;
    g_state.token_b = 0;
    __atomic_store_n(&g_pending_s, 0u, __ATOMIC_RELAXED);
}

static uint32_t edge_count_a_last_period(void)
{
    const uint32_t total = __atomic_load_n(&g_edge_count_a, __ATOMIC_RELAXED);
    const uint32_t delta = total - g_state.last_edge_count_a;
    g_state.last_edge_count_a = total;
    return delta;
}

static uint32_t edge_count_b_last_period(void)
{
    const uint32_t total = __atomic_load_n(&g_edge_count_b, __ATOMIC_RELAXED);
    const uint32_t delta = total - g_state.last_edge_count_b;
    g_state.last_edge_count_b = total;
    return delta;
}

static bool try_take_sporadic_release(void)
{
    uint32_t pending = __atomic_load_n(&g_pending_s, __ATOMIC_ACQUIRE);

    while (pending > 0u) {
        if (__atomic_compare_exchange_n(&g_pending_s,
                                        &pending,
                                        pending - 1u,
                                        false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            return true;
        }
    }

    return false;
}

static int64_t next_periodic_release_us(void)
{
    int64_t next_release = g_state.next_a_us;

    if (g_state.next_b_us < next_release) {
        next_release = g_state.next_b_us;
    }
    if (g_state.next_agg_us < next_release) {
        next_release = g_state.next_agg_us;
    }
    if (g_state.next_c_us < next_release) {
        next_release = g_state.next_c_us;
    }
    if (g_state.next_d_us < next_release) {
        next_release = g_state.next_d_us;
    }

    return next_release;
}

static bool should_run_sporadic(int64_t now)
{
    if (__atomic_load_n(&g_pending_s, __ATOMIC_ACQUIRE) == 0u) {
        return false;
    }

    return (next_periodic_release_us() - now) > SPORADIC_SLACK_GUARD_US;
}

static void task_a(void)
{
    const uint32_t id = g_state.idx_a;
    const uint32_t count_a = edge_count_a_last_period();
    const uint32_t seed = (id << 16) ^ count_a ^ 0xA1u;

    beginTaskA(id);
    ack_high(ACK_A);
    const uint32_t token = WorkKernel(BUDGET_A_CYCLES, seed);
    ack_low(ACK_A);
    endTaskA();

    g_state.token_a = token;
    g_state.token_a_valid = true;

    printf("A,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n", id, count_a, token);
}


static void task_b(void)
{
    const uint32_t id = g_state.idx_b;
    const uint32_t count_b = edge_count_b_last_period();
    const uint32_t seed = (id << 16) ^ count_b ^ 0xB2u;

    beginTaskB(id);
    ack_high(ACK_B);
    const uint32_t token = WorkKernel(BUDGET_B_CYCLES, seed);
    ack_low(ACK_B);
    endTaskB();

    g_state.token_b = token;
    g_state.token_b_valid = true;

    printf("B,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n", id, count_b, token);
}

static void task_agg(void)
{
    const uint32_t id = g_state.idx_agg;
    const uint32_t agg = (g_state.token_a_valid && g_state.token_b_valid)
                             ? (g_state.token_a ^ g_state.token_b)
                             : AGG_FALLBACK_TOKEN;
    const uint32_t seed = (id << 16) ^ agg ^ 0xD4u;

    beginTaskAGG(id);
    ack_high(ACK_AGG);
    const uint32_t token = WorkKernel(BUDGET_AGG_CYCLES, seed);
    ack_low(ACK_AGG);
    endTaskAGG();

    printf("AGG,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n", id, agg, token);
}

static void task_c(void)
{
    const uint32_t id = g_state.idx_c;
    const uint32_t seed = (id << 16) ^ 0xC3u;

    beginTaskC(id);
    ack_high(ACK_C);
    const uint32_t token = WorkKernel(BUDGET_C_CYCLES, seed);
    ack_low(ACK_C);
    endTaskC();

    printf("C,%" PRIu32 ",%" PRIu32 "\n", id, token);
}

static void task_d(void)
{
    const uint32_t id = g_state.idx_d;
    const uint32_t seed = (id << 16) ^ 0xD5u;

    beginTaskD(id);
    ack_high(ACK_D);
    const uint32_t token = WorkKernel(BUDGET_D_CYCLES, seed);
    ack_low(ACK_D);
    endTaskD();

    printf("D,%" PRIu32 ",%" PRIu32 "\n", id, token);
}

static void task_s(void)
{
    const uint32_t id = g_state.idx_s;
    const uint32_t seed = (id << 16) ^ 0x55u;

    beginTaskS(id);
    ack_high(ACK_S);
    const uint32_t token = WorkKernel(BUDGET_S_CYCLES, seed);
    ack_low(ACK_S);
    endTaskS();

    printf("S,%" PRIu32 ",%" PRIu32 "\n", id, token);
}

void app_main(void)
{
    esp_task_wdt_deinit(); // disable task watchdog timer for testing
    monitorInit();
    monitorSetPeriodicReportEverySeconds(0);
    monitorSetFinalReportAfterSeconds(0);

    gpio_init();

    ESP_LOGI(TAG, "Waiting for SYNC");
    const int64_t t0_us = wait_for_sync_rising_edge();
    reset_schedule_state(t0_us);
    synch();
    __atomic_store_n(&g_schedule_started, true, __ATOMIC_RELEASE);

    ESP_LOGI(TAG, "Assignment 2 super-loop started at T0=%" PRIi64 " us", t0_us);

    while (true) {
        int64_t now = now_us();

        while (now >= g_state.next_a_us) {
            task_a();
            g_state.idx_a++;
            g_state.next_a_us += PERIOD_A_US;
            now = now_us();
        }

        while (now >= g_state.next_b_us) {
            task_b();
            g_state.idx_b++;
            g_state.next_b_us += PERIOD_B_US;
            now = now_us();
        }

        while (now >= g_state.next_agg_us) {
            task_agg();
            g_state.idx_agg++;
            g_state.next_agg_us += PERIOD_AGG_US;
            now = now_us();
        }

        while (now >= g_state.next_c_us) {
            task_c();
            g_state.idx_c++;
            g_state.next_c_us += PERIOD_C_US;
            now = now_us();
        }

        while (now >= g_state.next_d_us) {
            task_d();
            g_state.idx_d++;
            g_state.next_d_us += PERIOD_D_US;
            now = now_us();
        }

        now = now_us();
        if (should_run_sporadic(now) && try_take_sporadic_release()) {
            task_s();
            g_state.idx_s++;
            continue;
        }

        if (monitorPollReports()) {
            ESP_LOGI(TAG, "Final monitor report printed");
        }
    }
}
