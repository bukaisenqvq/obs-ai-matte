#pragma once

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 滤镜描述符（在 ai_matte_filter.cpp 中定义） */
extern struct obs_source_info ai_matte_filter_info;

/* ONNX Runtime 全局初始化 / 释放，由模块加载时调用 */
void ai_matte_init_ort(void);
void ai_matte_release_ort(void);

#ifdef __cplusplus
}
#endif
