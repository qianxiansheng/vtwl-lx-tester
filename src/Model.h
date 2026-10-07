#pragma once
#include <iostream>
#include <sstream>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>
#include "utils/thread_pool.h"
#include "ui_main.h"
#include "Domain.h"
#include "../BinFileWriter.h"

namespace Lx::Rec::Model {

struct SpeakerInfo {
	uint32_t id = 9300;
	uint32_t age = 18;
	bool gender = 0;
	std::string projectID = "TestProject";
	std::string hometown = "湖南";
};

struct WordsInfo {
	uint32_t id = 2401;
	uint32_t flag = 0;
	std::string text = "你好小牛";
};

struct PortInfo {
	bool enable = false;
	std::string portName = "COM0";
	uint32_t baud = 9600;
	uint8_t distance = 100;
	std::string noiseLevel = "low";
	bool isOpen = false;
};

class IDataModel {
public:
	virtual ~IDataModel() = default;
	virtual void setSpeakerInfo(const SpeakerInfo& info) = 0;
	virtual SpeakerInfo getSpeakerInfo() const = 0;
	virtual void updateWordsList(const std::vector<WordsInfo>& wordsList) = 0;
	virtual std::vector<WordsInfo> getWordsList() const = 0;
	virtual void setPortInfo(size_t idx, const PortInfo& p) = 0;
	virtual std::vector<PortInfo> getPortList() const = 0;
	// Consume and return accumulated dirty flags (bitmask) since last call.
	virtual uint32_t consumeDirtyFlags() = 0;
	// persistence
	virtual bool loadFromIni(const std::string& path) = 0;
	virtual bool saveToIni(const std::string& path) const = 0;
};

class DataModel : public IDataModel {
public:
	enum DirtyFlags : uint32_t {
		Dirty_None    = 0,
		Dirty_Speaker = 1 << 0,
		Dirty_Ports   = 1 << 1,
		Dirty_Words   = 1 << 2,
		Dirty_All     = Dirty_Speaker | Dirty_Ports | Dirty_Words,
	};

	void setSpeakerInfo(const SpeakerInfo& info) override {
		std::lock_guard<std::mutex> lk(m_mutex);
		m_speakerInfo = info;
		m_dirtyFlags.fetch_or(Dirty_Speaker);
	}

	SpeakerInfo getSpeakerInfo() const override {
		std::lock_guard<std::mutex> lk(m_mutex);
		return m_speakerInfo;
	}
	void updateWordsList(const std::vector<WordsInfo>& wordsList) override {
		std::lock_guard<std::mutex> lk(m_mutex);
		m_wordsList = wordsList;
		m_dirtyFlags.fetch_or(Dirty_Words);
	}
	std::vector<WordsInfo> getWordsList() const override {
		std::lock_guard<std::mutex> lk(m_mutex);
		return m_wordsList;
	}
	void setPortInfo(size_t idx, const PortInfo& p) override {
		std::lock_guard<std::mutex> lk(m_mutex);
		if (idx >= m_ports.size()) m_ports.resize(idx+1);
		m_ports[idx] = p;
		m_dirtyFlags.fetch_or(Dirty_Ports);
	}
	std::vector<PortInfo> getPortList() const override {
		std::lock_guard<std::mutex> lk(m_mutex);
		return m_ports;
	}
	uint32_t consumeDirtyFlags() override {
		return m_dirtyFlags.exchange(0);
	}
	// persistence
	bool loadFromIni(const std::string& path) override;
	bool saveToIni(const std::string& path) const override;

private:
	mutable std::mutex m_mutex;
	std::vector<WordsInfo> m_wordsList;
	std::vector<PortInfo> m_ports;
	SpeakerInfo m_speakerInfo;
	std::atomic<uint32_t> m_dirtyFlags{Dirty_None};
};

class BusinessThread {
public:
	using Task = std::function<void(IDataModel&)>;

