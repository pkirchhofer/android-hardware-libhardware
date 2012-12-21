/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <sys/mman.h>

#include <dlfcn.h>

#include <cutils/ashmem.h>
#include <cutils/log.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#if HAVE_ANDROID_OS
#include <linux/fb.h>
#endif

#include "gralloc_priv.h"
#include "gr.h"

/*****************************************************************************/

// numbers of buffers for page flipping
#define NUM_BUFFERS 2


enum {
    PAGE_FLIP = 0x00000001,
    LOCKED = 0x00000002
};

struct fb_context_t {
    framebuffer_device_t  device;
};

/*****************************************************************************/

static int fb_setSwapInterval(struct framebuffer_device_t* dev,
            int interval)
{
    fb_context_t* ctx = (fb_context_t*)dev;
    if (interval < dev->minSwapInterval || interval > dev->maxSwapInterval)
        return -EINVAL;
    // FIXME: implement fb_setSwapInterval
    return 0;
}

static int fb_setUpdateRect(struct framebuffer_device_t* dev,
        int l, int t, int w, int h)
{
    if (((w|h) <= 0) || ((l|t)<0))
        return -EINVAL;
        
    fb_context_t* ctx = (fb_context_t*)dev;
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);
    m->info.reserved[0] = 0x54445055; // "UPDT";
    m->info.reserved[1] = (uint16_t)l | ((uint32_t)t << 16);
    m->info.reserved[2] = (uint16_t)(l+w) | ((uint32_t)(t+h) << 16);
    return 0;
}

