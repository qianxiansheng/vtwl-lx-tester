#include "StationController.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace Lx::Tester {

using namespace Lx::Proto;
using Link::BoardLink;

namespace {

const char* const kItemNames[kTestItemCount] = { "无线收发包", "频偏", "功耗" };

constexpr OpCode kStartOp[kTestItemCount]  = { OpCode::RfTestTxRx, OpCode::RfTestCfo, OpCode::PowerTest };
constexpr OpCode kResultOp[kTestItemCount] = { OpCode::RfTestTxRxGetResult, OpCode::RfTestCfoGetResult, OpCode::PowerTestGetResult };

constexpr uint32_t kDutRetryIntervalMs = 1000;

long long steadyMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

long long systemMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const ItemCommonConfig& commonOf(const TestConfig& cfg, int idx)
{
	switch (idx) {
	case 0:  return cfg.rfPacket.common;
	case 1:  return cfg.freqOffset.common;
	default: return cfg.power.common;
	}
}

std::string rcText(int rc)
{
	char buf[96];
	snprintf(buf, sizeof(buf), "result_code=0x%02X（%s）", rc, resultCodeText(static_cast<uint8_t>(rc)));
	return buf;
}

template<typename... Args>
std::string fmt(const char* f, Args... args)
{
	char buf[256];
	snprintf(buf, sizeof(buf), f, args...);
	return buf;
}

} // namespace

StationController::StationController(IStationHost& host, int no)
	: m_host(host), m_no(no)
{
}

StationController::~StationController()
{
	m_host.loop().cancel(m_dutRetryTimer);
	m_host.loop().cancel(m_testTimer);
	if (m_holdsRf) m_host.releaseRf(m_no);
	if (m_link) m_link->close();
}

// ---------------------------------------------------------------- 回调保护

std::function<void()> StationController::guardTest(std::function<void()> fn)
{
	std::weak_ptr<StationController> weak = weak_from_this();
	const uint32_t gen = m_testGen;
	return [weak, gen, fn = std::move(fn)] {
		auto self = weak.lock();
		if (self && self->m_testGen == gen) fn();
	};
}

StationController::Reply StationController::guardTest(Reply fn)
{
	std::weak_ptr<StationController> weak = weak_from_this();
	const uint32_t gen = m_testGen;
	return [weak, gen, fn = std::move(fn)](BoardLink::Status st, const Frame& f) {
		auto self = weak.lock();
		if (self && self->m_testGen == gen) fn(st, f);
	};
}

StationController::Reply StationController::guardConn(Reply fn)
{
	std::weak_ptr<StationController> weak = weak_from_this();
	const uint32_t gen = m_connGen;
	return [weak, gen, fn = std::move(fn)](BoardLink::Status st, const Frame& f) {
		auto self = weak.lock();
		if (self && self->m_connGen == gen) fn(st, f);
	};
}

void StationController::request(OpCode op, uint32_t timeoutMs, Reply onReply)
{
	if (!m_link || !m_link->isOpen()) return;
	m_link->request(op, timeoutMs, std::move(onReply));
}

// ---------------------------------------------------------------- 连接

void StationController::connect()
{
	if (m_link) return;

	StationInfo info;
	{
		auto list = m_host.model().stations();
		auto it = std::find_if(list.begin(), list.end(), [this](const StationInfo& s) { return s.no == m_no; });
		if (it == list.end()) return;
		info = *it;
	}
	const TestConfig cfg = m_host.config();
	const std::string tag = "工位#" + std::to_string(m_no);

	if (info.portType == PortType::Usb) {
		m_host.notify(AlertLevel::Warning, "USB 端口暂未实现",
			fmt("USB 端口（VID=%04X, PID=%04X）尚未实现，请使用 UART 端口。", kUsbVid, kUsbPid));
		return;
	}
	if (info.port.empty()) {
		m_host.notify(AlertLevel::Warning, "未选择端口", tag + " 请先选择端口。");
		return;
	}

	auto transport = Link::isSimPort(info.port)
		? Link::makeSimTransport(info.port)
		: Link::makeSerialTransport(info.port, static_cast<uint32_t>(cfg.comm.baudRate));

	BoardLink::Timings t;
	t.timeoutMs = static_cast<uint32_t>(cfg.comm.cmdTimeoutMs);
	m_link = BoardLink::create(m_host.loop(), std::move(transport), t);
	m_port = info.port;
	++m_connGen;

	std::weak_ptr<StationController> weak = weak_from_this();
	const uint32_t gen = m_connGen;
	m_link->setOnLost([weak, gen](const std::string& reason) {
		auto self = weak.lock();
		if (self && self->m_connGen == gen) self->onLinkLost(reason);
	});
	m_link->setOnHeartbeat([weak, gen] {
		auto self = weak.lock();
		if (self && self->m_connGen == gen && self->m_board == BoardState::Connecting) self->onBoardAlive();
	});

	setDut(DutState::Disconnected);
	setBoard(BoardState::Connecting, "等待测试板心跳应答");

	std::string err;
	if (!m_link->open(err)) {
		m_link.reset();
		setBoard(BoardState::Error, "打开端口失败：" + err);
		m_host.log(LogLevel::Error, tag + " 打开端口 " + info.port + " 失败：" + err);
		m_host.notify(AlertLevel::Error, "打开端口失败", tag + " 打开端口 " + info.port + " 失败：\n" + err);
		return;
	}
	m_host.log(LogLevel::Info, tag + " 已打开端口 " + info.port + "，等待测试板应答");
}

