/*
 * Splash codec
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


#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"

/*
 * Frame header
 */
#define HEADER_LENGTH 12
#define HEADER_OFS_VERSION 9
#define HEADER_OFS_RADIUS 10
#define HEADER_OFS_COMPRESS 11

/**
 * Encoder context
 */
typedef struct SplashContext {
        AVClass class;

        int radius;        // pixel/brush radius
        float ppf;         // pixels per frame (width*height/ppf)
        float ppk;         // pixels per key frame (width*height*ppk)

        uint32_t *xError;  // total error along the x-axis
        uint32_t *yError;  // total error along the y-axis
        uint8_t *pixels;   // pixel data

        uint8_t *data;     // data buffer
        int pos;           // position within data buffer
        int size;          // size data buffer

        int numPixels;
} SplashContext;

static av_cold int splash_init(AVCodecContext *avctx) {
    SplashContext *const ctx = avctx->priv_data;
    int width = avctx->width, height = avctx->height;

    avctx->pix_fmt = AV_PIX_FMT_RGB0;

    // allocate
    ctx->xError = av_malloc(width * 4);
    ctx->yError = av_malloc(height * 4);
    ctx->pixels = av_malloc(width * height * 4);

    // test if all successful
    if (!ctx->xError || !ctx->yError || !ctx->pixels)
        return AVERROR(ENOMEM);

    // initial image, solid gray50
    {
        uint8_t *pixels = ctx->pixels;
        for (int ji = 0; ji < width * height; ji++) {
            *pixels++ = 0x7f;
            *pixels++ = 0x7f;
            *pixels++ = 0x7f;
        }
    }

    return 0;
}

static av_cold int splash_end(AVCodecContext *avctx) {
    SplashContext *const ctx = avctx->priv_data;

    av_freep(&ctx->pixels);
    av_freep(&ctx->yError);
    av_freep(&ctx->xError);

    return 0;
}