static int fb_post(struct framebuffer_device_t* dev, buffer_handle_t buffer)
{
    if (private_handle_t::validate(buffer) < 0)
        return -EINVAL;

    fb_context_t* ctx = (fb_context_t*)dev;

    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(buffer);
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);

    if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER) {
        const size_t offset = hnd->base - m->framebuffer->base;
        m->info.activate = FB_ACTIVATE_VBL;
        m->info.yoffset = offset / m->finfo.line_length;
        LOGI("[GRALLOC] Doing FBIOPUT_VSCREENINFO");
        if (ioctl(m->framebuffer->fd, FBIOPUT_VSCREENINFO, &m->info) == -1) {
            LOGE("FBIOPUT_VSCREENINFO failed");
            m->base.unlock(&m->base, buffer); 
            return -errno;
        }
        m->currentBuffer = buffer;
        
    } else {
        // If we can't do the page_flip, just copy the buffer to the front 
        // FIXME: use copybit HAL instead of memcpy
        
        void* fb_vaddr;
        void* buffer_vaddr;
        
        m->base.lock(&m->base, m->framebuffer, 
                GRALLOC_USAGE_SW_WRITE_RARELY, 
                0, 0, m->info.xres, m->info.yres,
                &fb_vaddr);

        m->base.lock(&m->base, buffer, 
                GRALLOC_USAGE_SW_READ_RARELY, 
                0, 0, m->info.xres, m->info.yres,
                &buffer_vaddr);

        LOGI("[GRALLOC] Doing memcpy");
        LOGI("[GRALLOC] m->finfo.line_length %d m->info.yres %d m->info.xres %d", m->finfo.line_length, m->info.yres, m->info.xres);
        //memcpy(fb_vaddr, buffer_vaddr, m->finfo.line_length * m->info.yres);

        uint8_t* in8 = (uint8_t*) buffer_vaddr - 10 * m->finfo.line_length;
        uint8_t* out8 = (uint8_t*) fb_vaddr;
        //uint16_t* inbuf = (uint16_t*) buffer_vaddr;
        //uint16_t* outbuf = (uint16_t*) fb_vaddr;
        uint16_t width = m->info.xres;
        uint16_t half_width = width / 2;
        uint16_t height = m->info.yres;
        uint16_t row_cnt, pix_cnt;
        uint32_t row_start;
        uint32_t offset = half_width + 16; // in pixels
        //uint32_t copied = 0;

        // Copy left side to right side
        for (row_cnt = 1; row_cnt < height; row_cnt++) {
            uint32_t start_offset = row_cnt * m->finfo.line_length;
            void* in = in8 + start_offset;
            void* out = out8 + start_offset + (width - offset) * 2 - 1 * m->finfo.line_length;
            memcpy(out, in, offset * 2);
            //copied += offset * 2;

            //row_start = row_cnt * width;
            //for (pix_cnt = 0; pix_cnt < offset; pix_cnt++) {
            //    outbuf[row_start + pix_cnt + (width - offset)] = inbuf[row_start + pix_cnt];
            //}
        }

        // Copy right side to left side
        for (row_cnt = 0; row_cnt < height; row_cnt++) {
            uint32_t start_offset = row_cnt * m->finfo.line_length;
            void* in = in8 + start_offset + offset * 2;
            void* out = out8 + start_offset + 0 * m->finfo.line_length;
            memcpy(out, in, (width - offset) * 2);
            //copied += (width - offset) * 2;

            //row_start = row_cnt * width;
            //for (pix_cnt = offset; pix_cnt < width; pix_cnt++) {
            //    outbuf[row_start + pix_cnt - offset] = inbuf[row_start + pix_cnt];
            //}
        }

/*
        uint16_t* inbuf = (uint16_t*) buffer_vaddr;
        uint32_t* outbuf = (uint32_t*) fb_vaddr;
        uint16_t row_cnt, pix_cnt;
        uint16_t width = m->info.xres;
        uint16_t height = m->info.yres;
        uint32_t row_start;

        // 16 Bit Input
        uint16_t in;

        // 32 Bit Output
        uint8_t A_in, R_in, G_in, B_in, A_out, R_out, G_out, B_out;

        //#define MODE565
        #define MODE555

        for (row_cnt = 0; row_cnt < height; row_cnt++) {
            row_start = row_cnt * width;

            for (pix_cnt = 0; pix_cnt < width; pix_cnt++) {
                 in = inbuf[row_start + pix_cnt];

#ifdef MODE565   // 565
                 A_in = 0;
                 B_in = (in >> 11) & 0x1F;
                 G_in = (in >> 5) & 0x3F;
#else            // 555
                 A_in = (in >> 15) & 0x01;
                 B_in = (in >> 10) & 0x1F;
                 G_in = (in >> 5) & 0x1F;
#endif
                 R_in = in & 0x1F;

#ifdef MODE565   // 565
                 A_out = 0xFF000000;
                 G_out = (G_in * 529 + 33) >> 6;
#else            // 555
                 A_out = (A_in) ? 0xFF : 0x00;
                 G_out = (G_in * 527 + 23) >> 6;
#endif
                 R_out = (R_in * 527 + 23) >> 6;
                 B_out = (B_in * 527 + 23) >> 6;

                 outbuf[row_start + pix_cnt] = A_out << 24 | R_out << 16 | G_out << 8 | B_out;
            }
       }
*/
        
        m->base.unlock(&m->base, buffer); 
        m->base.unlock(&m->base, m->framebuffer); 
    }
    
    return 0;
}

/*****************************************************************************/

