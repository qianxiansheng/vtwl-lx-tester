#pragma once
#include <cstdint>

// 模拟测试板的全局设置（调试菜单修改，模拟板线程读取）。线程安全。
namespace Lx::Link {

struct SimSettings {
	enum class Measure : uint8_t {
		AllPass,     // 全部合格
		Random,      // 随机（大部分合格，偶尔不合格）
		RfFail,      // 收发包不合格（收包率低）
		CfoFail,     // 频偏不合格（原始值 32767）
		PowerFail,   // 功耗不合格
	};

	bool    portsEnabled = false;   // 端口列表中显示 SIM1~SIM6
	int     target = 0;             // 以下故障注入的作用对象：0 = 全部，n = 仅 SIMn
	Measure measure = Measure::AllPass;
	uint8_t businessRc = 0;         // 业务结果 result_code 注入（0 = 正常）
	bool    dutAbsent = false;      // DUT 不在位：CONN_CHECK_DUT 返回 DUT 无应答
	bool    noResponse = false;     // 测试板不再应答任何指令（模拟断线）
	bool    hangTask = false;       // 业务启动后永不结束（触发上位机业务总超时）

	bool appliesTo(int simIndex) const { return target == 0 || target == simIndex; }

	static SimSettings get();
	static void set(const SimSettings& s);
};

} // namespace Lx::Link
