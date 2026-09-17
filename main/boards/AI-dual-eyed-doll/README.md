# AI-dual-eyed-doll

Custom dual-eye firmware based on XiaoZhi voice conversation. Hardware is **not** pin-compatible with Waveshare ESP32-S3-Touch-LCD-1.85, so this is a unique board identity.

## Hardware

- ESP32-S3R8, **4MB flash**, ES8390 codec (ES8389 driver), LM4890 speaker amp
- Two 0.71" GC9D01N 160x160 SPI panels (shared SCLK/MOSI/DC/RST, separate CS)
- Left eye: CS GPIO12. Right eye: CS GPIO13, hardware-mirrored so both screens share one RGB565 asset
- Official `espressif/esp_lcd_gc9d01` panel driver plus the vendor init sequence used by the working dual-eye firmware
- PA_SD on GPIO18 (KEY_2 removed; R44 DNP). GPIO35 is left to octal PSRAM
- Touch_out3 / Touch_out4 are not enabled (GPIO36/37 are PSRAM pins)

Eyes show either one of the eight static expressions embedded from `assets/` (`*.rgb565.z`) or one of the frame-delta animations (`*.anim.z`) kept in the read-only `assets` flash partition. The original 1.85" LVGL UI, built-in fonts, and emoji collections are not compiled (`CONFIG_USE_LVGL=n`) and are not flashed (`CONFIG_FLASH_NONE_ASSETS`).

### Eye animations

Six 160x160 clips are packed by `scripts/dual_eye_anim/pack_eye_anim.py` as byte-wise frame deltas, zlib-compressed per frame, and stored in the 1MB `assets` partition. They cannot live in the app image: the 3MB app partition was already full to within a few hundred bytes. Three `*_blnk` clips (packed from `Pictures/图片/blink_gif/08-shocked_blink.gif`, `01-cute_blink.gif`, `02-happy_blink.gif`) belong to the KEY 1 cycle; the `cute`/`happy`/`sad` crop clips belong to the power-button cycle. `scared` was dropped when the blink set arrived.

Asset file names must stay at 15 characters or fewer (`CONFIG_MMAP_FILE_NAME_LENGTH` is 16, longer names get truncated in the table and the exact-name lookup fails), hence `shckblnk`/`cuteblnk`/`hapyblnk` instead of `*_blink`.

| Animation | Kept frames | Frame delay | Loop | Packed |
| --- | --- | --- | --- | --- |
| `shckblnk.anim.z` | 164 | 40 ms | 6.56 s | 100 KB |
| `cuteblnk.anim.z` | 151 | 40 ms | 6.04 s | 101 KB |
| `hapyblnk.anim.z` | 157 | 40 ms | 6.28 s | 97 KB |
| `cute.anim.z` | 44 of 132 | 90 ms | 3.96 s | 262 KB |
| `happy.anim.z` | 23 of 68 | 90 ms | 2.07 s | 75 KB |
| `sad.anim.z` | 48 of 144 | 90 ms | 4.32 s | 165 KB |
| **total** | | | | **801 KB** |

The blink source GIFs use mixed per-frame durations - roughly half a second of blinking followed by a 2.5-2.9s hold, about 6s per cycle - but the packed format carries a single delay for the whole clip. Packing them one GIF frame per packed frame would have to pick one delay and collapses the 6s blink cycle into under a second, so they are packed with `--resample-ms 40`, which spreads the GIF timeline onto a uniform 40ms grid. The long hold becomes many copies of the same frame, and a repeated frame is an all-zero delta that zlib collapses to a few bytes, which is why 164 frames cost 100 KB and not 1.6 MB. The crop clips have uniform 30ms frames and keep the plain `--frame-step 3` packing, which is the only thing that makes them fit: at the native 30ms they alone total 2363 KB.

Two mistakes are easy to make here and both are expensive:

- **Scale with nearest-neighbour, never a smoothing filter.** The artwork is semi-realistic (soft iris gradients, highlights, lashes); LANCZOS or BICUBIC turns flat edges into gradients whose noise differs every frame, which destroys the frame-to-frame delta. Feeding the packer the LANCZOS-upscaled files from `Pictures/动图/optimized_160` makes the four clips total 3895 KB instead of 2363 KB. Pack the untrimmed originals and let the packer crop.
- **Keep the artwork's black canvas out of the panel.** The source GIFs draw a 124x124 circle on a 160x160 black canvas, which shows as an 18px black ring. The packer takes the union of the non-black bounds across every frame and scales that up to fill the panel. Pass `--no-trim` to keep the original framing.

At runtime `board_animations.cc` mmaps the partition once, looks assets up by file name, and `DualEyeDisplay` reads frames straight out of flash. A dedicated task decodes one delta per clip's own frame delay (40ms for the blink clips, 90ms for the crop clips), accumulates it into the frame buffer, and pushes both panels over SPI. The task is pinned to core 0 at priority 4 so the ~20ms of SPI flushing per frame stays off the audio core.

## Controls

