/*
 * Runtime loader for the detect_cuda TensorRT plugin.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"

#include "compat/w32dlfcn.h"
#include "vf_detect_cuda_trt.h"

#define DETECT_CUDA_TRT_PLUGIN "libavfilter_detect_cuda_trt" SLIBSUF

typedef int (*ff_detect_trt_create_fn)(FFDetectTRTContext **trt,
                                       const FFDetectTRTConfig *cfg,
                                       char *errbuf, int errbuf_size);
typedef int (*ff_detect_trt_resolve_cache_path_fn)(const FFDetectTRTConfig *cfg,
                                                   char *out_path, int out_path_size,
                                                   char *errbuf, int errbuf_size);
typedef int (*ff_detect_trt_get_info_fn)(const FFDetectTRTContext *trt,
                                         FFDetectTRTModelInfo *info);
typedef int (*ff_detect_trt_execute_fn)(FFDetectTRTContext *trt,
                                        const FFDetectTRTExecuteParams *params,
                                        char *errbuf, int errbuf_size);
typedef void (*ff_detect_trt_destroy_fn)(FFDetectTRTContext **trt);

static void *trt_plugin_handle;
static ff_detect_trt_create_fn  p_create;
static ff_detect_trt_resolve_cache_path_fn p_resolve_cache_path;
static ff_detect_trt_get_info_fn p_get_info;
static ff_detect_trt_execute_fn  p_execute;
static ff_detect_trt_destroy_fn  p_destroy;

static void set_err(char *errbuf, int errbuf_size, const char *msg)
{
    if (errbuf && errbuf_size > 0)
        snprintf(errbuf, errbuf_size, "%s", msg ? msg : "unknown error");
}

static int trt_plugin_load_symbol(void *sym, const char *name, char *errbuf, int errbuf_size)
{
    void *p = dlsym(trt_plugin_handle, name);
    if (!p) {
        set_err(errbuf, errbuf_size, dlerror());
        return AVERROR(EINVAL);
    }
    *(void **)sym = p;
    return 0;
}

static int trt_plugin_try_open(const char *path, char *errbuf, int errbuf_size)
{
    trt_plugin_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!trt_plugin_handle) {
        set_err(errbuf, errbuf_size, dlerror());
        return AVERROR(EINVAL);
    }

    if (trt_plugin_load_symbol((void *)&p_create, "ff_detect_trt_create",
                               errbuf, errbuf_size) < 0 ||
        trt_plugin_load_symbol((void *)&p_resolve_cache_path,
                               "ff_detect_trt_resolve_cache_path",
                               errbuf, errbuf_size) < 0 ||
        trt_plugin_load_symbol((void *)&p_get_info, "ff_detect_trt_get_info",
                               errbuf, errbuf_size) < 0 ||
        trt_plugin_load_symbol((void *)&p_execute, "ff_detect_trt_execute",
                               errbuf, errbuf_size) < 0 ||
        trt_plugin_load_symbol((void *)&p_destroy, "ff_detect_trt_destroy",
                               errbuf, errbuf_size) < 0) {
        dlclose(trt_plugin_handle);
        trt_plugin_handle = NULL;
        p_create = p_resolve_cache_path = p_get_info = p_execute = p_destroy = NULL;
        return AVERROR(EINVAL);
    }

    return 0;
}

static int trt_plugin_try_dir(const char *dir, char *errbuf, int errbuf_size)
{
    char *path;
    int ret;

    if (!dir || !dir[0])
        return AVERROR(EINVAL);

    path = av_asprintf("%s/%s", dir, DETECT_CUDA_TRT_PLUGIN);
    if (!path)
        return AVERROR(ENOMEM);

    ret = trt_plugin_try_open(path, errbuf, errbuf_size);
    av_free(path);
    return ret;
}

#if !HAVE_WINRT && !defined(_WIN32)
#include <unistd.h>
#include <limits.h>

static int trt_plugin_try_exe_dir(char *errbuf, int errbuf_size)
{
    char exe_path[PATH_MAX];
    char *slash;
    ssize_t len;

    len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0)
        return AVERROR(EINVAL);
    exe_path[len] = '\0';

    slash = strrchr(exe_path, '/');
    if (!slash)
        return AVERROR(EINVAL);
    *slash = '\0';

    return trt_plugin_try_dir(exe_path, errbuf, errbuf_size);
}
#endif

static void trt_plugin_save_err(char **last_err, const char *errbuf)
{
    av_freep(last_err);
    if (errbuf && errbuf[0])
        *last_err = av_strdup(errbuf);
}

static int trt_plugin_load(char *errbuf, int errbuf_size)
{
    static const char *const system_dirs[] = {
        "/usr/local/lib",
        "/usr/lib64",
        "/usr/lib",
        NULL,
    };
    const char *env_path;
    char *last_err = NULL;
    int i, ret;

    if (trt_plugin_handle)
        return 0;

    /* 1) Directory of the running ffmpeg/ffprobe binary */
