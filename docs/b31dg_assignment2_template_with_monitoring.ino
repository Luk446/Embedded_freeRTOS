#include <Arduino.h>
#include <inttypes.h>
#include <stdint.h>

// ===== Pin mapping (adjust to your wiring) =====
static const int PIN_SYNC = 4;
static const int PIN_IN_A = 16;
static const int PIN_IN_B = 17;
static const int PIN_IN_S = 18;
static const int PIN_IN_MODE = 19;  // unused in Assignment 2 super-loop

static const int PIN_ACK_A = 21;
static const int PIN_ACK_B = 22;
static const int PIN_ACK_AGG = 23;
static const int PIN_ACK_C = 25;
static const int PIN_ACK_D = 26;
static const int PIN_ACK_S = 27;
static const int PIN_ERR = 33; // OPTIONAL, LED TO SIGNAL ANY MISSING DEADLOINE

// ===== Budget profile, set to match the clock of your board =====
#define BOARD_PROFILE_240MHZ 1
#define BOARD_PROFILE_160MHZ 2

#ifndef BOARD_PROFILE
#define BOARD_PROFILE BOARD_PROFILE_240MHZ
#endif

#if BOARD_PROFILE == BOARD_PROFILE_240MHZ
// Assignment budgets @ 240MHz.
static const uint32_t BUDGET_A_CYCLES = 672000u;
static const uint32_t BUDGET_B_CYCLES = 960000u;
static const uint32_t BUDGET_AGG_CYCLES = 480000u;
static const uint32_t BUDGET_C_CYCLES = 1680000u;
static const uint32_t BUDGET_D_CYCLES = 960000u;
static const uint32_t BUDGET_S_CYCLES = 600000u;
#elif BOARD_PROFILE == BOARD_PROFILE_160MHZ
// Scaled by 2/3 for 160MHz-class targets (e.g., Wokwi ESP32, ESP32-C3).
static const uint32_t BUDGET_A_CYCLES = 448000u;
static const uint32_t BUDGET_B_CYCLES = 640000u;
static const uint32_t BUDGET_AGG_CYCLES = 320000u;
static const uint32_t BUDGET_C_CYCLES = 1120000u;
static const uint32_t BUDGET_D_CYCLES = 640000u;
static const uint32_t BUDGET_S_CYCLES = 400000u;
#else
#error "Invalid BOARD_PROFILE value"
#endif

static const uint32_t FINAL_REPORT_AFTER_SECONDS = 2u; // for the monitor, e.g. print the report after 2 seconds

// ===== WorkKernel (must not be modified for assignment marking) =====
static inline uint32_t wk_get_cycle32() {
#if defined(__XTENSA__)
  uint32_t cycles;
  asm volatile("rsr %0, ccount" : "=a"(cycles));
  return cycles;
#elif defined(__riscv)
  uint32_t cycles;
  asm volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
#else
#error "Unsupported architecture for WorkKernel cycle counter"
#endif
}

#define COMPILER_BARRIER() asm volatile("" ::: "memory")

static inline uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

static uint32_t WorkKernel(uint32_t budget_cycles, uint32_t seed) {
  uint32_t start = wk_get_cycle32();
  uint32_t acc = 0x12345678u ^ seed;
  uint32_t x = 0x9E3779B9u ^ (seed * 0x85EBCA6Bu);

  while ((uint32_t)(wk_get_cycle32() - start) < budget_cycles) {
    x = mix32(x + 0x9E3779B9u);
    acc ^= x;
    acc = (acc << 5) | (acc >> 27);
    acc += 0xA5A5A5A5u;
    COMPILER_BARRIER();
  }
  return mix32(acc);
}

// ===== Monitoring class =====
class TimingMonitor {
 public:
  void setPeriodicReportEverySeconds(uint32_t seconds) {
    periodic_report_every_us_ = (uint64_t)seconds * 1000000ull;
    next_periodic_report_us_ = (periodic_report_every_us_ > 0u) ? (t0_ + periodic_report_every_us_) : 0u;
  }
  void setFinalReportAfterSeconds(uint32_t seconds) {
    final_report_after_us_ = (uint64_t)seconds * 1000000ull;
    final_report_deadline_us_ = (final_report_after_us_ > 0u) ? (t0_ + final_report_after_us_) : 0u;
    final_report_printed_ = false;
  }

