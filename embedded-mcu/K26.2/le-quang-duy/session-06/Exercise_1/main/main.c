#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

/* --- HARDWARE DEFINITIONS --- */
#define LCD_HOST        SPI2_HOST
#define PIN_SCK         GPIO_NUM_12
#define PIN_MOSI        GPIO_NUM_11
#define PIN_MISO        GPIO_NUM_13
#define PIN_CS          GPIO_NUM_10
#define PIN_RS          GPIO_NUM_9
#define PIN_RST         GPIO_NUM_14
#define PIN_BK_LIGHT    GPIO_NUM_2

#define LCD_H_RES       (480U) /* landscape: width  */
#define LCD_V_RES       (320U) /* landscape: height */
#define LCD_CLK_HZ      (20 * 1000 * 1000)

/* CHUNK_PIXELS = 4800 (exactly 10 lines of 480 pixels).
 * Justification: 4800 pixels = 9600 bytes. This provides an excellent balance:
 * it is large enough to heavily amortise the SPI driver overhead per transaction, 
 * but small enough to easily fit into continuous SRAM/DMA allocations without failure. */
#define CHUNK_PIXELS    (4800U)
#define CHUNK_BYTES     (CHUNK_PIXELS * 2U)

/* --- ST7796U COMMANDS --- */
#define CMD_SWRESET     (0x01U)
#define CMD_SLPOUT      (0x11U)
#define CMD_INVON       (0x21U)
#define CMD_DISPON      (0x29U)
#define CMD_CASET       (0x2AU)
#define CMD_RASET       (0x2BU)
#define CMD_RAMWR       (0x2CU)
#define CMD_MADCTL      (0x36U)
#define CMD_COLMOD      (0x3AU)

#define VAL_COLMOD_16B  (0x55U)

/* --- MADCTL BITS --- */
#define MADCTL_MY       (0x80U) 
#define MADCTL_MX       (0x40U) 
#define MADCTL_MV       (0x20U) 
#define MADCTL_BGR      (0x08U) 

/* --- COLOURS (RGB565) --- */
#define COLOUR_RED      (0xF800U)
#define COLOUR_GREEN    (0x07E0U)
#define COLOUR_BLUE     (0x001FU)
#define COLOUR_WHITE    (0xFFFFU)
#define COLOUR_BLACK    (0x0000U)

/* --- HELPER: WRITE COMMAND --- */
void lcd_write_cmd(spi_device_handle_t spi, uint8_t cmd) {
    ESP_ERROR_CHECK(gpio_set_level(PIN_RS, 0)); /* RS Low = Command */
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8; /* 8 bits */
    t.tx_buffer = &cmd;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
}

/* --- HELPER: WRITE DATA --- */
void lcd_write_data(spi_device_handle_t spi, const uint8_t* data, size_t len) {
    if (len == 0) return;
    ESP_ERROR_CHECK(gpio_set_level(PIN_RS, 1)); /* RS High = Data */
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8; /* Size in bits */
    t.tx_buffer = data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
}

/* --- HELPER: SET WINDOW --- */
void lcd_set_window(spi_device_handle_t spi, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t data[4];
    
    lcd_write_cmd(spi, CMD_CASET);
    data[0] = (x0 >> 8) & 0xFF; data[1] = x0 & 0xFF;
    data[2] = (x1 >> 8) & 0xFF; data[3] = x1 & 0xFF;
    lcd_write_data(spi, data, 4);

    lcd_write_cmd(spi, CMD_RASET);
    data[0] = (y0 >> 8) & 0xFF; data[1] = y0 & 0xFF;
    data[2] = (y1 >> 8) & 0xFF; data[3] = y1 & 0xFF;
    lcd_write_data(spi, data, 4);

    lcd_write_cmd(spi, CMD_RAMWR);
}

