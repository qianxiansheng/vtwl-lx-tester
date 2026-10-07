#include "pch.h"

#include <iostream>
#include <windows.h>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <sstream>
#include "winbase.h"
#include <memory>
#include <chrono>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui/extensions/ImConsole.h"
#include "imgui/extensions/ImFileDialog.h"

#include "utils/utils.h"
#include "utils/thread_pool.h"
#include "ui_main.h"
#include <fstream>

#define OPEN_WORDS_FILE_FILE_DIALOG_KEY "open_words_file_dialog"

#define SERIAL_PORT_MAX_NUM 6

static bool show_root_window = true;
static bool opt_fullscreen = true;
static bool opt_showdemowindow = false;
static bool opt_showmainwindow = true;

static uint32_t port_color[] = {
	0xFFFF8080,
	0xFFFFBF80,
	0xFFFFFF80,
	0xFFBFFF80,
	0xFF80FF80,
	0xFF80FFBF,
	0xFF80FFFF,
	0xFF80BFFF,
	0xFF8080FF,
	0xFFBF80FF,
	0xFFFF80FF,
	0xFFFF80BF,
	0xFFFF4242,
	0xFF42FFFF,
	0xFFCC66CC,
	0xFF66CC66,
	0xFFCC9966,
	0xFF66CC99,
	0xFF6666CC,
	0xFF6699CC,
};

std::shared_ptr<ThreadPool> g_pool;

struct SerialPort {
	bool enable = false;
	char portName[32] = "COM0";
	char baudText[32] = "9600";
	uint8_t distance = 0;
	std::string noiseLevelText = "low";
	bool isOpen = false;
};

struct RecWord {
	char id[32] = "";
	char name[32] = "";
	int flag = 0;
};

enum BgMusicType {
	None = 0,
	Vocal = 1,
	Music = 2,
};

const char8_t* bgMusicType_Text[] = { 
	u8"无音乐", 
	u8"人声", 
	u8"音乐" 
};

const std::string bgMusicType_Tag[] = {
	"N",
	"V",
	"M"
};

struct RoundConfig {
	int bg_music_index = 0;
	BgMusicType bg_music_type = BgMusicType::None;
	const char8_t* bg_music_type_text = bgMusicType_Text[BgMusicType::None];
};

struct ViewModel {
	bool running = false;
	bool paused = false;
	uint32_t curr_index = 0;
	bool scroll_request = false;
	char project_id[64] = { 0 };
	char person_id[64] = { 0 };
	int gender = 0;
	char hometown[128] = { 0 };
	int age = 25;
	std::vector<RecWord> recWords = {};
	std::vector<SerialPort> serialPorts{6};

	std::string record_text_content = "";

	bool running_ex = false;
	uint16_t round_limit = 6;
	uint16_t round_count = 6;
	std::vector<RoundConfig> roundConfigs{6};
	uint32_t current_round = 0;
} vm;

struct Config {
	double factor = 1.0;
	double silence_tail_duration = 1.0;
	double silence_head_duration = 2.0;
	double music_head_duration = 2.0;
} acfg;

ImVec4 clear_color = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);

using TimerFunc = std::function<void()>;
struct Timer
{
	bool running = false;
	double last_time = 0.0f;
	double duration = 1.0f;
	TimerFunc callback;
};

void timer_start(Timer& t, double seconds, std::function<void()> cb)
{
	t.running = true;
	t.last_time = ImGui::GetTime();
	t.duration = seconds;
	t.callback = cb;
}

void timer_update(Timer& t)
{
	if (!t.running) return;

	double now = ImGui::GetTime();
	if (now - t.last_time >= t.duration)
	{
		t.running = false;  // 执行完自动停止
		if (t.callback) t.callback(); // 触发回调
	}
}

bool timer_running(Timer& t)
{
	return t.running;
}

bool main_init(int argc, char* argv[])
{
	// Enable Dock
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

	g_pool = std::make_shared<ThreadPool>(25);

	return true;
}

void main_shutdown(void)
{
}

static void ShowMainWindow(bool* p_opt_showmainwindow)
{
	bool show_alert_please_select_port = false;

	ImGui::Begin("Main Window", p_opt_showmainwindow);

	ImGui::End();

	utils::Alert(show_alert_please_select_port, "Message##m2", "Please select at least one.");
}

static void ShowRootWindowMenu()
{
	if (ImGui::BeginMenuBar()) {

		if (ImGui::BeginMenu("File")) {

			if (ImGui::MenuItem("Open ini")) {
			}

			if (ImGui::MenuItem("Save", "Ctrl + S")) {
			}

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Option")) {

			ImGui::MenuItem("Demo Window", NULL, &opt_showdemowindow);
			ImGui::Separator();
			ImGui::MenuItem("Main Window", NULL, &opt_showmainwindow);

			ImGui::Separator();
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}
}

static void ShowRootWindow(bool* p_open)
{
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;

	if (opt_fullscreen) {
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);
		window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	}
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGui::Begin("-", p_open, window_flags);

	if (opt_fullscreen) {
		ImGui::PopStyleVar();	// WindowBorderSize
		ImGui::PopStyleVar();	// WindowRounding
	}
	ImGui::PopStyleVar();	// WindowPadding

	ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_AutoHideTabBar);

	ShowRootWindowMenu();

	ImGui::End();
	if (opt_showdemowindow) {
		ImGui::ShowDemoWindow(&opt_showdemowindow);
	}

	if (opt_showmainwindow) {
		ShowMainWindow(&opt_showmainwindow);
	}
}

int main_gui()
{
	ShowRootWindow(&show_root_window);

	return 0;
}
