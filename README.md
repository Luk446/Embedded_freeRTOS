# Embedded FreeRTOS Project

## Overview

Implementing a FreeRTOS super-loop scheduler on an ESP-32 using GPIO intterrupts, timed periodic releases and one sporadic task. Code task is defined as:
1. Wait for hardware SYNC to define time zero
2. Run period tasks A,B,AGG,C,D at fixed periods
3. Accept sporadic release S from an interrupt - only run when there is enough timing slack 
4. Toggle ACK GPIOs and call monitor hooks around each task for timing/reporting
5. Exectute workload via WorkKernel with per-task cycle budgets.

### Procedural floW

![flowchart](docs/mermaid-diagram-2026-03-19-123326.png)