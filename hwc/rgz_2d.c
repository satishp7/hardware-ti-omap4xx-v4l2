/*
 * Copyright (C) Texas Instruments - http://www.ti.com/
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <strings.h>
#include <dlfcn.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/bltsville.h>
#include <video/dsscomp.h>
#include <video/omap_hwc.h>

#ifndef RGZ_TEST_INTEGRATION
#include <cutils/log.h>
#include <cutils/properties.h>
#include <hardware/hwcomposer.h>
#include "hal_public.h"
#else
#include "hwcomposer.h"
#include "buffer_handle.h"
#define ALIGN(x,a) (((x) + (a) - 1L) & ~((a) - 1L))
#define HW_ALIGN   32
#endif

#include "rgz_2d.h"

#ifdef RGZ_TEST_INTEGRATION
extern void BVDump(const char* prefix, const char* tab, const struct bvbltparams* parms);
#define BVDUMP(p,t,parms) BVDump(p, t, parms)
#define HANDLE_TO_BUFFER(h) handle_to_buffer(h)
#define HANDLE_TO_STRIDE(h) handle_to_stride(h)
#else
static int rgz_handle_to_stride(IMG_native_handle_t *h);
#define BVDUMP(p,t,parms)
#define HANDLE_TO_BUFFER(h) NULL
/* Needs to be meaningful for TILER & GFX buffers and NV12 */
#define HANDLE_TO_STRIDE(h) rgz_handle_to_stride(h)
#endif
#define DSTSTRIDE(dstgeom) dstgeom->virtstride

/* Borrowed macros from hwc.c vvv - consider sharing later */
#define min(a, b) ( { typeof(a) __a = (a), __b = (b); __a < __b ? __a : __b; } )
#define max(a, b) ( { typeof(a) __a = (a), __b = (b); __a > __b ? __a : __b; } )
#define swap(a, b) do { typeof(a) __a = (a); (a) = (b); (b) = __a; } while (0)

#define WIDTH(rect) ((rect).right - (rect).left)
#define HEIGHT(rect) ((rect).bottom - (rect).top)

#define is_RGB(format) ((format) == HAL_PIXEL_FORMAT_BGRA_8888 || (format) == HAL_PIXEL_FORMAT_RGB_565 || (format) == HAL_PIXEL_FORMAT_BGRX_8888)
#define is_BGR(format) ((format) == HAL_PIXEL_FORMAT_RGBX_8888 || (format) == HAL_PIXEL_FORMAT_RGBA_8888)
#define is_NV12(format) ((format) == HAL_PIXEL_FORMAT_TI_NV12 || (format) == HAL_PIXEL_FORMAT_TI_NV12_PADDED)

#define HAL_PIXEL_FORMAT_BGRX_8888 0x1FF
#define HAL_PIXEL_FORMAT_TI_NV12 0x100
#define HAL_PIXEL_FORMAT_TI_NV12_PADDED 0x101
/* Borrowed macros from hwc.c ^^^ */
#define is_OPAQUE(format) ((format) == HAL_PIXEL_FORMAT_RGB_565 || (format) == HAL_PIXEL_FORMAT_RGBX_8888 || (format) == HAL_PIXEL_FORMAT_BGRX_8888)

/* OUTP the means for grabbing diagnostic data */
#ifndef RGZ_TEST_INTEGRATION
#define OUTP LOGI
#define OUTE LOGE
#else
#define OUTP(...) { printf(__VA_ARGS__); printf("\n"); fflush(stdout); }
#define OUTE OUTP
#define LOGD_IF(debug, ...) { if (debug) OUTP(__VA_ARGS__); }
#endif

#define IS_BVCMD(params) (params->op == RGZ_OUT_BVCMD_REGION || params->op == RGZ_OUT_BVCMD_PAINT)

/* Number of framebuffers to track */
#define RGZ_NUM_FB 2

struct rgz_blts {
    struct rgz_blt_entry bvcmds[RGZ_MAX_BLITS];
    int idx;
};


static int rgz_hwc_layer_blit(hwc_layer_t *l, rgz_out_params_t *params, int buff_idx);
static void rgz_blts_init(struct rgz_blts *blts);
static void rgz_blts_free(struct rgz_blts *blts);
static struct rgz_blt_entry* rgz_blts_get(struct rgz_blts *blts, rgz_out_params_t *params);
static int rgz_blts_bvdirect(rgz_t* rgz, struct rgz_blts *blts, rgz_out_params_t *params);
static void rgz_get_src_rect(hwc_layer_t* layer, blit_rect_t *subregion_rect, blit_rect_t *res_rect);

int debug = 0;
struct rgz_blts blts;
/* Represents a screen sized background layer */
static hwc_layer_t bg_layer;

static void svgout_header(int htmlw, int htmlh, int coordw, int coordh)
{
    OUTP("<svg xmlns=\"http://www.w3.org/2000/svg\""
         "width=\"%d\" height=\"%d\""
         "viewBox=\"0 0 %d %d\">",
        htmlw, htmlh, coordw, coordh);
}

static void svgout_footer(void)
{
    OUTP("</svg>");
}

static void svgout_rect(blit_rect_t *r, char *color, char *text)
{
    OUTP("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"%s\" "
         "fill-opacity=\"%f\" stroke=\"black\" stroke-width=\"1\" />",
         r->left, r->top, r->right - r->left, r->bottom - r->top, color, 1.0f);

    if (!text)
        return;

    OUTP("<text x=\"%d\" y=\"%d\" style=\"font-size:30\" fill=\"black\">%s"
         "</text>",
         r->left, r->top + 40, text);
}

static int empty_rect(blit_rect_t *r)
{
    return !r->left && !r->top && !r->right && !r->bottom;
}

static int get_top_rect(blit_hregion_t *hregion, int subregion, blit_rect_t **routp)
{
    int l = hregion->nlayers - 1;
    do {
        *routp = &hregion->blitrects[l][subregion];
        if (!empty_rect(*routp))
            break;
    }
    while (--l >= 0);
    return l;
}

/*
 * The idea here is that we walk the layers from front to back and count the
 * number of layers in the hregion until the first layer which doesn't require
 * blending.
 */
static int get_layer_ops(blit_hregion_t *hregion, int subregion, int *bottom)
{
    int l = hregion->nlayers - 1;
    int ops = 0;
    *bottom = -1;
    do {
        if (!empty_rect(&hregion->blitrects[l][subregion])) {
            ops++;
            *bottom = l;
            hwc_layer_t *layer = hregion->rgz_layers[l]->hwc_layer;
            IMG_native_handle_t *h = (IMG_native_handle_t *)layer->handle;
            if ((layer->blending != HWC_BLENDING_PREMULT) || is_OPAQUE(h->iFormat))
                break;
        }
    }
    while (--l >= 0);
    return ops;
}

static int get_layer_ops_next(blit_hregion_t *hregion, int subregion, int l)
{
    while (++l < hregion->nlayers) {
        if (!empty_rect(&hregion->blitrects[l][subregion]))
            return l;
    }
    return -1;
}

static int svgout_intersects_display(blit_rect_t *a, int dispw, int disph)
{
    return ((a->bottom > 0) && (a->top < disph) &&
            (a->right > 0) && (a->left < dispw));
}

static void svgout_hregion(blit_hregion_t *hregion, int dispw, int disph)
{
    char *colors[] = {"red", "orange", "yellow", "green", "blue", "indigo", "violet", NULL};
    int b;
    for (b = 0; b < hregion->nsubregions; b++) {
        blit_rect_t *rect;
        (void)get_top_rect(hregion, b, &rect);
        /* Only generate SVG for subregions intersecting the displayed area */
        if (!svgout_intersects_display(rect, dispw, disph))
            continue;
        svgout_rect(rect, colors[b % 7], NULL);
    }
}

static void rgz_out_svg(rgz_t *rgz, rgz_out_params_t *params)
{
    if (!rgz || !(rgz->state & RGZ_REGION_DATA)) {
        OUTE("rgz_out_svg invoked with bad state");
        return;
    }
    blit_hregion_t *hregions = rgz->hregions;
    svgout_header(params->data.svg.htmlw, params->data.svg.htmlh,
                  params->data.svg.dispw, params->data.svg.disph);
    int i;
    for (i = 0; i < rgz->nhregions; i++) {

        OUTP("<!-- hregion %d (subcount %d)-->", i, hregions[i].nsubregions);
        svgout_hregion(&hregions[i], params->data.svg.dispw,
                       params->data.svg.disph);
    }
    svgout_footer();
}

