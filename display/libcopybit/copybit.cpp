/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (c) 2010 - 2011, Code Aurora Forum. All rights reserved.
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


#define LOG_TAG "copybit"

#include <cutils/log.h>

#include "msm_mdp.h"
#include <linux/fb.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>

#include "copybit.h"

#include <gralloc_priv.h>
#include "software_converter.h"

#define DEBUG_MDP_ERRORS 1

/******************************************************************************/

#if defined(COPYBIT_MSM7K)
#define MAX_SCALE_FACTOR    (4)
#define MAX_DIMENSION       (4096)
#elif defined(COPYBIT_QSD8K)
#define MAX_SCALE_FACTOR    (8)
#define MAX_DIMENSION       (2048)
#else
#error "Unsupported MDP version"
#endif

/******************************************************************************/

/** State information for each device instance */
struct copybit_context_t {
    struct copybit_device_t device;
    int     mFD;
    uint8_t mAlpha;
    uint8_t mFlags;
};

/**
 * Common hardware methods
 */

static int open_copybit(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t copybit_module_methods = {
    open:  open_copybit
};

/*
 * The COPYBIT Module
 */
struct copybit_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: COPYBIT_HARDWARE_MODULE_ID,
        name: "QCT MSM7K COPYBIT Module",
        author: "Google, Inc.",
        methods: &copybit_module_methods
    }
};

/******************************************************************************/

/** min of int a, b */
static inline int min(int a, int b) {
    return (a<b) ? a : b;
}

/** max of int a, b */
static inline int max(int a, int b) {
    return (a>b) ? a : b;
}

/** scale each parameter by mul/div. Assume div isn't 0 */
static inline void MULDIV(uint32_t *a, uint32_t *b, int mul, int div) {
    if (mul != div) {
        *a = (mul * *a) / div;
        *b = (mul * *b) / div;
    }
}

/** Determine the intersection of lhs & rhs store in out */
static void intersect(struct copybit_rect_t *out,
                      const struct copybit_rect_t *lhs,
                      const struct copybit_rect_t *rhs) {
    out->l = max(lhs->l, rhs->l);
    out->t = max(lhs->t, rhs->t);
    out->r = min(lhs->r, rhs->r);
    out->b = min(lhs->b, rhs->b);
}

/** convert COPYBIT_FORMAT to MDP format */
static int get_format(int format) {
    switch (format) {
    case HAL_PIXEL_FORMAT_RGB_565:       return MDP_RGB_565;
    case HAL_PIXEL_FORMAT_RGBX_8888:     return MDP_RGBX_8888;
    case HAL_PIXEL_FORMAT_RGB_888:       return MDP_RGB_888;
    case HAL_PIXEL_FORMAT_RGBA_8888:     return MDP_RGBA_8888;
    case HAL_PIXEL_FORMAT_BGRA_8888:     return MDP_BGRA_8888;
    case HAL_PIXEL_FORMAT_YCrCb_422_SP:  return MDP_Y_CBCR_H2V1;
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:  return MDP_Y_CBCR_H2V2;
    case HAL_PIXEL_FORMAT_YCbCr_422_SP:  return MDP_Y_CRCB_H2V1;
    case HAL_PIXEL_FORMAT_YCbCr_420_SP:  return MDP_Y_CRCB_H2V2;
    }
    return -1;
}

/** convert from copybit image to mdp image structure */
static void set_image(struct mdp_img *img, const struct copybit_image_t *rhs) 
{
    private_handle_t* hnd = (private_handle_t*)rhs->handle;
    if(hnd == NULL){
        LOGE("copybit: Invalid handle");
        return;
    }
    img->width      = rhs->w;
    img->height     = rhs->h;
    img->format     = get_format(rhs->format);
    img->offset     = hnd->offset;
#if defined(COPYBIT_MSM7K)
    if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_PMEM_ADSP) {
	img->offset += hnd->map_offset;
	img->memory_id = hnd->gpu_fd;
	if (img->format == MDP_RGBA_8888) {
	    img->format = MDP_BGRA_8888;
	}
	else {
	    img->memory_id = hnd->fd;
	}
    }
