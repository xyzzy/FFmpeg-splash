/*
 * Splash Encoder
 * Copyright (c) 2020 xyzzy@rockingship.org
 *
 * This file is part of FFmpeg.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * @file
 * jsFractalZoom based codec.
 *
 * Project page and background:
 * https://github.com/RockingShip/jsFractalZoom
 */

#include "avcodec.h"
#include "internal.h"
#include "splash.h"

static int splash_encode(AVCodecContext *avctx, AVPacket *pkt, const AVFrame *pic, int *got_packet) {
    SplashContext *const ctx = avctx->priv_data;
    int width = avctx->width, height = avctx->height;
    int ret;
    uint8_t *pixels = ctx->pixels;
    uint8_t *pData;

    // allocate worst-case frame
    if ((ret = ff_alloc_packet2(avctx, pkt, (HEADER_LENGTH + width + height + width * height) * 3, 0)) < 0)
        return ret;

    /*
     * Write header
     * +0 length (lsb)
     * +3 "spl"
     * +6 "ash"
     * +9 version
     * +10 radius
     * +11 zero
     * +12 start of data
     */
    pData = pkt->data;
    *pData++ = 12; // 3 bytes header length (lsb)
    *pData++ = 0;
    *pData++ = 0;
    *pData++ = 's'; // magic
    *pData++ = 'p';
    *pData++ = 'l';
    *pData++ = 'a';
    *pData++ = 's';
    *pData++ = 'h';
    *pData++ = 1; // version 1
    *pData++ = ctx->radius; // radius
    *pData++ = 0; // reserved for compression

    ctx->data = pkt->data + HEADER_LENGTH;
    ctx->pos = 0;
    ctx->size = pkt->size - HEADER_LENGTH;
    pData = ctx->data;

    /*
     * Create and output initial `xError[]`
     */
    for (int i = 0; i < width; i++) {
        int err = 0;
        for (int j = 0; j < height; j++) {
            int k = (j * width + i) * 4;
            uint8_t *rgb = &pic->data[0][j * pic->linesize[0]] + i * 4;

            err += FFABS(pixels[k + 0] - rgb[0]);
            err += FFABS(pixels[k + 1] - rgb[1]);
            err += FFABS(pixels[k + 2] - rgb[2]);
        }

        // keep within limits
        if (err > 0xffffff)
            err = 0xffffff;

        ctx->xError[i] = err;

        // output xError
        *pData++ = err;
        *pData++ = err >> 8;
        *pData++ = err >> 16;
    }

    /*
     * Create and output initial `yError[]`
     */
    for (int j = 0; j < height; j++) {
        int err = 0;
        for (int i = 0; i < width; i++) {
            int k = (j * width + i) * 4;
            uint8_t *rgb = &pic->data[0][j * pic->linesize[0]] + i * 4;

            err += FFABS(pixels[k + 0] - rgb[0]);
            err += FFABS(pixels[k + 1] - rgb[1]);
            err += FFABS(pixels[k + 2] - rgb[2]);
        }

        // keep within limits
        if (err > 0xffffff)
            err = 0xffffff;

        ctx->yError[j] = err;

        // output yError
        *pData++ = err;
        *pData++ = err >> 8;
        *pData++ = err >> 16;
    }

    /*
     * Start scanning lines
     */
    ctx->numPixels = 0;
    ctx->pos = pData - ctx->data;

    {
        // number of pixels for this frame
        int maxPixels;
        if (avctx->frame_number == 0)
            maxPixels = round(width * height / ctx->ppk);
        else
            maxPixels = round(width * height / ctx->ppf);

        do {
            if (!updateLines(avctx, pic, ctx->radius, 1))
                break;  // short frame
        } while (ctx->numPixels < maxPixels);
    }

    /*
     * Test if end frame matches
     */
    if (ctx->ppf == 1) {
        int cntMiss = 0;
        uint8_t *src = ctx->pixels;
        for (int j = 0; j < avctx->height; ++j) {
            uint8_t *rgb = &pic->data[0][j * pic->linesize[0]];
            for (int i = 0; i < avctx->width; ++i) {
                if (*rgb++ != *src++)
                    cntMiss++;
                if (*rgb++ != *src++)
                    cntMiss++;
                if (*rgb++ != *src++)
                    cntMiss++;
                rgb++; // skip alpha
                src++;
            }
        }
        if (cntMiss)
            av_log(NULL, AV_LOG_WARNING, "Inaccurate %d final pixels\n", cntMiss);
    }

    pkt->size = HEADER_LENGTH + ctx->pos;
    pkt->flags |= AV_PKT_FLAG_KEY;
    *got_packet = 1;
    return 0;
}

static const AVOption options[] = {
        {"ppf",    "pixels per frame (width*height/ppf)",     offsetof(SplashContext, ppf),    AV_OPT_TYPE_FLOAT, {.dbl = 1}, 1, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM},
        {"ppk",    "pixels per key frame (width*height/ppk)", offsetof(SplashContext, ppk),    AV_OPT_TYPE_FLOAT, {.dbl = 2}, 1, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM},
        {"radius", "brush radius",                            offsetof(SplashContext, radius), AV_OPT_TYPE_INT,   {.i64 = 5}, 1, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM},
        {NULL},
};

static const AVClass splash_encoder_class = {
        .class_name = "rasc decoder",
        .item_name  = av_default_item_name,
        .option     = options,
        .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_splash_encoder = {
        .name           = "splash",
        .long_name = NULL_IF_CONFIG_SMALL("Splash"),
        .type           = AVMEDIA_TYPE_VIDEO,
        .id             = AV_CODEC_ID_SPLASH,
        .priv_data_size        = sizeof(SplashContext),
        .init           = splash_init,
        .encode2        = splash_encode,
        .close          = splash_end,
        .pix_fmts = (const enum AVPixelFormat[]) {AV_PIX_FMT_RGB0, AV_PIX_FMT_NONE},
        .priv_class        = &splash_encoder_class,
};

