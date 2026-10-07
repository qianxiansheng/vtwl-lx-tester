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
#include "Model.h"
#include <fstream>

#define OPEN_WORDS_FILE_FILE_DIALOG_KEY "open_words_file_dialog"

#define SERIAL_PORT_MAX_NUM 6

static bool show_root_window = true;
static bool opt_fullscreen = true;
static bool opt_showdemowindow = false;
static bool opt_showlogwindow = true;
static bool opt_showspeakerinfowindow = true;
static bool opt_showportwindow = true;
static bool opt_showwordslistwindow = true;
static bool opt_showmainwindow = true;
static bool opt_showmainexwindow = true;
static bool opt_showroundconfigwindow = true;
static bool opt_showactiveconfigwindow = true;

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
std::unique_ptr<Lx::Rec::Model::BusinessThread> g_businessThread;

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

	// create business thread with model and UI update callback
	{
		using namespace Lx::Rec::Model;
		auto model = std::make_shared<DataModel>();
		std::shared_ptr<DataModel> raw = model;

		// UI update that receives dirty flags and updates only necessary parts
		auto uiUpdateWithFlags = [raw](uint32_t flags) {
			using DF = DataModel::DirtyFlags;
			// Speaker
			if (flags & DF::Dirty_Speaker) {
				auto s = raw->getSpeakerInfo();
				strncpy_s(vm.person_id, sizeof(vm.person_id), std::to_string(s.id).c_str(), _TRUNCATE);
				strncpy_s(vm.hometown, sizeof(vm.hometown), s.hometown.c_str(), _TRUNCATE);
				vm.age = static_cast<int>(s.age);
				strncpy_s(vm.project_id, sizeof(vm.project_id), s.projectID.c_str(), _TRUNCATE);
				vm.gender = s.gender ? 1 : 0;
			}
			// Ports
			if (flags & DF::Dirty_Ports) {
				auto ports = raw->getPortList();
				for (size_t i = 0; i < SERIAL_PORT_MAX_NUM; ++i) {
					if (i < ports.size()) {
						const auto& p = ports[i];
						strncpy_s(vm.serialPorts[i].portName, sizeof(vm.serialPorts[i].portName), p.portName.c_str(), _TRUNCATE);
						snprintf(vm.serialPorts[i].baudText, sizeof(vm.serialPorts[i].baudText), "%u", p.baud);
						vm.serialPorts[i].distance = p.distance;
						vm.serialPorts[i].noiseLevelText = p.noiseLevel;
						vm.serialPorts[i].isOpen = p.isOpen;
						vm.serialPorts[i].enable = p.enable;
					}
				}
			}
			// Words
			if (flags & DF::Dirty_Words) {
				auto words = raw->getWordsList();
				vm.recWords.clear();
				vm.curr_index = 0;
				UiDpiContext::instance().setWordIndex(vm.curr_index);
				for (const auto& w : words) {
					RecWord rw = {};
					snprintf(rw.id, sizeof(rw.id), "%u", w.id);
					strncpy_s(rw.name, sizeof(rw.name), w.text.c_str(), _TRUNCATE);
					rw.flag = static_cast<int>(w.flag);
					vm.recWords.push_back(rw);
				}
				// Rebuild big font atlas for large display on UI thread when model words are loaded
				std::vector<WordsInfo> words_copy = words;

				std::vector<std::string> text_list;
				std::transform(words_copy.begin(), words_copy.end(), std::back_inserter(text_list), [](const WordsInfo& w) {
					return w.text;
				});

				PostToUI([text_list]() {
					UiDpiContext::instance().setWordList(text_list);
				});
			}
		};

		g_businessThread = std::make_unique<BusinessThread>(model, 2, uiUpdateWithFlags);
		// initial full refresh
		PostToUI([uiUpdateWithFlags]() { uiUpdateWithFlags(DataModel::Dirty_All); });
	}

	return true;
}

void main_shutdown(void)
{
	if (g_businessThread) {
		g_businessThread->stop();
		g_businessThread.reset();
	}
}