#else
    img->memory_id  = hnd->fd;
#endif
}
/** setup rectangles */
static void set_rects(struct copybit_context_t *dev,
                      struct mdp_blit_req *e,
                      const struct copybit_rect_t *dst,
                      const struct copybit_rect_t *src,
                      const struct copybit_rect_t *scissor,
                      uint32_t horiz_padding,
                      uint32_t vert_padding) {
    struct copybit_rect_t clip;
    intersect(&clip, scissor, dst);

    e->dst_rect.x  = clip.l;
    e->dst_rect.y  = clip.t;
    e->dst_rect.w  = clip.r - clip.l;
    e->dst_rect.h  = clip.b - clip.t;

    uint32_t W, H;
    if (dev->mFlags & COPYBIT_TRANSFORM_ROT_90) {
        e->src_rect.x = (clip.t - dst->t) + src->t;
        e->src_rect.y = (dst->r - clip.r) + src->l;
        e->src_rect.w = (clip.b - clip.t);
        e->src_rect.h = (clip.r - clip.l);
        W = dst->b - dst->t;
        H = dst->r - dst->l;
    } else {
        e->src_rect.x  = (clip.l - dst->l) + src->l;
        e->src_rect.y  = (clip.t - dst->t) + src->t;
        e->src_rect.w  = (clip.r - clip.l);
        e->src_rect.h  = (clip.b - clip.t);
        W = dst->r - dst->l;
        H = dst->b - dst->t;
    }
    MULDIV(&e->src_rect.x, &e->src_rect.w, src->r - src->l, W);
    MULDIV(&e->src_rect.y, &e->src_rect.h, src->b - src->t, H);

    if (dev->mFlags & COPYBIT_TRANSFORM_FLIP_V) {
    /*
        if (dev->mFlags & COPYBIT_TRANSFORM_ROT_90) {
            e->src_rect.x = e->src.width - (e->src_rect.x + e->src_rect.w) - horiz_padding;
        }else{
            e->src_rect.y = e->src.height - (e->src_rect.y + e->src_rect.h) - vert_padding;
        }
    */
	e->src_rect.y = e->src.height - (e->src_rect.y + e->src_rect.h);
    }

    if (dev->mFlags & COPYBIT_TRANSFORM_FLIP_H) {
/*
        if (dev->mFlags & COPYBIT_TRANSFORM_ROT_90) {
            e->src_rect.y = e->src.height - (e->src_rect.y + e->src_rect.h) - vert_padding;
        }else{
            e->src_rect.x = e->src.width - (e->src_rect.x + e->src_rect.w) - horiz_padding;
        }
*/
	e->src_rect.x = e->src.width - (e->src_rect.x + e->src_rect.w);
    }
}

/** setup mdp request */
static void set_infos(struct copybit_context_t *dev, struct mdp_blit_req *req, int flags) {
    req->alpha = dev->mAlpha;
    req->transp_mask = MDP_TRANSP_NOP;
    req->flags = dev->mFlags;// | flags;
}

/** copy the bits */
static int msm_copybit(struct copybit_context_t *dev, void const *list) 
{
    int err = ioctl(dev->mFD, MSMFB_BLIT,
                    (struct mdp_blit_req_list const*)list);
    LOGE_IF(err<0, "copyBits failed (%s)", strerror(errno));
    if (err == 0) {
        return 0;
    } else {
#if DEBUG_MDP_ERRORS
        struct mdp_blit_req_list const* l = (struct mdp_blit_req_list const*)list;
        for (int i=0 ; i<l->count ; i++) {
            LOGD("%d: src={w=%d, h=%d, f=%d, rect={%d,%d,%d,%d}}\n"
                 "    dst={w=%d, h=%d, f=%d, rect={%d,%d,%d,%d}}\n"
                 "    flags=%08lx"
                    ,
                    i,
                    l->req[i].src.width,
                    l->req[i].src.height,
                    l->req[i].src.format,
                    l->req[i].src_rect.x,
                    l->req[i].src_rect.y,
                    l->req[i].src_rect.w,
                    l->req[i].src_rect.h,
                    l->req[i].dst.width,
                    l->req[i].dst.height,
                    l->req[i].dst.format,
                    l->req[i].dst_rect.x,
                    l->req[i].dst_rect.y,
                    l->req[i].dst_rect.w,
                    l->req[i].dst_rect.h,
                    l->req[i].flags
            );
        }
#endif
        return -errno;
    }
}

