Development & Self-Testing Guidelines
For both of your assignments, you are allowed to use the development environment of your choice, e.g. such as ESP-IDF (through the vscode plugin) or Arduino. Note that when you use Arduino you will use the Arduino porting of FreeRTOS. This is perfectly acceptable for the first part of the assignment (super-loop), as long as you do not explicitely use the FreeRTOS API (so no FreeRTOS tasks, no vTaskDelay, no FreeRTOS timers, no semaphores…).
The official tester will only be used during final assessment. During development, students must validate both timing and functional correctness using laboratory equipment.
1. Generating Test Signals
Use laboratory signal generators to provide:
• IN_A: square wave (e.g., 500–2000 Hz)
• IN_B: square wave at a different frequency
• SYNC: single pulse defining T0
• IN_S: manual trigger or low-frequency pulse
• IN_MODE: DC high/low or slow toggle (button).

Use 0–3.3V logic levels and 50% duty cycle square waves.
2. Verifying Timing (Oscilloscope / Logic Analyzer)
Connect SYNC and all ACK signals to the oscilloscope or logic analyzer.

Verify:
• ACK_A pulses once for every 10 ms cycle from SYNC
• ACK_B and ACK_AGG pulses once every 20 ms cycle
• ACK_C and ACK_D pulse once every 50 ms cycle (only if IN_MODE=1 for assignment 3)
• Sporadic: time from IN_S rising edge to ACK_S falling edge ≤ 30 ms (note that this is the only not hard-real time requirements, so occasional delays are tolerated).

Measure pulse width of ACK signals to confirm execution time consistency.
A template code is also provided to verify timeliness of your tasks, i.e. b31dg_assignment2_template_with_monitoring.ino 
The example code in the template includes a monitor class and shows how it how it should be used within a cyclic executive super-loop in Arduino. If you use ESP-IDF e.g. with vscode, simply add the source of the class in two separate files, e.g.. TimingMonitor.h and TimingMonitor.c in your project). 
Example use:

static TimingMonitor g_monitor; // declare monitor as a global variable

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

ying Functional Correctness
Edge Counting:
Expected edges = frequency × window duration.
Compare expected value with UART log output.

Token Determinism:
With identical inputs and SYNC alignment, repeated runs must produce identical tokens.
4. Mode Control Validation
When IN_MODE = 0:
• No ACK_C or ACK_D pulses
• No UART logs for C or D
• IDX for C/D must not advance

When IN_MODE = 1:
• Periodic behavior resumes at next scheduled release.
5. Using Wokwi Simulator
Wokwi can be used to simulate square waves and observe timing using its logic analyzer, as demonstrated in class. You can use it to validate scheduling logic and task interaction.
However, note that Wokwi is not cycle-accurate. Use real hardware for precise timing validation and make sure you are prepared to demonstrate your system using real hardware and lab tools.
6. Recommended Development Strategy
1. Validate SYNC alignment and deadlines.
2. Validate edge counting correctness.
3. Validate sporadic response time.
4. Stress test with IN_MODE toggling and frequent sporadic triggers.
Final assessment will independently verify deadlines, response times, WCET compliance, IDX correctness, and functional token correctness.

6. Further hints
Try to design functions that you can re-use across the two implementations, do not implement the second program from scratch but work incrementally. For instance, you could start by first writing and testing standalone functions to address single requirements, before you combine them using a cyclic executive approach, and later adapting and re-using them with FreeRTOS.  Alternatively, you could implement mockup functions, so that you can focus on the scheduling aspect, before replacing the mockup functions with the actual functions implementing the functional requirements for your program.

For a modular design, tasks should be implemented as independent as possible from each other, reducing dependencies between tasks at a minimum.

When you use FreeRTOS, use mutexes/semaphores to protect your program from race conditions, and semaphores and/or queues to synchronise independent tasks.

