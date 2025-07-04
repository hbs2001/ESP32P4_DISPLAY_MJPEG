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
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "driver/ppa.h"
#include "example_dsi_init.h"
#include "example_dsi_init_config.h"
#include "driver/jpeg_decode.h"
#include "esp_timer.h"

#define USE_JPEG_IMAGE 1  // 设置为 1 显示 JPEG 图片，设置为 0 播放 MJPEG 视频
#define EXAMPLE_IMAGE_H 608
#define EXAMPLE_IMAGE_W 1024
#define MAX_JPEG_FRAME_SIZE (1024 * 600)
#define JPEG_ALIGN_16(x) (((x) + 15) & ~15)
static const char *TAG = "mjpeg_player";
static char mjpeg_filename[256] = "/spiffs/output.mjpeg";
static size_t mjpeg_file_size = 0;

static jpeg_decoder_handle_t jpgd_handle = NULL;
typedef struct {
    uint8_t *jpeg_data;
    size_t jpeg_size;
} mjpeg_frame_t;

typedef struct {
    uint8_t *rgb_data;
    size_t rgb_size;
} decoded_frame_t;

static QueueHandle_t mjpeg_frame_queue;
static QueueHandle_t decoded_frame_queue;
esp_err_t init_mjpeg_file() {
    FILE *f = fopen(mjpeg_filename, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", mjpeg_filename);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    mjpeg_file_size = ftell(f);
    fclose(f);
    ESP_LOGI(TAG, "Found MJPEG file: %s (size: %d bytes)", mjpeg_filename, (int)mjpeg_file_size);
    return ESP_OK;
}

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

        vTaskDelay(pdMS_TO_TICKS(2));  // 控制读取速率
    }

    fclose(f);
    vTaskDelete(NULL);
}

void mjpeg_decode_task(void *pvParameters) {
    while (1) {
        mjpeg_frame_t frame;
        if (xQueueReceive(mjpeg_frame_queue, &frame, portMAX_DELAY) == pdTRUE) {
            // 解码配置
            jpeg_decode_cfg_t decode_cfg = {
                .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            };

            jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
                .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
            };
            size_t tx_buf_size;
            uint8_t *tx_buf = jpeg_alloc_decoder_mem(frame.jpeg_size, &tx_mem_cfg, &tx_buf_size);
            memcpy(tx_buf, frame.jpeg_data, frame.jpeg_size);

            jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
                .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
            };
            int aligned_height = JPEG_ALIGN_16(EXAMPLE_IMAGE_H);
            size_t rx_needed = EXAMPLE_IMAGE_W * aligned_height * 2;
            size_t rx_buf_size;
            uint8_t *rx_buf = jpeg_alloc_decoder_mem(rx_needed, &rx_mem_cfg, &rx_buf_size);

            uint32_t out_size;
            esp_err_t ret = jpeg_decoder_process(jpgd_handle, &decode_cfg,
                                                 tx_buf, frame.jpeg_size,
                                                 rx_buf, rx_buf_size, &out_size);

            if (ret == ESP_OK) {
                decoded_frame_t dframe = {
                    .rgb_data = rx_buf,
                    .rgb_size = rx_buf_size
                };
                xQueueSend(decoded_frame_queue, &dframe, portMAX_DELAY);
            } else {
                free(rx_buf);
                ESP_LOGE(TAG, "Decode failed");
            }

            free(frame.jpeg_data);
            free(tx_buf);
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}


void mjpeg_display_task(void *pvParameters) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)pvParameters;

    while (1) {
        decoded_frame_t frame;
        if (xQueueReceive(decoded_frame_queue, &frame, portMAX_DELAY) == pdTRUE) {
            esp_lcd_panel_draw_bitmap(panel, 0, 0, EXAMPLE_IMAGE_W, EXAMPLE_IMAGE_H, frame.rgb_data);
            free(frame.rgb_data);
        }

        vTaskDelay(pdMS_TO_TICKS(1));  // 控制帧间隔可选
    }
}

