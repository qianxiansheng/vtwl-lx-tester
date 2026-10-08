#pragma once
#include <filesystem>
#include <string>
#include "StationController.h"

namespace Lx::Tester {

// 测试结果 CSV：<dir>/results_YYYYMMDD.csv，按天分文件，每个工位每轮一行。
// UTF-8 带 BOM（Excel 直接打开中文不乱码）。只在业务线程使用。
class ResultCsv {
public:
	explicit ResultCsv(std::filesystem::path dir = "results") : m_dir(std::move(dir)) {}

	bool append(const RoundRecord& rec, std::string& err);
	// 今天的结果文件路径（UTF-8）
	std::string currentFile() const;

private:
	std::filesystem::path pathFor(long long timeMs) const;
	std::filesystem::path m_dir;
};

} // namespace Lx::Tester
