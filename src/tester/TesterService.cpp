#include "TesterService.h"
#include <algorithm>
#include "serial/serial.h"
#include "TesterConfigStore.h"

namespace Lx::Tester {

static std::string stationTag(int no)
{
	return "工位#" + std::to_string(no);
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

void TesterService::post(Task t)
{
	{
		std::lock_guard<std::mutex> lk(m_queueMutex);
		if (!m_running) return;
		m_tasks.push(std::move(t));
	}
	m_queueCv.notify_one();
}

void TesterService::threadLoop()
{
	loadConfig();

	using clock = std::chrono::steady_clock;
	auto nextSave = clock::now() + kSaveInterval;

	for (;;) {
		Task t;
		{
			std::unique_lock<std::mutex> lk(m_queueMutex);
			m_queueCv.wait_until(lk, nextSave, [this] { return !m_tasks.empty() || !m_running; });
			// 退出时先把队列里剩余的命令（例如最后一次参数修改）执行完
			if (!m_running && m_tasks.empty()) break;
			if (m_tasks.empty()) {
				// 超时：周期性保存
				lk.unlock();
				saveConfigIfChanged();
				nextSave = clock::now() + kSaveInterval;
				continue;
			}
			t = std::move(m_tasks.front());
			m_tasks.pop();
		}
		try {
			t();
		}
		catch (const std::exception& e) {
			log(LogLevel::Error, std::string("业务线程异常：") + e.what());
			notify(AlertLevel::Error, "业务异常", e.what());
		}
		if (clock::now() >= nextSave) {
			saveConfigIfChanged();
			nextSave = clock::now() + kSaveInterval;
		}
	}

	// 退出前保存最后一次修改
	saveConfigIfChanged();
}

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
		log(LogLevel::Info, stationTag(no) + " 已添加");
		// TODO: 新增未就绪工位后，全局状态应回到「准备中」
	});
}

void TesterService::removeStation(int no)
{
	post([this, no] {
		// TODO: 若端口已连接，先断开再移除
		if (m_model->removeStation(no))
			log(LogLevel::Info, stationTag(no) + " 已移除");
	});
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
		// TODO: 打开端口 -> DUT 状态 Connecting -> 握手成功 Ready / 失败 Error，
		//       全部工位 Ready 后全局状态切换为「准备就绪」
		log(LogLevel::Warn, stationTag(no) + " 连接端口：功能未实现");
	});
}

void TesterService::disconnectStation(int no)
{
	post([this, no] {
		// TODO: 关闭端口 -> DUT 状态 Disconnected，全局状态回到「准备中」
		log(LogLevel::Warn, stationTag(no) + " 断开端口：功能未实现");
	});
}

void TesterService::refreshPorts()
{
	post([this] {
		std::vector<PortEntry> ports;
		for (const auto& p : serial::list_ports())
			ports.push_back({ p.port, p.description });
		// TODO: 枚举 WinUSB 设备
		std::sort(ports.begin(), ports.end(),
			[](const PortEntry& a, const PortEntry& b) { return a.name < b.name; });
		m_model->setAvailablePorts(std::move(ports));
	});
}

// ---------------------------------------------------------------- 全局流程

void TesterService::startTest()
{
	post([this] {
		// TODO: 校验全局状态=准备就绪 -> 测试进行中；RF 全局互斥调度，功耗并行
		log(LogLevel::Warn, "开始测试：功能未实现");
	});
}

void TesterService::nextRound()
{
	post([this] {
		// TODO: 校验全局状态=测试结束 -> 清空本轮结果、复位工位状态 -> 准备中
		log(LogLevel::Warn, "下一轮：功能未实现");
	});
}

void TesterService::emergencyStop()
{
	post([this] {
		// TODO: 通知所有测试板终止业务、释放 RF 锁、全局回到准备中
		log(LogLevel::Error, "紧急停止：功能未实现");
	});
}

// ---------------------------------------------------------------- 参数 / 日志

void TesterService::setConfig(const TestConfig& cfg)
{
	post([this, cfg] {
		TestConfig c = cfg;
		ConfigStore::sanitize(c);
		m_model->setConfig(c);
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
			log(LogLevel::Error, "工位#3 测试返回ABORT：通信超时（模拟）");
			notify(AlertLevel::Error, "工位#3 通信异常", "测试板通信超时，测试项已 ABORT。\n（业务线程模拟事件）");
			return;
		}
		confirm(AlertLevel::Warning, "DUT 断开",
			"工位#2 DUT 连接断开，是否重新连接？\n（业务线程模拟事件）",
			[this](bool accepted) {
				log(LogLevel::Info, std::string("用户选择：") + (accepted ? "重新连接" : "取消"));
			});
	});
}

void TesterService::debugFillDemoData()
{
	post([this] {
		while (m_model->stations().size() < 4 && m_model->addStation() != 0) {}

		auto ports = m_model->availablePorts();
		int i = 0;
		m_model->updateAllStations([&](StationInfo& s) {
			s.port = i < (int)ports.size() ? ports[i].name : "COM" + std::to_string(10 + i);
			auto& it = s.items;
			switch (i % 4) {
			case 0:
				s.dut = DutState::Ready; s.state = StationState::Finished; s.lastVerdict = Verdict::Pass;
				it[0] = { ItemState::Pass, 2, { 97.2, 98.5 } };
				it[1] = { ItemState::Pass, 1, { -12.5 } };
				it[2] = { ItemState::Pass, 1, { 85.0 } };
				break;
			case 1:
				s.dut = DutState::Ready; s.state = StationState::Testing; s.lastVerdict = Verdict::None;
				it[0] = { ItemState::Pass, 2, { 99.0, 96.4 } };
				it[1] = { ItemState::Running };
				it[2] = { ItemState::Pending };
				break;
			case 2:
				s.dut = DutState::Error; s.state = StationState::Finished; s.lastVerdict = Verdict::Abort;
				it[0] = { ItemState::Abort };
				it[1] = { ItemState::Pending };
				it[2] = { ItemState::Pending };
				break;
			default:
				s.dut = DutState::Connecting; s.state = StationState::Finished; s.lastVerdict = Verdict::Fail;
				it[0] = { ItemState::Fail, 2, { 96.1, 91.3 } };
				it[1] = { ItemState::Fail, 1, { 63.8 } };
				it[2] = { ItemState::Running };
				break;
			}
			++i;
		});
		m_model->setGlobalState(GlobalState::Testing);
		log(LogLevel::Info, "已填充演示数据");
	});
}

void TesterService::debugCycleGlobalState()
{
	post([this] {
		auto s = static_cast<GlobalState>((static_cast<int>(m_model->globalState()) + 1) % 4);
		m_model->setGlobalState(s);
	});
}

} // namespace Lx::Tester
