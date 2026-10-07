#include "utils.h"
#include <sstream>
#include <vector>
#include <fstream>
#include <functional>

#include "imgui.h"
#include "imgui/imgui_internal.h"
#include "ImConsole.h"

// OS Specific sleep
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

extern ImLogger* logger;

void utils::Alert(bool show, const char* title, const char* text)
{
	if (show)
		ImGui::OpenPopup(title);

	// Always center this window when appearing
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text(text);
		ImGui::Separator();

		if (ImGui::Button("OK", ImVec2(120, 0))) { ImGui::CloseCurrentPopup(); }
		ImGui::SetItemDefaultFocus();
		ImGui::EndPopup();
	}
}

bool utils::Confirm(bool show, const char* title, const char* text)
{
	if (show)
		ImGui::OpenPopup(title);

	// Always center this window when appearing
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	bool result = false;

	if (ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text(text);
		ImGui::Separator();

		if (ImGui::Button("OK", ImVec2(120, 0))) 
		{ 
			ImGui::CloseCurrentPopup(); 
			result = true;
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) 
		{ 
			ImGui::CloseCurrentPopup(); 
		}
		ImGui::EndPopup();
	}
	return result;
}

void utils::readFileData(std::filesystem::path path, void* out_data)
{
	std::ifstream inFile;
	inFile.open(path, std::ios::in | std::ios::binary);
	while (inFile.read((char*)out_data, std::filesystem::file_size(path)));
	inFile.close();
}

std::string utils::readFileText(std::filesystem::path path)
{
	std::ifstream inFile;
	std::stringstream ss;
	std::string content;
	inFile.open(path, std::ios::in);

	while (!inFile.eof())
	{
		inFile >> content;
		ss << content;
	}
	inFile.close();

	return ss.str();
}


uint8_t utils::htoi_4(const char c)
{
	if ('0' <= c && c <= '9') return c - '0';
	if ('A' <= c && c <= 'F') return c - 'A' + 10;
	if ('a' <= c && c <= 'f') return c - 'a' + 10;
	return 0;
}
uint8_t utils::htoi_8(const char* c)
{
	return (htoi_4(c[0]) << 4) | htoi_4(c[1]);
}
uint16_t utils::htoi_16(const char* c)
{
	return (htoi_8(c) << 8) | htoi_8(c + 2);
}
uint32_t utils::htoi_32(const char* c)
{
	return (htoi_8(c) << 24) | (htoi_8(c + 2) << 16) | (htoi_8(c + 4) << 8) | htoi_8(c + 6);
}

void utils::ValidateU8Text(char* text, uint8_t& v)
{
	// Load address
	if (text[0] == '\0') {
		v = 0;
		strcpy(text, "00");
	}
	else {
		if (strlen(text) == 1) {
			text[2] = '\0';
			text[1] = text[0];
			text[0] = '0';
		}
		v = utils::htoi_8(text);
	}
}
void utils::ValidateU16Text(char* text, uint16_t& v)
{
	// Load address
	if (text[0] == '\0') {
		v = 0;
		strcpy(text, "0000");
	}
	else {
		if (strlen(text) == 1) {
			text[4] = '\0';
			text[3] = text[0];
			text[2] = '0';
			text[1] = '0';
			text[0] = '0';
		}
		if (strlen(text) == 2) {
			text[4] = '\0';
			text[3] = text[1];
			text[2] = text[0];
			text[1] = '0';
			text[0] = '0';
		}
		if (strlen(text) == 3) {
			text[4] = '\0';
			text[3] = text[2];
			text[2] = text[1];
			text[1] = text[0];
			text[0] = '0';
		}
		v = utils::htoi_16(text);
	}
}
void utils::ValidateU32Text(char* text, uint32_t& v)
{
	// Load address
	if (text[0] == '\0') {
		v = 0;
		strcpy(text, "00000000");
	}
	else {
		size_t len = strlen(text);
		for (size_t i = 0; i < 8; ++i)
			text[7 - i] = (i < len) ? text[len - 1 - i] : '0';
		text[8] = '\0';
		v = utils::htoi_32(text);
	}
}

