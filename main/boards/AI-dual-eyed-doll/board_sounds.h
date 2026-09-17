#ifndef BOARD_SOUNDS_H
#define BOARD_SOUNDS_H

#include <string_view>

// Embedded OGG (Opus) clips.
//
// - One clip per KEY 1 scene: feng-baobao-flavoured lines (lazy, deadpan).
// - One clip per power-button scene: lanmeng-flavoured lines (lively loli).
// - tsundere.ogg is no longer bound to a scene; it plays as the confirmation
//   beep when scene audio is switched back on with the KEY 1 long press.
//
// NOTE: these are generic young-female TTS voices shaped by phrasing and tone,
// not cloned voice-actor timbres.
namespace BoardSounds {

extern const char tsundere_ogg_start[] asm("_binary_tsundere_ogg_start");
extern const char tsundere_ogg_end[] asm("_binary_tsundere_ogg_end");
static const std::string_view OGG_TSUNDERE{
    static_cast<const char*>(tsundere_ogg_start),
    static_cast<size_t>(tsundere_ogg_end - tsundere_ogg_start)};

extern const char baoer_shocked_ogg_start[] asm("_binary_baoer_shocked_ogg_start");
extern const char baoer_shocked_ogg_end[] asm("_binary_baoer_shocked_ogg_end");
static const std::string_view OGG_BAOER_SHOCKED{
    static_cast<const char*>(baoer_shocked_ogg_start),
    static_cast<size_t>(baoer_shocked_ogg_end - baoer_shocked_ogg_start)};

extern const char baoer_cute_ogg_start[] asm("_binary_baoer_cute_ogg_start");
extern const char baoer_cute_ogg_end[] asm("_binary_baoer_cute_ogg_end");
static const std::string_view OGG_BAOER_CUTE{
    static_cast<const char*>(baoer_cute_ogg_start),
    static_cast<size_t>(baoer_cute_ogg_end - baoer_cute_ogg_start)};

extern const char baoer_happy_ogg_start[] asm("_binary_baoer_happy_ogg_start");
extern const char baoer_happy_ogg_end[] asm("_binary_baoer_happy_ogg_end");
static const std::string_view OGG_BAOER_HAPPY{
    static_cast<const char*>(baoer_happy_ogg_start),
    static_cast<size_t>(baoer_happy_ogg_end - baoer_happy_ogg_start)};

extern const char lanmeng_cute_ogg_start[] asm("_binary_lanmeng_cute_ogg_start");
extern const char lanmeng_cute_ogg_end[] asm("_binary_lanmeng_cute_ogg_end");
static const std::string_view OGG_LANMENG_CUTE{
    static_cast<const char*>(lanmeng_cute_ogg_start),
    static_cast<size_t>(lanmeng_cute_ogg_end - lanmeng_cute_ogg_start)};

extern const char lanmeng_happy_ogg_start[] asm("_binary_lanmeng_happy_ogg_start");
extern const char lanmeng_happy_ogg_end[] asm("_binary_lanmeng_happy_ogg_end");
static const std::string_view OGG_LANMENG_HAPPY{
    static_cast<const char*>(lanmeng_happy_ogg_start),
    static_cast<size_t>(lanmeng_happy_ogg_end - lanmeng_happy_ogg_start)};

extern const char lanmeng_sad_ogg_start[] asm("_binary_lanmeng_sad_ogg_start");
extern const char lanmeng_sad_ogg_end[] asm("_binary_lanmeng_sad_ogg_end");
static const std::string_view OGG_LANMENG_SAD{
    static_cast<const char*>(lanmeng_sad_ogg_start),
    static_cast<size_t>(lanmeng_sad_ogg_end - lanmeng_sad_ogg_start)};

}  // namespace BoardSounds

#endif  // BOARD_SOUNDS_H
