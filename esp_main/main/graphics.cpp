#include "lgfx/v1/misc/enum.hpp"
#define LGFX_USE_V1

#include "esp_log.h"
#include "freertos/projdefs.h"

#include <LovyanGFX.hpp>

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();

      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 320000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_dc = 9;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();

      cfg.pin_cs = 10;
      cfg.pin_rst = 8;
      cfg.pin_busy = -1;
      cfg.panel_width = SCREEN_WIDTH;
      cfg.panel_height = SCREEN_HEIGHT;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }

    {
      auto cfg = _light_instance.config();

      cfg.pin_bl = 4;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;

      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    setPanel(&_panel_instance);
  }
};

struct Junimo {
  int32_t x;
  int32_t y;
  int32_t dx;
  int32_t dy;

  size_t anim_frame;

  void move() {
    x += dx;
    y += dy;
    if (x < 0) {
      x = 0;
      if (dx < 0)
        dx = -dx;
    } else if (x >= SCREEN_WIDTH) {
      x = SCREEN_WIDTH - 1;
      if (dx > 0)
        dx = -dx;
    }
    if (y < 0) {
      y = 0;
      if (dy < 0)
        dy = -dy;
    } else if (y >= SCREEN_HEIGHT) {
      y = SCREEN_HEIGHT - 1;
      if (dy > 0)
        dy = -dy;
    }
  }
};

#define N_ANIM_FRAMES 8
#define N_JUNIMOS 2

const uint16_t TRANSPARENT = TFT_GREEN;

static LGFX lcd;
static LGFX_Sprite buffers[2];
static uint8_t currentBuffer = 0;

static Junimo junimos[N_JUNIMOS];
static LGFX_Sprite junimoAnimationFrames[N_ANIM_FRAMES];

void graphics_init() {
  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(128);
  lcd.setColorDepth(16);

  lcd.clear();

  for (int i = 0; i < 2; i++) {
    buffers[i].setPsram(false);
    buffers[i].setColorDepth(16);
    buffers[i].createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
  }
}

void graphics_main() {
  lcd.startWrite();
  uint8_t hue = 0;

  while (1) {

    LGFX_Sprite *drawBuffer = &buffers[currentBuffer];

    uint8_t region = hue / 43;
    uint8_t remainder = (hue - (region * 43)) * 6;
    uint8_t p = 0;
    uint8_t q = (200 * (255 - remainder)) / 255;
    uint8_t t = (200 * remainder) / 255;
    uint8_t r, g, b;
    switch (region) {
    case 0:
      r = 200;
      g = t;
      b = p;
      break;
    case 1:
      r = q;
      g = 200;
      b = p;
      break;
    case 2:
      r = p;
      g = 200;
      b = t;
      break;
    case 3:
      r = p;
      g = q;
      b = 200;
      break;
    case 4:
      r = t;
      g = p;
      b = 200;
      break;
    default:
      r = 200;
      g = p;
      b = q;
      break;
    }
    drawBuffer->fillScreen(drawBuffer->color565(r, g, b));
    hue += 1;
    lcd.display();

    currentBuffer = 1 - currentBuffer;

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}
