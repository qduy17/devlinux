/*
 * DISPLAY TYPE: COMMON CATHODE
 * (To use a Common Anode display, change the IS_COMMON_ANODE macro to 1 below)
 *
 * DESIGN CHOICE EXPLANATION (Single click delay):
 * A single click is necessarily reported late (deferred by DOUBLE_CLICK_MS).
 * If we act on the first click immediately upon release, a double click would 
 * cause the counter to instantly jump +1 then jump -1, causing an ugly visual 
 * flicker. By delaying the commitment of the single click, the display remains 
 * stable while we wait to see if the user intends a double click.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"

/* --- Configuration --- */
#define IS_COMMON_ANODE 0

/* Pin Definitions */
#define SEG_A_PIN (4U)
#define SEG_B_PIN (5U)
#define SEG_C_PIN (6U)
#define SEG_D_PIN (7U)
#define SEG_E_PIN (15U)
#define SEG_F_PIN (16U)
#define SEG_G_PIN (17U)
#define BTN_PIN   (14U)

/* Timing Constants */
#define DEBOUNCE_MS      (25U)
#define DOUBLE_CLICK_MS  (350U)
#define LONG_PRESS_MS    (800U)
#define REPEAT_PERIOD_MS (500U)
#define POLL_PERIOD_MS   (5U)

/* --- Register Addresses & Masks --- */
#define REG_PTR(addr)              ((volatile uint32_t *)(addr))

/* ESP-IDF defines base addresses in the SOC headers; we cast them to volatile pointers */
#define MUX_REG(pin)               REG_PTR(GPIO_PIN_MUX_REG[pin])
#define GPIO_OUT_W1TS_PTR          REG_PTR(GPIO_OUT_W1TS_REG)
#define GPIO_OUT_W1TC_PTR          REG_PTR(GPIO_OUT_W1TC_REG)
#define GPIO_ENABLE_W1TS_PTR       REG_PTR(GPIO_ENABLE_W1TS_REG)
#define GPIO_IN_PTR                REG_PTR(GPIO_IN_REG)
#define GPIO_FUNC_OUT_SEL_PTR(pin) REG_PTR(GPIO_FUNC0_OUT_SEL_CFG_REG + ((pin) * 4))

/* IO MUX Bit definitions */
#define MUX_FUN_IE_BIT             (1U << 9)
#define MUX_FUN_WPU_BIT            (1U << 8)
#define MUX_MCU_SEL_GPIO           (1U << 12) /* FUNC1 is simple GPIO on ESP32-S3 */
#define SIG_GPIO_OUT_IDX           128U       /* Magic index to bypass matrix and route direct output */

/* --- Segment Logic --- */
static const uint8_t SEGMENT_MAP[10] = {
    0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U, /* 0 1 2 3 4 */
    0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU  /* 5 6 7 8 9 */
};

/* Macro to map the 7-bit segment map to our actual 32-bit GPIO layout */
#define SEG_TO_GPIO_MASK(seg_mask) \
    ( (((seg_mask) & (1U << 0)) ? (1U << SEG_A_PIN) : 0) | \
      (((seg_mask) & (1U << 1)) ? (1U << SEG_B_PIN) : 0) | \
      (((seg_mask) & (1U << 2)) ? (1U << SEG_C_PIN) : 0) | \
      (((seg_mask) & (1U << 3)) ? (1U << SEG_D_PIN) : 0) | \
      (((seg_mask) & (1U << 4)) ? (1U << SEG_E_PIN) : 0) | \
      (((seg_mask) & (1U << 5)) ? (1U << SEG_F_PIN) : 0) | \
      (((seg_mask) & (1U << 6)) ? (1U << SEG_G_PIN) : 0) )

static uint32_t DIGIT_GPIO_MASKS[10];
static uint32_t ALL_SEGMENTS_MASK;
static int8_t counter = 0;

