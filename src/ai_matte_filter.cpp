#include "ai_matte_filter.hpp"

#include <obs-module.h>
#include <obs-data.h>
#include <obs-properties.h>
#include <util/platform.h>
#include <util/base.h>
#include <util/bmem.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// 必须先用 USE_DML 打开 OrtApi 里的 DirectML 入口，否则结构体中没有该成员
#ifndef USE_DML
#define USE_DML 1
#endif

// 官方发布包里头文件平铺在 include/ 下，不是源码仓库的 onnxruntime/core/session/ 路径
#include <onnxruntime_c_api.h>

/* OrtDmlApi 的最小声明。
 * ORT 1.20 起 DirectML 不再挂在 OrtApi 上，而是走 provider bridge：
 *   GetExecutionProviderApi("DML", ORT_API_VERSION, &ptr)
 * 返回的指针即是 OrtDmlApi*，其第一个成员就是
 * SessionOptionsAppendExecutionProvider_DML。
 * dml_provider_factory.h 不一定随发布包分发，故这里自行声明，
 * 只取第一个成员，偏移为 0，安全。 */
struct OrtDmlApiMinimal {
	OrtStatusPtr(ORT_API_CALL* SessionOptionsAppendExecutionProvider_DML)(
		OrtSessionOptions* options, int device_id);
};

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <utility>
#include <string>
#include <vector>

/* ===================== ONNX Runtime 全局 ===================== */
static const OrtApi* g_ort = nullptr;
static OrtEnv* g_env = nullptr;
static OrtMemoryInfo* g_mem_info = nullptr;

void ai_matte_init_ort(void)
{
	g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
	if (!g_ort)
		return;
	g_ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "obs-ai-matte", &g_env);
	g_ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault,
				   &g_mem_info);
}

void ai_matte_release_ort(void)
{
	if (g_mem_info) {
		g_ort->ReleaseMemoryInfo(g_mem_info);
		g_mem_info = nullptr;
	}
	if (g_env) {
		g_ort->ReleaseEnv(g_env);
		g_env = nullptr;
	}
	g_ort = nullptr;
}

/* ===================== 模型定义 ===================== */
struct ModelDef {
	const char* file;
	const char* input;
	const char* output;
	float mean[3];
	float std[3];
	int size;
};