  // Call once, after SYNCH is released
  void synch() {
    t0_ = micros();
    resetTask(a_);
    resetTask(b_);
    resetTask(agg_);
    resetTask(c_);
    resetTask(d_);
    resetTask(s_);
    s_q_head_ = 0;
    s_q_tail_ = 0;
    s_q_count_ = 0;
    next_periodic_report_us_ =
        (periodic_report_every_us_ > 0u) ? (t0_ + periodic_report_every_us_) : 0u;
    final_report_deadline_us_ = (final_report_after_us_ > 0u) ? (t0_ + final_report_after_us_) : 0u;
    final_report_printed_ = false;
  }

  // CALL TO NOTIFY EVERY RELEASE OF S
  void notifySRelease() {
    const uint64_t t = micros();
    if (s_q_count_ < S_RELEASE_Q_MAX) {
      s_release_us_[s_q_tail_] = t;
      s_q_tail_ = (s_q_tail_ + 1u) % S_RELEASE_Q_MAX;
      s_q_count_++;
    }
  }

  // CALL AT THE START AND END OF TASK EXECUTIONS
  void beginTaskA(uint32_t id) { beginTask(a_, id, t0_ + (uint64_t)id * 10000ull); }
  void endTaskA() { endTask(a_); }
  void beginTaskB(uint32_t id) { beginTask(b_, id, t0_ + (uint64_t)id * 20000ull); }
  void endTaskB() { endTask(b_); }
  void beginTaskAGG(uint32_t id) { beginTask(agg_, id, t0_ + (uint64_t)id * 20000ull); }
  void endTaskAGG() { endTask(agg_); }
  void beginTaskC(uint32_t id) { beginTask(c_, id, t0_ + (uint64_t)id * 50000ull); }
  void endTaskC() { endTask(c_); }
  void beginTaskD(uint32_t id) { beginTask(d_, id, t0_ + (uint64_t)id * 50000ull); }
  void endTaskD() { endTask(d_); }
  void beginTaskS(uint32_t id) { beginTask(s_, id, popSRelease()); }
  void endTaskS() { endTask(s_); }

  // returns true if all the deadlines have been met so far
  bool allDeadlinesMet() const {
    return a_.misses == 0 && b_.misses == 0 && agg_.misses == 0 && c_.misses == 0 &&
           d_.misses == 0 && s_.misses == 0;
  }

  // check if a report must be printed
  bool pollReports() {
    const uint64_t now = micros();
    if (periodic_report_every_us_ > 0u && now >= next_periodic_report_us_) {
      const uint64_t periods_missed =
          (now - next_periodic_report_us_) / periodic_report_every_us_;
      next_periodic_report_us_ += (periods_missed + 1u) * periodic_report_every_us_;
      report();
    }
    if (!final_report_printed_ && final_report_deadline_us_ > 0u &&
        now >= final_report_deadline_us_) {
      printFinalReport();
      final_report_printed_ = true;
      return true;
    }
    return false;
  }

  // Print task report
  void report() const {
    Serial.printf("[MON] T0=%llu us\n", t0_);
    reportOne("A", a_);
    reportOne("B", b_);
    reportOne("AGG", agg_);
    reportOne("C", c_);
    reportOne("D", d_);
    reportOne("S", s_);
  }

  void printFinalReport() const {
    Serial.println("FINAL_REPORT_BEGIN");
    report();
    Serial.println("FINAL_REPORT_END");
  }

 private:
  struct TaskStats {
    uint32_t jobs = 0;
    uint32_t misses = 0;
    uint32_t id = 0;
    bool active = false;
    uint64_t start_us = 0;
    uint64_t release_us = 0;
    uint64_t max_exec_us = 0;
    int64_t worst_lateness_us = 0;
    uint64_t deadline_us = 0;
  };

  TaskStats a_{0, 0, 0, false, 0, 0, 0, 0, 10000};
  TaskStats b_{0, 0, 0, false, 0, 0, 0, 0, 20000};
  TaskStats agg_{0, 0, 0, false, 0, 0, 0, 0, 20000};
  TaskStats c_{0, 0, 0, false, 0, 0, 0, 0, 50000};
  TaskStats d_{0, 0, 0, false, 0, 0, 0, 0, 50000};
  TaskStats s_{0, 0, 0, false, 0, 0, 0, 0, 30000};
  uint64_t t0_ = 0;
  uint64_t periodic_report_every_us_ = 0u;
  uint64_t final_report_after_us_ = 0u;
  uint64_t next_periodic_report_us_ = 0u;
  uint64_t final_report_deadline_us_ = 0u;
  bool final_report_printed_ = false;
  static const uint32_t S_RELEASE_Q_MAX = 32u;
  uint64_t s_release_us_[S_RELEASE_Q_MAX]{};
  uint32_t s_q_head_ = 0;
  uint32_t s_q_tail_ = 0;
  uint32_t s_q_count_ = 0;

