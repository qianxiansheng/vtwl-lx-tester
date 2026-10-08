#include "Transport.h"
#include "SimBoard.h"
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include "protocol/BoardProtocol.h"

namespace Lx::Link {

using namespace Lx::Proto;

// ---------------------------------------------------------------- SimSettings

namespace {
std::mutex  g_simMutex;
SimSettings g_sim;
}

SimSettings SimSettings::get()
{
	std::lock_guard<std::mutex> lk(g_simMutex);
	return g_sim;
}

void SimSettings::set(const SimSettings& s)
{
	std::lock_guard<std::mutex> lk(g_simMutex);
	g_sim = s;
}

bool isSimPort(const std::string& port)
{
	return port.rfind(kSimPortPrefix, 0) == 0;
}

namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

// 一块模拟测试板 + 一个模拟 DUT。所有状态只在模拟板线程中访问。
class SimTransport : public ITransport {
public:
	explicit SimTransport(std::string port) : m_name(std::move(port))
	{
		try { m_index = std::stoi(m_name.substr(3)); } catch (...) { m_index = 0; }
		m_rng.seed(static_cast<uint32_t>(Clock::now().time_since_epoch().count()) + m_index);
	}
	~SimTransport() override { close(); }

	bool open(DataHandler onData, ClosedHandler onClosed, std::string& err) override
	{
		close();
		if (m_index < 1) {
			err = "无效的模拟端口名";
			return false;
		}
		m_onData = std::move(onData);
		(void)onClosed;   // 模拟板不会物理断开；断线用“不再应答”模拟
		m_running = true;
		m_thread = std::thread(&SimTransport::loop, this);
		return true;
	}

	void close() override
	{
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			if (!m_running) return;
			m_running = false;
		}
		m_cv.notify_all();
		if (m_thread.joinable()) m_thread.join();
		m_events.clear();
	}

	bool send(const std::vector<uint8_t>& data) override
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (!m_running) return false;
		m_rx.insert(m_rx.end(), data.begin(), data.end());
		m_cv.notify_all();
		return true;
	}

	const std::string& name() const override { return m_name; }