void utils::ValidateIntText(char* text, uint32_t& v)
{
	// Load address
	bool has_sign = (text[0] == '-' || text[0] == '+');
	bool has_float = false;

	if (text[0] == '\0') {
		v = 0;
		strcpy(text, "0");
	}
	else if (text[0] == '-' && text[1] == '\0') {
		v = 0;
		strcpy(text, "-");
	}
	else {
		v = std::stoi(text);
	}
}
void utils::ValidateDataText(char* text, uint8_t* data, size_t& size)
{
	// Load address
	if (text[0] == '\0') {
		size = 0;
	}
	else {
		if (text[0] == '0' && text[1] == 'x') {
			size = (strlen(text) - 2) / 2;

			for (size_t i = 0; i < size; ++i) {
				data[i] = utils::htoi_8(text + 2 + (i * 2));
			}
		}
		else {
			size = strlen(text);
			strcpy((char*)data, text);
		}
	}
}
void utils::ValidateHex16ByteText(char* text, uint8_t* data)
{
	uint8_t rl = 32 - (uint8_t)strlen(text);
	std::stringstream ss;
	for (uint8_t i = 0; i < rl; ++i)
		ss << '0';
	ss << text;
	strcpy(text, ss.str().c_str());

	utils::String2Hex(text, data, 32);
}
bool utils::ValidateVersion(const char* v)
{
	if (v[0] != 'V' || v[2] != '.' || v[4] != '.')
		return false;
	if (v[1] < '0' || '9' < v[1])
		return false;
	if (v[3] < '0' || '9' < v[3])
		return false;
	if (v[5] < '0' || '9' < v[5])
		return false;
	return true;
}


std::string utils::dec2hex(uint32_t i)
{
	std::ostringstream ss;
	ss << "0x" << std::hex << i;
	return ss.str();
}


void utils::printf_hexdump(uint8_t* data, uint16_t size)
{
	std::cout << std::hex;
	for (uint16_t i = 0; i < size; ++i) {
		std::cout << (int)data[i];
	}
	std::cout << std::dec << std::endl;
}


unsigned char utils::Hex2String(const unsigned char* pSrc, unsigned char* dest, int nL)
{
	unsigned char* buf_hex = 0;
	int i;
	unsigned char hb;
	unsigned char lb;
	buf_hex = dest;
	memset((char*)buf_hex, 0, sizeof(buf_hex));
	hb = 0;
	lb = 0;
	for (i = 0; i < nL; i++)
	{
		hb = (pSrc[i] & 0xf0) >> 4;
		if (hb >= 0 && hb <= 9)
			hb += 0x30;
		else if (hb >= 10 && hb <= 15)
			hb = hb - 10 + 'A';
		else
			return 1;
		lb = pSrc[i] & 0x0f;
		if (lb >= 0 && lb <= 9)
			lb += 0x30;
		else if (lb >= 10 && lb <= 15)
			lb = lb - 10 + 'A';
		else
			return 1;
		buf_hex[i * 2] = hb;
		buf_hex[i * 2 + 1] = lb;
	}
	return 0;
}

void utils::String2Hex(const char* src, unsigned char* dest, int srcL)
{
	int halfSrcL = srcL / 2;
	for (int i = 0; i < halfSrcL; ++i)
		dest[i] = htoi_8(src + (i * 2));
}
std::wstring utils::utf8_to_wstr(const std::string& src) {
	if (src.empty()) return {};

	int size_needed = MultiByteToWideChar(CP_UTF8, 0, src.c_str(),
		(int)src.size(), nullptr, 0);
	std::wstring result(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, src.c_str(), (int)src.size(),
		&result[0], size_needed);
	return result;
}

// UTF-16 转 UTF-8
std::string utils::wstr_to_utf8(const std::wstring& src) {
	if (src.empty()) return {};

	int size_needed = WideCharToMultiByte(CP_UTF8, 0, src.c_str(),
		(int)src.size(), nullptr, 0,
		nullptr, nullptr);
	std::string result(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, src.c_str(), (int)src.size(),
		&result[0], size_needed, nullptr, nullptr);
	return result;
}

