#include "hd.h"
#include "state.h"
#include <array>
#include <map>
#include <unordered_map>
#include <vector>
#include <stdio.h>
#include <string.h>

using Key = std::array<uint8_t, 20>;
static std::map<Key, std::vector<uint32_t>> tiles;
using Pattern = std::array<uint8_t, 16>;
using ColorKey = std::array<uint8_t, 21>;
static std::map<Pattern, std::vector<uint8_t>> templates;
static std::map<ColorKey, std::vector<uint32_t>> colored;
static uint32_t colors[64] = {};
static std::vector<uint32_t> frame;
static bool enabled = false;
static bool truecolor = true;
static std::map<uint32_t,std::vector<uint32_t>> materials;
struct BackgroundKey {
    const uint32_t *tile;
    uint32_t base;
    uint16_t coordinate;
    bool operator==(const BackgroundKey &other) const {
        return tile==other.tile && base==other.base && coordinate==other.coordinate;
    }
};
struct BackgroundHash {
    size_t operator()(const BackgroundKey &key) const {
        return (reinterpret_cast<size_t>(key.tile)>>4)
            ^ (size_t(key.base)*0x9e3779b1u) ^ (size_t(key.coordinate)*0x85ebca77u);
    }
};
using BackgroundBlock=std::array<uint32_t,HD_SCALE*HD_SCALE>;
static std::unordered_map<BackgroundKey,BackgroundBlock,BackgroundHash> background_blocks;
bool hd_truecolor() { return truecolor; }
void hd_set_truecolor(bool value) { truecolor=value; }

static void load_materials(const char *pack_path) {
    materials.clear();
    char path[1024]; snprintf(path,sizeof(path),"%s",pack_path);
    char *slash=strrchr(path,'/'), *backslash=strrchr(path,'\\');
    if (!slash || (backslash && backslash>slash)) slash=backslash;
    size_t prefix=slash ? slash-path+1 : 0;
    snprintf(path+prefix,sizeof(path)-prefix,"materials.ffmt");
    FILE *f=fopen(path,"rb"); if (!f) return;
    char magic[8]; uint32_t scale=0,count=0;
    bool ok=fread(magic,1,8,f)==8 && !memcmp(magic,"FFMT0001",8)
        && fread(&scale,4,1,f)==1 && scale==9 && fread(&count,4,1,f)==1 && count<=8;
    for (uint32_t n=0;ok && n<count;++n) {
        uint32_t color; std::vector<uint8_t> rgba(72*72*4);
        ok=fread(&color,4,1,f)==1 && fread(rgba.data(),1,rgba.size(),f)==rgba.size();
        if (!ok || materials.count(color)) { ok=false; break; }
        std::vector<uint32_t> pixels(72*72);
        for (size_t i=0;i<pixels.size();++i)
            pixels[i]=(uint32_t(rgba[i*4])<<16)|(uint32_t(rgba[i*4+1])<<8)|rgba[i*4+2];
        materials.emplace(color,std::move(pixels));
    }
    if (ok && fgetc(f)!=EOF) ok=false;
    fclose(f);
    if (!ok) materials.clear();
}

static uint32_t grade_scene(uint32_t c,uint32_t source) {
    int r=(c>>16)&255,g=(c>>8)&255,b=c&255;
    int sr=(source>>16)&255,sg=(source>>8)&255,sb=source&255;
    if (sg>sr+24 && sg>sb+24) {
        // Painted foliage: retain the existing texture, soften neon greens.
        r=(r*3+g)/4; g=g*88/100; b=(b*3+g)/4;
    }
    return (uint32_t(r)<<16)|(uint32_t(g)<<8)|uint32_t(b);
}