static int updateLines(AVCodecContext *avctx, const AVFrame *pic, int radius, int encodeDecode) {

    SplashContext *const ctx = avctx->priv_data;
    int logLevel = av_log_get_level();

    int width = avctx->width;
    int height = avctx->height;
    uint32_t *xError = ctx->xError;
    uint32_t *yError = ctx->yError;
    uint8_t *pixels = ctx->pixels;
    int maxError;

    uint8_t *pData = ctx->data + ctx->pos;

    // which tabstops have the worst error
    int worstXerr = xError[0];
    int worstXi = 0;
    int worstYerr = yError[0];
    int worstYj = 0;

//	 int totalError = 0;

    for (int i = 1; i < width; i++) {
        if (xError[i] > worstXerr) {
            worstXi = i;
            worstXerr = xError[i];
        }
    }
    for (int j = 1; j < height; j++) {
        if (yError[j] > worstYerr) {
            worstYj = j;
            worstYerr = yError[j];
        }
    }


    if (worstXerr + worstYerr == 0)
        return 0; // nothing to do

    if (worstXerr > worstYerr) {
        int i = worstXi;

        /*
         * range of splash
         */
        int minI = i, maxI = i;
        for (int r = 1; r < radius; r++) {
            if (minI == 0 || xError[minI - 1] == 0)
                break;
            --minI;
        }
        for (int r = 1; r < radius; r++) {
            if (maxI >= width - 1 || xError[maxI + 1] == 0)
                break;
            ++maxI;
        }

        if (logLevel >= AV_LOG_TRACE)
            av_log(NULL, AV_LOG_TRACE, "%d %d X-%d %d\n", worstXerr, worstYerr, worstXi, worstYj);

        maxError = xError[i];

        /*
         * Apply changes to the ruler so X and Y are now balanced
         */
        for (int ii = minI; ii <= maxI; ii++) {
            float alpha = (float) FFABS(ii - i) / radius;

            xError[ii] = round(xError[ii] * alpha);
            if (i != ii && xError[ii] == 0) {
//				av_log(NULL, AV_LOG_WARNING, "xError[%d] may not become zero\n", ii);
                xError[ii] = 1;
            }
        }
        xError[i] = 0;

        /*
         * Scan line for cross points
         */
        for (int j = 0; j < height; j++) {
            // only calculate cross points of exact lines, fill the others
            if (yError[j] == 0) {

                int minJ = j, maxJ = j;

                /*
                 * Read pixel
                 */
                int srcR, srcG, srcB;
                if (encodeDecode) {
                    // encode
                    uint8_t *src = &pic->data[0][j * pic->linesize[0]] + i * 4;
                    srcR = *src++;
                    srcG = *src++;
                    srcB = *src;

                    // emit pixel
                    *pData++ = srcR;
                    *pData++ = srcG;
                    *pData++ = srcB;
                    ctx->numPixels++;
                } else {
                    //decode
                    srcR = *pData++;
                    srcG = *pData++;
                    srcB = *pData++;
                }

                /*
                 * range of splash
                 */
                for (int r = 1; r < radius; r++) {
                    if (minJ == 0 || yError[minJ - 1] == 0)
                        break;
                    --minJ;
                }
                for (int r = 1; r < radius; r++) {
                    if (maxJ >= height - 1 || yError[maxJ + 1] == 0)
                        break;
                    ++maxJ;
                }

                /*
                 * Weighted flood-fill cross point
                 */
                for (int jj = minJ; jj <= maxJ; jj++) {
                    for (int ii = minI; ii <= maxI; ii++) {
                        // get fill alpha
                        // the further the fill from the center, the less effect it has
                        float fillAlpha = 1 - sqrt((ii - i) * (ii - i) + (jj - j) * (jj - j)) / radius;

                        if (fillAlpha > 0) {

                            // get pixel alpha
                            // the more accurate the pixel (lower error) the lower the effect of the fill
                            // normally neighbouring pixels have neighbouring errors
                            // this should avoid filling delicate pixels like lines and letters
                            float xerr = (float) ctx->xError[ii] / maxError;
                            float yerr = (float) ctx->yError[jj] / maxError;
                            float xyerr = (xerr + yerr) / 2;

                            int alpha = 256 - round(256 * xyerr);

                            int k = (jj * width + ii) * 4;
                            int oldR = pixels[k + 0];
                            int oldG = pixels[k + 1];
                            int oldB = pixels[k + 2];

                            int newR = ((srcR * alpha) + (oldR * (256 - alpha))) >> 8;
                            int newG = ((srcG * alpha) + (oldG * (256 - alpha))) >> 8;
                            int newB = ((srcB * alpha) + (oldB * (256 - alpha))) >> 8;

//							av_assert0(ctx->xError[i] > 0);
                            assert(alpha < 256);
                            if (i == ii && j == jj)
                                av_assert0(alpha == 256);

                            /*
                            totalError -= Math.abs(srcR - oldR);
                            totalError -= Math.abs(srcG - oldG);
                            totalError -= Math.abs(srcB - oldB);
                            totalError += Math.abs(srcR - newR);
                            totalError += Math.abs(srcG - newG);
                            totalError += Math.abs(srcB - newB);
                             */

                            // save new pixel value
                            pixels[k + 0] = newR;
                            pixels[k + 1] = newG;
                            pixels[k + 2] = newB;
                        }
                    }
                }
            }
        }
    } else {
        int j = worstYj;

        /*
         * range of splash
         */
        int minJ = j, maxJ = j;
        for (int r = 1; r < radius; r++) {
            if (minJ == 0 || yError[minJ - 1] == 0)
                break;
            --minJ;
        }
        for (int r = 1; r < radius; r++) {
            if (maxJ >= height - 1 || yError[maxJ + 1] == 0)
                break;
            ++maxJ;
        }

        if (logLevel >= AV_LOG_TRACE)
            av_log(NULL, AV_LOG_TRACE, "%d %d %d Y-%d\n", worstXerr, worstYerr, worstXi, worstYj);

        maxError = yError[j];

        /*
         * Apply changes to the ruler so X and Y are now balanced
         */
        for (int jj = minJ; jj <= maxJ; jj++) {
            float alpha = (float) FFABS(jj - j) / radius;

            yError[jj] = round(yError[jj] * alpha);
            if (j != jj && yError[jj] == 0) {
//				av_log(NULL, AV_LOG_WARNING, "yError[%d] may not become zero\n", jj);
                yError[jj] = 1;
            }
        }
        yError[j] = 0;

        /*
         * Scan line for cross points
         */
        for (int i = 0; i < width; i++) {
            // only calculate cross points of exact lines, fill the others
            if (xError[i] == 0) {

                int minI = i, maxI = i;

                /*
                 * Read pixel
                 */
                int srcR, srcG, srcB;
                if (encodeDecode) {
                    // encode
                    uint8_t *src = &pic->data[0][j * pic->linesize[0]] + i * 4;
                    srcR = *src++;
                    srcG = *src++;
                    srcB = *src;

                    // emit pixel
                    *pData++ = srcR;
                    *pData++ = srcG;
                    *pData++ = srcB;
                    ctx->numPixels++;
                } else {
                    // decode
                    srcR = *pData++;
                    srcG = *pData++;
                    srcB = *pData++;
                }

                /*
                 * range of splash
                 */
                for (int r = 1; r < radius; r++) {
                    if (minI == 0 || xError[minI - 1] == 0)
                        break;
                    --minI;
                }
                for (int r = 1; r < radius; r++) {
                    if (maxI >= width - 1 || xError[maxI + 1] == 0)
                        break;
                    ++maxI;
                }

                /*
                 * Weighted flood-fill cross point
                 */
                for (int ii = minI; ii <= maxI; ii++) {
                    for (int jj = minJ; jj <= maxJ; jj++) {
                        float fillAlpha = 1 - sqrt((ii - i) * (ii - i) + (jj - j) * (jj - j)) / radius;

                        if (fillAlpha > 0) {
                            /*
                             * fillAlpha is also the distance to the splash center
                             * Dividing it between x and y in an attempt to prolong being selected to being the next scanline
                             *
                             * low `error[]` implies low chance, so change at least as possible
                             * high `error[]` implies already likely. Changing as much as possible might be the only escape.
                             */
                            float xerr = (float) ctx->xError[ii] / maxError;
                            float yerr = (float) ctx->yError[jj] / maxError;
                            float xyerr = (xerr + yerr) / 2;

                            int alpha = 256 - round(256 * xyerr);

                            int k = (jj * width + ii) * 4;
                            int oldR = pixels[k + 0];
                            int oldG = pixels[k + 1];
                            int oldB = pixels[k + 2];

                            int newR = ((srcR * alpha) + (oldR * (256 - alpha))) >> 8;
                            int newG = ((srcG * alpha) + (oldG * (256 - alpha))) >> 8;
                            int newB = ((srcB * alpha) + (oldB * (256 - alpha))) >> 8;

//							av_assert0(ctx->yError[j] > 0);
                            if (i == ii && j == jj)
                                av_assert0(alpha == 256);
                            assert(alpha < 256);

                            /*
                            totalError -= Math.abs(srcR - oldR);
                            totalError -= Math.abs(srcG - oldG);
                            totalError -= Math.abs(srcB - oldB);
                            totalError += Math.abs(srcR - newR);
                            totalError += Math.abs(srcG - newG);
                            totalError += Math.abs(srcB - newB);
                            */

                            // save new pixel value
                            pixels[k + 0] = newR;
                            pixels[k + 1] = newG;
                            pixels[k + 2] = newB;
                        }
                    }
                }
            }
        }
    }

    ctx->pos = pData - ctx->data;

    return 1;
}

