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

// 模拟测试板：端口名形如 "SIM1"~"SIM6"
constexpr const char* kSimPortPrefix = "SIM";
bool isSimPort(const std::string& port);
std::unique_ptr<ITransport> makeSimTransport(const std::string& port);

} // namespace Lx::Link
