#include "BoardProtocol.h"
#include <algorithm>
#include <cstring>

namespace Lx::Proto {

const char* opCodeName(uint16_t op)
{
	switch (static_cast<OpCode>(op & ~kAckFlag)) {
	case OpCode::TestboardAlive:      return "TESTBOARD_ALIVE";
	case OpCode::ConnCheckDut:        return "CONN_CHECK_DUT";
	case OpCode::RfTestTxRx:          return "RF_TEST_TX_RX";
	case OpCode::RfTestTxRxGetResult: return "RF_TEST_TX_RX_GET_RESULT";
	case OpCode::RfTestCfo:           return "RF_TEST_CFO";
	case OpCode::RfTestCfoGetResult:  return "RF_TEST_CFO_GET_RESULT";
	case OpCode::PowerTest:           return "POWER_TEST";
	case OpCode::PowerTestGetResult:  return "POWER_TEST_GET_RESULT";
	case OpCode::GetTestStatus:       return "GET_TEST_STATUS";
	case OpCode::ResetDut:            return "RESET_DUT";
	case OpCode::TestboardReset:      return "TESTBOARD_RESET";
	case OpCode::StopCurrTask:        return "STOP_CURR_TASK";
	case OpCode::AbortAllTask:        return "ABORT_ALL_TASK";
	case OpCode::GetVersion:          return "GET_VERSION";
	}
	return "UNKNOWN";
}

const char* resultCodeText(uint8_t rc)
{
	switch (rc) {
	case RET_OK:                   return "成功";
	case RET_ERR_FRAME_ERR:        return "被测板返回帧错误";
	case RET_ERR_DUT_NO_RESPONSE:  return "DUT无应答";
	case RET_ERR_DUT_BUSY:         return "DUT忙";
	case RET_ERR_BUSINESS_TIMEOUT: return "业务执行超时";
	case RET_ERR_NOT_SUPPORT:      return "指令不支持";
	case RET_ERR_ABORTED:          return "业务被中止";
	case RET_ERR_RESOURCE_BUSY:    return "测试板繁忙";
	default:                       return "未知错误";
	}
}

uint16_t crc16Modbus(const uint8_t* data, size_t len)
{
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < len; ++i) {
		crc ^= data[i];
		for (int b = 0; b < 8; ++b)
			crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
	}
	return crc;
}

std::vector<uint8_t> encodeFrame(const Frame& f)
{
	const size_t len = std::min(f.payload.size(), kMaxPayload);
	std::vector<uint8_t> out;
	out.reserve(kOverhead + len);
	out.push_back(kHeader0);
	out.push_back(kHeader1);
	out.push_back(static_cast<uint8_t>(f.tid & 0xFF));          // TID 小端
	out.push_back(static_cast<uint8_t>(f.tid >> 8));
	out.push_back(static_cast<uint8_t>(f.opcode >> 8));         // OpCode 大端
	out.push_back(static_cast<uint8_t>(f.opcode & 0xFF));
	out.push_back(static_cast<uint8_t>(len));
	out.insert(out.end(), f.payload.begin(), f.payload.begin() + len);
	const uint16_t crc = crc16Modbus(out.data(), out.size());
	out.push_back(static_cast<uint8_t>(crc & 0xFF));            // CRC 小端
	out.push_back(static_cast<uint8_t>(crc >> 8));
	out.push_back(kTail);
	return out;
}

void FrameParser::feed(const uint8_t* data, size_t len, const Handler& onFrame)
{
	m_buf.insert(m_buf.end(), data, data + len);

	size_t pos = 0;
	for (;;) {
		// 找帧头
		while (pos + 1 < m_buf.size() && !(m_buf[pos] == kHeader0 && m_buf[pos + 1] == kHeader1)) {
			++pos;
		}
		if (pos + kHeadLen > m_buf.size()) break;

		const size_t plen = m_buf[pos + 6];
		if (plen > kMaxPayload) {
			++m_errors;
			++pos;
			continue;
		}
		const size_t total = kOverhead + plen;
		if (pos + total > m_buf.size()) break;

		const uint8_t* f = m_buf.data() + pos;
		const uint16_t crc = static_cast<uint16_t>(f[kHeadLen + plen] | (f[kHeadLen + plen + 1] << 8));
		if (f[total - 1] != kTail || crc != crc16Modbus(f, kHeadLen + plen)) {
			++m_errors;
			++pos;
			continue;
		}

		Frame fr;
		fr.tid = static_cast<uint16_t>(f[2] | (f[3] << 8));
		fr.opcode = static_cast<uint16_t>((f[4] << 8) | f[5]);
		fr.payload.assign(f + kHeadLen, f + kHeadLen + plen);
		pos += total;
		onFrame(fr);
	}

	// 丢弃已消费/无效的部分；pos 停在未完整的帧头处（或最后 1 字节，可能是下一帧的 0x55）
	m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(std::min(pos, m_buf.size())));
	// 防御：缓冲区异常增长（理论上不会超过一帧）
	if (m_buf.size() > 4 * (kOverhead + kMaxPayload)) {
		m_buf.clear();
		++m_errors;
	}
}

