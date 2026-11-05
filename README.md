# auto-24point

Windows 端的「火拼 24 点」自动答题工具：**DXGI 桌面截屏 → OpenCV 模板识别 4 张牌 → 查 24 点解表 → 模拟鼠标点出算式 →（可选）自动切题**。

单文件 C++17 实现，无第三方框架依赖（除 OpenCV），无网络、无注入。

> 仅供 Windows 桌面自动化 / 图像识别 / 性能优化的学习交流，请勿用于违反游戏服务条款的用途。

## 特性

- **截屏**：DXGI Desktop Duplication，只把 4 张牌的包围盒 `CopySubresourceRegion` 到 staging 纹理回读，不搬整屏。
- **识别**：二值化 → 紧框 → 等比居中到固定 40×56 画布 → 零均值/L2 归一化 → 与预归一化模板做点积。数学上等价于 `TM_CCOEFF_NORMED`，实测分数偏差 `4.17e-7`。
- **并行**：常驻 4 线程识别池，每张牌一个 worker，条件变量屏障同步；一轮 4 张牌约 `0.17 ms`。
- **求解**：预计算解表 `24point_permutations.json`（22615 种有序牌型），vector 直接寻址 O(1)；另保留暴力求解器作参考实现。
- **点击时序**：`QPC` 高精度等待（实测最大正误差 `0.055 ms`），移动等待 / 按下保持 / 识别延迟三个参数独立可调。
- **校准**：4 套可视化校准方案（Win32 分层窗口拖拽），滚轮调整牌块尺寸。
- **自动切题**：每轮答题结束后按 1→5 循环追加一次切题点击，切题点可全屏拖拽设置。
- **快捷键**：全局钩子，`C` 开始 / `V` 停止 / `Esc` 返回菜单。

## 环境要求

| 项目 | 要求 |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 编译器 | Visual Studio 2022 及以上（`vcvars64.bat`） |
| OpenCV | 4.8.1，默认路径 `C:\opencv\opencv\build` |
| 游戏窗口 | 窗口化或无边框窗口（**全屏独占时桌面复制不可用**） |

## 构建

直接运行：

```bat
compile_cpp.bat
```

或手动执行：

```bat
cl /O2 /GL /std:c++17 /EHsc /utf-8 ^
   /I"C:\opencv\opencv\build\include" ^
   auto_24point.cpp /Fe:auto_24point.exe ^
   /link /LTCG /LIBPATH:"C:\opencv\opencv\build\x64\vc16\lib" ^
   opencv_world481.lib gdi32.lib user32.lib d3d11.lib dxgi.lib winmm.lib
```

编译后把 `opencv_world481.dll` 复制到 exe 同目录（仓库里不包含该 DLL）。

## 使用

1. 启动 `auto_24point.exe`（配置文件与 `templates/` 按**当前工作目录**查找，建议从项目根目录运行）。
2. 首次使用选择菜单 `3` 进入可视化校准：拖动 4 个红框对准牌位、4 个绿框对准运算符按钮，滚轮调整牌块大小，`Enter` 保存。
3. 菜单 `1` 进入监控模式，按 `C` 开始自动答题，`V` 停止，`Esc` 返回菜单。
4. 菜单 `5` 配置自动切题：开关，或进入全屏覆盖层拖动 5 个切题点。
5. 菜单 `4` 调整延迟参数（识别→执行、移动后等待、按下保持）。

## 目录结构

```
auto_24point.cpp            主程序（截屏 / 识别 / 求解 / 点击 / 校准 / 菜单）
24point_config.json         校准方案 ×4 + 延迟参数 + 自动切题点
24point_permutations.json   预计算解表，22615 条有序牌型
templates/                  牌面模板（50 张，标签 A/2-10/J/Q/K）
tests/                      离线验证与基准工具
compile_cpp.bat             构建脚本
```

## 配置说明

`24point_config.json`：

```jsonc
{
  "current_profile": 0,              // 当前方案索引 0-3
  "profiles": [
    {
      "initialized": true,
      "card_centers": [[x,y], ...],  // 4 张牌中心点（屏幕像素）
      "op_buttons": {"+":[x,y], "-":[x,y], "*":[x,y], "/":[x,y]},
      "card_crop": [w, h]            // 单张牌的裁剪尺寸
    }
  ],
  "recognize_delay_ms": 0,           // 识别成功 → 开始点击 的延迟
  "move_delay_ms": 0,                // 鼠标移动 → 按下 的等待
  "mouse_down_ms": 40,               // 按下保持时长（每点一次都生效）
  "auto_advance": {
    "enabled": true,
    "points": [[x,y], ...]           // 5 个切题点，按轮次循环
  }
}
```

## 测试

`tests/` 下的工具都通过 `#define main disabled_24point_main` + `#include "../auto_24point.cpp"` 复用主程序实现，不复制代码：

| 工具 | 用途 |
|---|---|
| `test_verify` | 模板留一法识别率、`preciseSleepMs` 精度、手写 NCC 与 `matchTemplate` 等价性 |
| `bench_ncc` | NCC 匹配方案性能对比 |
| `bench_threads` | 串行 / 线程池 / 自旋 / 每轮建线程 四种并行方案基准 |
| `test_poll` | DXGI 帧间隔分布统计（结果追加写入 `poll_result.txt`） |
| `test_capture` | 截屏链路验证 |
| `test_config` | 配置文件读写与旧格式兼容、切题点可视化窗口交互 |
| `test_pool` | 识别线程池正确性 |

## 性能实测

在本机（数据见 `tests/poll_result.txt` 与各工具输出）：

- 模板匹配 `0.317 µs/模板`，4 张牌一轮 `0.063 ms`
- 与 `matchTemplate(TM_CCOEFF_NORMED)` 最大偏差 `4.17e-7`
- `preciseSleepMs` 在 1/16/32/60 ms 档位最大正误差 `0.055 ms`
- DXGI 帧间隔 p50 `5.96 ms`（≈167 fps），p95 `10.75 ms`
- 解表加载约 `120 ms`

## 已知限制

- 识别完全跟随桌面刷新：画面静止时 DXGI 不产生新帧，此期间不会重复识别。
- DXGI 初始化失败（全屏独占、被 OBS 等软件占用 duplication、远程桌面）时当前版本不会自动降级到 GDI。
- 锁屏 / UAC / 改分辨率后 `DXGI_ERROR_ACCESS_LOST` 不会自动重建 duplication。
- 多显示器与 DPI 缩放场景未适配（绝对坐标注入只覆盖主屏）。

## 免责声明

本项目仅用于学习 Windows 桌面自动化、OpenCV 图像识别与性能优化的技术实践。使用自动化工具可能违反游戏服务条款并导致账号处罚，由此产生的一切后果由使用者自行承担。
