#include "esp_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mp6050.h"
#include "portmacro.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

// Change this to 1 for the second IMU device
#define DEVICE_ID 1

void imu_main();
extern "C" int app_main() {
  client_app_main();
  imu_main();
  return 0;
}

void imu_main() {
  imu_init();
  while (1) {
    IMU_DATA data = imu_read();
    data.device_id = DEVICE_ID;
    custom_queue_add(data);
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}
