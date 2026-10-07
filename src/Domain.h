#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>
#include "serial/serial.h"

namespace Lx::Domain {

struct SerialConfig {
  std::string port;
  uint32_t baudrate = 115200;
  serial::bytesize_t bytesize = serial::eightbits;
  serial::parity_t parity = serial::parity_none;
  serial::stopbits_t stopbits = serial::stopbits_one;
  serial::flowcontrol_t flowcontrol = serial::flowcontrol_none;
  serial::Timeout timeout = serial::Timeout::simpleTimeout(100);
};

class ISerialPort {
public:
  using DataCallback = std::function<void(const std::vector<uint8_t>&)>;

  virtual ~ISerialPort() = default;

  // Open a synchronous handle (blocking read/write on caller thread)
  virtual bool openSync(const SerialConfig& cfg) = 0;

  // Open an asynchronous handle (internal worker thread handles IO)
  virtual bool openAsync(const SerialConfig& cfg) = 0;

  // Close any open handles and stop worker thread if running
  virtual void close() = 0;

  // Change baudrate on the open port. Returns true on success.
  virtual bool setBaudrate(uint32_t baudrate) = 0;

  // Get the current configuration
  virtual SerialConfig getConfig() const = 0;

  // Synchronous send/receive
  virtual size_t sendSync(const std::vector<uint8_t>& data) = 0;
  virtual size_t recvSync(std::vector<uint8_t>& out, size_t size) = 0;

  // Asynchronous send enqueues data to be sent by worker thread.
  virtual void sendAsync(const std::vector<uint8_t>& data) = 0;

  // Register a callback that will be invoked from worker thread when data arrives.
  virtual void setDataCallback(DataCallback cb) = 0;

  // Query
  virtual bool isOpen() const = 0;
};

// libserial (wjwwood) implementation
class SerialPortLibSerial : public ISerialPort {
public:
  SerialPortLibSerial();
  ~SerialPortLibSerial() override;

  bool openSync(const SerialConfig& cfg) override;
  bool openAsync(const SerialConfig& cfg) override;
  void close() override;
  bool setBaudrate(uint32_t baudrate) override;
  SerialConfig getConfig() const override;
  size_t sendSync(const std::vector<uint8_t>& data) override;
  size_t recvSync(std::vector<uint8_t>& out, size_t size) override;
  void sendAsync(const std::vector<uint8_t>& data) override;
  void setDataCallback(DataCallback cb) override;
  bool isOpen() const override;

private:
  void workerThreadLoop();

  SerialConfig cfg_;
  std::unique_ptr<serial::Serial> ser_;

  // Async worker
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::queue<std::vector<uint8_t>> sendQueue_;
  DataCallback dataCb_;
};

// Windows native placeholder (not implemented yet)
class SerialPortWin : public ISerialPort {
public:
  SerialPortWin() = default;
  ~SerialPortWin() override = default;
  bool openSync(const SerialConfig& /*cfg*/) override { return false; }
  bool openAsync(const SerialConfig& /*cfg*/) override { return false; }
  void close() override {}
  bool setBaudrate(uint32_t /*baudrate*/) override { return false; }
  SerialConfig getConfig() const override { return SerialConfig(); }
  size_t sendSync(const std::vector<uint8_t>& /*data*/) override { return 0; }
  size_t recvSync(std::vector<uint8_t>& /*out*/, size_t /*size*/) override { return 0; }
  void sendAsync(const std::vector<uint8_t>& /*data*/) override {}
  void setDataCallback(DataCallback /*cb*/) override {}
  bool isOpen() const override { return false; }
};

} // namespace Lx::Domain
