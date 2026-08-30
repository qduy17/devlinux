/* DevKitC-1 v1.1, RGB LED on GPIO38 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"

/* ------------------- CONSTANT DEFINITIONS ------------------- */
#define UART_PORT_NUM   UART_NUM_0
#define UART_TX_PIN     GPIO_NUM_43
#define UART_RX_PIN     GPIO_NUM_44
#define UART_BAUD_RATE  115200UL
#define UART_BUF_SIZE   1024U
#define UART_QUEUE_SIZE 10U
#define CMD_BUF_SIZE    64U

#define RGB_LED_PIN     GPIO_NUM_38
#define LED_STRIP_MAX   1U

static const char *TAG = "UART_CONSOLE";

/* Valid commands */
static const char* CMD_LED_ON  = "LED_ON";
static const char* CMD_LED_OFF = "LED_OFF";
static const char* CMD_RED     = "RED";
static const char* CMD_GREEN   = "GREEN";
static const char* CMD_BLUE    = "BLUE";

/* ------------------- GLOBAL VARIABLES ------------------- */
QueueHandle_t uart_queue;
led_strip_handle_t led_strip;

/* ------------------- COMMAND HANDLER ------------------- */
void handle_command(const char *cmd) {
    ESP_LOGI(TAG, "Received command: \"%s\"", cmd);

    if (strcmp(cmd, CMD_LED_ON) == 0) {
        ESP_LOGI(TAG, "LED -> WHITE");
        led_strip_set_pixel(led_strip, 0, 255, 255, 255);
        led_strip_refresh(led_strip);
    } else if (strcmp(cmd, CMD_LED_OFF) == 0) {
        ESP_LOGI(TAG, "LED -> OFF");
        led_strip_clear(led_strip);
    } else if (strcmp(cmd, CMD_RED) == 0) {
        ESP_LOGI(TAG, "LED -> RED");
        led_strip_set_pixel(led_strip, 0, 255, 0, 0);
        led_strip_refresh(led_strip);
    } else if (strcmp(cmd, CMD_GREEN) == 0) {
        ESP_LOGI(TAG, "LED -> GREEN");
        led_strip_set_pixel(led_strip, 0, 0, 255, 0);
        led_strip_refresh(led_strip);
    } else if (strcmp(cmd, CMD_BLUE) == 0) {
        ESP_LOGI(TAG, "LED -> BLUE");
        led_strip_set_pixel(led_strip, 0, 0, 0, 255);
        led_strip_refresh(led_strip);
    } else {
        ESP_LOGW(TAG, "Unknown command: \"%s\"", cmd);
    }
}

/* ------------------- UART EVENT TASK ------------------- */
void uart_event_task(void *pvParameters) {
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *) malloc(UART_BUF_SIZE);
    char cmd_buf[CMD_BUF_SIZE];
    uint16_t cmd_len = 0;

    for (;;) {
        if (xQueueReceive(uart_queue, (void *)&event, portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA:
                    uart_read_bytes(UART_PORT_NUM, dtmp, event.size, portMAX_DELAY);
                    for (int i = 0; i < event.size; i++) {
                        char c = (char)dtmp[i];

                        // Handle Enter (\r or \n)
                        if (c == '\r' || c == '\n') {
                            if (cmd_len > 0) {
                                cmd_buf[cmd_len] = '\0';
                                uart_write_bytes(UART_PORT_NUM, "\r\n", 2);
                                handle_command(cmd_buf);
                                cmd_len = 0; // Reset line length
                            }
                        }
                        // Handle Backspace or Delete
                        else if (c == '\b' || c == 127) {
                            if (cmd_len > 0) {
                                cmd_len--;
                                uart_write_bytes(UART_PORT_NUM, "\b \b", 3);
                            }
                        }
                        // Handle printable characters
                        else if (c >= 32 && c <= 126) {
                            if (cmd_len < CMD_BUF_SIZE - 1) {
                                cmd_buf[cmd_len++] = c;
                                uart_write_bytes(UART_PORT_NUM, &c, 1);
                            }
                        }
                    }
                    break;

                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART overflow, input flushed");
                    uart_flush_input(UART_PORT_NUM);
                    xQueueReset(uart_queue);
                    cmd_len = 0; // Reset corrupted line state
                    break;

                default:
                    ESP_LOGI(TAG, "Unhandled UART event type: %d", event.type);
                    break;
            }
        }
    }
    free(dtmp);
    vTaskDelete(NULL);
}

/* ------------------- MAIN APPLICATION ------------------- */
void app_main(void) {
    /* Initialize LED Strip */
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_PIN,
        .max_leds = LED_STRIP_MAX,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);

    /* Initialize UART */
    uart_config_t uart_config = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE, UART_BUF_SIZE, UART_QUEUE_SIZE, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "Console ready on UART0, 115200-8-N-1");

    /* Create Task pinned to Core 1 */
    xTaskCreatePinnedToCore(uart_event_task, "uart_event_task", 4096, NULL, 5, NULL, 1);
}