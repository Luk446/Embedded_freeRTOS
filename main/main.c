// ----- Includes ----

#include <inttypes.h>
#include <stdbool.h> // boolean type
#include <stdint.h>
#include <stdio.h> // printf logging

#include "driver/gpio.h" // hardware control
#include "driver/pulse_cnt.h" // hardware pulse counter (PCNT)
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h" // monitor logging
#include "esp_timer.h"
#include "monitor.h"

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

// Cyclic executive frame constants
#define FRAME_US        10000LL   //  Task A period (10 ms)
#define SLOTS_PER_CYCLE 10u       // hyperperiod = 100 ms

// Sporadic guard: only start task S if enough slack remains before the next frame boundary
#define SPORADIC_WCET_US   2600LL  // S WorkKernel budget (600000 / 240 MHz) + overhead margin
#define FRAME_GUARD_US      300LL  // extra safety margin before frame boundary
#define MONITOR_LINE_BUDGET_US 6500LL // one monitor line at 115200 baud with margin

#define AGG_FALLBACK_TOKEN 0xDEADBEEFu

// work kernel budgets from the docs (conditional on device)
#define BUDGET_A_CYCLES 672000u
#define BUDGET_B_CYCLES 960000u
#define BUDGET_AGG_CYCLES 480000u
#define BUDGET_C_CYCLES 1680000u
#define BUDGET_D_CYCLES 960000u
#define BUDGET_S_CYCLES 600000u

// Timing-focused mode: disable high-rate UART task logs by default.
// Set to 1 when you need per-job functional UART traces.
#define TASK_UART_LOG_ENABLED 0

// Local scheduler-friendly monitor cadence in seconds.
#define SCHED_MONITOR_REPORT_EVERY_S 4u

#if TASK_UART_LOG_ENABLED
#define TASK_LOG(...) printf(__VA_ARGS__)
#define TASK_LOG_USE(v) do { } while (0)
#else
#define TASK_LOG(...) do { } while (0)
#define TASK_LOG_USE(v) do { (void)(v); } while (0)
#endif


static const char *TAG = "assignment2";

// declare the work kernel
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

    // validity tokens
    bool token_a_valid;
    bool token_b_valid;
    uint32_t token_a;
    uint32_t token_b;

} sched_state_t;

// global instance of the scheduler state
static sched_state_t g_state;

// edge counts latched at the frame boundary and consumed by task_a() / task_b()
static uint32_t g_latched_count_a = 0u;
static uint32_t g_latched_count_b = 0u;

// PCNT unit handles and previous-count baselines for delta calculation
static pcnt_unit_handle_t g_pcnt_unit_a = NULL;
static pcnt_unit_handle_t g_pcnt_unit_b = NULL;
static int g_pcnt_prev_a = 0;
static int g_pcnt_prev_b = 0;
static uint32_t g_frame_overrun_count = 0u;
static int64_t g_last_overrun_log_us = 0;

typedef struct {
    const char *name;
    int64_t deadline_us;
    uint32_t jobs;
    uint32_t misses;
    int64_t start_us;
    int64_t release_us;
    int64_t max_exec_us;
    int64_t worst_late_us;
    bool active;
} sched_monitor_task_t;

typedef struct {
    int64_t t0_us;
    sched_monitor_task_t a;
    sched_monitor_task_t b;
    sched_monitor_task_t agg;
    sched_monitor_task_t c;
    sched_monitor_task_t d;
    sched_monitor_task_t s;
} sched_monitor_snapshot_t;

static sched_monitor_task_t g_mon_a = {.name = "A", .deadline_us = 10000};
static sched_monitor_task_t g_mon_b = {.name = "B", .deadline_us = 20000};
static sched_monitor_task_t g_mon_agg = {.name = "AGG", .deadline_us = 20000};
static sched_monitor_task_t g_mon_c = {.name = "C", .deadline_us = 50000};
static sched_monitor_task_t g_mon_d = {.name = "D", .deadline_us = 50000};
static sched_monitor_task_t g_mon_s = {.name = "S", .deadline_us = 30000};

static sched_monitor_snapshot_t g_mon_snapshot;
static int64_t g_mon_t0_us = 0;
static int64_t g_mon_next_report_us = 0;
static bool g_mon_report_pending = false;
static uint32_t g_mon_report_line = 0;

#define LOCAL_S_RELEASE_Q_MAX 32
static volatile int64_t g_local_s_release_q[LOCAL_S_RELEASE_Q_MAX];
static volatile uint32_t g_local_s_release_q_head = 0;
static volatile uint32_t g_local_s_release_q_tail = 0;
static volatile uint32_t g_local_s_release_q_count = 0;