#if !HAVE_WINRT && !defined(_WIN32)
    ret = trt_plugin_try_exe_dir(errbuf, errbuf_size);
    if (ret >= 0)
        return ret;
    trt_plugin_save_err(&last_err, errbuf);
#endif

    /* 2) Explicit override: env path, then compile-time plugin dir */
    env_path = getenv("FFMPEG_DETECT_CUDA_TRT_PATH");
    if (env_path && env_path[0]) {
        ret = trt_plugin_try_open(env_path, errbuf, errbuf_size);
        if (ret >= 0) {
            av_freep(&last_err);
            return ret;
        }
        ret = trt_plugin_try_dir(env_path, errbuf, errbuf_size);
        if (ret >= 0) {
            av_freep(&last_err);
            return ret;
        }
        trt_plugin_save_err(&last_err, errbuf);
    }

#ifdef DETECT_CUDA_TRT_PLUGINDIR
    ret = trt_plugin_try_dir(DETECT_CUDA_TRT_PLUGINDIR, errbuf, errbuf_size);
    if (ret >= 0) {
        av_freep(&last_err);
        return ret;
    }
    trt_plugin_save_err(&last_err, errbuf);
#endif

    /* 3) Common system library directories */
    for (i = 0; system_dirs[i]; i++) {
        ret = trt_plugin_try_dir(system_dirs[i], errbuf, errbuf_size);
        if (ret >= 0) {
            av_freep(&last_err);
            return ret;
        }
        trt_plugin_save_err(&last_err, errbuf);
    }

    set_err(errbuf, errbuf_size,
            last_err ? last_err :
            "TensorRT plugin not found; place " DETECT_CUDA_TRT_PLUGIN
            " next to ffmpeg, under /usr/local/lib or /usr/lib, "
            "or set FFMPEG_DETECT_CUDA_TRT_PATH");
    av_freep(&last_err);
    return AVERROR(EINVAL);
}

int ff_detect_trt_create(FFDetectTRTContext **trt,
                         const FFDetectTRTConfig *cfg,
                         char *errbuf, int errbuf_size)
{
    int ret = trt_plugin_load(errbuf, errbuf_size);
    if (ret < 0)
        return ret;
    return p_create(trt, cfg, errbuf, errbuf_size);
}

int ff_detect_trt_resolve_cache_path(const FFDetectTRTConfig *cfg,
                                     char *out_path, int out_path_size,
                                     char *errbuf, int errbuf_size)
{
    int ret = trt_plugin_load(errbuf, errbuf_size);
    if (ret < 0)
        return ret;
    return p_resolve_cache_path(cfg, out_path, out_path_size, errbuf, errbuf_size);
}

int ff_detect_trt_get_info(const FFDetectTRTContext *trt,
                           FFDetectTRTModelInfo *info)
{
    if (!p_get_info)
        return AVERROR(EINVAL);
    return p_get_info(trt, info);
}

int ff_detect_trt_execute(FFDetectTRTContext *trt,
                          const FFDetectTRTExecuteParams *params,
                          char *errbuf, int errbuf_size)
{
    if (!p_execute) {
        set_err(errbuf, errbuf_size, "TensorRT plugin is not loaded");
        return AVERROR(EINVAL);
    }
    return p_execute(trt, params, errbuf, errbuf_size);
}

void ff_detect_trt_destroy(FFDetectTRTContext **trt)
{
    if (p_destroy)
        p_destroy(trt);
}