void StationController::disconnect()
{
	if (m_testing) return;
	++m_connGen;
	m_host.loop().cancel(m_dutRetryTimer);
	if (m_link) {
		m_link->close();
		m_link.reset();
		m_host.log(LogLevel::Info, "工位#" + std::to_string(m_no) + " 已断开端口");
	}
	m_dutChecking = false;
	setDut(DutState::Disconnected);
	setBoard(BoardState::Disconnected);
}

void StationController::onBoardAlive()
{
	const std::string tag = "工位#" + std::to_string(m_no);
	setBoard(BoardState::Connected);
	m_host.log(LogLevel::Info, tag + " 测试板已连接");

	request(OpCode::GetVersion, 0, guardConn([this, tag](BoardLink::Status st, const Frame& f) {
		VersionResult v;
		if (st == BoardLink::Status::Ok && parse(f.payload, v) && v.rc == RET_OK) {
			m_record.boardFw = v.fw;
			m_record.boardHw = v.hw;
			setBoard(BoardState::Connected, "固件 " + v.fw + " / 硬件 " + v.hw);
			m_host.log(LogLevel::Info, tag + " 测试板版本：固件 " + v.fw + "，硬件 " + v.hw);
		}
		else {
			m_host.log(LogLevel::Warn, tag + " 读取测试板版本失败");
		}
		m_dutFailLogged = false;
		startDutCheck();
	}));
}

void StationController::onLinkLost(const std::string& reason)
{
	const std::string tag = "工位#" + std::to_string(m_no);
	// 在 BoardLink 的回调里，不能立即销毁链路，延后释放
	auto dead = std::move(m_link);
	m_host.loop().post([dead] {});

	++m_connGen;
	m_dutChecking = false;
	m_host.loop().cancel(m_dutRetryTimer);
	setDut(DutState::Disconnected);
	setBoard(BoardState::Error, reason);
	m_host.log(LogLevel::Error, tag + " 测试板断开：" + reason);
	m_host.notify(AlertLevel::Error, tag + " 测试板断开", tag + "（" + m_port + "）测试板断开：" + reason);

	if (m_testing) {
		++m_testGen;
		m_host.loop().cancel(m_testTimer);
		abortRemaining(0, "测试板断开");
		finishRound();
	}
}

void StationController::startDutCheck(std::function<void(bool ok)> done)
{
	if (!m_link || !m_link->isOpen() || m_board != BoardState::Connected) {
		if (done) done(false);
		return;
	}
	if (m_dutChecking && !done) return;   // 自动握手已在进行

	const std::string tag = "工位#" + std::to_string(m_no);
	m_host.loop().cancel(m_dutRetryTimer);
	m_dutChecking = true;
	setDut(DutState::Connecting, "握手中");

	const uint32_t timeout = static_cast<uint32_t>(m_host.config().comm.connCheckTimeoutMs);
	request(OpCode::ConnCheckDut, timeout, guardConn([this, tag, done](BoardLink::Status st, const Frame& f) {
		m_dutChecking = false;
		RcOnly r;
		const bool ok = st == BoardLink::Status::Ok && parse(f.payload, r) && r.rc == RET_OK;
		if (ok) {
			setDut(DutState::Ready);
			m_dutFailLogged = false;
			m_host.log(LogLevel::Info, tag + " DUT连接就绪");
		}
		else {
			const std::string reason = st == BoardLink::Status::Timeout ? std::string("握手应答超时") : rcText(r.rc);
			setDut(DutState::Error, reason);
			if (!m_dutFailLogged) {   // 自动重试时只记一次，避免刷屏
				m_dutFailLogged = true;
				m_host.log(LogLevel::Warn, tag + " DUT握手失败：" + reason + (done ? "" : "，将自动重试"));
			}
		}
		if (done) done(ok);
		else if (!ok) scheduleDutRetry();
	}));
}