// global variables shared between loop and interrupts
static volatile uint32_t g_pending_s = 0;
static volatile uint32_t g_sync_time_us = 0;
static volatile bool g_sync_seen = false;
static volatile bool g_schedule_started = false;

// ----- Interrupt service routines and helpers ----

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

static void sched_monitor_reset_task(sched_monitor_task_t *task)
{
    task->jobs = 0;
    task->misses = 0;
    task->start_us = 0;
    task->release_us = 0;
    task->max_exec_us = 0;
    task->worst_late_us = 0;
    task->active = false;
}

static void sched_monitor_reset(int64_t t0_us)
{
    g_mon_t0_us = t0_us;
    sched_monitor_reset_task(&g_mon_a);
    sched_monitor_reset_task(&g_mon_b);
    sched_monitor_reset_task(&g_mon_agg);
    sched_monitor_reset_task(&g_mon_c);
    sched_monitor_reset_task(&g_mon_d);
    sched_monitor_reset_task(&g_mon_s);
    g_mon_report_pending = false;
    g_mon_report_line = 0;
    g_mon_next_report_us = (SCHED_MONITOR_REPORT_EVERY_S == 0u)
        ? 0
        : (t0_us + ((int64_t)SCHED_MONITOR_REPORT_EVERY_S * 1000000LL));
    g_local_s_release_q_head = 0;
    g_local_s_release_q_tail = 0;
    g_local_s_release_q_count = 0;
}                       

static void sched_monitor_begin_task(sched_monitor_task_t *task, int64_t release_us)
{
    task->active = true;
    task->release_us = release_us;
    task->start_us = now_us();
}

static void sched_monitor_end_task(sched_monitor_task_t *task)
{
    if (!task->active) {
        return;
    }

    const int64_t end_us = now_us();
    const int64_t exec_us = end_us - task->start_us;
    const int64_t late_us = end_us - (task->release_us + task->deadline_us);

    if (exec_us > task->max_exec_us) {
        task->max_exec_us = exec_us;
    }
    if (late_us > task->worst_late_us) {
        task->worst_late_us = late_us;
    }

    task->jobs++;
    if (late_us > 0) {
        task->misses++;
    }

    task->active = false;
}

static void sched_monitor_capture_snapshot(void)
{
    g_mon_snapshot.t0_us = g_mon_t0_us;
    g_mon_snapshot.a = g_mon_a;
    g_mon_snapshot.b = g_mon_b;
    g_mon_snapshot.agg = g_mon_agg;
    g_mon_snapshot.c = g_mon_c;
    g_mon_snapshot.d = g_mon_d;
    g_mon_snapshot.s = g_mon_s;
    g_mon_report_pending = true;
    g_mon_report_line = 0;
}

static void sched_monitor_queue_due_report(void)
{
    if (g_mon_next_report_us == 0) {
        return;
    }

    const int64_t now = now_us();
    if (now < g_mon_next_report_us) {
        return;
    }

    const int64_t period_us = (int64_t)SCHED_MONITOR_REPORT_EVERY_S * 1000000LL;
    const int64_t missed_periods = (now - g_mon_next_report_us) / period_us;
    g_mon_next_report_us += (missed_periods + 1) * period_us;

    if (!g_mon_report_pending) {
        sched_monitor_capture_snapshot();
    }
}

static void sched_monitor_print_task(const sched_monitor_task_t *task)
{
    printf("[MON] %s jobs=%" PRIu32 " misses=%" PRIu32 " max_exec=%" PRIi64
           "us worst_late=%" PRIi64 "us\n",
           task->name, task->jobs, task->misses, task->max_exec_us, task->worst_late_us);
}

static void sched_monitor_service_line(int64_t frame_end_us)
{
    if (!g_mon_report_pending) {
        return;
    }

    if ((frame_end_us - now_us()) < (MONITOR_LINE_BUDGET_US + FRAME_GUARD_US)) {
        return;
    }

    switch (g_mon_report_line) {
        case 0:
            printf("[MON] T0=%" PRIi64 " us\n", g_mon_snapshot.t0_us);
            break;
        case 1:
            sched_monitor_print_task(&g_mon_snapshot.a);
            break;
        case 2:
            sched_monitor_print_task(&g_mon_snapshot.b);
            break;
        case 3:
            sched_monitor_print_task(&g_mon_snapshot.agg);
            break;
        case 4:
            sched_monitor_print_task(&g_mon_snapshot.c);
            break;
        case 5:
            sched_monitor_print_task(&g_mon_snapshot.d);
            break;
        case 6:
            sched_monitor_print_task(&g_mon_snapshot.s);
            g_mon_report_pending = false;
            g_mon_report_line = 0;
            return;
        default:
            g_mon_report_pending = false;
            g_mon_report_line = 0;
            return;
    }

    g_mon_report_line++;
}