/* --- HELPER: FILL RECTANGLE --- */
void lcd_fill_rect(spi_device_handle_t spi, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour) {
    uint32_t total_pixels = w * h;
    lcd_set_window(spi, x, y, x + w - 1, y + h - 1);

    /* Allocate chunk buffer in DMA-capable memory */
    uint8_t *chunk_buf = (uint8_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA);
    assert(chunk_buf != NULL);

    /* Pre-fill the chunk buffer. ST7796 expects big-endian (high byte first). */
    uint8_t col_hi = (colour >> 8) & 0xFF;
    uint8_t col_lo = colour & 0xFF;
    for (uint32_t i = 0; i < CHUNK_PIXELS; i++) {
        chunk_buf[i * 2]     = col_hi;
        chunk_buf[i * 2 + 1] = col_lo;
    }

    /* Transmit in chunks */
    uint32_t pixels_sent = 0;
    while (pixels_sent < total_pixels) {
        uint32_t pixels_to_send = total_pixels - pixels_sent;
        if (pixels_to_send > CHUNK_PIXELS) {
            pixels_to_send = CHUNK_PIXELS;
        }
        lcd_write_data(spi, chunk_buf, pixels_to_send * 2);
        pixels_sent += pixels_to_send;
    }
    
    free(chunk_buf);
}

/* --- INIT SEQUENCE --- */
void lcd_init(spi_device_handle_t spi) {
    /* Initialize control pins */
    ESP_ERROR_CHECK(gpio_set_direction(PIN_RS, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_direction(PIN_BK_LIGHT, GPIO_MODE_OUTPUT));

    /* Backlight on */
    ESP_ERROR_CHECK(gpio_set_level(PIN_BK_LIGHT, 1));

    /* Hardware reset */
    ESP_ERROR_CHECK(gpio_set_level(PIN_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(gpio_set_level(PIN_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Vendor Initialisation sequence */
    lcd_write_cmd(spi, CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_write_cmd(spi, CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 
     * MADCTL: Setting orientation and colour mode
     * - MADCTL_MV sets it to landscape (row/column exchange).
     * - MADCTL_MX is needed to flip the x-axis so it draws top-left to bottom-right properly.
     * - MADCTL_BGR is set to fix the colour mapping (Red and Blue swapped). 
     *   Setting this bit translates standard RGB565 macros correctly.
     */
    uint8_t madctl = MADCTL_MV | MADCTL_MX | MADCTL_BGR;
    lcd_write_cmd(spi, CMD_MADCTL);
    lcd_write_data(spi, &madctl, 1);

    uint8_t colmod = VAL_COLMOD_16B;
    lcd_write_cmd(spi, CMD_COLMOD);
    lcd_write_data(spi, &colmod, 1);

    /* IPS panels require Display Inversion ON to avoid inverted colours */
    lcd_write_cmd(spi, CMD_INVON);

    lcd_write_cmd(spi, CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* --- MAIN ENTRY --- */
void app_main(void) {
    spi_device_handle_t spi;

    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_SCK,
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = CHUNK_BYTES,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = LCD_CLK_HZ,
        .mode           = 0,
        .spics_io_num   = PIN_CS,
        .queue_size     = 7,
    };

    /* Initialise SPI bus and attach LCD device */
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &spi));

    /* Execute the bring-up sequence */
    lcd_init(spi);

    /* 1. Draw three horizontal bars (Red, Green, Blue) */
    uint16_t h_bar1 = LCD_V_RES / 3;             /* 106 */
    uint16_t h_bar2 = LCD_V_RES / 3;             /* 106 */
    uint16_t h_bar3 = LCD_V_RES - (h_bar1 * 2);  /* 108 */

    lcd_fill_rect(spi, 0, 0, LCD_H_RES, h_bar1, COLOUR_RED);
    lcd_fill_rect(spi, 0, h_bar1, LCD_H_RES, h_bar2, COLOUR_GREEN);
    lcd_fill_rect(spi, 0, h_bar1 + h_bar2, LCD_H_RES, h_bar3, COLOUR_BLUE);

    /* Hold bars for 3 seconds for photographic evidence */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 2. Indefinite loop cycling 5 colours */
    uint16_t cycle_colours[] = {
        COLOUR_RED, 
        COLOUR_GREEN, 
        COLOUR_BLUE, 
        COLOUR_WHITE, 
        COLOUR_BLACK
    };
    uint8_t colour_idx = 0;

    while (1) {
        lcd_fill_rect(spi, 0, 0, LCD_H_RES, LCD_V_RES, cycle_colours[colour_idx]);
        
        colour_idx++;
        if (colour_idx >= 5) colour_idx = 0;
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}