void StationController::scheduleDutRetry()
{
	if (!m_link || m_testing || m_finished) return;
	std::weak_ptr<StationController> weak = weak_from_this();
	const uint32_t gen = m_connGen;
	m_host.loop().cancel(m_dutRetryTimer);
	m_dutRetryTimer = m_host.loop().after(kDutRetryIntervalMs, [weak, gen] {
		auto self = weak.lock();
		if (self && self->m_connGen == gen && !self->m_testing && self->m_dut != DutState::Ready)
			self->startDutCheck();
	});
}

void StationController::setBoard(BoardState s, std::string note)
{
	m_board = s;
	m_host.model().updateStation(m_no, [&](StationInfo& st) {
		st.board = s;
		st.boardNote = std::move(note);
	});
	refreshStationState();
	m_host.onStationChanged(m_no);
}

void StationController::setDut(DutState s, std::string note)
{
	m_dut = s;
	m_host.model().updateStation(m_no, [&](StationInfo& st) {
		st.dut = s;
		st.dutNote = std::move(note);
	});
	refreshStationState();
	m_host.onStationChanged(m_no);
}

void StationController::refreshStationState()
{
	StationState s = StationState::Preparing;
	if (m_testing) s = StationState::Testing;
	else if (m_finished) s = StationState::Finished;
	else if (m_board == BoardState::Connected && m_dut == DutState::Ready) s = StationState::Ready;
	m_host.model().updateStation(m_no, [s](StationInfo& st) { st.state = s; });
}

// ---------------------------------------------------------------- 测试流程

void StationController::setItem(int idx, const ItemResult& r)
{
	m_record.items[idx] = r;
	m_host.model().updateStation(m_no, [&](StationInfo& st) { st.items[idx] = r; });
}

void StationController::startRound()
{
	if (m_testing || !m_link) return;

	++m_testGen;
	m_testing = true;
	m_finished = false;
	m_holdsRf = false;
	m_host.loop().cancel(m_dutRetryTimer);

	const std::string fw = m_record.boardFw, hw = m_record.boardHw;
	m_record = RoundRecord{};
	m_record.no = m_no;
	m_record.port = m_port;
	m_record.boardFw = fw;
	m_record.boardHw = hw;
	for (int i = 0; i < kTestItemCount; ++i) setItem(i, ItemResult{});
	m_host.model().updateStation(m_no, [](StationInfo& st) { st.lastVerdict = Verdict::None; });
	refreshStationState();

	ItemResult waiting;
	waiting.state = ItemState::Running;
	waiting.note = "等待 RF 资源";
	setItem(0, waiting);

	m_host.acquireRf(m_no, guardTest([this] {
		m_holdsRf = true;
		m_host.log(LogLevel::Info, "工位#" + std::to_string(m_no) + " 获得 RF 资源");
		runItem(0);
	}));
}

void StationController::runItem(int idx)
{
	m_attempt = 0;
	if (idx == static_cast<int>(TestItem::Power) && m_holdsRf) {
		m_holdsRf = false;
		m_host.releaseRf(m_no);
		m_host.log(LogLevel::Info, "工位#" + std::to_string(m_no) + " 释放 RF 资源");
	}
	executeItem(idx);
}