static const ModelDef kModels[] = {
	{"isnet-general-use.onnx", "input_image", "output_image",
	 {0.5f, 0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, 1024},
	{"u2net_human_seg.onnx", "input.1", "1959",
	 {0.485f, 0.456f, 0.406f}, {0.229f, 0.224f, 0.225f}, 320},
};
static const int kNumModels = 2;

enum BgMode { BG_TRANSPARENT = 0, BG_SOLID = 1, BG_IMAGE = 2, BG_BLUR = 3 };

/* ===================== 滤镜数据 ===================== */
struct ai_matte_data {
	obs_source_t* context;
	int model_index;
	int bg_mode;
	uint32_t bg_color;
	bool has_bg_image;
	int blur_radius;
	int feather;
	bool decontaminate;

	OrtSession* session;
	const ModelDef* model;

	std::vector<uint8_t> bg_rgba;
	int bg_w, bg_h;
	std::string bg_loaded_path;
	std::vector<uint8_t> bg_scaled;
	int bg_scaled_w, bg_scaled_h;
};

/* ===================== 工具函数 ===================== */
static std::string find_model_path(const char* filename)
{
	/* 1) 环境变量覆盖 */
	const char* env = getenv("OBS_AI_MATTE_MODEL_DIR");
	if (env && *env) {
		std::string p = std::string(env) + "/" + filename;
		FILE* f = fopen(p.c_str(), "rb");
		if (f) {
			fclose(f);
			return p;
		}
	}
	/* 2) OBS data 目录：exe 位于 bin/64bit/obs64.exe，data 在 obs-studio/data/
	 *    因此从 exe 目录需要 ../../data/obs-plugins/obs-ai-matte/ */
	char exe[1024] = {0};
	bool got_exe = false;
#ifdef _WIN32
	/* 不用 os_get_executable_path（各 OBS 版本可用性不稳定），直接用 Win32 API */
	DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
	got_exe = (n > 0 && n < sizeof(exe));
#endif
	if (got_exe) {
		std::string dir = exe;
		size_t sl = dir.find_last_of("/\\");
		if (sl != std::string::npos)
			dir = dir.substr(0, sl);
		std::string p =
			dir + "/../../data/obs-plugins/obs-ai-matte/" + filename;
		FILE* f = fopen(p.c_str(), "rb");
		if (f) {
			fclose(f);
			return p;
		}
	}
	/* 3) 当前工作目录兜底 */
	{
		std::string p =
			std::string("data/obs-plugins/obs-ai-matte/") + filename;
		FILE* f = fopen(p.c_str(), "rb");
		if (f) {
			fclose(f);
			return p;
		}
	}
	return "";
}

static bool load_session(ai_matte_data* f, int model_index)
{
	if (f->session) {
		g_ort->ReleaseSession(f->session);
		f->session = nullptr;
	}
	if (model_index < 0 || model_index >= kNumModels)
		model_index = 0;
	f->model = &kModels[model_index];
	f->model_index = model_index;

	std::string path = find_model_path(f->model->file);
	if (path.empty()) {
		blog(LOG_ERROR, "[obs-ai-matte] model not found: %s",
		     f->model->file);
		return false;
	}

	OrtSessionOptions* sopts = nullptr;
	g_ort->CreateSessionOptions(&sopts);
#ifdef _WIN32
	/* 优先 DirectML（独显加速），不可用则自动回退 CPU */
	{
		const void* dml_raw = nullptr;
		OrtStatus* st = g_ort->GetExecutionProviderApi(
			"DML", ORT_API_VERSION, &dml_raw);
		if (st) {
			blog(LOG_WARNING,
			     "[obs-ai-matte] GetExecutionProviderApi(DML) failed, fall back to CPU");
			g_ort->ReleaseStatus(st);
		} else if (dml_raw) {
			const OrtDmlApiMinimal* dml =
				(const OrtDmlApiMinimal*)dml_raw;
			OrtStatus* ds =
				dml->SessionOptionsAppendExecutionProvider_DML(
					sopts, 0);
			if (ds) {
				blog(LOG_WARNING,
				     "[obs-ai-matte] DML unavailable, fall back to CPU: %s",
				     g_ort->GetErrorMessage(ds));
				g_ort->ReleaseStatus(ds);
			}
		}
	}
#endif

	std::wstring wpath(path.begin(), path.end());
	OrtStatus* s =
		g_ort->CreateSession(g_env, wpath.c_str(), sopts, &f->session);
	g_ort->ReleaseSessionOptions(sopts);
	if (s) {
		blog(LOG_ERROR, "[obs-ai-matte] CreateSession failed: %s",
		     g_ort->GetErrorMessage(s));
		g_ort->ReleaseStatus(s);
		return false;
	}
	blog(LOG_INFO, "[obs-ai-matte] loaded model %s", f->model->file);
	return true;
}

static void free_bg(ai_matte_data* f)
{
	f->bg_rgba.clear();
	f->bg_w = f->bg_h = 0;
	f->bg_loaded_path.clear();
	f->bg_scaled.clear();
	f->bg_scaled_w = f->bg_scaled_h = 0;
}

static void load_bg_image(ai_matte_data* f, const char* path)
{
	free_bg(f);
	f->has_bg_image = false;
	if (!path || !*path)
		return;
	int w, h, ch;
	unsigned char* data = stbi_load(path, &w, &h, &ch, 4);
	if (!data)
		return;
	f->bg_rgba.assign(data, data + (size_t)w * h * 4);
	f->bg_w = w;
	f->bg_h = h;
	f->bg_loaded_path = path;
	f->has_bg_image = true;
	stbi_image_free(data);
}

/* 缩放（bilinear）：RGBA(src, stride) -> RGB(dst) */
static void resize_rgb_bilinear(const uint8_t* src, int sw, int sh,
				int sstride, uint8_t* dst, int dw, int dh)
{
	for (int y = 0; y < dh; y++) {
		float fy = (sh == 1) ? 0.0f : (y + 0.5f) * sh / dh - 0.5f;
		int y0 = (int)floorf(fy);
		float fyf = fy - y0;
		if (y0 < 0) {
			y0 = 0;
			fyf = 0;
		}
		int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
		for (int x = 0; x < dw; x++) {
			float fx = (sw == 1) ? 0.0f : (x + 0.5f) * sw / dw - 0.5f;
			int x0 = (int)floorf(fx);
			float fxf = fx - x0;
			if (x0 < 0) {
				x0 = 0;
				fxf = 0;
			}
			int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
			for (int c = 0; c < 3; c++) {
				int i00 = y0 * sstride + x0 * 4 + c;
				int i01 = y0 * sstride + x1 * 4 + c;
				int i10 = y1 * sstride + x0 * 4 + c;
				int i11 = y1 * sstride + x1 * 4 + c;
				float v = (src[i00] * (1 - fxf) + src[i01] * fxf) *
						  (1 - fyf) +
					  (src[i10] * (1 - fxf) + src[i11] * fxf) * fyf;
				int iv = (int)(v + 0.5f);
				if (iv < 0)
					iv = 0;
				if (iv > 255)
					iv = 255;
				dst[(y * dw + x) * 3 + c] = (uint8_t)iv;
			}
		}
	}
}

/* 缩放（bilinear）：单通道 float(0..1) mask -> uint8(0..255) */
static void resize_mask_bilinear(const float* src, int sw, int sh,
				 uint8_t* dst, int dw, int dh)
{
	for (int y = 0; y < dh; y++) {
		float fy = (sh == 1) ? 0.0f : (y + 0.5f) * sh / dh - 0.5f;
		int y0 = (int)floorf(fy);
		float fyf = fy - y0;
		if (y0 < 0) {
			y0 = 0;
			fyf = 0;
		}
		int y1 = (y0 + 1 < sh) ? y0 + 1 : y0;
		for (int x = 0; x < dw; x++) {
			float fx = (sw == 1) ? 0.0f : (x + 0.5f) * sw / dw - 0.5f;
			int x0 = (int)floorf(fx);
			float fxf = fx - x0;
			if (x0 < 0) {
				x0 = 0;
				fxf = 0;
			}
			int x1 = (x0 + 1 < sw) ? x0 + 1 : x0;
			float v = (src[y0 * sw + x0] * (1 - fxf) +
				   src[y0 * sw + x1] * fxf) *
					  (1 - fyf) +
				  (src[y1 * sw + x0] * (1 - fxf) +
				   src[y1 * sw + x1] * fxf) *
					  fyf;
			int iv = (int)(v * 255.0f + 0.5f);
			if (iv < 0)
				iv = 0;
			if (iv > 255)
				iv = 255;
			dst[y * dw + x] = (uint8_t)iv;
		}
	}
}

/* box blur（羽化 / 去溢色 / 虚化背景） */
static void box_blur(const uint8_t* src, uint8_t* tmp, uint8_t* dst,
		     int w, int h, int radius)
{
	if (radius <= 0) {
		memcpy(dst, src, (size_t)w * h);
		return;
	}
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int sum = 0, cnt = 0;
			for (int k = -radius; k <= radius; k++) {
				int xx = x + k;
				if (xx >= 0 && xx < w) {
					sum += src[y * w + xx];
					cnt++;
				}
			}
			tmp[y * w + x] = (uint8_t)(sum / cnt);
		}
	}
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int sum = 0, cnt = 0;
			for (int k = -radius; k <= radius; k++) {
				int yy = y + k;
				if (yy >= 0 && yy < h) {
					sum += tmp[yy * w + x];
					cnt++;
				}
			}
			dst[y * w + x] = (uint8_t)(sum / cnt);
		}
	}
}

