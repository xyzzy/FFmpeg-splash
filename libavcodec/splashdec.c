/*
 * Splash Decoder
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

static int splash_decode(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt) {
    SplashContext *const ctx = avctx->priv_data;
    int width = avctx->width, height = avctx->height;

    AVFrame *pic = data;
    const uint8_t *pData = avpkt->data;
    int ret;
    int hdrLength, radius;

    avctx->pix_fmt = AV_PIX_FMT_RGB0;

    if ((ret = ff_get_buffer(avctx, pic, 0)) < 0)
        return ret;

    /*
     * Load header
     */

    hdrLength = pData[0];
    hdrLength |= pData[1] << 8;
    hdrLength |= pData[2] << 16;
    radius = pData[HEADER_OFS_RADIUS];

    ctx->data = avpkt->data + hdrLength;
    ctx->pos = 0;
    ctx->size = avpkt->size - HEADER_LENGTH;
    pData = ctx->data;

    /*
     * Load initial `xError[]`
     */
    for (int i = 0; i < width; i++) {
        int err = *pData++;
        err |= *pData++ << 8;
        err |= *pData++ << 16;

        ctx->xError[i] = err;
    }

    /*
     * Load initial `yError[]`
     */
    for (int j = 0; j < height; j++) {
        int err = *pData++;
        err |= *pData++ << 8;
        err |= *pData++ << 16;

        ctx->yError[j] = err;
    }

    ctx->pos = pData - ctx->data;

    do {
        if (!updateLines(avctx, pic, radius, 0))
            break; // short frame
    } while (ctx->pos < ctx->size);

    if (ctx->pos != ctx->size)
        av_log(avctx, AV_LOG_WARNING, "Incomplete scan line.\n");

    /*
     * Copy decoded pixels to frame
     */
    pData = ctx->pixels;
    for (int j = 0; j < avctx->height; ++j) {
        uint8_t *rgb = &pic->data[0][j * pic->linesize[0]];
        for (int i = 0; i < avctx->width; ++i) {
            *rgb++ = *pData++;
            *rgb++ = *pData++;
            *rgb++ = *pData++;
            *rgb++ = 255;
            pData++;
        }
    }

    pic->key_frame = 1;
    pic->pict_type = AV_PICTURE_TYPE_I;

    *got_frame = 1;

    return avpkt->size;
}


AVCodec ff_splash_decoder = {
        .name           = "splash",
        .long_name = NULL_IF_CONFIG_SMALL("Splash"),
        .type           = AVMEDIA_TYPE_VIDEO,
        .id             = AV_CODEC_ID_SPLASH,
        .priv_data_size        = sizeof(SplashContext),
        .init           = splash_init,
        .decode         = splash_decode,
        .close          = splash_end,
        .capabilities = AV_CODEC_CAP_DR1,
        .pix_fmts = (const enum AVPixelFormat[]) {AV_PIX_FMT_RGB0, AV_PIX_FMT_NONE},
};
