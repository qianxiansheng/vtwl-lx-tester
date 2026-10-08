#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "TesterModel.h"
#include "StationController.h"
#include "ResultCsv.h"
#include "link/EventLoop.h"

namespace Lx::Tester {

// ViewModel 看到的命令接口。所有调用立即返回，实际工作在业务线程执行，
// 结果只通过 TesterModel（dirty flag）回到 UI。
class ITesterService {
public:
	virtual ~ITesterService() = default;

	virtual void addStation() = 0;
	virtual void removeStation(int no) = 0;
	virtual void setPortType(int no, PortType type) = 0;
	virtual void selectPort(int no, const std::string& port) = 0;
	virtual void connectStation(int no) = 0;
	virtual void disconnectStation(int no) = 0;
	virtual void refreshPorts() = 0;

	virtual void startTest() = 0;
	virtual void nextRound() = 0;
	virtual void emergencyStop() = 0;

	virtual void setConfig(const TestConfig& cfg) = 0;
	virtual void clearLog() = 0;
	virtual void resetStats() = 0;

	// UI 关闭弹窗时回传结果（Confirm 类型才有意义）
	virtual void replyAlert(uint64_t alertId, bool accepted) = 0;

	// 调试：在业务线程上制造弹窗事件，验证 UI
	virtual void debugRaiseAlert(AlertKind kind) = 0;
};

// 业务线程：唯一调度器。
// - 事件循环：任务队列 + 定时器，链路层/工位控制器的所有回调都在这里执行，不阻塞等待
// - 每个工位一个 StationController；本类负责全局状态机、RF 互斥锁排队、结果 CSV 与统计
// - 持久化：启动时加载 configPath；每 kSaveInterval 检查一次，有修改才写盘；退出时再保存一次
class TesterService : public ITesterService, public Link::IEventLoop, private IStationHost {
public:
	static constexpr std::chrono::seconds kSaveInterval{ 5 };

	TesterService(std::shared_ptr<TesterModel> model, std::filesystem::path configPath = "config.json");
	~TesterService() override;

	void stop();

	// ---- ITesterService
	void addStation() override;
	void removeStation(int no) override;
	void setPortType(int no, PortType type) override;
	void selectPort(int no, const std::string& port) override;
	void connectStation(int no) override;
	void disconnectStation(int no) override;
	void refreshPorts() override;

	void startTest() override;
	void nextRound() override;
	void emergencyStop() override;

	void setConfig(const TestConfig& cfg) override;
	void clearLog() override;
	void resetStats() override;

	void replyAlert(uint64_t alertId, bool accepted) override;
	void debugRaiseAlert(AlertKind kind) override;

	// ---- IEventLoop
	void post(Task t) override;
	TimerId after(uint32_t ms, Task t) override;
	void cancel(TimerId id) override;

private:
	using Clock = std::chrono::steady_clock;
	using AlertReply = std::function<void(bool accepted)>;

	// ---- IStationHost（业务线程）
	Link::IEventLoop& loop() override { return *this; }
	TesterModel& model() override { return *m_model; }
	TestConfig config() override { return m_model->config(); }
	void log(LogLevel level, std::string text) override;
	void notify(AlertLevel level, std::string title, std::string message) override;
	void acquireRf(int no, std::function<void()> granted) override;
	void releaseRf(int no) override;
	void onStationChanged(int no) override;
	void onRoundFinished(const RoundRecord& rec) override;

	void threadLoop();
	void runDueTimers();
	void loadConfig();
	void saveConfigIfChanged();
	void confirm(AlertLevel level, std::string title, std::string message, AlertReply onReply);

	void createController(int no);
	StationController* controller(int no);
	void recomputeGlobal();
	void setGlobal(GlobalState s);

	std::shared_ptr<TesterModel> m_model;
	std::filesystem::path        m_configPath;
	uint64_t                     m_savedRevision = 0;   // 以下成员仅业务线程访问
	uint64_t                     m_failedRevision = 0;

	// 事件循环
	std::thread             m_thread;
	std::mutex              m_queueMutex;
	std::condition_variable m_queueCv;
	std::deque<Task>        m_tasks;                   // 受 m_queueMutex 保护
	bool                    m_running = true;          // 受 m_queueMutex 保护
	std::multimap<Clock::time_point, TimerId> m_timerQueue;
	std::unordered_map<TimerId, Task>         m_timers;   // 已取消的定时器不在此表中
	TimerId                 m_timerSeq = 0;

	// 调度
	std::map<int, std::shared_ptr<StationController>> m_controllers;
	int m_rfHolder = 0;                                          // 0 = 空闲
	std::deque<std::pair<int, std::function<void()>>> m_rfQueue;
	ResultCsv m_csv;

	// 等待 UI 回复的 Confirm 弹窗
	std::unordered_map<uint64_t, AlertReply> m_pendingReplies;
};

} // namespace Lx::Tester
