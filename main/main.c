// ----- Includes ----

#include <inttypes.h>
#include <stdbool.h> // bools 
#include <stdint.h>
#include <stdio.h> // printing

#include "driver/gpio.h" // hardware control
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h" // monitor logging
#include "esp_timer.h"
#include "monitor.h"
#include "esp_task_wdt.h" // manage watchdog for debug

// ----- Pin definitions -----

// inputs
#define PIN_SYNC 13
#define PIN_IN_A 14
#define PIN_IN_B 25
#define PIN_IN_S 26
#define PIN_IN_MODE 27

// outputs (ACKs)
#define ACK_A 16
#define ACK_B 17
#define ACK_AGG 18
#define ACK_C 19
#define ACK_D 21
#define ACK_S 22

// ----- Task parameters -----

// Periods in microseconds
#define PERIOD_A_US 10000LL
#define PERIOD_B_US 20000LL
#define PERIOD_AGG_US 20000LL
#define PERIOD_C_US 50000LL
#define PERIOD_D_US 50000LL

// scheduling logic constants
#define SPORADIC_SLACK_GUARD_US 3000LL
#define AGG_FALLBACK_TOKEN 0xDEADBEEFu

// work kernel budgets from the docs (conditional on device)
#define BUDGET_A_CYCLES 672000u
#define BUDGET_B_CYCLES 960000u
#define BUDGET_AGG_CYCLES 480000u
#define BUDGET_C_CYCLES 1680000u
#define BUDGET_D_CYCLES 960000u
#define BUDGET_S_CYCLES 600000u


static const char *TAG = "assignment2";

// delcare the work kernel
uint32_t WorkKernel(uint32_t budget_cycles, uint32_t seed);

// ----- State variables -----

// core of the scheduling logic - tracking state of all tasks and releases
typedef struct {

    // task indices
    uint32_t idx_a;
    uint32_t idx_b;
    uint32_t idx_agg;
    uint32_t idx_c;
    uint32_t idx_d;
    uint32_t idx_s;

    // edge counts
    uint32_t last_edge_count_a;
    uint32_t last_edge_count_b;

    // next release times
    int64_t next_a_us;
    int64_t next_b_us;
    int64_t next_agg_us;
    int64_t next_c_us;
    int64_t next_d_us;

    // validity tokens
    bool token_a_valid;
    bool token_b_valid;
    uint32_t token_a;
    uint32_t token_b;

} sched_state_t;

// global instannce of the scheduler state
static sched_state_t g_state;

// global variables shared between loop and interrupts
static volatile uint32_t g_edge_count_a = 0;
static volatile uint32_t g_edge_count_b = 0;
static volatile uint32_t g_pending_s = 0;
static volatile uint32_t g_sync_time_us = 0;
static volatile bool g_sync_seen = false;
static volatile bool g_schedule_started = false;

// ----- interrupt service routines and helpers ----

// helper functions for GPIO control and time

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

// ISRs for each input pin (using esp32 internal ram) and atomic built ins for safe shared variable access between ISRs and main loop

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

// ----- GPIO init and scheduler setup -----

// configure pins for input and output, set pull modes, initialize ACKs to low, and set up interrupts for inputs
static void gpio_init(void)
{
    // configure input pins
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

    // configure output pins (ACKs)
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

    // apply configurations
    ESP_ERROR_CHECK(gpio_config(&input_conf));
    ESP_ERROR_CHECK(gpio_config(&output_conf));

    // set pull modes
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_SYNC, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_S, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_MODE, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_A, GPIO_FLOATING));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_B, GPIO_FLOATING));

    // initialize ACKs to low
    ack_low(ACK_A);
    ack_low(ACK_B);
    ack_low(ACK_AGG);
    ack_low(ACK_C);
    ack_low(ACK_D);
    ack_low(ACK_S);

    // set up interrupts for inputs
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_SYNC, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_IN_A, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_IN_B, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_IN_S, GPIO_INTR_POSEDGE));

    // attach ISRs
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_SYNC, isr_sync, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_A, isr_in_a, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_B, isr_in_b, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_S, isr_in_s, NULL));
}

// block the program starting until the rising edge from the sync pin (button) is seen 
static int64_t wait_for_sync_rising_edge(void)
{
    __atomic_store_n(&g_sync_seen, false, __ATOMIC_RELEASE);

    while (gpio_get_level(PIN_SYNC) != 0) {
    }

    while (!__atomic_load_n(&g_sync_seen, __ATOMIC_ACQUIRE)) {
    }

    return (int64_t)__atomic_load_n(&g_sync_time_us, __ATOMIC_ACQUIRE);
}

// now once the T0 time is obtained from the sync signal, initialize the scheduler state and variables to be ready for the main loop scheduling logic

// initialize the scheduler 
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

// 
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

// ----- Task implementations -----

/*for each task :
- get current task id
- perform task specific calcs 
- calc the seed val
- signal start of task with monitor and ACK high
- run the work kernel for the task's budget and seed
- signal end of task with monitor and ACK low
- update the global state of task 
- print the results to serial monitor
*/
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

// ----- Main (super) loop -----


void app_main(void)
{
    esp_task_wdt_deinit(); // disable task watchdog timer for testing
    monitorInit(); // initialize the monitor
    monitorSetPeriodicReportEverySeconds(4); // enable periodic reporting every n seconds
    monitorSetFinalReportAfterSeconds(0); // disable final report timer

    gpio_init(); // initialize GPIOs and interrupts

    ESP_LOGI(TAG, "Waiting for SYNC"); // wait for the sync signal to start the scheduler and get the T0 time
    const int64_t t0_us = wait_for_sync_rising_edge(); // init scheduler state with T0 and start main loop
    reset_schedule_state(t0_us); // initialize the scheduler state with T0 and other initial values
    synch(); // synchronize with monitor that schedule is starting
    __atomic_store_n(&g_schedule_started, true, __ATOMIC_RELEASE); // allow sporadic task to be scheduled

    ESP_LOGI(TAG, "Assignment 2 super-loop started at T0=%" PRIi64 " us", t0_us); // log the start of the super-loop

    // super loop scheduling
    /* for each task check if the time is past the next release time for that task - if it is , task function is called and the next release time for that task is updated based on its period. 
    */
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
