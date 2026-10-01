[简体中文](README.zh_CN.md) · **English**

# VALORANT Match Board for AI Passport

A match schedule and results viewer for the FoloToy AI Passport. The online firmware connects to Wi-Fi to show recent VALORANT matches; an offline preview firmware includes sample matches for checking the screen and buttons.

## Quick start

1. Flash the [V18 online firmware](firmware/VCT-Board-v18-full.bin) to an AI Passport (ESP32-C3, 8 MB Flash) at address `0x0`.
2. On your phone, connect to the device hotspot named `VCTBoard-XXXX`. Open `http://192.168.4.1`, select a **2.4 GHz** Wi-Fi network, and enter its password.
3. Wait for the device to connect and sync the match schedule. If a sync attempt fails, the device retries automatically.

## Buttons

| Screen | Up / Down | OK |
| --- | --- | --- |
| Dates | Choose an earlier / later date | Open that day's matches |
| Matches | Choose a match; moving past the list changes the date | Open match details |
| Match details | Scroll the details | Return to matches |

On the dates or matches screen, hold **OK** to open Wi-Fi setup again.

## Offline UI preview

Flash the [V18 offline preview firmware](firmware/VCT-Board-v18-UI-preview-full.bin) at `0x0` if you only want to inspect the interface. It uses built-in sample matches and does not need Wi-Fi. Flash **one** firmware image at a time; the preview does not fetch live results.

The complete source is in this repository. To rebuild it, use ESP-IDF 5.5.3 with target `esp32c3`; see the [build guide](docs/development/engineering/build-and-test.md). Hardware and base software come from [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport).
