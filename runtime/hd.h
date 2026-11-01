#pragma once
#include <stdint.h>

// Nine samples per original pixel: 2304 x 2160, pillarboxed on a 4K display.
constexpr int HD_SCALE = 9;
constexpr int HD_WIDTH = 256 * HD_SCALE;
constexpr int HD_HEIGHT = 240 * HD_SCALE;
struct HdPixel {
    const uint32_t *tile = nullptr;
    uint8_t x = 0, y = 0;
    bool flip_x = false, flip_y = false;
    bool background_present = false;
};
bool hd_init(const char *path);
bool hd_active();
void hd_toggle();
bool hd_truecolor();
void hd_set_truecolor(bool enabled);
int hd_tile_count();
int hd_template_count();
void hd_set_colors(const uint32_t *colors);
const uint32_t *hd_find(const uint8_t *pattern, const uint8_t *palette);
const uint32_t *hd_find_sprite(const uint8_t *pattern, const uint8_t *palette);
void hd_scanline(int y, const uint32_t *original, const HdPixel *background,
                 const uint8_t *sprites, const HdPixel *sprite_art = nullptr,
                 const uint32_t *original_background = nullptr);
const uint32_t *hd_framebuffer();