void StationController::executeItem(int idx)
{
	const std::string tag = "工位#" + std::to_string(m_no);
	ItemResult r;
	r.state = ItemState::Running;
	r.attempts = m_attempt + 1;
	r.note = m_attempt == 0 ? "执行中" : fmt("执行中（第 %d 次重试）", m_attempt);
	setItem(idx, r);
	m_host.log(LogLevel::Info, tag + " 开始执行" + kItemNames[idx] + "测试" + (m_attempt ? fmt("（第 %d 次重试）", m_attempt) : ""));

	request(kStartOp[idx], 0, guardTest([this, idx](BoardLink::Status st, const Frame& f) {
		if (st == BoardLink::Status::Timeout) {
			request(OpCode::StopCurrTask, 0, nullptr);   // 不确定是否已启动，保险起见停止
			itemAbort(idx, "启动指令应答超时");
			return;
		}
		RcOnly ack;
		if (!parse(f.payload, ack)) { itemAbort(idx, "启动应答格式错误"); return; }
		if (ack.rc != RET_OK) { itemAbort(idx, "启动失败：" + rcText(ack.rc), ack.rc); return; }

		const TestConfig cfg = m_host.config();
		m_deadlineMs = steadyMs() + commonOf(cfg, idx).timeoutMs;
		m_testTimer = m_host.loop().after(static_cast<uint32_t>(cfg.comm.pollIntervalMs), guardTest([this, idx] { pollItem(idx); }));
	}));
}

void StationController::pollItem(int idx)
{
	if (steadyMs() >= m_deadlineMs) {
		request(OpCode::StopCurrTask, 0, nullptr);
		itemAbort(idx, fmt("业务总超时（%d ms）", commonOf(m_host.config(), idx).timeoutMs));
		return;
	}
	const uint32_t poll = static_cast<uint32_t>(m_host.config().comm.pollIntervalMs);
	auto again = [this, idx, poll] {
		m_testTimer = m_host.loop().after(poll, guardTest([this, idx] { pollItem(idx); }));
	};

	request(OpCode::GetTestStatus, 0, guardTest([this, idx, again](BoardLink::Status st, const Frame& f) {
		if (st == BoardLink::Status::Timeout) { again(); return; }   // 连续超时由链路层判定断开
		StatusResult s;
		if (!parse(f.payload, s)) { itemAbort(idx, "状态应答格式错误"); return; }
		if (s.rc != RET_OK) { itemAbort(idx, "查询状态失败：" + rcText(s.rc), s.rc); return; }
		if (s.status == BoardTaskStatus::Running) { again(); return; }
		fetchResult(idx, s.status == BoardTaskStatus::Error);
	}));
}

void StationController::fetchResult(int idx, bool boardReportedError)
{
	request(kResultOp[idx], 0, guardTest([this, idx, boardReportedError](BoardLink::Status st, const Frame& f) {
		if (st == BoardLink::Status::Timeout) {
			const uint32_t poll = static_cast<uint32_t>(m_host.config().comm.pollIntervalMs);
			m_testTimer = m_host.loop().after(poll, guardTest([this, idx] { pollItem(idx); }));
			return;
		}

		const TestConfig cfg = m_host.config();
		ItemResult r;
		r.attempts = m_attempt + 1;
		r.resultCode = RET_OK;
		uint8_t rc = RET_OK;
		bool parsed = false, pass = false;
		std::string detail;

		switch (static_cast<TestItem>(idx)) {
		case TestItem::RfPacket: {
			RfTxRxResult v;
			parsed = parse(f.payload, v);
			rc = v.rc;
			if (!parsed || rc != RET_OK) break;
			m_record.txrx = v;
			m_record.hasTxRx = true;
			const double total = v.total ? v.total : 1.0;
			const double rxRate = v.total ? v.dutRx * 100.0 / total : 0.0;     // DUT 收包率
			const double txRate = v.total ? v.boardRx * 100.0 / total : 0.0;   // DUT 发包率
			r.valueCount = 3;
			r.values = { rxRate, txRate, static_cast<double>(v.rssi) };
			pass = rxRate >= cfg.rfPacket.minRxRatePct && txRate >= cfg.rfPacket.minTxRatePct
				&& v.rssi >= cfg.rfPacket.minRssiDbm;
			detail = fmt("收包率 %.1f%%，发包率 %.1f%%，RSSI %d dBm（总发包 %u，测试板收 %u，被测板收 %u）",
				rxRate, txRate, v.rssi, v.total, v.boardRx, v.dutRx);
			break;
		}
		case TestItem::FreqOffset: {
			CfoResult v;
			parsed = parse(f.payload, v);
			rc = v.rc;
			if (!parsed || rc != RET_OK) break;
			m_record.cfo = v;
			m_record.hasCfo = true;
			const double khz = v.raw * cfg.freqOffset.cfoHzPerLsb / 1000.0;
			r.valueCount = 1;
			r.values = { khz, 0, 0 };
			pass = std::fabs(khz) <= cfg.freqOffset.maxAbsOffsetKHz;
			detail = fmt("频偏 %+.2f KHz（原始值 %d）", khz, v.raw);
			break;
		}
		case TestItem::Power: {
			PowerResult v;
			parsed = parse(f.payload, v);
			rc = v.rc;
			if (!parsed || rc != RET_OK) break;
			m_record.power = v;
			m_record.hasPower = true;
			r.valueCount = 3;
			r.values = { static_cast<double>(v.avgUA), static_cast<double>(v.maxUA), static_cast<double>(v.minUA) };
			pass = v.avgUA <= cfg.power.maxPowerUA;
			detail = fmt("平均电流 %u uA（最大 %u，最小 %u）", v.avgUA, v.maxUA, v.minUA);
			break;
		}
		}

		if (!parsed) { itemAbort(idx, "结果应答格式错误"); return; }
		if (rc != RET_OK) { itemAbort(idx, rcText(rc), rc); return; }
		if (boardReportedError) { itemAbort(idx, "测试板报告工位状态异常", RET_OK); return; }
		itemPassFail(idx, pass, r, detail);
	}));
}

