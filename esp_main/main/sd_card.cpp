/* SD card and FAT filesystem example.
   This example uses SPI peripheral to communicate with SD card.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "sd_test_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#define EXAMPLE_MAX_CHAR_SIZE    64
#define BYTE_QUEUE_SIZE          100
#define READ_TASK_STACK_SIZE     4096
#define PROCESS_TASK_STACK_SIZE  4096
#define MOUNT_POINT              "/sdcard"

static const char *TAG = "example";

// Global SD card handles
static sdmmc_card_t *card;
static sdmmc_host_t host;
static const char mount_point[] = MOUNT_POINT;
bool sd_card_mounted = false;

// FreeRTOS queue for byte-by-byte processing
static QueueHandle_t byte_queue = NULL;

// Special marker to indicate end of file
#define EOF_MARKER 0xFF

#ifdef CONFIG_EXAMPLE_DEBUG_PIN_CONNECTIONS
// Debug pin configuration disabled - using direct pin definitions above
#endif //CONFIG_EXAMPLE_DEBUG_PIN_CONNECTIONS

// SD Card SPI Pin Configuration (SPI3_HOST)
// These pins arechosen to avoid conflicts with the display (SPI2_HOST uses GPIO 8-12)
#define PIN_NUM_MISO  13  // SD Card MISO
#define PIN_NUM_MOSI  14  // SD Card MOSI
#define PIN_NUM_CLK   15  // SD Card CLK
#define PIN_NUM_CS    16  // SD Card CS

static esp_err_t s_example_write_file(const char *path, uint8_t *data, size_t data_len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);
    ESP_LOGI(TAG, "Wrote %d bytes to %s", written, path);
    return (written == data_len) ? ESP_OK : ESP_FAIL;
}

static void file_reader_task(void *pvParameters)
{
    const char *file_path = (const char *)pvParameters;
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Reader: Failed to open %s", file_path);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Reader: Reading %s byte-by-byte", file_path);
    int byte_count = 0, ch;

    while ((ch = fgetc(f)) != EOF) {
        uint8_t byte = (uint8_t)ch;
        if (xQueueSend(byte_queue, &byte, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Reader: Queue send failed");
            break;
        }
        ESP_LOGI(TAG, "Reader: Byte #%d: 0x%02X", ++byte_count, byte);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    fclose(f);
    ESP_LOGI(TAG, "Reader: Finished %d bytes", byte_count);

    uint8_t eof_marker = EOF_MARKER;
    xQueueSend(byte_queue, &eof_marker, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void byte_processor_task(void *pvParameters)
{
    uint8_t byte, buffer[EXAMPLE_MAX_CHAR_SIZE + 1] = {0};
    int processed_count = 0, buffer_idx = 0;

    ESP_LOGI(TAG, "Processor: Started, waiting for bytes...");

    while (xQueueReceive(byte_queue, &byte, portMAX_DELAY) == pdTRUE) {
        if (byte == EOF_MARKER) {
            ESP_LOGI(TAG, "Processor: EOF marker received");
            if (buffer_idx > 0) {
                ESP_LOGI(TAG, "Processor: Final buffer (%d bytes):", buffer_idx);
                ESP_LOG_BUFFER_HEX(TAG, buffer, buffer_idx);
            }
            break;
        }

        ESP_LOGI(TAG, "Processor: Byte #%d: 0x%02X", ++processed_count, byte);

        if (buffer_idx < EXAMPLE_MAX_CHAR_SIZE) buffer[buffer_idx++] = byte;

        if (buffer_idx >= EXAMPLE_MAX_CHAR_SIZE) {
            ESP_LOGI(TAG, "Processor: Accumulated data (%d bytes):", buffer_idx);
            ESP_LOG_BUFFER_HEX(TAG, buffer, buffer_idx);
            buffer_idx = 0;
            memset(buffer, 0, sizeof(buffer));
        }
    }

    ESP_LOGI(TAG, "Processor: Finished %d bytes", processed_count);
    vTaskDelete(NULL);
}

static esp_err_t s_example_read_file_byte_by_byte(const char *path)
{
    ESP_LOGI(TAG, "Starting byte-by-byte reading with FreeRTOS queue");

    if (!byte_queue) {
        byte_queue = xQueueCreate(BYTE_QUEUE_SIZE, sizeof(uint8_t));
        if (!byte_queue) {
            ESP_LOGE(TAG, "Failed to create byte queue");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Created byte queue (size: %d)", BYTE_QUEUE_SIZE);
    }

    if (xTaskCreate(file_reader_task, "file_reader", READ_TASK_STACK_SIZE,
                    (void *)path, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reader task");
        return ESP_FAIL;
    }

    if (xTaskCreate(byte_processor_task, "byte_processor", PROCESS_TASK_STACK_SIZE,
                    NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create processor task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Reader and processor tasks created");
    vTaskDelay(pdMS_TO_TICKS(5000));
    return ESP_OK;
}

esp_err_t sd_card_init(void)
{
    if (sd_card_mounted) return ESP_OK;

    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ESP_LOGI(TAG, "Initializing SD card using SPI peripheral");
    host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_config = {.ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID};
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create on-chip LDO power control driver");
        return ret;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    if ((ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus");
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize card (%s).", esp_err_to_name(ret));
        }
        spi_bus_free((spi_host_device_t)host.slot);
        return ret;
    }
    ESP_LOGI(TAG, "Filesystem mounted");
    sdmmc_card_print_info(stdout, card);
    sd_card_mounted = true;
    return ESP_OK;
}

void sd_card_deinit(void)
{
    if (!sd_card_mounted) return;

    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Card unmounted");
    spi_bus_free((spi_host_device_t)host.slot);

#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    if (host.pwr_ctrl_handle) {
        sd_pwr_ctrl_del_on_chip_ldo(host.pwr_ctrl_handle);
    }
#endif
    sd_card_mounted = false;
}

void spi_main(void)
{
    if (sd_card_init() != ESP_OK) return;

    const char *file_hello = MOUNT_POINT"/hello.txt";
    uint8_t data[EXAMPLE_MAX_CHAR_SIZE];
    int data_len = snprintf((char*)data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Hello", card->cid.name);
    if (s_example_write_file(file_hello, data, data_len) != ESP_OK) {
        sd_card_deinit();
        return;
    }

    const char *file_foo = MOUNT_POINT"/foo.txt";
    struct stat st;
    if (stat(file_foo, &st) == 0) unlink(file_foo);

    ESP_LOGI(TAG, "Renaming %s to %s", file_hello, file_foo);
    if (rename(file_hello, file_foo) != 0) {
        ESP_LOGE(TAG, "Rename failed");
        sd_card_deinit();
        return;
    }

    if (s_example_read_file_byte_by_byte(file_foo) != ESP_OK) {
        sd_card_deinit();
        return;
    }

#ifdef CONFIG_EXAMPLE_FORMAT_SD_CARD
    esp_err_t ret;
    if ((ret = esp_vfs_fat_sdcard_format(mount_point, card)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to format FATFS (%s)", esp_err_to_name(ret));
        sd_card_deinit();
        return;
    }
    ESP_LOGI(TAG, stat(file_foo, &st) == 0 ? "file still exists" : "file doesn't exist, formatting done");
#endif

    const char *file_nihao = MOUNT_POINT"/nihao.txt";
    memset(data, 0, EXAMPLE_MAX_CHAR_SIZE);
    data_len = snprintf((char*)data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Nihao", card->cid.name);
    if (s_example_write_file(file_nihao, data, data_len) != ESP_OK) {
        sd_card_deinit();
        return;
    }
    s_example_read_file_byte_by_byte(file_nihao);

    sd_card_deinit();
}