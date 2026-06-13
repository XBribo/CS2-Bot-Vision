# 迁移计划:MinHook → funchook(跨平台 hook 层)  [已完成 2026-06-13]

## 实际改动(已落地)
- gamedata.json: 加 8 个成员偏移条目(offsets.windows/linux, CSS 扁平风格), linux 暂填 0
- sig_scan.h/.cpp: 加 ResolveOffset(gamedata, name, defVal) 按平台读 offsets
- cdetour.h(新建): CDetour<T> 模板封装 funchook, Create 把 trampoline 回填进 g_orig* 指针
- hooks.cpp: include 换 funchook+cdetour; offset 常量改 static int 由 Install 从 gamedata 读; 4 个 CDetour 实例; Install/Remove 换 funchook; 5 个 Hooked* 函数体零改动
- CMakeLists.txt: minhook FetchContent → kubo/funchook(master); link minhook → funchook-static; FUNCHOOK_BUILD_SHARED/INSTALL OFF
- 验证: configure + Release build 通过, BotVision.dll 生成, 静态链接 funchook(distorm 后端)
- kMaxBots=64 保持 const(数组维度, 与游戏版本无关)

---

# 原计划

## 目标与范围
- 把 `hooks.cpp` 的 inline hook 底座从 **MinHook**(Windows-only)换成 **funchook**(Win+Linux x64)
- 反汇编引擎:**distorm**(= CS2Fixes 用的,也是 funchook x64 默认)
- 封装:引入精简版 **CDetour** 类(funchook 包装),适配现有 sig 扫描层
- 范围:**只换 hook 层**。Windows 照常编译运行;plugin.cpp 的 Windows.h、CMake 的 .lib 等 Linux 移植本次不做

## 已核实事实
- funchook C API:`funchook_create()` / `funchook_prepare(fh, (void**)&orig, detour)` / `funchook_install(fh,0)` / `funchook_uninstall(fh,0)` / `funchook_destroy(fh)`
- funchook 是 trampoline 模型,与现有 `g_orig*` 同签名指针用法一一对应 → 5 个 Hooked* 函数体零改写
- CMake 目标名:`funchook-static` / `funchook-shared`(无裸 `funchook`),OUTPUT_NAME 均为 funchook
- 默认 distorm,通过 FetchContent 自动拉取;选项 `FUNCHOOK_DISASM`、`FUNCHOOK_BUILD_SHARED/STATIC`、`FUNCHOOK_INSTALL`
- 用上游 kubo/funchook:HEAD 2025-09-28(已归档但 API 冻结=稳定);satisfactorymodding fork 落后上游 16 commits 且定制方向无关,不用

## 改动清单

### 1. 新增 src/cdetour.h(精简 CDetour 封装)
- 模板类 `CDetour<T>`,T 为被 hook 函数签名
- 成员:`T* m_pfnDetour`(替身)、`T* m_pfnOrig`(原始/trampoline)、`funchook_t* m_hook`、`const char* m_name`、`bool m_installed`
- 方法:
  - `Create(void* target)` → 存 target 到 m_pfnOrig,`funchook_create` + `funchook_prepare`
  - `Enable()` → `funchook_install`
  - `Disable()` → `funchook_uninstall`
  - `Free()` → 已装则先 Disable,再 `funchook_destroy`
  - `operator()(args...)` → `std::invoke(m_pfnOrig, ...)` 调原始
  - `GetOrig()` → 返回 m_pfnOrig(供需要裸指针处)
- 注意:不照搬 CS2Fixes 与 CModule/gameconfig 的耦合;target 地址由现有 cs2bv::sig::ResolveSig 提供
- 全局只用一个 funchook_t 还是每 hook 一个:**每个 CDetour 持有独立 funchook_t**(funchook 支持一个实例装多个 prepare,但独立更清晰、卸载粒度细)

### 2. src/hooks.cpp
- 删 `#include <MinHook.h>`,改 `#include <funchook.h>` + `#include "cdetour.h"`
- 5 个 `g_origXxx` 函数指针 → 改为 5 个 `CDetour<Sig>` 实例(或保留 g_orig 指针,CDetour 内部回填 —— 见下"封装取舍")
- `Install()`:每处 `MH_CreateHook + MH_EnableHook` → `detour.Create(target) + detour.Enable()`;删 `MH_Initialize()`
- `Remove()`:`MH_DisableHook(MH_ALL_HOOKS)+MH_Uninitialize()` → 对每个 detour `Free()`
- 5 个 Hooked* 函数体:**不动**(trampoline 语义一致)
- 调原始的写法:`g_origHeDetonate(self)` → 若用 CDetour 实例则 `g_heDetonateDetour.GetOrig()(self)` 或 `g_heDetonateDetour(self)`

### 封装取舍(实现时定)
两种接线方式,功能等价:
- A. 保留现有 `static HeDetonate_t g_origHeDetonate` 等指针,CDetour::Create 接 `void** origOut` 把 trampoline 回填进去 → Hooked* 函数体真正零改动
- B. 用 CDetour 实例的 operator()/GetOrig 调原始 → 需把 5 处 `g_origXxx(...)` 调用改成实例调用
- **倾向 A**:Hooked* 函数体完全不动,改动最集中在 Install/Remove

### 3. CMakeLists.txt
- 删 minhook 的 FetchContent_Declare/MakeAvailable 块(54-60 行)
- 加 funchook FetchContent:
  ```
  FetchContent_Declare(funchook
      GIT_REPOSITORY https://github.com/satisfactorymodding/funchook.git
      GIT_TAG master GIT_SHALLOW TRUE)
  set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)   # 只要静态库
  set(FUNCHOOK_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(funchook)
  ```
- `target_link_libraries`:`minhook` → `funchook-static`
- `psapi` 保留(sig_scan 仍用)
- 支持 MINHOOK_SOURCE 本地覆盖的逻辑(54-56 行)按需改成 FUNCHOOK_SOURCE 或删除

## 验证
- 用 CLAUDE.md 指定命令:
  - configure: `cmake -B build -G "Visual Studio 18 2026" -A x64`
  - build: `cmake --build build --config Release`
- 确认链接出 BotVision.dll,无 MinHook 残留符号
- 运行期验证需进游戏,非本次自动化范围;至少保证编译链接通过

## 风险
- 中:引入新依赖(funchook + distorm,FetchContent 首次拉取);distorm 是 LGPL,与项目 GPLv3 兼容
- 低:hook 逻辑零改写,trampoline 语义一致,行为不变
- 注意:funchook_prepare 失败码与 MinHook 不同,错误上报字符串要相应调整(funchook_error_message)
