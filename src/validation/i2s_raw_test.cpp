#include <Arduino.h>
#include <driver/i2s.h>

// ── Try different configs by changing CONFIG_NUM ──────────
// 0 = original (32-bit, left only)
// 1 = 32-bit, right only
// 2 = 32-bit, all (both channels) <-- using this one
// 3 = 16-bit, left only
// 4 = 16-bit MSB, left only
#define CONFIG_NUM 2

#define PIN_BCLK  26
#define PIN_WS    25
#define PIN_DATA  33

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.print("I2S raw test — config ");
  Serial.println(CONFIG_NUM);
  Serial.println("=============================");

  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  cfg.mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate       = 16000;
  cfg.intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count     = 4;
  cfg.dma_buf_len       = 256;
  cfg.use_apll          = false;
  cfg.tx_desc_auto_clear = false;
  cfg.fixed_mclk        = 0;

  #if CONFIG_NUM == 0
    Serial.println("Config: 32-bit, left channel only");
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;

  #elif CONFIG_NUM == 1
    Serial.println("Config: 32-bit, right channel only");
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;

  #elif CONFIG_NUM == 2
    Serial.println("Config: 32-bit, both channels");
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;

  #elif CONFIG_NUM == 3
    Serial.println("Config: 16-bit, left channel only");
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;

  #elif CONFIG_NUM == 4
    Serial.println("Config: 16-bit MSB, left channel only");
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  #endif

  i2s_pin_config_t pins = {
    .bck_io_num   = PIN_BCLK,
    .ws_io_num    = PIN_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = PIN_DATA
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  Serial.print("Driver install: ");
  Serial.println(err == ESP_OK ? "OK" : esp_err_to_name(err));

  err = i2s_set_pin(I2S_NUM_0, &pins);
  Serial.print("Pin config:     ");
  Serial.println(err == ESP_OK ? "OK" : esp_err_to_name(err));

  i2s_zero_dma_buffer(I2S_NUM_0);
  delay(500);
  Serial.println("Listening... speak or tap near the mic.");
  Serial.println("=============================");
}

void loop() {
  int32_t buf[64];
  size_t bytesRead = 0;

  i2s_read(I2S_NUM_0, buf, sizeof(buf), &bytesRead, portMAX_DELAY);

  int32_t peak   = 0;
  int32_t minVal = INT32_MAX;
  int32_t maxVal = INT32_MIN;

  for (int i = 0; i < 64; i++) {
    if (buf[i] > maxVal) maxVal = buf[i];
    if (buf[i] < minVal) minVal = buf[i];
    if (abs(buf[i]) > peak) peak = abs(buf[i]);
  }

  // Print first 8 raw values
  Serial.print("Raw: ");
  for (int i = 0; i < 8; i++) {
    Serial.print(buf[i]); Serial.print(" ");
  }
  Serial.println();
  Serial.print("Peak: "); Serial.print(peak);
  Serial.print("  Min: "); Serial.print(minVal);
  Serial.print("  Max: "); Serial.println(maxVal);

  if (peak == 0) {
    Serial.println("STATUS: All zeros");
  } else if (peak < 50000) {
    Serial.println("STATUS: Weak signal — try louder stimulus");
  } else {
    Serial.println("STATUS: ✓ Signal detected!");
  }
  Serial.println();
  delay(1000);
}