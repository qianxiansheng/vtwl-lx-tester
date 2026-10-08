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
			vm.portType = s.portType;
			vm.port = s.port;
			vm.board = s.board;
			vm.dut = s.dut;
			vm.boardNote = s.boardNote;
			vm.dutNote = s.dutNote;
			vm.state = s.state;
			vm.verdict = s.lastVerdict;
			for (int i = 0; i < kTestItemCount; ++i)
				vm.items[i] = toItemVM(static_cast<TestItem>(i), s.items[i]);
			m_stations.push_back(std::move(vm));
		}
	}

	if (flags & TesterModel::Dirty_Ports)
		m_ports = m_model->availablePorts();

	if (flags & TesterModel::Dirty_Stats) {
		const StatsSnapshot snap = m_model->stats();
		m_resultFile = snap.resultFile;
		m_statsRows.clear();
		StatsRowVM total;
		total.label = "合计";
		auto yieldOf = [](const StatsRowVM& r) -> std::string {
			if (r.pass + r.fail == 0) return "--";
			char buf[16];
			snprintf(buf, sizeof(buf), "%.1f%%", r.pass * 100.0 / (r.pass + r.fail));
			return buf;
		};
		for (const auto& s : snap.stations) {
			StatsRowVM r;
			r.label = "工位#" + std::to_string(s.no);
			r.tested = s.tested; r.pass = s.pass; r.fail = s.fail; r.abort = s.abort;
			r.yieldText = yieldOf(r);
			total.tested += r.tested; total.pass += r.pass; total.fail += r.fail; total.abort += r.abort;
			m_statsRows.push_back(std::move(r));
		}
		total.yieldText = yieldOf(total);
		m_statsRows.push_back(std::move(total));
	}

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

// 端口未打开（未连接 / 打开失败 / 已断开）
static bool linkClosed(const StationVM& s)
{
	return s.board == BoardState::Disconnected || s.board == BoardState::Error;
}

bool TesterViewModel::canEditStation(const StationVM& s) const
{
	return m_global != GlobalState::Testing && linkClosed(s);
}

bool TesterViewModel::canConnect(const StationVM& s) const
{
	return m_global != GlobalState::Testing && linkClosed(s) && !s.port.empty();
}

bool TesterViewModel::canDisconnect(const StationVM& s) const
{
	return m_global != GlobalState::Testing && !linkClosed(s);
}

// ---------------------------------------------------------------- 命令

void TesterViewModel::addStation()                 { if (canAddStation()) m_service->addStation(); }
void TesterViewModel::removeStation(int no)        { m_service->removeStation(no); }
void TesterViewModel::selectPort(int no, const std::string& port) { m_service->selectPort(no, port); }
void TesterViewModel::setPortType(int no, PortType type) { m_service->setPortType(no, type); }
void TesterViewModel::resetStats()                 { m_service->resetStats(); }
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

	// 悬停提示：说明 + result_code + 执行次数
	std::string detail = r.note;
	if (r.resultCode > 0) {
		char buf[32];
		snprintf(buf, sizeof(buf), "result_code=0x%02X", r.resultCode);
		detail += detail.empty() ? buf : std::string("\n") + buf;
	}
	if (r.attempts > 1)
		detail += (detail.empty() ? "" : "\n") + std::string("执行次数：") + std::to_string(r.attempts);
	vm.detailText = detail;

	if (r.valueCount <= 0 || r.state == ItemState::Abort || r.state == ItemState::Running) {
		if (r.state == ItemState::Running && !r.note.empty()) vm.valueText = r.note;
		return vm;
	}

	char brief[48] = "";
	const auto& v = r.values;
	switch (item) {
	case TestItem::RfPacket:   snprintf(brief, sizeof(brief), "%.1f%% / %.1f%%", v[0], v[1]); break;
	case TestItem::FreqOffset: snprintf(brief, sizeof(brief), "%+.2f KHz", v[0]); break;
	case TestItem::Power:      snprintf(brief, sizeof(brief), "%.0f uA", v[0]); break;
	}
	vm.valueText = brief;
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