static void IRAM_ATTR sched_monitor_notify_s_release(void)
{
    const int64_t release_us = esp_timer_get_time();
    if (g_local_s_release_q_count < LOCAL_S_RELEASE_Q_MAX) {
        g_local_s_release_q[g_local_s_release_q_tail] = release_us;
        g_local_s_release_q_tail = (g_local_s_release_q_tail + 1u) % LOCAL_S_RELEASE_Q_MAX;
        g_local_s_release_q_count++;
    }
}

static int64_t sched_monitor_take_s_release(void)
{
    int64_t release_us = now_us();
    if (g_local_s_release_q_count > 0u) {
        release_us = g_local_s_release_q[g_local_s_release_q_head];
        g_local_s_release_q_head = (g_local_s_release_q_head + 1u) % LOCAL_S_RELEASE_Q_MAX;
        g_local_s_release_q_count--;
    }
    return release_us;
}

// ISR for sporadic task release (IN_S) - edge counting for A and B is done in hardware (PCNT)

static void IRAM_ATTR isr_in_s(void *arg)
{
    (void)arg;

    if (!__atomic_load_n(&g_schedule_started, __ATOMIC_RELAXED)) {
        return;
    }

    __atomic_fetch_add(&g_pending_s, 1u, __ATOMIC_RELAXED);
    sched_monitor_notify_s_release();
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

// ----- GPIO/PCNT init and scheduler setup -----

// Configure SYNC/S input GPIOs, ACK outputs, and ISR hooks for SYNC and IN_S.
// IN_A and IN_B are configured by the PCNT driver in pcnt_init().
static void gpio_init(void)
{
    // configure input pins (IN_A and IN_B are owned by the PCNT driver)
    const gpio_config_t input_conf = {
        .pin_bit_mask = (1ULL << PIN_SYNC) |
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

    // set pull modes (IN_A and IN_B pull modes are configured by the PCNT driver)
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_SYNC, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_S, GPIO_PULLDOWN_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PIN_IN_MODE, GPIO_PULLDOWN_ONLY));

    // initialize ACKs to low
    ack_low(ACK_A);
    ack_low(ACK_B);
    ack_low(ACK_AGG);
    ack_low(ACK_C);
    ack_low(ACK_D);
    ack_low(ACK_S);

    // set up interrupts for SYNC and IN_S (IN_A and IN_B are counted by PCNT hardware)
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_SYNC, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_set_intr_type(PIN_IN_S, GPIO_INTR_POSEDGE));

    // attach ISRs
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_SYNC, isr_sync, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN_S, isr_in_s, NULL));
}

// configure two PCNT units to count rising edges on IN_A and IN_B in hardware
static void pcnt_init(void)
{
    // accum_count=1: the driver accumulates overflows so pcnt_unit_get_count() returns a
    // monotonically increasing total rather than a raw 16-bit value that wraps
    const pcnt_unit_config_t unit_cfg = {
        .low_limit  = -1,
        .high_limit = 30000,
        .flags      = { .accum_count = 1 },
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &g_pcnt_unit_a));
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &g_pcnt_unit_b));

    // 1 us glitch filter: reject narrow noise pulses but keep valid square-wave edges
    const pcnt_glitch_filter_config_t filter_cfg = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(g_pcnt_unit_a, &filter_cfg));
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(g_pcnt_unit_b, &filter_cfg));

    // channel A: increment on rising edge of PIN_IN_A, hold on falling edge
    const pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num  = PIN_IN_A,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t chan_a;
    ESP_ERROR_CHECK(pcnt_new_channel(g_pcnt_unit_a, &chan_a_cfg, &chan_a));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a,
                                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                 PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_a,
                                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP));

    // channel B: increment on rising edge of PIN_IN_B, hold on falling edge
    const pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num  = PIN_IN_B,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t chan_b;
    ESP_ERROR_CHECK(pcnt_new_channel(g_pcnt_unit_b, &chan_b_cfg, &chan_b));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_b,
                                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                 PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_b,
                                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP));

    // zero counters, then start both units
    ESP_ERROR_CHECK(pcnt_unit_enable(g_pcnt_unit_a));
    ESP_ERROR_CHECK(pcnt_unit_enable(g_pcnt_unit_b));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(g_pcnt_unit_a));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(g_pcnt_unit_b));
    ESP_ERROR_CHECK(pcnt_unit_start(g_pcnt_unit_a));
    ESP_ERROR_CHECK(pcnt_unit_start(g_pcnt_unit_b));
}