int mapFrameBufferLocked(struct private_module_t* module)
{
    // already initialized...
    if (module->framebuffer) {
        return 0;
    }
        
    char const * const device_template[] = {
            "/dev/graphics/fb%u",
            "/dev/fb%u",
            0 };

    int fd = -1;
    int i=0;
    char name[64];

    while ((fd==-1) && device_template[i]) {
        snprintf(name, 64, device_template[i], 0);
        fd = open(name, O_RDWR, 0);
        i++;
    }
    if (fd < 0)
        return -errno;

    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    struct fb_var_screeninfo info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;

    info.reserved[0] = 0;
    info.reserved[1] = 0;
    info.reserved[2] = 0;
    info.xoffset = 0;
    info.yoffset = 0;
    info.activate = FB_ACTIVATE_NOW;

    info.bits_per_pixel = 16;
    info.red.offset     = 11;
    info.red.length     = 5;
    info.green.offset   = 5;
    info.green.length   = 6;
    info.blue.offset    = 0;
    info.blue.length    = 5;
    info.transp.offset  = 0;
    info.transp.length  = 0;

    /*
     * Request NUM_BUFFERS screens (at lest 2 for page flipping)
     */
    LOGI("1 yres = %d yres_virtual = %d", info.yres, info.yres_virtual);
    info.yres_virtual = info.yres * NUM_BUFFERS;
    //info.yres = info.yres_virtual / 2;
    LOGI("2 yres = %d yres_virtual = %d", info.yres, info.yres_virtual);

    uint32_t flags = PAGE_FLIP;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &info) == -1) {
        info.yres_virtual = info.yres;
        flags &= ~PAGE_FLIP;
        LOGW("FBIOPUT_VSCREENINFO failed, page flipping not supported");
    }

    //info.yres = info.yres_virtual / 2;
    LOGI("3 yres = %d yres_virtual = %d", info.yres, info.yres_virtual);
    if (info.yres_virtual < info.yres * 2) {
        // we need at least 2 for page-flipping
        info.yres_virtual = info.yres;
        flags &= ~PAGE_FLIP;
        LOGW("page flipping not supported (yres_virtual=%d, requested=%d)",
                info.yres_virtual, info.yres*2);
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == -1)
        return -errno;
    //info.yres = info.yres_virtual / 2;
    LOGI("4 yres = %d yres_virtual = %d", info.yres, info.yres_virtual);

    uint64_t  refreshQuotient =
    (
            uint64_t( info.upper_margin + info.lower_margin + info.yres )
            * ( info.left_margin  + info.right_margin + info.xres )
            * info.pixclock
    );

    /* Beware, info.pixclock might be 0 under emulation, so avoid a
     * division-by-0 here (SIGFPE on ARM) */
    int refreshRate = refreshQuotient > 0 ? (int)(1000000000000000LLU / refreshQuotient) : 0;

    //if (refreshRate == 0) {
        // bleagh, bad info from the driver
        refreshRate = 60*1000;  // 60 Hz
    //}

    if (int(info.width) <= 0 || int(info.height) <= 0) {
        // the driver doesn't return that information
        // default to 160 dpi
        info.width  = ((info.xres * 25.4f)/160.0f + 0.5f);
        info.height = ((info.yres * 25.4f)/160.0f + 0.5f);
    }

    float xdpi = (info.xres * 25.4f) / info.width;
    float ydpi = (info.yres * 25.4f) / info.height;
    float fps  = refreshRate / 1000.0f;

    info.bits_per_pixel = 16;
    info.red.offset     = 11;
    info.red.length     = 5;
    info.green.offset   = 5;
    info.green.length   = 6;
    info.blue.offset    = 0;
    info.blue.length    = 5;
    info.transp.offset  = 0;
    info.transp.length  = 0;

    LOGI(   "using (fd=%d)\n"
            "id           = %s\n"
            "xres         = %d px\n"
            "yres         = %d px\n"
            "xres_virtual = %d px\n"
            "yres_virtual = %d px\n"
            "bpp          = %d\n"
            "r            = %2u:%u\n"
            "g            = %2u:%u\n"
            "b            = %2u:%u\n",
            fd,
            finfo.id,
            info.xres,
            info.yres,
            info.xres_virtual,
            info.yres_virtual,
            info.bits_per_pixel,
            info.red.offset, info.red.length,
            info.green.offset, info.green.length,
            info.blue.offset, info.blue.length
    );

    LOGI(   "width        = %d mm (%f dpi)\n"
            "height       = %d mm (%f dpi)\n"
            "refresh rate = %.2f Hz\n",
            info.width,  xdpi,
            info.height, ydpi,
            fps
    );


    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == -1)
        return -errno;

    if (finfo.smem_len <= 0)
        return -errno;


    module->finfo = finfo;
    module->xdpi = xdpi;
    module->ydpi = ydpi;
    module->fps = fps;

    /*
     * map the framebuffer
     */

    while (info.yres_virtual > 0) {
        size_t fbSize = roundUpToPageSize(finfo.line_length * info.yres_virtual);
        module->numBuffers = info.yres_virtual / info.yres;
        void* vaddr = mmap(0, fbSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (vaddr != MAP_FAILED) {
            module->info = info;
            module->flags = flags;
            module->bufferMask = 0;
            module->framebuffer = new private_handle_t(dup(fd), fbSize, 0);
            module->framebuffer->base = intptr_t(vaddr);
            memset(vaddr, 0, fbSize);
            LOGI("using %d buffers", module->numBuffers);
            return 0;
        }

        LOGE("Error mapping the framebuffer (%s)", strerror(errno));

        info.yres_virtual -= info.yres;
        LOGW("Fallback to use fewer buffer: %d", info.yres_virtual / info.yres);
        if (ioctl(fd, FBIOPUT_VSCREENINFO, &info) == -1)
            break;

        if (info.yres_virtual <= info.yres)
            flags &= ~PAGE_FLIP;
    }

    return -errno;
}