/* XXX duplicate of hwc.c version */
static void dump_layer(hwc_layer_t const* l, int iserr)
{
#define FMT(f) ((f) == HAL_PIXEL_FORMAT_TI_NV12 ? "NV12" : \
                (f) == HAL_PIXEL_FORMAT_BGRX_8888 ? "xRGB32" : \
                (f) == HAL_PIXEL_FORMAT_RGBX_8888 ? "xBGR32" : \
                (f) == HAL_PIXEL_FORMAT_BGRA_8888 ? "ARGB32" : \
                (f) == HAL_PIXEL_FORMAT_RGBA_8888 ? "ABGR32" : \
                (f) == HAL_PIXEL_FORMAT_RGB_565 ? "RGB565" : "??")

    OUTE("%stype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}",
            iserr ? ">>  " : "    ",
            l->compositionType, l->flags, l->handle, l->transform, l->blending,
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom);
    if (l->handle) {
        IMG_native_handle_t *h = (IMG_native_handle_t *)l->handle;
        OUTE("%s%d*%d(%s)",
            iserr ? ">>  " : "    ",
            h->iWidth, h->iHeight, FMT(h->iFormat));
        OUTE("hndl %p", l->handle);
    }
}

static void dump_all(rgz_layer_t *rgz_layers, unsigned int layerno, unsigned int errlayer)
{
    unsigned int i;
    for (i = 0; i < layerno; i++) {
        hwc_layer_t *l = rgz_layers[i].hwc_layer;
        OUTE("Layer %d", i);
        dump_layer(l, errlayer == i);
    }
}

static int rgz_out_bvdirect_paint(rgz_t *rgz, rgz_out_params_t *params)
{
    int rv = 0;
    unsigned int i;
    (void)rgz;

    rgz_blts_init(&blts);

    /* Begin from index 1 to remove the background layer from the output */
    for (i = 1; i < rgz->rgz_layerno; i++) {
        hwc_layer_t *l = rgz->rgz_layers[i].hwc_layer;

        rv = rgz_hwc_layer_blit(l, params, -1);
        if (rv) {
            OUTE("bvdirect_paint: error in layer %d: %d", i, rv);
            dump_all(rgz->rgz_layers, rgz->rgz_layerno, i);
            rgz_blts_free(&blts);
            return rv;
        }
    }
    rgz_blts_bvdirect(rgz, &blts, params);
    rgz_blts_free(&blts);
    return rv;
}

static void rgz_set_async(struct rgz_blt_entry *e, int async)
{
    e->bp.flags = async ? e->bp.flags | BVFLAG_ASYNC : e->bp.flags & ~BVFLAG_ASYNC;
}

/*
 * Clear the destination buffer, if rect is NULL means the whole screen, rect
 * cannot be outside the boundaries of the screen
 */
static void rgz_out_clrdst(rgz_out_params_t *params, blit_rect_t *rect)
{
    struct bvsurfgeom *scrgeom = params->data.bvc.dstgeom;

    struct rgz_blt_entry* e;
    e = rgz_blts_get(&blts, params);

    struct bvbuffdesc *src1desc = &e->src1desc;
    src1desc->structsize = sizeof(struct bvbuffdesc);
    src1desc->length = 4;
    /*
     * With the HWC we don't bother having a buffer for the fill we'll get the
     * OMAPLFB to fixup the src1desc if this address is -1
     */
    src1desc->auxptr = (void*)-1;
    struct bvsurfgeom *src1geom = &e->src1geom;
    src1geom->structsize = sizeof(struct bvsurfgeom);
    src1geom->format = OCDFMT_RGBA24;
    src1geom->width = src1geom->height = 1;
    src1geom->orientation = 0;
    src1geom->virtstride = 1;

    struct bvsurfgeom *dstgeom = &e->dstgeom;
    dstgeom->structsize = sizeof(struct bvsurfgeom);
    dstgeom->format = scrgeom->format;
    dstgeom->width = scrgeom->width;
    dstgeom->height = scrgeom->height;
    dstgeom->orientation = 0; /* TODO */
    dstgeom->virtstride = DSTSTRIDE(scrgeom);

    struct bvbltparams *bp = &e->bp;
    bp->structsize = sizeof(struct bvbltparams);
    bp->dstgeom = dstgeom;
    bp->src1.desc = src1desc;
    bp->src1geom = src1geom;
    bp->src1rect.left = 0;
    bp->src1rect.top = 0;
    bp->src1rect.width = bp->src1rect.height = 1;
    bp->cliprect.left = bp->dstrect.left = rect ? rect->left : 0;
    bp->cliprect.top = bp->dstrect.top = rect ? rect->top : 0;
    bp->cliprect.width = bp->dstrect.width = rect ? (unsigned int) WIDTH(*rect) : scrgeom->width;
    bp->cliprect.height = bp->dstrect.height = rect ? (unsigned int) HEIGHT(*rect) : scrgeom->height;

    bp->flags = BVFLAG_CLIP | BVFLAG_ROP;
    bp->op.rop = 0xCCCC; /* SRCCOPY */
    rgz_set_async(e, 1);
}

static int rgz_out_bvcmd_paint(rgz_t *rgz, rgz_out_params_t *params)
{
    int rv = 0;
    params->data.bvc.out_blits = 0;
    params->data.bvc.out_nhndls = 0;
    rgz_blts_init(&blts);
    rgz_out_clrdst(params, NULL);

    unsigned int i, j;

    /* Begin from index 1 to remove the background layer from the output */
    for (i = 1, j = 0; i < rgz->rgz_layerno; i++) {
        rgz_layer_t *rgz_layer = &rgz->rgz_layers[i];
        hwc_layer_t *l = rgz_layer->hwc_layer;

        //OUTP("blitting meminfo %d", rgz->rgz_layers[i].buffidx);

        /*
         * See if it is needed to put transparent pixels where this layer
         * is located in the screen
         */
        if (rgz_layer->buffidx == -1) {
            struct bvsurfgeom *scrgeom = params->data.bvc.dstgeom;
            blit_rect_t srcregion;
            srcregion.left = max(0, l->displayFrame.left);
            srcregion.top = max(0, l->displayFrame.top);
            srcregion.bottom = min(scrgeom->height, l->displayFrame.bottom);
            srcregion.right = min(scrgeom->width, l->displayFrame.right);
            rgz_out_clrdst(params, &srcregion);
            continue;
        }

        rv = rgz_hwc_layer_blit(l, params, rgz->rgz_layers[i].buffidx);
        if (rv) {
            OUTE("bvcmd_paint: error in layer %d: %d", i, rv);
            dump_all(rgz->rgz_layers, rgz->rgz_layerno, i);
            rgz_blts_free(&blts);
            return rv;
        }
        params->data.bvc.out_hndls[j++] = l->handle;
        params->data.bvc.out_nhndls++;
    }

    /* Last blit is made sync to act like a fence for the previous async blits */
    struct rgz_blt_entry* e = &blts.bvcmds[blts.idx-1];
    rgz_set_async(e, 0);

    /* FIXME: we want to be able to call rgz_blts_free and populate the actual
     * composition data structure ourselves */
    params->data.bvc.cmdp = blts.bvcmds;
    params->data.bvc.cmdlen = blts.idx;

    if (params->data.bvc.out_blits >= RGZ_MAX_BLITS) {
        rv = -1;
    // rgz_blts_free(&blts); // FIXME
    }
    return rv;
}

static float getscalew(hwc_layer_t *layer)
{
    int w = WIDTH(layer->sourceCrop);
    int h = HEIGHT(layer->sourceCrop);

    if (layer->transform & HWC_TRANSFORM_ROT_90)
        swap(w, h);

    return ((float)WIDTH(layer->displayFrame)) / (float)w;
}

static float getscaleh(hwc_layer_t *layer)
{
    int w = WIDTH(layer->sourceCrop);
    int h = HEIGHT(layer->sourceCrop);

    if (layer->transform & HWC_TRANSFORM_ROT_90)
        swap(w, h);

    return ((float)HEIGHT(layer->displayFrame)) / (float)h;
}

static int rgz_bswap(int *a, int *b)
{
    if (*a > *b) {
        int tmp = *b;
        *b = *a;
        *a = tmp;
        return 1;
    }
    return 0;
}

/*
 * Simple bubble sort on an array
 */
