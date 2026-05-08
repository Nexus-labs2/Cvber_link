/*
 * ╔═══════════════════════════════════════════════════════╗
 * ║  CYBERPUNK WALKIE - ESP32 RECEIVER (RX NODE)          ║
 * ║  WebSocket RX → AES-128 CTR Decrypt → MAX98357A I2S  ║
 * ╚═══════════════════════════════════════════════════════╝
 *
 * Hardware:
 *   - ESP32
 *   - MAX98357A I2S Amplifier
 *     BCLK  → GPIO 26
 *     LRC   → GPIO 25  (Word Select)
 *     DIN   → GPIO 22  (Data OUT to amp)
 *     SD    → 3.3V or GPIO (HIGH = enabled)
 *     GAIN  → Float (15dB) or GND (12dB) or 3.3V (18dB)
 *
 * Libraries Required:
 *   - ArduinoWebsockets  (by Gil Maimon)
 *   - AESLib             (by DavyLandman)
 *
 * NOTE: MAX98357A is mono. Use I2S_CHANNEL_FMT_ONLY_LEFT
 *       or RIGHT depending on which channel your amp is set.
 */

#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <driver/i2s.h>
#include <AESLib.h>

using namespace websockets;

// ─── CONFIG ──────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* WS_SERVER_URL = "ws://YOUR-RENDER-APP.onrender.com";

// AES-128 key - MUST match transmitter exactly
uint8_t AES_KEY[16] = {
  0xDE, 0xAD, 0xBE, 0xEF,
  0xCA, 0xFE, 0xBA, 0xBE,
  0x13, 0x37, 0x42, 0x69,
  0xAB, 0xCD, 0xEF, 0x01
};

// ─── MAX98357A I2S OUTPUT PINS ───────────────────────────
#define I2S_PORT          I2S_NUM_0
#define I2S_BCLK_PIN      26   // Bit Clock
#define I2S_WS_PIN        25   // Word Select / LRC
#define I2S_DOUT_PIN      22   // Data OUT → MAX98357A DIN
#define SAMPLE_RATE       8000
#define SAMPLE_BITS       16
#define AUDIO_BUFFER_SIZE 512

// ─── GLOBALS ─────────────────────────────────────────────
WebsocketsClient wsClient;
AESLib aesLib;
bool wsConnected = false;

// Audio jitter buffer for smooth playback
#define JITTER_SLOTS 4
uint8_t jitterBuffer[JITTER_SLOTS][AUDIO_BUFFER_SIZE * 2];
uint16_t jitterLengths[JITTER_SLOTS];
int jitterWrite = 0;
int jitterRead  = 0;
int jitterCount = 0;
SemaphoreHandle_t jitterMutex;

// Decryption buffer
uint8_t decryptedBuf[AUDIO_BUFFER_SIZE * 2 + 16];

// ─── I2S OUTPUT INIT (MAX98357A) ─────────────────────────
void initI2SOutput() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // MAX98357A mono
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = true,          // Better clock accuracy for audio
    .tx_desc_auto_clear = true, // Auto-clear on underflow (silence)
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_BCLK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);

  // Set volume (DAC scaling) - optional
  i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  Serial.println("[I2S] MAX98357A output initialized");
}

// ─── AES-128 CTR DECRYPTION ──────────────────────────────
uint16_t decryptPacket(const uint8_t* packet, uint16_t packetLen, uint8_t* output) {
  if (packetLen < 16) return 0;

  // Extract IV (first 16 bytes)
  uint8_t iv[16];
  memcpy(iv, packet, 16);

  uint16_t encLen   = packetLen - 16;
  const uint8_t* enc = packet + 16;

  uint8_t keyBytes[16];
  memcpy(keyBytes, AES_KEY, 16);

  uint8_t counter[16];
  memcpy(counter, iv, 16);

  // CTR decrypt (same as encrypt - XOR with keystream)
  for (uint16_t block = 0; block < encLen / 16; block++) {
    // Encrypt counter block to produce keystream
    uint8_t counterCopy[16];
    memcpy(counterCopy, counter, 16);
    uint8_t keystream[16];
    memcpy(keystream, counter, 16);
    aesLib.encrypt(keystream, 16, output, keyBytes, 128, counterCopy);
    // Re-encrypt properly
    uint8_t ks[16];
    memcpy(ks, counter, 16);
    aesLib.encrypt(ks, 16, ks, keyBytes, 128, counter);
    memcpy(counter, counterCopy, 16);

    // XOR ciphertext with keystream
    for (int i = 0; i < 16; i++) {
      output[block * 16 + i] = enc[block * 16 + i] ^ ks[i];
    }

    // Increment counter
    for (int i = 15; i >= 12; i--) {
      if (++counter[i] != 0) break;
    }
  }

  return encLen;
}

