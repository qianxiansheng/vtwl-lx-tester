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

	if (info.port.empty()) {
		m_host.notify(AlertLevel::Warning, "未选择端口", tag + " 请先选择端口。");
		return;
	}

	std::unique_ptr<Link::ITransport> transport;
	if (Link::isSimPort(info.port))
		transport = Link::makeSimTransport(info.port);
	else if (info.portType == PortType::Usb)
		transport = Link::makeUsbTransport(info.port);
	else
		transport = Link::makeSerialTransport(info.port, static_cast<uint32_t>(cfg.comm.baudRate));

	BoardLink::Timings t;
	t.heartbeatMs = static_cast<uint32_t>(cfg.comm.heartbeatTimeoutMs);
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
		abortFtFlag("测试板断开");
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

	const TestConfig cfg = m_host.config();
	const std::string fw = m_record.boardFw, hw = m_record.boardHw;
	m_record = RoundRecord{};
	m_record.no = m_no;
	m_record.port = m_port;
	m_record.boardFw = fw;
	m_record.boardHw = hw;
	m_mode = cfg.comm.testMode;
	const TestModeDef* modeDef = FindMode(m_mode);
	m_localEnableMask = modeDef->enableMask;
	
	for (int i = 0; i < kTestItemCount; ++i) {
		if (HasItem(m_localEnableMask, static_cast<TestItem>(i))) {
			setItem(i, ItemResult{});
		}
		else {
			ItemResult skipped;
			skipped.state = ItemState::Skipped;
			skipped.note = std::string() + modeDef->name + " 忽略测试项";
			setItem(i, skipped);
		}
	}
	setFtFlag(ItemResult{});
	m_host.model().updateStation(m_no, [](StationInfo& st) { st.lastVerdict = Verdict::None; });
	refreshStationState();

	TestItem firstItem{};
	int firstItemIdx = static_cast<int>(firstItem);
	if (GetFirstSetTestItem(m_localEnableMask, firstItem)) {
		if (IsRfTestItem(firstItem)) {
			ItemResult waiting;
			waiting.state = ItemState::Running;
			waiting.note = "等待 RF 资源";
			setItem(firstItemIdx, waiting);
		}
	}

	m_host.acquireRf(m_no, guardTest([this, firstItemIdx] {
		m_holdsRf = true;
		m_host.log(LogLevel::Info, "工位#" + std::to_string(m_no) + " 获得 RF 资源");
		runItem(firstItemIdx);
	}));
}