static void rgz_bsort(int *a, int len)
{
    int i, s;

    do {
        s=0;
        for (i=0; i+1<len; i++) {
            if (rgz_bswap(&a[i], &a[i+1]))
                s = 1;
        }
    } while (s);
}

/*
 * Leave only unique numbers in a sorted array
 */
static int rgz_bunique(int *a, int len)
{
    int unique = 1;
    int base = 0;
    while (base + 1 < len) {
        if (a[base] == a[base + 1]) {
            int skip = 1;
            while (base + skip < len && a[base] == a[base + skip])
                skip++;
            if (base + skip == len)
                break;
            int i;
            for (i = 0; i < skip - 1; i++)
                a[base + 1 + i] = a[base + skip];
        }
        unique++;
        base++;
    }
    return unique;
}

static int rgz_hwc_layer_sortbyy(rgz_layer_t *ra, int rsz, int *out, int *width, int screen_height)
{
    int outsz = 0;
    int i;
    *width = 0;
    for (i = 0; i < rsz; i++) {
        hwc_layer_t *layer = ra[i].hwc_layer;
        /* Maintain regions inside display boundaries */
        int top = layer->displayFrame.top;
        int bottom = layer->displayFrame.bottom;
        out[outsz++] = max(0, top);
        out[outsz++] = min(bottom, screen_height);
        int right = layer->displayFrame.right;
        *width = *width > right ? *width : right;
    }
    rgz_bsort(out, outsz);
    return outsz;
}

static int rgz_hwc_intersects(blit_rect_t *a, hwc_rect_t *b)
{
    return ((a->bottom > b->top) && (a->top < b->bottom) &&
            (a->right > b->left) && (a->left < b->right));
}

static void rgz_gen_blitregions(blit_hregion_t *hregion, int screen_width)
{
/*
 * 1. Get the offsets (left/right positions) of each layer within the
 *    hregion. Assume that layers describe the bounds of the hregion.
 * 2. We should then be able to generate an array of rects
 * 3. Each layer will have a different z-order, for each z-order
 *    find the intersection. Some intersections will be empty.
 */

    int offsets[RGZ_SUBREGIONMAX];
    int noffsets=0;
    int l, r;
    for (l = 0; l < hregion->nlayers; l++) {
        hwc_layer_t *layer = hregion->rgz_layers[l]->hwc_layer;
        /* Make sure the subregion is not outside the boundaries of the screen */
        int left = layer->displayFrame.left;
        int right = layer->displayFrame.right;
        offsets[noffsets++] = max(0, left);
        offsets[noffsets++] = min(right, screen_width);
    }
    rgz_bsort(offsets, noffsets);
    noffsets = rgz_bunique(offsets, noffsets);
    hregion->nsubregions = noffsets - 1;
    bzero(hregion->blitrects, sizeof(hregion->blitrects));
    for (r = 0; r + 1 < noffsets; r++) {
        blit_rect_t subregion;
        subregion.top = hregion->rect.top;
        subregion.bottom = hregion->rect.bottom;
        subregion.left = offsets[r];
        subregion.right = offsets[r+1];

        LOGD_IF(debug, "                sub l %d r %d",
            subregion.left, subregion.right);
        for (l = 0; l < hregion->nlayers; l++) {
            hwc_layer_t *layer = hregion->rgz_layers[l]->hwc_layer;
            if (rgz_hwc_intersects(&subregion, &layer->displayFrame)) {

                hregion->blitrects[l][r] = subregion;

                LOGD_IF(debug, "hregion->blitrects[%d][%d] (%d %d %d %d)", l, r,
                        hregion->blitrects[l][r].left,
                        hregion->blitrects[l][r].top,
                        hregion->blitrects[l][r].right,
                        hregion->blitrects[l][r].bottom);
            }
        }
    }
}

static int rgz_hwc_scaled(hwc_layer_t *layer)
{
    int w = WIDTH(layer->sourceCrop);
    int h = HEIGHT(layer->sourceCrop);

    if (layer->transform & HWC_TRANSFORM_ROT_90)
        swap(w, h);

    return WIDTH(layer->displayFrame) != w || HEIGHT(layer->displayFrame) != h;
}

static int rgz_in_valid_hwc_layer(hwc_layer_t *layer)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;
    if ((layer->flags & HWC_SKIP_LAYER) || !handle)
        return 0;

    if (rgz_hwc_scaled(layer))
        return 0;

    if (is_NV12(handle->iFormat))
        return (!layer->transform && handle->iFormat == HAL_PIXEL_FORMAT_TI_NV12);

    /* FIXME: The following must be removed when GC supports vertical/horizontal
     * buffer flips, please note having a FLIP_H and FLIP_V means 180 rotation
     * which is supported indeed
     */
    if (layer->transform) {
        int is_flipped = !!(layer->transform & HWC_TRANSFORM_FLIP_H) ^ !!(layer->transform & HWC_TRANSFORM_FLIP_V);
        if (is_flipped) {
            LOGE("Layer %p is flipped %d", layer, layer->transform);
            return 0;
        }
    }

    switch(handle->iFormat) {
    case HAL_PIXEL_FORMAT_BGRX_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_RGB_565:
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
        break;
    default:
        return 0;
    }
    return 1;
}

/* Reset dirty region data and state */
static void rgz_delete_region_data(rgz_t *rgz){
    if (!rgz)
        return;
    if (rgz->hregions)
        free(rgz->hregions);
    rgz->hregions = NULL;
    rgz->nhregions = 0;
    rgz->state &= ~RGZ_REGION_DATA;
}

static void rgz_handle_dirty_region(rgz_t *rgz, int reset_counters)
{
    unsigned int i;
    for (i = 0; i < rgz->rgz_layerno; i++) {
        rgz_layer_t *rgz_layer = &rgz->rgz_layers[i];
        void *new_handle;

        /*
         * We don't care about the handle for background and layers with the
         * clear fb hint, but we want to maintain a layer state for dirty
         * region handling.
         */
        if (i == 0 || rgz_layer->buffidx == -1)
            new_handle = (void*)0x1;
        else
            new_handle = (void*)rgz_layer->hwc_layer->handle;

        if (reset_counters || new_handle != rgz_layer->dirty_hndl) {
            rgz_layer->dirty_count = RGZ_NUM_FB;
            rgz_layer->dirty_hndl = new_handle;
        } else
            rgz_layer->dirty_count -= rgz_layer->dirty_count ? 1 : 0;

    }
}

static int rgz_in_hwccheck(rgz_in_params_t *p, rgz_t *rgz)
{
    hwc_layer_t *layers = p->data.hwc.layers;
    int layerno = p->data.hwc.layerno;

    rgz->state &= ~RGZ_STATE_INIT;

    if (!layers)
        return -1;

    /* For debugging */
    //dump_all(layers, layerno, 0);

    /*
     * Store buffer index to be sent in the HWC Post2 list. Any overlay
     * meminfos must come first
     */
    int l, memidx = 0;
    for (l = 0; l < layerno; l++) {
        /*
         * Workaround: If a NV12 layer is present in the list, don't even try
         * to blit. There is a performance degradation while playing video and
         * using GC at the same time.
         */
        IMG_native_handle_t *handle = (IMG_native_handle_t *)layers[l].handle;
        if (!(layers[l].flags & HWC_SKIP_LAYER) && handle && is_NV12(handle->iFormat))
            return -1;

        if (layers[l].compositionType == HWC_OVERLAY)
            memidx++;
    }

    int possible_blit = 0, candidates = 0;

    /*
     * Insert the background layer at the beginning of the list, maintain a
     * state for dirty region handling
     */
    rgz_layer_t *rgz_layer = &rgz->rgz_layers[0];
    rgz_layer->hwc_layer = &bg_layer;

    for (l = 0; l < layerno; l++) {
        if (layers[l].compositionType == HWC_FRAMEBUFFER) {
            candidates++;
            if (rgz_in_valid_hwc_layer(&layers[l]) &&
                    possible_blit < RGZ_INPUT_MAXLAYERS) {
                rgz_layer_t *rgz_layer = &rgz->rgz_layers[possible_blit+1];
                rgz_layer->hwc_layer = &layers[l];
                rgz_layer->buffidx = memidx++;
                possible_blit++;
            }
            continue;
        }

        if (layers[l].hints & HWC_HINT_CLEAR_FB) {
            candidates++;
            if (possible_blit < RGZ_INPUT_MAXLAYERS) {
                /*
                 * Use only the layer rectangle as an input to regionize when the clear
                 * fb hint is present, mark this layer to identify it.
                 */
                rgz_layer_t *rgz_layer = &rgz->rgz_layers[possible_blit+1];
                rgz_layer->buffidx = -1;
                rgz_layer->hwc_layer = &layers[l];
                possible_blit++;
            }
        }
    }

    if (!possible_blit || possible_blit != candidates) {
        return -1;
    }

    unsigned int blit_layers = possible_blit + 1; /* Account for background layer */
    int reset_dirty_counters = rgz->rgz_layerno != blit_layers ? 1 : 0;
    /*
     * The layers we are going to blit differ in number from the previous frame,
     * we can't trust anymore the region data, calculate it again
     */
    if (reset_dirty_counters)
        rgz_delete_region_data(rgz);

    rgz->state |= RGZ_STATE_INIT;
    rgz->rgz_layerno = blit_layers;

    rgz_handle_dirty_region(rgz, reset_dirty_counters);

    return RGZ_ALL;
}

