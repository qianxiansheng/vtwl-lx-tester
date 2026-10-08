#include "TesterViewModel.h"
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace Lx::Tester {

TesterViewModel::TesterViewModel(std::shared_ptr<TesterModel> model, std::shared_ptr<ITesterService> service)
	: m_model(std::move(model)), m_service(std::move(service))
{
}

void TesterViewModel::sync()
{
	const uint32_t flags = m_model->consumeDirtyFlags();
	if (flags == TesterModel::Dirty_None) return;

	if (flags & TesterModel::Dirty_Global)
		m_global = m_model->globalState();

	if (flags & TesterModel::Dirty_Stations) {
		auto src = m_model->stations();
		m_stations.clear();
		m_stations.reserve(src.size());
		for (const auto& s : src) {
			StationVM vm;
			vm.no = s.no;
			vm.port = s.port;
			vm.dut = s.dut;
			vm.state = s.state;
			vm.verdict = s.lastVerdict;
			for (int i = 0; i < kTestItemCount; ++i)
				vm.items[i] = toItemVM(static_cast<TestItem>(i), s.items[i]);
			m_stations.push_back(std::move(vm));
		}
	}

	if (flags & TesterModel::Dirty_Ports)
		m_ports = m_model->availablePorts();

	if (flags & TesterModel::Dirty_Config)
		m_config = m_model->config();

	// 先处理清空，再拉增量，保证“清空后紧接着的新日志”不会丢
	if (flags & TesterModel::Dirty_LogReset) {
		m_logs.clear();
		clearLogSelection();
	}

	if (flags & (TesterModel::Dirty_Log | TesterModel::Dirty_LogReset)) {
		auto entries = m_model->logsSince(m_lastLogSeq);
		for (const auto& e : entries) {
			m_logs.push_back(toLogLine(e));
			m_lastLogSeq = e.seq;
		}
		if (m_logs.size() > TesterModel::kMaxLogEntries)
			m_logs.erase(m_logs.begin(), m_logs.begin() + (m_logs.size() - TesterModel::kMaxLogEntries));
	}

	if (flags & TesterModel::Dirty_Alert) {
		for (auto& a : m_model->takeAlerts())
			m_alerts.push_back(std::move(a));
	}
}

// ---------------------------------------------------------------- 日志选择 / 复制

void TesterViewModel::selectLog(uint64_t seq, bool extend)
{
	if (!extend || m_selAnchor == 0) m_selAnchor = seq;
	m_selEnd = seq;
}

void TesterViewModel::selectAllLogs()
{
	if (m_logs.empty()) return;
	m_selAnchor = m_logs.front().seq;
	m_selEnd = m_logs.back().seq;
}

bool TesterViewModel::isLogSelected(uint64_t seq) const
{
	if (m_selAnchor == 0) return false;
	const uint64_t lo = std::min(m_selAnchor, m_selEnd), hi = std::max(m_selAnchor, m_selEnd);
	return seq >= lo && seq <= hi;
}

std::string TesterViewModel::selectedLogText() const
{
	std::string out;
	for (const auto& l : m_logs) {
		if (!isLogSelected(l.seq)) continue;
		out += l.text;
		out += "\r\n";
	}
	return out;
}

std::string TesterViewModel::allLogText() const
{
	std::string out;
	for (const auto& l : m_logs) {
		out += l.text;
		out += "\r\n";
	}
	return out;
}

// ---------------------------------------------------------------- 可用状态

bool TesterViewModel::canAddStation() const
{
	return m_stations.size() < kMaxStations && m_global != GlobalState::Testing;
}

bool TesterViewModel::canEditStation(const StationVM& s) const
{
	return m_global != GlobalState::Testing && s.dut == DutState::Disconnected;
}

bool TesterViewModel::canConnect(const StationVM& s) const
{
	return m_global != GlobalState::Testing && !s.port.empty()
		&& (s.dut == DutState::Disconnected || s.dut == DutState::Error);
}

bool TesterViewModel::canDisconnect(const StationVM& s) const
{
	return m_global != GlobalState::Testing && s.dut != DutState::Disconnected;
}

// ---------------------------------------------------------------- 命令

void TesterViewModel::addStation()                 { if (canAddStation()) m_service->addStation(); }
void TesterViewModel::removeStation(int no)        { m_service->removeStation(no); }
void TesterViewModel::selectPort(int no, const std::string& port) { m_service->selectPort(no, port); }
void TesterViewModel::connect(int no)              { m_service->connectStation(no); }
void TesterViewModel::disconnect(int no)           { m_service->disconnectStation(no); }
void TesterViewModel::refreshPorts()               { m_service->refreshPorts(); }
void TesterViewModel::start()                      { if (canStart()) m_service->startTest(); }
void TesterViewModel::nextRound()                  { if (canNextRound()) m_service->nextRound(); }
void TesterViewModel::emergencyStop()              { m_service->emergencyStop(); }
void TesterViewModel::clearLog()                   { m_service->clearLog(); }
void TesterViewModel::commitConfig()               { m_service->setConfig(m_config); }

void TesterViewModel::closeAlert(bool accepted)
{
	if (m_alerts.empty()) return;
	const AlertEvent a = m_alerts.front();
	m_alerts.pop_front();
	if (a.kind == AlertKind::Confirm)
		m_service->replyAlert(a.id, accepted);
}

// ---------------------------------------------------------------- 格式化

ItemVM TesterViewModel::toItemVM(TestItem item, const ItemResult& r)
{
	ItemVM vm;
	vm.state = r.state;
	if (r.valueCount <= 0 || r.state == ItemState::Abort) {
		if (r.state == ItemState::Abort) vm.detailText = "执行异常，无有效测量值";
		return vm;
	}

	char brief[48] = "", detail[96] = "";
	const auto& v = r.values;
	switch (item) {
	case TestItem::RfPacket:
		if (r.valueCount >= 2) {
			snprintf(brief, sizeof(brief), "%.1f%% / %.1f%%", v[0], v[1]);
			snprintf(detail, sizeof(detail), "收包率 %.1f%%，发包率 %.1f%%", v[0], v[1]);
		}
		else {
			snprintf(brief, sizeof(brief), "%.1f%%", v[0]);
			snprintf(detail, sizeof(detail), "收包率 %.1f%%", v[0]);
		}
		break;
	case TestItem::FreqOffset:
		snprintf(brief, sizeof(brief), "%+.1f KHz", v[0]);
		snprintf(detail, sizeof(detail), "频偏 %+.1f KHz", v[0]);
		break;
	case TestItem::Power:
		snprintf(brief, sizeof(brief), "%.1f uA", v[0]);
		snprintf(detail, sizeof(detail), "功耗 %.1f uA", v[0]);
		break;
	}
	vm.valueText = brief;
	vm.detailText = detail;
	return vm;
}

LogLineVM TesterViewModel::toLogLine(const LogEntry& e)
{
	std::time_t t = static_cast<std::time_t>(e.timeMs / 1000);
	std::tm tm{};
	localtime_s(&tm, &t);
	char ts[32];
	std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

	LogLineVM line;
	line.seq = e.seq;
	line.level = e.level;
	line.text = std::string("[") + ts + "] " + e.text;
	return line;
}

} // namespace Lx::Tester
