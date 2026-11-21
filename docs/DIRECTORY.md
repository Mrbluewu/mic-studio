# 目录结构说明

本文标注每个目录的用途、允许存放的内容和禁止存放的内容。新增文件前先对照本表，
不确定时按「源码 / 资源 / 配置 / 文档 / 产物」五类判断归属。

## 顶层

| 路径 | 用途 | 允许存放 | 禁止存放 |
|---|---|---|---|
| `CMakeLists.txt` | 工程入口 | 全局编译选项、模块注册 | 业务逻辑 |
| `README.md` | 项目说明与常用命令 | 概览、构建/测试/打包速查 | 详细设计文档 |
| `.gitignore` | 版本库忽略规则 | 忽略路径与注释 | — |
| `apps/` | 可执行程序 | 程序入口、UI 代码 | 可复用的算法与基础设施 |
| `modules/` | 功能模块库 | 可被 apps 复用的能力 | 程序入口、WinMain |
| `assets/` | 源资源 | 图标、图片、字体 | 运行时可生成的文件 |
| `packaging/` | 打包配置 | 安装脚本、打包相关配置 | 二进制资源、源码 |
| `tests/` | 测试 | 测试源码、测试夹具 | 生产代码 |
| `tools/` | 开发脚本 | 截图、排查类 PowerShell | 构建必需步骤 |
| `third_party/` | 第三方依赖 | vendored 源码 / 运行时 | 自研代码 |
| `docs/` | 项目文档 | 结构说明、设计文档 | 代码、产物 |
| `build/` | 构建产物 | CMake 输出、中间文件、调试截图 | **任何需要提交的文件** |
| `dist/` | 安装包产物 | 打包生成的 exe | **任何需要提交的文件** |

## apps/

| 路径 | 用途 |
|---|---|
| `apps/micstudio/` | 主程序。`main.cpp` 进程启动与设备/引擎装配，`app_window.cpp` 窗口过程与命令分发，`app_ui.cpp` 面板构建与状态刷新，`app_plugins.cpp` 插件选择/参数/编辑器窗口，`ui_theme.*` 布局与绘制，`ui_controls.*` 自绘控件，`ui_icons.h` 内联图标，`micstudio.rc.in` 资源模板，`resource.h` 资源 ID |
| `apps/engine-cli/` | 命令行工具，用于不启动界面时验证引擎链路 |

程序入口只写「组装 + 消息分发」，音频处理一律下沉到 `modules/`。

## modules/

每个模块固定为 `include/<模块名>/*.h` + `src/*.cpp`，头文件用 `#include <模块名/xxx.h>` 引用，
保证外部只依赖公开头文件，不直接包含 `src/` 下的实现细节。

| 模块 | 职责 |
|---|---|
| `platform/` | 进程级基础设施：COM 初始化、错误类型、日志、实时线程、UTF-8 转换、默认端点 |
| `audio_io/` | WASAPI 抓取与渲染流、设备枚举、设备热插拔监听、端点电平表 |
| `dsp_core/` | DSP 基础件：处理器接口、增益/滤波等基础处理器、格式桥、单声道重采样、采样率转换 |
| `denoise/` | 降噪后端抽象与 DeepFilterNet 实现 |
| `effects/` | 效果卡（EQ、压缩等）的定义与参数 |
| `presets/` | 预设的存取与内置预设库 |
| `config/` | 设置持久化（INI，`%APPDATA%\MicStudio`） |
| `engine/` | 音频引擎：抓取↔渲染的线程模型、SPSC 环、时钟漂移控制、处理链编排、TAP 缓冲 |
| `analysis/` | 信号分析（底噪估计等） |
| `vst2_host/` | VST2 插件加载与槽位封装 |
| `vst3_host/` | VST3 插件加载与槽位封装 |
| `ring_buffer/` | 无锁单生产者单消费者环形缓冲（header-only） |

新增模块步骤：建 `modules/<名字>/{include/<名字>,src}` → 写 `CMakeLists.txt` →
在根 `CMakeLists.txt` 里 `add_subdirectory` 并在需要它的目标上 `target_link_libraries`。

