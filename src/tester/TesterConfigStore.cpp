#include "TesterConfigStore.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include "nlohmann/json.hpp"

namespace Lx::Tester::ConfigStore {

using json = nlohmann::ordered_json;   // 保持字段书写顺序，便于人工查看

namespace {

// 字段缺失或类型不对时回退到默认值，保证旧版本/手改的文件也能加载
template<typename T>
T get(const json& j, const char* key, T def)
{
	auto it = j.find(key);
	if (it == j.end()) return def;
	try {
		return it->get<T>();
	}
	catch (const json::exception&) {
		return def;
	}
}

const json& child(const json& j, const char* key)
{
	static const json empty = json::object();
	auto it = j.find(key);
	return (it != j.end() && it->is_object()) ? *it : empty;
}

json toJson(const ItemCommonConfig& c)
{
	return { { "retryCount", c.retryCount }, { "timeoutMs", c.timeoutMs } };
}

ItemCommonConfig commonFromJson(const json& j, const ItemCommonConfig& def)
{
	ItemCommonConfig c;
	c.retryCount = get<int>(j, "retryCount", def.retryCount);
	c.timeoutMs = get<int>(j, "timeoutMs", def.timeoutMs);
	return c;
}

json toJson(const PersistentConfig& cfg)
{
	const TestConfig& t = cfg.tests;
	json stations = json::array();
	for (const auto& s : cfg.stations)
		stations.push_back({ { "no", s.no }, { "portType", s.portType == PortType::Usb ? "usb" : "uart" }, { "port", s.port } });

	return {
		{ "version", kFormatVersion },
		{ "tests", {
			{ "rfPacket", {
				{ "common", toJson(t.rfPacket.common) },
				{ "minRxRatePct", t.rfPacket.minRxRatePct },
				{ "minTxRatePct", t.rfPacket.minTxRatePct },
				{ "minRssiDbm", t.rfPacket.minRssiDbm },
			} },
			{ "freqOffset", {
				{ "common", toJson(t.freqOffset.common) },
				{ "maxAbsOffsetKHz", t.freqOffset.maxAbsOffsetKHz },
				{ "cfoHzPerLsb", t.freqOffset.cfoHzPerLsb },
			} },
			{ "power", {
				{ "common", toJson(t.power.common) },
				{ "maxPowerUA", t.power.maxPowerUA },
			} },
		} },
		{ "ftFlag", {
			{ "cmdRetryCount", t.ftFlag.cmdRetryCount },
			{ "readDelayMs", t.ftFlag.readDelayMs },
		} },
		{ "comm", {
			{ "cmdTimeoutMs", t.comm.cmdTimeoutMs },
			{ "connCheckTimeoutMs", t.comm.connCheckTimeoutMs },
			{ "heartbeatTimeoutMs", t.comm.heartbeatTimeoutMs },
			{ "pollIntervalMs", t.comm.pollIntervalMs },
			{ "baudRate", t.comm.baudRate },
			{ "usbVid", t.comm.usbVid },
			{ "usbPid", t.comm.usbPid },
		} },
		{ "stations", stations },
	};
}

PersistentConfig fromJson(const json& root)
{
	PersistentConfig cfg;
	TestConfig& t = cfg.tests;
	const TestConfig def;

	const json& tests = child(root, "tests");

	const json& rf = child(tests, "rfPacket");
	t.rfPacket.common = commonFromJson(child(rf, "common"), def.rfPacket.common);
	t.rfPacket.minRxRatePct = get<double>(rf, "minRxRatePct", def.rfPacket.minRxRatePct);
	t.rfPacket.minTxRatePct = get<double>(rf, "minTxRatePct", def.rfPacket.minTxRatePct);
	t.rfPacket.minRssiDbm = get<double>(rf, "minRssiDbm", def.rfPacket.minRssiDbm);

	const json& fo = child(tests, "freqOffset");
	t.freqOffset.common = commonFromJson(child(fo, "common"), def.freqOffset.common);
	t.freqOffset.maxAbsOffsetKHz = get<double>(fo, "maxAbsOffsetKHz", def.freqOffset.maxAbsOffsetKHz);
	t.freqOffset.cfoHzPerLsb = get<double>(fo, "cfoHzPerLsb", def.freqOffset.cfoHzPerLsb);

	const json& pw = child(tests, "power");
	t.power.common = commonFromJson(child(pw, "common"), def.power.common);
	t.power.maxPowerUA = get<double>(pw, "maxPowerUA", def.power.maxPowerUA);

	const json& ff = child(root, "ftFlag");
	t.ftFlag.cmdRetryCount = get<int>(ff, "cmdRetryCount", def.ftFlag.cmdRetryCount);
	t.ftFlag.readDelayMs = get<int>(ff, "readDelayMs", def.ftFlag.readDelayMs);

	const json& cm = child(root, "comm");
	t.comm.cmdTimeoutMs = get<int>(cm, "cmdTimeoutMs", def.comm.cmdTimeoutMs);
	t.comm.connCheckTimeoutMs = get<int>(cm, "connCheckTimeoutMs", def.comm.connCheckTimeoutMs);
	t.comm.heartbeatTimeoutMs = get<int>(cm, "heartbeatTimeoutMs", def.comm.heartbeatTimeoutMs);
	t.comm.pollIntervalMs = get<int>(cm, "pollIntervalMs", def.comm.pollIntervalMs);
	t.comm.baudRate = get<int>(cm, "baudRate", def.comm.baudRate);
	t.comm.usbVid = get<int>(cm, "usbVid", def.comm.usbVid);
	t.comm.usbPid = get<int>(cm, "usbPid", def.comm.usbPid);

	sanitize(t);

	auto it = root.find("stations");
	if (it != root.end() && it->is_array()) {
		for (const auto& s : *it) {
			if (!s.is_object()) continue;
			StationSetting st;
			st.no = get<int>(s, "no", 0);
			st.portType = get<std::string>(s, "portType", "usb") == "uart" ? PortType::Uart : PortType::Usb;
			st.port = get<std::string>(s, "port", "");
			if (st.no >= 1 && st.no <= kMaxStations)
				cfg.stations.push_back(std::move(st));
		}
	}
	return cfg;
}

void sanitizeCommon(ItemCommonConfig& c)
{
	c.retryCount = std::clamp(c.retryCount, 0, kMaxRetry);
	c.timeoutMs = std::clamp(c.timeoutMs, 100, 600000);
}

} // namespace

void sanitize(TestConfig& cfg)
{
	sanitizeCommon(cfg.rfPacket.common);
	sanitizeCommon(cfg.freqOffset.common);
	sanitizeCommon(cfg.power.common);
	cfg.rfPacket.minRxRatePct = std::clamp(cfg.rfPacket.minRxRatePct, 0.0, 100.0);
	cfg.rfPacket.minTxRatePct = std::clamp(cfg.rfPacket.minTxRatePct, 0.0, 100.0);
	cfg.freqOffset.maxAbsOffsetKHz = std::max(cfg.freqOffset.maxAbsOffsetKHz, 0.0);
	cfg.power.maxPowerUA = std::max(cfg.power.maxPowerUA, 0.0);
	cfg.rfPacket.minRssiDbm = std::clamp(cfg.rfPacket.minRssiDbm, -127.0, 127.0);
	if (!(cfg.freqOffset.cfoHzPerLsb > 0.0)) cfg.freqOffset.cfoHzPerLsb = 1.0;
	cfg.ftFlag.cmdRetryCount = std::clamp(cfg.ftFlag.cmdRetryCount, 0, 10);
	cfg.ftFlag.readDelayMs = std::clamp(cfg.ftFlag.readDelayMs, 0, 60000);
	cfg.comm.cmdTimeoutMs = std::clamp(cfg.comm.cmdTimeoutMs, 20, 10000);
	cfg.comm.connCheckTimeoutMs = std::clamp(cfg.comm.connCheckTimeoutMs, 350, 10000);
	cfg.comm.heartbeatTimeoutMs = std::clamp(cfg.comm.heartbeatTimeoutMs, 500, 10000);
	cfg.comm.pollIntervalMs = std::clamp(cfg.comm.pollIntervalMs, 10, 5000);
	cfg.comm.baudRate = std::clamp(cfg.comm.baudRate, 1200, 4000000);
	cfg.comm.usbVid = std::clamp(cfg.comm.usbVid, 0, 0xFFFF);
	cfg.comm.usbPid = std::clamp(cfg.comm.usbPid, 0, 0xFFFF);
}

LoadResult load(const std::filesystem::path& path, PersistentConfig& out, std::string& err)
{
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		out = PersistentConfig{};
		return LoadResult::NotFound;
	}

