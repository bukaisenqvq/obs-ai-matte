# obs-ai-matte —— OBS 实时 AI 抠图滤镜

一个 **OBS Studio 滤镜插件**：挂在你现有的「视频捕获设备」（摄像头）源上，实时把人像/主体 AI 抠出来，换成透明背景、纯色、自定义背景图，或原背景虚化。底层用 [rembg](https://github.com/danielgatis/rembg) 的同款 ONNX 模型（isnet / u2net_human_seg），通过 **ONNX Runtime + DirectML** 直接吃你的独显（NVIDIA / AMD / Intel 独显均可），无独显自动回退 CPU。

> 这是把「本地摄像头 AI 抠图」做成 **OBS 原生滤镜** 的形态：别人装好就是在 OBS 滤镜列表里多一项，不用装 Python、不用开额外程序、不用虚拟摄像头驱动——分发最干净。

## 功能

- AI 主体/人像分割（两种模型可选）
- 背景模式：透明 / 纯色 / 自定义背景图 / 原背景虚化
- 边缘羽化、去溢色（发丝更干净）
- 自动使用独显（DirectML），无独显回退 CPU

## 快速使用（给最终用户 / 朋友）

1. 到本仓库 **Releases** 下载 `obs-ai-matte-*.zip`（CI 自动构建，含 dll + onnxruntime + 模型）。
2. 把 zip 里的内容**解压到 OBS Studio 安装目录**（例如 `C:\Program Files\obs-studio\`），它会自动落到：
   - `obs-plugins/64bit/obs-ai-matte.dll`
   - `obs-plugins/64bit/onnxruntime*.dll`
   - `data/obs-plugins/obs-ai-matte/*.onnx`
3. 打开 OBS → 在「来源」里选中你的摄像头源 → 点「滤镜」→ 添加「AI 抠图滤镜 (DirectML)」→ 调参数即可。

> 整个包是自包含的，**不需要联网、不需要装任何运行库**（onnxruntime 已随包）。

## 参数说明（滤镜属性面板）

| 参数 | 说明 |
|---|---|
| 模型 | `通用高精度(isnet)` 边缘最干净；`人像专用(u2net)` 更快更稳但只锁人体 |
| 背景模式 | 透明(PNG 输出)/纯色/自定义背景图/原背景虚化 |
| 背景颜色 | 纯色模式下的颜色 |
| 背景图片 | 自定义背景图模式的图片路径 |
| 虚化半径 | 原背景虚化模式的模糊强度 |
| 边缘羽化 | alpha 边缘柔化半径(px) |
| 去除溢色 | 半透明边缘进一步去背景色残留（发丝更干净） |

> 性能参考（RTX 2080 Ti）：isnet 约 3~5 fps，u2net 约 10~15 fps。想要更流畅选「人像专用」模型。OBS 场景帧率会因此受限，属正常。

## 自动出包（CI）—— 你/朋友都免编译

本仓库已配置 GitHub Actions（`.github/workflows/build.yml`）：

- 推送到 `master`/`main` 分支 → 自动在 Windows 上构建，产出可下载的插件包（Artifact）。
- 发一个 GitHub Release → 自动把插件包作为 Release 资产上传。
- CI 会自动下载 ONNX Runtime 和两个 AI 模型并一并打包，所以 Release 里的 zip **开箱即用**。

你只需：把这个 `obs-ai-matte` 目录作为仓库推到 GitHub，之后每次发布版本都会自动生成可分发的插件包。把 Release 链接发给朋友即可。

## 本机编译（可选，仅当你想自己改代码）

需要：Windows + Visual Studio 2022 + CMake + OBS Studio 源码（用于 `obs` 头文件/库）。

```powershell
# 1. 下载 ONNX Runtime 并放到 third_party/onnxruntime
$v="1.20.1"
Invoke-WebRequest -Uri "https://github.com/microsoft/onnxruntime/releases/download/v$v/onnxruntime-win-x64-$v.zip" -OutFile ort.zip
Expand-Archive ort.zip -DestinationPath third_party
Move-Item "third_party/onnxruntime-win-x64-$v" "third_party/onnxruntime"

# 2. 把两个模型放到 data/obs-ai-matte/（同上 CI 步骤）

# 3. 配置 + 构建（OBS_SOURCE_DIR 指向你拉取的 obs-studio 目录）
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOBS_SOURCE_DIR="D:/obs-studio"
cmake --build build --config RelWithDebInfo
```

构建产物在 `build/Release/`（或 `build/`），连同 `third_party/onnxruntime/lib/*.dll` 与 `data/obs-ai-matte/*.onnx` 一起复制到 OBS 安装目录即可。

## 实现说明

- 前处理严格复刻 rembg：`LANCZOS` 等价缩放 → 除 255 → 减均值/除标准差 → HWC 转 NCHW → float32。
- 输出取模型第 0 个输出的通道 0，做 min-max 归一化，再缩放回原分辨率得到 alpha 蒙版。
- 推理通过 ONNX Runtime C API + `SessionOptionsAppendExecutionProvider_DML`（仅 Windows），自动回退 CPU。
- 摄像头源通常为 RGBA，滤镜直接处理；非 RGBA 格式（如 NV12）当前会原样跳过（后续可扩展）。

## 目录结构

```
obs-ai-matte/
├── CMakeLists.txt
├── buildspec.json
├── .github/workflows/build.yml   # 自动构建 + 出包
├── src/
│   ├── plugin-main.cpp           # 模块注册
│   ├── ai_matte_filter.hpp
│   └── ai_matte_filter.cpp       # 滤镜核心：推理 + 背景合成
└── data/obs-ai-matte/            # 模型（CI 下载；本机编译需手动放）
```