private:
	enum class Task { None, TxRx, Cfo, Power };

	// ---- 模拟板线程

	void loop()
	{
		std::unique_lock<std::mutex> lk(m_mutex);
		while (m_running) {
			auto wake = m_events.empty() ? Clock::now() + Ms(100) : m_events.begin()->first;
			m_cv.wait_until(lk, wake, [this] { return !m_running || !m_rx.empty(); });
			if (!m_running) break;

			std::vector<uint8_t> rx;
			rx.swap(m_rx);
			lk.unlock();

			if (!rx.empty())
				m_parser.feed(rx.data(), rx.size(), [this](const Frame& f) { onRequest(f); });

			// 到期事件
			while (!m_events.empty() && m_events.begin()->first <= Clock::now()) {
				auto fn = std::move(m_events.begin()->second);
				m_events.erase(m_events.begin());
				fn();
			}
			lk.lock();
		}
	}

	void after(int ms, std::function<void()> fn)
	{
		m_events.emplace(Clock::now() + Ms(ms), std::move(fn));
	}

	void reply(const Frame& req, std::vector<uint8_t> payload, int delayMs = 2)
	{
		Frame ack;
		ack.tid = req.tid;
		ack.opcode = static_cast<uint16_t>(req.opcode | kAckFlag);
		ack.payload = std::move(payload);
		after(delayMs, [this, ack] {
			if (SimSettings::get().appliesTo(m_index) && SimSettings::get().noResponse) return;
			if (m_onData) m_onData(encodeFrame(ack));
		});
	}

	void onRequest(const Frame& f)
	{
		const SimSettings s = SimSettings::get();
		const bool mine = s.appliesTo(m_index);
		if (mine && s.noResponse) return;

		switch (static_cast<OpCode>(f.opcode)) {
		case OpCode::TestboardAlive:
			reply(f, encode(RcOnly{}));
			break;
		case OpCode::GetVersion: {
			VersionResult v;
			v.fw = "SIM-FW 1.0." + std::to_string(m_index);
			v.hw = "SIM-HW A";
			reply(f, encode(v));
			break;
		}
		case OpCode::ConnCheckDut:
			// 测试板以 10ms 间隔 PING，最长 300ms
			if (mine && s.dutAbsent) {
				m_dutConnected = false;
				reply(f, encode(RcOnly{ RET_ERR_DUT_NO_RESPONSE }), 300);
			}
			else {
				m_dutConnected = true;
				reply(f, encode(RcOnly{}), 20 + static_cast<int>(m_rng() % 30));
			}
			break;
		case OpCode::RfTestTxRx:  startTask(f, Task::TxRx, 1500); break;
		case OpCode::RfTestCfo:   startTask(f, Task::Cfo, 800); break;
		case OpCode::PowerTest:   startTask(f, Task::Power, 1500); break;
		case OpCode::GetTestStatus:
			reply(f, encode(StatusResult{ RET_OK, m_status }));
			break;
		case OpCode::RfTestTxRxGetResult: reply(f, encode(m_txrx)); break;
		case OpCode::RfTestCfoGetResult:  reply(f, encode(m_cfo)); break;
		case OpCode::PowerTestGetResult:  reply(f, encode(m_power)); break;
		case OpCode::ResetDut:
			m_dutConnected = false;   // 复位后需要重新握手
			reply(f, encode(RcOnly{}), 50);
			break;
		case OpCode::StopCurrTask:
		case OpCode::AbortAllTask:
			if (m_task != Task::None) finishTask(RET_ERR_ABORTED);
			reply(f, encode(RcOnly{}));
			break;
		case OpCode::TestboardReset:
			reply(f, encode(RcOnly{}));
			m_task = Task::None;
			m_status = BoardTaskStatus::Idle;
			m_dutConnected = false;
			break;
		default:
			reply(f, encode(RcOnly{ RET_ERR_NOT_SUPPORT }));
			break;
		}
	}

	void startTask(const Frame& f, Task t, int durationMs)
	{
		if (m_task != Task::None) {
			reply(f, encode(RcOnly{ RET_ERR_RESOURCE_BUSY }));
			return;
		}
		m_task = t;
		m_status = BoardTaskStatus::Running;
		const uint32_t gen = ++m_taskGen;
		reply(f, encode(RcOnly{}));

		const SimSettings s = SimSettings::get();
		if (s.appliesTo(m_index) && s.hangTask) return;   // 永不结束
		after(durationMs, [this, gen] {
			if (gen != m_taskGen || m_task == Task::None) return;
			const SimSettings cur = SimSettings::get();
			uint8_t rc = RET_OK;
			if (!m_dutConnected) rc = RET_ERR_DUT_NO_RESPONSE;
			else if (cur.appliesTo(m_index) && cur.businessRc != RET_OK) rc = cur.businessRc;
			finishTask(rc);
		});
	}

	void finishTask(uint8_t rc)
	{
		const SimSettings s = SimSettings::get();
		const bool mine = s.appliesTo(m_index);
		auto measure = mine ? s.measure : SimSettings::Measure::AllPass;
		if (measure == SimSettings::Measure::Random) {
			const uint32_t r = m_rng() % 10;
			measure = r == 0 ? SimSettings::Measure::RfFail
				: r == 1 ? SimSettings::Measure::PowerFail
				: SimSettings::Measure::AllPass;
		}
		auto rnd = [this](int lo, int hi) { return lo + static_cast<int>(m_rng() % static_cast<uint32_t>(hi - lo + 1)); };

		switch (m_task) {
		case Task::TxRx:
			m_txrx = RfTxRxResult{ rc };
			if (rc == RET_OK) {
				const bool fail = measure == SimSettings::Measure::RfFail;
				m_txrx.total = 100;
				m_txrx.boardRx = static_cast<uint16_t>(fail ? rnd(70, 85) : rnd(97, 100));
				m_txrx.dutRx = static_cast<uint16_t>(rnd(96, 100));
				m_txrx.rssi = static_cast<int8_t>(rnd(-55, -40));
			}
			break;
		case Task::Cfo:
			m_cfo = CfoResult{ rc };
			if (rc == RET_OK)
				m_cfo.raw = static_cast<int16_t>(measure == SimSettings::Measure::CfoFail ? 32767 : rnd(-8000, 8000));
			break;
		case Task::Power:
			m_power = PowerResult{ rc };
			if (rc == RET_OK) {
				const int avg = measure == SimSettings::Measure::PowerFail ? rnd(150, 200) : rnd(60, 90);
				m_power.avgUA = static_cast<uint16_t>(avg);
				m_power.maxUA = static_cast<uint16_t>(avg + rnd(5, 30));
				m_power.minUA = static_cast<uint16_t>(avg - rnd(5, 30));
			}
			m_dutConnected = false;   // 功耗测试后 DUT 处于低功耗，串口静默
			break;
		case Task::None:
			break;
		}
		m_task = Task::None;
		m_status = rc == RET_OK || rc == RET_ERR_ABORTED ? BoardTaskStatus::Idle : BoardTaskStatus::Error;
	}

	std::string   m_name;
	int           m_index = 0;
	DataHandler   m_onData;

	std::thread             m_thread;
	std::mutex              m_mutex;
	std::condition_variable m_cv;
	bool                    m_running = false;
	std::vector<uint8_t>    m_rx;          // 受 m_mutex 保护

	// 以下只在模拟板线程访问
	std::multimap<Clock::time_point, std::function<void()>> m_events;
	FrameParser     m_parser;
	std::mt19937    m_rng;
	Task            m_task = Task::None;
	uint32_t        m_taskGen = 0;
	BoardTaskStatus m_status = BoardTaskStatus::Idle;
	bool            m_dutConnected = false;
	RfTxRxResult    m_txrx{ RET_ERR_NOT_SUPPORT };
	CfoResult       m_cfo{ RET_ERR_NOT_SUPPORT };
	PowerResult     m_power{ RET_ERR_NOT_SUPPORT };
};

} // namespace

std::unique_ptr<ITransport> makeSimTransport(const std::string& port)
{
	return std::make_unique<SimTransport>(port);
}

} // namespace Lx::Link
