/*
 * Copyright (c) 2013 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/imgutils.h"
#include "libavutil/intfloat.h"
#include "libavutil/eval.h"
#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"
#include "avfilter.h"
#include "formats.h"
#include "framesync.h"
#include "internal.h"
#include "video.h"
#include "blend.h"

#define TOP    0
#define BOTTOM 1

typedef struct BlendContext {
    const AVClass *class;
    FFFrameSync fs;
    int hsub, vsub;             ///< chroma subsampling values
    int nb_planes;
    char *all_expr;
    enum BlendMode all_mode;
    double all_opacity;

    int depth;
    FilterParams params[4];
    int tblend;
    AVFrame *prev_frame;        /* only used with tblend */
} BlendContext;

static const char *const var_names[] = {   "X",   "Y",   "W",   "H",   "SW",   "SH",   "T",   "N",   "A",   "B",   "TOP",   "BOTTOM",        NULL };
enum                                   { VAR_X, VAR_Y, VAR_W, VAR_H, VAR_SW, VAR_SH, VAR_T, VAR_N, VAR_A, VAR_B, VAR_TOP, VAR_BOTTOM, VAR_VARS_NB };

typedef struct ThreadData {
    const AVFrame *top, *bottom;
    AVFrame *dst;
    AVFilterLink *inlink;
    int plane;
    int w, h;
    FilterParams *param;
} ThreadData;

