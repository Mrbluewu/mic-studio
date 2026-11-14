# Mic Studio

Windows 桌面端实时麦克风降噪工具。自绘 Win32 界面，音频链路为
`抓取真实麦克风 → 降噪/效果链 → 重采样 → 输出到指定端点`，
对外表现为一个可被会议/直播软件选择的「干净麦克风」。

- 语言标准：C++20
- 构建：CMake + MSVC（Visual Studio 2022 BuildTools）
- 平台：Windows 10 / 11（WASAPI）
- 当前版本：0.1.13

## 目录

```
apps/          可执行程序（micstudio 主程序、engine-cli 命令行）
modules/       12 个功能模块，统一 include/<模块>/ + src/
assets/        源资源（图标、图片）
packaging/     打包配置（Inno Setup 脚本）
tests/         测试源码与本地测试夹具
tools/         开发脚本（截图、控件排查）
third_party/   第三方依赖
docs/          项目文档
build/         构建产物（不入库）
dist/          安装包产物（不入库）
```

各目录的存放规则见 [docs/DIRECTORY.md](docs/DIRECTORY.md)。

## 前置条件

| 依赖 | 要求 |
|---|---|
| 编译器 | Visual Studio 2022 BuildTools（MSVC，C++20） |
| 构建工具 | CMake 3.20+ |
| 打包（可选） | Inno Setup 6 |
| VST3 SDK | **需自行放置**到 `third_party/vst3sdk/`，见下 |

VST3 SDK 不入库，克隆后必须补齐，否则 `vst3_host` 模块无法编译：

1. 获取 VST3 SDK **3.7.1**（上游 tag `v3.7.1_build_50`）：
   `git clone --recursive --branch v3.7.1_build_50 https://github.com/steinbergmedia/vst3sdk.git`
2. 把仓库内容整体放到 `third_party/vst3sdk/`，确认其中存在 `CMakeLists.txt`、`pluginterfaces/`、`public.sdk/`。
3. 子模块（`base`、`cmake`、`vstgui4` 等）必须一并检出，不能只拷贝顶层目录。

## 构建

```powershell
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build
& $cmake --build build --config Release
```

产物：`build\apps\micstudio\Release\micstudio.exe`

## 测试

```powershell
& build\tests\Release\micstudio_ui_tests.exe
```

基线：305 checks，0 failed。界面布局改动会改变高度基线，需同步更新 `tests/ui_tests.cpp`。

## 打包

```powershell
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" packaging\installer.iss
```

输出：`dist\MicStudio-Setup-<版本>.exe`

## 版本号

版本号有两处，升级时必须同批修改：

- 根 [CMakeLists.txt](CMakeLists.txt) 的 `project(mic_studio VERSION x.y.z)` —— 决定 exe 资源版本
- [packaging/installer.iss](packaging/installer.iss) 的 `#define AppVersion` —— 决定安装包文件名

## 第三方依赖

| 目录 | 内容 | 是否入库 |
|---|---|---|
| `third_party/deepfilternet/` | DeepFilterNet 运行时（DLL），构建与运行必需 | 是 |
| `third_party/vst3sdk/` | VST3 SDK，独立 git 检出 | 否，需手动放置 |
| `tests/fixtures/plugins/VST3/` | DeepFilterNet 的 VST3 bundle，仅 VST3 手工验证用；VST2 验证直接复用 `third_party/deepfilternet/DeepFilterNet.dll` | 否 |

`third_party/vst3sdk` 与已移除的 `rnnoise` 原本都是独立 git 仓库，因此由 `.gitignore` 排除，避免外层仓库记录无法解析的子模块引用。

## 开发脚本

| 脚本 | 用途 |
|---|---|
| `tools/screenshot.ps1` | 截主窗口完整外观 → `build/shot.png` |
| `tools/screenshot-advanced.ps1` | 展开高级设置后截图 → `build/shot_adv.png` |
| `tools/screenshot-client.ps1` | 只截客户区 → `build/shot_client.png` |
| `tools/crop.ps1` | 把 shot.png 局部放大，核对像素布局 |
| `tools/dump-children.ps1` | 打印全部子控件句柄/类名/文本 |

均需先构建 Release，且运行中的 micstudio.exe 会被脚本结束时关闭。

## 许可证

本项目采用 [GPL-3.0](LICENSE) 发布。

第三方组件声明：

| 组件 | 许可证 | 说明 |
|---|---|---|
| DeepFilterNet | 见上游 DualSenseNet / DeepFilterNet 项目许可证 | `third_party/deepfilternet/` 下的预编译 DLL 与运行时文件 |
| VST3 SDK 3.7.1 | Steinberg VST3 License 或 GPL-3.0（双授权） | 不由本仓库分发，使用时自行放置；本项目按 GPL-3.0 一侧使用 |
| VST 插件（用户加载） | 各自上游许可证 | 本项目只做宿主加载，不再分发插件本体 |

