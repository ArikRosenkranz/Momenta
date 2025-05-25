#include "esp_camera.h"
#include "SD.h"
#include "FS.h"
#include "esp_sleep.h"
#include "Arduino.h"  // wichtig für analogWrite

#define LED_PIN 44
#define BUTTON_PIN 5
#define WAKEUP_PIN GPIO_NUM_5
#define SD_CS_PIN 21

#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40
#define CAM_PIN_SIOC    39
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

const int fps = 24;
const int skipInitialFrames = 1;  // kürzere Einstellphase
const int maxFrames = 363;

int videoIndex = 1;
bool isRecording = false;
bool waitForRelease = false;

// LED-Helligkeit (0–255)
#define LED_NORMAL_BRIGHTNESS 150
#define LED_DIM_BRIGHTNESS 20

struct Frame {
  uint8_t* data;
  size_t size;
};

Frame frames[maxFrames];
int frameCount = 0;
unsigned long lastFrameTime = 0;

int getNextVideoIndex() {
  int maxIndex = 0;
  File root = SD.open("/");
  while (File f = root.openNextFile()) {
    String name = f.name();
    name.trim();
    if (name.startsWith("video_") && (name.endsWith(".mp4") || name.endsWith(".mjpeg"))) {
      int start = name.indexOf('_') + 1;
      int end = name.lastIndexOf('.');
      int idx = name.substring(start, end).toInt();
      if (idx > maxIndex) maxIndex = idx;
    }
    f.close();
  }

  if (SD.exists("/.raw")) {
    File raw = SD.open("/.raw");
    while (File f = raw.openNextFile()) {
      String name = f.name();
      name.trim();
      if (name.startsWith("video_") && name.endsWith(".mjpeg")) {
        int start = name.indexOf('_') + 1;
        int end = name.lastIndexOf('.');
        int idx = name.substring(start, end).toInt();
        if (idx > maxIndex) maxIndex = idx;
      }
      f.close();
    }
  }

  return maxIndex + 1;
}

void enterDeepSleep() {
  Serial.println("😴 Gehe jetzt in Deep Sleep, warte auf Taster...");
  esp_sleep_enable_ext0_wakeup(WAKEUP_PIN, 0); // Wakeup wenn GPIO 5 LOW ist
  delay(100);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  analogWrite(LED_PIN, 0); // LED aus bei Start

  Serial.println("\n🎬 Setup startet...");

  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("▶️ Taster erkannt – Starte Aufnahme.");
  } else {
    Serial.println("⏳ Kein Taster – 3s Upload-Fenster...");
    analogWrite(LED_PIN, LED_DIM_BRIGHTNESS);  // leicht leuchten
    delay(3000);
    Serial.println("🕳 Kein Upload? → Deep Sleep.");
    enterDeepSleep();
  }

  if (!psramFound()) {
    Serial.println("❌ Kein PSRAM erkannt!");
    while (true);
  }

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sscb_sda = CAM_PIN_SIOD;
  config.pin_sscb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("❌ Kamera-Initialisierung fehlgeschlagen!");
    enterDeepSleep();
  }

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("❌ SD-Karte nicht erkannt!");
    enterDeepSleep();
  }

  if (!SD.exists("/.raw")) SD.mkdir("/.raw");

  videoIndex = getNextVideoIndex();

  Serial.println("✅ Setup abgeschlossen.");
  analogWrite(LED_PIN, LED_DIM_BRIGHTNESS);  // gedimmt weiter
}

void loop() {
  bool pressed = digitalRead(BUTTON_PIN) == LOW;

  if (pressed && !isRecording && !waitForRelease) {
    Serial.printf("🎬 Starte Aufnahme (video_%03d.mjpeg)\n", videoIndex);
    isRecording = true;
    waitForRelease = true;
    frameCount = 0;
    lastFrameTime = millis();
    analogWrite(LED_PIN, LED_NORMAL_BRIGHTNESS); // helles Licht
  }

  if (!pressed && isRecording) {
    Serial.println("🛑 Aufnahme gestoppt (Button losgelassen).");
    isRecording = false;
    analogWrite(LED_PIN, LED_DIM_BRIGHTNESS); // wieder dimmen
    saveMJPEG();
    videoIndex++;
    enterDeepSleep();
  }

  if (!pressed && waitForRelease) {
    waitForRelease = false;
  }

  if (isRecording && frameCount < maxFrames) {
    unsigned long now = millis();
    if (now - lastFrameTime >= 1000 / fps) {
      lastFrameTime = now;

      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("⚠️ Kein Frame erhalten!");
        return;
      }

      if (frameCount >= skipInitialFrames) {
        uint8_t* copy = (uint8_t*)ps_malloc(fb->len);
        if (copy) {
          memcpy(copy, fb->buf, fb->len);
          frames[frameCount].data = copy;
          frames[frameCount].size = fb->len;
          Serial.printf("📸 Frame %d gespeichert (%d Bytes)\n", frameCount, fb->len);
        } else {
          Serial.println("❌ PSRAM voll – Aufnahme stoppt.");
          isRecording = false;
          analogWrite(LED_PIN, LED_DIM_BRIGHTNESS);
          saveMJPEG();
          videoIndex++;
          enterDeepSleep();
        }
      } else {
        Serial.printf("⏭️ Frame %d übersprungen (Einstellphase)\n", frameCount);
      }

      esp_camera_fb_return(fb);
      frameCount++;

      if (frameCount >= maxFrames) {
        Serial.println("⏱ Max. Dauer erreicht – Aufnahme gestoppt.");
        isRecording = false;
        analogWrite(LED_PIN, LED_DIM_BRIGHTNESS);
        saveMJPEG();
        videoIndex++;
        enterDeepSleep();
      }
    }
  }
}

void saveMJPEG() {
  char path[32];
  sprintf(path, "/.raw/video_%03d.mjpeg", videoIndex);
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Fehler beim Öffnen der Datei!");
    return;
  }

  Serial.printf("💾 Speichere %d Frames nach: %s\n", frameCount, path);

  int writtenFrames = 0;
  for (int i = skipInitialFrames; i < frameCount; i++) {
    if (frames[i].data && frames[i].size > 0) {
      file.write(frames[i].data, frames[i].size);
      writtenFrames++;
    }
  }

  file.close();
  Serial.println("✅ Video gespeichert.");

  float duration = writtenFrames / float(fps);
  Serial.printf("⏱ Dauer: %.2f Sekunden (%d Frames @ %d FPS)\n", duration, writtenFrames, fps);

  for (int i = 0; i < maxFrames; i++) {
    if (frames[i].data) {
      free(frames[i].data);
      frames[i].data = nullptr;
    }
  }
  frameCount = 0;
}

