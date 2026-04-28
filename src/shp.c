#include "shp.h"
#include "debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SHP_TD format reference: third_party/OpenRA/OpenRA.Mods.Cnc/SpriteLoaders/ShpTDLoader.cs
 *
 * File layout:
 *   header  (14 bytes): u16 imageCount; u16 zeros[2]; u16 width; u16 height; u32 zero;
 *   per-frame headers (8 bytes each):
 *     u32 packed   (low 24 bits = file offset, high 8 bits = format)
 *     u16 refOffset
 *     u16 refFormat
 *   16 bytes (eof + zero header), then frame data.
 *
 * Format bytes:
 *   0x80  LCW         direct LCW-compressed pixels
 *   0x40  XORLCW      LCW-decompress, then XOR-apply against another frame
 *                     pointed to by refOffset (== that frame's FileOffset)
 *   0x20  XORPrev     XOR-apply against the previous frame (no LCW)
 */

typedef struct {
    unsigned char *src;
    int            len;
    int            pos;
} ByteReader;

static unsigned char br_u8(ByteReader *r) {
    if (r->pos >= r->len) return 0;
    return r->src[r->pos++];
}
static unsigned int br_u16(ByteReader *r) {
    unsigned int lo = br_u8(r);
    unsigned int hi = br_u8(r);
    return lo | (hi << 8);
}
static void br_copy(ByteReader *r, unsigned char *dst, int n) {
    for (int i = 0; i < n; ++i) dst[i] = br_u8(r);
}

/* LCW (Format80) decoder, ported from OpenRA's LCWCompression.DecodeInto. */
static int lcw_decode(unsigned char *src, int src_len, int src_offset,
                      unsigned char *dst, int dst_len) {
    ByteReader r = { src, src_len, src_offset };
    int di = 0;
    for (;;) {
        unsigned char i = br_u8(&r);
        if ((i & 0x80) == 0) {
            /* case 2: 0CCCxxxx yyyyyyyy
             *   count = ((i & 0x70) >> 4) + 3
             *   rpos  = ((i & 0x0F) << 8) | yyyyyyyy
             *   replicate from dst[di-rpos] for `count` bytes */
            unsigned char b2 = br_u8(&r);
            int count = ((i & 0x70) >> 4) + 3;
            int rpos  = ((i & 0x0F) << 8) | b2;
            if (di + count > dst_len) return di;
            int srci = di - rpos;
            for (int k = 0; k < count; ++k) {
                if (di - srci == 1) dst[di + k] = dst[di - 1];
                else                dst[di + k] = dst[srci + k];
            }
            di += count;
        } else if ((i & 0x40) == 0) {
            /* case 1: 10CCCCCC raw-copy `count` bytes */
            int count = i & 0x3F;
            if (count == 0) return di; /* terminator */
            if (di + count > dst_len) count = dst_len - di;
            br_copy(&r, dst + di, count);
            di += count;
        } else {
            int c = i & 0x3F;
            if (c == 0x3E) {
                /* case 4: 11111110 cnt_lo cnt_hi color → fill */
                int count = br_u16(&r);
                unsigned char color = br_u8(&r);
                if (di + count > dst_len) count = dst_len - di;
                for (int k = 0; k < count; ++k) dst[di + k] = color;
                di += count;
            } else {
                /* case 3 / 5: copy from absolute dst offset */
                int count = (c == 0x3F) ? br_u16(&r) : c + 3;
                int srci  = br_u16(&r);
                if (srci >= di) return di; /* malformed */
                if (di + count > dst_len) count = dst_len - di;
                for (int k = 0; k < count; ++k) dst[di + k] = dst[srci + k];
                di += count;
            }
        }
    }
}

/* XOR-delta (Format40) decoder, ported from OpenRA's XORDeltaCompression. */
static int xor_decode(unsigned char *src, int src_len, int src_offset,
                      unsigned char *dst, int dst_len) {
    ByteReader r = { src, src_len, src_offset };
    int di = 0;
    for (;;) {
        unsigned char i = br_u8(&r);
        if ((i & 0x80) == 0) {
            int count = i & 0x7F;
            if (count == 0) {
                /* case 6 */
                count = br_u8(&r);
                unsigned char value = br_u8(&r);
                for (int k = 0; k < count && di < dst_len; ++k, ++di)
                    dst[di] ^= value;
            } else {
                /* case 5: XOR `count` source bytes into dst */
                for (int k = 0; k < count && di < dst_len; ++k, ++di)
                    dst[di] ^= br_u8(&r);
            }
        } else {
            int count = i & 0x7F;
            if (count == 0) {
                int w = br_u16(&r);
                if (w == 0) return di;
                if ((w & 0x8000) == 0) {
                    /* case 2: skip */
                    di += (w & 0x7FFF);
                } else if ((w & 0x4000) == 0) {
                    /* case 3 */
                    int n = w & 0x3FFF;
                    for (int k = 0; k < n && di < dst_len; ++k, ++di)
                        dst[di] ^= br_u8(&r);
                } else {
                    /* case 4 */
                    int n = w & 0x3FFF;
                    unsigned char value = br_u8(&r);
                    for (int k = 0; k < n && di < dst_len; ++k, ++di)
                        dst[di] ^= value;
                }
            } else {
                /* case 1: skip */
                di += count;
            }
        }
    }
}

typedef struct {
    unsigned int  file_offset;   /* low 24 bits of packed dword */
    unsigned char format;        /* high 8 bits  of packed dword */
    unsigned int  ref_offset;
    unsigned int  ref_format;
    int           ref_index;     /* resolved later, -1 if none */
    unsigned char *pixels;       /* width*height indexed bytes */
    bool          decoded;
} FrameHdr;

static int find_index_by_offset(const FrameHdr *hdrs, int n, unsigned int off) {
    for (int i = 0; i < n; ++i) if (hdrs[i].file_offset == off) return i;
    return -1;
}

static bool decode_frame(FrameHdr *hdrs, int n, int idx,
                         unsigned char *file_bytes, int file_len,
                         int frame_pixel_count, int recurse_depth) {
    if (recurse_depth > n) return false;
    FrameHdr *h = &hdrs[idx];
    if (h->decoded) return true;

    int src_offset = (int)h->file_offset;
    if (src_offset >= file_len) return false;

    if (h->format == 0x80) {
        h->pixels = (unsigned char *)calloc(1, frame_pixel_count);
        if (!h->pixels) return false;
        lcw_decode(file_bytes, file_len, src_offset, h->pixels, frame_pixel_count);
    } else if (h->format == 0x20 || h->format == 0x40) {
        int ref = h->ref_index;
        if (ref < 0) return false;
        if (!hdrs[ref].decoded) {
            if (!decode_frame(hdrs, n, ref, file_bytes, file_len,
                              frame_pixel_count, recurse_depth + 1))
                return false;
        }
        h->pixels = (unsigned char *)malloc(frame_pixel_count);
        if (!h->pixels) return false;
        memcpy(h->pixels, hdrs[ref].pixels, frame_pixel_count);
        if (h->format == 0x40) {
            xor_decode(file_bytes, file_len, src_offset, h->pixels, frame_pixel_count);
        } else {
            xor_decode(file_bytes, file_len, src_offset, h->pixels, frame_pixel_count);
        }
    } else {
        return false;
    }
    h->decoded = true;
    return true;
}

bool shp_load(ShpSprite *out, const char *path, const Palette *palette) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        debug_log("shp_load: cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_len < 16) { fclose(f); return false; }

    unsigned char *file_bytes = (unsigned char *)malloc((size_t)file_len);
    if (!file_bytes) { fclose(f); return false; }
    if ((long)fread(file_bytes, 1, (size_t)file_len, f) != file_len) {
        fclose(f); free(file_bytes); return false;
    }
    fclose(f);

    ByteReader r = { file_bytes, (int)file_len, 0 };
    int image_count = (int)br_u16(&r);
    if (image_count <= 0) { free(file_bytes); return false; }
    /* skip 4 zero bytes */
    r.pos += 4;
    int w = (int)br_u16(&r);
    int h = (int)br_u16(&r);
    /* skip 4 zero bytes */
    r.pos += 4;

    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        free(file_bytes); return false;
    }

    FrameHdr *hdrs = (FrameHdr *)calloc((size_t)image_count, sizeof(FrameHdr));
    if (!hdrs) { free(file_bytes); return false; }

    for (int i = 0; i < image_count; ++i) {
        unsigned int packed = (unsigned int)br_u8(&r);
        packed |= ((unsigned int)br_u8(&r)) << 8;
        packed |= ((unsigned int)br_u8(&r)) << 16;
        packed |= ((unsigned int)br_u8(&r)) << 24;
        hdrs[i].file_offset = packed & 0x00FFFFFF;
        hdrs[i].format      = (unsigned char)((packed >> 24) & 0xFF);
        hdrs[i].ref_offset  = br_u16(&r);
        hdrs[i].ref_format  = br_u16(&r);
        hdrs[i].ref_index   = -1;
    }

    /* Resolve XOR references. */
    for (int i = 0; i < image_count; ++i) {
        if (hdrs[i].format == 0x20) {
            hdrs[i].ref_index = (i > 0) ? (i - 1) : -1;
        } else if (hdrs[i].format == 0x40) {
            hdrs[i].ref_index = find_index_by_offset(
                hdrs, image_count, hdrs[i].ref_offset);
        }
    }

    int frame_pixel_count = w * h;

    for (int i = 0; i < image_count; ++i) {
        decode_frame(hdrs, image_count, i, file_bytes, (int)file_len,
                     frame_pixel_count, 0);
    }

    out->width       = w;
    out->height      = h;
    out->frame_count = image_count;
    out->frames      = (Texture2D *)calloc((size_t)image_count, sizeof(Texture2D));
    if (!out->frames) goto fail;

    int succeeded = 0;
    unsigned char *rgba = (unsigned char *)malloc((size_t)(frame_pixel_count * 4));
    if (!rgba) goto fail;

    for (int i = 0; i < image_count; ++i) {
        if (!hdrs[i].pixels) continue;
        palette_indexed_to_rgba(palette, hdrs[i].pixels, w, h, rgba);
        Image img = {
            .data    = rgba,
            .width   = w,
            .height  = h,
            .mipmaps = 1,
            .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
        };
        out->frames[i] = LoadTextureFromImage(img);
        succeeded++;
    }

    free(rgba);
    for (int i = 0; i < image_count; ++i) free(hdrs[i].pixels);
    free(hdrs);
    free(file_bytes);

    debug_log("shp_load %s: %d/%d frames %dx%d", path, succeeded, image_count, w, h);
    return succeeded > 0;

fail:
    if (out->frames) free(out->frames);
    out->frames = NULL;
    for (int i = 0; i < image_count; ++i) free(hdrs[i].pixels);
    free(hdrs);
    free(file_bytes);
    return false;
}

void shp_unload(ShpSprite *s) {
    if (!s || !s->frames) return;
    for (int i = 0; i < s->frame_count; ++i) {
        if (s->frames[i].id != 0) UnloadTexture(s->frames[i]);
    }
    free(s->frames);
    s->frames = NULL;
    s->frame_count = 0;
}