void jpeg_display_task(void *pvParameters) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)pvParameters;
    static int current_file_num = 1;
    while (1) {
        int64_t read_start = esp_timer_get_time();
        // if (current_file_num > 44) { // 假设最多有 100 张图片
        //     current_file_num = 1; // 循环播放
        // }
        char jpeg_file_path[32];
        snprintf(jpeg_file_path, sizeof(jpeg_file_path), "/spiffs/%04d.jpg", current_file_num%44);
        //const char *jpeg_file_path = "/spiffs/0001.jpg";
        FILE *f = fopen(jpeg_file_path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open JPEG file: %s", jpeg_file_path);
            vTaskDelay((1000/24) / portTICK_PERIOD_MS);
            continue;
        }

        fseek(f, 0, SEEK_END);
        size_t file_size = ftell(f);
        rewind(f);

        if (file_size > MAX_JPEG_FRAME_SIZE) {
            ESP_LOGE(TAG, "JPEG file too large (%d bytes)", (int)file_size);
            fclose(f);
            vTaskDelay((1000/24) / portTICK_PERIOD_MS);
            continue;
        }

        uint8_t *jpeg_data = malloc(file_size);
        if (!jpeg_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for JPEG");
            fclose(f);
            vTaskDelay((1000/24) / portTICK_PERIOD_MS);
            continue;
        }

        fread(jpeg_data, 1, file_size, f);
        fclose(f);
        int64_t read_time = (esp_timer_get_time() - read_start) / 1000;

        jpeg_decode_cfg_t decode_cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        };

        jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
        };
        size_t tx_buffer_size = 0;
        uint8_t *tx_buf = (uint8_t *)jpeg_alloc_decoder_mem(file_size, &tx_mem_cfg, &tx_buffer_size);
        if (!tx_buf) {
            free(jpeg_data);
            ESP_LOGE(TAG, "Failed to allocate TX buffer");
            vTaskDelay((1000/24) / portTICK_PERIOD_MS);
            continue;
        }
        memcpy(tx_buf, jpeg_data, file_size);

        jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        int aligned_height = JPEG_ALIGN_16(EXAMPLE_IMAGE_H);
        size_t rx_buffer_size = EXAMPLE_IMAGE_W * aligned_height * 2;
        uint8_t *rx_buf = (uint8_t *)jpeg_alloc_decoder_mem(rx_buffer_size, &rx_mem_cfg, &rx_buffer_size);

        int64_t decode_start = esp_timer_get_time();
        uint32_t out_size = 0;
        esp_err_t ret = jpeg_decoder_process(jpgd_handle, &decode_cfg, tx_buf, file_size, rx_buf, rx_buffer_size, &out_size);
        int64_t decode_time = (esp_timer_get_time() - decode_start) / 1000;

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "JPEG decode failed");
        } else {
            int64_t display_start = esp_timer_get_time();
            esp_lcd_panel_draw_bitmap(panel, 0, 0, EXAMPLE_IMAGE_W, EXAMPLE_IMAGE_H, rx_buf);
            int64_t display_time = (esp_timer_get_time() - display_start) / 1000;
            
            ESP_LOGI(TAG, "[Frame:%04d] Read:%lldms Decode:%lldms Display:%lldms Total:%lldms", 
                    current_file_num, 
                    read_time, 
                    decode_time,
                    display_time,
                    read_time + decode_time + display_time);
        }
        current_file_num++;
        free(jpeg_data);
        free(tx_buf);
        free(rx_buf);

        vTaskDelay((1000/24) / portTICK_PERIOD_MS);  
    }
}
esp_err_t display_mjpeg_frame(esp_lcd_panel_handle_t panel, FILE *f) {
    uint8_t *jpeg_data = malloc(MAX_JPEG_FRAME_SIZE);
    if (!jpeg_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG frame");
        return ESP_ERR_NO_MEM;
    }

    size_t frame_size = 0;
    int found_start = 0;
    while (!feof(f)) {
        uint8_t byte;
        fread(&byte, 1, 1, f);
        if (byte == 0xFF) {
            fread(&byte, 1, 1, f);
            if (byte == 0xD8) {
                found_start = 1;
                jpeg_data[frame_size++] = 0xFF;
                jpeg_data[frame_size++] = 0xD8;
                break;
            }
        }
    }

    if (!found_start) {
        ESP_LOGE(TAG, "No JPEG start marker found");
        free(jpeg_data);
        return ESP_FAIL;
    }

    while (frame_size < MAX_JPEG_FRAME_SIZE && !feof(f)) {
        uint8_t byte;
        fread(&byte, 1, 1, f);
        jpeg_data[frame_size++] = byte;
        if (byte == 0xFF) {
            fread(&byte, 1, 1, f);
            jpeg_data[frame_size++] = byte;
            if (byte == 0xD9) {
                break;
            }
        }
    }

    // ---- 硬解码配置 ----
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t tx_buffer_size = 0;
    uint8_t *tx_buf = (uint8_t *)jpeg_alloc_decoder_mem(frame_size, &tx_mem_cfg, &tx_buffer_size);
    if (!tx_buf) {
        free(jpeg_data);
        ESP_LOGE(TAG, "Failed to alloc input buffer");
        return ESP_ERR_NO_MEM;
    }
    memcpy(tx_buf, jpeg_data, frame_size);

    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t rx_buffer_size = 0;

    int aligned_height = JPEG_ALIGN_16(EXAMPLE_IMAGE_H);
    size_t rx_needed_size = EXAMPLE_IMAGE_W * aligned_height * 2;
    
    uint8_t *rx_buf = (uint8_t *)jpeg_alloc_decoder_mem(rx_needed_size, &rx_mem_cfg, &rx_buffer_size);

    uint32_t out_size = 0;
    esp_err_t ret = jpeg_decoder_process(jpgd_handle, &decode_cfg, tx_buf, frame_size, rx_buf, rx_buffer_size, &out_size);
    if (ret != ESP_OK) {
        free(jpeg_data);
        free(tx_buf);
        free(rx_buf);
        ESP_LOGE(TAG, "JPEG hardware decode failed");
        return ret;
    }

    ret = esp_lcd_panel_draw_bitmap(panel, 0, 0, EXAMPLE_IMAGE_W, EXAMPLE_IMAGE_H, rx_buf);

    free(jpeg_data);
    free(tx_buf);
    free(rx_buf);

    return ret;
}