static int rgz_in_hwc(rgz_in_params_t *p, rgz_t *rgz)
{
    int yentries[RGZ_SUBREGIONMAX];
    int dispw;  /* widest layer */
    int screen_width = p->data.hwc.dstgeom->width;
    int screen_height = p->data.hwc.dstgeom->height;

    if (!(rgz->state & RGZ_STATE_INIT)) {
        OUTE("rgz_process started with bad state");
        return -1;
    }

    /* If there is already region data avoid parsing it again */
    if (rgz->state & RGZ_REGION_DATA) {
        return 0;
    }

    int layerno = rgz->rgz_layerno;

    /* Find the horizontal regions */
    rgz_layer_t *rgz_layers = rgz->rgz_layers;
    int ylen = rgz_hwc_layer_sortbyy(rgz_layers, layerno, yentries, &dispw, screen_height);

    ylen = rgz_bunique(yentries, ylen);

    /* at this point we have an array of horizontal regions */
    rgz->nhregions = ylen - 1;

    blit_hregion_t *hregions = calloc(rgz->nhregions, sizeof(blit_hregion_t));
    if (!hregions) {
        OUTE("Unable to allocate memory for hregions");
        return -1;
    }
    rgz->hregions = hregions;

    LOGD_IF(debug, "Allocated %d regions (sz = %d), layerno = %d", rgz->nhregions, rgz->nhregions * sizeof(blit_hregion_t), layerno);
    int i, j;
    for (i = 0; i < rgz->nhregions; i++) {
        hregions[i].rect.top = yentries[i];
        hregions[i].rect.bottom = yentries[i+1];
        /* Avoid hregions outside the display boundaries */
        hregions[i].rect.left = 0;
        hregions[i].rect.right = dispw > screen_width ? screen_width : dispw;
        hregions[i].nlayers = 0;
        for (j = 0; j < layerno; j++) {
            hwc_layer_t *layer = rgz_layers[j].hwc_layer;
            if (rgz_hwc_intersects(&hregions[i].rect, &layer->displayFrame)) {
                int l = hregions[i].nlayers++;
                hregions[i].rgz_layers[l] = &rgz_layers[j];
            }
        }
    }

    /* Calculate blit regions */
    for (i = 0; i < rgz->nhregions; i++) {
        rgz_gen_blitregions(&hregions[i], screen_width);
        LOGD_IF(debug, "hregion %3d: nsubregions %d", i, hregions[i].nsubregions);
        LOGD_IF(debug, "           : %d to %d: ",
            hregions[i].rect.top, hregions[i].rect.bottom);
        for (j = 0; j < hregions[i].nlayers; j++)
            LOGD_IF(debug, "              %p ", hregions[i].rgz_layers[j]->hwc_layer);
    }
    rgz->state |= RGZ_REGION_DATA;
    return 0;
}

/*
 * generate a human readable description of the layer
 *
 * idx, flags, fmt, type, sleft, stop, sright, sbot, dleft, dtop, \
 * dright, dbot, rot, flip, blending, scalew, scaleh, visrects
 *
 */
static void rgz_print_layer(hwc_layer_t *l, int idx, int csv)
{
    char big_log[1024];
    int e = sizeof(big_log);
    char *end = big_log + e;
    e -= snprintf(end - e, e, "<!-- LAYER-DAT: %d", idx);


    e -= snprintf(end - e, e, "%s %p", csv ? "," : " hndl:",
            l->handle ? l->handle : NULL);

    e -= snprintf(end - e, e, "%s %s", csv ? "," : " flags:",
        l->flags & HWC_SKIP_LAYER ? "skip" : "none");

    IMG_native_handle_t *handle = (IMG_native_handle_t *)l->handle;
    if (handle) {
        e -= snprintf(end - e, e, "%s", csv ? ", " : " fmt: ");
        switch(handle->iFormat) {
        case HAL_PIXEL_FORMAT_BGRA_8888:
            e -= snprintf(end - e, e, "bgra"); break;
        case HAL_PIXEL_FORMAT_RGB_565:
            e -= snprintf(end - e, e, "rgb565"); break;
        case HAL_PIXEL_FORMAT_BGRX_8888:
            e -= snprintf(end - e, e, "bgrx"); break;
        case HAL_PIXEL_FORMAT_RGBX_8888:
            e -= snprintf(end - e, e, "rgbx"); break;
        case HAL_PIXEL_FORMAT_RGBA_8888:
            e -= snprintf(end - e, e, "rgba"); break;
        case HAL_PIXEL_FORMAT_TI_NV12:
        case HAL_PIXEL_FORMAT_TI_NV12_PADDED:
            e -= snprintf(end - e, e, "nv12"); break;
        default:
            e -= snprintf(end - e, e, "unknown");
        }
        e -= snprintf(end - e, e, "%s", csv ? ", " : " type: ");
        if (handle->usage & GRALLOC_USAGE_HW_RENDER)
            e -= snprintf(end - e, e, "hw");
        else if (handle->usage & GRALLOC_USAGE_SW_READ_MASK ||
                 handle->usage & GRALLOC_USAGE_SW_WRITE_MASK)
            e -= snprintf(end - e, e, "sw");
        else
            e -= snprintf(end - e, e, "unknown");
    } else {
        e -= snprintf(end - e, e, csv ? ", unknown" : " fmt: unknown");
        e -= snprintf(end - e, e, csv ? ", na" : " type: na");
    }
    e -= snprintf(end - e, e, csv ? ", %d, %d, %d, %d" : " src: %d %d %d %d",
        l->sourceCrop.left, l->sourceCrop.top, l->sourceCrop.right,
        l->sourceCrop.bottom);
    e -= snprintf(end - e, e, csv ? ", %d, %d, %d, %d" : " disp: %d %d %d %d",
        l->displayFrame.left, l->displayFrame.top,
        l->displayFrame.right, l->displayFrame.bottom);

    e -= snprintf(end - e, e, "%s %s", csv ? "," : " rot:",
        l->transform & HWC_TRANSFORM_ROT_90 ? "90" :
            l->transform & HWC_TRANSFORM_ROT_180 ? "180" :
            l->transform & HWC_TRANSFORM_ROT_270 ? "270" : "none");

    char flip[5] = "";
    strcat(flip, l->transform & HWC_TRANSFORM_FLIP_H ? "H" : "");
    strcat(flip, l->transform & HWC_TRANSFORM_FLIP_V ? "V" : "");
    if (!(l->transform & (HWC_TRANSFORM_FLIP_V|HWC_TRANSFORM_FLIP_H)))
        strcpy(flip, "none");
    e -= snprintf(end - e, e, "%s %s", csv ? "," : " flip:", flip);

    e -= snprintf(end - e, e, "%s %s", csv ? "," : " blending:",
        l->blending == HWC_BLENDING_NONE ? "none" :
        l->blending == HWC_BLENDING_PREMULT ? "premult" :
        l->blending == HWC_BLENDING_COVERAGE ? "coverage" : "invalid");

    e -= snprintf(end - e, e, "%s %1.3f", csv ? "," : " scalew:", getscalew(l));
    e -= snprintf(end - e, e, "%s %1.3f", csv ? "," : " scaleh:", getscaleh(l));

    e -= snprintf(end - e, e, "%s %d", csv ? "," : " visrect:",
        l->visibleRegionScreen.numRects);

    if (!csv) {
        e -= snprintf(end - e, e, " -->");
        OUTP("%s", big_log);

        size_t i = 0;
        for (; i < l->visibleRegionScreen.numRects; i++) {
            hwc_rect_t const *r = &l->visibleRegionScreen.rects[i];
            OUTP("<!-- LAYER-VIS: %d: rect: %d %d %d %d -->",
                    i, r->left, r->top, r->right, r->bottom);
        }
    } else {
        size_t i = 0;
        for (; i < l->visibleRegionScreen.numRects; i++) {
            hwc_rect_t const *r = &l->visibleRegionScreen.rects[i];
            e -= snprintf(end - e, e, ", %d, %d, %d, %d",
                    r->left, r->top, r->right, r->bottom);
        }
        e -= snprintf(end - e, e, " -->");
        OUTP("%s", big_log);
    }
}

