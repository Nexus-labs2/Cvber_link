/*
 * ╔═══════════════════════════════════════════════════════╗
 * ║  CYBERPUNK WALKIE - ESP32 TRANSMITTER (TX NODE)       ║
 * ║  Audio Capture → AES-128 CTR Encrypt → WebSocket TX  ║
 * ╚═══════════════════════════════════════════════════════╝
 *
 * Hardware:
 *   - ESP32 (any variant)
 *   - I2S MEMS Microphone (INMP441 recommended)
 *     BCLK → GPIO 26
 *     WS   → GPIO 25
 *     SD   → GPIO 22
 *
 * Libraries Required:
 *   - ArduinoWebsockets  (by Gil Maimon)
 *   - AESLib             (by DavyLandman)
 *   - Arduino_JSON       (optional, for registration)
 *
 * Install via Arduino Library Manager.
 */

#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <driver/i2s.h>
#include <AESLib.h>
#include <esp_random.h>

using namespace websockets;

// ─── CONFIG ──────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* WS_SERVER_URL = "ws://YOUR-RENDER-APP.onrender.com";  // e.g. ws://cyberpunk-walkie.onrender.com

// AES-128 shared key (must match receiver + dashboard)
// 16 bytes exactly
uint8_t AES_KEY[16] = {
  0xDE, 0xAD, 0xBE, 0xEF,
  0xCA, 0xFE, 0xBA, 0xBE,
  0x13, 0x37, 0x42, 0x69,
  0xAB, 0xCD, 0xEF, 0x01
};

// ─── I2S CONFIG ──────────────────────────────────────────
#define I2S_PORT          I2S_NUM_0
#define I2S_BCLK_PIN      26
#define I2S_WS_PIN        25
#define I2S_DIN_PIN       22   // Data IN from mic
#define SAMPLE_RATE       8000  // 8kHz for low-latency / bandwidth
#define SAMPLE_BITS       16
#define AUDIO_BUFFER_SIZE 512   // samples per packet

// ─── GLOBALS ─────────────────────────────────────────────
WebsocketsClient wsClient;
AESLib aesLib;
bool wsConnected = false;
uint32_t packetSeq = 0;

// Buffers
int16_t  audioRaw[AUDIO_BUFFER_SIZE];
uint8_t  audioBytes[AUDIO_BUFFER_SIZE * 2];
uint8_t  encryptedBuf[AUDIO_BUFFER_SIZE * 2];
uint8_t  txPacket[16 + AUDIO_BUFFER_SIZE * 2]; // IV + encrypted

// ─── I2S INIT ────────────────────────────────────────────
void initI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // INMP441 outputs 32-bit
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_BCLK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_DIN_PIN
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  Serial.println("[I2S] Microphone initialized");
}

// ─── WIFI ────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ─── WEBSOCKET ───────────────────────────────────────────
void connectWebSocket() {
  wsClient.onMessage([](WebsocketsMessage msg) {
    // TX doesn't receive audio, but may get server status
    Serial.printf("[WS] MSG: %s\n", msg.data().c_str());
  });

  wsClient.onEvent([](WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
      wsConnected = true;
      Serial.println("[WS] Connected to server");
      // Register as transmitter
      wsClient.send("{\"event\":\"register\",\"data\":{\"role\":\"transmitter\",\"label\":\"TX-NODE-01\"}}");
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      wsConnected = false;
      Serial.println("[WS] Disconnected");
    } else if (event == WebsocketsEvent::GotPing) {
      wsClient.pong();
    }
  });

  Serial.printf("[WS] Connecting to %s...\n", WS_SERVER_URL);
  wsClient.connect(WS_SERVER_URL);
}