// Block until the first SYNC rising edge and return its timestamp.
static int64_t wait_for_sync_rising_edge(void)
{
    __atomic_store_n(&g_sync_seen, false, __ATOMIC_RELEASE);

    while (gpio_get_level(PIN_SYNC) != 0) {
    }

    while (!__atomic_load_n(&g_sync_seen, __ATOMIC_ACQUIRE)) {
    }

    return (int64_t)__atomic_load_n(&g_sync_time_us, __ATOMIC_ACQUIRE);
}

// Initialize scheduler counters and tokens for a new run at T0.
static void reset_schedule_state(int64_t t0_us)
{
    g_state.idx_a = 0;
    g_state.idx_b = 0;
    g_state.idx_agg = 0;
    g_state.idx_c = 0;
    g_state.idx_d = 0;
    g_state.idx_s = 0;
    // Reset PCNT counters and baselines so the first counting window starts cleanly at T0
    ESP_ERROR_CHECK(pcnt_unit_clear_count(g_pcnt_unit_a));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(g_pcnt_unit_b));
    g_pcnt_prev_a = 0;
    g_pcnt_prev_b = 0;
    g_latched_count_a = 0u;
    g_latched_count_b = 0u;
    g_state.token_a_valid = false;
    g_state.token_b_valid = false;
    g_state.token_a = 0;
    g_state.token_b = 0;
    g_frame_overrun_count = 0u;
    g_last_overrun_log_us = t0_us;
    sched_monitor_reset(t0_us);
    __atomic_store_n(&g_pending_s, 0u, __ATOMIC_RELAXED);
}

// Read PCNT hardware count and return the delta since the last call (delta-per-period method).
// Bounded O(1) execution: one register read and one integer subtraction, no CPU ISR overhead.
static uint32_t edge_count_a_last_period(void)
{
    int current = 0;
    pcnt_unit_get_count(g_pcnt_unit_a, &current);
    const uint32_t delta = (uint32_t)(current - g_pcnt_prev_a);
    g_pcnt_prev_a = current;
    return delta;
}

static uint32_t edge_count_b_last_period(void)
{
    int current = 0;
    pcnt_unit_get_count(g_pcnt_unit_b, &current);
    const uint32_t delta = (uint32_t)(current - g_pcnt_prev_b);
    g_pcnt_prev_b = current;
    return delta;
}

// attempt to take a sporadic release
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

// ----- Task implementations -----

/*
Task execution pattern used by each periodic/sporadic task:
- read current task index and derive seed inputs
- assert ACK and notify monitor task-begin
- execute WorkKernel() with the task budget
- deassert ACK and notify monitor task-end
- update shared token state (where relevant)
- print UART log line for tester validation
*/
static void task_a(void)
{
    const uint32_t id = g_state.idx_a;
    const uint32_t count_a = g_latched_count_a;  // sampled at frame boundary
    const uint32_t seed = (id << 16) ^ count_a ^ 0xA1u;
    const int64_t release_us = g_mon_t0_us + ((int64_t)id * FRAME_US);

    sched_monitor_begin_task(&g_mon_a, release_us);
    beginTaskA(id);
    ack_high(ACK_A);
    const uint32_t token = WorkKernel(BUDGET_A_CYCLES, seed);
    ack_low(ACK_A);
    endTaskA();
    sched_monitor_end_task(&g_mon_a);

    g_state.token_a = token;
    g_state.token_a_valid = true;

    TASK_LOG("A,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n", id, count_a, token);
}


static void task_b(void)
{
    const uint32_t id = g_state.idx_b;
    const uint32_t count_b = g_latched_count_b;  // sampled at frame boundary
    const uint32_t seed = (id << 16) ^ count_b ^ 0xB2u;
    const int64_t release_us = g_mon_t0_us + ((int64_t)id * 20000LL);

    sched_monitor_begin_task(&g_mon_b, release_us);
    beginTaskB(id);
    ack_high(ACK_B);
    const uint32_t token = WorkKernel(BUDGET_B_CYCLES, seed);
    ack_low(ACK_B);
    endTaskB();
    sched_monitor_end_task(&g_mon_b);

    g_state.token_b = token;
    g_state.token_b_valid = true;

    TASK_LOG("B,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n", id, count_b, token);
}