static void rgz_print_layers(hwc_layer_list_t* list, int csv)
{
    size_t i;
    for (i = 0; i < list->numHwLayers; i++) {
        hwc_layer_t *l = &list->hwLayers[i];
        rgz_print_layer(l, i, csv);
    }
}

static int hal_to_ocd(int color)
{
    switch(color) {
    case HAL_PIXEL_FORMAT_BGRA_8888:
        return OCDFMT_BGRA24;
    case HAL_PIXEL_FORMAT_BGRX_8888:
        return OCDFMT_BGR124;
    case HAL_PIXEL_FORMAT_RGB_565:
        return OCDFMT_RGB16;
    case HAL_PIXEL_FORMAT_RGBA_8888:
        return OCDFMT_RGBA24;
    case HAL_PIXEL_FORMAT_RGBX_8888:
        return OCDFMT_RGB124;
    case HAL_PIXEL_FORMAT_TI_NV12:
        return OCDFMT_NV12;
    case HAL_PIXEL_FORMAT_YV12:
        return OCDFMT_YV12;
    default:
        return OCDFMT_UNKNOWN;
    }
}

/*
 * The loadbltsville fn is only needed for testing, the bltsville shared
 * libraries aren't planned to be used directly in production code here
 */
static BVFN_MAP bv_map;
static BVFN_BLT bv_blt;
static BVFN_UNMAP bv_unmap;
#ifndef RGZ_TEST_INTEGRATION
gralloc_module_t const *gralloc;
#endif
#define BLTSVILLELIB "libbltsville_cpu.so"

#ifdef RGZ_TEST_INTEGRATION
static int loadbltsville(void)
{
    void *hndl = dlopen(BLTSVILLELIB, RTLD_LOCAL | RTLD_LAZY);
    if (!hndl) {
        OUTE("Loading bltsville failed");
        return -1;
    }
    bv_map = (BVFN_MAP)dlsym(hndl, "bv_map");
    bv_blt = (BVFN_BLT)dlsym(hndl, "bv_blt");
    bv_unmap = (BVFN_UNMAP)dlsym(hndl, "bv_unmap");
    if(!bv_blt || !bv_map || !bv_unmap) {
        OUTE("Missing bltsville fn %p %p %p", bv_map, bv_blt, bv_unmap);
        return -1;
    }
    OUTP("Loaded %s", BLTSVILLELIB);

#ifndef RGZ_TEST_INTEGRATION
    hw_module_t const* module;
    int err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module);
    if (err != 0) {
        OUTE("Loading gralloc failed");
        return -1;
    }
    gralloc = (gralloc_module_t const *)module;
#endif
    return 0;
}
#else
static int loadbltsville(void) {
    return 0;
}
#endif

#ifndef RGZ_TEST_INTEGRATION
static int rgz_handle_to_stride(IMG_native_handle_t *h)
{
    int bpp = is_NV12(h->iFormat) ? 0 : (h->iFormat == HAL_PIXEL_FORMAT_RGB_565 ? 2 : 4);
    int stride = ALIGN(h->iWidth, HW_ALIGN) * bpp;
    return stride;
}

#endif

extern void BVDump(const char* prefix, const char* tab, const struct bvbltparams* parms);

static int rgz_get_orientation(unsigned int transform)
{
    int orientation = 0;
    if ((transform & HWC_TRANSFORM_FLIP_H) && (transform & HWC_TRANSFORM_FLIP_V))
        orientation += 180;
    if (transform & HWC_TRANSFORM_ROT_90)
        orientation += 90;

    return orientation;
}

static int rgz_get_flip_flags(unsigned int transform, int use_src2_flags)
{
    /*
     * If vertical and horizontal flip flags are set it means a 180 rotation
     * (with no flip) is intended for the layer, so we return 0 in that case.
     */
    int flip_flags = 0;
    if (transform & HWC_TRANSFORM_FLIP_H)
        flip_flags |= (use_src2_flags ? BVFLAG_HORZ_FLIP_SRC2 : BVFLAG_HORZ_FLIP_SRC1);
    if (transform & HWC_TRANSFORM_FLIP_V)
        flip_flags = flip_flags ? 0 : flip_flags | (use_src2_flags ? BVFLAG_VERT_FLIP_SRC2 : BVFLAG_VERT_FLIP_SRC1);
    return flip_flags;
}

static int rgz_hwc_layer_blit(hwc_layer_t *l, rgz_out_params_t *params, int buff_idx)
{
    IMG_native_handle_t *handle = (IMG_native_handle_t *)l->handle;
    if (!handle || l->flags & HWC_SKIP_LAYER) {
        /*
         * This shouldn't happen regionizer should reject compositions w/ skip
         * layers
         */
        OUTP("Cannot handle skip layers\n");
        return -1;
    }
    static int loaded = 0;
    if (!loaded)
        loaded = loadbltsville() ? : 1; /* attempt load once */

    struct bvbuffdesc *scrdesc;
    struct bvsurfgeom *scrgeom;
    int noblend;

    if (IS_BVCMD(params)) {
        scrdesc = NULL;
        scrgeom = params->data.bvc.dstgeom;
        noblend = params->data.bvc.noblend;
    } else {
        scrdesc = params->data.bv.dstdesc;
        scrgeom = params->data.bv.dstgeom;
        noblend = params->data.bv.noblend;
    }

    struct rgz_blt_entry* e;
    e = rgz_blts_get(&blts, params);

    struct bvbuffdesc *src1desc = &e->src1desc;
    src1desc->structsize = sizeof(struct bvbuffdesc);
    src1desc->length = handle->iHeight * HANDLE_TO_STRIDE(handle);
    /*
     * The virtaddr isn't going to be used in the final 2D h/w integration
     * because we will be handling buffers differently
     */
    src1desc->auxptr = buff_idx == -1 ? HANDLE_TO_BUFFER(handle) : (void*)buff_idx; /* FIXME: revisit this later */

    struct bvsurfgeom *src1geom = &e->src1geom;
    src1geom->structsize = sizeof(struct bvsurfgeom);
    src1geom->format = hal_to_ocd(handle->iFormat);
    src1geom->width = handle->iWidth;
    src1geom->height = handle->iHeight;
    if (l->transform & HAL_TRANSFORM_ROT_90)
        swap(src1geom->width, src1geom->height);
    src1geom->orientation = rgz_get_orientation(l->transform);
    src1geom->virtstride = HANDLE_TO_STRIDE(handle);

    struct bvsurfgeom *dstgeom = &e->dstgeom;
    dstgeom->structsize = sizeof(struct bvsurfgeom);
    dstgeom->format = scrgeom->format;
    dstgeom->width = scrgeom->width;
    dstgeom->height = scrgeom->height;
    /* Destination always set to 0, src buffers will contain rotation values as needed */
    dstgeom->orientation = 0;
    dstgeom->virtstride = DSTSTRIDE(scrgeom);

    struct bvbltparams *bp = &e->bp;
    bp->structsize = sizeof(struct bvbltparams);
    bp->dstdesc = scrdesc;
    bp->dstgeom = dstgeom;
    bp->dstrect.left = l->displayFrame.left;
    bp->dstrect.top = l->displayFrame.top;
    bp->dstrect.width = WIDTH(l->displayFrame);
    bp->dstrect.height = HEIGHT(l->displayFrame);
    bp->src1.desc = src1desc;
    bp->src1geom = src1geom;
    bp->cliprect.left = bp->cliprect.top = 0;
    bp->cliprect.width = scrgeom->width;
    bp->cliprect.height = scrgeom->height;

    blit_rect_t src1rect;
    blit_rect_t src1region;
    src1region.left = l->displayFrame.left;
    src1region.top = l->displayFrame.top;
    src1region.bottom = l->displayFrame.bottom;
    src1region.right = l->displayFrame.right;
    rgz_get_src_rect(l, &src1region, &src1rect);
    bp->src1rect.left = src1rect.left;
    bp->src1rect.top = src1rect.top;
    bp->src1rect.width = WIDTH(src1rect);
    bp->src1rect.height = HEIGHT(src1rect);

    unsigned long bpflags = BVFLAG_CLIP;
    if (!noblend && l->blending == HWC_BLENDING_PREMULT) {
        struct bvsurfgeom *src2geom = &e->src2geom;
        struct bvbuffdesc *src2desc = &e->src2desc;
        *src2geom = *dstgeom;
        src2desc->structsize = sizeof(struct bvbuffdesc);
        src2desc->auxptr = (void*)HWC_BLT_DESC_FB_FN(0);
        bpflags |= BVFLAG_BLEND;
        bp->op.blend = BVBLEND_SRC1OVER;
        bp->src2.desc = scrdesc;
        bp->src2geom = dstgeom;
        bp->src2rect.left = l->displayFrame.left;
        bp->src2rect.top = l->displayFrame.top;
        bp->src2rect.width = WIDTH(l->displayFrame);
        bp->src2rect.height = HEIGHT(l->displayFrame);
    } else {
        bpflags |= BVFLAG_ROP;
        bp->op.rop = 0xCCCC; /* SRCCOPY */
        if((src1geom->format == OCDFMT_BGR124) ||
           (src1geom->format == OCDFMT_RGB124) ||
           (src1geom->format == OCDFMT_RGB16))
            dstgeom->format = OCDFMT_BGR124;
    }

    bpflags |= rgz_get_flip_flags(l->transform, 0);
    bp->flags = bpflags;
    rgz_set_async(e, 1);

    return 0;
}

