/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <dirent.h>
#include <sys/stat.h>
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
 #include "jpeg_decoder.h"
 #include "esp_timer.h"
 `
 #define EXAMPLE_IMAGE_H 600
 #define EXAMPLE_IMAGE_W 1024
 #define MAX_MJPEG_FILES 20  // Maximum number of MJPEG files to handle
 
 static const char *TAG = "mjpeg_player";
 
 typedef struct {
     char filename[256];  // 将缓冲区从64扩大到256字节
     size_t size;
 } mjpeg_file_t;
 
 // Global variables for MJPEG playback
 static mjpeg_file_t mjpeg_files[MAX_MJPEG_FILES];
 static int file_count = 0;
 static int current_file = 0;
 
 // Function to scan directory for MJPEG files
 esp_err_t scan_mjpeg_files(const char *base_path) {
     DIR *dir = opendir(base_path);
     if (!dir) {
         ESP_LOGE(TAG, "Failed to open directory: %s", base_path);
         return ESP_FAIL;
     }
 
     struct dirent *entry;
     while ((entry = readdir(dir)) != NULL && file_count < MAX_MJPEG_FILES) {
         // Check if file has .mjpg or .mjpeg extension
         char *ext = strrchr(entry->d_name, '.');
         if (ext && (strcasecmp(ext, ".mjpg") == 0 || strcasecmp(ext, ".mjpeg") == 0)) {
             int needed = snprintf(NULL, 0, "%s/%s", base_path, entry->d_name);
             if (needed >= sizeof(mjpeg_files[file_count].filename)) {
                 ESP_LOGW(TAG, "Path too long: %s/%s (max %d)", base_path, entry->d_name, sizeof(mjpeg_files[file_count].filename));
                 continue;
             }
             
             int written = snprintf(mjpeg_files[file_count].filename, sizeof(mjpeg_files[file_count].filename), 
                     "%s/%s", base_path, entry->d_name);
             if (written < 0 || written >= sizeof(mjpeg_files[file_count].filename)) {
                 ESP_LOGE(TAG, "Path encoding error");
                 continue;
             }
             
             // Get file size
             FILE *f = fopen(mjpeg_files[file_count].filename, "rb");
             if (f) {
                 fseek(f, 0, SEEK_END);
                 mjpeg_files[file_count].size = ftell(f);
                 fclose(f);
                 file_count++;
             }
         }
     }
     closedir(dir);
 
     if (file_count == 0) {
         ESP_LOGE(TAG, "No MJPEG files found in directory: %s", base_path);
         return ESP_FAIL;
     }
 
     ESP_LOGI(TAG, "Found %d MJPEG files", file_count);
     return ESP_OK;
 }
 
 // Function to decode and display MJPEG frame
 esp_err_t display_mjpeg_frame(const char *filename, esp_lcd_panel_handle_t panel) {
     FILE *f = fopen(filename, "rb");
     if (!f) {
         ESP_LOGE(TAG, "Failed to open file: %s", filename);
         return ESP_FAIL;
     }
 
     uint8_t *jpeg_data = malloc(mjpeg_files[current_file].size);
     if (!jpeg_data) {
         fclose(f);
         ESP_LOGE(TAG, "Failed to allocate memory for JPEG data");
         return ESP_ERR_NO_MEM;
     }
 
     size_t read = fread(jpeg_data, 1, mjpeg_files[current_file].size, f);
     fclose(f);
 
     if (read != mjpeg_files[current_file].size) {
         free(jpeg_data);
         ESP_LOGE(TAG, "Failed to read complete file");
         return ESP_FAIL;
     }
 
     // Allocate buffer for decoded image
     uint16_t *pixels = calloc(EXAMPLE_IMAGE_H * EXAMPLE_IMAGE_W, sizeof(uint16_t));
     if (!pixels) {
         free(jpeg_data);
         ESP_LOGE(TAG, "Failed to allocate memory for decoded image");
         return ESP_ERR_NO_MEM;
     }
 
     // JPEG decode config
     esp_jpeg_image_cfg_t jpeg_cfg = {
         .indata = jpeg_data,
         .indata_size = mjpeg_files[current_file].size,
         .outbuf = (uint8_t*)pixels,
         .outbuf_size = EXAMPLE_IMAGE_W * EXAMPLE_IMAGE_H * sizeof(uint16_t),
         .out_format = JPEG_IMAGE_FORMAT_RGB565,
         .out_scale = JPEG_IMAGE_SCALE_0,
         .flags = {
             .swap_color_bytes = 0
         }
     };
 
     // JPEG decode
     esp_jpeg_image_output_t outimg;
     esp_err_t ret = esp_jpeg_decode(&jpeg_cfg, &outimg);
     if (ret != ESP_OK) {
         free(jpeg_data);
         free(pixels);
         ESP_LOGE(TAG, "JPEG decode failed");
         return ret;
     }
     
     // 删除无效的刷新调用，改用正确的显示流程
     ret = esp_lcd_panel_draw_bitmap(panel, 0, 0, outimg.width, outimg.height, pixels);
     
     free(jpeg_data);
     free(pixels);
     
     return ret;
 }
 
 void mjpeg_playback_task(void *pvParameters) {
     esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)pvParameters;
     
     while (1) {
         ESP_LOGI(TAG, "Playing file: %s", mjpeg_files[current_file].filename);
         
         // 增加精确的时间测量（单位：微秒）
         int64_t start_time = esp_timer_get_time();
         esp_err_t ret = display_mjpeg_frame(mjpeg_files[current_file].filename, panel);
         int64_t decode_time = (esp_timer_get_time() - start_time) / 1000;
         ESP_LOGI(TAG, "Decode time: %lld ms", decode_time);
         
         // 根据实际解码时间动态调整延迟（保留至少100ms余量）
         vTaskDelay((decode_time + 100) / portTICK_PERIOD_MS);
         // Move to next file
         current_file = (current_file + 1) % file_count;
         // 当循环到第一个文件时重置文件系统
         if (current_file == 0) {
             fflush(stdin);
             ESP_LOGI(TAG, "Restarting playback cycle");
         }
         
         // Add a small delay between frames (adjust as needed)
         // 增加延迟到500ms（根据实际解码时间调整）
         vTaskDelay(500 / portTICK_PERIOD_MS);
     }
 }
 
 void app_main(void) {
     // Initialize SPIFFS
     esp_vfs_spiffs_conf_t conf = {
         .base_path = "/spiffs",
         .partition_label = "storage",
         .max_files = 5,
         .format_if_mount_failed = true
     };
     ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
     
     // Scan for MJPEG files
     if (scan_mjpeg_files("/spiffs") != ESP_OK) {
         return;
     }
 
     //---------------MIPI LDO Init------------------//
     esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
     esp_ldo_channel_config_t ldo_mipi_phy_config = {
         .chan_id = CONFIG_EXAMPLE_USED_LDO_CHAN_ID,
         .voltage_mv = CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV,
     };
     ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
 
     //---------------DSI Init------------------//
     esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
     esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
     esp_lcd_panel_handle_t mipi_dpi_panel = NULL;
     
     example_dsi_resource_alloc(&mipi_dsi_bus, &mipi_dbi_io, &mipi_dpi_panel, NULL);
     example_dpi_panel_reset(mipi_dpi_panel);
     example_dpi_panel_init(mipi_dpi_panel);
 
     // Create playback task
     xTaskCreate(mjpeg_playback_task, "mjpeg_playback", 4096, mipi_dpi_panel, 5, NULL);
 }