  static void resetTask(TaskStats &t) {
    t.jobs = 0;
    t.misses = 0;
    t.id = 0;
    t.active = false;
    t.start_us = 0;
    t.release_us = 0;
    t.max_exec_us = 0;
    t.worst_lateness_us = 0;
  }

  static void beginTask(TaskStats &t, uint32_t id, uint64_t release_us) {
    t.active = true;
    t.id = id;
    t.release_us = release_us;
    t.start_us = micros();
  }

  static void endTask(TaskStats &t) {
    if (!t.active) {
      return;
    }
    const uint64_t end_us = micros();
    const uint64_t exec_us = end_us - t.start_us;
    const uint64_t abs_deadline_us = t.release_us + t.deadline_us;
    const int64_t lateness_us = (int64_t)end_us - (int64_t)abs_deadline_us;
    if (exec_us > t.max_exec_us) {
      t.max_exec_us = exec_us;
    }
    if (lateness_us > t.worst_lateness_us) {
      t.worst_lateness_us = lateness_us;
    }
    t.jobs++;
    if (lateness_us > 0) {
      t.misses++;
    }
    t.active = false;
  }

  uint64_t popSRelease() {
    if (s_q_count_ == 0u) {
      return micros();
    }
    const uint64_t t = s_release_us_[s_q_head_];
    s_q_head_ = (s_q_head_ + 1u) % S_RELEASE_Q_MAX;
    s_q_count_--;
    return t;
  }

  static void reportOne(const char *name, const TaskStats &t) {
    Serial.printf("[MON] %s jobs=%" PRIu32 " misses=%" PRIu32 " max_exec=%lluus worst_late=%" PRIi64
                  "us\n",
                  name, t.jobs, t.misses, t.max_exec_us, t.worst_lateness_us);
  }
};

// ===== Global runtime state =====

static uint32_t g_idA = 0;
...
static TimingMonitor g_monitor; // declare monitor

// ===== Helpers =====
static inline uint64_t now_us() { return micros(); }

static inline void ack_high(int pin) { digitalWrite(pin, HIGH); }
static inline void ack_low(int pin) { digitalWrite(pin, LOW); }


// ===== Example Task =====
static void taskA(...) {
 
  const uint32_t countA = countEdgesA(...);

  const uint32_t seed = (g_idA << 16) ^ countA ^ 0xA1u;

  g_monitor.beginTaskA(g_idA); // signal start of the task to the monitor
  ack_high(PIN_ACK_A); // raise ack
  const uint32_t token = WorkKernel(BUDGET_A_CYCLES, seed);
  ack_low(PIN_ACK_A); // lower ack
  g_monitor.endTaskA(); // signal end of task to the monitor

  g_idA++;
}


// ===== Example Arduino setup/loop =====
void setup() {
  Serial.begin(115200);
  delay(200);
  g_monitor.setPeriodicReportEverySeconds(0);
  g_monitor.setFinalReportAfterSeconds(FINAL_REPORT_AFTER_SECONDS);

  pinMode(PIN_SYNC, INPUT_PULLDOWN);
  pinMode(PIN_IN_A, INPUT);
  pinMode(PIN_IN_B, INPUT);
  pinMode(PIN_IN_S, INPUT);
  pinMode(PIN_IN_MODE, INPUT);

  pinMode(PIN_ACK_A, OUTPUT);
  pinMode(PIN_ACK_B, OUTPUT);
  pinMode(PIN_ACK_AGG, OUTPUT);
  pinMode(PIN_ACK_C, OUTPUT);
  pinMode(PIN_ACK_D, OUTPUT);
  pinMode(PIN_ACK_S, OUTPUT);
  pinMode(PIN_ERR, OUTPUT);

  ack_low(PIN_ACK_A);
  ack_low(PIN_ACK_B);
  ack_low(PIN_ACK_AGG);
  ack_low(PIN_ACK_C);
  ack_low(PIN_ACK_D);
  ack_low(PIN_ACK_S);
  digitalWrite(PIN_ERR, LOW); 

  ...
}

// Example loop
void loop() {
  
  ... wait for synch (e.g. with the help of an interrupt)...
  g_monitor.sycnh(); // tell the monitor to start monitoring
  
  ... run your schedule ...

  taskA();

  ...

  if (!g_monitor.allDeadlinesMet()) {
    digitalWrite(PIN_ERR, HIGH);
  }
  if (g_monitor.pollReports()) {
    while (true) {
      asm volatile("nop"); // or exit
    }
  }
}