void StationController::runItem(int idx)
{
	m_attempt = 0;
	if (IsLastSetTestItem(m_localEnableMask, static_cast<TestItem>(idx)) && m_holdsRf) {
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
	TestItem nextItem{};
	if (GetNextSetTestItem(m_localEnableMask, idx, nextItem)) {
		int nextItemIdx = static_cast<int>(nextItem);
		runItem(nextItemIdx);
	}
	else finishTest();
}

void StationController::finishTest()
{
	if (m_holdsRf) {
		m_holdsRf = false;
		m_host.releaseRf(m_no);
	}
	if (!m_link) {
		abortFtFlag("测试板断开");
		finishRound();
		return;
	}

	const bool allPass = ([this]() {
		bool pass = true;
		const int itemCount = static_cast<int>(m_record.items.size());
		for (int i = 0; i < itemCount; ++i) {
			if (HasItem(m_localEnableMask, static_cast<TestItem>(i))) {
				if (m_record.items[i].state != ItemState::Pass) {
					pass = false;
					break;
				}
			}
		}
		return pass;
	})();
	
	if (allPass) {
		// 全部测试项通过：只有包含功耗测试流程才需要执行复位+重握手
		const bool needReset = HasItem(m_localEnableMask, TestItem::Power);
		startFtFlag(needReset);   // 写标志流程本身包含复位 + 重新握手
		return;
	}

	ItemResult skipped;
	skipped.state = ItemState::Skipped;
	skipped.note = "测试项未全部 PASS，不写入产测标志";
	setFtFlag(skipped);

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

// ---------------------------------------------------------------- 写入产测标志

void StationController::setFtFlag(const ItemResult& r)
{
	m_record.ftFlag = r;
	m_host.model().updateStation(m_no, [&](StationInfo& st) { st.ftFlag = r; });
}

void StationController::setFtFlagStep(const std::string& step, int attempt)
{
	ItemResult r;
	r.state = ItemState::Running;
	r.attempts = attempt + 1;
	r.note = attempt == 0 ? step : step + fmt("（第 %d 次重试）", attempt);
	setFtFlag(r);
}

void StationController::startFtFlag(bool reset)
{
	m_host.log(LogLevel::Info, "工位#" + std::to_string(m_no) + " 测试项全部 PASS，开始写入产测标志");
	if (reset)
		ftReset(0);
	else
		ftSet(0);
}

// 1. 复位 DUT，失败按配置重试
void StationController::ftReset(int attempt)
{
	setFtFlagStep("复位 DUT", attempt);
	request(OpCode::ResetDut, 0, guardTest([this, attempt](BoardLink::Status st, const Frame& f) {
		RcOnly r;
		if (st == BoardLink::Status::Ok && parse(f.payload, r) && r.rc == RET_OK) {
			ftConnCheck();
			return;
		}
		const std::string reason = st == BoardLink::Status::Timeout ? std::string("应答超时") : rcText(r.rc);
		if (attempt < m_host.config().ftFlag.cmdRetryCount) {
			m_host.log(LogLevel::Warn, "工位#" + std::to_string(m_no) + " 写标志前复位 DUT 失败（" + reason + "），重试");
			ftReset(attempt + 1);
			return;
		}
		ftDone(false, "复位 DUT 失败：" + reason, st == BoardLink::Status::Ok ? r.rc : -1);
	}));
}

// 2. 握手，失败不重试
void StationController::ftConnCheck()
{
	setFtFlagStep("DUT 握手", 0);
	const uint32_t gen = m_testGen;
	startDutCheck([this, gen](bool ok) {
		if (gen != m_testGen) return;
		if (ok) ftSet(0);
		else ftDone(false, "DUT 握手失败");
	});
}

// 3. 写入产测标志，失败按配置重试；成功后延时读回
void StationController::ftSet(int attempt)
{
	setFtFlagStep("写入产测标志", attempt);
	request(OpCode::SetFtFlag, 0, guardTest([this, attempt](BoardLink::Status st, const Frame& f) {
		RcOnly r;
		if (st == BoardLink::Status::Ok && parse(f.payload, r) && r.rc == RET_OK) {
			setFtFlagStep("等待读回产测标志", 0);
			const uint32_t delay = static_cast<uint32_t>(m_host.config().ftFlag.readDelayMs);
			m_testTimer = m_host.loop().after(delay, guardTest([this] { ftRead(); }));
			return;
		}
		const std::string reason = st == BoardLink::Status::Timeout ? std::string("应答超时") : rcText(r.rc);
		if (attempt < m_host.config().ftFlag.cmdRetryCount) {
			m_host.log(LogLevel::Warn, "工位#" + std::to_string(m_no) + " 写入产测标志失败（" + reason + "），重试");
			ftSet(attempt + 1);
			return;
		}
		ftDone(false, "写入产测标志失败：" + reason, st == BoardLink::Status::Ok ? r.rc : -1);
	}));
}

// 4. 读回产测标志，== 1 才算成功
void StationController::ftRead()
{
	setFtFlagStep("读取产测标志", 0);
	request(OpCode::GetFtFlag, 0, guardTest([this](BoardLink::Status st, const Frame& f) {
		if (st == BoardLink::Status::Timeout) { ftDone(false, "读取产测标志应答超时"); return; }
		FtFlagResult r;
		if (!parse(f.payload, r)) { ftDone(false, "读取产测标志应答格式错误"); return; }
		if (r.rc != RET_OK) { ftDone(false, "读取产测标志失败：" + rcText(r.rc), r.rc); return; }
		if (r.flag != 1) { ftDone(false, fmt("读回产测标志为 %u（期望 1）", r.flag), r.rc, r.flag); return; }
		ftDone(true, "已写入，读回产测标志为 1", r.rc, r.flag);
	}));
}

void StationController::ftDone(bool ok, const std::string& note, int rc, int flag)
{
	ItemResult r;
	r.state = ok ? ItemState::Pass : ItemState::Fail;
	r.attempts = m_record.ftFlag.attempts;
	r.resultCode = rc;
	r.note = note;
	if (flag >= 0) {
		r.valueCount = 1;
		r.values = { static_cast<double>(flag), 0, 0 };
	}
	setFtFlag(r);
	if (rc == RET_ERR_DUT_NO_RESPONSE) setDut(DutState::Error, note);
	m_host.log(ok ? LogLevel::Info : LogLevel::Error,
		"工位#" + std::to_string(m_no) + " 写入产测标志：" + (ok ? "成功，" : "失败，") + note);
	finishRound();
}

void StationController::abortFtFlag(const std::string& reason)
{
	const ItemState s = m_record.ftFlag.state;
	if (s == ItemState::Pass || s == ItemState::Fail || s == ItemState::Abort || s == ItemState::Skipped) return;
	ItemResult r;
	// 已开始写标志 → ABORT；还没轮到 → 跳过
	r.state = s == ItemState::Running ? ItemState::Abort : ItemState::Skipped;
	r.attempts = m_record.ftFlag.attempts;
	r.note = reason;
	setFtFlag(r);
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
	// 写产测标志失败 = 产测整体失败；被紧急停止/断线打断 = 无有效结果
	anyFail |= m_record.ftFlag.state == ItemState::Fail;
	anyAbort |= m_record.ftFlag.state == ItemState::Abort;
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
	abortFtFlag("紧急停止");
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
	setFtFlag(ItemResult{});
	m_host.model().updateStation(m_no, [](StationInfo& st) { st.lastVerdict = Verdict::None; });
	refreshStationState();
	m_host.onStationChanged(m_no);
	// 换上新的 DUT 后重新握手（失败会每秒自动重试）
	m_dutFailLogged = false;
	startDutCheck();
}

} // namespace Lx::Tester
