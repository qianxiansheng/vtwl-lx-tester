#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>
#include "TesterTypes.h"

namespace Lx::Tester {

// Model：线程安全的数据仓库，唯一数据源。
// - 写：业务线程（TesterService）调用 setter，setter 置对应 dirty 位。
// - 读：UI 线程（TesterViewModel）每帧 consumeDirtyFlags()，只对变化的部分拉快照。
class TesterModel {
public:
	enum DirtyFlags : uint32_t {
		Dirty_None     = 0,
		Dirty_Global   = 1 << 0,
		Dirty_Stations = 1 << 1,
		Dirty_Config   = 1 << 2,
		Dirty_Ports    = 1 << 3,
		Dirty_Log      = 1 << 4,   // 有新日志
		Dirty_LogReset = 1 << 5,   // 日志被清空
		Dirty_Alert    = 1 << 6,   // 有新的弹窗事件
		Dirty_Stats    = 1 << 7,   // 统计 / 结果文件
		Dirty_All      = (1 << 8) - 1,
	};

	static constexpr size_t kMaxLogEntries = 5000;

	// ---- 读（任意线程）
	GlobalState              globalState() const;
	std::vector<StationInfo> stations() const;
	TestConfig               config() const;
	std::vector<PortEntry>   availablePorts() const;
	std::vector<LogEntry>    logsSince(uint64_t seq) const;
	std::vector<AlertEvent>  takeAlerts();
	StatsSnapshot            stats() const;
	uint32_t                 consumeDirtyFlags() { return m_dirty.exchange(Dirty_None); }

	// ---- 持久化（与 UI dirty flags 相互独立：UI 消费 dirty 不影响保存判断）
	// 用户配置每次变化 revision +1；保存方比较 revision 决定是否需要写盘
	PersistentConfig         persistentConfig(uint64_t* revision = nullptr) const;
	void                     restorePersistentConfig(const PersistentConfig& cfg);

	// ---- 写（业务线程）
	void setGlobalState(GlobalState s);
	int  addStation();                                   // 返回新工位编号，满则返回 0
	bool removeStation(int no);
	bool setStationPort(int no, const std::string& port);  // 用户配置，计入持久化
	bool setStationPortType(int no, PortType type);       // 用户配置，计入持久化（同时清空端口）
	bool updateStation(int no, const std::function<void(StationInfo&)>& fn);  // 仅运行时字段
	void updateAllStations(const std::function<void(StationInfo&)>& fn);
	void setConfig(const TestConfig& cfg);
	void setAvailablePorts(std::vector<PortEntry> ports);
	void appendLog(LogLevel level, std::string text);
	void clearLog();
	uint64_t pushAlert(AlertEvent ev);                   // 返回分配的 alert id
	void recordVerdict(int no, Verdict v);               // 统计一轮结果
	void resetStats();
	void setResultFile(const std::string& path);

private:
	void markDirty(uint32_t f) { m_dirty.fetch_or(f); }

	mutable std::mutex       m_mutex;
	GlobalState              m_global = GlobalState::Preparing;
	std::vector<StationInfo> m_stations;                // 按工位编号升序
	TestConfig               m_config;
	std::vector<PortEntry>   m_ports;
	std::deque<LogEntry>     m_logs;
	uint64_t                 m_logSeq = 0;
	std::vector<AlertEvent>  m_alerts;
	uint64_t                 m_alertSeq = 0;
	uint64_t                 m_persistRevision = 0;
	std::vector<StationStats> m_stats;
	std::string              m_resultFile;
	std::atomic<uint32_t>    m_dirty{ Dirty_All };
};

} // namespace Lx::Tester
