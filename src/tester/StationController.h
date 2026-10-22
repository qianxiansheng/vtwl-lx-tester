#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include "TesterModel.h"
#include "link/BoardLink.h"
#include "protocol/BoardProtocol.h"

namespace Lx::Tester {

// 一个工位一轮测试的完整记录（写 CSV / 统计用）
struct RoundRecord {
	long long   timeMs = 0;            // 本轮结束时间
	int         no = 0;
	std::string port;
	std::string boardFw, boardHw;
	std::string dutId;                 // 保留，暂无查询指令
	bool        emergencyStopped = false;

	std::array<ItemResult, kTestItemCount> items{};
	ItemResult           ftFlag;       // 写入产测标志步骤（values[0] = 读回的标志值）
	bool                 hasTxRx = false, hasCfo = false, hasPower = false;   // 是否拿到原始数据
	Proto::RfTxRxResult  txrx;
	Proto::CfoResult     cfo;
	Proto::PowerResult   power;
	Verdict              verdict = Verdict::None;
};

// 工位控制器需要的宿主能力（由 TesterService 实现，全部在业务线程上调用）
class IStationHost {
public:
	virtual ~IStationHost() = default;
	virtual Link::IEventLoop& loop() = 0;
	virtual TesterModel& model() = 0;
	virtual TestConfig config() = 0;
	virtual void log(LogLevel level, std::string text) = 0;
	virtual void notify(AlertLevel level, std::string title, std::string message) = 0;

	// RF 互斥资源：granted 在拿到锁时（异步）调用
	virtual void acquireRf(int no, std::function<void()> granted) = 0;
	virtual void releaseRf(int no) = 0;   // 同时会把该工位移出等待队列

	virtual void onStationChanged(int no) = 0;           // 连接/测试状态变化，用于重算全局状态
	virtual void onRoundFinished(const RoundRecord& rec) = 0;
};

// 一个工位（一块测试板 + 一个 DUT）的连接管理与测试流程状态机。只在业务线程上使用。
//
// 连接：打开端口 → 心跳应答（测试板已连接）→ GET_VERSION → CONN_CHECK_DUT（DUT 就绪）
//       非测试中 DUT 未就绪时每秒自动重新握手。
// 测试：每项 = 启动指令 → 轮询 GET_TEST_STATUS 直到空闲/异常 → GET_RESULT → 判定
//       顺序 收发包 → 频偏（持有 RF 锁）→ 功耗；结束后 RESET_DUT + CONN_CHECK_DUT。
//       ABORT（非紧急停止）按配置重试，重试前 RESET_DUT + CONN_CHECK_DUT；FAIL 不重试。
// 产测标志：3 项全部 PASS 后 RESET_DUT（重试）→ CONN_CHECK_DUT → SET_FT_FLAG（重试）→ 延时 → GET_FT_FLAG == 1，
//       任一步失败本轮判 FAIL；未全部 PASS 时跳过，只做 RESET_DUT + CONN_CHECK_DUT。
class StationController : public std::enable_shared_from_this<StationController> {
public:
	StationController(IStationHost& host, int no);
	~StationController();

	int  no() const { return m_no; }
	bool isBoardConnected() const { return m_board == BoardState::Connected; }
	bool isDutReady() const { return m_dut == DutState::Ready; }
	bool isTesting() const { return m_testing; }

	void connect();
	void disconnect();

	void startRound();
	void emergencyStop();
	void prepareNextRound();

private:
	using Reply = Link::BoardLink::Reply;

	// ---- 连接
	void onBoardAlive();
	void onLinkLost(const std::string& reason);
	void startDutCheck(std::function<void(bool ok)> done = nullptr);
	void scheduleDutRetry();
	void setBoard(BoardState s, std::string note = {});
	void setDut(DutState s, std::string note = {});
	void refreshStationState();

	// ---- 测试
	void runItem(int idx);
	void executeItem(int idx);
	void pollItem(int idx);
	void fetchResult(int idx, bool boardReportedError);
	void itemPassFail(int idx, bool pass, const ItemResult& r, const std::string& detail);
	void itemAbort(int idx, const std::string& reason, int rc = -1);
	void nextItem(int idx);
	void finishTest();
	void finishRound();
	void setItem(int idx, const ItemResult& r);
	void abortRemaining(int fromIdx, const std::string& reason);

	// ---- 写入产测标志
	void setFtFlag(const ItemResult& r);
	void setFtFlagStep(const std::string& step, int attempt);
	void startFtFlag();
	void ftReset(int attempt);
	void ftConnCheck();
	void ftSet(int attempt);
	void ftRead();
	void ftDone(bool ok, const std::string& note, int rc = -1, int flag = -1);
	void abortFtFlag(const std::string& reason);

	// 请求封装：链路不存在时直接忽略；回调受 generation 保护
	void request(Proto::OpCode op, uint32_t timeoutMs, Reply onReply);
	// 包装回调：本工位已销毁、测试已被打断（m_testGen 变化）时丢弃
	std::function<void()> guardTest(std::function<void()> fn);
	Reply guardTest(Reply fn);
	Reply guardConn(Reply fn);

	IStationHost& m_host;
	int           m_no;

	std::shared_ptr<Link::BoardLink> m_link;
	BoardState    m_board = BoardState::Disconnected;
	DutState      m_dut = DutState::Disconnected;
	uint32_t      m_connGen = 0;            // 每次连接/断开 +1
	bool          m_dutChecking = false;
	bool          m_dutFailLogged = false;
	Link::IEventLoop::TimerId m_dutRetryTimer = 0;
	std::string   m_port;

	bool          m_testing = false;
	bool          m_finished = false;       // 本轮已结束，等待“下一轮”
	uint32_t      m_testGen = 0;            // 每轮开始/紧急停止/断开 +1
	bool          m_holdsRf = false;
	int           m_attempt = 0;
	long long     m_deadlineMs = 0;         // 当前项业务总超时时间点（steady clock ms）
	Link::IEventLoop::TimerId m_testTimer = 0;
	RoundRecord   m_record;
};

} // namespace Lx::Tester