/*
 * Calculate the src rectangle on the basis of the layer display, source crop
 * and subregion rectangles. Additionally any rotation will be taken in
 * account. The resulting rectangle is written in res_rect.
 * Note: This doesn't work if the layer is scaled, another approach must be
 * taken in this situation.
 */
static void rgz_get_src_rect(hwc_layer_t* layer, blit_rect_t *subregion_rect, blit_rect_t *res_rect)
{
    if (rgz_hwc_scaled(layer))
        LOGE("%s: layer %p is scaled", __func__, layer);

    IMG_native_handle_t *handle = (IMG_native_handle_t *)layer->handle;
    int res_left = 0;
    int res_top = 0;
    int delta_left = subregion_rect->left - layer->displayFrame.left;
    int delta_top = subregion_rect->top - layer->displayFrame.top;
    /*
     * Calculate the top, left offset from the source cropping rectangle
     * depending on the rotation
     */
    switch(layer->transform) {
        case 0:
            res_left = layer->sourceCrop.left + delta_left;
            res_top = layer->sourceCrop.top + delta_top;
            break;
        case HAL_TRANSFORM_ROT_90:
            res_left = handle->iHeight - layer->sourceCrop.bottom + delta_left;
            res_top = layer->sourceCrop.left + delta_top;
            break;
        case HAL_TRANSFORM_ROT_180:
            res_left = handle->iWidth - layer->sourceCrop.right + delta_left;
            res_top = handle->iHeight - layer->sourceCrop.bottom + delta_top;
            break;
        case HAL_TRANSFORM_ROT_270:
            res_left = layer->sourceCrop.top + delta_left;
            res_top = handle->iWidth - layer->sourceCrop.right + delta_top;
            break;
        default:
            OUTE("Invalid transform value %d", layer->transform);
    }

    /* Resulting rectangle has the subregion dimensions */
    res_rect->left = res_left;
    res_rect->top = res_top;
    res_rect->right = res_left + WIDTH(*subregion_rect);
    res_rect->bottom = res_top + HEIGHT(*subregion_rect);
}


/*
 * Setup the src2 rectangle and the passed descriptor and
 * geometry
 */
static void rgz_src2blend_prep2(
    struct rgz_blt_entry* e, unsigned int hwc_transform, blit_rect_t *rect,
    struct bvbuffdesc *dstdesc, struct bvsurfgeom *dstgeom, int is_fb_dest)
{
    unsigned long bpflags = BVFLAG_CLIP;

    struct bvbltparams *bp = &e->bp;
    bpflags |= BVFLAG_BLEND;
    bp->op.blend = BVBLEND_SRC1OVER;
    bp->src2.desc = dstdesc;
    bp->src2geom = dstgeom;

    if (is_fb_dest) {
        struct bvsurfgeom *src2geom = &e->src2geom;
        struct bvbuffdesc *src2desc = &e->src2desc;
        *src2geom = *dstgeom;
        src2desc->structsize = sizeof(struct bvbuffdesc);
        src2desc->auxptr = (void*)HWC_BLT_DESC_FB_FN(0);
        /* Destination always set to 0, src buffers will contain rotation values as needed */
        src2geom->orientation = 0;
        bp->src2rect.left = rect->left;
        bp->src2rect.top = rect->top;
        bp->src2rect.width = WIDTH(*rect);
        bp->src2rect.height = HEIGHT(*rect);
    } else {
        struct bvsurfgeom *src2geom = &e->src2geom;
        src2geom->orientation = rgz_get_orientation(hwc_transform);
        bpflags |= rgz_get_flip_flags(hwc_transform, 1);
    }

    bp->flags |= bpflags;
}

static void rgz_src2blend_prep(
    struct rgz_blt_entry* e, rgz_layer_t *rgz_layer, blit_rect_t *rect, rgz_out_params_t *params)
{
    hwc_layer_t *l = rgz_layer->hwc_layer;
    IMG_native_handle_t *handle = (IMG_native_handle_t *)l->handle;

    struct bvbuffdesc *src2desc = &e->src2desc;
    src2desc->structsize = sizeof(struct bvbuffdesc);
    src2desc->length = handle->iHeight * HANDLE_TO_STRIDE(handle);
    src2desc->auxptr = IS_BVCMD(params)?
        (void*)rgz_layer->buffidx : HANDLE_TO_BUFFER(handle);

    struct bvsurfgeom *src2geom = &e->src2geom;
    src2geom->structsize = sizeof(struct bvsurfgeom);
    src2geom->format = hal_to_ocd(handle->iFormat);
    src2geom->width = handle->iWidth;
    src2geom->height = handle->iHeight;
    src2geom->virtstride = HANDLE_TO_STRIDE(handle);
    if (l->transform & HAL_TRANSFORM_ROT_90)
        swap(src2geom->width, src2geom->height);

    struct bvbltparams *bp = &e->bp;
    blit_rect_t src2rect;
    rgz_get_src_rect(l, rect, &src2rect);
    bp->src2rect.left = src2rect.left;
    bp->src2rect.top = src2rect.top;
    bp->src2rect.width = WIDTH(src2rect);
    bp->src2rect.height = HEIGHT(src2rect);

    rgz_src2blend_prep2(e, l->transform, rect, src2desc, src2geom, 0);
}

static void rgz_src1_prep(
    struct rgz_blt_entry* e, rgz_layer_t *rgz_layer,
    blit_rect_t *rect,
    struct bvbuffdesc *scrdesc, struct bvsurfgeom *scrgeom, rgz_out_params_t *params)
{
    hwc_layer_t *l = rgz_layer->hwc_layer;
    if (!l)
        return;

    IMG_native_handle_t *handle = (IMG_native_handle_t *)l->handle;

    struct bvbuffdesc *src1desc = &e->src1desc;
    src1desc->structsize = sizeof(struct bvbuffdesc);
    src1desc->length = handle->iHeight * HANDLE_TO_STRIDE(handle);
    src1desc->auxptr = IS_BVCMD(params) ?
        (void*)rgz_layer->buffidx : HANDLE_TO_BUFFER(handle);

    struct bvsurfgeom *src1geom = &e->src1geom;
    src1geom->structsize = sizeof(struct bvsurfgeom);
    src1geom->format = hal_to_ocd(handle->iFormat);
    src1geom->width = handle->iWidth;
    src1geom->height = handle->iHeight;
    src1geom->orientation = rgz_get_orientation(l->transform);
    src1geom->virtstride = HANDLE_TO_STRIDE(handle);
    if (l->transform & HAL_TRANSFORM_ROT_90)
        swap(src1geom->width, src1geom->height);

