#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// 产测上位机的领域数据类型（Model / Service / ViewModel 共用，不依赖 ImGui）
namespace Lx::Tester {

constexpr int kMaxStations  = 6;
constexpr int kTestItemCount = 3;
constexpr int kMaxRetry      = 5;

// 全局主状态：准备中 -> 准备就绪 -> 测试进行中 -> 测试结束 -> 准备中
enum class GlobalState : uint8_t { Preparing, Ready, Testing, Finished };

// 测试板连接状态（上位机 - 测试板链路）
enum class BoardState : uint8_t { Disconnected, Connecting, Connected, Error };

// DUT 连接状态（测试板 - DUT 链路；Connecting = 握手中）
enum class DutState : uint8_t { Disconnected, Connecting, Ready, Error };

enum class PortType : uint8_t { Uart = 0, Usb = 1 };

// USB 端口预留（第一版未实现）
constexpr uint16_t kUsbVid = 0x0000;
constexpr uint16_t kUsbPid = 0x0000;

// 工位本地业务状态
enum class StationState : uint8_t { Preparing, Ready, Testing, Finished };

// 单个测试项状态
enum class ItemState : uint8_t { Pending, Running, Pass, Fail, Abort };

// 工位一轮综合结果
enum class Verdict : uint8_t { None, Pass, Fail, Abort };

enum class TestItem : uint8_t { RfPacket = 0, FreqOffset = 1, Power = 2 };

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
	int cmdTimeoutMs       = 200;     // 普通指令应答超时
	int connCheckTimeoutMs = 600;     // REQ_CONN_CHECK_DUT 应答超时
	int pollIntervalMs     = 100;     // 业务状态轮询间隔
	int baudRate           = 115200;  // UART 波特率
};

struct TestConfig {
	RfPacketConfig   rfPacket;
	FreqOffsetConfig freqOffset;
	PowerConfig      power;
	CommConfig       comm;
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
	PortType    portType = PortType::Uart;
	std::string port;                 // 选中的端口名，空 = 未选择
	BoardState   board = BoardState::Disconnected;
	DutState     dut   = DutState::Disconnected;
	std::string  boardNote;           // 测试板状态说明（固件/硬件版本、断开原因）
	std::string  dutNote;             // DUT 状态说明（握手失败原因）
	StationState state = StationState::Preparing;
	Verdict      lastVerdict = Verdict::None;
	std::array<ItemResult, kTestItemCount> items{};
};

// 需要持久化的用户配置（config.json）；运行时状态不在此列
struct StationSetting {
	int         no = 0;
	PortType    portType = PortType::Uart;
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
	std::string name;                 // COMx / WinUSB 设备路径
	std::string description;
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
