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

// I2S PDM sample rate limits for ESP32-S3
#define I2S_PDM_MIN_RATE 8000       // Minimum supported sample rate
#define I2S_PDM_MAX_RATE 48000      // Maximum reliable sample rate for PDM TX on ESP32-S3

// constants
static const char *TAG = "speaker_pdm";

// handles
static i2s_chan_handle_t tx_handle = NULL;

// Playback speed control (1.0 = normal, 2.0 = 2x speed, 0.5 = half speed)
static float playback_speed = 1.0f;

static esp_err_t i2s_setup(uint32_t sample_rate, i2s_slot_mode_t slot_mode)
{
    ESP_LOGI(TAG, "Initializing I2S PDM TX channel with Rate: %ld, Mode: %s", 
             sample_rate, slot_mode == I2S_SLOT_MODE_STEREO ? "Stereo" : "Mono");

    // Channel should always be NULL here (cleaned up after each playback)
    if (tx_handle != NULL) {
        ESP_LOGW(TAG, "I2S channel handle is not NULL, this shouldn't happen!");
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

    // Apply playback speed adjustment with frame skipping for speeds above hardware limit
    uint32_t adjusted_sample_rate = (uint32_t)(sample_rate * playback_speed);
    float frame_skip_ratio = 1.0f;  // 1.0 = play all frames, 2.0 = skip every other frame

    // Check if we exceed hardware limits
    if (adjusted_sample_rate > I2S_PDM_MAX_RATE) {
        // Use frame skipping for speeds above hardware limit
        frame_skip_ratio = (float)adjusted_sample_rate / I2S_PDM_MAX_RATE;
        adjusted_sample_rate = I2S_PDM_MAX_RATE;

        ESP_LOGI(TAG, "Speed %.2fx exceeds hardware limit (max %.2fx for %ld Hz)",
                 playback_speed, (float)I2S_PDM_MAX_RATE / sample_rate, sample_rate);
        ESP_LOGI(TAG, "Using frame skipping: playing at %d Hz, skipping %.1f%% of samples",
                 I2S_PDM_MAX_RATE, (frame_skip_ratio - 1.0f) * 100.0f / frame_skip_ratio);
        ESP_LOGI(TAG, "Effective speed: %.2fx (hardware %.2fx + frame skip %.2fx)",
                 playback_speed, (float)I2S_PDM_MAX_RATE / sample_rate, frame_skip_ratio);
    } else if (adjusted_sample_rate < I2S_PDM_MIN_RATE) {
        ESP_LOGW(TAG, "Adjusted sample rate %ld Hz is below PDM minimum, clamping to %d Hz",
                 adjusted_sample_rate, I2S_PDM_MIN_RATE);
        adjusted_sample_rate = I2S_PDM_MIN_RATE;
    }
    
    ESP_LOGI(TAG, "Playback speed: %.2fx (Original: %ld Hz -> Adjusted: %ld Hz)",
             playback_speed, sample_rate, adjusted_sample_rate);

    // Configure I2S based on file properties
    i2s_slot_mode_t mode = (channels == 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    ESP_ERROR_CHECK(i2s_setup(adjusted_sample_rate, mode));

    // create a writer buffer
    int16_t *buf = (int16_t *)calloc(AUDIO_BUFFER, sizeof(int16_t));
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        fclose(fh);
        return ESP_ERR_NO_MEM;
    }

    size_t bytes_read = 0;
    size_t bytes_written = 0;

    // Frame skipping state
    float frame_position = 0.0f;
    size_t samples_skipped = 0;
    size_t samples_played = 0;

    // Read first chunk
    bytes_read = fread(buf, sizeof(int16_t), AUDIO_BUFFER, fh);

    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    int chunk_count = 0;
    while (bytes_read > 0) {
        // Apply frame skipping if needed
        if (frame_skip_ratio > 1.0f) {
            // Use fractional frame position for accurate skipping
            size_t samples_to_process = bytes_read;
            size_t samples_to_write = 0;

            // Reset frame position for each chunk
            frame_position = 0.0f;

            // Fractional decimation: works for any ratio (2.0x, 2.5x, etc.)
            while (frame_position < samples_to_process) {
                size_t input_index = (size_t)frame_position;
                if (input_index < samples_to_process) {
                    buf[samples_to_write] = buf[input_index];
                    samples_to_write++;
                }
                frame_position += frame_skip_ratio;
            }

            samples_skipped += (samples_to_process - samples_to_write);
            samples_played += samples_to_write;
            bytes_read = samples_to_write;
        }

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

    // Log frame skipping statistics if used
    if (frame_skip_ratio > 1.0f && samples_skipped > 0) {
        ESP_LOGI(TAG, "Frame skipping stats: Played %zu samples, Skipped %zu samples (%.1f%%)",
                 samples_played, samples_skipped,
                 (float)samples_skipped * 100.0f / (samples_played + samples_skipped));
    }

    // Delete the channel after playback to free resources
    ESP_LOGI(TAG, "Cleaning up I2S channel");
    esp_err_t ret = i2s_del_channel(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete I2S channel: %s", esp_err_to_name(ret));
    }
    tx_handle = NULL;  // Important: Set to NULL after deletion

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

#ifdef __cplusplus
extern "C" {
#endif

// Set playback speed (1.0 = normal, 2.0 = 2x speed, 0.5 = half speed)
void set_playback_speed(float speed)
{
    if (speed <= 0.0f) {
        ESP_LOGW(TAG, "Invalid playback speed %.2f, must be > 0. Using 1.0", speed);
        playback_speed = 1.0f;
    } else if (speed > 4.0f) {
        ESP_LOGW(TAG, "Playback speed %.2f is very high, clamping to 4.0x", speed);
        playback_speed = 4.0f;
    } else if (speed < 0.25f) {
        ESP_LOGW(TAG, "Playback speed %.2f is very low, clamping to 0.25x", speed);
        playback_speed = 0.25f;
    } else {
        playback_speed = speed;
    }
    ESP_LOGI(TAG, "Playback speed set to %.2fx", playback_speed);
}

// Get current playback speed
float get_playback_speed(void)
{
    return playback_speed;
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

    // Channel is already cleaned up in play_wav()
    // We don't deinit SD card here just in case other tasks need it

    ESP_LOGI(TAG, "Speaker test complete");
}

#ifdef __cplusplus
}
#endif

