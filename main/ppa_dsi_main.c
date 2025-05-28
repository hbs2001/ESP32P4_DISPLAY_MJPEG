#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "driver/jpeg_types.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "driver/ppa.h"
#include "example_dsi_init.h"
#include "example_dsi_init_config.h"
#include "driver/jpeg_decode.h"
#include "esp_timer.h"

#define EXAMPLE_IMAGE_H 608
#define EXAMPLE_IMAGE_W 1024
#define MAX_JPEG_FRAME_SIZE (1024 * 600)
#define MJPEG_FRAME_QUEUE_SIZE 3
#define JPEG_ALIGN_16(x) (((x) + 15) & ~15)

static const char *TAG = "mjpeg_dma_player";
static char mjpeg_filename[256] = "/spiffs/output.mjpeg";

static jpeg_decoder_handle_t jpgd_handle = NULL;

typedef struct {
    uint8_t *jpeg_data;
    size_t jpeg_size;
} mjpeg_frame_t;

static QueueHandle_t mjpeg_frame_queue;

// SPIFFS Init
esp_err_t init_mjpeg_file() {
    FILE *f = fopen(mjpeg_filename, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", mjpeg_filename);
        return ESP_FAIL;
    }
    fclose(f);
    return ESP_OK;
}

// MJPEG Reader Task
void mjpeg_reader_task(void *pvParameters) {
    FILE *f = fopen(mjpeg_filename, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open MJPEG file");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        mjpeg_frame_t frame = {0};
        frame.jpeg_data = malloc(MAX_JPEG_FRAME_SIZE);
        if (!frame.jpeg_data) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer");
            break;
        }

        // 查找 JPEG 起始
        int found_start = 0;
        size_t pos = 0;
        while (!feof(f)) {
            uint8_t byte;
            fread(&byte, 1, 1, f);
            if (byte == 0xFF) {
                fread(&byte, 1, 1, f);
                if (byte == 0xD8) {
                    frame.jpeg_data[pos++] = 0xFF;
                    frame.jpeg_data[pos++] = 0xD8;
                    found_start = 1;
                    break;
                }
            }
        }

        if (!found_start) {
            free(frame.jpeg_data);
            ESP_LOGI(TAG, "No more frames");
            break;
        }

        // 读取到 JPEG 结束
        while (pos < MAX_JPEG_FRAME_SIZE && !feof(f)) {
            uint8_t byte;
            fread(&byte, 1, 1, f);
            frame.jpeg_data[pos++] = byte;
            if (byte == 0xFF) {
                fread(&byte, 1, 1, f);
                frame.jpeg_data[pos++] = byte;
                if (byte == 0xD9) break;
            }
        }

        frame.jpeg_size = pos;

        if (xQueueSend(mjpeg_frame_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Queue full, dropping frame");
            free(frame.jpeg_data);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    fclose(f);
    vTaskDelete(NULL);
}

// MJPEG Decode & Display Task (双缓冲 + DMA)
void mjpeg_decode_display_task(void *pvParameters) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)pvParameters;

    int aligned_height = JPEG_ALIGN_16(EXAMPLE_IMAGE_H);
    size_t rx_buffer_size = EXAMPLE_IMAGE_W * aligned_height * 2;

    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    
    uint8_t *rx_buf[2] = {
        (uint8_t *)jpeg_alloc_decoder_mem(rx_buffer_size, &rx_mem_cfg, NULL),
        (uint8_t *)jpeg_alloc_decoder_mem(rx_buffer_size, &rx_mem_cfg, NULL)
    };

    int current_index = 0;
    mjpeg_frame_t current_frame, next_frame;

    if (xQueueReceive(mjpeg_frame_queue, &current_frame, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to receive first frame");
        vTaskDelete(NULL);
    }

    while (1) {
        if (xQueueReceive(mjpeg_frame_queue, &next_frame, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to receive next frame");
            break;
        }

        jpeg_decode_cfg_t decode_cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        };

        jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
        };
        size_t tx_buf_size;
        uint8_t *tx_buf = jpeg_alloc_decoder_mem(current_frame.jpeg_size, &tx_mem_cfg, &tx_buf_size);
        memcpy(tx_buf, current_frame.jpeg_data, current_frame.jpeg_size);

        uint32_t out_size;
        esp_err_t ret = jpeg_decoder_process(jpgd_handle, &decode_cfg,
                                             tx_buf, current_frame.jpeg_size,
                                             rx_buf[current_index], rx_buffer_size, &out_size);

        free(current_frame.jpeg_data);
        free(tx_buf);

        if (ret == ESP_OK) {
            esp_lcd_panel_draw_bitmap(panel, 0, 0,
                                      EXAMPLE_IMAGE_W, EXAMPLE_IMAGE_H,
                                      rx_buf[current_index]);
        } else {
            ESP_LOGE(TAG, "JPEG decode failed");
        }

        current_frame = next_frame;
        current_index = 1 - current_index;
    }

    free(rx_buf[0]);
    free(rx_buf[1]);
    vTaskDelete(NULL);
}

void app_main(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    if (init_mjpeg_file() != ESP_OK) {
        return;
    }

    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .intr_priority = 2,
        .timeout_ms = 40,
    };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle));

    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = CONFIG_EXAMPLE_USED_LDO_CHAN_ID,
        .voltage_mv = CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_panel_handle_t mipi_dpi_panel = NULL;

    example_dsi_resource_alloc(&mipi_dsi_bus, &mipi_dbi_io, &mipi_dpi_panel, NULL);
    example_dpi_panel_reset(mipi_dpi_panel);
    example_dpi_panel_init(mipi_dpi_panel);

    mjpeg_frame_queue = xQueueCreate(MJPEG_FRAME_QUEUE_SIZE, sizeof(mjpeg_frame_t));

    xTaskCreate(mjpeg_reader_task, "reader", 8192, NULL, 5, NULL);
    xTaskCreate(mjpeg_decode_display_task, "decode_display", 8192, mipi_dpi_panel, 6, NULL);
}
