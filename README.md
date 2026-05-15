# Voice Magic 8-Ball (RP2350 AMOLED)

Sarcastic mystical sage in your hand. Shake the
[Waveshare RP2350-Touch-AMOLED-1.8](https://www.waveshare.com/wiki/RP2350-Touch-AMOLED-1.8),
ask a question out loud, and a hooded sage answers you through the speaker
while a parametric face on the AMOLED lip-syncs to the audio.

Responses bias on question length, loudness, and time of day. ~150 lines
pre-rendered with ElevenLabs and stored as WAVs on the microSD card.

See `../.copilot/session-state/<session-id>/plan.md` for full architecture.

## Foundation

This project **forks Waveshare's official LVGL demo** for this exact board
(found in their `RP2350-Touch-AMOLED-1.8-LVGL.zip` → `IMU_RTC` project) as the
hardware bring-up foundation. Their drivers handle the gnarly bits:

- SH8601 AMOLED via PIO-based QSPI (`lib/QSPI_PIO`, `lib/AMOLED`)
- FT3168 capacitive touch (`lib/Touch`)
- QMI8658 6-axis IMU (`lib/QMI8658`)
- PCF85063A RTC (`lib/PCF85063A`)
- LVGL 8.x graphics (`lib/lvgl`)
- Pin / GPIO config (`lib/Config`)

Our app code lives in `src/` (TODO — currently still running their tile-view
demo from `examples/`).

To-be-added drivers (from Waveshare's basic non-LVGL demo and/or written
from scratch):

- ES8311 audio codec + NS4150B amp control (speaker out, mic in)
- microSD via SPI + FatFS (WAV bank)
- AXP2101 power management (battery monitor, low-power modes)

## Toolchain

- **VS Code (Insiders or Stable)**
- **Raspberry Pi Pico VS Code extension** (`raspberry-pi.raspberry-pi-pico`)
  - Bundles the Pico SDK, ARM toolchain, CMake, Ninja, OpenOCD, picotool —
    no separate installs needed.
- **Board:** `pico2` (RP2350)

### First-time setup

1. Install the **Raspberry Pi Pico** extension from the VS Code marketplace.
   On first launch it downloads the SDK, toolchain, and helpers (~1 GB,
   one-time).
2. Open this folder in VS Code.
3. Pico extension sidebar → **Import Project** → point at this folder →
   pick the latest stable SDK version (>= 2.0.0 for RP2350 support) → confirm
   board type **pico2**.
4. The extension will configure CMake. The status bar should show the build
   target `magic_8_ball`.

### Build

- Pico extension sidebar → **Compile Project**.
- Output: `build/magic_8_ball.uf2`.

### Upload

1. Hold the **BOOT** button on the board, then plug into USB (or hold BOOT
   and tap RESET if already plugged in).
2. The board mounts as a USB drive (`RP2350`).
3. Pico extension sidebar → **Run Project (USB)**, or just drag-drop
   `build/magic_8_ball.uf2` onto the drive.

### Serial monitor

USB CDC serial is enabled (`pico_enable_stdio_usb`). After upload the board
re-enumerates as a new COM port. Open it from VS Code's serial monitor
(or any terminal) at any baud — USB CDC ignores the rate. Last known port on
this dev machine: **COM6**.

## Controls

- **Shake to ask.** Hold a vigorous shake for ~2 s. While you're shaking,
  the device plays `shake.wav`. Stop shaking before 2 s and it cancels;
  hold the full 2 s and it plays `answer.wav` followed by a random
  voiceline.
- **BOOT button cycles volume.** Tap the BOOT (BOOTSEL) button on the
  board to cycle the codec volume through `LOW → MED → HIGH → LOW …`
  (codec values 40 / 55 / 70). Each press also plays a brief 880 Hz
  preview tone at the new level. Volume defaults to MED on power-up.
  Presses are ignored while audio is playing so the preview tone can't
  trample a voiceline.

## SD card layout

```
0:/voicelines/*.wav   # 24 kHz / mono / 16-bit PCM, randomly picked answers
0:/sfx/answer.wav     # boot intro + post-shake "ahem" before each answer
0:/sfx/shake.wav      # ~2 s loop while user is holding the shake gesture
```

## Project layout

```
.
├── CMakeLists.txt              # top-level Pico SDK build (forked from Waveshare)
├── pico_sdk_import.cmake       # SDK import boilerplate
├── main.c                      # entry: calls AMOLED_1IN8_LVGL_Test() (Waveshare demo for now)
├── lib/                        # Waveshare drivers + LVGL (vendored, mostly untouched)
│   ├── AMOLED/                 # SH8601 driver
│   ├── Config/                 # DEV_Config (pin defs, GPIO helpers)
│   ├── lvgl/                   # LVGL 8.x
│   ├── PCF85063A/              # RTC driver
│   ├── QMI8658/                # IMU driver
│   ├── QSPI_PIO/               # PIO-based QSPI for SH8601
│   └── Touch/                  # FT3168 driver
├── examples/                   # Waveshare's tile-view demo (TO BE REPLACED with our app)
│   ├── inc/                    # headers + lv_conf.h
│   └── src/                    # demo source
└── README.md
```

Eventually our app code will replace `examples/` (or sit alongside as `src/`).
For Phase 0 we leave Waveshare's demo intact so we can verify the toolchain
end-to-end.

## Status

- [x] Fork Waveshare LVGL demo as foundation
- [ ] Verify unmodified demo builds + runs (current step)
- [ ] Add ES8311 audio driver + microSD driver
- [ ] Replace tile-view UI with sage face state machine
- [ ] Hook up VAD, shake detection, response selection, lip-sync
- [ ] Generate 150 ElevenLabs voice lines, deploy to SD

## Credits

Waveshare for the board and the painstaking PIO QSPI work that makes this
display usable from a Pico SDK project.
