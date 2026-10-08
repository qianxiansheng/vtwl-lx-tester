#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include "EventLoop.h"
#include "Transport.h"
#include "protocol/BoardProtocol.h"

namespace Lx::Link {

// 上位机与一块测试板之间的链路（一个端口一条）。只在业务线程上使用。
// - 请求/应答按 TID 匹配，应答 OpCode 必须是 请求 | 0x8000
// - 每个请求独立超时；任意请求连续超时 maxConsecutiveTimeouts 次 → 判定测试板断开
// - 打开后按 heartbeatMs 周期发送 REQ_TESTBOARD_ALIVE
// - 传输层物理断开 → 判定测试板断开
// 断开（lost）或 close() 之后，未完成的请求不再回调。
class BoardLink : public std::enable_shared_from_this<BoardLink> {
public:
	enum class Status { Ok, Timeout };
	using Reply = std::function<void(Status status, const Proto::Frame& ack)>;
	using LostHandler = std::function<void(const std::string& reason)>;
	using HeartbeatHandler = std::function<void()>;

	struct Timings {
		uint32_t heartbeatMs = 500;
		uint32_t timeoutMs = 200;             // 心跳及默认指令超时
		int      maxConsecutiveTimeouts = 3;
	};

	static std::shared_ptr<BoardLink> create(IEventLoop& loop, std::unique_ptr<ITransport> transport, Timings t);
	~BoardLink();

	void setOnLost(LostHandler h) { m_onLost = std::move(h); }
	void setOnHeartbeat(HeartbeatHandler h) { m_onHeartbeat = std::move(h); }

	bool open(std::string& err);
	void close();
	bool isOpen() const { return m_open; }

	// timeoutMs = 0 使用默认超时
	void request(Proto::OpCode op, uint32_t timeoutMs, Reply onReply, std::vector<uint8_t> payload = {});

	const std::string& portName() const { return m_transport->name(); }
	uint32_t frameErrors() const { return m_parser.errorCount() + m_unmatched; }

private:
	BoardLink(IEventLoop& loop, std::unique_ptr<ITransport> transport, Timings t);

	void onBytes(const std::vector<uint8_t>& data);
	void onFrame(const Proto::Frame& f);
	void onTimeout(uint16_t tid);
	void heartbeat();
	void lost(const std::string& reason);

	struct Pending {
		uint16_t opcode = 0;
		IEventLoop::TimerId timer = 0;
		Reply onReply;
	};

	IEventLoop&                  m_loop;
	std::unique_ptr<ITransport>  m_transport;
	Timings                      m_timings;
	Proto::FrameParser           m_parser;
	std::unordered_map<uint16_t, Pending> m_pending;
	uint16_t                     m_nextTid = 1;
	int                          m_consecutiveTimeouts = 0;
	bool                         m_open = false;
	bool                         m_heartbeatInFlight = false;
	IEventLoop::TimerId          m_heartbeatTimer = 0;
	uint32_t                     m_unmatched = 0;
	LostHandler                  m_onLost;
	HeartbeatHandler             m_onHeartbeat;
};

} // namespace Lx::Link