/* ===================== OBS 回调 ===================== */
static void ai_matte_update(void* data, obs_data_t* settings);

static const char* ai_matte_get_name(void* private_data)
{
	UNUSED_PARAMETER(private_data);
	return "AI 抠图滤镜 (DirectML)";
}

static void* ai_matte_create(obs_data_t* settings, obs_source_t* source)
{
	auto* f = (ai_matte_data*)bzalloc(sizeof(ai_matte_data));
	f->context = source;
	f->bg_scaled_w = f->bg_scaled_h = 0;

	if (g_ort && g_env) {
		int mi = (int)obs_data_get_int(settings, "model");
		load_session(f, mi);
		load_bg_image(f, obs_data_get_string(settings, "bg_image"));
	}
	ai_matte_update(f, settings);
	return f;
}

static void ai_matte_destroy(void* data)
{
	auto* f = (ai_matte_data*)data;
	if (!f)
		return;
	if (f->session)
		g_ort->ReleaseSession(f->session);
	free_bg(f);
	bfree(f);
}

static void ai_matte_update(void* data, obs_data_t* settings)
{
	auto* f = (ai_matte_data*)data;
	int mi = (int)obs_data_get_int(settings, "model");
	if (mi != f->model_index || !f->session)
		load_session(f, mi);

	f->bg_mode = (int)obs_data_get_int(settings, "bg_mode");
	f->bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");
	f->blur_radius = (int)obs_data_get_int(settings, "blur_radius");
	f->feather = (int)obs_data_get_int(settings, "feather");
	f->decontaminate = obs_data_get_bool(settings, "decontaminate");

	const char* bp = obs_data_get_string(settings, "bg_image");
	std::string bps = bp ? bp : "";
	if (bps != f->bg_loaded_path)
		load_bg_image(f, bp);
}