static void task_agg(void)
{
    const uint32_t id = g_state.idx_agg;
    const uint32_t agg = (g_state.token_a_valid && g_state.token_b_valid)
                             ? (g_state.token_a ^ g_state.token_b)
                             : AGG_FALLBACK_TOKEN;
    const uint32_t seed = (id << 16) ^ agg ^ 0xD4u;
    const int64_t release_us = g_mon_t0_us + ((int64_t)id * 20000LL);

    sched_monitor_begin_task(&g_mon_agg, release_us);
    beginTaskAGG(id);
    ack_high(ACK_AGG);
    const uint32_t token = WorkKernel(BUDGET_AGG_CYCLES, seed);
    ack_low(ACK_AGG);
    endTaskAGG();
    sched_monitor_end_task(&g_mon_agg);

    TASK_LOG_USE(token);
    TASK_LOG("AGG,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n", id, agg, token);
}

static void task_c(void)
{
    const uint32_t id = g_state.idx_c;
    const uint32_t seed = (id << 16) ^ 0xC3u;
    const int64_t release_us = g_mon_t0_us + ((int64_t)id * 50000LL);

    sched_monitor_begin_task(&g_mon_c, release_us);
    beginTaskC(id);
    ack_high(ACK_C);
    const uint32_t token = WorkKernel(BUDGET_C_CYCLES, seed);
    ack_low(ACK_C);
    endTaskC();
    sched_monitor_end_task(&g_mon_c);

    TASK_LOG_USE(token);
    TASK_LOG("C,%" PRIu32 ",%" PRIu32 "\n", id, token);
}

static void task_d(void)
{
    const uint32_t id = g_state.idx_d;
    const uint32_t seed = (id << 16) ^ 0xD5u;
    const int64_t release_us = g_mon_t0_us + ((int64_t)id * 50000LL);

    sched_monitor_begin_task(&g_mon_d, release_us);
    beginTaskD(id);
    ack_high(ACK_D);
    const uint32_t token = WorkKernel(BUDGET_D_CYCLES, seed);
    ack_low(ACK_D);
    endTaskD();
    sched_monitor_end_task(&g_mon_d);

    TASK_LOG_USE(token);
    TASK_LOG("D,%" PRIu32 ",%" PRIu32 "\n", id, token);
}

static void task_s(void)
{
    const uint32_t id = g_state.idx_s;
    const uint32_t seed = (id << 16) ^ 0x55u;
    const int64_t release_us = sched_monitor_take_s_release();

    sched_monitor_begin_task(&g_mon_s, release_us);
    beginTaskS(id);
    ack_high(ACK_S);
    const uint32_t token = WorkKernel(BUDGET_S_CYCLES, seed);
    ack_low(ACK_S);
    endTaskS();
    sched_monitor_end_task(&g_mon_s);

    TASK_LOG_USE(token);
    TASK_LOG("S,%" PRIu32 ",%" PRIu32 "\n", id, token);
}

// ----- Cyclic executive frame dispatch -----

// Latch edge-count deltas at the start of each frame, before any task runs.
// A: every frame (10 ms period).
// B: every even frame (20 ms period) so the delta window is exactly 20 ms.
static void latch_counts_for_slot(uint32_t slot)
{
    g_latched_count_a = edge_count_a_last_period();
    if ((slot & 1u) == 0u) {
        g_latched_count_b = edge_count_b_last_period();
    }
}

