#include "Model.h"

#include "utils/INIReader.h"
#include <fstream>
#include <chrono>

namespace Lx::Rec::Model {

bool DataModel::loadFromIni(const std::string& path) {
	try {
		INIReader reader(path);
		if (reader.ParseError() != 0) return false;
		// speaker
		SpeakerInfo s;
		s.id = static_cast<uint32_t>(reader.GetUnsigned("Speaker", "id", s.id));
		s.age = static_cast<uint32_t>(reader.GetUnsigned("Speaker", "age", s.age));
		s.gender = reader.GetBoolean("Speaker", "gender", s.gender);
		s.projectID = reader.Get("Speaker", "projectID", s.projectID);
		s.hometown = reader.Get("Speaker", "hometown", s.hometown);
		setSpeakerInfo(s);

		// ports count
		int portCount = static_cast<int>(reader.GetUnsigned("Ports", "count", 0));
		std::vector<PortInfo> ports;
		for (int i = 0; i < portCount; ++i) {
			PortInfo p;
			std::string sec = "Port" + std::to_string(i);
			// do not load enable from config; runtime-only
			p.enable = false;
			p.portName = reader.Get(sec, "portName", p.portName);
			p.baud = static_cast<uint32_t>(reader.GetUnsigned(sec, "baud", p.baud));
			p.distance = static_cast<uint32_t>(reader.GetUnsigned(sec, "distance", p.distance));
			p.noiseLevel = reader.Get(sec, "noiseLevel", p.noiseLevel);
			// do not load isOpen from config; runtime-only
			p.isOpen = false;
			ports.push_back(p);
		}
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_ports = std::move(ports);
		}

		// words (simple indexed sections)
		int wordCount = static_cast<int>(reader.GetUnsigned("Words", "count", 0));
		std::vector<WordsInfo> words;
		for (int i = 0; i < wordCount; ++i) {
			std::string sec = "Word" + std::to_string(i);
			WordsInfo w;
			w.id = static_cast<uint32_t>(reader.GetUnsigned(sec, "id", w.id));
			w.flag = static_cast<uint32_t>(reader.GetUnsigned(sec, "flag", w.flag));
			w.text = reader.Get(sec, "text", w.text);
			words.push_back(w);
		}
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_wordsList = std::move(words);
		}

		return true;
	} catch(...) {
		return false;
	}
}

bool DataModel::saveToIni(const std::string& path) const {
	try {
		std::ofstream ofs(path, std::ios::out | std::ios::trunc);
		if (!ofs) return false;
		// speaker
		auto s = getSpeakerInfo();
		ofs << "[Speaker]\n";
		ofs << "id=" << s.id << "\n";
		ofs << "age=" << s.age << "\n";
		ofs << "gender=" << s.gender << "\n";
		ofs << "projectID=" << s.projectID << "\n";
		ofs << "hometown=" << s.hometown << "\n";

		// ports
		auto ports = getPortList();
		ofs << "[Ports]\n";
		ofs << "count=" << ports.size() << "\n";
		for (size_t i = 0; i < ports.size(); ++i) {
			ofs << "[Port" << i << "]\n";
			// do not persist enable (runtime-only)
			ofs << "portName=" << ports[i].portName << "\n";
			ofs << "baud=" << ports[i].baud << "\n";
			ofs << "distance=" << static_cast<unsigned int>(ports[i].distance) << "\n";
			ofs << "noiseLevel=" << ports[i].noiseLevel << "\n";
			// do not persist isOpen (runtime-only)
		}

		// words
		auto words = getWordsList();
		ofs << "[Words]\n";
		ofs << "count=" << words.size() << "\n";
		for (size_t i = 0; i < words.size(); ++i) {
			ofs << "[Word" << i << "]\n";
			ofs << "id=" << words[i].id << "\n";
			ofs << "flag=" << words[i].flag << "\n";
			ofs << "text=" << words[i].text << "\n";
		}

		ofs.close();
		return true;
	} catch(...) {
		return false;
	}
}

} // namespace Lx::Rec::Model

