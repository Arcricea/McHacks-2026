// Copyright (c) 2023 Michael Heijmans
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <stdio.h>
#include <string.h>
#include <dirent.h> // Added for directory listing
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "sd_card.h" // Shared SD card initialization

// Setup Pins
#define I2S_CLK_PIN GPIO_NUM_1  // PDM CLK (Bit Clock) - NOT CONNECTED to Amp
#define I2S_DATA_PIN GPIO_NUM_2 // PDM DATA (DOUT) -> Connect to Amp Input with RC Filter

/* WIRING GUIDE: PAM8403 (Analog Amp) + ESP32 (PDM Digital Out)
 * -------------------------------------------------------------
 * Since the ESP32-S3 doesn't have an internal DAC, we use PDM mode.
 * The PAM8403 expects an ANALOG signal.
 *
 * To make this work, you need a simple RC Low-Pass Filter on the Data Pin.
 *
 * [ESP32 Side]                    [PAM8403 Amp Input Side]
 * 5V / VBUS   ------------------>  5V +
 * GND         ------------------>  5V -
 * GND         ------------------>  ⊥ (Upside down T / Audio Ground)
 *
 * GPIO 2 (Data) -> [Resistor] -> + -> L (Left Input)
 *                    (1k-4.7k)   |
 *                                = [Capacitor] (10nF-100nF)
 *                                |
 *                               GND
 *
 * Note: If you don't have a resistor/capacitor, you can connect GPIO 2
 * directly to "L", but the sound might be harsh/noisy.
 *
 * [Speaker Side]
 * Connect your speaker wires to L+ and L- on the PAM8403.
 * DO NOT CONNECT L- or R- to GROUND! The PAM8403 outputs are "Bridged" (BTL).
 */

// defines
#define REBOOT_WAIT 5000            // reboot after 5 seconds
#define AUDIO_BUFFER 2048           // buffer size for reading the wav file and sending to i2s
#define WAV_FILE "/sdcard/test.wav" // wav file to play

// constants
static const char *TAG = "speaker_pdm";

// handles
static i2s_chan_handle_t tx_handle = NULL;

static esp_err_t i2s_setup(uint32_t sample_rate, i2s_slot_mode_t slot_mode)
{
    ESP_LOGI(TAG, "Initializing I2S PDM TX channel with Rate: %ld, Mode: %s", 
             sample_rate, slot_mode == I2S_SLOT_MODE_STEREO ? "Stereo" : "Mono");

    if (tx_handle != NULL) {
        ESP_LOGI(TAG, "I2S channel already exists, deleting...");
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }

    // Create new I2S channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    // Setup PDM TX configuration
    i2s_pdm_tx_config_t pdm_tx_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, slot_mode),
        .gpio_cfg = {
            .clk = I2S_CLK_PIN,
            .dout = I2S_DATA_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    return i2s_channel_init_pdm_tx_mode(tx_handle, &pdm_tx_cfg);
}

static esp_err_t play_wav(const char *fp)
{
    // Ensure SD card is mounted
    if (sd_card_init() != ESP_OK) {
        ESP_LOGE(TAG, "SD Card not initialized");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Opening file %s", fp);
    FILE *fh = fopen(fp, "rb");
    if (fh == NULL) {
        ESP_LOGE(TAG, "Failed to open file");
        return ESP_ERR_INVALID_ARG;
    }

    // Read WAV header (44 bytes)
    uint8_t header[44];
    if (fread(header, 1, 44, fh) != 44) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        fclose(fh);
        return ESP_FAIL;
    }

    // Parse WAV Header
    // Offset 22: Num Channels (2 bytes)
    uint16_t channels = header[22] | (header[23] << 8);
    // Offset 24: Sample Rate (4 bytes)
    uint32_t sample_rate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    // Offset 34: Bits Per Sample (2 bytes)
    uint16_t bits_per_sample = header[34] | (header[35] << 8);

    ESP_LOGI(TAG, "WAV Info - Rate: %ld Hz, Channels: %d, Bits: %d", sample_rate, channels, bits_per_sample);

    if (bits_per_sample != 16) {
        ESP_LOGE(TAG, "Only 16-bit WAV files are supported");
        fclose(fh);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Configure I2S based on file properties
    i2s_slot_mode_t mode = (channels == 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    ESP_ERROR_CHECK(i2s_setup(sample_rate, mode));

    // create a writer buffer
    int16_t *buf = (int16_t *)calloc(AUDIO_BUFFER, sizeof(int16_t));
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        fclose(fh);
        return ESP_ERR_NO_MEM;
    }

    size_t bytes_read = 0;
    size_t bytes_written = 0;

    // Read first chunk
    bytes_read = fread(buf, sizeof(int16_t), AUDIO_BUFFER, fh);

    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    int chunk_count = 0;
    while (bytes_read > 0) {
        // write the buffer to the i2s
        if (i2s_channel_write(tx_handle, buf, bytes_read * sizeof(int16_t), &bytes_written, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed");
            break;
        }
        bytes_read = fread(buf, sizeof(int16_t), AUDIO_BUFFER, fh);

        // Visual feedback for playback
        if (++chunk_count % 10 == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    printf("\n");

    ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));
    free(buf);
    fclose(fh);

    return ESP_OK;
}

static void list_sd_files(const char *path)
{
    ESP_LOGI(TAG, "Listing files in %s", path);
    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
             ESP_LOGI(TAG, "  FILE: %s", entry->d_name);
        } else if (entry->d_type == DT_DIR) {
             ESP_LOGI(TAG, "  DIR : %s", entry->d_name);
        }
    }
    closedir(dir);
}

// Renamed from app_main to avoid conflict with McHacks.cpp
void speaker_main(void)
{
    // Ensure SD is initialized first
    sd_card_init();

    // List files to help debug
    list_sd_files("/sdcard");

    // play the wav file
    ESP_LOGI(TAG, "Playing wav file");
    if (play_wav(WAV_FILE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play WAV file");
    }

    // Clean up
    if (tx_handle) {
        i2s_del_channel(tx_handle);
    }

    // We don't deinit SD card here just in case other tasks need it,
    // or we can call sd_card_deinit() if we are sure.
    // sd_card_deinit();

    ESP_LOGI(TAG, "Speaker test complete");
}