void StationController::itemPassFail(int idx, bool pass, const ItemResult& base, const std::string& detail)
{
	ItemResult r = base;
	r.state = pass ? ItemState::Pass : ItemState::Fail;
	r.note = detail;
	setItem(idx, r);
	m_host.log(pass ? LogLevel::Info : LogLevel::Warn,
		"工位#" + std::to_string(m_no) + " " + kItemNames[idx] + "测试：" + (pass ? "PASS，" : "FAIL，") + detail);
	nextItem(idx);
}

void StationController::itemAbort(int idx, const std::string& reason, int rc)
{
	m_host.loop().cancel(m_testTimer);
	const std::string tag = "工位#" + std::to_string(m_no);
	if (rc == RET_ERR_DUT_NO_RESPONSE) setDut(DutState::Error, reason);

	const int retries = commonOf(m_host.config(), idx).retryCount;
	if (m_attempt < retries && m_link) {
		++m_attempt;
		m_host.log(LogLevel::Warn, tag + " " + kItemNames[idx] + "测试 ABORT（" + reason + "），"
			+ fmt("第 %d/%d 次重试：复位 DUT 并重新握手", m_attempt, retries));
		ItemResult r;
		r.state = ItemState::Running;
		r.attempts = m_attempt;
		r.resultCode = rc;
		r.note = "重试准备：复位 DUT（上次：" + reason + "）";
		setItem(idx, r);

		request(OpCode::ResetDut, 0, guardTest([this, idx, reason, rc](BoardLink::Status st, const Frame& f) {
			RcOnly rr;
			if (st != BoardLink::Status::Ok || !parse(f.payload, rr) || rr.rc != RET_OK) {
				m_attempt = commonOf(m_host.config(), idx).retryCount;   // 不再重试
				itemAbort(idx, reason + "；重试前复位 DUT 失败", rc);
				return;
			}
			const uint32_t gen = m_testGen;
			startDutCheck([this, idx, reason, rc, gen](bool ok) {
				if (gen != m_testGen) return;
				if (ok) {
					executeItem(idx);
				}
				else {
					m_attempt = commonOf(m_host.config(), idx).retryCount;
					itemAbort(idx, reason + "；重试前 DUT 握手失败", rc);
				}
			});
		}));
		return;
	}

	ItemResult r;
	r.state = ItemState::Abort;
	r.attempts = m_attempt + 1;
	r.resultCode = rc;
	r.note = reason;
	setItem(idx, r);
	m_host.log(LogLevel::Error, tag + " " + kItemNames[idx] + "测试：ABORT，" + reason);

	if (!m_link) {
		abortRemaining(idx + 1, "测试板断开");
		finishTest();
	}
	else if (m_dut != DutState::Ready) {
		abortRemaining(idx + 1, "DUT 未就绪");
		finishTest();
	}
	else {
		nextItem(idx);
	}
}

void StationController::abortRemaining(int fromIdx, const std::string& reason)
{
	for (int i = fromIdx; i < kTestItemCount; ++i) {
		const ItemState s = m_record.items[i].state;
		if (s == ItemState::Pass || s == ItemState::Fail || s == ItemState::Abort) continue;
		ItemResult r;
		r.state = ItemState::Abort;
		r.attempts = m_record.items[i].attempts;
		r.note = reason;
		setItem(i, r);
	}
}