// ---------------------------------------------------------------- Payload 读写

namespace {

struct Reader {
	const std::vector<uint8_t>& p;
	size_t pos = 0;
	bool ok = true;

	uint8_t u8()
	{
		if (pos + 1 > p.size()) { ok = false; return 0; }
		return p[pos++];
	}
	uint16_t u16()
	{
		if (pos + 2 > p.size()) { ok = false; return 0; }
		uint16_t v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8));
		pos += 2;
		return v;
	}
	std::string cstr(size_t fixedLen)
	{
		if (pos + fixedLen > p.size()) { ok = false; return {}; }
		const char* s = reinterpret_cast<const char*>(p.data() + pos);
		size_t n = 0;
		while (n < fixedLen && s[n] != '\0') ++n;
		pos += fixedLen;
		return std::string(s, n);
	}
};

struct Writer {
	std::vector<uint8_t> p;
	void u8(uint8_t v) { p.push_back(v); }
	void u16(uint16_t v) { p.push_back(static_cast<uint8_t>(v & 0xFF)); p.push_back(static_cast<uint8_t>(v >> 8)); }
	void cstr(const std::string& s, size_t fixedLen)
	{
		for (size_t i = 0; i < fixedLen; ++i)
			p.push_back(i + 1 < fixedLen && i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
	}
};

constexpr size_t kVersionStrLen = 16;

// 读 rc；rc != OK 时不再要求后续数据
template<typename T, typename Fn>
bool parseWithRc(const std::vector<uint8_t>& p, T& out, Fn&& body)
{
	Reader r{ p };
	out = T{};
	out.rc = r.u8();
	if (!r.ok) return false;
	if (out.rc != RET_OK) return true;
	body(r, out);
	return r.ok;
}

} // namespace

bool parse(const std::vector<uint8_t>& p, RcOnly& out)
{
	return parseWithRc(p, out, [](Reader&, RcOnly&) {});
}

bool parse(const std::vector<uint8_t>& p, RfTxRxResult& out)
{
	return parseWithRc(p, out, [](Reader& r, RfTxRxResult& o) {
		o.total = r.u16();
		o.boardRx = r.u16();
		o.dutRx = r.u16();
		o.rssi = static_cast<int8_t>(r.u8());
	});
}

bool parse(const std::vector<uint8_t>& p, CfoResult& out)
{
	return parseWithRc(p, out, [](Reader& r, CfoResult& o) { o.raw = static_cast<int16_t>(r.u16()); });
}

bool parse(const std::vector<uint8_t>& p, PowerResult& out)
{
	return parseWithRc(p, out, [](Reader& r, PowerResult& o) {
		o.avgUA = r.u16();
		o.maxUA = r.u16();
		o.minUA = r.u16();
	});
}

bool parse(const std::vector<uint8_t>& p, StatusResult& out)
{
	return parseWithRc(p, out, [](Reader& r, StatusResult& o) {
		o.status = static_cast<BoardTaskStatus>(r.u8());
	});
}

bool parse(const std::vector<uint8_t>& p, VersionResult& out)
{
	return parseWithRc(p, out, [](Reader& r, VersionResult& o) {
		o.fw = r.cstr(kVersionStrLen);
		o.hw = r.cstr(kVersionStrLen);
	});
}

std::vector<uint8_t> encode(const RcOnly& v)
{
	return { v.rc };
}

std::vector<uint8_t> encode(const RfTxRxResult& v)
{
	Writer w;
	w.u8(v.rc);
	if (v.rc == RET_OK) {
		w.u16(v.total);
		w.u16(v.boardRx);
		w.u16(v.dutRx);
		w.u8(static_cast<uint8_t>(v.rssi));
	}
	return w.p;
}

std::vector<uint8_t> encode(const CfoResult& v)
{
	Writer w;
	w.u8(v.rc);
	if (v.rc == RET_OK) w.u16(static_cast<uint16_t>(v.raw));
	return w.p;
}

std::vector<uint8_t> encode(const PowerResult& v)
{
	Writer w;
	w.u8(v.rc);
	if (v.rc == RET_OK) {
		w.u16(v.avgUA);
		w.u16(v.maxUA);
		w.u16(v.minUA);
	}
	return w.p;
}

std::vector<uint8_t> encode(const StatusResult& v)
{
	Writer w;
	w.u8(v.rc);
	if (v.rc == RET_OK) w.u8(static_cast<uint8_t>(v.status));
	return w.p;
}

std::vector<uint8_t> encode(const VersionResult& v)
{
	Writer w;
	w.u8(v.rc);
	if (v.rc == RET_OK) {
		w.cstr(v.fw, kVersionStrLen);
		w.cstr(v.hw, kVersionStrLen);
	}
	return w.p;
}

} // namespace Lx::Proto