// UTF-8 转 GBK
std::string utils::utf8_to_gbk(const std::string& str) {
	// UTF-8 -> UTF-16 -> GBK
	std::wstring wide = utf8_to_wstr(str);
	return utils::wstr_to_gbk(wide);
}

// GBK 转 UTF-8
std::string utils::gbk_to_utf8(const std::string& str) {
	// GBK -> UTF-16 -> UTF-8
	std::wstring wide = gbk_to_wstr(str);
	return wstr_to_utf8(wide);
}

// GBK 转 UTF-16
std::wstring utils::gbk_to_wstr(const std::string& str) {
	if (str.empty()) return {};

	int size_needed = MultiByteToWideChar(CP_ACP, 0, str.c_str(),
		(int)str.size(), nullptr, 0);
	std::wstring result(size_needed, 0);
	MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(),
		&result[0], size_needed);
	return result;
}

// UTF-16 转 GBK
std::string utils::wstr_to_gbk(const std::wstring& src) {
	if (src.empty()) return {};

	int size_needed = WideCharToMultiByte(CP_ACP, 0, src.c_str(),
		(int)src.size(), nullptr, 0,
		nullptr, nullptr);
	std::string result(size_needed, 0);
	WideCharToMultiByte(CP_ACP, 0, src.c_str(), (int)src.size(),
		&result[0], size_needed, nullptr, nullptr);
	return result;
}

std::filesystem::path utils::make_u8path(const std::string& utf8_string) {
	// 将 std::string (char) 视为 char8_t 字符串
	return std::filesystem::path(
		std::u8string_view(
			reinterpret_cast<const char8_t*>(utf8_string.data()),
			utf8_string.size()
		)
	);
}

std::filesystem::path utils::make_u8path(const char8_t* utf8_string) {
	return std::filesystem::path(utf8_string);
}

std::filesystem::path utils::make_u8path(const std::u8string& utf8_string) {
	return std::filesystem::path(utf8_string);
}

std::string utils::path_to_utf8_string(const std::filesystem::path& p) {
	std::u8string u8str = p.generic_u8string();
	return std::string(
		reinterpret_cast<const char*>(u8str.data()),
		u8str.size()
	);
}

std::u8string utils::path_to_u8string(const std::filesystem::path& p) {
	return p.generic_u8string();
}

void utils::my_sleep(unsigned long milliseconds) {
#ifdef _WIN32
	Sleep(milliseconds); // 100 ms
#else
	usleep(milliseconds * 1000); // 100 ms
#endif
}

long long utils::get_current_system_time_ms()
{
	std::chrono::system_clock::time_point time_point_now = std::chrono::system_clock::now();
	std::chrono::system_clock::duration duration_since_epoch = time_point_now.time_since_epoch();
	return std::chrono::duration_cast<std::chrono::milliseconds>(duration_since_epoch).count();
}
long long utils::get_current_system_time_s()
{
	std::chrono::system_clock::time_point time_point_now = std::chrono::system_clock::now();
	std::chrono::system_clock::duration duration_since_epoch = time_point_now.time_since_epoch();
	return std::chrono::duration_cast<std::chrono::seconds>(duration_since_epoch).count();
}

void utils::logToFile(const std::string& message, const std::string& filename) 
{
	std::time_t currentTime = std::time(nullptr);
	std::tm* timeinfo = std::localtime(&currentTime);

	std::ofstream logfile(filename, std::ios::app);
	if (logfile.is_open()) {
		char buffer[80];
		strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S] ", timeinfo);

		logfile << buffer << message << std::endl;

		logfile.close();
	}
	else {
		std::cerr << "Error opening the log file." << std::endl;
	}
}