static void init_hw(void) {
    /* Pre-compute 32-bit masks for O(1) single-write digit updates */
    for (int i = 0; i < 10; i++) {
        DIGIT_GPIO_MASKS[i] = SEG_TO_GPIO_MASK(SEGMENT_MAP[i]);
    }
    ALL_SEGMENTS_MASK = SEG_TO_GPIO_MASK(0x7FU);

    /* Configure 7-segment output pins */
    uint32_t seg_pins[] = {SEG_A_PIN, SEG_B_PIN, SEG_C_PIN, SEG_D_PIN, SEG_E_PIN, SEG_F_PIN, SEG_G_PIN};
    for (int i = 0; i < 7; i++) {
        uint32_t pin = seg_pins[i];
        *MUX_REG(pin) = MUX_MCU_SEL_GPIO;             /* Set to GPIO function */
        *GPIO_FUNC_OUT_SEL_PTR(pin) = SIG_GPIO_OUT_IDX; /* Route direct output to pad */
        *GPIO_ENABLE_W1TS_PTR = (1U << pin);          /* Enable output driver */
    }

    /* Configure Button input pin (internal pull-up) */
    *MUX_REG(BTN_PIN) = MUX_MCU_SEL_GPIO | MUX_FUN_IE_BIT | MUX_FUN_WPU_BIT;
}

static void display_digit(int8_t digit) {
    uint32_t active_pins = DIGIT_GPIO_MASKS[digit];
    uint32_t set_mask, clear_mask;

    if (IS_COMMON_ANODE) {
        clear_mask = active_pins;
        set_mask = ALL_SEGMENTS_MASK & ~active_pins;
    } else {
        set_mask = active_pins;
        clear_mask = ALL_SEGMENTS_MASK & ~active_pins;
    }

    /* Single atomic-like register update */
    *GPIO_OUT_W1TC_PTR = clear_mask;
    *GPIO_OUT_W1TS_PTR = set_mask;
}

static void counter_inc(void) {
    counter++;
    if (counter > 9) counter = 0;
    display_digit(counter);
}

static void counter_dec(void) {
    counter--;
    if (counter < 0) counter = 9;
    display_digit(counter);
}

void app_main(void) {
    init_hw();
    display_digit(counter);

    /* State variables for debouncing */
    bool last_raw_pressed = false;
    bool debounced_pressed = false;
    uint32_t last_debounce_time = 0;

    /* State variables for gesture decoding */
    uint32_t press_time = 0;
    uint32_t click_time = 0;
    uint32_t last_repeat_time = 0;
    bool pending_click = false;
    bool is_long_press = false;

    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        
        /* Button pulls to GND, so 0 bit = pressed */
        bool raw_pressed = ((*GPIO_IN_PTR & (1U << BTN_PIN)) == 0);

        /* Debounce check */
        if (raw_pressed != last_raw_pressed) {
            last_debounce_time = now_ms;
        }

        if ((now_ms - last_debounce_time) >= DEBOUNCE_MS) {
            if (raw_pressed != debounced_pressed) {
                debounced_pressed = raw_pressed;
                
                if (debounced_pressed) { /* Edge: Released -> Pressed */
                    press_time = now_ms;
                    is_long_press = false;
                    
                    if (pending_click && (now_ms - click_time <= DOUBLE_CLICK_MS)) {
                        /* Second press within window. Wait for release to confirm double click. */
                    } else if (pending_click) {
                        /* Second press arrived too late. Commit the pending single click. */
                        counter_inc();
                        pending_click = false;
                    }
                } else { /* Edge: Pressed -> Released */
                    if (!is_long_press) {
                        if (pending_click) {
                            /* Double click confirmed */
                            counter_dec();
                            pending_click = false;
                        } else {
                            /* First click detected. Stage it and await potential double click. */
                            pending_click = true;
                            click_time = now_ms;
                        }
                    }
                }
            }
        }
        last_raw_pressed = raw_pressed;

        /* Evaluate hold timeouts while pressed */
        if (debounced_pressed) {
            uint32_t hold_time = now_ms - press_time;
            if (!is_long_press && (hold_time >= LONG_PRESS_MS)) {
                is_long_press = true;
                if (pending_click) {
                    /* Edge case: they clicked, then pressed-and-held the second click */
                    counter_inc();
                    pending_click = false;
                }
                counter_inc(); /* Trigger initial long press step */
                last_repeat_time = now_ms;
            } else if (is_long_press && (now_ms - last_repeat_time >= REPEAT_PERIOD_MS)) {
                counter_inc(); /* Trigger auto-repeat step */
                last_repeat_time = now_ms;
            }
        }

        /* Evaluate timeout for uncommitted single click while released */
        if (pending_click && !debounced_pressed && (now_ms - click_time >= DOUBLE_CLICK_MS)) {
            counter_inc();
            pending_click = false;
        }

        /* Sleep to prevent Task Watchdog Timeout */
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}