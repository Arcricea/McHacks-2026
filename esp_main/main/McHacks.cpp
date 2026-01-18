#include "esp_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "graphics.h"
#include "sd_card.h"
#include "speaker.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern "C" int app_main() {
  printf("\n=== ESP32 DEVKIT V1 Starting ===\n");

  // Initialize display first
  graphics_init();

  // Start graphics task (will run continuously)
  xTaskCreate(graphics_main, "graphics", 4096, NULL, 10, NULL);

  // Start server (WiFi/network stuff)
  server_app_main();

  // Test audio playback with frame skipping for high speeds
  printf("\n=== Testing Variable Playback Speed with Frame Skipping ===\n");

  printf("\n--- Playing at 1.0x speed (normal) ---\n");
  set_playback_speed(1.0f);
  speaker_main();
  vTaskDelay(pdMS_TO_TICKS(1000));

  printf("\n--- Playing at 1.5x speed ---\n");
  set_playback_speed(1.5f);
  speaker_main();
  vTaskDelay(pdMS_TO_TICKS(1000));

  printf("\n--- Playing at 2.0x speed (frame skipping!) ---\n");
  set_playback_speed(2.0f);
  speaker_main();
  vTaskDelay(pdMS_TO_TICKS(1000));

  printf("\n--- Playing at 3.0x speed (heavy frame skipping!) ---\n");
  set_playback_speed(3.0f);
  speaker_main();

  printf("\n=== Playback Speed Test Complete ===\n");
  printf("\n=== System running, graphics task active ===\n");

  // Keep app_main alive forever - don't return!
  while(1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  return 0;
}
