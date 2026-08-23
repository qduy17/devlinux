/*
 * DISPLAY TYPE: COMMON CATHODE
 * (To use a Common Anode display, change the IS_COMMON_ANODE macro to 1 below)
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"

/* --- Configuration --- */
#define IS_COMMON_ANODE 0

#define SEG_A_PIN (4U)
#define SEG_B_PIN (5U)
#define SEG_C_PIN (6U)
#define SEG_D_PIN (7U)
#define SEG_E_PIN (15U)
#define SEG_F_PIN (16U)
#define SEG_G_PIN (17U)
#define BTN_PIN   GPIO_NUM_14

/* Timing Constants (ms) */
#define DEBOUNCE_MS      (25U)
#define DOUBLE_CLICK_MS  (350U)
#define LONG_PRESS_MS    (800U)
#define REPEAT_PERIOD_MS (500U)

/* --- Register Addresses & Masks (From Session 03) --- */
#define REG_PTR(addr)              ((volatile uint32_t *)(addr))
#define MUX_REG(pin)               REG_PTR(GPIO_PIN_MUX_REG[pin])
#define GPIO_OUT_W1TS_PTR          REG_PTR(GPIO_OUT_W1TS_REG)
#define GPIO_OUT_W1TC_PTR          REG_PTR(GPIO_OUT_W1TC_REG)
#define GPIO_ENABLE_W1TS_PTR       REG_PTR(GPIO_ENABLE_W1TS_REG)
#define GPIO_FUNC_OUT_SEL_PTR(pin) REG_PTR(GPIO_FUNC0_OUT_SEL_CFG_REG + ((pin) * 4))

#define MUX_MCU_SEL_GPIO           (1U << 12)
#define SIG_GPIO_OUT_IDX           128U

static const uint8_t SEGMENT_MAP[10] = {
    0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U, /* 0 1 2 3 4 */
    0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU  /* 5 6 7 8 9 */
};

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

/* --- Interrupt & Queue Types --- */
typedef struct {
    int64_t timestamp_us;
    bool    is_press;
} btn_event_t;

static QueueHandle_t btn_queue;

/* --- Display Implementation --- */
static void init_display_hw(void) {
    for (int i = 0; i < 10; i++) {
        DIGIT_GPIO_MASKS[i] = SEG_TO_GPIO_MASK(SEGMENT_MAP[i]);
    }
    ALL_SEGMENTS_MASK = SEG_TO_GPIO_MASK(0x7FU);

    uint32_t seg_pins[] = {SEG_A_PIN, SEG_B_PIN, SEG_C_PIN, SEG_D_PIN, SEG_E_PIN, SEG_F_PIN, SEG_G_PIN};
    for (int i = 0; i < 7; i++) {
        uint32_t pin = seg_pins[i];
        *MUX_REG(pin) = MUX_MCU_SEL_GPIO;
        *GPIO_FUNC_OUT_SEL_PTR(pin) = SIG_GPIO_OUT_IDX;
        *GPIO_ENABLE_W1TS_PTR = (1U << pin);
    }
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

    *GPIO_OUT_W1TC_PTR = clear_mask;
    *GPIO_OUT_W1TS_PTR = set_mask;
}

static void counter_add(int8_t val) {
    counter += val;
    if (counter > 9) counter = 0;
    if (counter < 0) counter = 9;
    display_digit(counter);
}

/* --- ISR Implementation --- */
static void IRAM_ATTR button_isr(void* arg) {
    btn_event_t event;
    event.timestamp_us = esp_timer_get_time();
    /* Internal pull-up: 0V means pressed */
    event.is_press = (gpio_get_level(BTN_PIN) == 0); 

    BaseType_t high_task_wakeup = pdFALSE;
    xQueueSendFromISR(btn_queue, &event, &high_task_wakeup);
    
    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* --- Gesture Task --- */
#define ST_IDLE            0
#define ST_PRESSED         1
#define ST_WAIT_DOUBLE     2
#define ST_LONG_PRESS      3
#define ST_IGNORE_RELEASE  4

static void gesture_task(void *arg) {
    int state = ST_IDLE;
    uint32_t press_time = 0;
    uint32_t click_time = 0;
    uint32_t last_repeat_time = 0;
    int64_t last_accepted_us = 0;

    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        TickType_t timeout = portMAX_DELAY;

        /* Calculate timeout based on current state to wake us up exactly when needed */
        if (state == ST_PRESSED) {
            uint32_t elapsed = now_ms - press_time;
            timeout = (elapsed >= LONG_PRESS_MS) ? 0 : pdMS_TO_TICKS(LONG_PRESS_MS - elapsed);
        } else if (state == ST_LONG_PRESS) {
            uint32_t elapsed = now_ms - last_repeat_time;
            timeout = (elapsed >= REPEAT_PERIOD_MS) ? 0 : pdMS_TO_TICKS(REPEAT_PERIOD_MS - elapsed);
        } else if (state == ST_WAIT_DOUBLE) {
            uint32_t elapsed = now_ms - click_time;
            timeout = (elapsed >= DOUBLE_CLICK_MS) ? 0 : pdMS_TO_TICKS(DOUBLE_CLICK_MS - elapsed);
        }

        btn_event_t event;
        if (xQueueReceive(btn_queue, &event, timeout) == pdPASS) {
            /* Edge Received: Filter mechanical bounce */
            if ((event.timestamp_us - last_accepted_us) < (DEBOUNCE_MS * 1000ULL)) {
                continue;
            }
            last_accepted_us = event.timestamp_us;
            
            bool pressed = event.is_press;
            uint32_t evt_time_ms = (uint32_t)(event.timestamp_us / 1000ULL);

            if (pressed) {
                if (state == ST_IDLE) {
                    state = ST_PRESSED;
                    press_time = evt_time_ms;
                } else if (state == ST_WAIT_DOUBLE) {
                    /* Second press within window -> Double click confirmed */
                    counter_add(-1);
                    state = ST_IGNORE_RELEASE; /* Wait for this second press to be released quietly */
                }
            } else { /* Released */
                if (state == ST_PRESSED) {
                    /* Released before LONG_PRESS -> Stage the single click */
                    state = ST_WAIT_DOUBLE;
                    click_time = evt_time_ms;
                } else if (state == ST_LONG_PRESS || state == ST_IGNORE_RELEASE) {
                    /* Gesture complete, reset to idle */
                    state = ST_IDLE;
                }
            }
        } else {
            /* Timeout Expired: Progress the state machine without new edges */
            now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            
            if (state == ST_PRESSED) {
                /* Held long enough to become a long press */
                state = ST_LONG_PRESS;
                counter_add(1); /* Initial increment */
                last_repeat_time = now_ms;
            } else if (state == ST_LONG_PRESS) {
                /* Periodic auto-repeat */
                counter_add(1);
                last_repeat_time = now_ms;
            } else if (state == ST_WAIT_DOUBLE) {
                /* Window closed without a second press -> Single click confirmed */
                counter_add(1);
                state = ST_IDLE;
            }
        }
    }
}

void app_main(void) {
    init_display_hw();
    display_digit(counter);

    btn_queue = xQueueCreate(20, sizeof(btn_event_t));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_PIN, button_isr, NULL);

    xTaskCreate(gesture_task, "gesture_task", 4096, NULL, 5, NULL);
}