#pragma once
#include <iostream>
#include <string>
#include <codecvt>
#include <filesystem>
#include <mutex>
#include "serial/serial.h"
#include "../imgui/imgui.h"

namespace utils {

	void Alert(bool show, const char* title, const char* text);
	bool Confirm(bool show, const char* title, const char* text);

	void readFileData(std::filesystem::path path, void* out_data);
	std::string readFileText(std::filesystem::path path);

	uint8_t htoi_4(const char c);
	uint8_t htoi_8(const char* c);
	uint16_t htoi_16(const char* c);
	uint32_t htoi_32(const char* c);
	void ValidateU8Text(char* text, uint8_t& v);
	void ValidateU16Text(char* text, uint16_t& v);
	void ValidateU32Text(char* text, uint32_t& v);
	void ValidateIntText(char* text, uint32_t& v);
	void ValidateDataText(char* text, uint8_t* data, size_t& size);
	void ValidateHex16ByteText(char* text, uint8_t* data);
	bool ValidateVersion(const char* v);

	std::string dec2hex(uint32_t i);
	void printf_hexdump(uint8_t* data, uint16_t size);
	unsigned char Hex2String(const unsigned char* pSrc, unsigned char* dest, int nL);
	void String2Hex(const char* src, unsigned char* dest, int srcL);

	std::wstring utf8_to_wstr(const std::string& src);
	std::string wstr_to_utf8(const std::wstring& src);
	std::string utf8_to_gbk(const std::string& str);
	std::string gbk_to_utf8(const std::string& str);
	std::wstring gbk_to_wstr(const std::string& str);
	std::string wstr_to_gbk(const std::wstring& src);

	// C++20 标准方式：直接使用 path 构造函数 + char8_t
	std::filesystem::path make_u8path(const std::string& utf8_string);
	// 重载版本：接受字符串字面量
	std::filesystem::path make_u8path(const char8_t* utf8_string);
	// 重载版本：接受 std::u8string
	std::filesystem::path make_u8path(const std::u8string& utf8_string);
	// 从 path 获取 UTF-8 std::string
	std::string path_to_utf8_string(const std::filesystem::path& p);
	// 从 path 获取 std::u8string（C++20 推荐）
	std::u8string path_to_u8string(const std::filesystem::path& p);

	void my_sleep(unsigned long milliseconds);

	long long get_current_system_time_ms();
	long long get_current_system_time_s();

	void logToFile(const std::string& message, const std::string& filename = "log.txt");

	void ImColor_HSV(ImColor* out_color, float h, float s, float v, float a);

	ImColor ImColor_HSV(float h, float s, float v, float a);
	void SpinnerTopup(const char* label, float radius1, float radius2, const ImColor& color, const ImColor& fg, const ImColor& bg, float speed, float start_time, float duration_time);
}
