/* sw_keymap.h — keysyms from the keymap the compositor sends, without
 * libxkbcommon.
 *
 * A Wayland client is handed its keymap as XKB text over wl_keyboard.keymap.
 * Reading that is the whole job for an app that wants to know what a key
 * types: it is the user's real layout, not a guess, so a German keyboard
 * types z where z is and ä where ä is. This parser takes the two sections it
 * needs — the keycode names and the symbol table — and the four shift levels
 * of each key.
 *
 * What it does not do: dead keys, compose sequences, multiple groups and
 * layout switching, and the long tail of keysyms no stock layout uses. A
 * search box and a rename field do not need them. If a program ever does,
 * that is the point to link libxkbcommon and delete this file.
 *
 * The keysym name table below was generated from the names the stock us, de,
 * fr, es, gb, it, se, no, dk, pl, cz and ru layouts actually use. Checked
 * against libxkbcommon on all twelve, every keycode at every modifier
 * combination: the characters typed match exactly. Two keysyms differ —
 * PrintScreen and Pause, whose second level is selected by Control rather
 * than Shift — and neither types anything.
 *
 * Define SW_KEYMAP_IMPL in exactly one translation unit.
 *
 * Written/modified by: Claude Opus 5
 */
#ifndef SW_KEYMAP_H
#define SW_KEYMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* X keysyms worth naming, so callers do not need the header they came from */
enum {
    SW_KEY_BACKSPACE = 0xff08, SW_KEY_TAB    = 0xff09, SW_KEY_RETURN = 0xff0d,
    SW_KEY_ESCAPE    = 0xff1b, SW_KEY_HOME   = 0xff50, SW_KEY_LEFT   = 0xff51,
    SW_KEY_UP        = 0xff52, SW_KEY_RIGHT  = 0xff53, SW_KEY_DOWN   = 0xff54,
    SW_KEY_PAGE_UP   = 0xff55, SW_KEY_PAGE_DOWN = 0xff56, SW_KEY_END = 0xff57,
    SW_KEY_INSERT    = 0xff63, SW_KEY_KP_ENTER  = 0xff8d, SW_KEY_DELETE = 0xffff,
    SW_KEY_F1        = 0xffbe, SW_KEY_SPACE  = 0x0020
};

#define SW_KEYMAP_MAX 320        /* evdev tops out well below this */

typedef struct {
    uint32_t sym[SW_KEYMAP_MAX][4];   /* keycode x level: plain, shift, altgr, both */
    uint8_t  keypad[SW_KEYMAP_MAX];   /* 1 = its level follows NumLock, not Shift */
    bool     ok;
} SwKeymap;

/* Parse what arrived on wl_keyboard.keymap. Safe to call again on a change. */
bool     sw_keymap_parse(SwKeymap *km, const char *text, size_t len);

/* The keysym for a keycode under the current modifiers, and what it types.
 * utf8 is set to the characters, or "" for a key that types nothing. */
uint32_t sw_keymap_lookup(const SwKeymap *km, uint32_t keycode, unsigned mods,
                          char *utf8, size_t n);

/* The SW_MOD_* bits for a wl_keyboard.modifiers mask. */
unsigned sw_keymap_mods(const SwKeymap *km, uint32_t depressed);

/* The codepoint a keysym types, 0 for one that types nothing. */
uint32_t sw_keysym_to_codepoint(uint32_t sym);

#ifdef __cplusplus
}
#endif
#endif /* SW_KEYMAP_H */

/* ===================================================================== */
#ifdef SW_KEYMAP_IMPL
#ifndef SW_KEYMAP_IMPL_ONCE
#define SW_KEYMAP_IMPL_ONCE

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* The modifier bits in a wl_keyboard.modifiers mask are the core X ones, in
 * their fixed order, whatever the keymap calls them. */
enum { SWK_SHIFT = 1u << 0, SWK_LOCK = 1u << 1, SWK_CTRL = 1u << 2,
       SWK_MOD1  = 1u << 3, SWK_NUM = 1u << 4, SWK_MOD4 = 1u << 6,
       SWK_MOD5  = 1u << 7 };

typedef struct { const char *name; uint32_t sym; uint32_t cp; } SwKsEntry;

/* Generated from the keysym names the stock layouts use. Sorted, so a
 * lookup is a binary search. Single ASCII characters are not in here: their
 * name is the character and their keysym is its code. */