/*****************************************************************************/

/** Set a parameter to value */
static int set_parameter_copybit(
        struct copybit_device_t *dev,
        int name,
        int value) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    int status = 0;
    if (ctx) {
        switch(name) {
        case COPYBIT_ROTATION_DEG:
            switch (value) {
            case 0:
                ctx->mFlags &= ~0x7;
                break;
            case 90:
                ctx->mFlags &= ~0x7;
                ctx->mFlags |= MDP_ROT_90;
                break;
            case 180:
                ctx->mFlags &= ~0x7;
                ctx->mFlags |= MDP_ROT_180;
                break;
            case 270:
                ctx->mFlags &= ~0x7;
                ctx->mFlags |= MDP_ROT_270;
                break;
            default:
                LOGE("Invalid value for COPYBIT_ROTATION_DEG");
                status = -EINVAL;
                break;
            }
            break;
        case COPYBIT_PLANE_ALPHA:
            if (value < 0)      value = 0;
            if (value >= 256)   value = 255;
            ctx->mAlpha = value;
            break;
        case COPYBIT_DITHER:
            if (value == COPYBIT_ENABLE) {
                ctx->mFlags |= MDP_DITHER;
            } else if (value == COPYBIT_DISABLE) {
                ctx->mFlags &= ~MDP_DITHER;
            }
            break;
        case COPYBIT_BLUR:
            if (value == COPYBIT_ENABLE) {
                ctx->mFlags |= MDP_BLUR;
            } else if (value == COPYBIT_DISABLE) {
                ctx->mFlags &= ~MDP_BLUR;
            }
            break;
        case COPYBIT_TRANSFORM:
            ctx->mFlags &= ~0x7;
            ctx->mFlags |= value & 0x7;
            break;
        default:
            status = -EINVAL;
            break;
        }
    } else {
        status = -EINVAL;
    }
    return status;
}

/** Get a static info value */
static int get(struct copybit_device_t *dev, int name) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    int value;
    if (ctx) {
        switch(name) {
        case COPYBIT_MINIFICATION_LIMIT:
            value = MAX_SCALE_FACTOR;
            break;
        case COPYBIT_MAGNIFICATION_LIMIT:
            value = MAX_SCALE_FACTOR;
            break;
        case COPYBIT_SCALING_FRAC_BITS:
            value = 32;
            break;
        case COPYBIT_ROTATION_STEP_DEG:
            value = 90;
            break;
        default:
            value = -EINVAL;
        }
    } else {
        value = -EINVAL;
    }
    return value;
}

