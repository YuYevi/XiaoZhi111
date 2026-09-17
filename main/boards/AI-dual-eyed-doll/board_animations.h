#ifndef BOARD_ANIMATIONS_H
#define BOARD_ANIMATIONS_H

#include <cstddef>
#include <cstdint>

// Frame-delta eye animations embedded from assets/*.anim.z.
// Regenerate with scripts/dual_eye_anim/pack_eye_anim.py.
namespace BoardAnimations {

extern const uint8_t shy_anim_z_start[] asm("_binary_shy_anim_z_start");
extern const uint8_t shy_anim_z_end[] asm("_binary_shy_anim_z_end");
extern const uint8_t cute_anim_z_start[] asm("_binary_cute_anim_z_start");
extern const uint8_t cute_anim_z_end[] asm("_binary_cute_anim_z_end");

struct Asset {
    const void* data;
    size_t size;
};

inline Asset Shy() {
    return {shy_anim_z_start, static_cast<size_t>(shy_anim_z_end - shy_anim_z_start)};
}

inline Asset Cute() {
    return {cute_anim_z_start, static_cast<size_t>(cute_anim_z_end - cute_anim_z_start)};
}

}  // namespace BoardAnimations

#endif  // BOARD_ANIMATIONS_H