| Input | GPIO | Action |
| --- | --- | --- |
| KEY 1 | 17 | Short: cycle eye scenes `shocked_blink` -> `cute_blink` -> `happy_blink`, each restarting with its baobao-flavoured voice clip. Long: mute/unmute the scene voice clips (on = confirmation `tsundere.ogg`) |
| Power | 2 | Hold 3s to boot, hold 3s again to power off (`PWR_CTRL` GPIO1). Short press: cycle scenes `cute` -> `happy` -> `sad` (crop animations), each restarting with its lanmeng-flavoured voice clip |
| Touch 1 | 8 | Unused |
| Touch 2 | 38 | Unused |
| BOOT | 0 | Unused at runtime (download / reset) |

Every scene press restarts the animation from its first frame and restarts the scene's voice clip, so a press is never swallowed and picture and sound start together. The animation keeps looping; it is not stopped when the clip ends. The voice clips are generic young-female TTS lines written in the two characters' phrasing and tone - they are not cloned voice-actor voices. With scene audio muted, presses still switch animations silently; unmuting plays `tsundere.ogg` once as confirmation. The KEY 1 long press no longer toggles the WiFi config AP; first-boot provisioning without a saved SSID is unaffected.

`AudioService::PlaySound()` pushes the whole clip into a decode queue only 2400ms deep and blocks its caller whenever the queue is full. `tsundere.ogg` runs 5.3s, so playing it from the main task would freeze button handling for about three seconds. A small `eye_sound` task owns the playback instead and is triggered by a task notification, which also coalesces a burst of presses into a single restart.

The touch-pad reactions and the KEY 1 AI chat toggle were switched off when KEY 1 was reworked into the animation cycle. The power button keeps its boot and power-off behaviour; only its short press was repurposed, from backlight toggle to static-expression cycling.

Wake-word conversation and the original no-SSID boot provisioning path are unchanged.

ES8390 is driven with the shared ES8389 codec. Both analog mics are opened as stereo input (`AUDIO_INPUT_CHANNELS 2`).

## Build

Flash is 4MB (`CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, `partitions/v2/4m.csv`): factory app `0x2F0000` (~3MB) plus the 1MB `assets` slot. Dual-bank OTA does not fit on 4MB, so app rollback is disabled. Flash and octal PSRAM run at 40MHz in DIO, with WiFi IRAM options, to avoid cache MMU faults on this 4MB module.

`main/CMakeLists.txt` calls `spiffs_create_partition_assets()` for this board, which packs `partition_assets/` into a partition image at build time and registers it with the flash arguments. `idf.py flash` and `idf.py merge-bin` therefore both carry it, so `scripts/release.py` needs no changes. The component installs Pillow, NumPy and qoi-conv into the ESP-IDF Python environment on the first build.

The eight static expressions (~74KB) stay embedded in the app image, which now has roughly 660KB of headroom where it used to have 496 bytes:

```sh
python3 scripts/release.py AI-dual-eyed-doll
```

Regenerate the animations after changing a source GIF, then check the image still parses and fits:

```sh
python3 -m pip install pillow
# the blink GIFs have mixed frame durations: resample their timeline onto a
# uniform 40ms grid, otherwise the 6s blink cycle collapses to under a second
python3 scripts/dual_eye_anim/pack_eye_anim.py \
    "C:/Users/YuYevi/Pictures/图片/blink_gif/08-shocked_blink.gif" \
    "C:/Users/YuYevi/Pictures/图片/blink_gif/01-cute_blink.gif" \
    "C:/Users/YuYevi/Pictures/图片/blink_gif/02-happy_blink.gif" \
    --out-dir main/boards/AI-dual-eyed-doll/partition_assets \
    --resample-ms 40

# the crop clips have uniform 30ms frames and are packed every third frame
python3 scripts/dual_eye_anim/pack_eye_anim.py \
    "C:/Users/YuYevi/Pictures/动图/cute_crop_160_black.gif" \
    "C:/Users/YuYevi/Pictures/动图/happy_crop_160_black.gif" \
    "C:/Users/YuYevi/Pictures/动图/sad_crop_160_black.gif" \
    --out-dir main/boards/AI-dual-eyed-doll/partition_assets \
    --frame-step 3

# the blink asset names must fit the 15-char assets-table name limit
cd main/boards/AI-dual-eyed-doll/partition_assets
mv 08-shocked_blink.anim.z shckblnk.anim.z
mv 01-cute_blink.anim.z cuteblnk.anim.z
mv 02-happy_blink.anim.z hapyblnk.anim.z
cd ../../../../

# rebuild the image and parse it back with the same rules the firmware uses
python3 scripts/dual_eye_anim/verify_partition_image.py
```

Dropping `--frame-step` gives the crop clips the full 30ms cadence at 2363 KB, which does not fit the partition; lower the step to raise the frame rate and re-run the verifier to see where the budget lands.

## Notes

- ESP32-S3R8 octal PSRAM occupies GPIO33-37 and GPIO47-48. Do not use those pads as GPIO.
- TTP233H touch pads are treated as active-high.
- `partition_assets/` must contain only the generated `*.anim.z` files: the image generator packs every file it finds in that directory.
- The assets table entry stride comes from `CONFIG_MMAP_FILE_NAME_LENGTH` (16 in this project), so each entry is 28 bytes. `main/assets.cc` hardcodes a 32-byte name field and would misparse this image; `board_animations.cc` derives the stride from the Kconfig value instead.
- Physical hardware still needs to be checked after a successful build.