    struct bvsurfgeom *dstgeom = &e->dstgeom;
    dstgeom->structsize = sizeof(struct bvsurfgeom);
    dstgeom->format = scrgeom->format;
    dstgeom->width = scrgeom->width;
    dstgeom->height = scrgeom->height;
    /* Destination always set to 0, src buffers will contain rotation values as needed */
    dstgeom->orientation = 0;
    dstgeom->virtstride = DSTSTRIDE(scrgeom);

    struct bvbltparams *bp = &e->bp;
    bp->structsize = sizeof(struct bvbltparams);
    bp->dstdesc = scrdesc;
    bp->dstgeom = dstgeom;
    bp->cliprect.left = bp->dstrect.left = rect->left;
    bp->cliprect.top = bp->dstrect.top = rect->top;
    bp->cliprect.width = bp->dstrect.width = WIDTH(*rect);
    bp->cliprect.height = bp->dstrect.height = HEIGHT(*rect);
    bp->src1.desc = src1desc;
    bp->src1geom = src1geom;

    blit_rect_t src1rect;
    rgz_get_src_rect(l, rect, &src1rect);
    bp->src1rect.left = src1rect.left;
    bp->src1rect.top = src1rect.top;
    bp->src1rect.width = WIDTH(src1rect);
    bp->src1rect.height = HEIGHT(src1rect);
    bp->flags |= rgz_get_flip_flags(l->transform, 0);
}

static void rgz_batch_entry(struct rgz_blt_entry* e, unsigned int flag, unsigned int set)
{
    e->bp.flags &= ~BVFLAG_BATCH_MASK;
    e->bp.flags |= flag;
    e->bp.batchflags |= set;
}

static int rgz_hwc_subregion_blit(blit_hregion_t *hregion, int sidx, rgz_out_params_t *params)
{
    static int loaded = 0;
    if (!loaded)
        loaded = loadbltsville() ? : 1; /* attempt load once */

    struct bvbuffdesc *scrdesc;
    struct bvsurfgeom *scrgeom;
    int noblend;

    if (IS_BVCMD(params)) {
        scrdesc = NULL;
        scrgeom = params->data.bvc.dstgeom;
        noblend = params->data.bvc.noblend;
    } else {
        scrdesc = params->data.bv.dstdesc;
        scrgeom = params->data.bv.dstgeom;
        noblend = params->data.bv.noblend;
    }

    int lix;
    int ldepth = get_layer_ops(hregion, sidx, &lix);
    if (ldepth == 0) {
        /* Impossible, there are no layers in this region even if the
         * background is covering the whole screen
         */
        OUTE("hregion %p subregion %d doesn't have any ops", hregion, sidx);
        return -1;
    }

    /* Determine if this region is dirty */
    int dirty = 0, dirtylix = lix;
    while (dirtylix != -1) {
        rgz_layer_t *rgz_layer = hregion->rgz_layers[dirtylix];
        if (rgz_layer->dirty_count){
            /* One of the layers is dirty, we need to generate blits for this subregion */
            dirty = 1;
            break;
        }
        dirtylix = get_layer_ops_next(hregion, sidx, dirtylix);
    }

    if (!dirty)
        return 0;

    /* Check if the bottom layer is the background */
    if (hregion->rgz_layers[lix]->hwc_layer == &bg_layer) {
        if (ldepth == 1) {
            /* Background layer is the only operation, clear subregion */
            rgz_out_clrdst(params, &hregion->blitrects[lix][sidx]);
            return 0;
        } else {
            /* No need to generate blits with background layer if there is
             * another layer on top of it, discard it
             */
            ldepth--;
            lix = get_layer_ops_next(hregion, sidx, lix);
        }
    }

    /*
     * See if the depth most layer needs to be ignored. If this layer is the
     * only operation, we need to clear this subregion.
     */
    if (hregion->rgz_layers[lix]->buffidx == -1) {
        ldepth--;
        if (!ldepth) {
            rgz_out_clrdst(params, &hregion->blitrects[lix][sidx]);
            return 0;
        }
        lix = get_layer_ops_next(hregion, sidx, lix);
    }

    if (!noblend && ldepth > 1) { /* BLEND */
        blit_rect_t *rect = &hregion->blitrects[lix][sidx];
        struct rgz_blt_entry* e = rgz_blts_get(&blts, params);

        int s2lix = lix;
        lix = get_layer_ops_next(hregion, sidx, lix);

        /*
         * We save a read and a write from the FB if we blend the bottom
         * two layers
         */
        rgz_src1_prep(e, hregion->rgz_layers[lix], rect, scrdesc, scrgeom, params);
        rgz_src2blend_prep(e, hregion->rgz_layers[s2lix], rect, params);
        rgz_batch_entry(e, BVFLAG_BATCH_BEGIN, 0);
        rgz_set_async(e, 1);

        /* Rest of layers blended with FB */
        int first = 1;
        while((lix = get_layer_ops_next(hregion, sidx, lix)) != -1) {
            int batchflags = 0;
            e = rgz_blts_get(&blts, params);

            rgz_layer_t *rgz_layer = hregion->rgz_layers[lix];
            hwc_layer_t *layer = rgz_layer->hwc_layer;
            rgz_src1_prep(e, rgz_layer, rect, scrdesc, scrgeom, params);
            rgz_src2blend_prep2(e, layer->transform, rect, scrdesc, scrgeom, 1);

            if (first) {
                first = 0;
                batchflags |= BVBATCH_SRC2 | BVBATCH_SRC2RECT_ORIGIN;
            }
            batchflags |= BVBATCH_SRC1 | BVBATCH_SRC1RECT_ORIGIN;
            if (rgz_hwc_scaled(layer))
                batchflags |= BVBATCH_SRC1RECT_ORIGIN | BVBATCH_SRC1RECT_SIZE;
            rgz_batch_entry(e, BVFLAG_BATCH_CONTINUE, batchflags);
            rgz_set_async(e, 1);
        }

        if (e->bp.flags & BVFLAG_BATCH_BEGIN)
            rgz_batch_entry(e, 0, 0);
        else
            rgz_batch_entry(e, BVFLAG_BATCH_END, 0);

    } else { /* COPY */
        blit_rect_t *rect = &hregion->blitrects[lix][sidx];
        if (noblend)    /* get_layer_ops() doesn't understand this so get the top */
            lix = get_top_rect(hregion, sidx, &rect);

        struct rgz_blt_entry* e = rgz_blts_get(&blts, params);

        rgz_layer_t *rgz_layer = hregion->rgz_layers[lix];
        hwc_layer_t *l = rgz_layer->hwc_layer;
        rgz_src1_prep(e, rgz_layer, rect, scrdesc, scrgeom, params);

        struct bvsurfgeom *src1geom = &e->src1geom;
        unsigned long bpflags = BVFLAG_CLIP | BVFLAG_ROP;
        e->bp.op.rop = 0xCCCC; /* SRCCOPY */
        if((src1geom->format == OCDFMT_BGR124) ||
           (src1geom->format == OCDFMT_RGB124) ||
           (src1geom->format == OCDFMT_RGB16))
            e->dstgeom.format = OCDFMT_BGR124;

        rgz_set_async(e, 1);
        e->bp.flags |= bpflags;
    }
    return 0;
}

struct bvbuffdesc gscrndesc = {
    .structsize = sizeof(struct bvbuffdesc), .length = 0,
    .auxptr = MAP_FAILED
};
struct bvsurfgeom gscrngeom = {
    .structsize = sizeof(struct bvsurfgeom), .format = OCDFMT_UNKNOWN
};

static void rgz_blts_init(struct rgz_blts *blts)
{
    bzero(blts, sizeof(*blts));
}

static void rgz_blts_free(struct rgz_blts *blts)
{
    /* TODO ??? maybe we should dynamically allocate this */
    rgz_blts_init(blts);
}

static struct rgz_blt_entry* rgz_blts_get(struct rgz_blts *blts, rgz_out_params_t *params)
{
    struct rgz_blt_entry *ne;
    if (blts->idx < RGZ_MAX_BLITS) {
        ne = &blts->bvcmds[blts->idx++];
        if (IS_BVCMD(params))
            params->data.bvc.out_blits++;
    } else {
        OUTE("!!! BIG PROBLEM !!! run out of blit entries");
        ne = &blts->bvcmds[blts->idx - 1]; /* Return last slot */
    }
    return ne;
}

