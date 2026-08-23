# Session 04 - ISR Notes

### 1. Why must a flag shared between the ISR and a task be `volatile`?
Without the `volatile` keyword, the compiler's optimizer will assume that a flag's value cannot change unexpectedly within a local scope. For example, in a `while(!flag)` loop, the compiler will load the variable into a CPU register once and check that register repeatedly, resulting in an infinite loop. The ISR writing to the variable's physical memory address does not prevent this because the optimizer is unaware of the asynchronous, hardware-level interruption. Marking it `volatile` forces the compiler to fetch the true value directly from RAM on every single read.

### 2. Why is calling `ESP_LOGI()` or `vTaskDelay()` from inside `button_isr()` dangerous?
`vTaskDelay()` is a blocking FreeRTOS API designed to suspend the calling task and yield CPU control to the scheduler. An ISR runs in a specialized hardware interrupt context, not a software task context; calling blocking RTOS APIs from an ISR will corrupt the scheduler's state or trigger an immediate hardware exception. Similarly, `ESP_LOGI()` is highly dangerous because it relies on standard output streams that use mutexes to ensure thread safety. If the ISR interrupts a task that currently holds that UART mutex, the ISR will block waiting for it, causing an unrecoverable system deadlock. 

### 3. Compare your two implementations.
**Responsiveness:** The interrupt-driven approach is significantly more responsive. In polling (Session 03), the response is delayed by up to the length of the polling period (e.g., 5-10 ms) since the CPU only notices the change when it asks. With an interrupt, the hardware preempts the CPU and records the event instantly within microseconds.

**CPU Usage:** The interrupt implementation uses exactly 0% CPU while the button is untouched, as the gesture task remains purely in the FreeRTOS Blocked state waiting on the queue. The polling implementation wastes CPU cycles constantly waking up just to check a pin that hasn't changed.

**Ease of Implementation:** The polling implementation was easier to get right. Polling inherently filters out high-frequency noise because it ignores the pin state between checks, making debouncing trivial. Interrupts, however, force you to deal with concurrency issues, carefully manage queues, calculate variable block-timeouts, and explicitly filter out every single mechanical bounce because each bounce fires a discrete hardware interrupt.