	// uiUpdateWithFlags will be called on UI thread with dirty flags telling which parts changed.
	BusinessThread(std::shared_ptr<IDataModel> model,
				   size_t poolSize = std::thread::hardware_concurrency(),
				   std::function<void(uint32_t)> uiUpdateWithFlags = nullptr)
		: m_model(model), m_pool(std::make_unique<ThreadPool>(poolSize)), m_uiUpdateWithFlags(std::move(uiUpdateWithFlags))
	{
		running.store(true);
		// load model from default ini on startup
		try { m_model->loadFromIni("config.ini"); } catch(...) {}
		m_thread = std::thread(&BusinessThread::threadLoop, this);
		// start autosave thread
		// start autosave thread - use condition_variable to allow quick shutdown
		saverRunning.store(true);
		saverThread = std::thread([this]() {
			const std::chrono::seconds interval(5);
			std::unique_lock<std::mutex> lk(saverMutex);
			while (saverRunning.load()) {
				// wait_for will return early if notified (on shutdown)
				if (saverCv.wait_for(lk, interval, [this](){ return !saverRunning.load(); })) {
					break; // saverRunning became false
				}
				// unlock while performing save to avoid blocking stop notifier
				lk.unlock();
				try { m_model->saveToIni("config.ini"); } catch(...) {}
				lk.lock();
			}
		});
	}


	// Start/stop all recording (implemented after class definition)
	void startRecordingAll(uint32_t wordIndex, std::string suffix);
	void stopRecordingAll();
	void startMusicAll(int music_index);
	void stopMusicAll();

	~BusinessThread() {
		stop();
	}

	void stop() {
		stopRecordingAll();
		bool expected = true;
		if (running.compare_exchange_strong(expected, false)) {
			queueCv.notify_all();
			if (m_thread.joinable()) m_thread.join();
		}
		// stop saver
		bool se = true;
		if (saverRunning.compare_exchange_strong(se, false)) {
			saverCv.notify_all();
			if (saverThread.joinable()) saverThread.join();
		}
	}

	// Post a task from UI thread -> executed on business thread
	void postFromUI(Task t) {
		{
			std::lock_guard<std::mutex> lk(queueMutex);
			tasks.push(std::move(t));
		}
		queueCv.notify_one();
	}

	// Post a task from worker thread (async completion) -> business thread
	void postFromWorker(Task t) { postFromUI(std::move(t)); }

	// Run heavy task in thread pool; onComplete will be posted back to business thread
	template<typename F, typename OnComplete>
	void runAsync(F heavyTask, OnComplete onComplete) {
		auto pool = m_pool.get();
		pool->enqueue([this, heavyTask = std::move(heavyTask), onComplete = std::move(onComplete)]() mutable {
			auto result = heavyTask();
			this->postFromWorker([onComplete = std::move(onComplete), result = std::move(result)](IDataModel& model) mutable {
				onComplete(model, result);
			});
		});
	}

	bool runningStatus() const { return running.load(); }

	// Toggle recording for a port (creates/closes BinFileWriter and sends start/stop commands)
	void toggleRecording(size_t idx, uint32_t wordIndex, bool enable) {
		postFromUI([this, idx, wordIndex, enable](IDataModel& model) {
			if (idx >= m_serials.size()) return;
			auto ports = model.getPortList();
			if (idx >= ports.size()) return;
			auto p = ports[idx];
			// obtain speaker and words info for path
			auto s = model.getSpeakerInfo();
			auto words = model.getWordsList();

			if (enable) {
				// create path ./<distance>_<noise>/<Speaker.ID>/C<Speaker.ID><Words.ID>.bin
				std::ostringstream oss;
				oss << "./" << p.distance << "_" << p.noiseLevel << "/" << s.id << "/C" << s.id << words[wordIndex].id << ".bin";
				auto path = oss.str();
				// create writer
				std::shared_ptr<BinFileWriter> w = BinFileWriter::create(path, m_pool.get());
				{
					std::lock_guard<std::mutex> lk(m_writerMutex);
					if (idx >= m_writers.size()) m_writers.resize(idx+1);
					m_writers[idx] = w;
				}
				// send 'S'
				if (m_serials[idx]) {
					std::vector<uint8_t> cmd = { static_cast<uint8_t>('S') };
					m_serials[idx]->sendAsync(cmd);
				}
			} else {
				// send 'E' then close writer
				if (m_serials[idx]) {
					std::vector<uint8_t> cmd = { static_cast<uint8_t>('E') };
					m_serials[idx]->sendAsync(cmd);
				}
				std::shared_ptr<BinFileWriter> w;
				{
					std::lock_guard<std::mutex> lk(m_writerMutex);
					if (idx < m_writers.size()) { w = m_writers[idx]; m_writers[idx].reset(); }
				}
				if (w) w->close();
			}
		});
	}

