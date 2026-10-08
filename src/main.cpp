#include "pch.h"

#include <memory>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui/extensions/ImFileDialog.h"

#include "ui_main.h"
#include "tester/TesterModel.h"
#include "tester/TesterService.h"
#include "tester/TesterViewModel.h"
#include "tester/TesterView.h"

namespace View = Lx::Tester::View;

static bool show_root_window = true;
static bool opt_showdemowindow = false;
static bool opt_showconsole = true;
static bool opt_showstations = true;
static bool opt_showconfig = true;
static bool opt_showlog = true;
static bool opt_resetlayout = false;

// Model（数据） <- Service（业务线程，写 Model） ; ViewModel（UI 线程，读 Model 快照，发命令给 Service）
static std::shared_ptr<Lx::Tester::TesterModel>     g_model;
static std::shared_ptr<Lx::Tester::TesterService>   g_service;
static std::unique_ptr<Lx::Tester::TesterViewModel> g_vm;

bool main_init(int argc, char* argv[])
{
	// Enable Dock（布局保存在 imgui.ini）
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	// Theme
	ImGui::StyleColorsLight();

	// File dialog adapter
	ifd::FileDialog::Instance().CreateTexture = [](uint8_t* data, int w, int h, char fmt) -> void* {
		return CreateTexture(data, w, h, fmt);
		};
	ifd::FileDialog::Instance().DeleteTexture = [](void* tex) {
		DeleteTexture(tex);
		};

	g_model = std::make_shared<Lx::Tester::TesterModel>();
	g_service = std::make_shared<Lx::Tester::TesterService>(g_model, "config.json");
	g_vm = std::make_unique<Lx::Tester::TesterViewModel>(g_model, g_service);

	g_service->refreshPorts();

	return true;
}

void main_shutdown(void)
{
	g_vm.reset();
	if (g_service) g_service->stop();   // 业务线程退出前会保存一次 config.json
	g_service.reset();
	g_model.reset();
}

// 默认布局：
// +-----------------------------+
// | 控制台                      |
// +-----------------------------+
// | 工位管理                    |
// +----------+------------------+
// | 测试参数 | 日志             |
// +----------+------------------+
static void BuildDefaultDockLayout(ImGuiID dockspace_id, ImVec2 size)
{
	const ImGuiStyle& st = ImGui::GetStyle();
	const float fh = ImGui::GetFrameHeight();
	const float fhs = ImGui::GetFrameHeightWithSpacing();
	const float chrome = fh + st.WindowPadding.y * 2;   // 标签栏 + 内边距

	// 按内容估算高度（与 TesterView 中的尺寸一致），再换算成比例
	const float consoleH = chrome + fh * 1.8f + fh * 2.2f + st.ItemSpacing.y;
	const float cardH = fh * 1.25f + fhs * 8 + st.ItemSpacing.y * 3 + st.WindowPadding.y * 2;
	const float stationsH = chrome + fh * 1.6f + st.ItemSpacing.y + cardH + st.ScrollbarSize + st.ItemSpacing.y * 2;

	ImGui::DockBuilderRemoveNode(dockspace_id);
	ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspace_id, size);

	ImGuiID rest = 0, bottom = 0, logNode = 0;
	ImGuiID consoleNode = ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, consoleH / size.y, nullptr, &rest);
	ImGuiID stationsNode = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Up, stationsH / (size.y - consoleH), nullptr, &bottom);
	ImGuiID configNode = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.36f, nullptr, &logNode);

	ImGui::DockBuilderDockWindow(View::kConsoleWindow, consoleNode);
	ImGui::DockBuilderDockWindow(View::kStationsWindow, stationsNode);
	ImGui::DockBuilderDockWindow(View::kConfigWindow, configNode);
	ImGui::DockBuilderDockWindow(View::kLogWindow, logNode);
	ImGui::DockBuilderFinish(dockspace_id);
}

static void ShowRootWindowMenu()
{
	if (ImGui::BeginMenuBar()) {

		if (ImGui::BeginMenu("视图")) {
			ImGui::MenuItem("控制台", NULL, &opt_showconsole);
			ImGui::MenuItem("工位管理", NULL, &opt_showstations);
			ImGui::MenuItem("测试参数", NULL, &opt_showconfig);
			ImGui::MenuItem("日志", NULL, &opt_showlog);
			ImGui::Separator();
			if (ImGui::MenuItem("重置布局"))
				opt_resetlayout = true;
			ImGui::Separator();
			ImGui::MenuItem("Demo Window", NULL, &opt_showdemowindow);
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("调试")) {
			auto& svc = g_vm->service();
			if (ImGui::MenuItem("模拟业务告警（通知）"))
				svc.debugRaiseAlert(Lx::Tester::AlertKind::Notify);
			if (ImGui::MenuItem("模拟业务告警（确认）"))
				svc.debugRaiseAlert(Lx::Tester::AlertKind::Confirm);
			ImGui::Separator();
			if (ImGui::MenuItem("填充演示数据"))
				svc.debugFillDemoData();
			if (ImGui::MenuItem("切换全局状态"))
				svc.debugCycleGlobalState();
			ImGui::EndMenu();
		}

		ImGui::EndMenuBar();
	}
}

static void ShowRootWindow(bool* p_open)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	// NoBringToFrontOnFocus：点击停靠的面板时，业务弹窗仍保持在最上层
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking
		| ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("-", p_open, window_flags);
	ImGui::PopStyleVar(3);

	// 首次运行（imgui.ini 中没有该 dockspace）或用户要求时构建默认布局
	ImGuiID dockspace_id = ImGui::GetID("TesterDockSpace");
	if (opt_resetlayout || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
		opt_resetlayout = false;
		opt_showconsole = opt_showstations = opt_showconfig = opt_showlog = true;
		BuildDefaultDockLayout(dockspace_id, ImGui::GetContentRegionAvail());
	}
	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

	ShowRootWindowMenu();

	ImGui::End();

	if (opt_showconsole)  View::DrawConsole(*g_vm, &opt_showconsole);
	if (opt_showstations) View::DrawStations(*g_vm, &opt_showstations);
	if (opt_showconfig)   View::DrawConfig(*g_vm, &opt_showconfig);
	if (opt_showlog)      View::DrawLog(*g_vm, &opt_showlog);

	// 弹窗在日志面板之后绘制，以便定位到日志面板上方
	View::DrawAlert(*g_vm);

	if (opt_showdemowindow) {
		ImGui::ShowDemoWindow(&opt_showdemowindow);
	}
}

int main_gui()
{
	// 消费 Model 的 dirty flags，刷新 ViewModel 快照
	g_vm->sync();

	ShowRootWindow(&show_root_window);

	return 0;
}
