#ifndef BOARD_ANIMATIONS_H
#define BOARD_ANIMATIONS_H

#include <cstddef>
#include <cstdint>

// Frame-delta eye animations, packed by scripts/dual_eye_anim/pack_eye_anim.py.
//
// They live in the read-only "assets" flash partition rather than in the app
// image. Four 160x160 clips need about 1MB, and the 3MB app partition on this
// board is full to within a few hundred bytes, so the animations are mmapped
// straight out of flash and read in place - nothing is copied into RAM.
//
// main/CMakeLists.txt builds the partition image with
// spiffs_create_partition_assets() and flashes it together with the app, so the
// partition table and the flash command need no special handling. A plain
// `idf.py flash` is enough; the erase-flash variant works too.
namespace BoardAnimations {

struct Asset {
    const void* data;  // packed *.anim.z blob, or nullptr when the lookup failed
    size_t size;       // blob size in bytes
};

// Looks up a file in the assets partition by its exact file name, for example
// "cute.anim.z". The returned pointer refers to mmapped flash and stays valid
// for the lifetime of the program, which is what DualEyeDisplay::StartAnimation
// needs. Returns {nullptr, 0} and logs an error when the partition is missing or
// the name is not present.
Asset Get(const char* name);

}  // namespace BoardAnimations

#endif  // BOARD_ANIMATIONS_H