// Fixed slot dispatch for the 100 ms major cycle (10 frames x 10 ms).
//
// Frame layout:
//   0: A, B, AGG   B@0  released t=  0 ms, deadline t= 20 ms
//   1: A, C        C@0  released t=  0 ms, deadline t= 50 ms
//   2: A, B, AGG   B@1  released t= 20 ms, deadline t= 40 ms
//   3: A, D        D@0  released t=  0 ms, deadline t= 50 ms  (slack for S)
//   4: A, B, AGG   B@2  released t= 40 ms, deadline t= 60 ms
//   5: A           (free frame - maximum slack for S)
//   6: A, B, AGG   B@3  released t= 60 ms, deadline t= 80 ms
//   7: A, C        C@1  released t= 50 ms, deadline t=100 ms
//   8: A, B, AGG   B@4  released t= 80 ms, deadline t=100 ms
//   9: A, D        D@1  released t= 50 ms, deadline t=100 ms  (slack for S)
static void run_slot(uint32_t slot)
{
    switch (slot) {
        case 0: task_a(); g_state.idx_a++; task_b(); g_state.idx_b++; task_agg(); g_state.idx_agg++; break;
        case 1: task_a(); g_state.idx_a++; task_c(); g_state.idx_c++;                                 break;
        case 2: task_a(); g_state.idx_a++; task_b(); g_state.idx_b++; task_agg(); g_state.idx_agg++; break;
        case 3: task_a(); g_state.idx_a++; task_d(); g_state.idx_d++;                                 break;
        case 4: task_a(); g_state.idx_a++; task_b(); g_state.idx_b++; task_agg(); g_state.idx_agg++; break;
        case 5: task_a(); g_state.idx_a++;                                                             break;
        case 6: task_a(); g_state.idx_a++; task_b(); g_state.idx_b++; task_agg(); g_state.idx_agg++; break;
        case 7: task_a(); g_state.idx_a++; task_c(); g_state.idx_c++;                                 break;
        case 8: task_a(); g_state.idx_a++; task_b(); g_state.idx_b++; task_agg(); g_state.idx_agg++; break;
        case 9: task_a(); g_state.idx_a++; task_d(); g_state.idx_d++;                                 break;
        default: break;
    }
}

// Run pending sporadic jobs while there is enough slack before the next frame boundary.
static void run_sporadic_if_budget(int64_t frame_end_us)
{
    while (__atomic_load_n(&g_pending_s, __ATOMIC_ACQUIRE) > 0u) {
        if ((frame_end_us - now_us()) < (SPORADIC_WCET_US + FRAME_GUARD_US)) {
            break;
        }
        if (!try_take_sporadic_release()) {
            break;
        }
        task_s();
        g_state.idx_s++;
    }
}

// ----- Main (cyclic executive) loop -----


void app_main(void)
{
    monitorInit(); // initialize the monitor

    gpio_init(); // initialize GPIOs and SYNC/IN_S interrupts
    pcnt_init(); // start hardware edge counters for IN_A and IN_B

    ESP_LOGI(TAG, "Waiting for SYNC"); // wait for the sync signal to start the scheduler and get the T0 time
    const int64_t t0_us = wait_for_sync_rising_edge(); // init scheduler state with T0 and start main loop
    reset_schedule_state(t0_us); // initialize the scheduler state with T0 and other initial values
    synch(); // synchronize with monitor that schedule is starting
    __atomic_store_n(&g_schedule_started, true, __ATOMIC_RELEASE); // allow sporadic task to be scheduled

    ESP_LOGI(TAG, "Assignment 2 super-loop started at T0=%" PRIi64 " us", t0_us); // log the start of the super-loop

    // Cyclic executive: advance one 10 ms frame at a time, dispatching tasks by
    // their fixed slot in the 100 ms major cycle.
    uint32_t frame_counter = 0u;
    int64_t next_frame_start = t0_us;

    while (true) {
        // wait for the start of this frame
        while (now_us() < next_frame_start) {}

        const uint32_t slot       = frame_counter % SLOTS_PER_CYCLE;
        const int64_t  frame_end  = next_frame_start + FRAME_US;

        // latch edge counts before any task runs this frame
        latch_counts_for_slot(slot);

        // run the periodic tasks assigned to this slot
        run_slot(slot);

        // use remaining frame slack for any pending sporadic jobs
        run_sporadic_if_budget(frame_end);

        // Queue and drain one local monitor line from the free frame.
        sched_monitor_queue_due_report();
        if (slot == 5u) {
            sched_monitor_service_line(frame_end);
        }

        // detect frame overrun (logged before the idle busy-wait)
        const int64_t now_after = now_us();
        if (now_after > frame_end) {
            g_frame_overrun_count++;

            // Throttle overrun logs: printing every frame causes a feedback loop that
            // can dominate CPU time and worsen deadline misses.
            if ((now_after - g_last_overrun_log_us) >= 1000000LL) {
                ESP_LOGW(TAG, "Frame overruns=%" PRIu32 ", last slot=%" PRIu32 ", last over=%" PRIi64 " us",
                         g_frame_overrun_count, slot, now_after - frame_end);
                g_last_overrun_log_us = now_after;
            }
        }

        // idle until the frame boundary
        while (now_us() < frame_end) {}

        frame_counter++;
        next_frame_start += FRAME_US;
    }
}
