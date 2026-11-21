# 实施计划：插件参数并发与 TapBuffer 修复

## Context

高优先级清单 H-1/H-2/H-3 涉及实时写入与并发读取的数据竞争、插件参数回调锁/无界扩容，以及 VST3 UI 到 process 线程未经同步的 SDK 传递。修复目标是在保留实时路径有界、无锁、无运行期分配的前提下，明确溢出语义，并覆盖并发与生命周期。

## 实施步骤

1. H-1：将 `TapBuffer` 环形样本存储改为预分配原子浮点样本；每槽以原子读写消除慢读者跨发布周期造成的 C++ 数据竞争。写侧保持固定循环和 release 发布序号；读侧按已发布的最近窗口读取，同时检查起止序号，若快照期间被覆盖则返回明确失败（或依项目已有接口选择并文档化一致的失败语义）。新增跨平台、无第三方依赖的 CTest 测试，覆盖空/短/满/多圈、环绕和并发压力。

2. 参数通知：新增可复用的预分配有界 MPSC 参数变化队列，采用固定容量序列槽算法；成功入队/出队均有限步，满队列时拒绝新事件并计数/提供明确状态，不分配、不等待、不加锁。VST2 `audioMasterAutomate` 和 VST3 `performEdit` 使用此队列，UI 消费端仍可构造返回 vector。

3. UI 到实时参数：引擎 `set_plugin_parameter` 仅写入引擎持有的固定容量、每参数原子邮箱（ID 与浮点位模式在单一原子字内一致），并立即更新 GUI 缓存；音频回调在 `chain.process` 前 drain 有界命令并调用 VST 槽 setter。邮箱与引擎同生命周期，避免 UI 与 watchdog 重建期间解引用失效 slot；通过锁住生命周期边界或保持当前 pipeline mutex 规则，确保参数表/编辑器/队列消费不会与插件卸载并发。

4. VST3：移除跨线程共享 `ParameterChangeTransfer`。process 线程从引擎命令邮箱取得参数后，在 process 线程独占地写入本线程的 `ParameterChanges`；它仅交给插件本次 process 调用，不与 UI 共享。验证 setter 的 UI/controller 侧访问遵守 VST3 线程约束；必要时只保留原子命令同步，不在 UI 线程触碰 controller。

5. 更新 CMake 项目自有测试目标与 CTest。对实时路径审查无 mutex、动态扩容和数据相关无界循环；运行构建、CTest，并审查现有 build 生成器和工作区改动，保留用户文件。

6. 所有实现和验证完成后更新 `.trae/documents/高优先级审查修复清单.md` 的状态、溢出契约与证据。不给版本号递增、不提交、不发布、不打包。

## 关键文件

- `modules/engine/include/engine/tap_buffer.h`
- `modules/engine/src/audio_engine.cpp` 与 `modules/engine/include/engine/audio_engine.h`
- `modules/vst2_host/include/vst2_host/vst2_slot.h`、`modules/vst2_host/src/vst2_slot.cpp`
- `modules/vst3_host/src/vst3_slot.cpp`
- 根 `CMakeLists.txt`、`modules/engine/CMakeLists.txt`（测试目标）
- `.trae/documents/高优先级审查修复清单.md`

## 验证

- CMake 重新生成现有 Visual Studio 2022 build 并构建全目标。
- CTest 执行 TapBuffer 与并发有界参数通道测试，包含饱和、多个生产者、读写并发和一致性检查。
- 静态检查实时回调链路中无阻塞锁、无动态分配、队列操作有固定上界；尝试可用的 MSVC 并发/运行时验证工具，并记录工具限制。
- 检查音频关闭/重建、插件卸载与参数消费交错场景；若环境允许，再运行应用验证 VST2/VST3 编辑器参数拖动和音频连续性。