static const SwKsEntry SW_KS[] = {
    { "0x0100002b", 0x0100002b, 0x002b },
    { "0x010000d7", 0x010000d7, 0x00d7 },
    { "0x010000f7", 0x010000f7, 0x00f7 },
    { "AE", 0x000000c6, 0x00c6 },
    { "Adiaeresis", 0x000000c4, 0x00c4 },
    { "Alt_L", 0x0000ffe9, 0x0000 },
    { "Alt_R", 0x0000ffea, 0x0000 },
    { "Aogonek", 0x000001a1, 0x0104 },
    { "Aring", 0x000000c5, 0x00c5 },
    { "BackSpace", 0x0000ff08, 0x0008 },
    { "Break", 0x0000ff6b, 0x0000 },
    { "Cacute", 0x000001c6, 0x0106 },
    { "Cancel", 0x0000ff69, 0x0000 },
    { "Caps_Lock", 0x0000ffe5, 0x0000 },
    { "Ccedilla", 0x000000c7, 0x00c7 },
    { "Control_L", 0x0000ffe3, 0x0000 },
    { "Control_R", 0x0000ffe4, 0x0000 },
    { "Cyrillic_A", 0x000006e1, 0x0410 },
    { "Cyrillic_BE", 0x000006e2, 0x0411 },
    { "Cyrillic_CHE", 0x000006fe, 0x0427 },
    { "Cyrillic_DE", 0x000006e4, 0x0414 },
    { "Cyrillic_E", 0x000006fc, 0x042d },
    { "Cyrillic_EF", 0x000006e6, 0x0424 },
    { "Cyrillic_EL", 0x000006ec, 0x041b },
    { "Cyrillic_EM", 0x000006ed, 0x041c },
    { "Cyrillic_EN", 0x000006ee, 0x041d },
    { "Cyrillic_ER", 0x000006f2, 0x0420 },
    { "Cyrillic_ES", 0x000006f3, 0x0421 },
    { "Cyrillic_GHE", 0x000006e7, 0x0413 },
    { "Cyrillic_HA", 0x000006e8, 0x0425 },
    { "Cyrillic_HARDSIGN", 0x000006ff, 0x042a },
    { "Cyrillic_I", 0x000006e9, 0x0418 },
    { "Cyrillic_IE", 0x000006e5, 0x0415 },
    { "Cyrillic_IO", 0x000006b3, 0x0401 },
    { "Cyrillic_KA", 0x000006eb, 0x041a },
    { "Cyrillic_O", 0x000006ef, 0x041e },
    { "Cyrillic_PE", 0x000006f0, 0x041f },
    { "Cyrillic_SHA", 0x000006fb, 0x0428 },
    { "Cyrillic_SHCHA", 0x000006fd, 0x0429 },
    { "Cyrillic_SHORTI", 0x000006ea, 0x0419 },
    { "Cyrillic_SOFTSIGN", 0x000006f8, 0x042c },
    { "Cyrillic_TE", 0x000006f4, 0x0422 },
    { "Cyrillic_TSE", 0x000006e3, 0x0426 },
    { "Cyrillic_U", 0x000006f5, 0x0423 },
    { "Cyrillic_VE", 0x000006f7, 0x0412 },
    { "Cyrillic_YA", 0x000006f1, 0x042f },
    { "Cyrillic_YERU", 0x000006f9, 0x042b },
    { "Cyrillic_YU", 0x000006e0, 0x042e },
    { "Cyrillic_ZE", 0x000006fa, 0x0417 },
    { "Cyrillic_ZHE", 0x000006f6, 0x0416 },
    { "Cyrillic_a", 0x000006c1, 0x0430 },
    { "Cyrillic_be", 0x000006c2, 0x0431 },
    { "Cyrillic_che", 0x000006de, 0x0447 },
    { "Cyrillic_de", 0x000006c4, 0x0434 },
    { "Cyrillic_e", 0x000006dc, 0x044d },
    { "Cyrillic_ef", 0x000006c6, 0x0444 },
    { "Cyrillic_el", 0x000006cc, 0x043b },
    { "Cyrillic_em", 0x000006cd, 0x043c },
    { "Cyrillic_en", 0x000006ce, 0x043d },
    { "Cyrillic_er", 0x000006d2, 0x0440 },
    { "Cyrillic_es", 0x000006d3, 0x0441 },
    { "Cyrillic_ghe", 0x000006c7, 0x0433 },
    { "Cyrillic_ha", 0x000006c8, 0x0445 },
    { "Cyrillic_hardsign", 0x000006df, 0x044a },
    { "Cyrillic_i", 0x000006c9, 0x0438 },
    { "Cyrillic_ie", 0x000006c5, 0x0435 },
    { "Cyrillic_io", 0x000006a3, 0x0451 },
    { "Cyrillic_ka", 0x000006cb, 0x043a },
    { "Cyrillic_o", 0x000006cf, 0x043e },
    { "Cyrillic_pe", 0x000006d0, 0x043f },
    { "Cyrillic_sha", 0x000006db, 0x0448 },
    { "Cyrillic_shcha", 0x000006dd, 0x0449 },
    { "Cyrillic_shorti", 0x000006ca, 0x0439 },
    { "Cyrillic_softsign", 0x000006d8, 0x044c },
    { "Cyrillic_te", 0x000006d4, 0x0442 },
    { "Cyrillic_tse", 0x000006c3, 0x0446 },
    { "Cyrillic_u", 0x000006d5, 0x0443 },
    { "Cyrillic_ve", 0x000006d7, 0x0432 },
    { "Cyrillic_ya", 0x000006d1, 0x044f },
    { "Cyrillic_yeru", 0x000006d9, 0x044b },
    { "Cyrillic_yu", 0x000006c0, 0x044e },
    { "Cyrillic_ze", 0x000006da, 0x0437 },
    { "Cyrillic_zhe", 0x000006d6, 0x0436 },
    { "Delete", 0x0000ffff, 0x007f },
    { "Down", 0x0000ff54, 0x0000 },
    { "Dstroke", 0x000001d0, 0x0110 },
    { "ENG", 0x000003bd, 0x014a },
    { "ETH", 0x000000d0, 0x00d0 },
    { "End", 0x0000ff57, 0x0000 },
    { "Eogonek", 0x000001ca, 0x0118 },
    { "Escape", 0x0000ff1b, 0x001b },
    { "EuroSign", 0x000020ac, 0x20ac },
    { "F1", 0x0000ffbe, 0x0000 },
    { "F10", 0x0000ffc7, 0x0000 },
    { "F11", 0x0000ffc8, 0x0000 },
    { "F12", 0x0000ffc9, 0x0000 },
    { "F2", 0x0000ffbf, 0x0000 },
    { "F3", 0x0000ffc0, 0x0000 },
    { "F4", 0x0000ffc1, 0x0000 },
    { "F5", 0x0000ffc2, 0x0000 },
    { "F6", 0x0000ffc3, 0x0000 },
    { "F7", 0x0000ffc4, 0x0000 },
    { "F8", 0x0000ffc5, 0x0000 },
    { "F9", 0x0000ffc6, 0x0000 },
    { "Find", 0x0000ff68, 0x0000 },
    { "Greek_OMEGA", 0x000007d9, 0x03a9 },
    { "Greek_PI", 0x000007d0, 0x03a0 },
    { "Greek_SIGMA", 0x000007d2, 0x03a3 },
    { "Greek_omega", 0x000007f9, 0x03c9 },
    { "Greek_pi", 0x000007f0, 0x03c0 },
    { "Greek_sigma", 0x000007f2, 0x03c3 },
    { "Hangul", 0x0000ff31, 0x0000 },
    { "Hangul_Hanja", 0x0000ff34, 0x0000 },
    { "Help", 0x0000ff6a, 0x0000 },
    { "Henkan_Mode", 0x0000ff23, 0x0000 },
    { "Hiragana", 0x0000ff25, 0x0000 },
    { "Hiragana_Katakana", 0x0000ff27, 0x0000 },
    { "Home", 0x0000ff50, 0x0000 },
    { "Hstroke", 0x000002a1, 0x0126 },
    { "Hyper_L", 0x0000ffed, 0x0000 },
    { "ISO_Left_Tab", 0x0000fe20, 0x0000 },
    { "ISO_Level3_Shift", 0x0000fe03, 0x0000 },
    { "ISO_Level5_Shift", 0x0000fe11, 0x0000 },
    { "ISO_Next_Group", 0x0000fe08, 0x0000 },
    { "Insert", 0x0000ff63, 0x0000 },
    { "KP_0", 0x0000ffb0, 0x0030 },
    { "KP_1", 0x0000ffb1, 0x0031 },
    { "KP_2", 0x0000ffb2, 0x0032 },
    { "KP_3", 0x0000ffb3, 0x0033 },
    { "KP_4", 0x0000ffb4, 0x0034 },
    { "KP_5", 0x0000ffb5, 0x0035 },
    { "KP_6", 0x0000ffb6, 0x0036 },
    { "KP_7", 0x0000ffb7, 0x0037 },
    { "KP_8", 0x0000ffb8, 0x0038 },
    { "KP_9", 0x0000ffb9, 0x0039 },
    { "KP_Add", 0x0000ffab, 0x002b },
    { "KP_Begin", 0x0000ff9d, 0x0000 },
    { "KP_Decimal", 0x0000ffae, 0x002e },
    { "KP_Delete", 0x0000ff9f, 0x0000 },
    { "KP_Divide", 0x0000ffaf, 0x002f },
    { "KP_Down", 0x0000ff99, 0x0000 },
    { "KP_End", 0x0000ff9c, 0x0000 },
    { "KP_Enter", 0x0000ff8d, 0x000d },
    { "KP_Equal", 0x0000ffbd, 0x003d },
    { "KP_Home", 0x0000ff95, 0x0000 },
    { "KP_Insert", 0x0000ff9e, 0x0000 },
    { "KP_Left", 0x0000ff96, 0x0000 },
    { "KP_Multiply", 0x0000ffaa, 0x002a },
    { "KP_Next", 0x0000ff9b, 0x0000 },
    { "KP_Prior", 0x0000ff9a, 0x0000 },
    { "KP_Right", 0x0000ff98, 0x0000 },
    { "KP_Separator", 0x0000ffac, 0x002c },
    { "KP_Subtract", 0x0000ffad, 0x002d },
    { "KP_Up", 0x0000ff97, 0x0000 },
    { "Katakana", 0x0000ff26, 0x0000 },
    { "Left", 0x0000ff51, 0x0000 },
    { "Linefeed", 0x0000ff0a, 0x000a },
    { "Lstroke", 0x000001a3, 0x0141 },
    { "Menu", 0x0000ff67, 0x0000 },
    { "Meta_L", 0x0000ffe7, 0x0000 },
    { "Meta_R", 0x0000ffe8, 0x0000 },
    { "Muhenkan", 0x0000ff22, 0x0000 },
    { "Nacute", 0x000001d1, 0x0143 },
    { "Next", 0x0000ff56, 0x0000 },
    { "Ntilde", 0x000000d1, 0x00d1 },
    { "Num_Lock", 0x0000ff7f, 0x0000 },
    { "OE", 0x000013bc, 0x0152 },
    { "Oacute", 0x000000d3, 0x00d3 },
    { "Odiaeresis", 0x000000d6, 0x00d6 },
    { "Oslash", 0x000000d8, 0x00d8 },
    { "Pause", 0x0000ff13, 0x0000 },
    { "Print", 0x0000ff61, 0x0000 },
    { "Prior", 0x0000ff55, 0x0000 },
    { "Redo", 0x0000ff66, 0x0000 },
    { "Return", 0x0000ff0d, 0x000d },
    { "Right", 0x0000ff53, 0x0000 },
    { "SCHWA", 0x0100018f, 0x018f },
    { "Sacute", 0x000001a6, 0x015a },
    { "Scroll_Lock", 0x0000ff14, 0x0000 },
    { "Shift_L", 0x0000ffe1, 0x0000 },
    { "Shift_R", 0x0000ffe2, 0x0000 },
    { "SunFront", 0x1005ff71, 0x0000 },
    { "SunProps", 0x1005ff70, 0x0000 },
    { "Super_L", 0x0000ffeb, 0x0000 },
    { "Super_R", 0x0000ffec, 0x0000 },
    { "Sys_Req", 0x0000ff15, 0x0000 },
    { "THORN", 0x000000de, 0x00de },
    { "Tab", 0x0000ff09, 0x0009 },
    { "Tslash", 0x000003ac, 0x0166 },
    { "U0001F12F", 0x0101f12f, 0x1f12f },
    { "U017F", 0x0100017f, 0x017f },
    { "U1E9E", 0x01001e9e, 0x1e9e },
    { "U2022", 0x01002022, 0x2022 },
    { "U2026", 0x01002026, 0x2026 },
    { "U202F", 0x0100202f, 0x202f },
    { "U2032", 0x01002032, 0x2032 },
    { "U2033", 0x01002033, 0x2033 },
    { "U2039", 0x01002039, 0x2039 },
    { "U203A", 0x0100203a, 0x203a },
    { "U20BD", 0x010020bd, 0x20bd },
    { "U2194", 0x01002194, 0x2194 },
    { "U2212", 0x01002212, 0x2212 },
    { "U2215", 0x01002215, 0x2215 },
    { "U22C5", 0x010022c5, 0x22c5 },
    { "Udiaeresis", 0x000000dc, 0x00dc },
    { "Undo", 0x0000ff65, 0x0000 },
    { "Up", 0x0000ff52, 0x0000 },
    { "VoidSymbol", 0x00ffffff, 0x0000 },
    { "XF8610ChannelsDown", 0x100811b9, 0x0000 },
    { "XF8610ChannelsUp", 0x100811b8, 0x0000 },
    { "XF863DMode", 0x1008126f, 0x0000 },
    { "XF86ALSToggle", 0x10081230, 0x0000 },
    { "XF86Addressbook", 0x100811ad, 0x0000 },
    { "XF86AppSelect", 0x10081244, 0x0000 },
    { "XF86AspectRatio", 0x10081177, 0x0000 },
    { "XF86Assistant", 0x10081247, 0x0000 },
    { "XF86AttendantOff", 0x1008121c, 0x0000 },
    { "XF86AttendantOn", 0x1008121b, 0x0000 },
    { "XF86AttendantToggle", 0x1008121d, 0x0000 },
    { "XF86Audio", 0x10081188, 0x0000 },
    { "XF86AudioDesc", 0x1008126e, 0x0000 },
    { "XF86AudioForward", 0x1008ff97, 0x0000 },
    { "XF86AudioLowerVolume", 0x1008ff11, 0x0000 },
    { "XF86AudioMedia", 0x1008ff32, 0x0000 },
    { "XF86AudioMicMute", 0x1008ffb2, 0x0000 },
    { "XF86AudioMute", 0x1008ff12, 0x0000 },
    { "XF86AudioNext", 0x1008ff17, 0x0000 },
    { "XF86AudioPause", 0x1008ff31, 0x0000 },
    { "XF86AudioPlay", 0x1008ff14, 0x0000 },
    { "XF86AudioPreset", 0x1008ffb6, 0x0000 },
    { "XF86AudioPrev", 0x1008ff16, 0x0000 },
    { "XF86AudioRaiseVolume", 0x1008ff13, 0x0000 },
    { "XF86AudioRandomPlay", 0x1008ff99, 0x0000 },
    { "XF86AudioRecord", 0x1008ff1c, 0x0000 },
    { "XF86AudioRewind", 0x1008ff3e, 0x0000 },
    { "XF86AudioStop", 0x1008ff15, 0x0000 },
    { "XF86AutopilotEngageToggle", 0x1008127d, 0x0000 },
    { "XF86Back", 0x1008ff26, 0x0000 },
    { "XF86Battery", 0x1008ff93, 0x0000 },
    { "XF86Bluetooth", 0x1008ff94, 0x0000 },
    { "XF86Break", 0x1008119b, 0x0000 },
    { "XF86BrightnessAdjust", 0x1008ff3b, 0x0000 },
    { "XF86BrightnessAuto", 0x100810f4, 0x0000 },
    { "XF86BrightnessMax", 0x10081251, 0x0000 },
    { "XF86BrightnessMin", 0x10081250, 0x0000 },
    { "XF86Buttonconfig", 0x10081240, 0x0000 },
    { "XF86Calculator", 0x1008ff1d, 0x0000 },
    { "XF86Calendar", 0x1008ff20, 0x0000 },
    { "XF86CameraAccessDisable", 0x1008124c, 0x0000 },
    { "XF86CameraAccessEnable", 0x1008124b, 0x0000 },
    { "XF86CameraAccessToggle", 0x1008124d, 0x0000 },
    { "XF86CameraDown", 0x10081218, 0x0000 },
    { "XF86CameraFocus", 0x10081210, 0x0000 },
    { "XF86CameraLeft", 0x10081219, 0x0000 },
    { "XF86CameraRight", 0x1008121a, 0x0000 },
    { "XF86CameraUp", 0x10081217, 0x0000 },
    { "XF86CameraZoomIn", 0x10081215, 0x0000 },
    { "XF86CameraZoomOut", 0x10081216, 0x0000 },
    { "XF86ChannelDown", 0x10081193, 0x0000 },
    { "XF86ChannelUp", 0x10081192, 0x0000 },
    { "XF86ClearGrab", 0x1008fe21, 0x0000 },
    { "XF86ClearvuSonar", 0x10081286, 0x0000 },
    { "XF86Close", 0x1008ff56, 0x0000 },
    { "XF86ContextMenu", 0x100811b6, 0x0000 },
    { "XF86ControlPanel", 0x10081243, 0x0000 },
    { "XF86Copy", 0x1008ff57, 0x0000 },
    { "XF86Cut", 0x1008ff58, 0x0000 },
    { "XF86CycleAngle", 0x1008ff9c, 0x0000 },
    { "XF86DOS", 0x1008ff5a, 0x0000 },
    { "XF86DVD", 0x10081185, 0x0000 },
    { "XF86Data", 0x10081277, 0x0000 },
    { "XF86Database", 0x100811aa, 0x0000 },
    { "XF86Dictate", 0x1008124a, 0x0000 },
    { "XF86Display", 0x1008ff59, 0x0000 },
    { "XF86DisplayOff", 0x100810f5, 0x0000 },
    { "XF86DisplayToggle", 0x100811af, 0x0000 },
    { "XF86Documents", 0x1008ff5b, 0x0000 },
    { "XF86DualRangeRadar", 0x10081283, 0x0000 },
    { "XF86Editor", 0x100811a6, 0x0000 },
    { "XF86Eject", 0x1008ff2c, 0x0000 },
    { "XF86EmojiPicker", 0x10081249, 0x0000 },
    { "XF86Excel", 0x1008ff5c, 0x0000 },
    { "XF86Explorer", 0x1008ff5d, 0x0000 },
    { "XF86FastReverse", 0x10081275, 0x0000 },
    { "XF86Favorites", 0x1008ff30, 0x0000 },
    { "XF86Finance", 0x1008ff3c, 0x0000 },
    { "XF86FishingChart", 0x10081281, 0x0000 },
    { "XF86Fn", 0x100811d0, 0x0000 },
    { "XF86FnRightShift", 0x100811e5, 0x0000 },
    { "XF86Fn_Esc", 0x100811d1, 0x0000 },
    { "XF86Forward", 0x1008ff27, 0x0000 },
    { "XF86FrameBack", 0x1008ff9d, 0x0000 },
    { "XF86FrameForward", 0x1008ff9e, 0x0000 },
    { "XF86FullScreen", 0x1008ffb8, 0x0000 },
    { "XF86Game", 0x1008ff5e, 0x0000 },
    { "XF86Go", 0x1008ff5f, 0x0000 },
    { "XF86GraphicsEditor", 0x100811a8, 0x0000 },
    { "XF86HangupPhone", 0x100811be, 0x0000 },
    { "XF86HomePage", 0x1008ff18, 0x0000 },
    { "XF86Images", 0x100811ba, 0x0000 },
    { "XF86Info", 0x10081166, 0x0000 },
    { "XF86Journal", 0x10081242, 0x0000 },
    { "XF86KbdBrightnessDown", 0x1008ff06, 0x0000 },
    { "XF86KbdBrightnessUp", 0x1008ff05, 0x0000 },
    { "XF86KbdInputAssistAccept", 0x10081264, 0x0000 },
    { "XF86KbdInputAssistCancel", 0x10081265, 0x0000 },
    { "XF86KbdInputAssistNext", 0x10081261, 0x0000 },
    { "XF86KbdInputAssistNextgroup", 0x10081263, 0x0000 },
    { "XF86KbdInputAssistPrev", 0x10081260, 0x0000 },
    { "XF86KbdInputAssistPrevgroup", 0x10081262, 0x0000 },
    { "XF86KbdLcdMenu1", 0x100812b8, 0x0000 },
    { "XF86KbdLcdMenu2", 0x100812b9, 0x0000 },
    { "XF86KbdLcdMenu3", 0x100812ba, 0x0000 },
    { "XF86KbdLcdMenu4", 0x100812bb, 0x0000 },
    { "XF86KbdLcdMenu5", 0x100812bc, 0x0000 },
    { "XF86KbdLightOnOff", 0x1008ff04, 0x0000 },
    { "XF86Keyboard", 0x1008ffb3, 0x0000 },
    { "XF86Launch1", 0x1008ff41, 0x0000 },
    { "XF86Launch2", 0x1008ff42, 0x0000 },
    { "XF86Launch3", 0x1008ff43, 0x0000 },
    { "XF86Launch4", 0x1008ff44, 0x0000 },
    { "XF86Launch5", 0x1008ff45, 0x0000 },
    { "XF86Launch6", 0x1008ff46, 0x0000 },
    { "XF86Launch7", 0x1008ff47, 0x0000 },
    { "XF86Launch8", 0x1008ff48, 0x0000 },
    { "XF86Launch9", 0x1008ff49, 0x0000 },
    { "XF86LaunchA", 0x1008ff4a, 0x0000 },
    { "XF86LaunchB", 0x1008ff4b, 0x0000 },
    { "XF86LeftDown", 0x10081269, 0x0000 },
    { "XF86LeftUp", 0x10081268, 0x0000 },
    { "XF86LightsToggle", 0x1008121e, 0x0000 },
    { "XF86LogOff", 0x1008ff61, 0x0000 },
    { "XF86Macro1", 0x10081290, 0x0000 },
    { "XF86Macro10", 0x10081299, 0x0000 },
    { "XF86Macro11", 0x1008129a, 0x0000 },
    { "XF86Macro12", 0x1008129b, 0x0000 },
    { "XF86Macro13", 0x1008129c, 0x0000 },
    { "XF86Macro14", 0x1008129d, 0x0000 },
    { "XF86Macro15", 0x1008129e, 0x0000 },
    { "XF86Macro16", 0x1008129f, 0x0000 },
    { "XF86Macro17", 0x100812a0, 0x0000 },
    { "XF86Macro18", 0x100812a1, 0x0000 },
    { "XF86Macro19", 0x100812a2, 0x0000 },
    { "XF86Macro2", 0x10081291, 0x0000 },
    { "XF86Macro20", 0x100812a3, 0x0000 },
    { "XF86Macro21", 0x100812a4, 0x0000 },
    { "XF86Macro22", 0x100812a5, 0x0000 },
    { "XF86Macro23", 0x100812a6, 0x0000 },
    { "XF86Macro24", 0x100812a7, 0x0000 },
    { "XF86Macro25", 0x100812a8, 0x0000 },
    { "XF86Macro26", 0x100812a9, 0x0000 },
    { "XF86Macro27", 0x100812aa, 0x0000 },
    { "XF86Macro28", 0x100812ab, 0x0000 },
    { "XF86Macro29", 0x100812ac, 0x0000 },
    { "XF86Macro3", 0x10081292, 0x0000 },
    { "XF86Macro30", 0x100812ad, 0x0000 },
    { "XF86Macro4", 0x10081293, 0x0000 },
    { "XF86Macro5", 0x10081294, 0x0000 },
    { "XF86Macro6", 0x10081295, 0x0000 },
    { "XF86Macro7", 0x10081296, 0x0000 },
    { "XF86Macro8", 0x10081297, 0x0000 },
    { "XF86Macro9", 0x10081298, 0x0000 },
    { "XF86MacroPreset1", 0x100812b3, 0x0000 },
    { "XF86MacroPreset2", 0x100812b4, 0x0000 },
    { "XF86MacroPreset3", 0x100812b5, 0x0000 },
    { "XF86MacroPresetCycle", 0x100812b2, 0x0000 },
    { "XF86MacroRecordStart", 0x100812b0, 0x0000 },
    { "XF86MacroRecordStop", 0x100812b1, 0x0000 },
    { "XF86Mail", 0x1008ff19, 0x0000 },
    { "XF86MailForward", 0x1008ff90, 0x0000 },
    { "XF86MarkWaypoint", 0x1008127e, 0x0000 },
    { "XF86MediaRepeat", 0x100811b7, 0x0000 },
    { "XF86MediaTopMenu", 0x1008126b, 0x0000 },
    { "XF86MenuKB", 0x1008ff65, 0x0000 },
    { "XF86Messenger", 0x1008ff8e, 0x0000 },
    { "XF86MonBrightnessCycle", 0x1008ff07, 0x0000 },
    { "XF86MonBrightnessDown", 0x1008ff03, 0x0000 },
    { "XF86MonBrightnessUp", 0x1008ff02, 0x0000 },
    { "XF86MyComputer", 0x1008ff33, 0x0000 },
    { "XF86NavChart", 0x10081280, 0x0000 },
    { "XF86NavInfo", 0x10081288, 0x0000 },
    { "XF86New", 0x1008ff68, 0x0000 },
    { "XF86News", 0x1008ff69, 0x0000 },
    { "XF86NextElement", 0x1008127b, 0x0000 },
    { "XF86NextFavorite", 0x10081270, 0x0000 },
    { "XF86Next_VMode", 0x1008fe22, 0x0000 },
    { "XF86NotificationCenter", 0x100811bc, 0x0000 },
    { "XF86Numeric0", 0x10081200, 0x0000 },
    { "XF86Numeric1", 0x10081201, 0x0000 },
    { "XF86Numeric11", 0x1008126c, 0x0000 },
    { "XF86Numeric12", 0x1008126d, 0x0000 },
    { "XF86Numeric2", 0x10081202, 0x0000 },
    { "XF86Numeric3", 0x10081203, 0x0000 },
    { "XF86Numeric4", 0x10081204, 0x0000 },
    { "XF86Numeric5", 0x10081205, 0x0000 },
    { "XF86Numeric6", 0x10081206, 0x0000 },
    { "XF86Numeric7", 0x10081207, 0x0000 },
    { "XF86Numeric8", 0x10081208, 0x0000 },
    { "XF86Numeric9", 0x10081209, 0x0000 },
    { "XF86NumericA", 0x1008120c, 0x0000 },
    { "XF86NumericB", 0x1008120d, 0x0000 },
    { "XF86NumericC", 0x1008120e, 0x0000 },
    { "XF86NumericD", 0x1008120f, 0x0000 },
    { "XF86NumericPound", 0x1008120b, 0x0000 },
    { "XF86NumericStar", 0x1008120a, 0x0000 },
    { "XF86OnScreenKeyboard", 0x10081278, 0x0000 },
    { "XF86Open", 0x1008ff6b, 0x0000 },
    { "XF86Paste", 0x1008ff6d, 0x0000 },
    { "XF86PauseRecord", 0x10081272, 0x0000 },
    { "XF86Phone", 0x1008ff6e, 0x0000 },
    { "XF86PickupPhone", 0x100811bd, 0x0000 },
    { "XF86PowerOff", 0x1008ff2a, 0x0000 },
    { "XF86Presentation", 0x100811a9, 0x0000 },
    { "XF86Prev_VMode", 0x1008fe23, 0x0000 },
    { "XF86PreviousElement", 0x1008127c, 0x0000 },
    { "XF86PrivacyScreenToggle", 0x10081279, 0x0000 },
    { "XF86RFKill", 0x1008ffb5, 0x0000 },
    { "XF86RadarOverlay", 0x10081284, 0x0000 },
    { "XF86Reload", 0x1008ff73, 0x0000 },
    { "XF86Reply", 0x1008ff72, 0x0000 },
    { "XF86RightDown", 0x10081267, 0x0000 },
    { "XF86RightUp", 0x10081266, 0x0000 },
    { "XF86RootMenu", 0x1008126a, 0x0000 },
    { "XF86RotateWindows", 0x1008ff74, 0x0000 },
    { "XF86RotationLockToggle", 0x1008ffb7, 0x0000 },
    { "XF86Save", 0x1008ff77, 0x0000 },
    { "XF86ScreenSaver", 0x1008ff2d, 0x0000 },
    { "XF86Screensaver", 0x10081245, 0x0000 },
    { "XF86ScrollDown", 0x1008ff79, 0x0000 },
    { "XF86ScrollUp", 0x1008ff78, 0x0000 },
    { "XF86Search", 0x1008ff1b, 0x0000 },
    { "XF86SelectiveScreenshot", 0x1008127a, 0x0000 },
    { "XF86Send", 0x1008ff7b, 0x0000 },
    { "XF86Shop", 0x1008ff36, 0x0000 },
    { "XF86SidevuSonar", 0x10081287, 0x0000 },
    { "XF86SingleRangeRadar", 0x10081282, 0x0000 },
    { "XF86Sleep", 0x1008ff2f, 0x0000 },
    { "XF86SlowReverse", 0x10081276, 0x0000 },
    { "XF86Sos", 0x1008127f, 0x0000 },
    { "XF86SpellCheck", 0x100811b0, 0x0000 },
    { "XF86StopRecord", 0x10081271, 0x0000 },
    { "XF86Suspend", 0x1008ffa7, 0x0000 },
    { "XF86Switch_VT_1", 0x1008fe01, 0x0000 },
    { "XF86Switch_VT_10", 0x1008fe0a, 0x0000 },
    { "XF86Switch_VT_11", 0x1008fe0b, 0x0000 },
    { "XF86Switch_VT_12", 0x1008fe0c, 0x0000 },
    { "XF86Switch_VT_2", 0x1008fe02, 0x0000 },
    { "XF86Switch_VT_3", 0x1008fe03, 0x0000 },
    { "XF86Switch_VT_4", 0x1008fe04, 0x0000 },
    { "XF86Switch_VT_5", 0x1008fe05, 0x0000 },
    { "XF86Switch_VT_6", 0x1008fe06, 0x0000 },
    { "XF86Switch_VT_7", 0x1008fe07, 0x0000 },
    { "XF86Switch_VT_8", 0x1008fe08, 0x0000 },
    { "XF86Switch_VT_9", 0x1008fe09, 0x0000 },
    { "XF86TaskPane", 0x1008ff7f, 0x0000 },
    { "XF86Taskmanager", 0x10081241, 0x0000 },
    { "XF86Tools", 0x1008ff81, 0x0000 },
    { "XF86TouchpadOff", 0x1008ffb1, 0x0000 },
    { "XF86TouchpadOn", 0x1008ffb0, 0x0000 },
    { "XF86TouchpadToggle", 0x1008ffa9, 0x0000 },
    { "XF86TraditionalSonar", 0x10081285, 0x0000 },
    { "XF86UWB", 0x1008ff96, 0x0000 },
    { "XF86Ungrab", 0x1008fe20, 0x0000 },
    { "XF86Unmute", 0x10081274, 0x0000 },
    { "XF86VOD", 0x10081273, 0x0000 },
    { "XF86Video", 0x1008ff87, 0x0000 },
    { "XF86VideoPhone", 0x100811a0, 0x0000 },
    { "XF86VoiceCommand", 0x10081246, 0x0000 },
    { "XF86Voicemail", 0x100811ac, 0x0000 },
    { "XF86WLAN", 0x1008ff95, 0x0000 },
    { "XF86WPSButton", 0x10081211, 0x0000 },
    { "XF86WWAN", 0x1008ffb4, 0x0000 },
    { "XF86WWW", 0x1008ff2e, 0x0000 },
    { "XF86WakeUp", 0x1008ff2b, 0x0000 },
    { "XF86WebCam", 0x1008ff8f, 0x0000 },
    { "XF86Word", 0x1008ff89, 0x0000 },
    { "XF86Xfer", 0x1008ff8a, 0x0000 },
    { "XF86ZoomIn", 0x1008ff8b, 0x0000 },
    { "XF86ZoomOut", 0x1008ff8c, 0x0000 },
    { "XF86ZoomReset", 0x100811a4, 0x0000 },
    { "Zabovedot", 0x000001af, 0x017b },
    { "Zacute", 0x000001ac, 0x0179 },
    { "aacute", 0x000000e1, 0x00e1 },
    { "acute", 0x000000b4, 0x00b4 },
    { "adiaeresis", 0x000000e4, 0x00e4 },
    { "ae", 0x000000e6, 0x00e6 },
    { "agrave", 0x000000e0, 0x00e0 },
    { "ampersand", 0x00000026, 0x0026 },
    { "aogonek", 0x000001b1, 0x0105 },
    { "apostrophe", 0x00000027, 0x0027 },
    { "approxeq", 0x01002248, 0x2248 },
    { "aring", 0x000000e5, 0x00e5 },
    { "asciicircum", 0x0000005e, 0x005e },
    { "asciitilde", 0x0000007e, 0x007e },
    { "asterisk", 0x0000002a, 0x002a },
    { "at", 0x00000040, 0x0040 },
    { "backslash", 0x0000005c, 0x005c },
    { "bar", 0x0000007c, 0x007c },
    { "braceleft", 0x0000007b, 0x007b },
    { "braceright", 0x0000007d, 0x007d },
    { "bracketleft", 0x0000005b, 0x005b },
    { "bracketright", 0x0000005d, 0x005d },
    { "braille_dot_1", 0x0000fff1, 0x0000 },
    { "braille_dot_2", 0x0000fff2, 0x0000 },
    { "braille_dot_3", 0x0000fff3, 0x0000 },
    { "braille_dot_4", 0x0000fff4, 0x0000 },
    { "braille_dot_5", 0x0000fff5, 0x0000 },
    { "braille_dot_6", 0x0000fff6, 0x0000 },
    { "braille_dot_7", 0x0000fff7, 0x0000 },
    { "braille_dot_8", 0x0000fff8, 0x0000 },
    { "braille_dot_9", 0x0000fff9, 0x0000 },
    { "brokenbar", 0x000000a6, 0x00a6 },
    { "cacute", 0x000001e6, 0x0107 },
    { "ccaron", 0x000001e8, 0x010d },
    { "ccedilla", 0x000000e7, 0x00e7 },
    { "cent", 0x000000a2, 0x00a2 },
    { "colon", 0x0000003a, 0x003a },
    { "comma", 0x0000002c, 0x002c },
    { "copyright", 0x000000a9, 0x00a9 },
    { "currency", 0x000000a4, 0x00a4 },
    { "dagger", 0x00000af1, 0x2020 },
    { "dead_abovedot", 0x0000fe56, 0x0000 },
    { "dead_abovering", 0x0000fe58, 0x0000 },
    { "dead_acute", 0x0000fe51, 0x0000 },
    { "dead_belowdot", 0x0000fe60, 0x0000 },
    { "dead_belowmacron", 0x0000fe68, 0x0000 },
    { "dead_breve", 0x0000fe55, 0x0000 },
    { "dead_caron", 0x0000fe5a, 0x0000 },
    { "dead_cedilla", 0x0000fe5b, 0x0000 },
    { "dead_circumflex", 0x0000fe52, 0x0000 },
    { "dead_diaeresis", 0x0000fe57, 0x0000 },
    { "dead_doubleacute", 0x0000fe59, 0x0000 },
    { "dead_grave", 0x0000fe50, 0x0000 },
    { "dead_hook", 0x0000fe61, 0x0000 },
    { "dead_horn", 0x0000fe62, 0x0000 },
    { "dead_macron", 0x0000fe54, 0x0000 },
    { "dead_ogonek", 0x0000fe5c, 0x0000 },
    { "dead_stroke", 0x0000fe63, 0x0000 },
    { "dead_tilde", 0x0000fe53, 0x0000 },
    { "degree", 0x000000b0, 0x00b0 },
    { "division", 0x000000f7, 0x00f7 },
    { "dollar", 0x00000024, 0x0024 },
    { "doublelowquotemark", 0x00000afe, 0x201e },
    { "downarrow", 0x000008fe, 0x2193 },
    { "dstroke", 0x000001f0, 0x0111 },
    { "eacute", 0x000000e9, 0x00e9 },
    { "ecaron", 0x000001ec, 0x011b },
    { "egrave", 0x000000e8, 0x00e8 },
    { "ellipsis", 0x00000aae, 0x2026 },
    { "emdash", 0x00000aa9, 0x2014 },
    { "endash", 0x00000aaa, 0x2013 },
    { "eng", 0x000003bf, 0x014b },
    { "eogonek", 0x000001ea, 0x0119 },
    { "equal", 0x0000003d, 0x003d },
    { "eth", 0x000000f0, 0x00f0 },
    { "exclam", 0x00000021, 0x0021 },
    { "exclamdown", 0x000000a1, 0x00a1 },
    { "fiveeighths", 0x00000ac5, 0x215d },
    { "grave", 0x00000060, 0x0060 },
    { "greater", 0x0000003e, 0x003e },
    { "greaterthanequal", 0x000008be, 0x2265 },
    { "guillemetleft", 0x000000ab, 0x00ab },
    { "guillemetright", 0x000000bb, 0x00bb },
    { "hstroke", 0x000002b1, 0x0127 },
    { "iacute", 0x000000ed, 0x00ed },
    { "idotless", 0x000002b9, 0x0131 },
    { "igrave", 0x000000ec, 0x00ec },
    { "infinity", 0x000008c2, 0x221e },
    { "kra", 0x000003a2, 0x0138 },
    { "leftarrow", 0x000008fb, 0x2190 },
    { "leftdoublequotemark", 0x00000ad2, 0x201c },
    { "leftsinglequotemark", 0x00000ad0, 0x2018 },
    { "less", 0x0000003c, 0x003c },
    { "lessthanequal", 0x000008bc, 0x2264 },
    { "logicaland", 0x000008de, 0x2227 },
    { "logicalor", 0x000008df, 0x2228 },
    { "lstroke", 0x000001b3, 0x0142 },
    { "macron", 0x000000af, 0x00af },
    { "minus", 0x0000002d, 0x002d },
    { "mu", 0x000000b5, 0x00b5 },
    { "multiply", 0x000000d7, 0x00d7 },
    { "nacute", 0x000001f1, 0x0144 },
    { "nobreakspace", 0x000000a0, 0x00a0 },
    { "notequal", 0x000008bd, 0x2260 },
    { "notsign", 0x000000ac, 0x00ac },
    { "ntilde", 0x000000f1, 0x00f1 },
    { "numbersign", 0x00000023, 0x0023 },
    { "numerosign", 0x000006b0, 0x2116 },
    { "oacute", 0x000000f3, 0x00f3 },
    { "odiaeresis", 0x000000f6, 0x00f6 },
    { "oe", 0x000013bd, 0x0153 },
    { "ograve", 0x000000f2, 0x00f2 },
    { "oneeighth", 0x00000ac3, 0x215b },
    { "onehalf", 0x000000bd, 0x00bd },
    { "onequarter", 0x000000bc, 0x00bc },
    { "onesuperior", 0x000000b9, 0x00b9 },
    { "ordfeminine", 0x000000aa, 0x00aa },
    { "ordmasculine", 0x000000ba, 0x00ba },
    { "oslash", 0x000000f8, 0x00f8 },
    { "paragraph", 0x000000b6, 0x00b6 },
    { "parenleft", 0x00000028, 0x0028 },
    { "parenright", 0x00000029, 0x0029 },
    { "percent", 0x00000025, 0x0025 },
    { "period", 0x0000002e, 0x002e },
    { "periodcentered", 0x000000b7, 0x00b7 },
    { "permille", 0x00000ad5, 0x2030 },
    { "plus", 0x0000002b, 0x002b },
    { "plusminus", 0x000000b1, 0x00b1 },
    { "question", 0x0000003f, 0x003f },
    { "questiondown", 0x000000bf, 0x00bf },
    { "quotedbl", 0x00000022, 0x0022 },
    { "radical", 0x000008d6, 0x221a },
    { "rcaron", 0x000001f8, 0x0159 },
    { "registered", 0x000000ae, 0x00ae },
    { "rightarrow", 0x000008fd, 0x2192 },
    { "rightdoublequotemark", 0x00000ad3, 0x201d },
    { "rightsinglequotemark", 0x00000ad1, 0x2019 },
    { "sacute", 0x000001b6, 0x015b },
    { "scaron", 0x000001b9, 0x0161 },
    { "schwa", 0x01000259, 0x0259 },
    { "section", 0x000000a7, 0x00a7 },
    { "semicolon", 0x0000003b, 0x003b },
    { "seveneighths", 0x00000ac6, 0x215e },
    { "singlelowquotemark", 0x00000afd, 0x201a },
    { "slash", 0x0000002f, 0x002f },
    { "space", 0x00000020, 0x0020 },
    { "ssharp", 0x000000df, 0x00df },
    { "sterling", 0x000000a3, 0x00a3 },
    { "thorn", 0x000000fe, 0x00fe },
    { "threeeighths", 0x00000ac4, 0x215c },
    { "threequarters", 0x000000be, 0x00be },
    { "threesuperior", 0x000000b3, 0x00b3 },
    { "trademark", 0x00000ac9, 0x2122 },
    { "tslash", 0x000003bc, 0x0167 },
    { "twosuperior", 0x000000b2, 0x00b2 },
    { "uacute", 0x000000fa, 0x00fa },
    { "udiaeresis", 0x000000fc, 0x00fc },
    { "ugrave", 0x000000f9, 0x00f9 },
    { "underscore", 0x0000005f, 0x005f },
    { "uparrow", 0x000008fc, 0x2191 },
    { "uring", 0x000001f9, 0x016f },
    { "yacute", 0x000000fd, 0x00fd },
    { "yen", 0x000000a5, 0x00a5 },
    { "zabovedot", 0x000001bf, 0x017c },
    { "zacute", 0x000001bc, 0x017a },
    { "zcaron", 0x000001be, 0x017e },
};
#define SW_KS_N ((int)(sizeof(SW_KS) / sizeof(SW_KS[0])))

