#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>

#define I2S_BCLK 26
#define I2S_LRCL 25
#define I2S_DOUT 33

void setup() {
  Serial.begin(115200);
  delay(1000);

  // ── I2C scan ──────────────────────────────────────────
  Serial.println("--- I2C ---");
  Wire.begin(21, 22);
  byte found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("0x"); Serial.print(addr, HEX);
      if (addr == 0x53) Serial.print(" (ADXL345)");
      if (addr == 0x68) Serial.print(" (DS3231)");
      if (addr == 0x76) Serial.print(" (BME688)");
      Serial.println();
      found++;
    }
  }
  Serial.print(found); Serial.println("/3 I2C devices found");

  // ── I2S scan ──────────────────────────────────────────
  Serial.println("\n--- I2S ---");
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = 16000,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = 0,
    .dma_buf_count        = 2,
    .dma_buf_len          = 64,
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_BCLK,
    .ws_io_num    = I2S_LRCL,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_DOUT
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  if (err == ESP_OK) err = i2s_set_pin(I2S_NUM_0, &pins);

  if (err == ESP_OK) {
    Serial.println("INMP441 I2S init OK (BCLK=26, LRCL=25, DOUT=33)");
    i2s_driver_uninstall(I2S_NUM_0);
  } else {
    Serial.print("I2S FAILED: "); Serial.println(esp_err_to_name(err));
  }
}

void loop() {}