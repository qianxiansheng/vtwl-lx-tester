#include "Transport.h"
#include <atomic>
#include <mutex>
#include <thread>
#include "serial/serial.h"

namespace Lx::Link {

namespace {

// 基于 wjwwood serial：读线程带超时阻塞读取，写在调用线程完成（库内部读写锁分离）。
// 读写抛异常（如 USB 拔出）视为物理断开。
class SerialTransport : public ITransport {
public:
	SerialTransport(std::string port, uint32_t baud) : m_name(std::move(port)), m_baud(baud) {}
	~SerialTransport() override { close(); }

	bool open(DataHandler onData, ClosedHandler onClosed, std::string& err) override
	{
		close();
		try {
			m_serial = std::make_unique<serial::Serial>(m_name, m_baud, serial::Timeout::simpleTimeout(20));
			if (!m_serial->isOpen()) {
				err = "端口未能打开";
				m_serial.reset();
				return false;
			}
		}
		catch (const std::exception& e) {
			err = e.what();
			m_serial.reset();
			return false;
		}
		m_onData = std::move(onData);
		m_onClosed = std::move(onClosed);
		m_running = true;
		m_reader = std::thread(&SerialTransport::readLoop, this);
		return true;
	}

	void close() override
	{
		m_running = false;
		if (m_reader.joinable()) m_reader.join();
		std::lock_guard<std::mutex> lk(m_writeMutex);
		if (m_serial) {
			try { m_serial->close(); } catch (...) {}
			m_serial.reset();
		}
	}

	bool send(const std::vector<uint8_t>& data) override
	{
		std::lock_guard<std::mutex> lk(m_writeMutex);
		if (!m_serial || !m_running) return false;
		try {
			return m_serial->write(data) == data.size();
		}
		catch (...) {
			return false;   // 读线程会检测到断开并上报
		}
	}

	const std::string& name() const override { return m_name; }

private:
	void readLoop()
	{
		std::vector<uint8_t> buf(256);
		while (m_running) {
			try {
				size_t n = m_serial->read(buf.data(), buf.size());
				if (n > 0 && m_onData)
					m_onData(std::vector<uint8_t>(buf.begin(), buf.begin() + n));
			}
			catch (const std::exception& e) {
				if (m_running.exchange(false) && m_onClosed) m_onClosed(e.what());
				return;
			}
		}
	}

	std::string m_name;
	uint32_t    m_baud;
	std::unique_ptr<serial::Serial> m_serial;
	std::mutex  m_writeMutex;
	std::thread m_reader;
	std::atomic<bool> m_running{ false };
	DataHandler   m_onData;
	ClosedHandler m_onClosed;
};

} // namespace

std::unique_ptr<ITransport> makeSerialTransport(const std::string& port, uint32_t baudrate)
{
	return std::make_unique<SerialTransport>(port, baudrate);
}

} // namespace Lx::Link
