#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS 4

File logFile;
unsigned long startMs = 0;

void setup() {
  // Onboard LED as a heartbeat indicator
  pinMode(2, OUTPUT);

  SPI.begin(18, 19, 23, SD_CS);
  if (!SD.begin(SD_CS)) {
    // Rapid blink = SD failed to mount
    while (true) {
      digitalWrite(2, HIGH); delay(200);
      digitalWrite(2, LOW);  delay(200);
    }
  }

  logFile = SD.open("/power_test.txt", FILE_WRITE);
  startMs = millis();
  if (logFile) {
    logFile.println("LiPo Battery Power test started.");
    logFile.flush();
  }

  // Slow blink = running normally
  digitalWrite(2, HIGH);
}

void loop() {
  unsigned long elapsed = (millis() - startMs) / 1000;
  unsigned long hrs  = elapsed / 3600;
  unsigned long mins = (elapsed % 3600) / 60;
  unsigned long secs = elapsed % 60;

  char buf[40];
  snprintf(buf, sizeof(buf), "Uptime: %02lu:%02lu:%02lu", hrs, mins, secs);

  if (logFile) {
    logFile.println(buf);
    logFile.flush(); // flush every write so data survives a reset
  }
  delay(60000);
  digitalWrite(2, LOW);  delay(100);
  digitalWrite(2, HIGH);
}