	try {
		std::ifstream in(path, std::ios::binary);
		if (!in) throw std::runtime_error("无法打开文件");
		json root = json::parse(in);
		if (!root.is_object()) throw std::runtime_error("根节点不是 JSON 对象");
		out = fromJson(root);
		return LoadResult::Ok;
	}
	catch (const std::exception& e) {
		err = e.what();
		out = PersistentConfig{};
		// 保留损坏的文件，避免下一次自动保存把用户的配置覆盖掉
		auto bad = path;
		bad += ".bad";
		std::filesystem::copy_file(path, bad, std::filesystem::copy_options::overwrite_existing, ec);
		return LoadResult::Invalid;
	}
}

bool save(const std::filesystem::path& path, const PersistentConfig& cfg, std::string& err)
{
	auto tmp = path;
	tmp += ".tmp";
	try {
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out) throw std::runtime_error("无法写入临时文件");
			out << toJson(cfg).dump(2) << '\n';
			out.flush();
			if (!out) throw std::runtime_error("写入失败");
		}
		std::filesystem::rename(tmp, path);   // 同卷内替换已有文件
		return true;
	}
	catch (const std::exception& e) {
		err = e.what();
		std::error_code ec;
		std::filesystem::remove(tmp, ec);
		return false;
	}
}

} // namespace Lx::Tester::ConfigStore
