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

// DUT 连接状态（工位指示灯）
enum class DutState : uint8_t { Disconnected, Connecting, Ready, Error };

// 工位本地业务状态
enum class StationState : uint8_t { Preparing, Ready, Testing, Finished };

// 单个测试项状态
enum class ItemState : uint8_t { Pending, Running, Pass, Fail, Abort };

// 工位一轮综合结果
enum class Verdict : uint8_t { None, Pass, Fail, Abort };

enum class TestItem : uint8_t { RfPacket = 0, FreqOffset = 1, Power = 2 };

// 每个测试类别都由「公共部分」+「专用部分」组成
struct ItemCommonConfig {
	int retryCount = 0;               // 失败重试次数 0~5
};

struct RfPacketConfig {
	ItemCommonConfig common;
	double minRxRatePct = 95.0;       // 收包率 >= 阈值 -> PASS
	double minTxRatePct = 95.0;       // 发包率 >= 阈值 -> PASS
};

struct FreqOffsetConfig {
	ItemCommonConfig common;
	double maxAbsOffsetKHz = 50.0;    // |频偏| <= 阈值 -> PASS
};

struct PowerConfig {
	ItemCommonConfig common;
	double maxPowerUA = 100.0;        // 功耗 <= 阈值 -> PASS
};

struct TestConfig {
	RfPacketConfig   rfPacket;
	FreqOffsetConfig freqOffset;
	PowerConfig      power;
};

constexpr int kMaxItemValues = 2;

struct ItemResult {
	ItemState state = ItemState::Pending;
	int    valueCount = 0;            // 有效测量值个数，ABORT 时为 0
	std::array<double, kMaxItemValues> values{};  // 测试板上报的原始测量值
	                                  // RfPacket: {收包率%, 发包率%}；FreqOffset: {频偏KHz}；Power: {功耗uA}
};

struct StationInfo {
	int         no = 0;               // 工位编号 1~6
	std::string port;                 // 选中的端口名，空 = 未选择
	DutState     dut   = DutState::Disconnected;
	StationState state = StationState::Preparing;
	Verdict      lastVerdict = Verdict::None;
	std::array<ItemResult, kTestItemCount> items{};
};

// 需要持久化的用户配置（config.json）；运行时状态不在此列
struct StationSetting {
	int         no = 0;
	std::string port;
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
