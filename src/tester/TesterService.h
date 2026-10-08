#pragma once
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include "TesterModel.h"

namespace Lx::Tester {

// ViewModel 看到的命令接口。所有调用立即返回，实际工作在业务线程执行，
// 结果只通过 TesterModel（dirty flag）回到 UI。
class ITesterService {
public:
	virtual ~ITesterService() = default;

	virtual void addStation() = 0;
	virtual void removeStation(int no) = 0;
	virtual void selectPort(int no, const std::string& port) = 0;
	virtual void connectStation(int no) = 0;
	virtual void disconnectStation(int no) = 0;
	virtual void refreshPorts() = 0;

	virtual void startTest() = 0;
	virtual void nextRound() = 0;
	virtual void emergencyStop() = 0;

	virtual void setConfig(const TestConfig& cfg) = 0;
	virtual void clearLog() = 0;

	// UI 关闭弹窗时回传结果（Confirm 类型才有意义）
	virtual void replyAlert(uint64_t alertId, bool accepted) = 0;

	// 调试：在业务线程上制造数据/事件，验证 UI
	virtual void debugRaiseAlert(AlertKind kind) = 0;
	virtual void debugFillDemoData() = 0;
	virtual void debugCycleGlobalState() = 0;
};

// 业务线程。当前只实现 UI 所需的数据管理（增删工位、选端口、参数、日志）和配置持久化，
// 连接/测试/调度等业务逻辑均为 TODO 桩。
// 持久化：启动时从 configPath 加载用户配置；之后每 kSaveInterval 检查一次，配置有变化才写盘；退出时再保存一次。
class TesterService : public ITesterService {
public:
	static constexpr std::chrono::seconds kSaveInterval{ 5 };

	TesterService(std::shared_ptr<TesterModel> model, std::filesystem::path configPath = "config.json");
	~TesterService() override;

	void stop();

	void addStation() override;
	void removeStation(int no) override;
	void selectPort(int no, const std::string& port) override;
	void connectStation(int no) override;
	void disconnectStation(int no) override;
	void refreshPorts() override;

	void startTest() override;
	void nextRound() override;
	void emergencyStop() override;

	void setConfig(const TestConfig& cfg) override;
	void clearLog() override;

	void replyAlert(uint64_t alertId, bool accepted) override;

	void debugRaiseAlert(AlertKind kind) override;
	void debugFillDemoData() override;
	void debugCycleGlobalState() override;

private:
	using Task = std::function<void()>;
	using AlertReply = std::function<void(bool accepted)>;

	void post(Task t);
	void threadLoop();
	void loadConfig();
	void saveConfigIfChanged();

	// 以下仅在业务线程内调用
	void log(LogLevel level, std::string text);
	void notify(AlertLevel level, std::string title, std::string message);
	void confirm(AlertLevel level, std::string title, std::string message, AlertReply onReply);

	std::shared_ptr<TesterModel> m_model;
	std::filesystem::path        m_configPath;
	uint64_t                     m_savedRevision = 0;   // 仅业务线程访问
	uint64_t                     m_failedRevision = 0;

	std::thread             m_thread;
	std::mutex              m_queueMutex;
	std::condition_variable m_queueCv;
	std::queue<Task>        m_tasks;
	bool                    m_running = true;

	// 等待 UI 回复的 Confirm 弹窗（仅业务线程访问）
	std::unordered_map<uint64_t, AlertReply> m_pendingReplies;
};

} // namespace Lx::Tester
