#pragma once
#include <filesystem>
#include <string>
#include "TesterTypes.h"

// 用户配置 <-> config.json。纯函数，不持有状态，由业务线程调用。
namespace Lx::Tester::ConfigStore {

constexpr int kFormatVersion = 1;

// 把越界的参数拉回合法范围（加载文件、UI 提交时都要过一遍）
void sanitize(TestConfig& cfg);

enum class LoadResult {
	Ok,
	NotFound,     // 文件不存在，使用默认值
	Invalid,      // 解析失败，使用默认值；原文件已备份为 *.bad
};

// 读取失败时 out 保持默认值；err 给出可读的原因
LoadResult load(const std::filesystem::path& path, PersistentConfig& out, std::string& err);

// 先写临时文件再替换，避免写一半断电导致配置损坏
bool save(const std::filesystem::path& path, const PersistentConfig& cfg, std::string& err);

} // namespace Lx::Tester::ConfigStore