static uint32_t color_background(uint32_t c,uint32_t original,const HdPixel &p,
                                 const uint32_t *material,int dx,int dy) {
    if (!truecolor || !p.background_present) return c;
    if (material) {
        uint32_t m=material[(p.y*9+dy)*72+p.x*9+dx];
        int delta=0;
        for (int s=0;s<=16;s+=8) delta+=(int)((c>>s)&255)-(int)((original>>s)&255);
        delta/=6;
        if (delta < -24) delta=-24;
        if (delta > 24) delta=24;
        uint32_t out=0;
        for (int s=0;s<=16;s+=8) {
            int ch=(int)((m>>s)&255)+delta;
            if (ch<0) ch=0; if (ch>255) ch=255;
            out|=uint32_t(ch)<<s;
        }
        return out;
    }
    return grade_scene(c,original);
}

void hd_set_colors(const uint32_t *source) {
    memcpy(colors, source, sizeof(colors)); background_blocks.clear(); colored.clear();
}
int hd_template_count() { return (int)templates.size(); }

static void load_templates(const char *pack_path) {
    background_blocks.clear();
    templates.clear(); colored.clear();
    load_materials(pack_path);
    char path[1024];
    snprintf(path, sizeof(path), "%s", pack_path);
    char *slash = strrchr(path, '/'), *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    size_t prefix = slash ? (size_t)(slash-path+1) : 0;
    snprintf(path+prefix, sizeof(path)-prefix, "templates.ffht");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char magic[8]; uint32_t scale = 0, count = 0;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "FFTM0001", 8)
        && fread(&scale, 4, 1, f) == 1 && scale == HD_SCALE
        && fread(&count, 4, 1, f) == 1 && count <= 8192;
    for (uint32_t i = 0; ok && i < count; ++i) {
        Pattern p;
        std::vector<uint8_t> pixels(72*72);
        ok = fread(p.data(), 1, 16, f) == 16
            && fread(pixels.data(), 1, pixels.size(), f) == pixels.size();
        if (ok && !templates.count(p)) templates.emplace(p, std::move(pixels));
        else ok = false;
    }
    if (ok && fgetc(f) != EOF) ok = false;
    fclose(f);
    if (!ok) { templates.clear(); fprintf(stderr, "HD: invalid template atlas; ignoring it\n"); }
    else fprintf(stderr, "HD: loaded %zu palette-adaptive templates\n", templates.size());
}

bool hd_init(const char *path) {
    enabled = false;
    tiles.clear();
    templates.clear(); colored.clear();
    background_blocks.clear(); background_blocks.reserve(8192);
    frame.assign(HD_WIDTH * HD_HEIGHT, 0);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "HD: no exact pack at %s\n", path); load_templates(path); enabled = true; return true; }
    char magic[8];
    uint32_t scale = 0, count = 0;
    bool ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "FFHD0001", 8)
        && fread(&scale, 4, 1, f) == 1 && scale == HD_SCALE
        && fread(&count, 4, 1, f) == 1 && count <= 8192;
    for (uint32_t n = 0; ok && n < count; ++n) {
        Key key;
        std::vector<uint8_t> rgba(72 * 72 * 4);
        ok = fread(key.data(), 1, 20, f) == 20
            && fread(rgba.data(), 1, rgba.size(), f) == rgba.size();
        if (!ok || tiles.count(key)) { ok = false; break; }
        std::vector<uint32_t> pixels(72 * 72);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = (uint32_t(rgba[i*4+3]) << 24) | (uint32_t(rgba[i*4]) << 16)
                      | (uint32_t(rgba[i*4+1]) << 8) | rgba[i*4+2];
        tiles.emplace(key, std::move(pixels));
    }
    if (ok && fgetc(f) != EOF) ok = false;
    fclose(f);
    if (!ok) { tiles.clear(); fprintf(stderr, "HD: invalid pack; using original renderer\n"); return false; }
    enabled = true;
    load_templates(path);
    fprintf(stderr, "HD: loaded %zu background tiles\n", tiles.size());
    return true;
}

bool hd_active() { return enabled && !frame.empty(); }
void hd_toggle() { if (!frame.empty()) enabled = !enabled; }
int hd_tile_count() { return (int)tiles.size(); }
const uint32_t *hd_framebuffer() { return frame.data(); }

