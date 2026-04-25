# Mintality — Plant Monitor

An ESP32-based IoT plant care monitor that tracks soil moisture, displays an emotion-based face on a TFT screen, and lets a companion app remotely trigger a watering pump via Firebase.

## What it does

- Reads soil moisture continuously and classifies the plant as **Happy**, **Middle**, or **Sad**
- Displays a matching animated face on a 160×128 color TFT screen
- Syncs moisture data and plant state to **Firebase Realtime Database** in real time
- Responds to remote watering commands from the companion mobile app
- Detects **artificial watering** (rapid moisture jump) and prompts the user to complete plant care tasks
- Scales CPU frequency (240 MHz → 80 MHz) and sleeps the display/motor driver when the app is offline

## Hardware

| Component | Part | Pin(s) |
|-----------|------|--------|
| Microcontroller | ESP32 Dev Board | — |
| Moisture sensor | Capacitive (analog) | GPIO 34 (ADC1) |
| Display | ST7735 TFT 160×128 | SPI (VSPI) |
| Motor driver | DRV8833 H-bridge | GPIO 27, 26, 13 |
| SD card | SPI (shared with display) | GPIO 15 (CS) |

> **Note:** The moisture sensor must use an ADC1 pin (GPIO 32–39). ADC2 pins are muxed with the WiFi radio and produce incorrect readings when WiFi is active.

## Firebase data

The device reads and writes to the following paths under `DB_PATH`:

| Path | Type | Direction | Description |
|------|------|-----------|-------------|
| `moistureLevel` | number | write | Raw ADC moisture reading |
| `plantState` | string | write | `"Happy"` / `"Middle"` / `"Sad"` |
| `motorState` | string | read | `"on"` triggers a 400ms pump pulse |
| `isOnline` | boolean | read | App presence — scales CPU and enables border animation |
| `artificialWaterCount` | number | write | Incremented on detected artificial watering events |

## Key implementation notes

**ADC1 only for moisture** — ADC2 shares silicon with the WiFi radio; any ADC2 read during WiFi activity returns noise. GPIO 34 (ADC1) avoids this conflict.

**SPI bus sharing** — The TFT display and SD card share the VSPI bus with separate chip-select pins. Bitmaps are streamed row-by-row from the SD card; if the card is absent the device falls back to PROGMEM-stored arrays in `faces_bitmaps.h`.

**Polling architecture** — Firebase motor and online-status fields are polled every ~2 seconds from the main loop. There is no persistent WebSocket subscription.

**Power management** — CPU runs at 80 MHz when `isOnline` is false, 240 MHz when the app is connected. The display sleeps after 5 minutes of inactivity. The DRV8833 sleep pin is asserted when the app is offline.
