#pragma once
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "TesterModel.h"
#include "TesterService.h"

namespace Lx::Tester {

struct ItemVM {
	ItemState   state = ItemState::Pending;
	std::string valueText;              // 卡片上显示的简短测量值，ABORT/无值时为空
	std::string detailText;             // 悬停提示的完整描述
};

struct StationVM {
	int          no = 0;
	PortType     portType = PortType::Uart;
	std::string  port;
	BoardState   board = BoardState::Disconnected;
	DutState     dut = DutState::Disconnected;
	std::string  boardNote;
	std::string  dutNote;
	StationState state = StationState::Preparing;
	Verdict      verdict = Verdict::None;
	std::array<ItemVM, kTestItemCount> items{};
};

struct StatsRowVM {
	std::string label;                  // "工位#1" / "合计"
	int tested = 0, pass = 0, fail = 0, abort = 0;
	std::string yieldText;              // 良率 = PASS / (PASS + FAIL)，ABORT 不计入
};

struct LogLineVM {
	uint64_t    seq = 0;
	LogLevel    level = LogLevel::Info;
	std::string text;                   // 含时间戳的整行文本
};

// ViewModel：只在 UI 线程使用，无锁。
// - sync() 每帧调用一次：消费 Model 的 dirty flags，仅刷新变化部分的快照。
// - View 只读这里的快照和 canXxx()，用户操作调用命令方法，命令转发给 ITesterService。
class TesterViewModel {
public:
	TesterViewModel(std::shared_ptr<TesterModel> model, std::shared_ptr<ITesterService> service);

	void sync();

	// ---- 快照（View 只读）
	GlobalState                   globalState() const { return m_global; }
	const std::vector<StationVM>& stations() const { return m_stations; }
	const std::vector<PortEntry>& ports() const { return m_ports; }
	const std::vector<LogLineVM>& logs() const { return m_logs; }
	const std::vector<StatsRowVM>& statsRows() const { return m_statsRows; }   // 最后一行为合计
	const std::string&            resultFile() const { return m_resultFile; }

	// 日志选择（按行，seq 区间）。选择属于界面状态，放在 VM 而不是 View 的静态变量里
	void        selectLog(uint64_t seq, bool extend);   // extend: shift 点击/拖拽，扩展到 seq
	void        selectAllLogs();
	void        clearLogSelection() { m_selAnchor = m_selEnd = 0; }
	bool        isLogSelected(uint64_t seq) const;
	bool        hasLogSelection() const { return m_selAnchor != 0; }
	std::string selectedLogText() const;
	std::string allLogText() const;

	// 参数编辑缓冲区，View 直接绑定到输入控件，修改后调用 commitConfig()
	TestConfig&                   configEdit() { return m_config; }

	// 弹窗队列：一次只显示队首
	const AlertEvent*             currentAlert() const { return m_alerts.empty() ? nullptr : &m_alerts.front(); }
	size_t                        pendingAlertCount() const { return m_alerts.size(); }

	// ---- 派生的可用状态
	bool canAddStation() const;
	bool canEditStation(const StationVM& s) const;      // 选端口 / 移除
	bool canConnect(const StationVM& s) const;
	bool canDisconnect(const StationVM& s) const;
	bool canStart() const { return m_global == GlobalState::Ready; }
	bool canNextRound() const { return m_global == GlobalState::Finished; }
	bool canEmergencyStop() const { return true; }      // 紧急停止永远可用

	// ---- 命令
	void addStation();
	void removeStation(int no);
	void setPortType(int no, PortType type);
	void selectPort(int no, const std::string& port);
	void resetStats();
	void connect(int no);
	void disconnect(int no);
	void refreshPorts();
	void start();
	void nextRound();
	void emergencyStop();
	void clearLog();
	void commitConfig();
	void closeAlert(bool accepted);

	ITesterService& service() { return *m_service; }    // 调试菜单用

private:
	static ItemVM toItemVM(TestItem item, const ItemResult& r);
	static LogLineVM toLogLine(const LogEntry& e);

	std::shared_ptr<TesterModel>    m_model;
	std::shared_ptr<ITesterService> m_service;

	GlobalState            m_global = GlobalState::Preparing;
	std::vector<StationVM> m_stations;
	std::vector<PortEntry> m_ports;
	TestConfig             m_config;
	std::vector<LogLineVM> m_logs;
	std::vector<StatsRowVM> m_statsRows;
	std::string            m_resultFile;
	uint64_t               m_lastLogSeq = 0;
	uint64_t               m_selAnchor = 0;     // 0 = 无选择
	uint64_t               m_selEnd = 0;
	std::deque<AlertEvent> m_alerts;
};

} // namespace Lx::Tester
