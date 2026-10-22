#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// 产测上位机的领域数据类型（Model / Service / ViewModel 共用，不依赖 ImGui）
namespace Lx::Tester {

constexpr int kMaxStations  = 6;
constexpr int kTestItemCount = 3;
constexpr int kTestModeCount = 4;
constexpr int kMaxRetry      = 5;

// 全局主状态：准备中 -> 准备就绪 -> 测试进行中 -> 测试结束 -> 准备中
enum class GlobalState : uint8_t { Preparing, Ready, Testing, Finished };

// 测试板连接状态（上位机 - 测试板链路）
enum class BoardState : uint8_t { Disconnected, Connecting, Connected, Error };

// DUT 连接状态（测试板 - DUT 链路；Connecting = 握手中）
enum class DutState : uint8_t { Disconnected, Connecting, Ready, Error };

enum class PortType : uint8_t { Uart = 0, Usb = 1 };

// 工位本地业务状态
enum class StationState : uint8_t { Preparing, Ready, Testing, Finished };

// 单个测试项状态
// Skipped：本轮未执行（如测试项未全部 PASS 时不写产测标志）
enum class ItemState : uint8_t { Pending, Running, Pass, Fail, Abort, Skipped };

// 工位一轮综合结果
enum class Verdict : uint8_t { None, Pass, Fail, Abort };

// 测试模式
enum class TestMode : uint8_t { ModeA = 0, ModeB = 1, ModeC = 2, ModeD = 3 };

enum class TestItem : uint8_t { RfPacket = 0, FreqOffset = 1, Power = 2 };

inline constexpr bool IsRfTestItem(TestItem item) noexcept
{
	return (item == TestItem::RfPacket) || (item == TestItem::FreqOffset);
}

// 位掩码：每个bit代表一个测试项是否启用
enum class TestItemMask : uint8_t {
	None       = 0,
	RfPacket   = 1 << static_cast<uint8_t>(TestItem::RfPacket),
	FreqOffset = 1 << static_cast<uint8_t>(TestItem::FreqOffset),
	Power      = 1 << static_cast<uint8_t>(TestItem::Power),
	All        = RfPacket | FreqOffset | Power,
};

inline constexpr TestItemMask operator|(TestItemMask a, TestItemMask b) noexcept
{
	return static_cast<TestItemMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline constexpr bool HasItem(TestItemMask mask, TestItem item) noexcept
{
	return (static_cast<uint8_t>(mask) & (1U << static_cast<uint8_t>(item))) != 0;
}

inline constexpr bool GetFirstSetTestItem(TestItemMask mask, TestItem& outItem) noexcept
{
	const uint8_t raw = static_cast<uint8_t>(mask);
	for (uint8_t i = 0; i < kTestItemCount; ++i) {
		if (raw & (1 << i)) {
			outItem = static_cast<TestItem>(i);
			return true;
		}
	}
	return false;
}

inline constexpr bool IsFirstItemRfRelated(TestItemMask mask) noexcept
{
	TestItem item{};
	if (!GetFirstSetTestItem(mask, item))
		return false;
	return IsRfTestItem(item);
}

inline constexpr bool IsLastSetTestItem(TestItemMask mask, TestItem currentItem) noexcept
{
	const uint8_t raw = static_cast<uint8_t>(mask);
	for (int i = kTestItemCount - 1; i >= 0; --i)
	{
		if (raw & (1U << i))
		{
			return static_cast<TestItem>(i) == currentItem;
		}
	}
	return false; // mask无任何bit置位
}

inline constexpr bool GetNextSetTestItem(TestItemMask mask, int currentIdx, TestItem& outNextItem) noexcept
{
	const uint8_t raw = static_cast<uint8_t>(mask);
	for (int i = currentIdx + 1; i < kTestItemCount; ++i)
	{
		if (raw & (1U << i))
		{
			outNextItem = static_cast<TestItem>(i);
			return true;
		}
	}
	return false;
}

struct TestModeDef {
	TestMode        mode;
	const char*     name;
	TestItemMask    enableMask;
};

// 每个测试类别都由「公共部分」+「专用部分」组成
struct ItemCommonConfig {
	int retryCount = 0;               // ABORT（非紧急停止）重试次数 0~5；FAIL 不重试
	int timeoutMs  = 10000;           // 业务总超时（启动指令发出到拿到结果），超时即 ABORT
};

struct RfPacketConfig {
	ItemCommonConfig common;
	double minRxRatePct = 95.0;       // DUT 收包率 = 被测板收包数 / 总发包 >= 阈值
	double minTxRatePct = 95.0;       // DUT 发包率 = 测试板收包数 / 总发包 >= 阈值
	double minRssiDbm   = -70.0;      // RSSI 均值 >= 阈值
};

struct FreqOffsetConfig {
	ItemCommonConfig common{ 0, 5000 };
	double maxAbsOffsetKHz = 50.0;    // |频偏| <= 阈值 -> PASS
	double cfoHzPerLsb     = 1.0;     // CFO 原始 int16 值换算系数（Hz / LSB），待协议确认
};

struct PowerConfig {
	ItemCommonConfig common;
	double maxPowerUA = 100.0;        // 平均电流 <= 阈值 -> PASS
};

// 通信参数（所有工位共用）
struct CommConfig {
	TestMode testMode      = TestMode::ModeA;	// 测试模式选择
	int cmdTimeoutMs       = 200;     // 普通指令应答超时
	int heartbeatTimeoutMs = 500;     // 普通指令应答超时
	int connCheckTimeoutMs = 600;     // REQ_CONN_CHECK_DUT 应答超时
	int pollIntervalMs     = 100;     // 业务状态轮询间隔
	int baudRate           = 115200;  // UART 波特率
	int usbVid             = 0x0000;  // USB 测试板 VID（枚举 WinUSB 设备用）
	int usbPid             = 0x0000;  // USB 测试板 PID
};

// 写入产测标志（3 项测试全部 PASS 后执行）：
// RESET_DUT（重试）→ CONN_CHECK_DUT（不重试）→ SET_FT_FLAG（重试）→ 延时 → GET_FT_FLAG 读回 == 1
struct FtFlagConfig {
	int cmdRetryCount = 3;     // RESET_DUT / SET_FT_FLAG 失败后的重试次数
	int readDelayMs   = 500;   // 写入成功后延时多久读回
};

struct TestConfig {
	RfPacketConfig   rfPacket;
	FreqOffsetConfig freqOffset;
	PowerConfig      power;
	CommConfig       comm;
	FtFlagConfig     ftFlag;
};

constexpr int kMaxItemValues = 3;

struct ItemResult {
	ItemState state = ItemState::Pending;
	int    valueCount = 0;            // 有效测量值个数，ABORT 时为 0
	std::array<double, kMaxItemValues> values{};  // 换算后的测量值
	                                  // RfPacket: {收包率%, 发包率%, RSSI dBm}；FreqOffset: {频偏KHz}；Power: {平均, 最大, 最小 uA}
	int    resultCode = -1;           // 测试板 result_code，-1 = 无（未执行 / 超时）
	int    attempts = 0;              // 已执行次数（含重试）
	std::string note;                 // 状态说明（ABORT 原因、等待资源等）
};

struct StationInfo {
	int         no = 0;               // 工位编号 1~6
	PortType    portType = PortType::Usb;   // 优先 USB
	std::string port;                 // 端口名：UART 为 COMx；USB 为设备实例 ID；模拟板为 SIMn
	BoardState   board = BoardState::Disconnected;
	DutState     dut   = DutState::Disconnected;
	std::string  boardNote;           // 测试板状态说明（固件/硬件版本、断开原因）
	std::string  dutNote;             // DUT 状态说明（握手失败原因）
	StationState state = StationState::Preparing;
	Verdict      lastVerdict = Verdict::None;
	std::array<ItemResult, kTestItemCount> items{};
	ItemResult   ftFlag;              // 写入产测标志步骤（values[0] = 读回的标志值）
};

// 需要持久化的用户配置（config.json）；运行时状态不在此列
struct StationSetting {
	int         no = 0;
	PortType    portType = PortType::Usb;
	std::string port;
};

// 本次运行的统计（不持久化）
struct StationStats {
	int no = 0;
	int tested = 0, pass = 0, fail = 0, abort = 0;
};

struct StatsSnapshot {
	std::vector<StationStats> stations;   // 按工位编号升序
	std::string resultFile;               // 当前 CSV 结果文件路径
};

struct PersistentConfig {
	TestConfig                  tests;
	std::vector<StationSetting> stations;
};

struct PortEntry {
	PortType    type = PortType::Uart;
	std::string name;                 // COMx / USB 设备实例 ID / SIMn
	std::string label;                // 简短显示名（USB 为序列号或实例 ID 最后一段）
	std::string description;
	bool        usable = true;        // USB 未绑定 WinUSB 驱动时为 false
};

enum class LogLevel : uint8_t { Info, Warn, Error };

struct LogEntry {
	uint64_t    seq = 0;              // 单调递增，用于 ViewModel 增量拉取
	long long   timeMs = 0;
	LogLevel    level = LogLevel::Info;
	std::string text;
};

// 业务线程 -> UI 的弹窗事件
enum class AlertLevel : uint8_t { Info, Warning, Error };
enum class AlertKind  : uint8_t {
	Notify,   // 仅“确定”，不需要回传
	Confirm,  // “确定/取消”，结果通过 ITesterService::replyAlert 回传业务线程
};

struct AlertEvent {
	uint64_t    id = 0;
	AlertLevel  level = AlertLevel::Info;
	AlertKind   kind  = AlertKind::Notify;
	std::string title;
	std::string message;
};

} // namespace Lx::Tester