static obs_properties_t* ai_matte_get_properties(void* data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t* p = obs_properties_create();

	obs_property_t* pm = obs_properties_add_list(
		p, "model", "AI 模型", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pm, "通用高精度 (isnet，发丝级)", 0);
	obs_property_list_add_int(pm, "人像专用 (u2net_human_seg)", 1);

	obs_property_t* pb = obs_properties_add_list(
		p, "bg_mode", "背景模式", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pb, "透明背景 (PNG)", BG_TRANSPARENT);
	obs_property_list_add_int(pb, "纯色背景", BG_SOLID);
	obs_property_list_add_int(pb, "自定义背景图", BG_IMAGE);
	obs_property_list_add_int(pb, "原背景虚化", BG_BLUR);

	obs_properties_add_color(p, "bg_color", "背景颜色");
	obs_properties_add_path(p, "bg_image", "背景图片", OBS_PATH_FILE,
				"图片 (*.png *.jpg *.jpeg *.bmp *.webp)",
				nullptr);
	obs_properties_add_int_slider(p, "blur_radius", "虚化半径", 0, 40, 1);
	obs_properties_add_int_slider(p, "feather", "边缘羽化", 0, 10, 1);
	obs_properties_add_bool(p, "decontaminate", "去除溢色（发丝更干净）");

	return p;
}

