#include "core/arduino_compat.h"
#include "backlight.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

namespace backlight {

static Cfg  g_cfg;
static bool g_ready = false;
static bool g_pwmOn = false;
static uint8_t g_current_level = 0;

static void gpio_set_level_from_byte(uint8_t level) {
    gpio_set_level((gpio_num_t)g_cfg.pin, level ? 1 : 0);
}

bool begin(const Cfg &cfg) {
  g_cfg = cfg;
  // Configure pin as output (replaces Arduino pinMode)
  gpio_config_t io_conf = {};
  io_conf.pin_bit_mask = (1ULL << g_cfg.pin);
  io_conf.mode         = GPIO_MODE_OUTPUT;
  io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&io_conf);
  gpio_set_level_from_byte(0);  // start OFF
  g_pwmOn = false;

  if (g_cfg.usePwm) {
    // Fixed, widely-supported setup: 8-bit duty @ 20 kHz on LOW_SPEED
    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode      = LEDC_LOW_SPEED_MODE;
    tcfg.duty_resolution = LEDC_TIMER_8_BIT;    // <-- keep it simple & portable
    tcfg.timer_num       = (ledc_timer_t)g_cfg.ledcTimer; // 0..3
    tcfg.freq_hz         = (uint32_t)g_cfg.freqHz;        // e.g. 20 kHz
    tcfg.clk_cfg         = LEDC_AUTO_CLK;
    esp_err_t e1 = ledc_timer_config(&tcfg);

    ledc_channel_config_t c = {};
    c.speed_mode     = LEDC_LOW_SPEED_MODE;
    c.channel        = (ledc_channel_t)g_cfg.ledcChan;    // 0..7
    c.timer_sel      = (ledc_timer_t)g_cfg.ledcTimer;
    c.intr_type      = LEDC_INTR_DISABLE;
    c.gpio_num       = g_cfg.pin;                         // GPIO6
    c.duty           = 0;
    c.hpoint         = 0;
    esp_err_t e2 = ledc_channel_config(&c);

    if (e1 == ESP_OK && e2 == ESP_OK) g_pwmOn = true;
  }

  g_ready = true;
  return true;
}

void set(uint8_t level) {
  if (!g_ready) return;
  g_current_level = level;  // Track current level
  if (g_pwmOn) {
    // 8-bit duty (0..255) directly
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)g_cfg.ledcChan, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)g_cfg.ledcChan);
  } else {
    gpio_set_level_from_byte(level);
  }
}

void setPercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  uint8_t level = (percent * 255) / 100;
  set(level);
}

uint8_t getPercent() {
  return (g_current_level * 100) / 255;
}

void on()  { set(255); }
void off() { set(0);   }

} // namespace backlight