	// Open serial port asynchronously and save handle inside business thread
	void openSerial(size_t idx, PortInfo p) {
		postFromUI([this, idx, p](IDataModel& model) {
			// ensure serials vector
			if (idx >= m_serials.size()) m_serials.resize(idx+1);
			// close existing if any
			if (m_serials[idx]) {
				try { m_serials[idx]->close(); } catch(...) {}
				m_serials[idx].reset();
			}

			// create config
			Lx::Domain::SerialConfig cfg;
			cfg.port = p.portName;
			cfg.baudrate = p.baud;
			// attempt async open
			try {
				auto sp = std::make_unique<Lx::Domain::SerialPortLibSerial>();
				bool ok = sp->openAsync(cfg);
				if (ok) {
					// forward received data: dispatch to UI (log) and also to any active bin writer
					sp->setDataCallback([this, idx](const std::vector<uint8_t>& data){
						// post a small UI log
						PostToUI([idx, data]() {
							std::cerr << "Serial[" << idx << "] recv " << data.size() << " bytes\n";
						});
						// forward to writer if present
						std::shared_ptr<BinFileWriter> w;
						{
							std::lock_guard<std::mutex> lk(m_writerMutex);
							if (idx < m_writers.size()) w = m_writers[idx];
						}
						if (w) {
							// copy data into vector and push
							w->write(data);
						}
					});
					m_serials[idx] = std::move(sp);
					PortInfo pi = p; pi.isOpen = true; pi.enable = false;
					model.setPortInfo(idx, pi);
					stopRecordingAll();
				} else {
					PortInfo pi = p; pi.isOpen = false; pi.enable = false;
					model.setPortInfo(idx, pi);
				}
			} catch(...) {
				PortInfo pi = p; pi.isOpen = false; pi.enable = false;
				model.setPortInfo(idx, pi);
			}
		});
	}

	void closeSerial(size_t idx) {
		postFromUI([this, idx](IDataModel& model) {
			if (idx < m_serials.size() && m_serials[idx]) {
				try { m_serials[idx]->close(); } catch(...) {}
				m_serials[idx].reset();
			}
			// update model
			auto ports = model.getPortList();
			if (idx >= ports.size()) ports.resize(idx+1);
			ports[idx].isOpen = false;
			model.updateWordsList(model.getWordsList()); // trick to trigger UI update: keep words but post UI
			model.setPortInfo(idx, ports[idx]);
		});
	}

private:
	void threadLoop() {
		while (running.load()) {
			Task t;
			{
				std::unique_lock<std::mutex> lk(queueMutex);
				queueCv.wait(lk, [this] { return !tasks.empty() || !running.load(); });
				if (!running.load() && tasks.empty()) break;
				if (!tasks.empty()) { t = std::move(tasks.front()); tasks.pop(); }
			}
			if (t) {
				try { t(*m_model); } catch (...) {}
				// notify UI to refresh with dirty flags
				if (m_uiUpdateWithFlags) {
					// consume flags from model
					try {
						uint32_t flags = m_model->consumeDirtyFlags();
						if (flags != 0) {
							PostToUI([this, flags]() { m_uiUpdateWithFlags(flags); });
						}
					} catch(...) {
						// on error, still post empty to keep UI responsive
						PostToUI([](){});
					}
				} else {
					PostToUI([](){});
				}
			}
		}
	}

