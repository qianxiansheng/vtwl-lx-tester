#include "TesterModel.h"
#include <algorithm>
#include <chrono>

namespace Lx::Tester {

GlobalState TesterModel::globalState() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	return m_global;
}

std::vector<StationInfo> TesterModel::stations() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	return m_stations;
}

TestConfig TesterModel::config() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	return m_config;
}

std::vector<PortEntry> TesterModel::availablePorts() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	return m_ports;
}

std::vector<LogEntry> TesterModel::logsSince(uint64_t seq) const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	auto it = std::upper_bound(m_logs.begin(), m_logs.end(), seq,
		[](uint64_t s, const LogEntry& e) { return s < e.seq; });
	return std::vector<LogEntry>(it, m_logs.end());
}

std::vector<AlertEvent> TesterModel::takeAlerts()
{
	std::lock_guard<std::mutex> lk(m_mutex);
	std::vector<AlertEvent> out;
	out.swap(m_alerts);
	return out;
}

void TesterModel::setGlobalState(GlobalState s)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (m_global == s) return;
		m_global = s;
	}
	markDirty(Dirty_Global);
}

int TesterModel::addStation()
{
	int no = 0;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (m_stations.size() >= kMaxStations) return 0;
		// 取最小的空闲编号
		for (no = 1; no <= kMaxStations; ++no) {
			bool used = std::any_of(m_stations.begin(), m_stations.end(),
				[no](const StationInfo& s) { return s.no == no; });
			if (!used) break;
		}
		StationInfo st;
		st.no = no;
		auto pos = std::find_if(m_stations.begin(), m_stations.end(),
			[no](const StationInfo& s) { return s.no > no; });
		m_stations.insert(pos, st);
		++m_persistRevision;
	}
	markDirty(Dirty_Stations);
	return no;
}

bool TesterModel::removeStation(int no)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		auto it = std::find_if(m_stations.begin(), m_stations.end(),
			[no](const StationInfo& s) { return s.no == no; });
		if (it == m_stations.end()) return false;
		m_stations.erase(it);
		++m_persistRevision;
	}
	markDirty(Dirty_Stations);
	return true;
}

bool TesterModel::setStationPort(int no, const std::string& port)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		auto it = std::find_if(m_stations.begin(), m_stations.end(),
			[no](const StationInfo& s) { return s.no == no; });
		if (it == m_stations.end()) return false;
		if (it->port == port) return true;
		it->port = port;
		++m_persistRevision;
	}
	markDirty(Dirty_Stations);
	return true;
}

bool TesterModel::updateStation(int no, const std::function<void(StationInfo&)>& fn)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		auto it = std::find_if(m_stations.begin(), m_stations.end(),
			[no](const StationInfo& s) { return s.no == no; });
		if (it == m_stations.end()) return false;
		fn(*it);
	}
	markDirty(Dirty_Stations);
	return true;
}

void TesterModel::updateAllStations(const std::function<void(StationInfo&)>& fn)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		for (auto& s : m_stations) fn(s);
	}
	markDirty(Dirty_Stations);
}

void TesterModel::setConfig(const TestConfig& cfg)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_config = cfg;
		++m_persistRevision;
	}
	markDirty(Dirty_Config);
}

void TesterModel::setAvailablePorts(std::vector<PortEntry> ports)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_ports = std::move(ports);
	}
	markDirty(Dirty_Ports);
}

void TesterModel::appendLog(LogLevel level, std::string text)
{
	using namespace std::chrono;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		LogEntry e;
		e.seq = ++m_logSeq;
		e.timeMs = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
		e.level = level;
		e.text = std::move(text);
		m_logs.push_back(std::move(e));
		while (m_logs.size() > kMaxLogEntries) m_logs.pop_front();
	}
	markDirty(Dirty_Log);
}

void TesterModel::clearLog()
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_logs.clear();
	}
	markDirty(Dirty_LogReset);
}

PersistentConfig TesterModel::persistentConfig(uint64_t* revision) const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	PersistentConfig cfg;
	cfg.tests = m_config;
	for (const auto& s : m_stations)
		cfg.stations.push_back({ s.no, s.port });
	if (revision) *revision = m_persistRevision;
	return cfg;
}

void TesterModel::restorePersistentConfig(const PersistentConfig& cfg)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_config = cfg.tests;
		m_stations.clear();
		for (const auto& st : cfg.stations) {
			if (m_stations.size() >= kMaxStations) break;
			if (st.no < 1 || st.no > kMaxStations) continue;
			bool dup = std::any_of(m_stations.begin(), m_stations.end(),
				[&](const StationInfo& s) { return s.no == st.no; });
			if (dup) continue;
			StationInfo info;
			info.no = st.no;
			info.port = st.port;
			m_stations.push_back(std::move(info));
		}
		std::sort(m_stations.begin(), m_stations.end(),
			[](const StationInfo& a, const StationInfo& b) { return a.no < b.no; });
		++m_persistRevision;
	}
	markDirty(Dirty_Config | Dirty_Stations);
}

uint64_t TesterModel::pushAlert(AlertEvent ev)
{
	uint64_t id;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		id = ++m_alertSeq;
		ev.id = id;
		m_alerts.push_back(std::move(ev));
	}
	markDirty(Dirty_Alert);
	return id;
}

} // namespace Lx::Tester