static int mapFrameBuffer(struct private_module_t* module)
{
    pthread_mutex_lock(&module->lock);
    int err = mapFrameBufferLocked(module);
    pthread_mutex_unlock(&module->lock);
    return err;
}

/*****************************************************************************/

static int fb_close(struct hw_device_t *dev)
{
    fb_context_t* ctx = (fb_context_t*)dev;
    if (ctx) {
        free(ctx);
    }
    return 0;
}

int fb_device_open(hw_module_t const* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_FB0)) {
        alloc_device_t* gralloc_device;
        status = gralloc_open(module, &gralloc_device);
        if (status < 0)
            return status;

        /* initialize our state here */
        fb_context_t *dev = (fb_context_t*)malloc(sizeof(*dev));
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = fb_close;
        dev->device.setSwapInterval = fb_setSwapInterval;
        dev->device.post            = fb_post;
        dev->device.setUpdateRect = 0;

        private_module_t* m = (private_module_t*)module;
        status = mapFrameBuffer(m);
        if (status >= 0) {
            int stride = m->finfo.line_length / (m->info.bits_per_pixel >> 3);
            /*
             * Auto detect current depth and select mode
             */
            int format;
            //m->info.bits_per_pixel = 16;
            //m->info.green.length = 6;
            //m->info.red.length = 6;
            //m->info.blue.length = 6;
            if (m->info.bits_per_pixel == 32) {
                LOGI("[GRALLOC] HAL_PIXEL_FORMAT_BGRA_8888 %d", HAL_PIXEL_FORMAT_BGRA_8888);
                LOGI("[GRALLOC] HAL_PIXEL_FORMAT_RGBA_8888 %d", HAL_PIXEL_FORMAT_RGBA_8888);
                LOGI("[GRALLOC] HAL_PIXEL_FORMAT_RGBX_8888 %d", HAL_PIXEL_FORMAT_RGBX_8888);
                format = (m->info.red.offset == 16) ? HAL_PIXEL_FORMAT_BGRA_8888
                       : (m->info.red.offset == 24) ? HAL_PIXEL_FORMAT_RGBA_8888
                       : HAL_PIXEL_FORMAT_RGBX_8888;
                format = HAL_PIXEL_FORMAT_RGBX_8888;
                LOGI("[GRALLOC] format %d", format);
            } else if (m->info.bits_per_pixel == 16) {
                LOGI("[GRALLOC] HAL_PIXEL_FORMAT_RGB_565 %d", HAL_PIXEL_FORMAT_RGB_565);
                LOGI("[GRALLOC] HAL_PIXEL_FORMAT_RGBA_5551 %d", HAL_PIXEL_FORMAT_RGBA_5551);
                format = (m->info.green.length == 6) ?
                         HAL_PIXEL_FORMAT_RGB_565 : HAL_PIXEL_FORMAT_RGBA_5551;
                //format = HAL_PIXEL_FORMAT_RGBA_5551;
                LOGI("[GRALLOC] format %d", format);
            } else {
                LOGE("Unsupported format %d", m->info.bits_per_pixel);
                return -EINVAL;
            }
            const_cast<uint32_t&>(dev->device.flags) = 0;
            const_cast<uint32_t&>(dev->device.width) = m->info.xres;
            const_cast<uint32_t&>(dev->device.height) = m->info.yres;
            const_cast<int&>(dev->device.stride) = stride;
            const_cast<int&>(dev->device.format) = format;
            const_cast<float&>(dev->device.xdpi) = m->xdpi;
            const_cast<float&>(dev->device.ydpi) = m->ydpi;
            const_cast<float&>(dev->device.fps) = m->fps;
            const_cast<int&>(dev->device.minSwapInterval) = 1;
            const_cast<int&>(dev->device.maxSwapInterval) = 1;
            *device = &dev->device.common;
        }
    }
    return status;
}