#define OFFSET(x) offsetof(BlendContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption blend_options[] = {
    { "c0_mode", "set component #0 blend mode", OFFSET(params[0].mode), AV_OPT_TYPE_INT, {.i64=0}, 0, BLEND_NB-1, FLAGS, "mode" },
    { "c1_mode", "set component #1 blend mode", OFFSET(params[1].mode), AV_OPT_TYPE_INT, {.i64=0}, 0, BLEND_NB-1, FLAGS, "mode" },
    { "c2_mode", "set component #2 blend mode", OFFSET(params[2].mode), AV_OPT_TYPE_INT, {.i64=0}, 0, BLEND_NB-1, FLAGS, "mode" },
    { "c3_mode", "set component #3 blend mode", OFFSET(params[3].mode), AV_OPT_TYPE_INT, {.i64=0}, 0, BLEND_NB-1, FLAGS, "mode" },
    { "all_mode", "set blend mode for all components", OFFSET(all_mode), AV_OPT_TYPE_INT, {.i64=-1},-1, BLEND_NB-1, FLAGS, "mode" },
    { "addition",   "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_ADDITION},   0, 0, FLAGS, "mode" },
    { "addition128","", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_GRAINMERGE}, 0, 0, FLAGS, "mode" },
    { "grainmerge", "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_GRAINMERGE}, 0, 0, FLAGS, "mode" },
    { "and",        "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_AND},        0, 0, FLAGS, "mode" },
    { "average",    "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_AVERAGE},    0, 0, FLAGS, "mode" },
    { "burn",       "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_BURN},       0, 0, FLAGS, "mode" },
    { "darken",     "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_DARKEN},     0, 0, FLAGS, "mode" },
    { "difference", "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_DIFFERENCE}, 0, 0, FLAGS, "mode" },
    { "difference128", "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_GRAINEXTRACT}, 0, 0, FLAGS, "mode" },
    { "grainextract", "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_GRAINEXTRACT}, 0, 0, FLAGS, "mode" },
    { "divide",     "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_DIVIDE},     0, 0, FLAGS, "mode" },
    { "dodge",      "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_DODGE},      0, 0, FLAGS, "mode" },
    { "exclusion",  "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_EXCLUSION},  0, 0, FLAGS, "mode" },
    { "extremity",  "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_EXTREMITY},  0, 0, FLAGS, "mode" },
    { "freeze",     "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_FREEZE},     0, 0, FLAGS, "mode" },
    { "glow",       "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_GLOW},       0, 0, FLAGS, "mode" },
    { "hardlight",  "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_HARDLIGHT},  0, 0, FLAGS, "mode" },
    { "hardmix",    "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_HARDMIX},    0, 0, FLAGS, "mode" },
    { "heat",       "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_HEAT},       0, 0, FLAGS, "mode" },
    { "lighten",    "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_LIGHTEN},    0, 0, FLAGS, "mode" },
    { "linearlight","", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_LINEARLIGHT},0, 0, FLAGS, "mode" },
    { "multiply",   "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_MULTIPLY},   0, 0, FLAGS, "mode" },
    { "multiply128","", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_MULTIPLY128},0, 0, FLAGS, "mode" },
    { "negation",   "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_NEGATION},   0, 0, FLAGS, "mode" },
    { "normal",     "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_NORMAL},     0, 0, FLAGS, "mode" },
    { "or",         "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_OR},         0, 0, FLAGS, "mode" },
    { "overlay",    "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_OVERLAY},    0, 0, FLAGS, "mode" },
    { "phoenix",    "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_PHOENIX},    0, 0, FLAGS, "mode" },
    { "pinlight",   "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_PINLIGHT},   0, 0, FLAGS, "mode" },
    { "reflect",    "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_REFLECT},    0, 0, FLAGS, "mode" },
    { "screen",     "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_SCREEN},     0, 0, FLAGS, "mode" },
    { "softlight",  "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_SOFTLIGHT},  0, 0, FLAGS, "mode" },
    { "subtract",   "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_SUBTRACT},   0, 0, FLAGS, "mode" },
    { "vividlight", "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_VIVIDLIGHT}, 0, 0, FLAGS, "mode" },
    { "xor",        "", 0, AV_OPT_TYPE_CONST, {.i64=BLEND_XOR},        0, 0, FLAGS, "mode" },
    { "c0_expr",  "set color component #0 expression", OFFSET(params[0].expr_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "c1_expr",  "set color component #1 expression", OFFSET(params[1].expr_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "c2_expr",  "set color component #2 expression", OFFSET(params[2].expr_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "c3_expr",  "set color component #3 expression", OFFSET(params[3].expr_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "all_expr", "set expression for all color components", OFFSET(all_expr), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { "c0_opacity",  "set color component #0 opacity", OFFSET(params[0].opacity), AV_OPT_TYPE_DOUBLE, {.dbl=1}, 0, 1, FLAGS },
    { "c1_opacity",  "set color component #1 opacity", OFFSET(params[1].opacity), AV_OPT_TYPE_DOUBLE, {.dbl=1}, 0, 1, FLAGS },
    { "c2_opacity",  "set color component #2 opacity", OFFSET(params[2].opacity), AV_OPT_TYPE_DOUBLE, {.dbl=1}, 0, 1, FLAGS },
    { "c3_opacity",  "set color component #3 opacity", OFFSET(params[3].opacity), AV_OPT_TYPE_DOUBLE, {.dbl=1}, 0, 1, FLAGS },
    { "all_opacity", "set opacity for all color components", OFFSET(all_opacity), AV_OPT_TYPE_DOUBLE, {.dbl=1}, 0, 1, FLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(blend, BlendContext, fs);

#define COPY(src, depth)                                                            \
static void blend_copy ## src##_##depth(const uint8_t *top, ptrdiff_t top_linesize,    \
                            const uint8_t *bottom, ptrdiff_t bottom_linesize,\
                            uint8_t *dst, ptrdiff_t dst_linesize,            \
                            ptrdiff_t width, ptrdiff_t height,               \
                            FilterParams *param, double *values, int starty) \
{                                                                            \
    av_image_copy_plane(dst, dst_linesize, src, src ## _linesize,            \
                        width * depth / 8, height);                          \
}

COPY(top, 8)
COPY(bottom, 8)

COPY(top, 16)
COPY(bottom, 16)

COPY(top, 32)
COPY(bottom, 32)

#undef COPY

static void blend_normal_8bit(const uint8_t *top, ptrdiff_t top_linesize,
                              const uint8_t *bottom, ptrdiff_t bottom_linesize,
                              uint8_t *dst, ptrdiff_t dst_linesize,
                              ptrdiff_t width, ptrdiff_t height,
                              FilterParams *param, double *values, int starty)
{
    const float opacity = param->opacity;
    int i, j;

    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++) {
            dst[j] = top[j] * opacity + bottom[j] * (1.f - opacity);
        }
        dst    += dst_linesize;
        top    += top_linesize;
        bottom += bottom_linesize;
    }
}

static void blend_normal_16bit(const uint8_t *_top, ptrdiff_t top_linesize,
                                  const uint8_t *_bottom, ptrdiff_t bottom_linesize,
                                  uint8_t *_dst, ptrdiff_t dst_linesize,
                                  ptrdiff_t width, ptrdiff_t height,
                                  FilterParams *param, double *values, int starty)
{
    const uint16_t *top = (uint16_t*)_top;
    const uint16_t *bottom = (uint16_t*)_bottom;
    uint16_t *dst = (uint16_t*)_dst;
    const float opacity = param->opacity;
    int i, j;
    dst_linesize /= 2;
    top_linesize /= 2;
    bottom_linesize /= 2;

    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++) {
            dst[j] = top[j] * opacity + bottom[j] * (1.f - opacity);
        }
        dst    += dst_linesize;
        top    += top_linesize;
        bottom += bottom_linesize;
    }
}

static void blend_normal_32bit(const uint8_t *_top, ptrdiff_t top_linesize,
                               const uint8_t *_bottom, ptrdiff_t bottom_linesize,
                               uint8_t *_dst, ptrdiff_t dst_linesize,
                               ptrdiff_t width, ptrdiff_t height,
                               FilterParams *param, double *values, int starty)
{
    const float *top = (float*)_top;
    const float *bottom = (float*)_bottom;
    float *dst = (float*)_dst;
    const float opacity = param->opacity;
    int i, j;
    dst_linesize /= 4;
    top_linesize /= 4;
    bottom_linesize /= 4;

    for (i = 0; i < height; i++) {
        for (j = 0; j < width; j++) {
            dst[j] = top[j] * opacity + bottom[j] * (1.f - opacity);
        }
        dst    += dst_linesize;
        top    += top_linesize;
        bottom += bottom_linesize;
    }
}

#define DEFINE_BLEND8(name, expr)                                              \
static void blend_## name##_8bit(const uint8_t *top, ptrdiff_t top_linesize,         \
                                 const uint8_t *bottom, ptrdiff_t bottom_linesize,   \
                                 uint8_t *dst, ptrdiff_t dst_linesize,               \
                                 ptrdiff_t width, ptrdiff_t height,                \
                                 FilterParams *param, double *values, int starty) \
{                                                                              \
    const float opacity = param->opacity;                                      \
    int i, j;                                                                  \
                                                                               \
    for (i = 0; i < height; i++) {                                             \
        for (j = 0; j < width; j++) {                                          \
            dst[j] = top[j] + ((expr) - top[j]) * opacity;                     \
        }                                                                      \
        dst    += dst_linesize;                                                \
        top    += top_linesize;                                                \
        bottom += bottom_linesize;                                             \
    }                                                                          \
}

#define DEFINE_BLEND16(name, expr, depth)                                            \
static void blend_## name##_##depth##bit(const uint8_t *_top, ptrdiff_t top_linesize,\
                                  const uint8_t *_bottom, ptrdiff_t bottom_linesize, \
                                  uint8_t *_dst, ptrdiff_t dst_linesize,             \
                                  ptrdiff_t width, ptrdiff_t height,           \
                                  FilterParams *param, double *values, int starty)         \
{                                                                              \
    const uint16_t *top = (const uint16_t*)_top;                               \
    const uint16_t *bottom = (const uint16_t*)_bottom;                         \
    uint16_t *dst = (uint16_t*)_dst;                                           \
    const float opacity = param->opacity;                                      \
    int i, j;                                                                  \
    dst_linesize /= 2;                                                         \
    top_linesize /= 2;                                                         \
    bottom_linesize /= 2;                                                      \
                                                                               \
    for (i = 0; i < height; i++) {                                             \
        for (j = 0; j < width; j++) {                                          \
            dst[j] = top[j] + ((expr) - top[j]) * opacity;                     \
        }                                                                      \
        dst    += dst_linesize;                                                \
        top    += top_linesize;                                                \
        bottom += bottom_linesize;                                             \
    }                                                                          \
}

#define DEFINE_BLEND32(name, expr, depth)                                            \
static void blend_## name##_##depth##bit(const uint8_t *_top, ptrdiff_t top_linesize,\
                                  const uint8_t *_bottom, ptrdiff_t bottom_linesize, \
                                  uint8_t *_dst, ptrdiff_t dst_linesize,             \
                                  ptrdiff_t width, ptrdiff_t height,                 \
                                  FilterParams *param, double *values, int starty)   \
{                                                                              \
    const float *top = (const float*)_top;                                     \
    const float *bottom = (const float*)_bottom;                               \
    float *dst = (float*)_dst;                                                 \
    const float opacity = param->opacity;                                      \
    int i, j;                                                                  \
    dst_linesize /= 4;                                                         \
    top_linesize /= 4;                                                         \
    bottom_linesize /= 4;                                                      \
                                                                               \
    for (i = 0; i < height; i++) {                                             \
        for (j = 0; j < width; j++) {                                          \
            dst[j] = top[j] + ((expr) - top[j]) * opacity;                     \
        }                                                                      \
        dst    += dst_linesize;                                                \
        top    += top_linesize;                                                \
        bottom += bottom_linesize;                                             \
    }                                                                          \
}

#define A top[j]
#define B bottom[j]

#define MULTIPLY(x, a, b) ((x) * (((a) * (b)) / 255))
#define SCREEN(x, a, b)   (255 - (x) * ((255 - (a)) * (255 - (b)) / 255))
#define BURN(a, b)        (((a) == 0) ? (a) : FFMAX(0, 255 - ((255 - (b)) << 8) / (a)))
#define DODGE(a, b)       (((a) == 255) ? (a) : FFMIN(255, (((b) << 8) / (255 - (a)))))

DEFINE_BLEND8(addition,   FFMIN(255, A + B))
DEFINE_BLEND8(grainmerge, av_clip_uint8(A + B - 128))
DEFINE_BLEND8(average,    (A + B) / 2)
DEFINE_BLEND8(subtract,   FFMAX(0, A - B))
DEFINE_BLEND8(multiply,   MULTIPLY(1, A, B))
DEFINE_BLEND8(multiply128,av_clip_uint8((A - 128) * B / 32. + 128))
DEFINE_BLEND8(negation,   255 - FFABS(255 - A - B))
DEFINE_BLEND8(extremity,  FFABS(255 - A - B))
DEFINE_BLEND8(difference, FFABS(A - B))
DEFINE_BLEND8(grainextract, av_clip_uint8(128 + A - B))
DEFINE_BLEND8(screen,     SCREEN(1, A, B))
DEFINE_BLEND8(overlay,    (A < 128) ? MULTIPLY(2, A, B) : SCREEN(2, A, B))
DEFINE_BLEND8(hardlight,  (B < 128) ? MULTIPLY(2, B, A) : SCREEN(2, B, A))
DEFINE_BLEND8(hardmix,    (A < (255 - B)) ? 0: 255)
DEFINE_BLEND8(heat,       (A == 0) ? 0 : 255 - FFMIN(((255 - B) * (255 - B)) / A, 255))
DEFINE_BLEND8(freeze,     (B == 0) ? 0 : 255 - FFMIN(((255 - A) * (255 - A)) / B, 255))
DEFINE_BLEND8(darken,     FFMIN(A, B))
DEFINE_BLEND8(lighten,    FFMAX(A, B))
DEFINE_BLEND8(divide,     av_clip_uint8(B == 0 ? 255 : 255 * A / B))
DEFINE_BLEND8(dodge,      DODGE(A, B))
DEFINE_BLEND8(burn,       BURN(A, B))
DEFINE_BLEND8(softlight,  (A > 127) ? B + (255 - B) * (A - 127.5) / 127.5 * (0.5 - fabs(B - 127.5) / 255): B - B * ((127.5 - A) / 127.5) * (0.5 - fabs(B - 127.5)/255))
DEFINE_BLEND8(exclusion,  A + B - 2 * A * B / 255)
DEFINE_BLEND8(pinlight,   (B < 128) ? FFMIN(A, 2 * B) : FFMAX(A, 2 * (B - 128)))
DEFINE_BLEND8(phoenix,    FFMIN(A, B) - FFMAX(A, B) + 255)
DEFINE_BLEND8(reflect,    (B == 255) ? B : FFMIN(255, (A * A / (255 - B))))
DEFINE_BLEND8(glow,       (A == 255) ? A : FFMIN(255, (B * B / (255 - A))))
DEFINE_BLEND8(and,        A & B)
DEFINE_BLEND8(or,         A | B)
DEFINE_BLEND8(xor,        A ^ B)
DEFINE_BLEND8(vividlight, (A < 128) ? BURN(2 * A, B) : DODGE(2 * (A - 128), B))
DEFINE_BLEND8(linearlight,av_clip_uint8((B < 128) ? B + 2 * A - 255 : B + 2 * (A - 128)))

#undef MULTIPLY
#undef SCREEN
#undef BURN
#undef DODGE

#define MULTIPLY(x, a, b) ((x) * (((a) * (b)) / 65535))
#define SCREEN(x, a, b)   (65535 - (x) * ((65535 - (a)) * (65535 - (b)) / 65535))
#define BURN(a, b)        (((a) == 0) ? (a) : FFMAX(0, 65535 - ((65535 - (b)) << 16) / (a)))
#define DODGE(a, b)       (((a) == 65535) ? (a) : FFMIN(65535, (((b) << 16) / (65535 - (a)))))

DEFINE_BLEND16(addition,   FFMIN(65535, A + B), 16)
DEFINE_BLEND16(grainmerge, av_clip_uint16(A + B - 32768), 16)
DEFINE_BLEND16(average,    (A + B) / 2, 16)
DEFINE_BLEND16(subtract,   FFMAX(0, A - B), 16)
DEFINE_BLEND16(multiply,   MULTIPLY(1, A, B), 16)
DEFINE_BLEND16(multiply128, av_clip_uint16((A - 32768) * B / 8192. + 32768), 16)
DEFINE_BLEND16(negation,   65535 - FFABS(65535 - A - B), 16)
DEFINE_BLEND16(extremity,  FFABS(65535 - A - B), 16)
DEFINE_BLEND16(difference, FFABS(A - B), 16)
DEFINE_BLEND16(grainextract, av_clip_uint16(32768 + A - B), 16)
DEFINE_BLEND16(screen,     SCREEN(1, A, B), 16)
DEFINE_BLEND16(overlay,    (A < 32768) ? MULTIPLY(2, A, B) : SCREEN(2, A, B), 16)
DEFINE_BLEND16(hardlight,  (B < 32768) ? MULTIPLY(2, B, A) : SCREEN(2, B, A), 16)
DEFINE_BLEND16(hardmix,    (A < (65535 - B)) ? 0: 65535, 16)
DEFINE_BLEND16(heat,       (A == 0) ? 0 : 65535 - FFMIN(((65535 - B) * (65535 - B)) / A, 65535), 16)
DEFINE_BLEND16(freeze,     (B == 0) ? 0 : 65535 - FFMIN(((65535 - A) * (65535 - A)) / B, 65535), 16)
DEFINE_BLEND16(darken,     FFMIN(A, B), 16)
DEFINE_BLEND16(lighten,    FFMAX(A, B), 16)
DEFINE_BLEND16(divide,     av_clip_uint16(B == 0 ? 65535 : 65535 * A / B), 16)
DEFINE_BLEND16(dodge,      DODGE(A, B), 16)
DEFINE_BLEND16(burn,       BURN(A, B), 16)
DEFINE_BLEND16(softlight,  (A > 32767) ? B + (65535 - B) * (A - 32767.5) / 32767.5 * (0.5 - fabs(B - 32767.5) / 65535): B - B * ((32767.5 - A) / 32767.5) * (0.5 - fabs(B - 32767.5)/65535), 16)
DEFINE_BLEND16(exclusion,  A + B - 2 * A * B / 65535, 16)
DEFINE_BLEND16(pinlight,   (B < 32768) ? FFMIN(A, 2 * B) : FFMAX(A, 2 * (B - 32768)), 16)
DEFINE_BLEND16(phoenix,    FFMIN(A, B) - FFMAX(A, B) + 65535, 16)
DEFINE_BLEND16(reflect,    (B == 65535) ? B : FFMIN(65535, (A * A / (65535 - B))), 16)
DEFINE_BLEND16(glow,       (A == 65535) ? A : FFMIN(65535, (B * B / (65535 - A))), 16)
DEFINE_BLEND16(and,        A & B, 16)
DEFINE_BLEND16(or,         A | B, 16)
DEFINE_BLEND16(xor,        A ^ B, 16)
DEFINE_BLEND16(vividlight, (A < 32768) ? BURN(2 * A, B) : DODGE(2 * (A - 32768), B), 16)
DEFINE_BLEND16(linearlight,av_clip_uint16((B < 32768) ? B + 2 * A - 65535 : B + 2 * (A - 32768)), 16)

#undef MULTIPLY
#undef SCREEN
#undef BURN
#undef DODGE

#define MULTIPLY(x, a, b) ((x) * (((a) * (b)) / 1023))
#define SCREEN(x, a, b)   (1023 - (x) * ((1023 - (a)) * (1023 - (b)) / 1023))
#define BURN(a, b)        (((a) == 0) ? (a) : FFMAX(0, 1023 - ((1023 - (b)) << 10) / (a)))
#define DODGE(a, b)       (((a) == 1023) ? (a) : FFMIN(1023, (((b) << 10) / (1023 - (a)))))

DEFINE_BLEND16(addition,   FFMIN(1023, A + B), 10)
DEFINE_BLEND16(grainmerge, (int)av_clip_uintp2(A + B - 512, 10), 10)
DEFINE_BLEND16(average,    (A + B) / 2, 10)
DEFINE_BLEND16(subtract,   FFMAX(0, A - B), 10)
DEFINE_BLEND16(multiply,   MULTIPLY(1, A, B), 10)
DEFINE_BLEND16(multiply128, (int)av_clip_uintp2((A - 512) * B / 128. + 512, 10), 10)
DEFINE_BLEND16(negation,   1023 - FFABS(1023 - A - B), 10)
DEFINE_BLEND16(extremity,  FFABS(1023 - A - B), 10)
DEFINE_BLEND16(difference, FFABS(A - B), 10)
DEFINE_BLEND16(grainextract, (int)av_clip_uintp2(512 + A - B, 10), 10)
DEFINE_BLEND16(screen,     SCREEN(1, A, B), 10)
DEFINE_BLEND16(overlay,    (A < 512) ? MULTIPLY(2, A, B) : SCREEN(2, A, B), 10)
DEFINE_BLEND16(hardlight,  (B < 512) ? MULTIPLY(2, B, A) : SCREEN(2, B, A), 10)
DEFINE_BLEND16(hardmix,    (A < (1023 - B)) ? 0: 1023, 10)
DEFINE_BLEND16(heat,       (A == 0) ? 0 : 1023 - FFMIN(((1023 - B) * (1023 - B)) / A, 1023), 10)
DEFINE_BLEND16(freeze,     (B == 0) ? 0 : 1023 - FFMIN(((1023 - A) * (1023 - A)) / B, 1023), 10)
DEFINE_BLEND16(darken,     FFMIN(A, B), 10)
DEFINE_BLEND16(lighten,    FFMAX(A, B), 10)
DEFINE_BLEND16(divide,     (int)av_clip_uintp2(B == 0 ? 1023 : 1023 * A / B, 10), 10)
DEFINE_BLEND16(dodge,      DODGE(A, B), 10)
DEFINE_BLEND16(burn,       BURN(A, B), 10)
DEFINE_BLEND16(softlight,  (A > 511) ? B + (1023 - B) * (A - 511.5) / 511.5 * (0.5 - fabs(B - 511.5) / 1023): B - B * ((511.5 - A) / 511.5) * (0.5 - fabs(B - 511.5)/1023), 10)
DEFINE_BLEND16(exclusion,  A + B - 2 * A * B / 1023, 10)
DEFINE_BLEND16(pinlight,   (B < 512) ? FFMIN(A, 2 * B) : FFMAX(A, 2 * (B - 512)), 10)
DEFINE_BLEND16(phoenix,    FFMIN(A, B) - FFMAX(A, B) + 1023, 10)
DEFINE_BLEND16(reflect,    (B == 1023) ? B : FFMIN(1023, (A * A / (1023 - B))), 10)
DEFINE_BLEND16(glow,       (A == 1023) ? A : FFMIN(1023, (B * B / (1023 - A))), 10)
DEFINE_BLEND16(and,        A & B, 10)
DEFINE_BLEND16(or,         A | B, 10)
DEFINE_BLEND16(xor,        A ^ B, 10)
DEFINE_BLEND16(vividlight, (A < 512) ? BURN(2 * A, B) : DODGE(2 * (A - 512), B), 10)
DEFINE_BLEND16(linearlight,(int)av_clip_uintp2((B < 512) ? B + 2 * A - 1023 : B + 2 * (A - 512), 10), 10)

#undef MULTIPLY
#undef SCREEN
#undef BURN
#undef DODGE

#define MULTIPLY(x, a, b) ((x) * (((a) * (b)) / 4095))
#define SCREEN(x, a, b)   (4095 - (x) * ((4095 - (a)) * (4095 - (b)) / 4095))
#define BURN(a, b)        (((a) == 0) ? (a) : FFMAX(0, 4095 - ((4095 - (b)) << 12) / (a)))
#define DODGE(a, b)       (((a) == 4095) ? (a) : FFMIN(4095, (((b) << 12) / (4095 - (a)))))

DEFINE_BLEND16(addition,   FFMIN(4095, A + B), 12)
DEFINE_BLEND16(grainmerge, (int)av_clip_uintp2(A + B - 2048, 12), 12)
DEFINE_BLEND16(average,    (A + B) / 2, 12)
DEFINE_BLEND16(subtract,   FFMAX(0, A - B), 12)
DEFINE_BLEND16(multiply,   MULTIPLY(1, A, B), 12)
DEFINE_BLEND16(multiply128, (int)av_clip_uintp2((A - 2048) * B / 512. + 2048, 12), 12)
DEFINE_BLEND16(negation,   4095 - FFABS(4095 - A - B), 12)
DEFINE_BLEND16(extremity,  FFABS(4095 - A - B), 12)
DEFINE_BLEND16(difference, FFABS(A - B), 12)
DEFINE_BLEND16(grainextract, (int)av_clip_uintp2(2048 + A - B, 12), 12)
DEFINE_BLEND16(screen,     SCREEN(1, A, B), 12)
DEFINE_BLEND16(overlay,    (A < 2048) ? MULTIPLY(2, A, B) : SCREEN(2, A, B), 12)
DEFINE_BLEND16(hardlight,  (B < 2048) ? MULTIPLY(2, B, A) : SCREEN(2, B, A), 12)
DEFINE_BLEND16(hardmix,    (A < (4095 - B)) ? 0: 4095, 12)
DEFINE_BLEND16(heat,       (A == 0) ? 0 : 4095 - FFMIN(((4095 - B) * (4095 - B)) / A, 4095), 12)
DEFINE_BLEND16(freeze,     (B == 0) ? 0 : 4095 - FFMIN(((4095 - A) * (4095 - A)) / B, 4095), 12)
DEFINE_BLEND16(darken,     FFMIN(A, B), 12)
DEFINE_BLEND16(lighten,    FFMAX(A, B), 12)
DEFINE_BLEND16(divide,     (int)av_clip_uintp2(B == 0 ? 4095 : 4095 * A / B, 12), 12)
DEFINE_BLEND16(dodge,      DODGE(A, B), 12)
DEFINE_BLEND16(burn,       BURN(A, B), 12)
DEFINE_BLEND16(softlight,  (A > 2047) ? B + (4095 - B) * (A - 2047.5) / 2047.5 * (0.5 - fabs(B - 2047.5) / 4095): B - B * ((2047.5 - A) / 2047.5) * (0.5 - fabs(B - 2047.5)/4095), 12)
DEFINE_BLEND16(exclusion,  A + B - 2 * A * B / 4095, 12)
DEFINE_BLEND16(pinlight,   (B < 2048) ? FFMIN(A, 2 * B) : FFMAX(A, 2 * (B - 2048)), 12)
DEFINE_BLEND16(phoenix,    FFMIN(A, B) - FFMAX(A, B) + 4095, 12)
DEFINE_BLEND16(reflect,    (B == 4095) ? B : FFMIN(4095, (A * A / (4095 - B))), 12)
DEFINE_BLEND16(glow,       (A == 4095) ? A : FFMIN(4095, (B * B / (4095 - A))), 12)
DEFINE_BLEND16(and,        A & B, 12)
DEFINE_BLEND16(or,         A | B, 12)
DEFINE_BLEND16(xor,        A ^ B, 12)
DEFINE_BLEND16(vividlight, (A < 2048) ? BURN(2 * A, B) : DODGE(2 * (A - 2048), B), 12)
DEFINE_BLEND16(linearlight,(int)av_clip_uintp2((B < 2048) ? B + 2 * A - 4095 : B + 2 * (A - 2048), 12), 12)

#undef MULTIPLY
#undef SCREEN
#undef BURN
#undef DODGE

#define MULTIPLY(x, a, b) ((x) * (((a) * (b)) / 511))
#define SCREEN(x, a, b)   (511 - (x) * ((511 - (a)) * (511 - (b)) / 511))
#define BURN(a, b)        (((a) == 0) ? (a) : FFMAX(0, 511 - ((511 - (b)) << 9) / (a)))
#define DODGE(a, b)       (((a) == 511) ? (a) : FFMIN(511, (((b) << 9) / (511 - (a)))))

DEFINE_BLEND16(addition,   FFMIN(511, A + B), 9)
DEFINE_BLEND16(grainmerge, (int)av_clip_uintp2(A + B - 256, 9), 9)
DEFINE_BLEND16(average,    (A + B) / 2, 9)
DEFINE_BLEND16(subtract,   FFMAX(0, A - B), 9)
DEFINE_BLEND16(multiply,   MULTIPLY(1, A, B), 9)
DEFINE_BLEND16(multiply128, (int)av_clip_uintp2((A - 256) * B / 64. + 256, 9), 9)
DEFINE_BLEND16(negation,   511 - FFABS(511 - A - B), 9)
DEFINE_BLEND16(extremity,  FFABS(511 - A - B), 9)
DEFINE_BLEND16(difference, FFABS(A - B), 9)
DEFINE_BLEND16(grainextract, (int)av_clip_uintp2(256 + A - B, 9), 9)
DEFINE_BLEND16(screen,     SCREEN(1, A, B), 9)
DEFINE_BLEND16(overlay,    (A < 256) ? MULTIPLY(2, A, B) : SCREEN(2, A, B), 9)
DEFINE_BLEND16(hardlight,  (B < 256) ? MULTIPLY(2, B, A) : SCREEN(2, B, A), 9)
DEFINE_BLEND16(hardmix,    (A < (511 - B)) ? 0: 511, 9)
DEFINE_BLEND16(heat,       (A == 0) ? 0 : 511 - FFMIN(((511 - B) * (511 - B)) / A, 511), 9)
DEFINE_BLEND16(freeze,     (B == 0) ? 0 : 511 - FFMIN(((511 - A) * (511 - A)) / B, 511), 9)
DEFINE_BLEND16(darken,     FFMIN(A, B), 9)
DEFINE_BLEND16(lighten,    FFMAX(A, B), 9)
DEFINE_BLEND16(divide,     (int)av_clip_uintp2(B == 0 ? 511 : 511 * A / B, 9), 9)
DEFINE_BLEND16(dodge,      DODGE(A, B), 9)
DEFINE_BLEND16(burn,       BURN(A, B), 9)
DEFINE_BLEND16(softlight,  (A > 511) ? B + (511 - B) * (A - 511.5) / 511.5 * (0.5 - fabs(B - 511.5) / 511): B - B * ((511.5 - A) / 511.5) * (0.5 - fabs(B - 511.5)/511), 9)
DEFINE_BLEND16(exclusion,  A + B - 2 * A * B / 511, 9)
DEFINE_BLEND16(pinlight,   (B < 256) ? FFMIN(A, 2 * B) : FFMAX(A, 2 * (B - 256)), 9)
DEFINE_BLEND16(phoenix,    FFMIN(A, B) - FFMAX(A, B) + 511, 9)
DEFINE_BLEND16(reflect,    (B == 511) ? B : FFMIN(511, (A * A / (511 - B))), 9)
DEFINE_BLEND16(glow,       (A == 511) ? A : FFMIN(511, (B * B / (511 - A))), 9)
DEFINE_BLEND16(and,        A & B, 9)
DEFINE_BLEND16(or,         A | B, 9)
DEFINE_BLEND16(xor,        A ^ B, 9)
DEFINE_BLEND16(vividlight, (A < 256) ? BURN(2 * A, B) : DODGE(2 * (A - 256), B), 9)
DEFINE_BLEND16(linearlight,(int)av_clip_uintp2((B < 256) ? B + 2 * A - 511 : B + 2 * (A - 256), 9), 9)

#undef MULTIPLY
#undef SCREEN
#undef BURN
#undef DODGE

#define MULTIPLY(x, a, b) ((x) * (((a) * (b)) / 1.0))
#define SCREEN(x, a, b)   (1.0 - (x) * ((1.0 - (a)) * (1.0 - (b)) / 1.0))
#define BURN(a, b)        (((a) <= 0.0) ? (a) : FFMAX(0.0, 1.0 - (1.0 - (b)) / (a)))
#define DODGE(a, b)       (((a) >= 1.0) ? (a) : FFMIN(1.0, ((b) / (1.0 - (a)))))

DEFINE_BLEND32(addition,   A + B, 32)
DEFINE_BLEND32(grainmerge, A + B - 0.5, 32)
DEFINE_BLEND32(average,    (A + B) / 2, 32)
DEFINE_BLEND32(subtract,   A - B, 32)
DEFINE_BLEND32(multiply,   A * B, 32)
DEFINE_BLEND32(multiply128, (A - 0.5) * B / 0.125 + 0.5, 32)
DEFINE_BLEND32(negation,   1.0 - FFABS(1.0 - A - B), 32)
DEFINE_BLEND32(extremity,  FFABS(1.0 - A - B), 32)
DEFINE_BLEND32(difference, FFABS(A - B), 32)
DEFINE_BLEND32(grainextract, 0.5 + A - B, 32)
DEFINE_BLEND32(screen,     SCREEN(1, A, B), 32)
DEFINE_BLEND32(overlay,    (A < 0.5) ? MULTIPLY(2, A, B) : SCREEN(2, A, B), 32)
DEFINE_BLEND32(hardlight,  (B < 0.5) ? MULTIPLY(2, B, A) : SCREEN(2, B, A), 32)
DEFINE_BLEND32(hardmix,    (A < (1.0 - B)) ? 0: 1.0, 32)
DEFINE_BLEND32(heat,       (A == 0) ? 0 : 1.0 - FFMIN(((1.0 - B) * (1.0 - B)) / A, 1.0), 32)
DEFINE_BLEND32(freeze,     (B == 0) ? 0 : 1.0 - FFMIN(((1.0 - A) * (1.0 - A)) / B, 1.0), 32)
DEFINE_BLEND32(darken,     FFMIN(A, B), 32)
DEFINE_BLEND32(lighten,    FFMAX(A, B), 32)
DEFINE_BLEND32(divide,     B == 0 ? 1.0 : 1.0 * A / B, 32)
DEFINE_BLEND32(dodge,      DODGE(A, B), 32)
DEFINE_BLEND32(burn,       BURN(A, B), 32)
DEFINE_BLEND32(softlight,  (A > 0.5) ? B + (1.0 - B) * (A - 0.5) / 0.5 * (0.5 - fabs(B - 0.5) / 1.0): B - B * ((0.5 - A) / 0.5) * (0.5 - fabs(B - 0.5)/1.0), 32)
DEFINE_BLEND32(exclusion,  A + B - 2 * A * B / 1.0, 32)
DEFINE_BLEND32(pinlight,   (B < 0.5) ? FFMIN(A, 2 * B) : FFMAX(A, 2 * (B - 0.5)), 32)
DEFINE_BLEND32(phoenix,    FFMIN(A, B) - FFMAX(A, B) + 1.0, 32)
DEFINE_BLEND32(reflect,    (B == 1.0) ? B : FFMIN(1.0, (A * A / (1.0 - B))), 32)
DEFINE_BLEND32(glow,       (A == 1.0) ? A : FFMIN(1.0, (B * B / (1.0 - A))), 32)
DEFINE_BLEND32(and,        av_int2float(av_float2int(A) & av_float2int(B)), 32)
DEFINE_BLEND32(or,         av_int2float(av_float2int(A) | av_float2int(B)), 32)
DEFINE_BLEND32(xor,        av_int2float(av_float2int(A) ^ av_float2int(B)), 32)
DEFINE_BLEND32(vividlight, (A < 0.5) ? BURN(2 * A, B) : DODGE(2 * (A - 0.5), B), 32)
DEFINE_BLEND32(linearlight,(B < 0.5) ? B + 2 * A - 1.0 : B + 2 * (A - 0.5), 32)

#define DEFINE_BLEND_EXPR(type, name, div)                                     \
static void blend_expr_## name(const uint8_t *_top, ptrdiff_t top_linesize,          \
                               const uint8_t *_bottom, ptrdiff_t bottom_linesize,    \
                               uint8_t *_dst, ptrdiff_t dst_linesize,                \
                               ptrdiff_t width, ptrdiff_t height,              \
                               FilterParams *param, double *values, int starty) \
{                                                                              \
    const type *top = (type*)_top;                                             \
    const type *bottom = (type*)_bottom;                                       \
    type *dst = (type*)_dst;                                                   \
    AVExpr *e = param->e;                                                      \
    int y, x;                                                                  \
    dst_linesize /= div;                                                       \
    top_linesize /= div;                                                       \
    bottom_linesize /= div;                                                    \
                                                                               \
    for (y = 0; y < height; y++) {                                             \
        values[VAR_Y] = y + starty;                                            \
        for (x = 0; x < width; x++) {                                          \
            values[VAR_X]      = x;                                            \
            values[VAR_TOP]    = values[VAR_A] = top[x];                       \
            values[VAR_BOTTOM] = values[VAR_B] = bottom[x];                    \
            dst[x] = av_expr_eval(e, values, NULL);                            \
        }                                                                      \
        dst    += dst_linesize;                                                \
        top    += top_linesize;                                                \
        bottom += bottom_linesize;                                             \
    }                                                                          \
}

DEFINE_BLEND_EXPR(uint8_t, 8bit, 1)
DEFINE_BLEND_EXPR(uint16_t, 16bit, 2)
DEFINE_BLEND_EXPR(float, 32bit, 4)

static int filter_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    ThreadData *td = arg;
    int slice_start = (td->h *  jobnr   ) / nb_jobs;
    int slice_end   = (td->h * (jobnr+1)) / nb_jobs;
    int height      = slice_end - slice_start;
    const uint8_t *top    = td->top->data[td->plane];
    const uint8_t *bottom = td->bottom->data[td->plane];
    uint8_t *dst    = td->dst->data[td->plane];
    double values[VAR_VARS_NB];

    values[VAR_N]  = td->inlink->frame_count_out;
    values[VAR_T]  = td->dst->pts == AV_NOPTS_VALUE ? NAN : td->dst->pts * av_q2d(td->inlink->time_base);
    values[VAR_W]  = td->w;
    values[VAR_H]  = td->h;
    values[VAR_SW] = td->w / (double)td->dst->width;
    values[VAR_SH] = td->h / (double)td->dst->height;

    td->param->blend(top + slice_start * td->top->linesize[td->plane],
                     td->top->linesize[td->plane],
                     bottom + slice_start * td->bottom->linesize[td->plane],
                     td->bottom->linesize[td->plane],
                     dst + slice_start * td->dst->linesize[td->plane],
                     td->dst->linesize[td->plane],
                     td->w, height, td->param, &values[0], slice_start);
    return 0;
}

static AVFrame *blend_frame(AVFilterContext *ctx, AVFrame *top_buf,
                            const AVFrame *bottom_buf)
{
    BlendContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *dst_buf;
    int plane;

    dst_buf = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!dst_buf)
        return top_buf;
    av_frame_copy_props(dst_buf, top_buf);

    for (plane = 0; plane < s->nb_planes; plane++) {
        int hsub = plane == 1 || plane == 2 ? s->hsub : 0;
        int vsub = plane == 1 || plane == 2 ? s->vsub : 0;
        int outw = AV_CEIL_RSHIFT(dst_buf->width,  hsub);
        int outh = AV_CEIL_RSHIFT(dst_buf->height, vsub);
        FilterParams *param = &s->params[plane];
        ThreadData td = { .top = top_buf, .bottom = bottom_buf, .dst = dst_buf,
                          .w = outw, .h = outh, .param = param, .plane = plane,
                          .inlink = inlink };

        ff_filter_execute(ctx, filter_slice, &td, NULL,
                          FFMIN(outh, ff_filter_get_nb_threads(ctx)));
    }

    if (!s->tblend)
        av_frame_free(&top_buf);

    return dst_buf;
}

static int blend_frame_for_dualinput(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFrame *top_buf, *bottom_buf, *dst_buf;
    int ret;

    ret = ff_framesync_dualinput_get(fs, &top_buf, &bottom_buf);
    if (ret < 0)
        return ret;
    if (!bottom_buf)
        return ff_filter_frame(ctx->outputs[0], top_buf);
    dst_buf = blend_frame(ctx, top_buf, bottom_buf);
    return ff_filter_frame(ctx->outputs[0], dst_buf);
}

static av_cold int init(AVFilterContext *ctx)
{
    BlendContext *s = ctx->priv;

    s->tblend = !strcmp(ctx->filter->name, "tblend");

    s->fs.on_event = blend_frame_for_dualinput;
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ422P,AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ411P,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRAP, AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
        AV_PIX_FMT_YUVA420P9, AV_PIX_FMT_YUVA422P9, AV_PIX_FMT_YUVA444P9, AV_PIX_FMT_GBRP9,
        AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_YUV440P10,
        AV_PIX_FMT_YUVA420P10, AV_PIX_FMT_YUVA422P10, AV_PIX_FMT_YUVA444P10,
        AV_PIX_FMT_GBRP10, AV_PIX_FMT_GBRAP10, AV_PIX_FMT_GRAY10,
        AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV440P12,
        AV_PIX_FMT_YUVA422P12, AV_PIX_FMT_YUVA444P12,
        AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRAP12, AV_PIX_FMT_GRAY12,
        AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_YUVA420P16, AV_PIX_FMT_YUVA422P16, AV_PIX_FMT_YUVA444P16,
        AV_PIX_FMT_GBRP16, AV_PIX_FMT_GBRAP16, AV_PIX_FMT_GRAY16,
        AV_PIX_FMT_GBRPF32, AV_PIX_FMT_GBRAPF32, AV_PIX_FMT_GRAYF32,
        AV_PIX_FMT_NONE
    };

    return ff_set_common_formats_from_list(ctx, pix_fmts);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    BlendContext *s = ctx->priv;
    int i;

    ff_framesync_uninit(&s->fs);
    av_frame_free(&s->prev_frame);

    for (i = 0; i < FF_ARRAY_ELEMS(s->params); i++)
        av_expr_free(s->params[i].e);
}

#define DEFINE_INIT_BLEND_FUNC(depth, nbits)                                          \
static av_cold void init_blend_func_##depth##_##nbits##bit(FilterParams *param)       \
{                                                                                     \
    switch (param->mode) {                                                            \
    case BLEND_ADDITION:     param->blend = blend_addition_##depth##bit;     break;   \
    case BLEND_GRAINMERGE:   param->blend = blend_grainmerge_##depth##bit;   break;   \
    case BLEND_AND:          param->blend = blend_and_##depth##bit;          break;   \
    case BLEND_AVERAGE:      param->blend = blend_average_##depth##bit;      break;   \
    case BLEND_BURN:         param->blend = blend_burn_##depth##bit;         break;   \
    case BLEND_DARKEN:       param->blend = blend_darken_##depth##bit;       break;   \
    case BLEND_DIFFERENCE:   param->blend = blend_difference_##depth##bit;   break;   \
    case BLEND_GRAINEXTRACT: param->blend = blend_grainextract_##depth##bit; break;   \
    case BLEND_DIVIDE:       param->blend = blend_divide_##depth##bit;       break;   \
    case BLEND_DODGE:        param->blend = blend_dodge_##depth##bit;        break;   \
    case BLEND_EXCLUSION:    param->blend = blend_exclusion_##depth##bit;    break;   \
    case BLEND_EXTREMITY:    param->blend = blend_extremity_##depth##bit;    break;   \
    case BLEND_FREEZE:       param->blend = blend_freeze_##depth##bit;       break;   \
    case BLEND_GLOW:         param->blend = blend_glow_##depth##bit;         break;   \
    case BLEND_HARDLIGHT:    param->blend = blend_hardlight_##depth##bit;    break;   \
    case BLEND_HARDMIX:      param->blend = blend_hardmix_##depth##bit;      break;   \
    case BLEND_HEAT:         param->blend = blend_heat_##depth##bit;         break;   \
    case BLEND_LIGHTEN:      param->blend = blend_lighten_##depth##bit;      break;   \
    case BLEND_LINEARLIGHT:  param->blend = blend_linearlight_##depth##bit;  break;   \
    case BLEND_MULTIPLY:     param->blend = blend_multiply_##depth##bit;     break;   \
    case BLEND_MULTIPLY128:  param->blend = blend_multiply128_##depth##bit;  break;   \
    case BLEND_NEGATION:     param->blend = blend_negation_##depth##bit;     break;   \
    case BLEND_NORMAL:       param->blend = blend_normal_##nbits##bit;       break;   \
    case BLEND_OR:           param->blend = blend_or_##depth##bit;           break;   \
    case BLEND_OVERLAY:      param->blend = blend_overlay_##depth##bit;      break;   \
    case BLEND_PHOENIX:      param->blend = blend_phoenix_##depth##bit;      break;   \
    case BLEND_PINLIGHT:     param->blend = blend_pinlight_##depth##bit;     break;   \
    case BLEND_REFLECT:      param->blend = blend_reflect_##depth##bit;      break;   \
    case BLEND_SCREEN:       param->blend = blend_screen_##depth##bit;       break;   \
    case BLEND_SOFTLIGHT:    param->blend = blend_softlight_##depth##bit;    break;   \
    case BLEND_SUBTRACT:     param->blend = blend_subtract_##depth##bit;     break;   \
    case BLEND_VIVIDLIGHT:   param->blend = blend_vividlight_##depth##bit;   break;   \
    case BLEND_XOR:          param->blend = blend_xor_##depth##bit;          break;   \
    }                                                                                 \
}
DEFINE_INIT_BLEND_FUNC(8, 8)
DEFINE_INIT_BLEND_FUNC(9, 16)
DEFINE_INIT_BLEND_FUNC(10, 16)
DEFINE_INIT_BLEND_FUNC(12, 16)
DEFINE_INIT_BLEND_FUNC(16, 16)
DEFINE_INIT_BLEND_FUNC(32, 32)

void ff_blend_init(FilterParams *param, int depth)
{
    switch (depth) {
    case 8:
        init_blend_func_8_8bit(param);
        break;
    case 9:
        init_blend_func_9_16bit(param);
        break;
    case 10:
        init_blend_func_10_16bit(param);
        break;
    case 12:
        init_blend_func_12_16bit(param);
        break;
    case 16:
        init_blend_func_16_16bit(param);
        break;
    case 32:
        init_blend_func_32_32bit(param);
        break;
    }

    if (param->opacity == 0 && param->mode != BLEND_NORMAL) {
        param->blend = depth > 8 ? depth > 16 ? blend_copytop_32 : blend_copytop_16 : blend_copytop_8;
    } else if (param->mode == BLEND_NORMAL) {
        if (param->opacity == 1)
            param->blend = depth > 8 ? depth > 16 ? blend_copytop_32 : blend_copytop_16 : blend_copytop_8;
        else if (param->opacity == 0)
            param->blend = depth > 8 ? depth > 16 ? blend_copybottom_32 : blend_copybottom_16 : blend_copybottom_8;
    }

    if (ARCH_X86)
        ff_blend_init_x86(param, depth);
}

static int config_params(AVFilterContext *ctx)
{
    BlendContext *s = ctx->priv;
    int ret;

    for (int plane = 0; plane < FF_ARRAY_ELEMS(s->params); plane++) {
        FilterParams *param = &s->params[plane];

        if (s->all_mode >= 0)
            param->mode = s->all_mode;
        if (s->all_opacity < 1)
            param->opacity = s->all_opacity;

        ff_blend_init(param, s->depth);

        if (s->all_expr && !param->expr_str) {
            param->expr_str = av_strdup(s->all_expr);
            if (!param->expr_str)
                return AVERROR(ENOMEM);
        }
        if (param->expr_str) {
            ret = av_expr_parse(&param->e, param->expr_str, var_names,
                                NULL, NULL, NULL, NULL, 0, ctx);
            if (ret < 0)
                return ret;
            param->blend = s->depth > 8 ? s->depth > 16 ? blend_expr_32bit : blend_expr_16bit : blend_expr_8bit;
        }
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *toplink = ctx->inputs[TOP];
    BlendContext *s = ctx->priv;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(toplink->format);
    int ret;

    if (!s->tblend) {
        AVFilterLink *bottomlink = ctx->inputs[BOTTOM];

        if (toplink->w != bottomlink->w || toplink->h != bottomlink->h) {
            av_log(ctx, AV_LOG_ERROR, "First input link %s parameters "
                   "(size %dx%d) do not match the corresponding "
                   "second input link %s parameters (size %dx%d)\n",
                   ctx->input_pads[TOP].name, toplink->w, toplink->h,
                   ctx->input_pads[BOTTOM].name, bottomlink->w, bottomlink->h);
            return AVERROR(EINVAL);
        }
    }

    outlink->w = toplink->w;
    outlink->h = toplink->h;
    outlink->time_base = toplink->time_base;
    outlink->sample_aspect_ratio = toplink->sample_aspect_ratio;
    outlink->frame_rate = toplink->frame_rate;

    s->hsub = pix_desc->log2_chroma_w;
    s->vsub = pix_desc->log2_chroma_h;

    s->depth = pix_desc->comp[0].depth;
    s->nb_planes = av_pix_fmt_count_planes(toplink->format);

    if (!s->tblend)
        if ((ret = ff_framesync_init_dualinput(&s->fs, ctx)) < 0)
            return ret;

    ret = config_params(ctx);
    if (ret < 0)
        return ret;

    if (s->tblend)
        return 0;

    ret = ff_framesync_configure(&s->fs);
    outlink->time_base = s->fs.time_base;

    return ret;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    return config_params(ctx);
}

#if CONFIG_BLEND_FILTER

static int activate(AVFilterContext *ctx)
{
    BlendContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static const AVFilterPad blend_inputs[] = {
    {
        .name          = "top",
        .type          = AVMEDIA_TYPE_VIDEO,
    },{
        .name          = "bottom",
        .type          = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad blend_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

const AVFilter ff_vf_blend = {
    .name          = "blend",
    .description   = NULL_IF_CONFIG_SMALL("Blend two video frames into each other."),
    .preinit       = blend_framesync_preinit,
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(BlendContext),
    .query_formats = query_formats,
    .activate      = activate,
    FILTER_INPUTS(blend_inputs),
    FILTER_OUTPUTS(blend_outputs),
    .priv_class    = &blend_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL | AVFILTER_FLAG_SLICE_THREADS,
    .process_command = process_command,
};

#endif

#if CONFIG_TBLEND_FILTER

static int tblend_filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    BlendContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    if (s->prev_frame) {
        AVFrame *out;

        if (ctx->is_disabled)
            out = av_frame_clone(frame);
        else
            out = blend_frame(ctx, frame, s->prev_frame);
        av_frame_free(&s->prev_frame);
        s->prev_frame = frame;
        return ff_filter_frame(outlink, out);
    }
    s->prev_frame = frame;
    return 0;
}

AVFILTER_DEFINE_CLASS_EXT(tblend, "tblend", blend_options);

static const AVFilterPad tblend_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .filter_frame  = tblend_filter_frame,
    },
};

static const AVFilterPad tblend_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

const AVFilter ff_vf_tblend = {
    .name          = "tblend",
    .description   = NULL_IF_CONFIG_SMALL("Blend successive frames."),
    .priv_size     = sizeof(BlendContext),
    .priv_class    = &tblend_class,
    .query_formats = query_formats,
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(tblend_inputs),
    FILTER_OUTPUTS(tblend_outputs),
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL | AVFILTER_FLAG_SLICE_THREADS,
    .process_command = process_command,
};

#endif