## assets/

| 路径 | 内容 |
|---|---|
| `assets/icons/` | `mic-studio.ico`：exe 图标与安装包图标（被 `apps/micstudio/CMakeLists.txt` 与 `packaging/installer.iss.in` 引用） |
| `assets/images/` | `mic-studio.png`：文档与展示用位图 |

移动或改名这里的文件，必须同批修改上述两处引用，否则构建或打包会中断。

## packaging/

| 路径 | 内容 |
|---|---|
| `packaging/installer.iss.in` | Inno Setup 脚本模板：安装目录、快捷方式、卸载项、LicenseFile；`{#AppVersion}` 由 CMake 注入 |

模板在 CMake 配置阶段生成到 `build/packaging/installer.iss`，故模板内的相对路径以 `build/packaging/` 为基准（用 `..\..\` 回到仓库根）。修改打包配置请改 `.in` 模板，不要改生成物。

## tests/

| 路径 | 内容 |
|---|---|
| `tests/CMakeLists.txt` | 测试目标定义（三个测试目标均注册到 CTest） |
| `tests/ui_tests.cpp` | 布局与控件行为测试，含窗口高度基线 |
| `tests/dsp_core_tests.cpp` | 重采样、格式桥、参数队列等 DSP 基础件测试 |
| `tests/concurrency_tests.cpp` | 环缓冲、TAP 缓冲、参数队列的并发正确性测试 |
| `tests/support/check.h` | 测试断言宏与计数汇总 |
| `tests/fixtures/plugins/VST3/` | DeepFilterNet 的 VST3 bundle，作 VST3 托管的手工验证样本；**不入库**，需要时自行准备 `DeepFilterNet.vst3` 并整目录放到本路径 |

VST2 托管验证不需要单独夹具：`third_party/deepfilternet/DeepFilterNet.dll` 与 VST2 插件二进制完全一致（MD5
`F1B3BFDA6BDB8593721378D0C213A545`），直接把 VST2 host 指向该文件即可。此前的
`tests/fixtures/plugins/VST2/DeepFilterNet.dll` 就是这份文件的重复副本，已删除。

## tools/

| 脚本 | 用途 |
|---|---|
| `screenshot.ps1` | 截主窗口完整外观（含标题栏）→ `build/shot.png` |
| `screenshot-advanced.ps1` | 点开「高级设置」后截图 → `build/shot_adv.png` |
| `screenshot-client.ps1` | 只截客户区 → `build/shot_client.png` |
| `crop.ps1` | 把 `build/shot.png` 局部放大 → `build/z_*.png` |
| `dump-children.ps1` | 打印主窗口全部子控件句柄、类名、文本 |

脚本统一以 `$PSScriptRoot` 的父目录作为项目根，输出一律写 `build/`，不污染源码目录。

## third_party/

| 路径 | 内容 | 入库 |
|---|---|---|
| `third_party/deepfilternet/` | DeepFilterNet.dll 及运行时文件，缺失会导致构建失败 | 是 |
| `third_party/vst3sdk/` | VST3 SDK 完整源码 | 否（独立 git 检出） |

`vst3sdk` 是独立仓库，被 `.gitignore` 排除。新环境克隆后需要自行把 SDK 放到该路径，
否则 `vst3_host` 模块无法编译。

## build/ 与 dist/

两者都不入库，可随时删除重建：

- `build/`：CMake 生成物、编译中间文件、`tools/` 脚本的输出
- `dist/`：`build/packaging/installer.iss` 产出的安装包

## 存放规则速查

1. 新增功能模块：`modules/<名字>/{include/<名字>,src}`，并登记到根 `CMakeLists.txt`
2. 新增二进制资源：只放 `assets/`，同时更新引用它的 CMake / ISS
3. 新增配置：只放 `packaging/`（打包）或 `modules/config/`（运行时设置）
4. 大体积测试夹具：放 `tests/fixtures/`，并加入 `.gitignore`
5. 任何构建产物、截图、日志：只能落在 `build/` 或 `dist/`
