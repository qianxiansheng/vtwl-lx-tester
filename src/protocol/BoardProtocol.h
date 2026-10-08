#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 上位机 - 测试板 帧协议（见 产测协议规范.md）
// 帧：Header(55 AA) | TID(2,LE) | OpCode(2,BE) | PayloadLen(1) | Payload | CRC16(2,LE, modbus, 覆盖 [0, 7+len)) | Tail(33)
namespace Lx::Proto {

constexpr uint8_t kHeader0   = 0x55;
constexpr uint8_t kHeader1   = 0xAA;
constexpr uint8_t kTail      = 0x33;
constexpr size_t  kMaxPayload = 54;
constexpr size_t  kHeadLen    = 7;   // Header + TID + OpCode + PayloadLen
constexpr size_t  kOverhead   = 10;  // kHeadLen + CRC16 + Tail

constexpr uint16_t kAckFlag = 0x8000;   // 应答 OpCode = 请求 OpCode | 0x8000

enum class OpCode : uint16_t {
	TestboardAlive      = 0x0000,
	ConnCheckDut        = 0x0001,
	RfTestTxRx          = 0x0002,
	RfTestTxRxGetResult = 0x0003,
	RfTestCfo           = 0x0004,
	RfTestCfoGetResult  = 0x0005,
	PowerTest           = 0x0006,
	PowerTestGetResult  = 0x0007,
	GetTestStatus       = 0x0008,
	ResetDut            = 0x0009,
	TestboardReset      = 0x000A,
	StopCurrTask        = 0x000B,
	AbortAllTask        = 0x000C,
	GetVersion          = 0x000D,
};

constexpr uint16_t ackOf(OpCode op) { return static_cast<uint16_t>(op) | kAckFlag; }
const char* opCodeName(uint16_t op);

// Board-Link 业务返回码
enum ResultCode : uint8_t {
	RET_OK                   = 0x00,
	RET_ERR_DUT_NO_RESPONSE  = 0x10,
	RET_ERR_DUT_BUSY         = 0x11,
	RET_ERR_BUSINESS_TIMEOUT = 0x17,
	RET_ERR_NOT_SUPPORT      = 0x18,
	RET_ERR_ABORTED          = 0x19,
	RET_ERR_RESOURCE_BUSY    = 0x1A,
	RET_RESERVED_BEGIN       = 0x20,
	RET_ERR_FRAME_ERR        = 0x80,
};
const char* resultCodeText(uint8_t rc);   // 中文说明，未知码返回 "未知错误"

// 工位运行状态（ACK_GET_TEST_STATUS）
enum class BoardTaskStatus : uint8_t { Idle = 0, Running = 1, Error = 2 };

uint16_t crc16Modbus(const uint8_t* data, size_t len);

struct Frame {
	uint16_t tid = 0;
	uint16_t opcode = 0;
	std::vector<uint8_t> payload;
};

// payload 超过 kMaxPayload 时截断（调用方保证不超）
std::vector<uint8_t> encodeFrame(const Frame& f);

// 流式解析：处理半包/粘包，帧头/长度/CRC/帧尾错误时丢弃 1 字节重新同步
class FrameParser {
public:
	using Handler = std::function<void(const Frame&)>;
	void feed(const uint8_t* data, size_t len, const Handler& onFrame);
	void reset() { m_buf.clear(); }
	uint32_t errorCount() const { return m_errors; }
private:
	std::vector<uint8_t> m_buf;
	uint32_t m_errors = 0;
};

// ---------------------------------------------------------------- 应答 Payload
// 所有应答第 1 字节为 result_code；result_code != RET_OK 时后续数据可能缺失，此时只保证 rc 有效。

struct RcOnly       { uint8_t rc = RET_OK; };
struct RfTxRxResult { uint8_t rc = RET_OK; uint16_t total = 0, boardRx = 0, dutRx = 0; int8_t rssi = 0; };
struct CfoResult    { uint8_t rc = RET_OK; int16_t raw = 0; };
struct PowerResult  { uint8_t rc = RET_OK; uint16_t avgUA = 0, maxUA = 0, minUA = 0; };
struct StatusResult { uint8_t rc = RET_OK; BoardTaskStatus status = BoardTaskStatus::Idle; };
struct VersionResult{ uint8_t rc = RET_OK; std::string fw, hw; };

// 解析成功返回 true；rc == RET_OK 但数据长度不足时返回 false（视为格式错误）
bool parse(const std::vector<uint8_t>& p, RcOnly& out);
bool parse(const std::vector<uint8_t>& p, RfTxRxResult& out);
bool parse(const std::vector<uint8_t>& p, CfoResult& out);
bool parse(const std::vector<uint8_t>& p, PowerResult& out);
bool parse(const std::vector<uint8_t>& p, StatusResult& out);
bool parse(const std::vector<uint8_t>& p, VersionResult& out);

// 编码（模拟测试板使用）
std::vector<uint8_t> encode(const RcOnly& v);
std::vector<uint8_t> encode(const RfTxRxResult& v);
std::vector<uint8_t> encode(const CfoResult& v);
std::vector<uint8_t> encode(const PowerResult& v);
std::vector<uint8_t> encode(const StatusResult& v);
std::vector<uint8_t> encode(const VersionResult& v);

} // namespace Lx::Proto
