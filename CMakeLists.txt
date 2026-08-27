cmake_minimum_required(VERSION 3.28...3.30)

project(obs-ai-matte
	VERSION 0.1.0
	DESCRIPTION "OBS filter: real-time AI background removal (DirectML)"
	LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- OBS 插件助手（由 CI / 本机构建通过 -DOBS_SOURCE_DIR=... 提供）----
list(APPEND CMAKE_MODULE_PATH "${OBS_SOURCE_DIR}/cmake")
include(ObsPluginHelpers)

# ---- ONNX Runtime：默认使用仓库内 third_party/onnxruntime（CI 会自动下载）----
if(NOT DEFINED ONNXRUNTIME_DIR)
	set(ONNXRUNTIME_DIR "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
endif()

add_obs_plugin(
	obs-ai-matte
	SOURCES
		src/plugin-main.cpp
		src/ai_matte_filter.cpp
		src/ai_matte_filter.hpp)

target_include_directories(obs-ai-matte PRIVATE
	"${ONNXRUNTIME_DIR}/include"
	"${CMAKE_SOURCE_DIR}/third_party/stb")

target_link_libraries(obs-ai-matte PRIVATE "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")

# 把 onnxruntime 运行所需的 DLL 一并复制到插件输出目录（随包分发）
# 注意：onnxruntime 的 .dll 在包根目录（不是 lib/），lib/ 下是 .lib 导入库
add_custom_command(TARGET obs-ai-matte POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${ONNXRUNTIME_DIR}"
		"$<TARGET_FILE_DIR:obs-ai-matte>"
	COMMENT "Copying onnxruntime DLLs next to the plugin")
