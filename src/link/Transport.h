#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Lx::Link {

// 字节流传输（串口 / 模拟测试板 / 以后的 WinUSB）。
// 回调在传输层自己的 IO 线程上触发，使用方负责切回业务线程。
class ITransport {
public:
	using DataHandler   = std::function<void(std::vector<uint8_t> data)>;
	using ClosedHandler = std::function<void(std::string reason)>;   // 异常断开（物理断开等），最多触发一次

	virtual ~ITransport() = default;

	virtual bool open(DataHandler onData, ClosedHandler onClosed, std::string& err) = 0;
	// 幂等；返回后不会再触发任何回调
	virtual void close() = 0;
	virtual bool send(const std::vector<uint8_t>& data) = 0;
	virtual const std::string& name() const = 0;
};

std::unique_ptr<ITransport> makeSerialTransport(const std::string& port, uint32_t baudrate);

// ---------------------------------------------------------------- USB（WinUSB）
// 端口名 = 设备实例 ID（如 "USB\VID_1234&PID_5678\SN0001"），同一块测试板（同序列号或同一物理 USB 口）保持不变。
struct UsbDeviceInfo {
	std::string instanceId;    // 端口名
	std::string label;         // 简短显示名（序列号 / 实例 ID 最后一段）
	std::string description;   // 设备名称、位置、驱动
	bool        winusb = false; // 是否已绑定 WinUSB 驱动（否则无法打开）
};

// 枚举当前在位、VID/PID 匹配的 USB 设备（复合设备的父节点不列出，只列各接口）
std::vector<UsbDeviceInfo> enumerateUsbDevices(uint16_t vid, uint16_t pid);

// 简短显示名：实例 ID 最后一段
std::string usbShortName(const std::string& instanceId);

std::unique_ptr<ITransport> makeUsbTransport(const std::string& instanceId);

// 模拟测试板：端口名形如 "SIM1"~"SIM6"
constexpr const char* kSimPortPrefix = "SIM";
bool isSimPort(const std::string& port);
std::unique_ptr<ITransport> makeSimTransport(const std::string& port);

} // namespace Lx::Link
