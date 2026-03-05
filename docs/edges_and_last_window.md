Appendix – Suggestions for Implementing EdgesInLastWindow()
Purpose:
To solve the assignments you need to implement a function such as EdgesInLastWindow(pin, window_ms), to return the number of rising edges observed on a digital input during the last window_ms milliseconds. The implementation must be bounded in execution time and suitable for real-time use inside periodic tasks.
Recommended Approach: PCNT (Pulse Counter) Peripheral
The ESP32 includes a hardware Pulse Counter (PCNT) peripheral that can count rising edges without CPU intervention. This is the preferred solution because it provides deterministic timing and minimal overhead.
Suggested strategy:
1. Configure one PCNT unit per input signal (IN_A, IN_B).
2. Configure it to increment on rising edges.
3. Do NOT reset the counter every period.
4. At each task release:
   - Read the current counter value (C_now).
   - Compute delta = C_now - C_prev.
   - Store C_prev = C_now.
5. Return delta as the edge count for that window.

This delta-per-period method guarantees bounded execution time because each call performs only a constant number of register reads and arithmetic operations.
Alternative Approach: Software Timestamp Buffer (Advanced)
Students may implement an ISR that records timestamps of rising edges into a circular buffer. EdgesInLastWindow() would then count how many timestamps fall within the last window_ms interval.

However:
- The ISR must be extremely short.
- The buffer must be bounded in size.
- Access must be protected against race conditions.
- Worst-case execution time must remain bounded.
What NOT to Do
- Do not busy-wait on the input pin.
- Do not scan unbounded timestamp arrays.
- Do not disable interrupts for long periods.
- Do not use delay-based polling loops.
- Do not perform serial printing inside the edge-counting function.
Real-Time Constraints
EdgesInLastWindow() must execute in bounded time independent of the input signal frequency. The function must not scale with the number of edges observed during the window.
Testing Recommendations
To validate correctness:
- Feed a known-frequency square wave from the tester.
- Compare expected edges per period with measured delta.
- Verify behavior at low frequency, high frequency, and near deadline limits.
For best real-time performance and deterministic behavior, the PCNT-based delta-per-period implementation is strongly recommended.