// HSV 转 RGB 的核心函数
inline void ImGui_ColorConvertHSVtoRGB(float h, float s, float v, float& out_r, float& out_g, float& out_b) {
	h = fmodf(h, 360.0f);
	if (h < 0) h += 360.0f;
	s = std::clamp(s, 0.0f, 1.0f);
	v = std::clamp(v, 0.0f, 1.0f);

	if (s <= 0.0f) {
		out_r = out_g = out_b = v;
		return;
	}

	float hh = h / 60.0f;
	int i = (int)hh;
	float ff = hh - i;
	float p = v * (1.0f - s);
	float q = v * (1.0f - (s * ff));
	float t = v * (1.0f - (s * (1.0f - ff)));

	switch (i) {
	case 0: out_r = v; out_g = t; out_b = p; break;
	case 1: out_r = q; out_g = v; out_b = p; break;
	case 2: out_r = p; out_g = v; out_b = t; break;
	case 3: out_r = p; out_g = q; out_b = v; break;
	case 4: out_r = t; out_g = p; out_b = v; break;
	default: out_r = v; out_g = p; out_b = q; break;
	}
}

void utils::ImColor_HSV(ImColor* out_color, float h, float s, float v, float a = 1.0f) {
	float r, g, b;
	ImGui_ColorConvertHSVtoRGB(h, s, v, r, g, b);
	*out_color = ImColor(r, g, b, a);
}

ImColor utils::ImColor_HSV(float h, float s, float v, float a = 1.0f) {
	float r, g, b;
	ImGui_ColorConvertHSVtoRGB(h, s, v, r, g, b);
	return ImColor(r, g, b, a);
}

static inline bool SpinnerBegin(const char* label, float radius, ImVec2& pos, ImVec2& size, ImVec2& centre, int& num_segments) {
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext& g = *GImGui;
	const ImGuiStyle& style = g.Style;
	const ImGuiID id = window->GetID(label);

	pos = window->DC.CursorPos;
	// The size of the spinner is set to twice the radius, plus some padding based on the style
	size = ImVec2((radius) * 2, (radius + style.FramePadding.y) * 2);

	const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
	ImGui::ItemSize(bb, style.FramePadding.y);

	num_segments = window->DrawList->_CalcCircleAutoSegmentCount(radius);

	centre = bb.GetCenter();
	// If the item cannot be added to the window, return false
	if (!ImGui::ItemAdd(bb, id))
		return false;

	return true;
}

static inline ImColor color_alpha(ImColor c, float alpha) { c.Value.w *= alpha * ImGui::GetStyle().Alpha; return c; }

void utils::SpinnerTopup(const char* label, float radius1, float radius2, const ImColor& color, const ImColor& fg, const ImColor& bg, float speed = 2.8f, float start_time = ImGui::GetTime(), float duration_time = 3.0f)
{
	const float radius = ImMax(radius1, radius2);
	constexpr float PI_DIV_2 = IM_PI / 2.f;

	ImVec2 pos, size, centre; int num_segments;
	if (!SpinnerBegin(label, radius, pos, size, centre, num_segments)) { return; };
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	auto circle = [&](const std::function<ImVec2(int)>& point_func, ImU32 dbc, float dth) {
		window->DrawList->PathClear();
		for (int i = 0; i < num_segments; i++) {
			ImVec2 p = point_func(i);
			window->DrawList->PathLineTo(ImVec2(centre.x + p.x, centre.y + p.y));
		}
		window->DrawList->PathStroke(dbc, 0, dth);
	};

	float elapsed = ImGui::GetTime() - start_time;
	float t = ImClamp(elapsed / duration_time, 0.0f, 1.0f);
	const float start = t * IM_PI;

	// const float start = ImFmod((float)ImGui::GetTime() * speed, IM_PI);
	window->DrawList->AddCircleFilled(centre, radius1, color_alpha(bg, 1.f), num_segments);

	const float abegin = (PI_DIV_2)-start;
	const float aend = (PI_DIV_2)+start;
	const float angle_offset = (aend - abegin) / num_segments;

	window->DrawList->PathClear();
	window->DrawList->PathArcTo(centre, radius1, abegin, aend, num_segments * 2);

	ImDrawListFlags save = window->DrawList->Flags;
	window->DrawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;

	window->DrawList->PathFillConvex(color_alpha(color, 1.f));

	window->DrawList->AddCircleFilled(centre, radius2, color_alpha(fg, 1.f), num_segments);

	window->DrawList->Flags = save;
}