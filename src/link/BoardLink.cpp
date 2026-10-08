#include "BoardLink.h"

namespace Lx::Link {

using namespace Lx::Proto;

std::shared_ptr<BoardLink> BoardLink::create(IEventLoop& loop, std::unique_ptr<ITransport> transport, Timings t)
{
	return std::shared_ptr<BoardLink>(new BoardLink(loop, std::move(transport), t));
}

BoardLink::BoardLink(IEventLoop& loop, std::unique_ptr<ITransport> transport, Timings t)
	: m_loop(loop), m_transport(std::move(transport)), m_timings(t)
{
}

BoardLink::~BoardLink()
{
	close();
}

bool BoardLink::open(std::string& err)
{
	std::weak_ptr<BoardLink> weak = weak_from_this();
	IEventLoop* loop = &m_loop;

	// 传输层回调在 IO 线程，切回业务线程处理；链路已销毁则丢弃
	auto onData = [weak, loop](std::vector<uint8_t> data) {
		loop->post([weak, data = std::move(data)] {
			if (auto self = weak.lock()) self->onBytes(data);
		});
	};
	auto onClosed = [weak, loop](std::string reason) {
		loop->post([weak, reason = std::move(reason)] {
			if (auto self = weak.lock()) self->lost("端口断开：" + reason);
		});
	};

	if (!m_transport->open(onData, onClosed, err)) return false;
	m_open = true;
	m_consecutiveTimeouts = 0;
	m_parser.reset();
	heartbeat();   // 立即发第一次心跳
	return true;
}

void BoardLink::close()
{
	if (!m_open) return;
	m_open = false;
	m_loop.cancel(m_heartbeatTimer);
	for (auto& [tid, p] : m_pending) m_loop.cancel(p.timer);
	m_pending.clear();
	m_heartbeatInFlight = false;
	m_transport->close();
}

void BoardLink::request(OpCode op, uint32_t timeoutMs, Reply onReply, std::vector<uint8_t> payload)
{
	if (!m_open) return;

	uint16_t tid = m_nextTid++;
	if (m_nextTid == 0) m_nextTid = 1;
	while (m_pending.count(tid)) tid = m_nextTid++;   // 理论上不会发生（同时在途请求很少）

	Frame f;
	f.tid = tid;
	f.opcode = static_cast<uint16_t>(op);
	f.payload = std::move(payload);

	Pending p;
	p.opcode = f.opcode;
	p.onReply = std::move(onReply);
	std::weak_ptr<BoardLink> weak = weak_from_this();
	p.timer = m_loop.after(timeoutMs ? timeoutMs : m_timings.timeoutMs, [weak, tid] {
		if (auto self = weak.lock()) self->onTimeout(tid);
	});
	m_pending.emplace(tid, std::move(p));

	m_transport->send(encodeFrame(f));   // 发送失败按超时处理
}

void BoardLink::onBytes(const std::vector<uint8_t>& data)
{
	if (!m_open) return;
	m_parser.feed(data.data(), data.size(), [this](const Frame& f) { onFrame(f); });
}

void BoardLink::onFrame(const Frame& f)
{
	auto it = m_pending.find(f.tid);
	if (it == m_pending.end() || f.opcode != (it->second.opcode | kAckFlag)) {
		++m_unmatched;   // 超时后迟到的应答，或 OpCode 不匹配
		return;
	}
	Pending p = std::move(it->second);
	m_pending.erase(it);
	m_loop.cancel(p.timer);
	m_consecutiveTimeouts = 0;
	if (p.onReply) p.onReply(Status::Ok, f);
}

void BoardLink::onTimeout(uint16_t tid)
{
	auto it = m_pending.find(tid);
	if (it == m_pending.end() || !m_open) return;
	Pending p = std::move(it->second);
	m_pending.erase(it);

	if (++m_consecutiveTimeouts >= m_timings.maxConsecutiveTimeouts) {
		lost("连续 " + std::to_string(m_consecutiveTimeouts) + " 次指令超时");
		return;
	}
	if (p.onReply) p.onReply(Status::Timeout, Frame{ tid, static_cast<uint16_t>(p.opcode | kAckFlag), {} });
}

void BoardLink::heartbeat()
{
	if (!m_open) return;
	std::weak_ptr<BoardLink> weak = weak_from_this();
	if (!m_heartbeatInFlight) {
		m_heartbeatInFlight = true;
		request(OpCode::TestboardAlive, 0, [weak](Status st, const Frame&) {
			auto self = weak.lock();
			if (!self) return;
			self->m_heartbeatInFlight = false;
			if (st == Status::Ok && self->m_onHeartbeat) self->m_onHeartbeat();
		});
	}
	m_heartbeatTimer = m_loop.after(m_timings.heartbeatMs, [weak] {
		if (auto self = weak.lock()) self->heartbeat();
	});
}

void BoardLink::lost(const std::string& reason)
{
	if (!m_open) return;
	close();
	auto onLost = m_onLost;   // 回调里可能销毁本链路，先拷贝
	if (onLost) onLost(reason);
}

} // namespace Lx::Link
