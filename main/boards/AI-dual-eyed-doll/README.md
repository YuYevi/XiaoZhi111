# AI-dual-eyed-doll

Custom dual-eye firmware based on XiaoZhi voice conversation. Hardware is **not** pin-compatible with Waveshare ESP32-S3-Touch-LCD-1.85, so this is a unique board identity.

## Hardware

- ESP32-S3R8, **4MB flash**, ES8390 codec (ES8389 driver), LM4890 speaker amp
- Two 0.71" GC9D01N 160x160 SPI panels (shared SCLK/MOSI/DC/RST, separate CS)
- Left eye: CS GPIO12. Right eye: CS GPIO13, hardware-mirrored so both screens share one RGB565 asset
- Official `espressif/esp_lcd_gc9d01` panel driver plus the vendor init sequence used by the working dual-eye firmware
- PA_SD on GPIO18 (KEY_2 removed; R44 DNP). GPIO35 is left to octal PSRAM
- Touch_out3 / Touch_out4 are not enabled (GPIO36/37 are PSRAM pins)

Eyes show either one of the eight static expressions under `assets/` (`*.rgb565.z`) or one of the frame-delta animations (`*.anim.z`). The original 1.85" LVGL UI, built-in fonts, and emoji collections are not compiled (`CONFIG_USE_LVGL=n`) and are not flashed (`CONFIG_FLASH_NONE_ASSETS`).

### Eye animations

The two GIFs in `assets/` are far too large to store as raw RGB565 frames on a 4MB module (269 frames of `shy` and 132 frames of `cute` at 51,200 bytes each). They are packed as byte-wise frame deltas and zlib-compressed per frame by `scripts/dual_eye_anim/pack_eye_anim.py`, which brings the pair down to ~660KB total:

| Source | Frames | Raw frames | Packed |
| --- | --- | --- | --- |
| `shy_crop_160_black.gif` | 269 (30ms) | 13.1 MB | 68 KB |
| `cute_crop_160_black.gif` | 132 (30ms) | 6.4 MB | 592 KB |

Both source GIFs draw their artwork as a 124x124 circle centred on a 160x160 black canvas, which shows up as an 18px black ring on the panels. The packer takes the union of the non-black bounds across every frame, crops to it and scales the result up to the full panel, so the eyes fill the screen. Scaling uses nearest-neighbour on purpose: smooth upscaling blurs the line art and roughly doubles the delta size (591KB vs 1055KB for `cute`). Pass `--no-trim` to keep the original framing.

A dedicated task decodes one delta per 30ms tick, accumulates it into the frame buffer, and pushes both panels over SPI. The task is pinned to core 0 at priority 4 so the ~20ms of SPI flushing per frame stays off the audio core.

## Controls

| Input | GPIO | Action |
| --- | --- | --- |
| KEY 1 | 17 | Short: cycle eye scenes (shy animation -> cute animation + `Short_laugh.ogg` -> cute animation + `tsundere.ogg`). Long: enter/exit WiFi AP config |
| Power | 2 | Hold 3s to boot, hold 3s again to power off (`PWR_CTRL` GPIO1). Short press: cycle the eight static expressions (`cute`, `happy`, `surprised`, `angry`, `sad`, `sleepy`, `shy`, `shocked`) |
| Touch 1 | 8 | Unused (was happy face + `Short_laugh.ogg`) |
| Touch 2 | 38 | Unused (was angry face + `tsundere.ogg`) |
| BOOT | 0 | Unused at runtime (download / reset) |

The touch-pad reactions and the KEY 1 AI chat toggle are commented out in `ai_dual_eyed_doll.cc`, not deleted, so they can be restored. The power button keeps its boot and power-off behaviour; only its short press was repurposed, from backlight toggle to static-expression cycling. Showing a static expression stops the running animation, so the next KEY 1 press restarts the scene.

Wake-word conversation and the original no-SSID boot provisioning path are unchanged.

ES8390 is driven with the shared ES8389 codec. Both analog mics are opened as stereo input (`AUDIO_INPUT_CHANNELS 2`).

## Build

Flash is 4MB (`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, `partitions/v2/4m.csv`): factory app `0x2F0000` (~3MB) plus a 1MB `assets` slot. This board already uses `CONFIG_FLASH_NONE_ASSETS`, so the assets partition is left empty. Dual-bank OTA does not fit on 4MB, so app rollback is disabled. Flash and octal PSRAM run at 40MHz in DIO, with WiFi IRAM options, to avoid cache MMU faults on this 4MB module.

The app slot also carries the embedded expressions and animations, so keep an eye on image size after adding more:

```sh
python3 scripts/release.py AI-dual-eyed-doll
```

Regenerate `assets/shy.anim.z` and `assets/cute.anim.z` after editing the source GIFs:

```sh
python3 -m pip install pillow
python3 scripts/dual_eye_anim/pack_eye_anim.py \
    main/boards/AI-dual-eyed-doll/assets/*.gif \
    --out-dir main/boards/AI-dual-eyed-doll/assets
```

## Notes

- ESP32-S3R8 octal PSRAM occupies GPIO33-37 and GPIO47-48. Do not use those pads as GPIO.
- TTP233H touch pads are treated as active-high.
- Physical hardware still needs to be checked after a successful build.