// ─── AES ENCRYPTION ──────────────────────────────────────
// AES-128 CTR mode: generates IV, encrypts audio, prepends IV
uint16_t encryptPacket(uint8_t* plaintext, uint16_t len, uint8_t* output) {
  // Generate 16-byte random IV using ESP32 hardware RNG
  uint8_t iv[16];
  for (int i = 0; i < 4; i++) {
    uint32_t r = esp_random();
    iv[i*4+0] = (r >> 24) & 0xFF;
    iv[i*4+1] = (r >> 16) & 0xFF;
    iv[i*4+2] = (r >> 8)  & 0xFF;
    iv[i*4+3] = (r >> 0)  & 0xFF;
  }

  // Pad to 16-byte boundary
  uint16_t paddedLen = ((len + 15) / 16) * 16;
  uint8_t padded[paddedLen];
  memcpy(padded, plaintext, len);
  memset(padded + len, 0, paddedLen - len);

  // Encrypt using AES-128 CTR
  // AESLib uses CBC by default; we simulate CTR manually
  uint8_t keyBytes[16];
  memcpy(keyBytes, AES_KEY, 16);

  uint8_t counter[16];
  memcpy(counter, iv, 16);
  uint8_t keystream[16];
  uint8_t ivCopy[16];

  for (uint16_t block = 0; block < paddedLen / 16; block++) {
    memcpy(ivCopy, counter, 16);
    aesLib.encrypt(ivCopy, 16, keystream, keyBytes, 128, counter);
    memcpy(counter, ivCopy, 16); // restore (encrypt mutates)

    // Actually encrypt counter block
    memcpy(ivCopy, counter, 16);
    uint8_t encBlock[16];
    memcpy(encBlock, counter, 16);
    aesLib.encrypt(encBlock, 16, keystream, keyBytes, 128, ivCopy);

    // XOR plaintext with keystream
    for (int i = 0; i < 16; i++) {
      output[16 + block*16 + i] = padded[block*16 + i] ^ keystream[i];
    }

    // Increment counter (last 4 bytes as big-endian uint32)
    for (int i = 15; i >= 12; i--) {
      if (++counter[i] != 0) break;
    }
  }

  // Prepend IV
  memcpy(output, iv, 16);
  return 16 + paddedLen;
}

// ─── SETUP ───────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n╔═══════════════════════════╗");
  Serial.println("║  CYBERPUNK WALKIE TX NODE  ║");
  Serial.println("╚═══════════════════════════╝");

  aesLib.gen_iv(NULL); // init
  connectWiFi();
  initI2S();
  connectWebSocket();
}

// ─── LOOP ────────────────────────────────────────────────
void loop() {
  // Maintain WebSocket connection
  wsClient.poll();

  if (!wsConnected) {
    delay(1000);
    Serial.println("[WS] Reconnecting...");
    connectWebSocket();
    return;
  }

  // Read I2S audio
  size_t bytesRead = 0;
  int32_t rawSamples[AUDIO_BUFFER_SIZE]; // 32-bit from INMP441

  esp_err_t result = i2s_read(I2S_PORT,
                               rawSamples,
                               sizeof(rawSamples),
                               &bytesRead,
                               portMAX_DELAY);

  if (result != ESP_OK || bytesRead == 0) return;

  int samplesRead = bytesRead / sizeof(int32_t);

  // Convert 32-bit I2S → 16-bit PCM (INMP441 data is in upper 18 bits)
  for (int i = 0; i < samplesRead; i++) {
    audioRaw[i] = (int16_t)(rawSamples[i] >> 14);
  }

  // Convert int16 samples → raw bytes (little-endian)
  uint16_t audioLen = samplesRead * 2;
  memcpy(audioBytes, audioRaw, audioLen);

  // Encrypt
  uint16_t txLen = encryptPacket(audioBytes, audioLen, txPacket);

  // Send binary WebSocket packet
  // Socket.IO binary: emit "audio_packet" with binary data
  // We use a simple framing: 4-byte event name length + event name + binary
  // Actually send via ArduinoWebsockets binary send:
  wsClient.sendBinary((const char*)txPacket, txLen);

  packetSeq++;
  if (packetSeq % 100 == 0) {
    Serial.printf("[TX] Packets sent: %u | Bytes: %u\n", packetSeq, txLen);
  }
}