	std::shared_ptr<IDataModel> m_model;
	std::unique_ptr<ThreadPool> m_pool;
	std::thread m_thread;
	std::mutex queueMutex;
	std::condition_variable queueCv;
	std::queue<Task> tasks;
	std::atomic<bool> running{false};
	std::function<void()> m_uiUpdate;
	std::function<void(uint32_t)> m_uiUpdateWithFlags;
	std::vector<std::unique_ptr<Lx::Domain::ISerialPort>> m_serials;
	std::vector<std::shared_ptr<BinFileWriter>> m_writers;
	std::mutex m_writerMutex;
	std::atomic<bool> saverRunning{false};
	std::thread saverThread;
	std::condition_variable saverCv;
	std::mutex saverMutex;
};
// Out-of-class definitions for start/stop all recording
inline void BusinessThread::startRecordingAll(uint32_t wordIndex, std::string suffix) {
	postFromUI([this, wordIndex, suffix](IDataModel& model) {
		auto ports = model.getPortList();
		auto s = model.getSpeakerInfo();
		auto words = model.getWordsList();
		for (size_t idx = 0; idx < ports.size(); ++idx) {
			const auto& p = ports[idx];
			if (!p.enable) continue;
			{
				std::lock_guard<std::mutex> lk(m_writerMutex);
				if (idx >= m_writers.size()) m_writers.resize(idx + 1);
			}
			std::ostringstream oss;
			oss << "./" << s.projectID << "/" << static_cast<int>(p.distance) << "_" << p.noiseLevel << "/" << s.id << "/C" << static_cast<int>(p.distance) << s.id << words[wordIndex].id << suffix << ".bin";
			auto path = oss.str();
			std::shared_ptr<BinFileWriter> w = BinFileWriter::create(path, m_pool.get());
			{
				std::lock_guard<std::mutex> lk(m_writerMutex);
				m_writers[idx] = w;
			}
			if (idx < m_serials.size() && m_serials[idx]) {
				std::vector<uint8_t> cmd = { static_cast<uint8_t>('S') };
				m_serials[idx]->sendAsync(cmd);
			}
			PortInfo pi = p; pi.isOpen = true;
			model.setPortInfo(idx, pi);
		}
	});
}

inline void BusinessThread::stopRecordingAll() {
	postFromUI([this](IDataModel& model) {
		auto ports = model.getPortList();
		auto words = model.getWordsList();
		for (size_t idx = 0; idx < ports.size(); ++idx) {
			const auto& p = ports[idx];
			if (idx < m_serials.size() && m_serials[idx]) {
				std::vector<uint8_t> cmd = { static_cast<uint8_t>('E') };
				m_serials[idx]->sendAsync(cmd);
			}
			std::shared_ptr<BinFileWriter> w;
			{
				std::lock_guard<std::mutex> lk(m_writerMutex);
				if (idx < m_writers.size()) { w = m_writers[idx]; m_writers[idx].reset(); }
			}
			if (w) w->close();
		}
	});
}

inline void BusinessThread::startMusicAll(int music_index) {
	postFromUI([this, music_index](IDataModel& model) {
		auto ports = model.getPortList();
		for (size_t idx = 0; idx < ports.size(); ++idx) {
			const auto& p = ports[idx];
			if (idx < m_serials.size() && m_serials[idx]) {
				std::vector<uint8_t> cmd = { 
					static_cast<uint8_t>('P'),
					static_cast<uint8_t>(music_index) 
				};
				m_serials[idx]->sendAsync(cmd);
			}
		}
		});
}

inline void BusinessThread::stopMusicAll() {
	postFromUI([this](IDataModel& model) {
		auto ports = model.getPortList();
		for (size_t idx = 0; idx < ports.size(); ++idx) {
			const auto& p = ports[idx];
			if (idx < m_serials.size() && m_serials[idx]) {
				std::vector<uint8_t> cmd = {
					static_cast<uint8_t>('T')
				};
				m_serials[idx]->sendAsync(cmd);
			}
		}
		});
}

} // namespace Lx::Rec::Model


