# Pico 2 W Bluetooth Audio Receiver

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge)
![Platform: Pico 2 W](https://img.shields.io/badge/Platform-Raspberry%20Pi%20Pico%202%20W-red.svg?style=for-the-badge)
![Language: C/C++](https://img.shields.io/badge/Language-C%2FC%2B%2B-00599C.svg?style=for-the-badge)
![Bluetooth: BTstack](https://img.shields.io/badge/Bluetooth-BTstack-0B5C92.svg?style=for-the-badge)

A Bluetooth A2DP audio receiver for the Raspberry Pi Pico 2 W. It receives Bluetooth audio and outputs 16-bit stereo PCM over I2S to a CJMCU-1334 (UDA1334A) DAC. 

Built using the BTstack library and the Pico C/C++ SDK.

## Architecture & Features

- **A2DP Sink & SBC Decoder:** Receives SBC encoded audio (up to Bitpool 53 / ~328 kbps) and decodes it to 16-bit PCM stereo.
- **Hardware I2S Output:** Uses PIO and DMA to stream decoded audio to the I2S DAC at 44.1 kHz.
- **Drift Synchronization:** Implements dynamic software resampling (`btstack_resample`) to synchronize the incoming Bluetooth clock with the RP2350 hardware I2S clock, preventing buffer under/overflows.
- **Audio Buffering:** Allocates 92ms of I2S DMA buffering to mitigate CPU contention between the CYW43439 Wi-Fi/BT combo chip and audio processing.
- **AVRCP Volume Control:** Processes AVRCP absolute volume commands and applies a logarithmic (quadratic) scaling function to the PCM data in software.
- **UI Sound Synthesizer:** Contains a blocking square-wave synthesizer that injects status tones directly into the I2S hardware pool on boot, connection, and disconnection events.

## UI Sound Configuration

The status tones are generated mathematically via `bt_audio.c`. Configuration is managed via preprocessor macros:

- `ENABLE_UI_SOUNDS` (default: `1`): Set to `0` to disable the synthesizer.
- `UI_SOUND_VOLUME` (default: `50`): Sets the 16-bit PCM amplitude of the square wave (max 32767). 

## Hardware Requirements

| Component | Role |
|---|---|
| **Raspberry Pi Pico 2 W** | Main controller (RP2350 + CYW43439) |
| **CJMCU-1334** (UDA1334A) | I2S DAC |

## Wiring Guide

| Pico 2 W Pin | Physical Pin | CJMCU-1334 | Signal |
|---|---|---|---|
| **VBUS** | 40 | **VIN** | 5V USB Power |
| **GND** | 38 | **GND** | Common Ground |
| **GPIO 16** | 21 | **BCLK** | I2S Bit Clock |
| **GPIO 17** | 22 | **WSEL** | I2S Word Select (L/R) |
| **GPIO 18** | 24 | **DIN** | I2S Serial Data |

> [!NOTE]
> VBUS (5V) is used instead of 3V3. The Pico's switching 3V3 regulator introduces noise; feeding 5V to the DAC's onboard LDO provides cleaner power for the analog output.

### DAC Configuration Pins (CJMCU-1334)
Most breakout boards pull these low by default:
- **SF0 / SF1:** LOW (I2S format)
- **PLL:** LOW (Audio PLL mode)
- **DEEM:** LOW (De-emphasis OFF)
- **MUTE:** LOW (Mute OFF)

## Build Instructions

1. Install and configure the Raspberry Pi Pico C/C++ SDK for the RP2350.
2. Clone the repository.
3. Build the project:
```bash
mkdir -p build && cd build
cmake -DPICO_BOARD=pico2_w ..
ninja
```

## Flashing

1. Hold the **BOOTSEL** button on the Pico 2 W while plugging in the USB cable.
2. A mass storage device named `RP2350` will mount.
3. Copy the compiled `.uf2` binary to the device:
```bash
cp build/bluetooth_audio_receiver.uf2 /run/media/$USER/RP2350/
```
4. The Pico will automatically reboot. The Bluetooth device name is **Pico2W-Audio**.

## LED Status Indicator

| LED State | Meaning |
|---|---|
| **1 Hz Blink** | Discoverable / Waiting for pairing |
| **4 Hz Blink** | Connected, no active audio stream |
| **Solid ON** | Connected, audio streaming |
| **OFF** | Initialization error |

## Troubleshooting

| Symptom | Cause | Resolution |
|---|---|---|
| No LED blink on boot | CYW43 init failure | Verify USB power, reflash `.uf2` |
| Volume buttons unresponsive | AVRCP sync failure | Re-pair the device; check UART logs for `[avrcp]` |
| Audio distortion | Floating DAC config pins | Bridge SF0 and SF1 to GND on the DAC board |

## License
Licensed under the MIT License. See `LICENSE` for details.