static void ShowSpeakerInfoWindow(bool* p_opt_showspeakerinfowindow)
{
	ImGui::Begin("Speaker Info", p_opt_showspeakerinfowindow);
	// Add speaker info content here
	ImGui::BeginDisabled(vm.running || vm.running_ex);
	bool changed = false;
	if (ImGui::InputText((char*)u8"项目名称##Project_ID", vm.project_id, sizeof(vm.project_id))) changed = true;
	ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);
	if (ImGui::InputText((char*)u8"录音人ID##Person_ID", vm.person_id, sizeof(vm.person_id))) changed = true;
	ImGui::SameLine();
	if (ImGui::RadioButton((char*)u8"男##GenderM", &vm.gender, 0)) changed = true;
	ImGui::SameLine();
	if (ImGui::RadioButton((char*)u8"女##GenderF", &vm.gender, 1)) changed = true;
	if (ImGui::InputText((char*)u8"籍贯##Hometown", vm.hometown, sizeof(vm.hometown))) changed = true;
	if (ImGui::InputInt((char*)u8"年龄##Age", &vm.age)) changed = true;
	ImGui::EndDisabled();

	// if changed in UI, post to business thread to sync to model
	if (changed && g_businessThread) {
		using namespace Lx::Rec::Model;
		// copy values
		std::string idStr(vm.person_id);
		uint32_t id = 0;
		try { id = static_cast<uint32_t>(std::stoul(idStr)); } catch(...) { id = 0; }
		SpeakerInfo s;
		s.id = id;
		s.age = static_cast<uint32_t>(vm.age);
		s.hometown = std::string(vm.hometown);
		s.gender = vm.gender == 1;
		s.projectID = vm.project_id;
		g_businessThread->postFromUI([s](IDataModel& model){ model.setSpeakerInfo(s); });
	}
	ImGui::End();
}
static void ShowPortWindow(bool* p_opt_showportwindow)
{

	bool show_alert_port_open_failed = false;
	bool show_alert_please_select_firmware = false;

	ImGui::Begin("Port", p_opt_showportwindow);
	ImGuiWindow* window = ImGui::GetCurrentWindow();

	const float cw = 5;

	ImGui::BeginDisabled(vm.running || vm.running_ex);
	static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
	if (ImGui::BeginTable("port_config_table", 7, flags))
	{
		ImGui::TableSetupColumn("Check");
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Baud");
		ImGui::TableSetupColumn("Distance");
		ImGui::TableSetupColumn("NoiseLevel");
		ImGui::TableSetupColumn("Action");
		ImGui::TableSetupColumn("State");

		ImGui::TableHeadersRow();

		for (uint32_t i = 0; i < SERIAL_PORT_MAX_NUM; ++i) {

			SerialPort& port = vm.serialPorts[i];

			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			float item_top_y = window->DC.CursorPos.y;
			float item_top_x = window->DC.CursorPos.x;
			ImGuiDCXAxisAlign(cw + 10);
			if (!port.isOpen) ImGui::BeginDisabled();
			if (ImGui::Checkbox(IMIDTEXT("##Enable", i), &port.enable)) {
				// notify model
				if (g_businessThread) {
					Lx::Rec::Model::PortInfo p;
					p.portName = std::string(port.portName);
					try { p.baud = static_cast<uint32_t>(std::stoul(port.baudText)); }
					catch (...) { p.baud = 9600; }
					p.distance = port.distance;
					p.noiseLevel = port.noiseLevelText;
					p.enable = port.enable;
					p.isOpen = port.isOpen;
					g_businessThread->postFromUI([i, p](Lx::Rec::Model::IDataModel& model) { model.setPortInfo(i, p); });
				}
			}
			if (!port.isOpen) ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(1);
			if (port.isOpen) ImGui::BeginDisabled();
			if (ImGui::InputTextEx(IMIDTEXT("##SerialPort", i), "", port.portName, sizeof(port.portName), ImVec2(200.0f, 0), ImGuiInputTextFlags_None)) {
				// notify model
				if (g_businessThread) {
					Lx::Rec::Model::PortInfo p;
					p.portName = std::string(port.portName);
					p.baud = static_cast<uint32_t>(std::stoul(port.baudText));
					p.distance = port.distance;
					p.noiseLevel = port.noiseLevelText;
					p.enable = port.enable;
					p.isOpen = port.isOpen;
					g_businessThread->postFromUI([i, p](Lx::Rec::Model::IDataModel& model){ model.setPortInfo(i, p); });
				}
			}
			if (port.isOpen) ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(2);
			if (port.isOpen) ImGui::BeginDisabled();
			if (ImGui::InputTextEx(IMIDTEXT("##BaudRate", i), "", port.baudText, sizeof(port.baudText), ImVec2(200.0f, 0), ImGuiInputTextFlags_CharsDecimal)) {
				if (g_businessThread) {
					Lx::Rec::Model::PortInfo p;
					p.portName = std::string(port.portName);
					try { p.baud = static_cast<uint32_t>(std::stoul(port.baudText)); } catch(...) { p.baud = 9600; }
					p.distance = port.distance;
					p.noiseLevel = port.noiseLevelText;
					p.enable = port.enable;
					p.isOpen = port.isOpen;
					g_businessThread->postFromUI([i, p](Lx::Rec::Model::IDataModel& model){ model.setPortInfo(i, p); });
				}
			}
			if (port.isOpen) ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(3);
			static bool inputs_step = true;
			static uint8_t u8_one = 1;
			if (port.isOpen) ImGui::BeginDisabled();
			if (ImGui::InputScalar(IMIDTEXT("##DISTANCE", i), ImGuiDataType_U8, &port.distance, inputs_step ? &u8_one : NULL, NULL, "%u")) {
				if (g_businessThread) {
					Lx::Rec::Model::PortInfo p;
					p.portName = std::string(port.portName);
					try { p.baud = static_cast<uint32_t>(std::stoul(port.baudText)); } catch(...) { p.baud = 9600; }
					p.distance = port.distance;
					p.noiseLevel = port.noiseLevelText;
					p.enable = port.enable;
					p.isOpen = port.isOpen;
					g_businessThread->postFromUI([i, p](Lx::Rec::Model::IDataModel& model){ model.setPortInfo(i, p); });
				}
			}
			if (port.isOpen) ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(4);

			const char* items[] = { "low", "mid", "high" };
			int item_current_idx = 0; // Here we store our selection data as an index.

			for (int n = 0; n < IM_ARRAYSIZE(items); n++)
				if (port.noiseLevelText == items[n]) {
					item_current_idx = n;
					break;
				}

			const char* combo_preview_value = items[item_current_idx];  // Pass in the preview value visible before opening the combo (it could be anything)

			if (port.isOpen) ImGui::BeginDisabled();
			if (ImGui::BeginCombo(IMIDTEXT("##NoiseLevel", i), combo_preview_value, 0))
			{
				for (int n = 0; n < IM_ARRAYSIZE(items); n++)
				{
					const bool is_selected = (item_current_idx == n);
					if (ImGui::Selectable(items[n], is_selected)) {
						item_current_idx = n;
						port.noiseLevelText = items[n];
					}

					// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
					if (is_selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			if (port.isOpen) ImGui::EndDisabled();

			ImGui::TableSetColumnIndex(5);
			ImGui::PushStyleColor(ImGuiCol_Button, port_color[i]);
			if (port.isOpen) {
				if (ImGui::Button(IMIDTEXT("Close##Close", i)))
				{
					if (g_businessThread) g_businessThread->closeSerial(i);
					port.isOpen = false;
				}

				ImGui::TableSetColumnIndex(6);
				ImGui::Text("Serial port Ready");
			}
			else
			{
				if (ImGui::Button(IMIDTEXT("Open##Open", i)))
				{
					// post to business thread to open serial
					if (g_businessThread) {
						Lx::Rec::Model::PortInfo p;
						p.portName = std::string(port.portName);
						try { p.baud = static_cast<uint32_t>(std::stoul(port.baudText)); } catch(...) { p.baud = 9600; }
						p.distance = port.distance;
						p.noiseLevel = port.noiseLevelText;
						p.enable = port.enable;
						p.isOpen = port.isOpen;
						g_businessThread->openSerial(i, p);
					}
				}

				ImGui::TableSetColumnIndex(6);
				ImGui::Text("");
			}
			ImGui::PopStyleColor();

			float item_bottom_y = window->DC.CursorPos.y;

			window->DrawList->AddRectFilled(ImVec2(item_top_x, item_top_y), ImVec2(item_top_x + cw, item_bottom_y), port_color[i]);
		}
		ImGui::EndTable();
	}
	ImGui::EndDisabled();

	utils::Alert(show_alert_port_open_failed, "Message##m1", "The current serial port number cannot be opened, please check and reopen.");


	ImGui::End();
}
static void ShowwordslistWindow(bool* p_opt_showwordslistwindow)
{
	ImGui::Begin("Words List", p_opt_showwordslistwindow);

	ImGui::BeginDisabled(vm.running || vm.running_ex);
	if (ImGui::Button("Open Words File"))
		ifd::FileDialog::Instance().Open(OPEN_WORDS_FILE_FILE_DIALOG_KEY, "选择录音文件", ".txt", false);
	ImGui::EndDisabled();

	ImVec2 avail = ImGui::GetContentRegionAvail();
#if 0
	ImGui::BeginChild("words_list", ImVec2(avail.x, avail.y - 200), true);
	for (size_t i = 0; i < vm.recWords.size(); ++i) {
		ImGui::Text(vm.recWords[i].name);
	}
#endif


	static ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_BordersOuterV;
	if (ImGui::BeginTable("port_config_table", 3, flags, ImVec2(avail.x, avail.y - 20)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("#ID");
		ImGui::TableSetupColumn("TXT");
		ImGui::TableSetupColumn("Flag");

		ImGui::TableHeadersRow();

		if (vm.scroll_request) {
			vm.scroll_request = false;
			if (vm.curr_index >= 0 && vm.curr_index < vm.recWords.size())
			{
				float lineHeight = ImGui::GetTextLineHeightWithSpacing();


				// 显示在第 3 行 = 向上偏移 2 行
				float targetScrollY = (static_cast<int>(vm.curr_index) - 2) * lineHeight;

				// 防止滚动到负数（顶部越界）
				if (targetScrollY < 0.0f)
					targetScrollY = 0.0f;

				ImGui::SetScrollY(targetScrollY);
			}
		}

		for (uint32_t i = 0; i < vm.recWords.size(); ++i) 
		{
			ImGui::TableNextRow();

			if (i == vm.curr_index)
			{
				ImU32 color = ImGui::GetColorU32(ImGuiCol_HeaderActive);
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, color);
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, color);
			}

			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", vm.recWords[i].id);

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%s", vm.recWords[i].name);

			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%d", vm.recWords[i].flag);
		}
		ImGui::EndTable();
	}

	ImGui::End();

	if (ifd::FileDialog::Instance().IsDone(OPEN_WORDS_FILE_FILE_DIALOG_KEY)) {
		if (ifd::FileDialog::Instance().HasResult()) {
			const std::vector<std::filesystem::path>& res = ifd::FileDialog::Instance().GetResults();
			if (res.size() > 0) {
				std::filesystem::path const& path = res[0];
				std::string pathStr = utils::path_to_utf8_string(path.string());
				// Read file and parse CSV-like content: ID,text,wordID
				std::ifstream ifs(path.string(), std::ios::in);
				if (ifs) {
					std::vector<Lx::Rec::Model::WordsInfo> wordsList;
					std::string line;
					vm.record_text_content = "";
					while (std::getline(ifs, line)) {
						vm.record_text_content += line;
						// trim front/back
						auto l = line;
						while (!l.empty() && isspace((unsigned char)l.front())) l.erase(l.begin());
						while (!l.empty() && isspace((unsigned char)l.back())) l.pop_back();
						if (l.empty()) continue;
						if (l.size() > 0 && (l[0] == '#' || (l.size()>=2 && l[0]=='/' && l[1]=='/'))) continue;
						// split by comma
						std::vector<std::string> cols;
						std::string cur;
						std::istringstream ss(l);
						while (std::getline(ss, cur, ',')) {
							// trim
							auto a = 0u, b = (unsigned)cur.size();
							while (a < b && isspace((unsigned char)cur[a])) ++a;
							while (b > a && isspace((unsigned char)cur[b-1])) --b;
							cols.push_back(cur.substr(a, b-a));
						}
						if (cols.empty()) continue;
						// skip header if first column not numeric
						bool firstIsNum = false;
						try { std::stoul(cols[0]); firstIsNum = true; } catch(...) { firstIsNum = false; }
						if (!firstIsNum) continue;
						Lx::Rec::Model::WordsInfo wi;
						try { wi.id = static_cast<uint32_t>(std::stoul(cols[0])); } catch(...) { wi.id = 0; }
						if (cols.size() > 1) wi.text = cols[1];
						if (cols.size() > 2) {
							try { wi.flag = static_cast<uint32_t>(std::stoul(cols[2])); } catch(...) { wi.flag = 0; }
						}
						wordsList.push_back(std::move(wi));
					}
					ifs.close();
					// Rebuild big font atlas for large display on UI thread when a words file is selected
					{
						std::vector<Lx::Rec::Model::WordsInfo> words_copy = wordsList;
						std::vector<std::string> text_list;
						std::transform(words_copy.begin(), words_copy.end(), std::back_inserter(text_list), [](const Lx::Rec::Model::WordsInfo& w) {
							return w.text;
							});

						PostToUI([text_list]() {
							UiDpiContext::instance().setWordList(text_list);
							});
					}
					// Update model via business thread
					if (g_businessThread) {
						g_businessThread->postFromUI([words = wordsList](Lx::Rec::Model::IDataModel& model){
							model.updateWordsList(words);
						});
					}
					// Update view model for immediate display
					vm.recWords.clear();
					for (const auto& w : wordsList) {
						RecWord rw = {};
						snprintf(rw.id, sizeof(rw.id), "%u", w.id);
						strncpy_s(rw.name, sizeof(rw.name), w.text.c_str(), _TRUNCATE);
						rw.flag = static_cast<int>(w.flag);
						vm.recWords.push_back(rw);
					}
				}
			}
		}
		ifd::FileDialog::Instance().Close();
	}
}

static void ShowRoundConifgWindow(bool* p_opt_showroundconfigwindow)
{
	ImGui::Begin("Round Config Window", p_opt_showroundconfigwindow);

	ImGui::BeginDisabled(vm.running || vm.running_ex);

	static bool inputs_step = true;
	static uint16_t u16_one = 1;
	if (ImGui::InputScalar("##ROUND_LIMIT", ImGuiDataType_U16, &vm.round_limit, inputs_step ? &u16_one : NULL, NULL, "%u")) {
		if (vm.round_limit > vm.recWords.size()) {
			vm.round_limit = static_cast<uint16_t>(vm.recWords.size());
		}
		if (vm.round_limit < 1) {
			vm.round_limit = 1;
		}

		if (vm.recWords.size() >= vm.round_limit) {
			if (vm.recWords.size() % vm.round_limit == 0) {
				vm.round_count = static_cast<uint16_t>(vm.recWords.size() / vm.round_limit);
			} else {
				vm.round_count = static_cast<uint16_t>(vm.recWords.size() / vm.round_limit) + 1;
			}
			if (vm.round_count > vm.roundConfigs.size()) {
				vm.roundConfigs.resize(vm.round_count);
			}
		}
	}

	static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
	if (ImGui::BeginTable("round_config_table", 3, flags)) {
		ImGui::TableSetupColumn("");
		ImGui::TableSetupColumn("BG Music Index");
		ImGui::TableSetupColumn("BG Music Type");

		ImGui::TableHeadersRow();

		for (uint32_t i = 0; i < vm.round_count; ++i) {
			RoundConfig& rc = vm.roundConfigs[i];

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%d", (i));

			ImGui::TableSetColumnIndex(1);
			if (ImGui::InputScalar(IMIDTEXT("##MUSIC_INDEX", i), ImGuiDataType_U16, &rc.bg_music_index, inputs_step ? &u16_one : NULL, NULL, "%u")) {

			}

			ImGui::TableSetColumnIndex(2);

			for (int n = 0; n < IM_ARRAYSIZE(bgMusicType_Text); n++)
				if (rc.bg_music_type_text == bgMusicType_Text[n]) {
					rc.bg_music_type = static_cast<BgMusicType>(n);
					break;
				}

			const char8_t* combo_preview_value = bgMusicType_Text[rc.bg_music_type];  // Pass in the preview value visible before opening the combo (it could be anything)

			if (ImGui::BeginCombo(IMIDTEXT("##MUSIC_TYPE_", i), (char*)combo_preview_value, 0))
			{
				for (int n = 0; n < IM_ARRAYSIZE(bgMusicType_Text); n++)
				{
					const bool is_selected = (rc.bg_music_type == n);
					if (ImGui::Selectable((char*)bgMusicType_Text[n], is_selected)) {
						rc.bg_music_type = static_cast<BgMusicType>(n);
						rc.bg_music_type_text = bgMusicType_Text[n];
					}

					// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
					if (is_selected) {
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
		}

		ImGui::EndTable();
	}

	ImGui::EndDisabled();

	ImGui::End();
}

static void ShowActiveConfigWindow(bool* opt_showactiveconfigwindow)
{
	ImGui::Begin("Active Config Window", opt_showactiveconfigwindow);
	

	static double f64_v = 90000.01234567890123456789;

	const double f64_one = 1.;
	ImGui::InputScalar((char*)u8"头部静音区时长##silence_head", ImGuiDataType_Double, &acfg.silence_head_duration, &f64_one);
	ImGui::InputScalar((char*)u8"音乐播放等待时长##music_head", ImGuiDataType_Double, &acfg.music_head_duration, &f64_one);
	ImGui::InputScalar((char*)u8"尾部静音区时长##silence_tail", ImGuiDataType_Double, &acfg.silence_tail_duration, &f64_one);

	ImGui::End();
}

static void ShowMainExWindow(bool* p_opt_showmainwindow)
{
	bool show_alert_please_select_port = false;
	bool show_alert_please_open_wordlist = false;

	ImGui::Begin("Main EX Window", p_opt_showmainwindow);

	ImGui::BeginDisabled(vm.running);

	static Timer timer_tail_silence;
	static Timer timer_head_silence;
	static Timer timer_head_music;
	static double spinner_silence_anim_start_time = 0.0f;
	static double spinner_silence_anim_duration = 1.0f;
	static double spinner_music_anim_start_time = 0.0f;
	static double spinner_music_anim_duration = 1.0f;

	static double waiting_end_time = 0.0f;

	double now = ImGui::GetTime();
	bool silence_counting_down = timer_running(timer_tail_silence)
		|| timer_running(timer_head_silence);
	bool music_counting_down = timer_running(timer_head_music);
	bool counting_down = silence_counting_down || music_counting_down;

	timer_update(timer_tail_silence);
	timer_update(timer_head_silence);
	timer_update(timer_head_music);

	// helper: enter current word -> start backend recording
	auto enterCurrentWord = [&]() {
		if (g_businessThread) g_businessThread->startRecordingAll(vm.curr_index, bgMusicType_Tag[vm.roundConfigs[vm.current_round].bg_music_type]);
	};

	// helper: leave current word -> stop backend recording
	auto leaveCurrentWord = [&]() {
		if (g_businessThread) g_businessThread->stopRecordingAll();
	};

	auto enterCurrentMusic = [&]() {
		if (g_businessThread) g_businessThread->startMusicAll(vm.roundConfigs[vm.current_round].bg_music_index);
	};
	auto leaveCurrentMusic = [&]() {
		if (g_businessThread) g_businessThread->stopMusicAll();
	};

	auto switch_word = [&]() {
		std::cout << "==== T ====" << std::endl;
		leaveCurrentMusic();
		waiting_end_time = now + (acfg.silence_tail_duration + acfg.silence_head_duration + acfg.music_head_duration);
		spinner_silence_anim_start_time = now;
		spinner_silence_anim_duration = (acfg.silence_tail_duration + acfg.silence_head_duration);
		timer_start(timer_tail_silence, (acfg.silence_tail_duration), [&]() {
			std::cout << "==== E ====" << std::endl;
			leaveCurrentWord();
			std::cout << "==== S ====" << std::endl;
			enterCurrentWord();
			timer_start(timer_head_silence, (acfg.silence_head_duration), [&]() {
				std::cout << "==== P ====" << std::endl;
				enterCurrentMusic();
				spinner_music_anim_start_time = ImGui::GetTime();
				spinner_music_anim_duration = (acfg.music_head_duration);
				timer_start(timer_head_music, (acfg.music_head_duration), []() {
					std::cout << "please speak..." << std::endl;
				});
			});
		});
	};

	auto start_word = [&]() {
		std::cout << "==== S ====" << std::endl;
		enterCurrentWord();
		waiting_end_time = ImGui::GetTime() + (acfg.silence_head_duration + acfg.music_head_duration);
		spinner_silence_anim_start_time = ImGui::GetTime();
		spinner_silence_anim_duration = (acfg.silence_head_duration);
		timer_start(timer_head_silence, (acfg.silence_head_duration), [&]() {
			std::cout << "==== P ====" << std::endl;
			enterCurrentMusic();
			spinner_music_anim_start_time = ImGui::GetTime();
			spinner_music_anim_duration = (acfg.music_head_duration);
			timer_start(timer_head_music, (acfg.music_head_duration), []() {
				std::cout << "please speak..." << std::endl;
			});
		});
	};

	auto stop_word = [&]() {
		std::cout << "==== T ====" << std::endl;
		leaveCurrentMusic();
		waiting_end_time = ImGui::GetTime() + (acfg.silence_tail_duration);
		spinner_silence_anim_start_time = ImGui::GetTime();
		spinner_silence_anim_duration = (acfg.silence_tail_duration);
		timer_start(timer_tail_silence, (acfg.silence_tail_duration), [&]() {
			std::cout << "==== E ====" << std::endl;
			leaveCurrentWord();
		});
	};

	// display current word under spinner in large font
	if (!vm.recWords.empty()) {
		const char* wordName = vm.recWords[vm.curr_index].name;
		// Use window font scale to make text larger (doubled)
		ImGui::PushFont(UiDpiContext::instance().getBigFont());
		ImGui::Text(wordName);
		ImGui::PopFont();
	}


	auto update_curr_index = [](uint32_t new_index, bool force = false) {
		if (!force && vm.curr_index == new_index) {
			return;
		}
		vm.curr_index = new_index;
		vm.current_round = new_index / vm.round_limit;
		UiDpiContext::instance().setWordIndex(vm.curr_index);
	};

	ImGui::BeginDisabled(!vm.running_ex || vm.paused || counting_down || vm.curr_index == 0);
	if (ImGui::Button("上一个##prev")) {
		if (vm.curr_index > 0) {
			update_curr_index(vm.curr_index - 1);
			vm.scroll_request = true;
			switch_word();
		}
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(vm.paused || counting_down);
	if (!vm.running_ex) {
		if (ImGui::Button("开始##startup")) {
			int count = static_cast<int>(std::count_if(vm.serialPorts.begin(), vm.serialPorts.end(), [](const SerialPort& port) {
				return port.enable;
			}));
			if (count == 0) {
				show_alert_please_select_port = true;
			} else  if (vm.recWords.size() == 0) {
				show_alert_please_open_wordlist = true;
			} else {
				vm.paused = false;
				vm.running_ex = true;
				update_curr_index(0, true);
				start_word();

				PostToUI([]() {
					try
					{
						std::filesystem::path root = std::filesystem::absolute(".");
						std::filesystem::path project_dir = root / vm.project_id;
						std::filesystem::path record_text_file = project_dir / "record_text.txt";
						std::filesystem::path recorder_file = project_dir / "recorder.txt";

						if (!std::filesystem::exists(project_dir)) {
							std::filesystem::create_directories(project_dir);
						}

						std::ofstream record_text_ofs(record_text_file, std::ios::out);
						if (!record_text_ofs.is_open()) {
							return;
						}
						record_text_ofs << "#ID,text,wordID" << "\n";
						for (int i = 0; i < vm.recWords.size(); ++i) {
							auto& word = vm.recWords[i];
							record_text_ofs << word.id << ","
								<< word.name << ","
								<< word.flag << "\n";
						}

						auto getGender = [](int g) -> const char*
						{
							// 安全强转 char8_t* -> const char*，统一返回类型
							return g == 0
								? reinterpret_cast<const char*>(u8"男")
								: reinterpret_cast<const char*>(u8"女");
						};
						if (!std::filesystem::exists(recorder_file)) {
							std::ofstream recorder_ofs(recorder_file, std::ios::out);
							if (!recorder_ofs.is_open()) {
								return;
							}
							recorder_ofs << "#ID,name,sex,age,location" << "\n";
							recorder_ofs << vm.person_id << ","
								<< vm.person_id << ","
								<< getGender(vm.gender) << ","
								<< vm.age << ","
								<< vm.hometown << "\n";
						}
						else {
							std::ofstream recorder_ofs(recorder_file, std::ios::out | std::ios::app);
							if (!recorder_ofs.is_open()) {
								return;
							}
							recorder_ofs << vm.person_id << ","
								<< vm.person_id << ","
								<< getGender(vm.gender) << ","
								<< vm.age << ","
								<< vm.hometown << "\n";
						}
					}
					catch (const std::filesystem::filesystem_error& e)
					{
					}
					catch (const std::exception& e)
					{
					}
					catch (...)
					{
					}
				});
			}
		}
	}
	else if (!vm.recWords.empty() && (vm.curr_index + 1) >= static_cast<int>(vm.recWords.size())) {
		if (ImGui::Button("结束##exit")) {
			// finish: stop current recordings and mark not running
			stop_word();
			vm.running_ex = false;
		}
	}
	else {
		if (ImGui::Button("下一个##next")) {
			// leave current, advance, enter next (unless last)
			update_curr_index(vm.curr_index + 1);
			vm.scroll_request = true;
			switch_word();
		}
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!vm.running_ex || vm.paused || counting_down);
	if (ImGui::Button("重录##rerec")) {
		// restart current word: stop then re-enter
		vm.scroll_request = true;
		switch_word();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!vm.running_ex || counting_down);
	if (vm.paused) {
		if (ImGui::Button("继续##resume")) {
			vm.scroll_request = true;
			vm.paused = false;
			start_word();
		}
	}
	else {
		if (ImGui::Button("暂停##pause")) {
			vm.paused = true;
			stop_word();
		}
	}
	ImGui::EndDisabled();

	ImGui::Text("Round Index: %d, Music index: %d", 
		vm.current_round, 
		vm.roundConfigs[vm.current_round].bg_music_index
	);

	if (silence_counting_down) {
		utils::SpinnerTopup("Silence",
			64, 48,
			utils::ImColor_HSV(200.0f, 0.8f, 0.8f, 1.0f),
			utils::ImColor_HSV(100.0f, 0.8f, 0.8f, 1.0f),
			utils::ImColor_HSV(100.0f, 0.2f, 0.8f, 1.0f),
			1.0f, (float)spinner_silence_anim_start_time, (float)spinner_silence_anim_duration
		);
	}
	if (silence_counting_down && music_counting_down) {
		ImGui::SameLine();
	}
	if (music_counting_down) {
		utils::SpinnerTopup("Music",
			64, 48,
			utils::ImColor_HSV(300.0f, 0.8f, 0.8f, 1.0f),
			utils::ImColor_HSV(200.0f, 0.8f, 0.8f, 1.0f),
			utils::ImColor_HSV(200.0f, 0.2f, 0.8f, 1.0f),
			1.0f, (float)spinner_music_anim_start_time, (float)spinner_music_anim_duration
		);
	}

	if (waiting_end_time > now) {
		int remain = static_cast<int>(std::ceil(waiting_end_time - now));
		ImGui::Text("Countdown: %d", remain);
	}

	ImGui::EndDisabled();

	ImGui::End();

	utils::Alert(show_alert_please_select_port, "Message##m3", "Please select at least one.");
	utils::Alert(show_alert_please_open_wordlist, "Message##m4", "Please open a words file.");
}

static void ShowMainWindow(bool* p_opt_showmainwindow)
{
	bool show_alert_please_select_port = false;

	ImGui::Begin("Main Window", p_opt_showmainwindow);

	ImGui::BeginDisabled(vm.running_ex);

	static float spinner_anim_start_time = static_cast<float>(ImGui::GetTime());
	static float spinner_anim_duration = 0.0f;
	// unified recording flow state
	static bool counting_down0 = false;
	static double counting_down0_end_time = 0.0f;
	static bool counting_down0_triggered = false;
	static double counting_down0_trigger_time = 0.0f;
	static bool counting_down0_triggered_need_stop = false;
	static bool counting_down0_triggered_need_start = false;
	static bool counting_down1 = false;
	static double counting_down1_end_time = 0.0f;

	bool counting_down = counting_down0 || counting_down1;

	// helper: 3s countdown
	auto startCountdown = [&](
		bool trigger_stop = true, 
		bool trigger_start = true,
		bool need_anim = true
	) {
		if (need_anim) {
			spinner_anim_start_time = static_cast<float>(ImGui::GetTime());
			spinner_anim_duration = 3.0f;
		}
		counting_down0 = true;
		counting_down0_triggered = false;
		counting_down0_trigger_time = ImGui::GetTime() + 1.0f;
		counting_down0_end_time = ImGui::GetTime() + 3.0f;
		counting_down0_triggered_need_stop = trigger_stop;
		counting_down0_triggered_need_start = trigger_start;
	};

	// helper: 1s countdown
	auto startCountdown1 = [&](bool need_anim = true) {
		if (need_anim) {
			spinner_anim_start_time = static_cast<float>(ImGui::GetTime());
			spinner_anim_duration = 1.0f;
		}
		counting_down1 = true;
		counting_down1_end_time = ImGui::GetTime() + 1.0f;
	};

	// helper: enter current word -> start backend recording
	auto enterCurrentWord = [&]() {
		if (g_businessThread) g_businessThread->startRecordingAll(vm.curr_index, "");
	};

	// helper: leave current word -> stop backend recording
	auto leaveCurrentWord = [&]() {
		if (g_businessThread) g_businessThread->stopRecordingAll();
	};

#if 0
	// Test button advances one word and triggers enter sequence (for quick testing)
	if (ImGui::Button("Test")) {
		if (!vm.recWords.empty()) {
			curr_index = (curr_index + 1) % static_cast<int>(vm.recWords.size());
		} else {
			curr_index = 0;
		}
		enterCurrentWord();
	}
#endif

	// display current word under spinner in large font
	if (!vm.recWords.empty()) {
		const char* wordName = vm.recWords[vm.curr_index].name;
		// Use window font scale to make text larger (doubled)
		ImGui::PushFont(UiDpiContext::instance().getBigFont());
		ImGui::Text(wordName);
		ImGui::PopFont();
	}

	// countdown display and completion log
	if (counting_down0) {
		double now = ImGui::GetTime();
		if (now >= counting_down0_end_time) {
			counting_down0 = false;
			// countdown finished — UI becomes enabled (recording data may be arriving)
			if (!vm.recWords.empty()) {
				std::cerr << "Countdown finished for word: " << vm.recWords[vm.curr_index].name << "\n";
			} else {
				std::cerr << "Countdown finished (no words)\n";
			}
		} else if (!counting_down0_triggered && now >= counting_down0_trigger_time) {
			counting_down0_triggered = true;
			if (counting_down0_triggered_need_stop) {
				std::cout << "=============E=============" << std::endl;
				leaveCurrentWord();
			}
			if (counting_down0_triggered_need_start) {
				std::cout << "=============S=============" << std::endl;
				enterCurrentWord();
			}
		}
	}

	if (counting_down1) {
		double now = ImGui::GetTime();
		if (now >= counting_down1_end_time) {
			counting_down1 = false;
			std::cout << "=============S=============" << std::endl;
			leaveCurrentWord();
			// countdown finished — UI becomes enabled (recording data may be arriving)
			if (!vm.recWords.empty()) {
				std::cerr << "Countdown finished for word: " << vm.recWords[vm.curr_index].name << "\n";
			}
			else {
				std::cerr << "Countdown finished (no words)\n";
			}
		}
	}

	ImGui::BeginDisabled(!vm.running || vm.paused || counting_down || vm.curr_index == 0);
	if (ImGui::Button("上一个##prev")) {
		if (vm.curr_index > 0) {
			vm.curr_index--;
			UiDpiContext::instance().setWordIndex(vm.curr_index);
			vm.scroll_request = true;
			startCountdown();
		}
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(vm.paused || counting_down);
	if (!vm.running) {
		if (ImGui::Button("开始##startup")) {
			int count = std::count_if(vm.serialPorts.begin(), vm.serialPorts.end(), [](const SerialPort& port) {
				return port.enable;
			});
			if (count == 0) {
				show_alert_please_select_port = true;
			}
			else {
				vm.paused = false;
				vm.curr_index = 0;
				UiDpiContext::instance().setWordIndex(vm.curr_index);
				vm.scroll_request = true;
				vm.running = true;
				// enter first word
				startCountdown(false);
			}
		}
	}
	else if (!vm.recWords.empty() && (vm.curr_index + 1) >= static_cast<int>(vm.recWords.size())) {
		if (ImGui::Button("结束##exit")) {
			// finish: stop current recordings and mark not running
			startCountdown1();
			vm.running = false;
		}
	}
	else {
		if (ImGui::Button("下一个##next")) {
			// leave current, advance, enter next (unless last)
			vm.curr_index++;
			UiDpiContext::instance().setWordIndex(vm.curr_index);
			vm.scroll_request = true;
			startCountdown();
		}
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!vm.running || vm.paused || counting_down);
	if (ImGui::Button("重录##rerec")) {
		// restart current word: stop then re-enter
		vm.curr_index = vm.curr_index;
		vm.scroll_request = true;
		startCountdown();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!vm.running || counting_down);
	if (vm.paused) {
		if (ImGui::Button("继续##resume")) {
			vm.curr_index = vm.curr_index;
			vm.scroll_request = true;
			vm.paused = false;
			startCountdown(false);
		}
	}
	else {
		if (ImGui::Button("暂停##pause")) {
			vm.paused = true;
			startCountdown1();
		}
	}
	ImGui::EndDisabled();

	if (counting_down) {
		utils::SpinnerTopup("SpinnerTopup",
			64, 48,
			utils::ImColor_HSV(200.0f, 0.8f, 0.8f, 1.0f),
			utils::ImColor_HSV(100.0f, 0.8f, 0.8f, 1.0f),
			utils::ImColor_HSV(100.0f, 0.2f, 0.8f, 1.0f),
			1.0f, spinner_anim_start_time, spinner_anim_duration
		);
		double now = ImGui::GetTime();
		int remain = 0;
		if (counting_down0_end_time > now) remain = static_cast<int>(std::ceil(counting_down0_end_time - now));
		if (counting_down1_end_time > now) remain = static_cast<int>(std::ceil(counting_down1_end_time - now));
		ImGui::Text("Countdown: %d", remain);
	}

	ImGui::EndDisabled();

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
			ImGui::MenuItem("Log Window", NULL, &opt_showlogwindow);
			ImGui::Separator();
			ImGui::MenuItem("Speaker Info", NULL, &opt_showspeakerinfowindow);
			ImGui::MenuItem("USB Port Window", NULL, &opt_showportwindow);
			ImGui::MenuItem("Words List", NULL, &opt_showwordslistwindow);
			ImGui::MenuItem("Main Window", NULL, &opt_showmainwindow);
			ImGui::MenuItem("Main Ex Window", NULL, &opt_showmainexwindow);
			ImGui::MenuItem("Round Config Window", NULL, &opt_showroundconfigwindow);
			ImGui::MenuItem("Active Config Window", NULL, &opt_showactiveconfigwindow);

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

	if (opt_showspeakerinfowindow) {
		ShowSpeakerInfoWindow(&opt_showspeakerinfowindow);
	}

	if (opt_showportwindow) {
		ShowPortWindow(&opt_showportwindow);
	}

	if (opt_showwordslistwindow) {
		ShowwordslistWindow(&opt_showwordslistwindow);
	}

	if (opt_showmainwindow) {
		ShowMainWindow(&opt_showmainwindow);
	}

	if (opt_showmainexwindow) {
		ShowMainExWindow(&opt_showmainexwindow);
	}

	if (opt_showroundconfigwindow) {
		ShowRoundConifgWindow(&opt_showroundconfigwindow);
	}

	if (opt_showactiveconfigwindow) {
		ShowActiveConfigWindow(&opt_showactiveconfigwindow);
	}
}

int main_gui()
{
	ShowRootWindow(&show_root_window);

	return 0;
}