void StationController::nextItem(int idx)
{
	if (idx + 1 < kTestItemCount) runItem(idx + 1);
	else finishTest();
}

void StationController::finishTest()
{
	if (m_holdsRf) {
		m_holdsRf = false;
		m_host.releaseRf(m_no);
	}
	if (!m_link) {
		finishRound();
		return;
	}
	// 功耗测试后 DUT 处于低功耗：复位 + 重新握手后才算进入空闲
	m_host.log(LogLevel::Info, "工位#" + std::to_string(m_no) + " 测试项完成，复位 DUT 并重新握手");
	request(OpCode::ResetDut, 0, guardTest([this](BoardLink::Status st, const Frame& f) {
		RcOnly rr;
		if (st != BoardLink::Status::Ok || !parse(f.payload, rr) || rr.rc != RET_OK) {
			setDut(DutState::Error, "复位 DUT 失败");
			m_host.log(LogLevel::Warn, "工位#" + std::to_string(m_no) + " 复位 DUT 失败");
			finishRound();
			return;
		}
		const uint32_t gen = m_testGen;
		startDutCheck([this, gen](bool) {
			if (gen == m_testGen) finishRound();
		});
	}));
}

void StationController::finishRound()
{
	m_host.loop().cancel(m_testTimer);
	if (m_holdsRf) {
		m_holdsRf = false;
		m_host.releaseRf(m_no);
	}

	bool anyFail = false, anyAbort = false;
	for (const auto& it : m_record.items) {
		anyFail |= it.state == ItemState::Fail;
		anyAbort |= it.state == ItemState::Abort || it.state == ItemState::Pending || it.state == ItemState::Running;
	}
	const Verdict v = anyFail ? Verdict::Fail : anyAbort ? Verdict::Abort : Verdict::Pass;
	m_record.verdict = v;
	m_record.timeMs = systemMs();

	m_testing = false;
	m_finished = !m_record.emergencyStopped;
	m_host.model().updateStation(m_no, [v](StationInfo& st) { st.lastVerdict = v; });
	refreshStationState();

	static const char* const kVerdictText[] = { "--", "PASS", "FAIL", "ABORT" };
	m_host.log(v == Verdict::Pass ? LogLevel::Info : v == Verdict::Fail ? LogLevel::Warn : LogLevel::Error,
		"工位#" + std::to_string(m_no) + " 本轮结果：" + kVerdictText[static_cast<int>(v)]);

	m_host.onRoundFinished(m_record);
	m_host.onStationChanged(m_no);
}

void StationController::emergencyStop()
{
	const std::string tag = "工位#" + std::to_string(m_no);
	const bool wasTesting = m_testing;
	++m_testGen;
	m_host.loop().cancel(m_testTimer);

	if (m_link && m_board == BoardState::Connected) {
		request(OpCode::StopCurrTask, 0, guardConn([this, tag](BoardLink::Status st, const Frame& f) {
			RcOnly r;
			if (st == BoardLink::Status::Ok && parse(f.payload, r) && r.rc == RET_OK)
				m_host.log(LogLevel::Info, tag + " 测试板已停止当前业务");
			else
				m_host.log(LogLevel::Warn, tag + " STOP_CURR_TASK 失败：" + (st == BoardLink::Status::Timeout ? std::string("应答超时") : rcText(r.rc)));
		}));
	}
	if (!wasTesting) return;

	m_record.emergencyStopped = true;
	abortRemaining(0, "紧急停止");
	finishRound();   // m_finished = false：回到准备中

	// 紧急停止后 DUT 状态不确定，重新握手确认是否就绪
	m_dutFailLogged = false;
	startDutCheck();
}

void StationController::prepareNextRound()
{
	if (m_testing) return;
	m_finished = false;
	for (int i = 0; i < kTestItemCount; ++i) setItem(i, ItemResult{});
	m_host.model().updateStation(m_no, [](StationInfo& st) { st.lastVerdict = Verdict::None; });
	refreshStationState();
	m_host.onStationChanged(m_no);
	// 换上新的 DUT 后重新握手（失败会每秒自动重试）
	m_dutFailLogged = false;
	startDutCheck();
}

} // namespace Lx::Tester