static void ai_matte_get_defaults(obs_data_t* settings)
{
	obs_data_set_default_int(settings, "model", 0);
	obs_data_set_default_int(settings, "bg_mode", BG_TRANSPARENT);
	obs_data_set_default_int(settings, "bg_color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "blur_radius", 12);
	obs_data_set_default_int(settings, "feather", 0);
	obs_data_set_default_bool(settings, "decontaminate", true);
}

static struct obs_source_frame* ai_matte_filter_video(
	void* data, struct obs_source_frame* frame)
{
	auto* f = (ai_matte_data*)data;
	if (!f || !frame || !f->session)
		return frame;
	if (frame->format != VIDEO_FORMAT_RGBA) {
		/* 仅支持 RGBA（多数摄像头源为 RGBA）；其他格式原样返回 */
		return frame;
	}

	const int W = (int)frame->width;
	const int H = (int)frame->height;
	const uint8_t* src = frame->data[0];
	const int sstride = (int)frame->linesize[0];
	const ModelDef* m = f->model;
	const int S = m->size;

	/* 1) 缩放 RGB -> SxS */
	std::vector<uint8_t> resized((size_t)S * S * 3);
	resize_rgb_bilinear(src, W, H, sstride, resized.data(), S, S);

	/* 2) 预处理：/255 - mean / std -> NCHW float32（复刻 rembg）*/
	std::vector<float> input((size_t)S * S * 3);
	for (int y = 0; y < S; y++) {
		for (int x = 0; x < S; x++) {
			int si = (y * S + x) * 3;
			float r = resized[si] / 255.0f;
			float g = resized[si + 1] / 255.0f;
			float b = resized[si + 2] / 255.0f;
			input[(0 * S + y) * S + x] = (r - m->mean[0]) / m->std[0];
			input[(1 * S + y) * S + x] = (g - m->mean[1]) / m->std[1];
			input[(2 * S + y) * S + x] = (b - m->mean[2]) / m->std[2];
		}
	}

	/* 3) 推理（ORT 分配对齐内存，再拷入输入）*/
	std::vector<int64_t> shape{1, 3, S, S};
	OrtValue* in_t = nullptr;
	OrtAllocator* allocator = nullptr;
	g_ort->GetAllocatorWithDefaultOptions(&allocator);
	g_ort->CreateTensorAsOrtValue(allocator, shape.data(),
				      shape.size(),
				      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
				      &in_t);
	float* in_data = nullptr;
	g_ort->GetTensorMutableData(in_t, (void**)&in_data);
	memcpy(in_data, input.data(), input.size() * sizeof(float));

	const char* in_names[1] = {m->input};
	const char* out_names[1] = {m->output};
	OrtValue* out_t = nullptr;
	OrtStatus* s = g_ort->Run(f->session, nullptr, in_names, &in_t, 1,
				  out_names, 1, &out_t);
	g_ort->ReleaseValue(in_t);
	if (s) {
		g_ort->ReleaseStatus(s);
		return frame;
	}

	float* out = nullptr;
	g_ort->GetTensorMutableData(out_t, (void**)&out);
	std::vector<float> norm((size_t)S * S);
	float mn = 1e30f, mx = -1e30f;
	for (int i = 0; i < S * S; i++) {
		if (out[i] < mn)
			mn = out[i];
		if (out[i] > mx)
			mx = out[i];
	}
	if (mx - mn > 1e-6f) {
		float inv = 1.0f / (mx - mn);
		for (int i = 0; i < S * S; i++)
			norm[i] = (out[i] - mn) * inv;
	} else {
		for (int i = 0; i < S * S; i++)
			norm[i] = 0.0f;
	}
	g_ort->ReleaseValue(out_t);

	/* 4) 缩放回原尺寸 -> alpha */
	std::vector<uint8_t> alpha((size_t)W * H);
	resize_mask_bilinear(norm.data(), S, S, alpha.data(), W, H);

	/* 5) 羽化 / 去溢色（alpha 平滑）*/
	if (f->feather > 0 || f->decontaminate) {
		std::vector<uint8_t> tmp((size_t)W * H), dst((size_t)W * H);
		int r = f->feather > 0 ? f->feather : 0;
		if (f->decontaminate)
			r = r > 0 ? r : 1;
		box_blur(alpha.data(), tmp.data(), dst.data(), W, H, r);
		alpha = std::move(dst);
	}

	/* 6) 虚化模式：生成模糊后的原图（box blur 近似）*/
	std::vector<uint8_t> blurred;
	if (f->bg_mode == BG_BLUR && f->blur_radius > 0) {
		blurred.resize((size_t)W * H * 4);
		int r = f->blur_radius;
		std::vector<uint8_t> tmpc((size_t)W * H), dstc((size_t)W * H);
		for (int c = 0; c < 3; c++) {
			std::vector<uint8_t> ch((size_t)W * H);
			for (int i = 0; i < W * H; i++)
				ch[i] = src[i * 4 + c];
			box_blur(ch.data(), tmpc.data(), dstc.data(), W, H, r);
			for (int i = 0; i < W * H; i++)
				blurred[i * 4 + c] = dstc[i];
		}
	}

	/* 7) 自定义背景图：缩放到 WxH（带缓存）*/
	std::vector<uint8_t> bg_scaled;
	if (f->bg_mode == BG_IMAGE && f->has_bg_image) {
		if (f->bg_scaled_w != W || f->bg_scaled_h != H) {
			f->bg_scaled.resize((size_t)W * H * 4);
			for (int y = 0; y < H; y++) {
				float fy = (f->bg_h == 1)
						   ? 0.0f
						   : (y + 0.5f) * f->bg_h / H - 0.5f;
				int y0 = (int)floorf(fy);
				float fyf = fy - y0;
				if (y0 < 0) {
					y0 = 0;
					fyf = 0;
				}
				int y1 = (y0 + 1 < f->bg_h) ? y0 + 1 : y0;
				for (int x = 0; x < W; x++) {
					float fx = (f->bg_w == 1)
						   ? 0.0f
						   : (x + 0.5f) * f->bg_w / W - 0.5f;
					int x0 = (int)floorf(fx);
					float fxf = fx - x0;
					if (x0 < 0) {
						x0 = 0;
						fxf = 0;
					}
					int x1 = (x0 + 1 < f->bg_w) ? x0 + 1 : x0;
					for (int c = 0; c < 4; c++) {
						int i00 = (y0 * f->bg_w + x0) * 4 + c;
						int i01 = (y0 * f->bg_w + x1) * 4 + c;
						int i10 = (y1 * f->bg_w + x0) * 4 + c;
						int i11 = (y1 * f->bg_w + x1) * 4 + c;
						float v = (f->bg_rgba[i00] * (1 - fxf) +
							   f->bg_rgba[i01] * fxf) *
								  (1 - fyf) +
							  (f->bg_rgba[i10] * (1 - fxf) +
							   f->bg_rgba[i11] * fxf) *
								  fyf;
						int iv = (int)(v + 0.5f);
						if (iv < 0)
							iv = 0;
						if (iv > 255)
							iv = 255;
						f->bg_scaled[(y * W + x) * 4 + c] =
							(uint8_t)iv;
					}
				}
			}
			f->bg_scaled_w = W;
			f->bg_scaled_h = H;
		}
		bg_scaled = f->bg_scaled;
	}

	/* 8) 合成（按行 stride 寻址，兼容可能的行对齐）
	 * OBS 颜色为 0xAABBGGRR：低字节=R, 次低=G, 次高=B */
	uint8_t cr = (uint8_t)(f->bg_color & 0xFF);
	uint8_t cg = (uint8_t)((f->bg_color >> 8) & 0xFF);
	uint8_t cb = (uint8_t)((f->bg_color >> 16) & 0xFF);

	for (int y = 0; y < H; y++) {
		uint8_t* row = frame->data[0] + (size_t)y * sstride;
		for (int x = 0; x < W; x++) {
			int p = x * 4;
			int i = y * W + x;
			uint8_t a = alpha[i];
			float af = a / 255.0f;
			uint8_t r = row[p], g = row[p + 1], b = row[p + 2];

			if (f->bg_mode == BG_TRANSPARENT) {
				row[p + 3] = a;
		} else if (f->bg_mode == BG_SOLID) {
			row[p] = (uint8_t)(r * af + cr * (1 - af));
			row[p + 1] = (uint8_t)(g * af + cg * (1 - af));
			row[p + 2] = (uint8_t)(b * af + cb * (1 - af));
			row[p + 3] = 255;
			} else if (f->bg_mode == BG_IMAGE &&
				   f->has_bg_image) {
				int bp = i * 4;
				uint8_t ir = bg_scaled[bp];
				uint8_t ig = bg_scaled[bp + 1];
				uint8_t ib = bg_scaled[bp + 2];
				row[p] = (uint8_t)(r * af + ir * (1 - af));
				row[p + 1] = (uint8_t)(g * af + ig * (1 - af));
				row[p + 2] = (uint8_t)(b * af + ib * (1 - af));
				row[p + 3] = 255;
			} else if (f->bg_mode == BG_BLUR &&
				   !blurred.empty()) {
				uint8_t ir = blurred[i * 4];
				uint8_t ig = blurred[i * 4 + 1];
				uint8_t ib = blurred[i * 4 + 2];
				row[p] = (uint8_t)(r * af + ir * (1 - af));
				row[p + 1] = (uint8_t)(g * af + ig * (1 - af));
				row[p + 2] = (uint8_t)(b * af + ib * (1 - af));
				row[p + 3] = 255;
			} else {
				row[p + 3] = a; /* 兜底：透明 */
			}
		}
	}

	return frame;
}

struct obs_source_info ai_matte_filter_info = {
	.id = "obs_ai_matte_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = ai_matte_get_name,
	.create = ai_matte_create,
	.destroy = ai_matte_destroy,
	/* 注意：C++20 指定初始化器必须严格按 obs_source_info 的成员声明顺序书写。
	 * OBS 31 顺序为：id, type, output_flags, get_name, create, destroy,
	 * get_defaults, get_properties, update, ..., filter_video */
	.get_defaults = ai_matte_get_defaults,
	.get_properties = ai_matte_get_properties,
	.update = ai_matte_update,
	.filter_video = ai_matte_filter_video,
};
