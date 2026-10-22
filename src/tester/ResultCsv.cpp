#include "ResultCsv.h"
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace Lx::Tester {

namespace {

std::tm localTm(long long timeMs)
{
	std::time_t t = static_cast<std::time_t>(timeMs / 1000);
	std::tm tm{};
	localtime_s(&tm, &t);
	return tm;
}

const char* stateText(ItemState s)
{
	switch (s) {
	case ItemState::Pass:  return "PASS";
	case ItemState::Fail:  return "FAIL";
	case ItemState::Abort: return "ABORT";
	case ItemState::Skipped: return "SKIP";
	default:               return "";
	}
}

const char* verdictText(Verdict v)
{
	switch (v) {
	case Verdict::Pass:  return "PASS";
	case Verdict::Fail:  return "FAIL";
	case Verdict::Abort: return "ABORT";
	default:             return "";
	}
}

// CSV 字段转义：含逗号/引号/换行时加引号
std::string esc(const std::string& s)
{
	if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
	std::string out = "\"";
	for (char c : s) {
		if (c == '"') out += '"';
		out += c;
	}
	return out + "\"";
}

std::string rcField(const ItemResult& r)
{
	if (r.resultCode < 0) return "";
	char buf[8];
	snprintf(buf, sizeof(buf), "0x%02X", r.resultCode);
	return buf;
}

std::string num(double v, int prec)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%.*f", prec, v);
	return buf;
}

const char* const kHeader =
	"时间,工位,端口,dut_id,测试板固件,测试板硬件,紧急停止,"
	"收发包_总发包,收发包_测试板收包数,收发包_被测板收包数,收发包_RSSI(dBm),收发包_收包率(%),收发包_发包率(%),收发包_result_code,收发包_判定,收发包_说明,"
	"频偏_CFO原始值,频偏_KHz,频偏_result_code,频偏_判定,频偏_说明,"
	"功耗_平均(uA),功耗_最大(uA),功耗_最小(uA),功耗_result_code,功耗_判定,功耗_说明,"
	"产测标志_读回值,产测标志_result_code,产测标志_判定,产测标志_说明,"
	"综合结果";

// 读文件第一行（去掉 BOM）
std::string firstLine(const std::filesystem::path& p)
{
	std::ifstream in(p, std::ios::binary);
	std::string line;
	std::getline(in, line);
	if (!line.empty() && line.back() == '\r') line.pop_back();
	if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
	return line;
}

} // namespace

// 当天文件已存在但表头不同（程序升级后字段变化）时，改写到 results_YYYYMMDD_2.csv、_3 ...，避免列错位
std::filesystem::path ResultCsv::pathFor(long long timeMs) const
{
	const std::tm tm = localTm(timeMs);
	char date[16];
	std::strftime(date, sizeof(date), "%Y%m%d", &tm);
	std::error_code ec;
	for (int i = 1; ; ++i) {
		const std::string name = "results_" + std::string(date) + (i == 1 ? "" : "_" + std::to_string(i)) + ".csv";
		const auto p = m_dir / name;
		if (!std::filesystem::exists(p, ec) || firstLine(p) == kHeader) return p;
	}
}

std::string ResultCsv::currentFile() const
{
	using namespace std::chrono;
	const long long now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	const std::u8string u8 = std::filesystem::absolute(pathFor(now)).u8string();
	return std::string(u8.begin(), u8.end());
}

bool ResultCsv::append(const RoundRecord& rec, std::string& err)
{
	const auto path = pathFor(rec.timeMs);
	std::error_code ec;
	std::filesystem::create_directories(m_dir, ec);
	const bool isNew = !std::filesystem::exists(path, ec);

	std::ofstream out(path, std::ios::binary | std::ios::app);
	if (!out) {
		err = "无法打开结果文件";
		return false;
	}
	if (isNew) out << "\xEF\xBB\xBF" << kHeader << "\r\n";

	const std::tm tm = localTm(rec.timeMs);
	char ts[32];
	std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

	const ItemResult& rf = rec.items[0];
	const ItemResult& fo = rec.items[1];
	const ItemResult& pw = rec.items[2];

	std::ostringstream row;
	row << ts << ',' << rec.no << ',' << esc(rec.port) << ',' << esc(rec.dutId) << ','
		<< esc(rec.boardFw) << ',' << esc(rec.boardHw) << ',' << (rec.emergencyStopped ? "是" : "") << ',';

	if (rec.hasTxRx)
		row << rec.txrx.total << ',' << rec.txrx.boardRx << ',' << rec.txrx.dutRx << ',' << static_cast<int>(rec.txrx.rssi) << ','
			<< num(rf.values[0], 1) << ',' << num(rf.values[1], 1) << ',';
	else
		row << ",,,,,,";
	row << rcField(rf) << ',' << stateText(rf.state) << ',' << esc(rf.note) << ',';

	if (rec.hasCfo)
		row << rec.cfo.raw << ',' << num(fo.values[0], 3) << ',';
	else
		row << ",,";
	row << rcField(fo) << ',' << stateText(fo.state) << ',' << esc(fo.note) << ',';

	if (rec.hasPower)
		row << rec.power.avgUA << ',' << rec.power.maxUA << ',' << rec.power.minUA << ',';
	else
		row << ",,,";
	row << rcField(pw) << ',' << stateText(pw.state) << ',' << esc(pw.note) << ',';

	const ItemResult& ff = rec.ftFlag;
	if (ff.valueCount > 0) row << static_cast<int>(ff.values[0]);
	row << ',' << rcField(ff) << ',' << stateText(ff.state) << ',' << esc(ff.note) << ',';

	row << verdictText(rec.verdict) << "\r\n";
	out << row.str();
	out.flush();
	if (!out) {
		err = "写入结果文件失败";
		return false;
	}
	return true;
}

} // namespace Lx::Tester