/** do a stretch blit type operation */
static int stretch_copybit(
        struct copybit_device_t *dev,
        struct copybit_image_t const *dst,
        struct copybit_image_t const *src,
        struct copybit_rect_t const *dst_rect,
        struct copybit_rect_t const *src_rect,
        struct copybit_region_t const *region) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    int status = 0;
    if (ctx) {
        struct {
            uint32_t count;
            struct mdp_blit_req req[12];
        } list;

        if (ctx->mAlpha < 255) {
            switch (src->format) {
                // we don't support plane alpha with RGBA formats
                case HAL_PIXEL_FORMAT_RGBA_8888:
                case HAL_PIXEL_FORMAT_BGRA_8888:
                case HAL_PIXEL_FORMAT_RGBA_5551:
                case HAL_PIXEL_FORMAT_RGBA_4444:
                    return -EINVAL;
            }
        }

        if (src_rect->l < 0 || src_rect->r > src->w ||
            src_rect->t < 0 || src_rect->b > src->h) {
            // this is always invalid
            return -EINVAL;
        }

        if (src->w > MAX_DIMENSION || src->h > MAX_DIMENSION)
            return -EINVAL;

        if (dst->w > MAX_DIMENSION || dst->h > MAX_DIMENSION)
            return -EINVAL;

        if(src->format ==  HAL_PIXEL_FORMAT_YV12) {
            if(0 == convertYV12toYCrCb420SP(src)){
                //if inplace conversion,just convert and return
                if(src->base == dst->base)
                    return status;
            }
            else{
                LOGE("Error copybit conversion from yv12 failed");
                return -EINVAL;
            }
        }

        const uint32_t maxCount = sizeof(list.req)/sizeof(list.req[0]);
        const struct copybit_rect_t bounds = { 0, 0, dst->w, dst->h };
        struct copybit_rect_t clip;
        list.count = 0;
        status = 0;
        while ((status == 0) && region->next(region, &clip)) {
            intersect(&clip, &bounds, &clip);
            mdp_blit_req* req = &list.req[list.count];
            int flags = 0;
/*
            private_handle_t* src_hnd = (private_handle_t*)src->handle;
            if(src_hnd != NULL && src_hnd->flags & private_handle_t::PRIV_FLAGS_DO_NOT_FLUSH) {
                flags |=  MDP_BLIT_NON_CACHED;
            }
*/
            set_infos(ctx, req, flags);
            set_image(&req->dst, dst);
            set_image(&req->src, src);
            set_rects(ctx, req, dst_rect, src_rect, &clip, src->horiz_padding, src->vert_padding);

            if (req->src_rect.w<=0 || req->src_rect.h<=0)
                continue;

            if (req->dst_rect.w<=0 || req->dst_rect.h<=0)
                continue;

            if (++list.count == maxCount) {
                status = msm_copybit(ctx, &list);
                list.count = 0;
            }
        }
        if ((status == 0) && list.count) {
            status = msm_copybit(ctx, &list);
        }
    } else {
        status = -EINVAL;
    }
    return status;
}

/** Perform a blit type operation */
static int blit_copybit(
        struct copybit_device_t *dev,
        struct copybit_image_t const *dst,
        struct copybit_image_t const *src,
        struct copybit_region_t const *region) 
{
    struct copybit_rect_t dr = { 0, 0, dst->w, dst->h };
    struct copybit_rect_t sr = { 0, 0, src->w, src->h };
    return stretch_copybit(dev, dst, src, &dr, &sr, region);
}

/*****************************************************************************/

/** Close the copybit device */
static int close_copybit(struct hw_device_t *dev) 
{
    struct copybit_context_t* ctx = (struct copybit_context_t*)dev;
    if (ctx) {
        close(ctx->mFD);
        free(ctx);
    }
    return 0;
}

/** Open a new instance of a copybit device using name */
static int open_copybit(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;
    copybit_context_t *ctx;
    ctx = (copybit_context_t *)malloc(sizeof(copybit_context_t));
    memset(ctx, 0, sizeof(*ctx));

    ctx->device.common.tag = HARDWARE_DEVICE_TAG;
    ctx->device.common.version = 1;
    ctx->device.common.module = const_cast<hw_module_t*>(module);
    ctx->device.common.close = close_copybit;
    ctx->device.set_parameter = set_parameter_copybit;
    ctx->device.get = get;
    ctx->device.blit = blit_copybit;
    ctx->device.stretch = stretch_copybit;
    ctx->mAlpha = MDP_ALPHA_NOP;
    ctx->mFlags = 0;
    ctx->mFD = open("/dev/graphics/fb0", O_RDWR, 0);
    
    if (ctx->mFD < 0) {
        status = errno;
        LOGE("Error opening frame buffer errno=%d (%s)",
             status, strerror(status));
        status = -status;
    } else {
        struct fb_fix_screeninfo finfo;
        if (ioctl(ctx->mFD, FBIOGET_FSCREENINFO, &finfo) == 0) {
            if (strncmp(finfo.id, "msmfb", 5) == 0) {
                /* Success */
                status = 0;
            } else {
                LOGE("Error not msm frame buffer");
                status = -EINVAL;
            }
        } else {
            LOGE("Error executing ioctl for screen info");
            status = -errno;
        }
    }

    if (status == 0) {
        *device = &ctx->device.common;
    } else {
        close_copybit(&ctx->device.common);
    }
    return status;
}
