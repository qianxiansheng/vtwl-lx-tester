#pragma once

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <fstream>
#include "utils/thread_pool.h"

class BinFileWriter : public std::enable_shared_from_this<BinFileWriter> {
public:
	// Create a writer that will run its worker on given thread pool
	static std::shared_ptr<BinFileWriter> create(const std::string& path, ThreadPool* pool);

	// Push data to be written (thread-safe)
	void write(std::vector<uint8_t> data);

	// Close writer and wait for worker to finish flushing
	void close();

	~BinFileWriter();

private:
	BinFileWriter(const std::string& path, ThreadPool* pool);
	void workerLoop();

	std::string path_;
	ThreadPool* pool_ = nullptr;
	std::queue<std::vector<uint8_t>> queue_;
	std::mutex mutex_;
	std::condition_variable cv_;
	std::atomic<bool> closing_{false};
	std::atomic<bool> closed_{false};
	std::future<void> workerFuture_;
};

// Implementation
#include <functional>
#include <future>
#include <filesystem>

inline std::shared_ptr<BinFileWriter> BinFileWriter::create(const std::string& path, ThreadPool* pool) {
	auto p = std::shared_ptr<BinFileWriter>(new BinFileWriter(path, pool));
	// schedule worker on pool
	// For simplicity and to avoid template deduction issues with the project's ThreadPool overloads,
	// run worker on a separate async task. This keeps lifetime tied to the shared_ptr 'p'.
	p->workerFuture_ = std::async(std::launch::async, [p]() { p->workerLoop(); });
	return p;
}

inline BinFileWriter::BinFileWriter(const std::string& path, ThreadPool* pool)
	: path_(path), pool_(pool) {
	// ensure directory exists
	try {
		auto dir = std::filesystem::path(path_).parent_path();
		if (!dir.empty()) std::filesystem::create_directories(dir);
	} catch(...) {}
}

inline BinFileWriter::~BinFileWriter() {
	close();
}

inline void BinFileWriter::write(std::vector<uint8_t> data) {
	if (closing_.load()) return;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		queue_.push(std::move(data));
	}
	cv_.notify_one();
}

inline void BinFileWriter::close() {
	bool expected = false;
	if (closing_.compare_exchange_strong(expected, true)) {
		cv_.notify_all();
		if (workerFuture_.valid()) {
			try { workerFuture_.wait(); } catch(...) {}
		}
		closed_.store(true);
	}
}

inline void BinFileWriter::workerLoop() {
	std::ofstream ofs;
	try {
		ofs.open(path_, std::ios::binary | std::ios::out);
	} catch(...) {}
	while (!closing_.load()) {
		std::vector<uint8_t> item;
		{
			std::unique_lock<std::mutex> lk(mutex_);
			if (queue_.empty()) {
				cv_.wait_for(lk, std::chrono::milliseconds(100));
			}
			if (!queue_.empty()) {
				item = std::move(queue_.front());
				queue_.pop();
			}
		}
		if (!item.empty() && ofs.is_open()) {
			try { ofs.write(reinterpret_cast<const char*>(item.data()), static_cast<std::streamsize>(item.size())); } catch(...) {}
		}
	}
	// flush remaining
	while (true) {
		std::vector<uint8_t> item;
		{
			std::lock_guard<std::mutex> lk(mutex_);
			if (queue_.empty()) break;
			item = std::move(queue_.front()); queue_.pop();
		}
		if (!item.empty() && ofs.is_open()) {
			try { ofs.write(reinterpret_cast<const char*>(item.data()), static_cast<std::streamsize>(item.size())); } catch(...) {}
		}
	}
	if (ofs.is_open()) ofs.close();
}