static const uint32_t *find_art(const uint8_t *pattern, const uint8_t *palette, bool sprite) {
    if (!hd_active()) return nullptr;
    Key key;
    memcpy(key.data(), pattern, 16);
    memcpy(key.data() + 16, palette, 4);
    if (!sprite) {
        auto it = tiles.find(key);
        if (it != tiles.end()) return it->second.data();
    }
    Pattern p;
    memcpy(p.data(), pattern, 16);
    auto master = templates.find(p);
    if (master == templates.end()) return nullptr;
    ColorKey ck;
    memcpy(ck.data(), key.data(), 20);
    ck[20] = sprite ? 1 : 0;
    auto cached = colored.find(ck);
    if (cached != colored.end()) return cached->second.data();
    bool used[4] = {};
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        int bit = 7-x;
        unsigned index = ((pattern[y]>>bit)&1) | (((pattern[y+8]>>bit)&1)<<1);
        used[index] = true;
    }
    std::vector<uint32_t> pixels(72*72);
    for (size_t i = 0; i < pixels.size(); ++i) {
        int source_x = (int)(i%72)/HD_SCALE, source_y = (int)(i/72)/HD_SCALE;
        int source_bit = 7-source_x;
        unsigned source_index = ((pattern[source_y]>>source_bit)&1)
            | (((pattern[source_y+8]>>source_bit)&1)<<1);
        if (sprite) {
            // Alpha belongs to the original sprite, not to the luminance of
            // an authored atlas. A white highlight must never cut a hole.
            int x = (int)(i%72)/HD_SCALE, y = (int)(i/72)/HD_SCALE;
            int bit = 7-x;
            unsigned index = ((pattern[y]>>bit)&1) | (((pattern[y+8]>>bit)&1)<<1);
            if (!index) { pixels[i] = 0; continue; }
            uint32_t color = colors[palette[index]&63];
            int original_gray = 255-(int)index*85;
            int detail = ((int)master->second[i]-original_gray)/8;
            if (detail < -6) detail = -6;
            if (detail > 6) detail = 6;
            uint32_t out = 0xff000000;
            for (int shift = 0; shift <= 16; shift += 8) {
                int channel = (int)((color>>shift)&255)+detail;
                if (channel < 0) channel = 0;
                if (channel > 255) channel = 255;
                out |= (uint32_t)channel << shift;
            }
            pixels[i] = out;
            continue;
        }
        // Background index zero is the backdrop. During text reveal/fades,
        // other entries can deliberately equal the backdrop too. Never turn
        // those invisible pixels into opaque paper/shadow from the atlas.
        if (!source_index || colors[palette[source_index]&63] == colors[palette[0]&63]) {
            pixels[i] = 0;
            continue;
        }
        // Interpolate only between indices actually used by this tile. Font
        // tiles typically use indices 2 and 3: shade variation around index 2
        // must not introduce an unrelated (often black) index 1.
        unsigned q = (255-master->second[i])*3;
        int lo = -1, hi = -1;
        for (int index = 0; index < 4; ++index) if (used[index]) {
            if ((unsigned)index*255 <= q) lo = index;
            if (hi < 0 && (unsigned)index*255 >= q) hi = index;
        }
        if (lo < 0) lo = hi;
        if (hi < 0) hi = lo;
        unsigned a = lo == hi ? 0 : (q-lo*255)/(unsigned)(hi-lo);
        unsigned alpha = 255;
        uint32_t c0 = colors[palette[lo]&63], c1 = colors[palette[hi]&63];
        uint32_t c = alpha << 24;
        for (int s = 0; s <= 16; s += 8)
            c |= (((((c0>>s)&255)*(255-a) + ((c1>>s)&255)*a)+127)/255) << s;
        pixels[i] = c;
    }
    auto result = colored.emplace(ck, std::move(pixels));
    return result.first->second.data();
}

const uint32_t *hd_find(const uint8_t *p, const uint8_t *pal) { return find_art(p, pal, false); }
const uint32_t *hd_find_sprite(const uint8_t *p, const uint8_t *pal) { return find_art(p, pal, true); }

