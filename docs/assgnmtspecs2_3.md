Real-Time Scheduling Laboratory Assignment(s)
1. Terminology
Device Under Test (DUT): the ESP32 board running the student implementation.
Tester: a separate ESP32-based system used to generate inputs and verify timing and functional correctness.
2. Objective
Implement the same real-time application twice on the DUT:
  (A) Cooperative cyclic executive (super-loop)
  (B) FreeRTOS-based implementation (tick = 1 ms)

Both implementations must satisfy time requirements and functional requirements.
3. Hardware Signals
Tester → DUT:
  • SYNC: defines global time origin T0 (rising edge)
  • IN_A: square wave A
  • IN_B: square wave B
  • IN_S: sporadic trigger (rising edge)
  • IN_MODE: enables/disables Tasks C and D

DUT → Tester:
  • ACK_A, ACK_B, ACK_AGG, ACK_C, ACK_D, ACK_S (HIGH during compute only)
  • UART TX (functional logs only; not used for deadline measurement)
4. Global Time Reference
T0 is defined as the rising edge of SYNC.
All periodic release times and deadlines are defined relative to T0.

Release pattern:
  Task A: every 10 ms from T0
  Task B: every 20 ms from T0
  Task AGG: every 20 ms from T0
  Task C: every 50 ms from T0 (if enabled)
  Task D: every 50 ms from T0 (if enabled)

Each periodic task has deadline equal to its period.
A deadline is met if the corresponding ACK signal goes LOW before release_time + deadline.
5. CPU and Core Constraint
CPU frequency will be fixed at 240 MHz during evaluation.
If a dual-core ESP32 is used, the program MUST use a single core only.
All application tasks must execute on the same CPU core. The second core must not be used for workload offloading.
6. Task Definitions
Each task maintains its own release index IDX starting from 0 at first release after SYNC.
For task X with period PX:
  release_X(k) = T0 + k·PX, k=0, 1, …
  IDX = k

Task A
Period: 10 ms | Deadline: 10 ms | Budget: 672,000 cycles
Compute countA = number of rising edges on IN_A during previous 10 ms.
seed = (IDA << 16) XOR countA XOR 0xA1
tokenA = WorkKernel(672000, seed)
Log: A,<IDA>,<countA>, <tokenA>
Task B
Period: 20 ms | Deadline: 20 ms | Budget: 960,000 cycles
Compute countB over 20 ms window.
seed = (IDB << 16) XOR countB XOR 0xB2
tokenB = WorkKernel(960000, seed)
Log: B,<IDB>,<countB>,<tokenB>
Task AGG
Period: 20 ms | Deadline: 20 ms | Budget: 480,000 cycles
If both tokenA and tokenB have been published at least once:
  agg = tokenA XOR tokenB
Else agg = 0xDEADBEEF
seed = (IDAGG << 16) XOR agg XOR 0xD4
tokenAGG = WorkKernel(480000, seed)
Log: AGG<IDAAG>,<agg>,<tokenAGG>
Task C (Mode Controlled – for FreeRTOS solution)
Period: 50 ms | Deadline: 50 ms | Budget: 1,680,000 cycles
Always enabled for super-loop solution (Assignment 2)
Executed only if IN_MODE == 1 for FreeRTOS solution (assignment 3)
seed = (IDC << 16) XOR 0xC3
tokenC = WorkKernel(1680000, seed)
Log: C,<IDC>, <tokenC>
Task D (Mode Controlled – for FreeRTOS solution)
Period: 50 ms | Deadline: 50 ms | Budget: 960,000 cycles
Always enabled for super-loop solution (Assignment 2)
Executed only if IN_MODE == 1 for FreeRTOS solution (assignment 3)
seed = (IDD << 16) XOR 0xD5
tokenD = WorkKernel(960000, seed)
Log: D, <IDD>,<tokenD>
Task S (Sporadic)
Released by each rising edge of IN_S.
Minimum inter-arrival time: 30 ms.
Response-time requirement: ≤ 30 ms.
Budget: 600,000 cycles total per trigger.
Let IDS be the sporadic job index (increment per trigger).
seed = (IDS << 16) XOR 0x55
tokenS = WorkKernel(600000, seed)
Log: S,<IDS>,<tokenS>
7. WorkKernel
WorkKernel(budget_cycles, seed) is provided and must not be modified.
The tester recomputes tokens from logged seeds to verify correctness.
8. Super-Loop Requirements
No FreeRTOS primitives (tasks, semaphores, queues, vTaskDelay…) allowed.

9. FreeRTOS Requirements
Periodic tasks must be released relative to SYNC.
Shared data must be protected.
All tasks must run on the same core.
10. Grading Rubric for Requirements under Test
Category	Verification	Pass Condition	Weight
Periodic deadlines	ACK timing vs SYNC	≥99% success	30%
Sporadic response time	ACK_S vs IN_S	≤30 ms	20%
WCET compliance	ACK pulse width	Within tolerance	15%
Functional correctness (counts)	UART vs expected	Correct	15%
Functional correctness (tokens)	Recomputed WorkKernel	Exact match	15%
Mode behavior	IN_MODE toggling	Correct enable/disable (only in RTOS/Assig. 3)	5%


11. Demos and Assessment

11.1 Assignment 2
The super-loop (Assignment 2) will need to be demonstrated at the demo session in Week 9.

You will be asked to answer questions on your system, e.g. design, testing, performance.

The final mark will be 60% from the results of the tests (see Section 10) and 40% from the Q&A

11.2 Assignment 3
Your FreeRTOS solution will need to be demonstrated at the demo session in Week 12.

You will be asked to answer questions on your system, e.g. design, testing, performance.

The final mark will be 50% from the result of the tests (see Section 10), 30% from the Q&A, and 20% from a short report, to be submitted by the end of Week 13.

11.3 Assignment 3 - Report
Submit the following paperwork:
• Link to your code repository, with fully documented source code
• Signed authorship statement.
• Short (maximum 2 pages) with answers to the following questions:
•	How did you use the FreeRTOS API (e.g. tasks, semaphores, queues)?
•	How did you decide to set the priorities of your FreeRTOS tasks? Why?
•	How did you decide how to size the stacks of your tasks?
•	How does your FreeRTOS implementation compare with the cyclic executive 	implementation? What are their advantages and disadvantages?

