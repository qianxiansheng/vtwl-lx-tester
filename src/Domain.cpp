#include "Domain.h"

namespace Lx::Domain {

SerialPortLibSerial::SerialPortLibSerial() {}

SerialPortLibSerial::~SerialPortLibSerial() {
  close();
}

bool SerialPortLibSerial::openSync(const SerialConfig& cfg) {
  close();
  cfg_ = cfg;
  try {
	ser_.reset(new serial::Serial(cfg_.port, cfg_.baudrate, cfg_.timeout,
								  cfg_.bytesize, cfg_.parity, cfg_.stopbits,
								  cfg_.flowcontrol));
	return ser_->isOpen();
  } catch (...) {
	ser_.reset();
	return false;
  }
}

bool SerialPortLibSerial::openAsync(const SerialConfig& cfg) {
  if (!openSync(cfg)) return false;
  running_.store(true);
  worker_ = std::thread(&SerialPortLibSerial::workerThreadLoop, this);
  return true;
}

void SerialPortLibSerial::close() {
  running_.store(false);
  queueCv_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (ser_) {
	try { ser_->close(); } catch (...) {}
	ser_.reset();
  }
  // clear queue
  std::lock_guard<std::mutex> lk(queueMutex_);
  while (!sendQueue_.empty()) sendQueue_.pop();
}

bool SerialPortLibSerial::setBaudrate(uint32_t baudrate) {
  if (!ser_) return false;
  try {
	ser_->setBaudrate(baudrate);
	cfg_.baudrate = baudrate;
	return true;
  } catch (...) {
	return false;
  }
}

SerialConfig SerialPortLibSerial::getConfig() const { return cfg_; }

size_t SerialPortLibSerial::sendSync(const std::vector<uint8_t>& data) {
  if (!ser_) return 0;
  try {
	return ser_->write(data);
  } catch (...) {
	return 0;
  }
}

size_t SerialPortLibSerial::recvSync(std::vector<uint8_t>& out, size_t size) {
  if (!ser_) return 0;
  out.resize(size);
  try {
	size_t n = ser_->read(out, size);
	out.resize(n);
	return n;
  } catch (...) {
	out.clear();
	return 0;
  }
}

void SerialPortLibSerial::sendAsync(const std::vector<uint8_t>& data) {
  if (!ser_) return;
  {
	std::lock_guard<std::mutex> lk(queueMutex_);
	sendQueue_.push(data);
  }
  queueCv_.notify_one();
}

void SerialPortLibSerial::setDataCallback(DataCallback cb) { dataCb_ = std::move(cb); }

bool SerialPortLibSerial::isOpen() const { return ser_ && ser_->isOpen(); }

void SerialPortLibSerial::workerThreadLoop() {
  // Thread will process send queue and poll for incoming data
  while (running_.load()) {
	// process outgoing
	std::vector<uint8_t> toSend;
	{
	  std::unique_lock<std::mutex> lk(queueMutex_);
	  if (sendQueue_.empty()) {
		// wait briefly or until new data
		queueCv_.wait_for(lk, std::chrono::milliseconds(20));
	  }
	  if (!sendQueue_.empty()) {
		toSend = std::move(sendQueue_.front());
		sendQueue_.pop();
	  }
	}

	if (!toSend.empty() && ser_) {
	  try { ser_->write(toSend); } catch (...) {}
	}

	// poll for incoming data
	if (ser_ && dataCb_) {
	  try {
		size_t avail = ser_->available();
		if (avail > 0) {
		  std::vector<uint8_t> buf(avail);
		  size_t n = ser_->read(buf.data(), avail);
		  if (n > 0) {
			buf.resize(n);
			// invoke callback
			try { dataCb_(buf); } catch (...) {}
		  }
		}
	  } catch (...) {
		// ignore
	  }
	}

	// sleep a little to avoid busy loop
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

} // namespace Lx::Domain