// ─── AUDIO PLAYBACK TASK ─────────────────────────────────
void audioTask(void* param) {
  Serial.println("[Audio] Playback task started");

  while (true) {
    if (xSemaphoreTake(jitterMutex, portMAX_DELAY)) {
      if (jitterCount > 0) {
        uint16_t len = jitterLengths[jitterRead];
        uint8_t* buf = jitterBuffer[jitterRead];

        jitterRead = (jitterRead + 1) % JITTER_SLOTS;
        jitterCount--;
        xSemaphoreGive(jitterMutex);

        // Write PCM to I2S → MAX98357A
        size_t bytesWritten = 0;
        i2s_write(I2S_PORT, buf, len, &bytesWritten, pdMS_TO_TICKS(100));

      } else {
        xSemaphoreGive(jitterMutex);
        // Buffer underrun - output silence
        int16_t silence[64] = {0};
        size_t bw = 0;
        i2s_write(I2S_PORT, silence, sizeof(silence), &bw, pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }
  }
}

// ─── WEBSOCKET ───────────────────────────────────────────
void onMessage(WebsocketsMessage msg) {
  if (!msg.isBinary()) {
    Serial.printf("[WS] Text: %s\n", msg.data().c_str());
    return;
  }

  const uint8_t* data = (const uint8_t*)msg.c_str();
  uint16_t len = msg.length();

  if (len < 16) {
    Serial.println("[RX] Packet too short, skipping");
    return;
  }

  // Decrypt packet
  uint16_t audioLen = decryptPacket(data, len, decryptedBuf);
  if (audioLen == 0) return;

  // Push into jitter buffer
  if (xSemaphoreTake(jitterMutex, pdMS_TO_TICKS(5))) {
    if (jitterCount < JITTER_SLOTS) {
      memcpy(jitterBuffer[jitterWrite], decryptedBuf, audioLen);
      jitterLengths[jitterWrite] = audioLen;
      jitterWrite = (jitterWrite + 1) % JITTER_SLOTS;
      jitterCount++;
    }
    xSemaphoreGive(jitterMutex);
  }
}

void connectWebSocket() {
  wsClient.onMessage(onMessage);

  wsClient.onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      wsConnected = true;
      Serial.println("[WS] Connected");
      wsClient.send("{\"event\":\"register\",\"data\":{\"role\":\"receiver\",\"label\":\"RX-MAX98357A\"}}");
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      wsConnected = false;
      Serial.println("[WS] Disconnected - will retry");
    } else if (event == WebsocketsEvent::GotPing) {
      wsClient.pong();
    }
  });

  wsClient.connect(WS_SERVER_URL);
}

// ─── SETUP ───────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n╔═════════════════════════════════╗");
  Serial.println("║  CYBERPUNK WALKIE RX - MAX98357A ║");
  Serial.println("╚═════════════════════════════════╝");

  jitterMutex = xSemaphoreCreateMutex();

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());

  initI2SOutput();

  // Start audio playback task on Core 1
  xTaskCreatePinnedToCore(
    audioTask,
    "AudioTask",
    4096,
    NULL,
    configMAX_PRIORITIES - 1,
    NULL,
    1   // Core 1 for audio
  );

  connectWebSocket();
  Serial.println("[RX] Ready - listening for audio...");
}

// ─── LOOP ────────────────────────────────────────────────
void loop() {
  wsClient.poll();

  if (!wsConnected) {
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry > 3000) {
      lastRetry = millis();
      Serial.println("[WS] Reconnecting...");
      connectWebSocket();
    }
  }

  delay(1);
}