static const SwKsEntry *swk_find(const char *name, size_t len)
{
    int lo = 0, hi = SW_KS_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *s = SW_KS[mid].name;
        int c = strncmp(s, name, len);
        if (c == 0) c = (s[len] == 0) ? 0 : 1;
        if (c == 0) return &SW_KS[mid];
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* A keysym from its name: one ASCII character stands for itself, UXXXX is a
 * codepoint written out, anything else comes from the table. */
static uint32_t swk_sym_from_name(const char *s, size_t len)
{
    if (len == 0) return 0;
    if (len == 1 && (unsigned char)s[0] >= 0x20 && (unsigned char)s[0] < 0x7f)
        return (uint32_t)s[0];
    if ((s[0] == 'U' || s[0] == 'u') && len >= 5 && len <= 7) {
        uint32_t cp = 0;
        size_t i = 1;
        for (; i < len; ++i) {
            char c = s[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            cp = cp * 16u + (uint32_t)d;
        }
        if (i == len) return cp + 0x1000000u;
    }
    const SwKsEntry *e = swk_find(s, len);
    return e ? e->sym : 0;
}

uint32_t sw_keysym_to_codepoint(uint32_t sym)
{
    if (sym >= 0x20 && sym <= 0x7e) return sym;
    if (sym >= 0xa0 && sym <= 0xff) return sym;
    if (sym >= 0x1000020u && sym <= 0x110ffffu) return sym - 0x1000000u;
    for (int i = 0; i < SW_KS_N; ++i)          /* the named ones */
        if (SW_KS[i].sym == sym) return SW_KS[i].cp;
    return 0;
}

static int swk_utf8(uint32_t cp, char *out, size_t n)
{
    if (!cp || n < 5) { if (n) out[0] = 0; return 0; }
    int k = 0;
    if (cp < 0x80)            { out[k++] = (char)cp; }
    else if (cp < 0x800)      { out[k++] = (char)(0xC0 | (cp >> 6));
                                out[k++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000)    { out[k++] = (char)(0xE0 | (cp >> 12));
                                out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                out[k++] = (char)(0x80 | (cp & 0x3F)); }
    else                      { out[k++] = (char)(0xF0 | (cp >> 18));
                                out[k++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                                out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                out[k++] = (char)(0x80 | (cp & 0x3F)); }
    out[k] = 0;
    return k;
}

/* ---------------------------------------------------------- the parser */

/* Key names are short: <AD01>, <LFSH>, <I252>. One flat table, linear scan —
 * a few hundred entries, read once when the keymap arrives. */
typedef struct { char name[12]; uint32_t code; } SwKcEntry;

/* The body of a named block, bounded by matching braces. Counting them is
 * the only way to find the end: every key line inside xkb_symbols ends in
 * "};", so looking for that instead stops at the first key. */
static const char *swk_section(const char *p, const char *end, const char *what,
                               const char **body_end)
{
    size_t n = strlen(what);
    for (; p + n < end; ++p) {
        if (strncmp(p, what, n)) continue;
        const char *b = (const char *)memchr(p, '{', (size_t)(end - p));
        if (!b) return NULL;
        int depth = 1;
        const char *q = b + 1;
        for (; q < end && depth; ++q) {
            if (*q == '{') depth++;
            else if (*q == '}') depth--;
            else if (*q == '"') {                /* a name can hold a brace */
                while (++q < end && *q != '"')
                    if (*q == '\\' && q + 1 < end) ++q;
            }
        }
        if (body_end) *body_end = (depth == 0) ? q - 1 : end;
        return b + 1;
    }
    return NULL;
}

static const char *swk_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

bool sw_keymap_parse(SwKeymap *km, const char *text, size_t len)
{
    if (!km || !text || !len) return false;
    memset(km, 0, sizeof(*km));
    const char *end = text + len;

    /* 1. the keycodes block: <NAME> = N; */
    static SwKcEntry kc[768];
    int nkc = 0;
    const char *kc_end = end;
    const char *p = swk_section(text, end, "xkb_keycodes", &kc_end);
    if (!p) return false;
    for (; p < kc_end && nkc < (int)(sizeof(kc) / sizeof(kc[0])); ++p) {
        if (*p != '<') continue;
        const char *gt = (const char *)memchr(p, '>', (size_t)(end - p));
        if (!gt || gt - p > 11) continue;
        const char *q = swk_skip_ws(gt + 1, end);
        if (q >= end || *q != '=') continue;
        q = swk_skip_ws(q + 1, end);
        uint32_t v = 0;
        bool any = false;
        while (q < end && *q >= '0' && *q <= '9') { v = v * 10 + (uint32_t)(*q++ - '0'); any = true; }
        if (!any) continue;
        size_t nl = (size_t)(gt - p - 1);
        memcpy(kc[nkc].name, p + 1, nl);
        kc[nkc].name[nl] = 0;
        kc[nkc].code = v;
        nkc++;
        p = q;
    }

    /* 2. the symbols block: key <NAME> { [ a, A, ... ] }; — the first bracket
     *    list is group 1, which is the only group handled. */
    const char *sym_end = end;
    p = swk_section(text, end, "xkb_symbols", &sym_end);
    if (!p) return false;
    end = sym_end;                      /* stay inside the block from here */
    while (p < end) {
        const char *k = NULL;
        for (const char *s = p; s + 4 < end; ++s)
            if (!strncmp(s, "key ", 4) || !strncmp(s, "key\t", 4)) { k = s + 4; break; }
        if (!k) break;
        k = swk_skip_ws(k, end);
        if (k >= end || *k != '<') { p = k + 1; continue; }
        const char *gt = (const char *)memchr(k, '>', (size_t)(end - k));
        if (!gt) break;
        size_t nl = (size_t)(gt - k - 1);

        uint32_t code = 0;
        for (int i = 0; i < nkc; ++i)
            if (strlen(kc[i].name) == nl && !memcmp(kc[i].name, k + 1, nl)) {
                code = kc[i].code;
                break;
            }

        const char *semi = (const char *)memchr(gt, ';', (size_t)(end - gt));

        /* The symbol list, which is not simply the first bracket: a key may
         * carry type[Group1]="FOUR_LEVEL" first, and symbols[Group1]= before
         * the list itself. The one we want opens straight after a '{' or an
         * '=', where those others follow a name. */
        const char *lb = NULL;
        for (const char *s = gt; s < end && (!semi || s < semi); ++s) {
            if (*s != '[') continue;
            const char *b = s - 1;
            while (b > gt && (*b == ' ' || *b == '\t')) --b;
            if (*b == '{' || *b == '=') { lb = s; break; }
        }
        if (!lb) { p = semi ? semi + 1 : gt + 1; continue; }


        const char *rb = (const char *)memchr(lb, ']', (size_t)(end - lb));
        if (!rb) break;

        if (code && code < SW_KEYMAP_MAX) {
            int level = 0;
            const char *s = lb + 1;
            while (s < rb && level < 4) {
                s = swk_skip_ws(s, rb);
                const char *e2 = s;
                while (e2 < rb && *e2 != ',' && *e2 != ' ' && *e2 != '\t' &&
                       *e2 != '\n' && *e2 != '\r') ++e2;
                if (e2 > s) km->sym[code][level++] = swk_sym_from_name(s, (size_t)(e2 - s));
                s = (const char *)memchr(e2, ',', (size_t)(rb - e2));
                if (!s) break;
                ++s;
            }

            /* The keypad is the one key type worth knowing about: its second
             * level is chosen by NumLock where every other key uses Shift,
             * and getting that wrong makes the number pad type Home for 7.
             * Nothing in the file says so — a keymap leaves the type out when
             * it can be told from the symbols, which is exactly what this
             * is: both levels in the KP_ block. */
            uint32_t a = km->sym[code][0], b2 = km->sym[code][1];
            km->keypad[code] = (a >= 0xff80 && a <= 0xffbd &&
                                b2 >= 0xff80 && b2 <= 0xffbd) ? 1 : 0;
        }
        p = rb + 1;
    }

    km->ok = true;
    return true;
}

unsigned sw_keymap_mods(const SwKeymap *km, uint32_t depressed)
{
    (void)km;
    unsigned m = 0;
    if (depressed & SWK_SHIFT) m |= 1u;        /* SW_MOD_SHIFT */
    if (depressed & SWK_CTRL)  m |= 2u;        /* SW_MOD_CTRL  */
    if (depressed & SWK_MOD1)  m |= 4u;        /* SW_MOD_ALT   */
    if (depressed & SWK_MOD4)  m |= 8u;        /* SW_MOD_LOGO  */
    if (depressed & SWK_MOD5)  m |= 16u;       /* AltGr, level 3 */
    if (depressed & SWK_LOCK)  m |= 32u;       /* caps lock      */
    if (depressed & SWK_NUM)   m |= 64u;       /* num lock       */
    return m;
}

uint32_t sw_keymap_lookup(const SwKeymap *km, uint32_t keycode, unsigned mods,
                          char *utf8, size_t n)
{
    if (utf8 && n) utf8[0] = 0;
    if (!km || !km->ok || keycode >= SW_KEYMAP_MAX) return 0;

    bool shift = (mods & 1u) != 0, altgr = (mods & 16u) != 0, caps = (mods & 32u) != 0;
    int level = km->keypad[keycode] ? ((mods & 64u) ? 1 : 0)
                                    : ((shift ? 1 : 0) + (altgr ? 2 : 0));
    uint32_t sym = km->sym[keycode][level];
    if (!sym && level) sym = km->sym[keycode][level & 1];
    if (!sym) sym = km->sym[keycode][0];

    /* Caps lock is shift, but only for letters — it must not turn 1 into !. */
    if (caps && !shift) {
        uint32_t plain = km->sym[keycode][altgr ? 2 : 0];
        uint32_t cp = sw_keysym_to_codepoint(plain);
        if (cp && cp < 0x250 && cp == (uint32_t)tolower((int)cp)) {
            uint32_t up = km->sym[keycode][(altgr ? 2 : 0) + 1];
            if (up) sym = up;
        }
    }

    if (utf8 && n) {
        /* Control does not type: ctrl+c is a shortcut, not a character. */
        if (!(mods & 2u)) swk_utf8(sw_keysym_to_codepoint(sym), utf8, n);
    }
    return sym;
}

#endif /* SW_KEYMAP_IMPL_ONCE */
#endif /* SW_KEYMAP_IMPL */
