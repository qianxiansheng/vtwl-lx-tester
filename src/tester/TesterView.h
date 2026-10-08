#pragma once
#include "TesterViewModel.h"

// View：纯 ImGui 绘制，只读 ViewModel 快照、调用 ViewModel 命令，不直接访问 Model / Service。
// 每个面板是一个可停靠（dock）的独立窗口，默认布局见 main.cpp 的 BuildDefaultDockLayout。
namespace Lx::Tester::View {

// 窗口名即 dock 标识，修改会导致 imgui.ini 中保存的布局失效
inline constexpr const char* kConsoleWindow  = "控制台###tester_console";
inline constexpr const char* kStationsWindow = "工位管理###tester_stations";
inline constexpr const char* kConfigWindow   = "测试参数###tester_config";
inline constexpr const char* kLogWindow      = "日志###tester_log";

void DrawConsole(TesterViewModel& vm, bool* p_open);   // 标题 + 全局状态 + 全局操作按钮
void DrawStations(TesterViewModel& vm, bool* p_open);  // 端口 / 工位管理
void DrawConfig(TesterViewModel& vm, bool* p_open);    // 测试参数配置
void DrawLog(TesterViewModel& vm, bool* p_open);       // 日志（可选择、复制）

// 绘制队首的业务弹窗（非模态浮动窗口，停在日志面板上方，保证紧急停止始终可点）
void DrawAlert(TesterViewModel& vm);

} // namespace Lx::Tester::View