static uint32_t blend(uint32_t fg, uint32_t bg) {
    unsigned a = fg >> 24;
    if (a == 255) return fg & 0xffffff;
    if (!a) return bg;
    uint32_t out = 0;
    for (int s = 0; s <= 16; s += 8) {
        unsigned c = (((fg >> s) & 255) * a + ((bg >> s) & 255) * (255-a) + 127) / 255;
        out |= c << s;
    }
    return out;
}

static const BackgroundBlock *background_block(const HdPixel &p,uint32_t base) {
    BackgroundKey key={p.tile,base,uint16_t(p.x|(p.y<<3)|(p.background_present?64:0)|(truecolor?128:0))};
    auto found=background_blocks.find(key);
    if (found!=background_blocks.end()) return &found->second;
    const uint32_t *material=nullptr;
    if (truecolor && p.background_present) {
        auto it=materials.find(base&0xffffff);
        if (it!=materials.end()) material=it->second.data();
    }
    BackgroundBlock result;
    for (int dy=0;dy<HD_SCALE;++dy) {
        const uint32_t *row=p.tile ? p.tile+(p.y*9+dy)*72+p.x*9 : nullptr;
        for (int dx=0;dx<HD_SCALE;++dx) {
            uint32_t c=row ? blend(row[dx],base) : base;
            result[dy*HD_SCALE+dx]=color_background(c,base,p,material,dx,dy);
        }
    }
    auto inserted=background_blocks.emplace(key,result);
    return &inserted.first->second;
}

void hd_scanline(int y, const uint32_t *original, const HdPixel *background,
                 const uint8_t *sprites, const HdPixel *sprite_art,
                 const uint32_t *original_background) {
    if (!hd_active()) return;
    // Lookups and RGB material work happen once per original pixel, not for
    // every one of its 81 high-resolution samples on every frame. Node-based
    // unordered_map keeps returned blocks valid across rehashes in this line.
    // Clear only at a scanline boundary, before retaining any block pointers.
    if (background_blocks.size()>65536) background_blocks.clear();
    const BackgroundBlock *blocks[256];
    for (int x=0;x<256;++x)
        blocks[x]=background_block(background[x],original_background ? original_background[x] : original[x]);
    for (int dy=0;dy<HD_SCALE;++dy) {
        uint32_t *dest=frame.data()+(y*HD_SCALE+dy)*HD_WIDTH;
        for (int x=0;x<256;++x) {
            const uint32_t *row=blocks[x]->data()+dy*HD_SCALE;
            if (!sprites[x]) {
                memcpy(dest,row,HD_SCALE*sizeof(uint32_t));
                dest+=HD_SCALE;
                continue;
            }
            const HdPixel *sp=sprite_art ? sprite_art+x : nullptr;
            if (sp && sp->tile) {
                int sy=sp->y*9+(sp->flip_y ? 8-dy : dy);
                for (int dx=0;dx<HD_SCALE;++dx) {
                    int sx=sp->x*9+(sp->flip_x ? 8-dx : dx);
                    *dest++=blend(sp->tile[sy*72+sx],row[dx]);
                }
            } else {
                for (int dx=0;dx<HD_SCALE;++dx) *dest++=original[x];
            }
        }
    }
}

void hd_state(StateArchive &a) {
    a.value(enabled); a.value(truecolor);
    const size_t bytes=HD_WIDTH*HD_HEIGHT*sizeof(uint32_t);
    if (a.mode==StateArchive::Write && frame.empty()) {
        std::vector<uint32_t> blank(HD_WIDTH*HD_HEIGHT);
        a.bytes(blank.data(),bytes);
    } else {
        if (a.reading() && frame.empty()) frame.resize(HD_WIDTH*HD_HEIGHT);
        a.bytes(frame.empty() ? nullptr : frame.data(),bytes);
    }
    if (a.reading()) { background_blocks.clear(); colored.clear(); }
}
