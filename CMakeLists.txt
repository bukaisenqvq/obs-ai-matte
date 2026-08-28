cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.cpp src/ai_matte_filter.cpp)

# ---------------------------------------------------------------------------
# ONNX Runtime (DirectML / GPU build)
# CI 会在 configure 之前把 SDK 下载到 third_party/onnxruntime
# ---------------------------------------------------------------------------
if(NOT DEFINED ONNXRUNTIME_DIR)
  set(ONNXRUNTIME_DIR "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
endif()

target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE "${ONNXRUNTIME_DIR}/include"
          "${ONNXRUNTIME_DIR}/include/onnxruntime/core/session"
          "${CMAKE_SOURCE_DIR}/third_party/stb")

target_link_libraries(${CMAKE_PROJECT_NAME}
                      PRIVATE "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")

# 把运行时 DLL 复制到插件输出目录随包分发。
# 不同版本的 onnxruntime 包把 .dll 放在根目录或 lib/ 下，这里两处都扫。
file(GLOB ORT_DLLS "${ONNXRUNTIME_DIR}/*.dll" "${ONNXRUNTIME_DIR}/lib/*.dll")

foreach(ORT_DLL IN LISTS ORT_DLLS)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ORT_DLL}"
            "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
    COMMENT "Copying ${ORT_DLL} next to the plugin")
endforeach()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
