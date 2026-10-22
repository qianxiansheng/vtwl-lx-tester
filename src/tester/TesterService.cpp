#include "TesterService.h"
#include <algorithm>
#include "serial/serial.h"
#include "TesterConfigStore.h"
#include "link/SimBoard.h"
#include "link/Transport.h"

namespace Lx::Tester {

static std::string stationTag(int no)
{
	return "工位#" + std::to_string(no);
}

static const char* globalText(GlobalState s)
{
	switch (s) {
	case GlobalState::Preparing: return "准备中";
	case GlobalState::Ready:     return "准备就绪";
	case GlobalState::Testing:   return "测试进行中";
	case GlobalState::Finished:  return "测试结束";
	}
	return "?";
}

TesterService::TesterService(std::shared_ptr<TesterModel> model, std::filesystem::path configPath)
	: m_model(std::move(model)), m_configPath(std::move(configPath))
{
	m_thread = std::thread(&TesterService::threadLoop, this);
}

TesterService::~TesterService()
{
	stop();
}

void TesterService::stop()
{
	{
		std::lock_guard<std::mutex> lk(m_queueMutex);
		if (!m_running) return;
		m_running = false;
	}
	m_queueCv.notify_all();
	if (m_thread.joinable()) m_thread.join();
}

// ---------------------------------------------------------------- 事件循环

void TesterService::post(Task t)
{
	{
		std::lock_guard<std::mutex> lk(m_queueMutex);
		if (!m_running) return;
		m_tasks.push_back(std::move(t));
	}
	m_queueCv.notify_one();
}

Link::IEventLoop::TimerId TesterService::after(uint32_t ms, Task t)
{
	const TimerId id = ++m_timerSeq;
	m_timers.emplace(id, std::move(t));
	m_timerQueue.emplace(Clock::now() + std::chrono::milliseconds(ms), id);
	return id;
}

void TesterService::cancel(TimerId id)
{
	if (id) m_timers.erase(id);   // 队列里的条目到期时被跳过
}

void TesterService::runDueTimers()
{
	const auto now = Clock::now();
	while (!m_timerQueue.empty() && m_timerQueue.begin()->first <= now) {
		const TimerId id = m_timerQueue.begin()->second;
		m_timerQueue.erase(m_timerQueue.begin());
		auto it = m_timers.find(id);
		if (it == m_timers.end()) continue;
		Task fn = std::move(it->second);
		m_timers.erase(it);
		fn();
	}
}

void TesterService::threadLoop()
{
	loadConfig();

	auto nextSave = Clock::now() + kSaveInterval;
	auto runSafely = [this](const Task& t) {
		try {
			t();
		}
		catch (const std::exception& e) {
			log(LogLevel::Error, std::string("业务线程异常：") + e.what());
			notify(AlertLevel::Error, "业务异常", e.what());
		}
	};

	for (;;) {
		runSafely([this] { runDueTimers(); });

		// 丢掉已取消的队首定时器，算出下一次唤醒时间
		while (!m_timerQueue.empty() && !m_timers.count(m_timerQueue.begin()->second))
			m_timerQueue.erase(m_timerQueue.begin());
		auto wake = nextSave;
		if (!m_timerQueue.empty()) wake = std::min(wake, m_timerQueue.begin()->first);

		std::deque<Task> batch;
		{
			std::unique_lock<std::mutex> lk(m_queueMutex);
			m_queueCv.wait_until(lk, wake, [this] { return !m_tasks.empty() || !m_running; });
			// 退出时先把队列里剩余的命令（例如最后一次参数修改）执行完
			if (!m_running && m_tasks.empty()) break;
			batch.swap(m_tasks);
		}
		for (auto& t : batch) runSafely(t);

		if (Clock::now() >= nextSave) {
			saveConfigIfChanged();
			nextSave = Clock::now() + kSaveInterval;
		}
	}

	// 关闭所有链路（会等待各 IO 线程退出），再保存最后一次修改
	m_rfQueue.clear();
	m_rfHolder = 0;
	m_controllers.clear();
	m_timers.clear();
	m_timerQueue.clear();
	saveConfigIfChanged();
}

// ---------------------------------------------------------------- 持久化

void TesterService::loadConfig()
{
	PersistentConfig cfg;
	std::string err;
	const std::u8string u8path = m_configPath.u8string();
	const std::string path(u8path.begin(), u8path.end());

	switch (ConfigStore::load(m_configPath, cfg, err)) {
	case ConfigStore::LoadResult::Ok:
		m_model->restorePersistentConfig(cfg);
		log(LogLevel::Info, "已加载配置：" + path);
		break;
	case ConfigStore::LoadResult::NotFound:
		log(LogLevel::Info, "未找到配置文件，使用默认参数：" + path);
		break;
	case ConfigStore::LoadResult::Invalid:
		log(LogLevel::Error, "配置文件解析失败，已使用默认参数（原文件备份为 .bad）：" + err);
		notify(AlertLevel::Error, "配置加载失败",
			"配置文件 " + path + " 解析失败，已使用默认参数。\n原文件已备份为 " + path + ".bad\n\n" + err);
		break;
	}
	// 加载结果视为“已保存”状态，不需要立即回写
	m_model->persistentConfig(&m_savedRevision);

	for (const auto& s : m_model->stations())
		createController(s.no);
	m_model->setResultFile(m_csv.currentFile());
}

void TesterService::saveConfigIfChanged()
{
	uint64_t rev = 0;
	PersistentConfig cfg = m_model->persistentConfig(&rev);
	if (rev == m_savedRevision) return;

	std::string err;
	if (ConfigStore::save(m_configPath, cfg, err)) {
		m_savedRevision = rev;
	}
	else {
		// 不更新 m_savedRevision，下个周期重试；同一版本只记一次日志避免刷屏
		if (m_failedRevision != rev) {
			m_failedRevision = rev;
			log(LogLevel::Error, "保存配置失败：" + err);
		}
	}
}

// ---------------------------------------------------------------- 日志 / 弹窗

void TesterService::log(LogLevel level, std::string text)
{
	m_model->appendLog(level, std::move(text));
}

void TesterService::notify(AlertLevel level, std::string title, std::string message)
{
	AlertEvent ev;
	ev.level = level;
	ev.kind = AlertKind::Notify;
	ev.title = std::move(title);
	ev.message = std::move(message);
	m_model->pushAlert(std::move(ev));
}

void TesterService::confirm(AlertLevel level, std::string title, std::string message, AlertReply onReply)
{
	AlertEvent ev;
	ev.level = level;
	ev.kind = AlertKind::Confirm;
	ev.title = std::move(title);
	ev.message = std::move(message);
	uint64_t id = m_model->pushAlert(std::move(ev));
	m_pendingReplies[id] = std::move(onReply);
}

// ---------------------------------------------------------------- 工位管理

void TesterService::createController(int no)
{
	m_controllers[no] = std::make_shared<StationController>(static_cast<IStationHost&>(*this), no);
}

StationController* TesterService::controller(int no)
{
	auto it = m_controllers.find(no);
	return it == m_controllers.end() ? nullptr : it->second.get();
}

void TesterService::addStation()
{
	post([this] {
		if (m_model->globalState() == GlobalState::Testing) {
			notify(AlertLevel::Warning, "无法添加工位", "测试进行中，不能添加工位。");
			return;
		}
		int no = m_model->addStation();
		if (no == 0) {
			notify(AlertLevel::Warning, "无法添加工位", "工位数量已达上限（6 个）。");
			return;
		}
		createController(no);
		log(LogLevel::Info, stationTag(no) + " 已添加");
		recomputeGlobal();
	});
}

void TesterService::removeStation(int no)
{
	post([this, no] {
		if (auto* c = controller(no); c && c->isTesting()) return;
		m_controllers.erase(no);   // 析构时关闭链路
		if (m_model->removeStation(no))
			log(LogLevel::Info, stationTag(no) + " 已移除");
		recomputeGlobal();
	});
}

void TesterService::setPortType(int no, PortType type)
{
	post([this, no, type] { m_model->setStationPortType(no, type); });
}

void TesterService::selectPort(int no, const std::string& port)
{
	post([this, no, port] {
		auto stations = m_model->stations();
		auto other = std::find_if(stations.begin(), stations.end(),
			[&](const StationInfo& s) { return s.no != no && !port.empty() && s.port == port; });
		if (other != stations.end()) {
			notify(AlertLevel::Warning, "端口冲突",
				port + " 已被" + stationTag(other->no) + " 使用，请选择其他端口。");
			return;
		}
		m_model->setStationPort(no, port);
	});
}

void TesterService::connectStation(int no)
{
	post([this, no] {
		if (auto* c = controller(no)) c->connect();
	});
}

void TesterService::disconnectStation(int no)
{
	post([this, no] {
		if (auto* c = controller(no)) c->disconnect();
	});
}

void TesterService::refreshPorts()
{
	post([this] {
		std::vector<PortEntry> ports;
		try {
			for (const auto& p : serial::list_ports())
				ports.push_back({ PortType::Uart, p.port, p.port, p.description, true });
		}
		catch (const std::exception& e) {
			log(LogLevel::Warn, std::string("枚举串口失败：") + e.what());
		}
		std::sort(ports.begin(), ports.end(),
			[](const PortEntry& a, const PortEntry& b) { return a.name < b.name; });

		const CommConfig comm = m_model->config().comm;
		for (const auto& d : Link::enumerateUsbDevices(static_cast<uint16_t>(comm.usbVid), static_cast<uint16_t>(comm.usbPid)))
			ports.push_back({ PortType::Usb, d.instanceId, d.label, d.description, d.winusb });

		// 模拟测试板在两种端口类型下都可选
		if (Link::SimSettings::get().portsEnabled) {
			for (PortType t : { PortType::Uart, PortType::Usb })
				for (int i = 1; i <= kMaxStations; ++i) {
					const std::string name = std::string(Link::kSimPortPrefix) + std::to_string(i);
					ports.push_back({ t, name, name, "模拟测试板", true });
				}
		}
		m_model->setAvailablePorts(std::move(ports));
	});
}

// ---------------------------------------------------------------- 全局流程

void TesterService::setGlobal(GlobalState s)
{
	if (m_model->globalState() == s) return;
	m_model->setGlobalState(s);
	log(LogLevel::Info, std::string("全局状态切换为：") + globalText(s));
}

void TesterService::recomputeGlobal()
{
	const GlobalState g = m_model->globalState();
	if (g == GlobalState::Testing) {
		const bool anyTesting = std::any_of(m_controllers.begin(), m_controllers.end(),
			[](const auto& kv) { return kv.second->isTesting(); });
		if (!anyTesting) setGlobal(GlobalState::Finished);
		return;
	}
	if (g == GlobalState::Finished) return;   // 等待“下一轮”

	const bool ready = !m_controllers.empty() && std::all_of(m_controllers.begin(), m_controllers.end(),
		[](const auto& kv) { return kv.second->isBoardConnected() && kv.second->isDutReady() && !kv.second->isTesting(); });
	setGlobal(ready ? GlobalState::Ready : GlobalState::Preparing);
}

void TesterService::onStationChanged(int)
{
	recomputeGlobal();
}

void TesterService::startTest()
{
	post([this] {
		recomputeGlobal();
		if (m_model->globalState() != GlobalState::Ready) {
			notify(AlertLevel::Warning, "无法开始测试", "全局状态不是「准备就绪」：请确认所有工位的测试板和 DUT 均已连接就绪。");
			return;
		}
		setGlobal(GlobalState::Testing);
		m_rfQueue.clear();
		m_rfHolder = 0;
		for (auto& [no, c] : m_controllers) c->startRound();   // 按工位编号顺序排队 RF 资源
	});
}

void TesterService::nextRound()
{
	post([this] {
		if (m_model->globalState() != GlobalState::Finished) return;
		setGlobal(GlobalState::Preparing);
		for (auto& [no, c] : m_controllers) c->prepareNextRound();
		recomputeGlobal();
	});
}

void TesterService::emergencyStop()
{
	post([this] {
		log(LogLevel::Error, "紧急停止：通知所有测试板终止业务，释放 RF 资源");
		m_rfQueue.clear();
		m_rfHolder = 0;
		// 先切到准备中，避免各工位结束时被判定为“测试结束”
		setGlobal(GlobalState::Preparing);
		for (auto& [no, c] : m_controllers) c->emergencyStop();
		m_rfQueue.clear();
		m_rfHolder = 0;
		recomputeGlobal();
	});
}

// ---------------------------------------------------------------- RF 互斥资源

void TesterService::acquireRf(int no, std::function<void()> granted)
{
	if (m_rfHolder == 0 || m_rfHolder == no) {
		m_rfHolder = no;
		post(std::move(granted));   // 异步回调，避免在调用方栈内重入
		return;
	}
	m_rfQueue.emplace_back(no, std::move(granted));
	log(LogLevel::Info, stationTag(no) + " 等待 RF 资源（当前占用：" + stationTag(m_rfHolder) + "）");
}

void TesterService::releaseRf(int no)
{
	m_rfQueue.erase(std::remove_if(m_rfQueue.begin(), m_rfQueue.end(),
		[no](const auto& e) { return e.first == no; }), m_rfQueue.end());
	if (m_rfHolder != no) return;
	m_rfHolder = 0;
	if (!m_rfQueue.empty()) {
		auto next = std::move(m_rfQueue.front());
		m_rfQueue.pop_front();
		m_rfHolder = next.first;
		post(std::move(next.second));
	}
}

// ---------------------------------------------------------------- 结果

void TesterService::onRoundFinished(const RoundRecord& rec)
{
	m_model->recordVerdict(rec.no, rec.verdict);
	std::string err;
	if (!m_csv.append(rec, err)) {
		log(LogLevel::Error, "写入结果文件失败：" + err);
		notify(AlertLevel::Error, "结果保存失败", "写入结果文件失败：" + err + "\n" + m_csv.currentFile());
	}
	m_model->setResultFile(m_csv.currentFile());
}

void TesterService::resetStats()
{
	post([this] {
		m_model->resetStats();
		log(LogLevel::Info, "统计已清零");
	});
}

// ---------------------------------------------------------------- 参数 / 日志

void TesterService::setConfig(const TestConfig& cfg)
{
	post([this, cfg] {
		TestConfig c = cfg;
		ConfigStore::sanitize(c);
		const CommConfig old = m_model->config().comm;
		m_model->setConfig(c);
		if (old.usbVid != c.comm.usbVid || old.usbPid != c.comm.usbPid)
			refreshPorts();   // VID/PID 变了，重新枚举 USB 设备
	});
}

void TesterService::clearLog()
{
	post([this] { m_model->clearLog(); });
}

// ---------------------------------------------------------------- 弹窗回复

void TesterService::replyAlert(uint64_t alertId, bool accepted)
{
	post([this, alertId, accepted] {
		auto it = m_pendingReplies.find(alertId);
		if (it == m_pendingReplies.end()) return;   // Notify 类型没有回调
		auto cb = std::move(it->second);
		m_pendingReplies.erase(it);
		if (cb) cb(accepted);
	});
}

// ---------------------------------------------------------------- 调试

void TesterService::debugRaiseAlert(AlertKind kind)
{
	post([this, kind] {
		if (kind == AlertKind::Notify) {
			notify(AlertLevel::Error, "模拟通知", "这是一条由业务线程发出的通知弹窗。");
			return;
		}
		confirm(AlertLevel::Warning, "模拟确认", "这是一条由业务线程发出的确认弹窗，结果会回传业务线程。",
			[this](bool accepted) {
				log(LogLevel::Info, std::string("用户选择：") + (accepted ? "确定" : "取消"));
			});
	});
}

} // namespace Lx::Tester
