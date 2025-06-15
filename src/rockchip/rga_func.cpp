// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rga_func.h"
#include "../config.h"

int rknn_rga_init(rga_context *rga_ctx)
{
#if ENABLE_RGA_HARDWARE
    rga_ctx->rga_handle = dlopen("/usr/lib/aarch64-linux-gnu/librga.so", RTLD_LAZY);
    if (!rga_ctx->rga_handle)
    {
        printf("dlopen /usr/lib/aarch64-linux-gnu/librga.so failed\n");
        return -1;
    }
    rga_ctx->init_func = (FUNC_RGA_INIT)dlsym(rga_ctx->rga_handle, "c_RkRgaInit");
    rga_ctx->deinit_func = (FUNC_RGA_DEINIT)dlsym(rga_ctx->rga_handle, "c_RkRgaDeInit");
    rga_ctx->blit_func = (FUNC_RGA_BLIT)dlsym(rga_ctx->rga_handle, "c_RkRgaBlit");

    if (!rga_ctx->init_func || !rga_ctx->deinit_func || !rga_ctx->blit_func) {
        printf("Failed to load RGA functions\n");
        return -1;
    }

    rga_ctx->init_func();
    return 0;
#else
    // RGA disabled - return success but don't initialize
    memset(rga_ctx, 0, sizeof(rga_context));
    return 0;
#endif
}

int rknn_img_resize_phy_to_phy(rga_context *rga_ctx, int src_fd, int src_w, int src_h, int src_fmt, uint64_t dst_fd, int dst_w, int dst_h, int dst_fmt)
{
#if !ENABLE_RGA_HARDWARE
    // RGA disabled - return error to trigger software fallback
    return -1;
#else
    int ret = 0;

    // Parameter validation
    if (!rga_ctx || !rga_ctx->rga_handle) {
        return -1; // Suppress error message when RGA is intentionally disabled
    }

    if (src_fd < 0 || dst_fd < 0) {
        printf("Invalid file descriptors: src_fd=%d, dst_fd=%llu\n", src_fd, dst_fd);
        return -1;
    }

    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        printf("Invalid dimensions: src=%dx%d, dst=%dx%d\n", src_w, src_h, dst_w, dst_h);
        return -1;
    }

    if (src_w > 4096 || src_h > 4096 || dst_w > 4096 || dst_h > 4096) {
        printf("Dimensions too large: src=%dx%d, dst=%dx%d (max 4096x4096)\n", src_w, src_h, dst_w, dst_h);
        return -1;
    }

    rga_info_t src, dst;

    memset(&src, 0, sizeof(rga_info_t));
    src.fd = src_fd;
    src.mmuFlag = 1;
    // src.rotation = rotation;

    memset(&dst, 0, sizeof(rga_info_t));
    dst.fd = dst_fd;
    dst.mmuFlag = 0;
    dst.nn.nn_flag = 0;

    // Calculate proper stride for different formats
    int src_stride = src_w;
    int dst_stride = dst_w;

    // For YUV420 formats, use width as stride (RGA handles alignment internally)
    if (src_fmt == RK_FORMAT_YCbCr_420_SP || src_fmt == RK_FORMAT_YCbCr_420_P) {
        src_stride = src_w;  // Use actual width, not aligned
    }

    // For RGB formats, stride equals width (not width * bytes_per_pixel)
    if (dst_fmt == RK_FORMAT_RGB_888 || dst_fmt == RK_FORMAT_BGR_888) {
        dst_stride = dst_w;  // RGA expects width, not byte stride
    }

    rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_stride, src_h, src_fmt);
    rga_set_rect(&dst.rect, 0, 0, dst_w, dst_h, dst_stride, dst_h, dst_fmt);

    ret = rga_ctx->blit_func(&src, &dst, NULL);
    if (ret != 0) {
        printf("RGA blit failed: ret=%d, src=%dx%d(fd=%d), dst=%dx%d(fd=%llu)\n",
               ret, src_w, src_h, src_fd, dst_w, dst_h, dst_fd);
    }

    return ret;
#endif
}

int rknn_img_resize_phy_to_virt(rga_context *rga_ctx, int src_fd, int src_w, int src_h, int src_fmt, void *dst_virt, int dst_w, int dst_h, int dst_fmt)
{
#if !ENABLE_RGA_HARDWARE
    return -1;
#else
    int ret = 0;

    if (rga_ctx->rga_handle)
    {
        rga_info_t src, dst;

        memset(&src, 0, sizeof(rga_info_t));
        src.fd = src_fd;
        src.mmuFlag = 1;
        // src.rotation = rotation;

        memset(&dst, 0, sizeof(rga_info_t));
        dst.fd = -1;
        dst.mmuFlag = 1;
        dst.virAddr = dst_virt;
        dst.nn.nn_flag = 0;

        rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h, src_fmt);
        rga_set_rect(&dst.rect, 0, 0, dst_w, dst_h, dst_w, dst_h, dst_fmt);

        return rga_ctx->blit_func(&src, &dst, NULL);
    }
    return ret;
#endif
}

int rknn_img_resize_virt_to_phy(rga_context *rga_ctx, void *src_virt, int src_w, int src_h, int src_fmt, uint64_t dst_fd, int dst_w, int dst_h, int dst_fmt)
{
#if !ENABLE_RGA_HARDWARE
    return -1;
#else
    int ret = 0;

    if (rga_ctx->rga_handle)
    {
        rga_info_t src, dst;

        memset(&src, 0, sizeof(rga_info_t));
        src.fd = -1;
        src.mmuFlag = 1;
        src.virAddr = (void *)src_virt;
        // src.rotation = rotation;

        memset(&dst, 0, sizeof(rga_info_t));
        dst.fd = dst_fd;
        dst.mmuFlag = 0;
        dst.nn.nn_flag = 0;

        rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h, src_fmt);
        rga_set_rect(&dst.rect, 0, 0, dst_w, dst_h, dst_w, dst_h, dst_fmt);

        return rga_ctx->blit_func(&src, &dst, NULL);
    }
    return ret;
#endif
}

int rknn_img_resize_virt_to_virt(rga_context *rga_ctx, void *src_virt, int src_w, int src_h, int src_fmt, void *dst_virt, int dst_w, int dst_h, int dst_fmt)
{
#if !ENABLE_RGA_HARDWARE
    return -1;
#else
    int ret = 0;

    if (rga_ctx->rga_handle)
    {
        rga_info_t src, dst;

        memset(&src, 0, sizeof(rga_info_t));
        src.fd = -1;
        src.mmuFlag = 1;
        src.virAddr = (void *)src_virt;
        // src.rotation = rotation;

        memset(&dst, 0, sizeof(rga_info_t));
        dst.fd = -1;
        dst.mmuFlag = 1;
        dst.virAddr = dst_virt;
        dst.nn.nn_flag = 0;

        rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h, src_fmt);
        rga_set_rect(&dst.rect, 0, 0, dst_w, dst_h, dst_w, dst_h, dst_fmt);

        return rga_ctx->blit_func(&src, &dst, NULL);
    }
    return ret;
#endif
}

int rknn_rga_deinit(rga_context *rga_ctx)
{
#if ENABLE_RGA_HARDWARE
    if (rga_ctx->rga_handle)
    {
        dlclose(rga_ctx->rga_handle);
        rga_ctx->rga_handle = NULL;
    }
#endif
    return 0;
}