static int rgz_blts_bvdirect(rgz_t *rgz, struct rgz_blts *blts, rgz_out_params_t *params)
{
    struct bvbatch *batch = NULL;
    int rv = -1;
    int idx = 0;

    while (idx < blts->idx) {
        struct rgz_blt_entry *e = &blts->bvcmds[idx];
        if (e->bp.flags & BVFLAG_BATCH_MASK)
            e->bp.batch = batch;
        rv = bv_blt(&e->bp);
        if (rv) {
            OUTE("BV_BLT failed: %d", rv);
            BVDUMP("bv_blt:", "  ", &e->bp);
            return -1;
        }
        if (e->bp.flags & BVFLAG_BATCH_BEGIN)
            batch = e->bp.batch;
        idx++;
    }
    return rv;
}

static int rgz_out_region(rgz_t *rgz, rgz_out_params_t *params)
{
    if (!(rgz->state & RGZ_REGION_DATA)) {
        OUTE("rgz_out_region invoked with bad state");
        return -1;
    }

    rgz_blts_init(&blts);
    LOGD_IF(debug, "rgz_out_region:");

    if (IS_BVCMD(params))
        params->data.bvc.out_blits = 0;

    int i;
    for (i = 0; i < rgz->nhregions; i++) {
        blit_hregion_t *hregion = &rgz->hregions[i];
        int s;
        LOGD_IF(debug, "h[%d] nsubregions = %d", i, hregion->nsubregions);
        if (hregion->nlayers == 0) {
            /* Impossible, there are no layers in this region even if the
             * background is covering the whole screen
             */
            OUTE("hregion %p doesn't have any ops", hregion);
            return -1;
        }
        for (s = 0; s < hregion->nsubregions; s++) {
            LOGD_IF(debug, "h[%d] -> [%d]", i, s);
            if (rgz_hwc_subregion_blit(hregion, s, params))
                return -1;
        }
    }

    int rv = 0;

    if (IS_BVCMD(params)) {
        unsigned int j;
        params->data.bvc.out_nhndls = 0;
        /* Begin from index 1 to remove the background layer from the output */
        for (j = 1, i = 0; j < rgz->rgz_layerno; j++) {
            rgz_layer_t *rgz_layer = &rgz->rgz_layers[j];
            /* We don't need the handles for layers marked as -1 */
            if (rgz_layer->buffidx == -1)
                continue;
            hwc_layer_t *layer = rgz_layer->hwc_layer;
            params->data.bvc.out_hndls[i++] = layer->handle;
            params->data.bvc.out_nhndls++;
        }

        /* Last blit is made sync to act like a fence for the previous async blits */
        struct rgz_blt_entry* e = &blts.bvcmds[blts.idx-1];
        rgz_set_async(e, 0);

        /* FIXME: we want to be able to call rgz_blts_free and populate the actual
         * composition data structure ourselves */
        params->data.bvc.cmdp = blts.bvcmds;
        params->data.bvc.cmdlen = blts.idx;
        if (params->data.bvc.out_blits >= RGZ_MAX_BLITS)
            rv = -1;
        //rgz_blts_free(&blts);
    } else {
        rv = rgz_blts_bvdirect(rgz, &blts, params);
        rgz_blts_free(&blts);
    }

    return rv;
}

void rgz_profile_hwc(hwc_layer_list_t* list, int dispw, int disph)
{
    if (!list)  /* A NULL composition list can occur */
        return;

#ifndef RGZ_TEST_INTEGRATION
    static char regiondump2[PROPERTY_VALUE_MAX] = "";
    char regiondump[PROPERTY_VALUE_MAX];
    property_get("debug.2dhwc.region", regiondump, "0");
    int dumpregions = strncmp(regiondump, regiondump2, PROPERTY_VALUE_MAX);
    if (dumpregions)
        strncpy(regiondump2, regiondump, PROPERTY_VALUE_MAX);
    else {
        dumpregions = !strncmp(regiondump, "all", PROPERTY_VALUE_MAX) &&
                      (list->flags & HWC_GEOMETRY_CHANGED);
        static int iteration = 0;
        if (dumpregions)
            sprintf(regiondump, "iteration %d", iteration++);
    }

    char dumplayerdata[PROPERTY_VALUE_MAX];
    /* 0 - off, 1 - human readable, 2 - CSV */
    property_get("debug.2dhwc.dumplayers", dumplayerdata, "0");
    int dumplayers = atoi(dumplayerdata);
#else
    char regiondump[] = "";
    int dumplayers = 1;
    int dumpregions = 0;
#endif
    if (dumplayers && (list->flags & HWC_GEOMETRY_CHANGED)) {
        OUTP("<!-- BEGUN-LAYER-DUMP: %d -->", list->numHwLayers);
        rgz_print_layers(list, dumplayers == 1 ? 0 : 1);
        OUTP("<!-- ENDED-LAYER-DUMP -->");
    }

    if(!dumpregions)
        return;

    rgz_t rgz;
    rgz_in_params_t ip = { .data = { .hwc = {
                           .layers = list->hwLayers,
                           .layerno = list->numHwLayers } } };
    ip.op = RGZ_IN_HWCCHK;
    if (rgz_in(&ip, &rgz) == RGZ_ALL) {
        ip.op = RGZ_IN_HWC;
        if (rgz_in(&ip, &rgz) == RGZ_ALL) {
            OUTP("<!-- BEGUN-SVG-DUMP: %s -->", regiondump);
            OUTP("<b>%s</b>", regiondump);
            rgz_out_params_t op = {
                .op = RGZ_OUT_SVG,
                .data = {
                    .svg = {
                        .dispw = dispw, .disph = disph,
                        .htmlw = 450, .htmlh = 800
                    }
                },
            };
            rgz_out(&rgz, &op);
            OUTP("<!-- ENDED-SVG-DUMP -->");
        }
    }
    rgz_release(&rgz);
}

int rgz_get_screengeometry(int fd, struct bvsurfgeom *geom, int fmt)
{
    /* Populate Bltsville destination buffer information with framebuffer data */
    struct fb_fix_screeninfo fb_fixinfo;
    struct fb_var_screeninfo fb_varinfo;

    LOGI("Attempting to get framebuffer device info.");
    if(ioctl(fd, FBIOGET_FSCREENINFO, &fb_fixinfo)) {
        OUTE("Error getting fb_fixinfo");
        return -EINVAL;
    }

    if(ioctl(fd, FBIOGET_VSCREENINFO, &fb_varinfo)) {
        LOGE("Error gettting fb_varinfo");
        return -EINVAL;
    }

    bzero(&bg_layer, sizeof(bg_layer));
    bg_layer.displayFrame.left = bg_layer.displayFrame.top = 0;
    bg_layer.displayFrame.right = fb_varinfo.xres;
    bg_layer.displayFrame.bottom = fb_varinfo.yres;

    bzero(geom, sizeof(*geom));
    geom->structsize = sizeof(*geom);
    geom->width = fb_varinfo.xres;
    geom->height = fb_varinfo.yres;
    geom->virtstride = fb_fixinfo.line_length;
    geom->format = hal_to_ocd(fmt);
    geom->orientation = 0;
    return 0;
}

int rgz_in(rgz_in_params_t *p, rgz_t *rgz)
{
    int rv = -1;
    switch (p->op) {
    case RGZ_IN_HWC:
        rv = rgz_in_hwccheck(p, rgz);
        if (rv == RGZ_ALL)
            rv = rgz_in_hwc(p, rgz) ? 0 : RGZ_ALL;
        break;
    case RGZ_IN_HWCCHK:
        bzero(rgz, sizeof(rgz_t));
        rv = rgz_in_hwccheck(p, rgz);
        break;
    default:
        return -1;
    }
    return rv;
}

void rgz_release(rgz_t *rgz)
{
    if (!rgz)
        return;
    if (rgz->hregions)
        free(rgz->hregions);
    bzero(rgz, sizeof(*rgz));
}

int rgz_out(rgz_t *rgz, rgz_out_params_t *params)
{
    switch (params->op) {
    case RGZ_OUT_SVG:
        rgz_out_svg(rgz, params);
        return 0;
    case RGZ_OUT_BVDIRECT_PAINT:
        return rgz_out_bvdirect_paint(rgz, params);
    case RGZ_OUT_BVCMD_PAINT:
        return rgz_out_bvcmd_paint(rgz, params);
    case RGZ_OUT_BVDIRECT_REGION:
    case RGZ_OUT_BVCMD_REGION:
        return rgz_out_region(rgz, params);
    default:
        return -1;
    }
}

