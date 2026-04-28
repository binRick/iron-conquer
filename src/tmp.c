#include "tmp.h"
#include "debug_log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t r_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t r_u32(const unsigned char *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

bool tmp_load(TmpSprite *out, const char *path, const Palette *palette) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        debug_log("tmp_load_fail open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len < 40) { fclose(f); return false; }

    unsigned char *data = (unsigned char *)malloc((size_t)len);
    if (!data) { fclose(f); return false; }
    if ((long)fread(data, 1, (size_t)len, f) != len) {
        free(data); fclose(f); return false;
    }
    fclose(f);

    /* TmpRA magic check: u32 at offset 20 == 0; u16 at offset 26 == 0x2c73. */
    if (r_u32(data + 20) != 0 || r_u16(data + 26) != 0x2c73) {
        debug_log("tmp_load_fail magic %s", path);
        free(data);
        return false;
    }

    int width  = (int)r_u16(data + 0);
    int height = (int)r_u16(data + 2);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        free(data); return false;
    }

    uint32_t img_start   = r_u32(data + 16);
    int32_t  index_end   = (int32_t)r_u32(data + 28);
    int32_t  index_start = (int32_t)r_u32(data + 36);
    int32_t  count = index_end - index_start;
    if (count <= 0 || count > 1024) { free(data); return false; }
    if (index_start < 0 || index_start + count > (int32_t)len) {
        free(data); return false;
    }

    int pix = width * height;
    unsigned char *index_table = data + index_start;

    out->width       = width;
    out->height      = height;
    out->frame_count = count;
    out->frames      = (Texture2D *)calloc((size_t)count, sizeof(Texture2D));
    if (!out->frames) { free(data); return false; }

    unsigned char *rgba = (unsigned char *)malloc((size_t)(pix * 4));
    if (!rgba) { free(out->frames); out->frames = NULL; free(data); return false; }

    int loaded = 0;
    for (int i = 0; i < count; ++i) {
        unsigned char b = index_table[i];
        if (b == 0xFF) continue; /* blank slot */
        long src_off = (long)img_start + (long)b * pix;
        if (src_off < 0 || src_off + pix > len) continue;
        palette_indexed_to_rgba(palette, data + src_off, width, height, rgba);
        Image img = {
            .data    = rgba,
            .width   = width,
            .height  = height,
            .mipmaps = 1,
            .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        };
        out->frames[i] = LoadTextureFromImage(img);
        loaded++;
    }
    free(rgba);
    free(data);

    debug_log("tmp_load %s: %d/%d tiles %dx%d", path, loaded, count, width, height);
    return loaded > 0;
}

void tmp_unload(TmpSprite *t) {
    if (!t || !t->frames) return;
    for (int i = 0; i < t->frame_count; ++i) {
        if (t->frames[i].id != 0) UnloadTexture(t->frames[i]);
    }
    free(t->frames);
    t->frames = NULL;
    t->frame_count = 0;
}
