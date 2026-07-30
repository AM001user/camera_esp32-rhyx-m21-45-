#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "img_converters.h"   // frame2jpg()

// --- WiFi AP for quick test ---
const char* AP_SSID = "ESP32-CAM-TEST";
const char* AP_PASSWORD = "12345678";

WebServer server(80);

// --- Pin mapping (AI-Thinker / common ESP32-CAM) ---
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define PCLK_GPIO_NUM     22
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23

// Qualite JPEG logicielle (0-63 : plus bas = plus net / plus lourd)
#define JPEG_QUALITY 12

// FPS cible pour le flux live. Reste bas expres pour ne pas saturer
// le WiFi AP ni le CPU -> monte prudemment (5-8 max raisonnable en QQVGA).
#define STREAM_FPS 5

static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART_HDR = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void printSystemInfo() {
  Serial.println("=== Camera Test ESP32 ===");
  Serial.printf("Chip model : %s\n", ESP.getChipModel());
  Serial.printf("Chip revision : %d\n", ESP.getChipRevision());
  Serial.printf("CPU frequency : %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Free heap : %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Flash size : %u bytes\n", ESP.getFlashChipSize());
  Serial.printf("PSRAM found : %s\n", psramFound() ? "yes" : "no");
}

// Snapshot unique en JPEG (garde le comportement precedent)
static void sendJpeg(camera_fb_t *fb) {
  uint8_t *jpg_buf = NULL;
  size_t jpg_len = 0;

  bool ok = frame2jpg(fb, JPEG_QUALITY, &jpg_buf, &jpg_len);
  if (!ok) {
    Serial.println("[CAP] Echec conversion JPEG");
    server.send(500, "text/plain", "JPEG conversion failed");
    return;
  }

  WiFiClient client = server.client();
  if (!client) {
    free(jpg_buf);
    return;
  }
  client.setNoDelay(true);
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: image/jpeg\r\n");
  client.print(String("Content-Length: ") + jpg_len + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.write(jpg_buf, jpg_len);
  client.flush();
  client.stop();

  free(jpg_buf);
}

void handleCapture() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    server.send(503, "text/plain", "Camera capture failed");
    return;
  }
  sendJpeg(fb);
  esp_camera_fb_return(fb);
}

// Flux MJPEG en direct : boucle qui capture, convertit et envoie chaque
// frame jusqu'a ce que le client se deconnecte. Un <img src="/stream">
// dans le navigateur lit ce type de flux nativement, sans JS.
void handleStream() {
  WiFiClient client = server.client();
  if (!client) {
    return;
  }
  client.setNoDelay(true);

  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("Connection: close\r\n\r\n");

  Serial.println("[STREAM] client connecte");

  const uint32_t frame_interval_ms = 1000 / STREAM_FPS;
  char part_buf[64];

  while (client.connected()) {
    unsigned long t_start = millis();

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[STREAM] capture echouee");
      break;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool ok = frame2jpg(fb, JPEG_QUALITY, &jpg_buf, &jpg_len);
    esp_camera_fb_return(fb);

    if (!ok) {
      Serial.println("[STREAM] conversion JPEG echouee");
      break;
    }

    client.write(STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART_HDR, (unsigned)jpg_len);
    client.write(part_buf, hlen);
    client.write(jpg_buf, jpg_len);

    free(jpg_buf);

    // Regule le FPS : on complete jusqu'a l'intervalle cible si la frame
    // a ete plus rapide a produire que le budget de temps alloue.
    unsigned long elapsed = millis() - t_start;
    if (elapsed < frame_interval_ms) {
      delay(frame_interval_ms - elapsed);
    }

    yield(); // laisse la stack WiFi/systeme respirer, evite le watchdog
  }

  client.stop();
  Serial.println("[STREAM] client deconnecte");
}

void handleIndex() {
  String html = "<html><head><title>ESP32-CAM Test</title></head><body>"
                "<h3>ESP32-CAM - Flux live (" + String(STREAM_FPS) + " fps)</h3>"
                "<p><img src=\"/stream\" style=\"width:480px;image-rendering:pixelated;\"></p>"
                "<p><a href=\"/capture.jpg\">Snapshot unique</a></p>"
                "</body></html>";
  server.send(200, "text/html", html);
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000; // redescends a 10000000 si tu revois les EV-VSYNC-OVF s'aggraver
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QQVGA; // 160x120
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST; // toujours la frame la plus recente, utile en continu
  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println("Warning: PSRAM not found — using internal RAM (may be unstable for large frames)");
  }

  Serial.println("Camera config: RGB565, XCLK=20MHz, QQVGA, fb_count=2");
  esp_err_t err = esp_camera_init(&config);
  if (err == ESP_OK) {
    Serial.println("Camera initialized (RGB565)");
    return true;
  }

  Serial.printf("Camera init failed: 0x%x\n", err);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  printSystemInfo();

  WiFi.mode(WIFI_AP);
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.setSleep(false);
  if (apStarted) {
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Failed to start AP");
  }

  Serial.println("Initializing camera...");
  if (!initCamera()) {
    Serial.println("Camera init failed - aborting camera functionality");
  }

  server.on("/", handleIndex);
  server.on("/capture.jpg", handleCapture);
  server.on("/stream", handleStream);
  server.begin();
}

void loop() {
  server.handleClient();
  delay(2);
}