void mjpeg_playback_task(void *pvParameters) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)pvParameters;

    FILE *f = fopen(mjpeg_filename, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open MJPEG file");
        return;
    }

    while (1) {
        ESP_LOGI(TAG, "Reading next JPEG frame from MJPEG file");

        int64_t start_time = esp_timer_get_time();
        esp_err_t ret = display_mjpeg_frame(panel, f);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to display MJPEG frame");
            fclose(f);
            return;
        }

        int64_t decode_time = (esp_timer_get_time() - start_time) / 1000;
        ESP_LOGI(TAG, "Decode time: %lld ms", decode_time);

        vTaskDelay((1000 / 30) / portTICK_PERIOD_MS);  // 控制帧率
    }

    fclose(f);
}

void app_main(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

#if !USE_JPEG_IMAGE
    if (init_mjpeg_file() != ESP_OK) {
        return;
    }
#endif

    // 初始化 JPEG 解码器
    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .intr_priority = 2,
        .timeout_ms = 40,
    };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle));

    // 初始化 LDO 和 MIPI 显示屏
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
    mjpeg_frame_queue = xQueueCreate(3, sizeof(mjpeg_frame_t));
    decoded_frame_queue = xQueueCreate(3, sizeof(decoded_frame_t));
    // 根据宏选择播放任务
#if USE_JPEG_IMAGE
    xTaskCreate(jpeg_display_task, "jpeg_display", 8192, mipi_dpi_panel, 5, NULL);
#else
xTaskCreate(mjpeg_reader_task, "reader", 8192, NULL, 5, NULL);
xTaskCreate(mjpeg_decode_task, "decoder", 8192, NULL, 6, NULL);
xTaskCreate(mjpeg_display_task, "display", 8192, mipi_dpi_panel, 7, NULL);
#endif
}
