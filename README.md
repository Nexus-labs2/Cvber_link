# CYBERLINK-7 // Encrypted Real-Time Walkie System

```
╔══════════════════════════════════════════════════════╗
║   ESP32 TX → Cloud Relay → ESP32 RX + Dashboard      ║
║   AES-128-CTR · WebSocket/Socket.IO · Node.js        ║
╚══════════════════════════════════════════════════════╝
```

## Architecture

```
INMP441 Mic
    │
    ▼
[ESP32 TX]
  I2S read → AES-128-CTR encrypt → WS binary send
    │
    ▼ WebSocket (ws://)
[Render Cloud Server]  ← Dashboard connects here too
  Socket.IO relay
    │
    ├──────────────────────────┐
    ▼                          ▼
[ESP32 RX #1]          [Browser Dashboard]
AES decrypt             AES-CTR decrypt (WebCrypto)
MAX98357A I2S out       FFT · Waveform · Radar · EQ
```

---

## MAX98357A Wiring (ESP32 RX)

```
MAX98357A     ESP32
─────────     ─────
BCLK    →     GPIO 26
LRC     →     GPIO 25
DIN     →     GPIO 22
GND     →     GND
VIN     →     3.3V or 5V
SD      →     3.3V (always ON) or GPIO for mute
GAIN    →     Float = 15dB | GND = 12dB | 3.3V = 18dB
```

Recommended: Leave GAIN floating for 15dB default.
Speaker: 4Ω or 8Ω, 3W max per MAX98357A spec.

---

## INMP441 Mic Wiring (ESP32 TX)

```
INMP441       ESP32
───────       ─────
BCLK    →     GPIO 26
WS      →     GPIO 25
SD      →     GPIO 22
GND     →     GND
VDD     →     3.3V
L/R     →     GND (left channel)
```

---

## AES-128-CTR Key

Change the key in BOTH ESP32 sketches AND the dashboard:

**ESP32 (C++ array):**
```cpp
uint8_t AES_KEY[16] = {
  0xDE, 0xAD, 0xBE, 0xEF,
  0xCA, 0xFE, 0xBA, 0xBE,
  0x13, 0x37, 0x42, 0x69,
  0xAB, 0xCD, 0xEF, 0x01
};
```

**Dashboard (hex string):**
```js
const AES_KEY_HEX = 'DEADBEEFCAFEBABE133742 69ABCDEF01';
```

---

## Deployment on Render

### Step 1 – Push to GitHub
```bash
git init
git add .
git commit -m "initial"
git remote add origin https://github.com/YOUR/repo.git
git push -u origin main
```

### Step 2 – Create Render Web Service
1. Go to https://render.com → New → Web Service
2. Connect your GitHub repo
3. Settings:
   - **Build Command:** `cd server && npm install`
   - **Start Command:** `cd server && npm start`
   - **Environment:** Node
   - **Plan:** Free (or Starter for no sleep)
4. Deploy

### Step 3 – Get your URL
Render gives you: `https://your-app-name.onrender.com`

### Step 4 – Update ESP32 code
```cpp
const char* WS_SERVER_URL = "ws://your-app-name.onrender.com";
```
For WSS (HTTPS Render): use `wss://your-app-name.onrender.com`

---

## Arduino Library Requirements

Install via Arduino Library Manager:

| Library | Author |
|---------|--------|
| ArduinoWebsockets | Gil Maimon |
| AESLib | DavyLandman |

Board: **ESP32 Dev Module**
Upload Speed: 921600
Flash Size: 4MB

---

## Audio Settings

| Parameter | Value |
|-----------|-------|
| Sample Rate | 8000 Hz |
| Bit Depth | 16-bit PCM |
| Channels | Mono |
| Buffer | 512 samples |
| Packet Size | 1040 bytes (16 IV + 1024 audio) |
| Latency | ~128ms typical |

Increase `SAMPLE_RATE` to 16000 for better quality (more bandwidth).
Reduce `AUDIO_BUFFER_SIZE` to 256 for lower latency (more packets).

---

## Project Structure

```
cyberpunk-walkie/
├── server/
│   ├── server.js          ← Node.js relay server
│   └── package.json
├── dashboard/
│   └── index.html         ← Cyberpunk dashboard UI
├── esp32-tx/
│   └── esp32_tx.ino       ← Transmitter (INMP441 mic)
├── esp32-rx/
│   └── esp32_rx.ino       ← Receiver (MAX98357A amp)
├── render.yaml            ← Render deployment config
└── README.md
```

---

## Troubleshooting

**No audio from MAX98357A:**
- Check SD pin is HIGH (tied to 3.3V or GPIO HIGH)
- Verify BCLK/LRC/DIN pins match sketch
- Try `i2s_set_clk()` with your exact sample rate
- Check speaker impedance (4Ω or 8Ω)

**WebSocket drops on Render free tier:**
- Render free tier sleeps after 15min inactivity
- Upgrade to Starter plan ($7/mo) for always-on
- ESP32 auto-reconnects after server wake

**Encryption mismatch:**
- Ensure key bytes match exactly between TX/RX/Dashboard
- AES-CTR counter must start at IV for each packet
- Check byte order (little-endian on ESP32)

**I2S noise/distortion:**
- Enable `use_apll: true` on RX for better clock
- Lower sample rate to 8000Hz
- Add 100nF cap on VDD of MAX98357A
