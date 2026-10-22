#include "pch.h"
#include "TesterView.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include "imgui.h"

namespace Lx::Tester::View {

namespace {

// 颜色规范（readme.md）
constexpr ImU32 kGray   = IM_COL32(0x88, 0x88, 0x88, 0xFF);
constexpr ImU32 kYellow = IM_COL32(0xFF, 0xCC, 0x00, 0xFF);
constexpr ImU32 kGreen  = IM_COL32(0x00, 0xBB, 0x44, 0xFF);
constexpr ImU32 kRed    = IM_COL32(0xDD, 0x22, 0x22, 0xFF);
constexpr ImU32 kOrange = IM_COL32(0xFF, 0x77, 0x00, 0xFF);
constexpr ImU32 kBlue   = IM_COL32(0x33, 0x88, 0xDD, 0xFF);   // 全局「测试结束」/ 普通强调按钮

// 工位卡片下半部分的行标签（左列），状态条（右列）统一对齐
const char* const kItemLabels[kTestItemCount] = { "无线收发包：", "频偏：", "功耗：" };
const char* const kBoardLabel   = "测试板：";
const char* const kDutLabel     = "DUT：";
const char* const kStateLabel   = "工位状态：";
const char* const kVerdictLabel = "本轮结果：";
const char* const kFtFlagLabel  = "产测标志：";

const char* const kPortTypeNames[] = { "UART", "USB" };

struct Chip {
	const char* text;
	ImU32       color;
};

Chip ChipOf(GlobalState s)
{
	switch (s) {
	case GlobalState::Preparing: return { "准备中",     kGray };
	case GlobalState::Ready:     return { "准备就绪",   kGreen };
	case GlobalState::Testing:   return { "测试进行中", kYellow };
	case GlobalState::Finished:  return { "测试结束",   kBlue };
	}
	return { "?", kGray };
}

Chip ChipOf(BoardState s)
{
	switch (s) {
	case BoardState::Disconnected: return { "未连接",   kGray };
	case BoardState::Connecting:   return { "连接中",   kYellow };
	case BoardState::Connected:    return { "已连接",   kGreen };
	case BoardState::Error:        return { "断开/异常", kRed };
	}
	return { "?", kGray };
}

Chip ChipOf(DutState s)
{
	switch (s) {
	case DutState::Disconnected: return { "未连接",   kGray };
	case DutState::Connecting:   return { "握手中",   kYellow };
	case DutState::Ready:        return { "DUT就绪",  kGreen };
	case DutState::Error:        return { "无应答/异常", kRed };
	}
	return { "?", kGray };
}

Chip ChipOf(ItemState s)
{
	switch (s) {
	case ItemState::Pending: return { "待执行", kGray };
	case ItemState::Running: return { "执行中", kYellow };
	case ItemState::Pass:    return { "PASS",   kGreen };
	case ItemState::Fail:    return { "FAIL",   kRed };
	case ItemState::Abort:   return { "ABORT",  kOrange };
	case ItemState::Skipped: return { "跳过",   kGray };
	}
	return { "?", kGray };
}

Chip ChipOf(Verdict v)
{
	switch (v) {
	case Verdict::None:  return { "--",    kGray };
	case Verdict::Pass:  return { "PASS",  kGreen };
	case Verdict::Fail:  return { "FAIL",  kRed };
	case Verdict::Abort: return { "ABORT", kOrange };
	}
	return { "?", kGray };
}

Chip ChipOf(AlertLevel l)
{
	switch (l) {
	case AlertLevel::Info:    return { "提示", kBlue };
	case AlertLevel::Warning: return { "警告", kOrange };
	case AlertLevel::Error:   return { "错误", kRed };
	}
	return { "?", kGray };
}

Chip ChipOf(StationState s)
{
	switch (s) {
	case StationState::Preparing: return { "准备中",     kGray };
	case StationState::Ready:     return { "准备就绪",   kGreen };
	case StationState::Testing:   return { "测试进行中", kYellow };
	case StationState::Finished:  return { "测试结束",   kBlue };
	}
	return { "?", kGray };
}

// 色块标签：size 为 0 的分量自动按文字/行高计算
void StatusChip(const char* text, ImU32 color, ImVec2 size = ImVec2(0, 0))
{
	const ImGuiStyle& st = ImGui::GetStyle();
	const ImVec2 ts = ImGui::CalcTextSize(text);
	if (size.x <= 0) size.x = ts.x + st.FramePadding.x * 2;
	if (size.y <= 0) size.y = ImGui::GetFrameHeight();

	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), color, st.FrameRounding);
	const ImU32 fg = (color == kYellow) ? IM_COL32_BLACK : IM_COL32_WHITE;
	dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), fg, text);
	ImGui::Dummy(size);
}

// 带底色的大按钮；color 为 0 时使用默认样式
bool BigButton(const char* label, ImVec2 size, ImU32 color, bool enabled)
{
	ImGui::BeginDisabled(!enabled);
	int pushed = 0;
	if (color != 0 && !enabled)
		color = IM_COL32(0xC8, 0xC8, 0xC8, 0xFF);   // 置灰，避免和可用状态混淆
	if (color != 0) {
		ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
		ImVec4 hovered(std::min(c.x * 1.15f, 1.0f), std::min(c.y * 1.15f, 1.0f), std::min(c.z * 1.15f, 1.0f), 1.0f);
		ImVec4 active(c.x * 0.85f, c.y * 0.85f, c.z * 0.85f, 1.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, c);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
		ImGui::PushStyleColor(ImGuiCol_Text, color == kYellow ? IM_COL32_BLACK : IM_COL32_WHITE);
		pushed = 4;
	}
	bool clicked = ImGui::Button(label, size);
	ImGui::PopStyleColor(pushed);
	ImGui::EndDisabled();
	return clicked && enabled;
}

// 本帧日志面板的中心（屏幕坐标），用于定位弹窗
ImVec2 g_logPanelCenter;
int    g_logPanelFrame = -1;

// 测试参数表格中的一行：左侧标签，右侧输入框
bool ConfigRowInt(const char* label, const char* tip, int* v, int lo, int hi)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
	ImGui::TableNextColumn();
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::PushID(label);
	bool changed = ImGui::InputInt("##v", v);
	ImGui::PopID();
	if (changed) *v = std::clamp(*v, lo, hi);
	return changed;
}

// 16 位十六进制输入（USB VID/PID）
bool ConfigRowHex16(const char* label, const char* tip, int* v)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
	ImGui::TableNextColumn();
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::PushID(label);
	uint16_t u = static_cast<uint16_t>(*v);
	const bool changed = ImGui::InputScalar("##v", ImGuiDataType_U16, &u, nullptr, nullptr, "%04X",
		ImGuiInputTextFlags_CharsHexadecimal);
	ImGui::PopID();
	if (changed) *v = u;
	return changed;
}

bool ConfigRowDouble(const char* label, const char* tip, double* v, double step, double lo, double hi, const char* format = "%.1f")
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
	ImGui::TableNextColumn();
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::PushID(label);
	bool changed = ImGui::InputDouble("##v", v, step, step * 10, format);
	ImGui::PopID();
	if (changed) *v = std::clamp(*v, lo, hi);
	return changed;
}

void ConfigSubHeader(const char* text)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextDisabled("%s", text);
}

// 一个测试类别的参数组：公共部分 + 专用部分
template<typename Fn>
bool ConfigGroup(const char* title, const char* rule, ItemCommonConfig* common, Fn&& specific)
{
	bool changed = false;
	ImGui::PushID(title);
	ImGui::SetWindowFontScale(1.1f);
	ImGui::TextUnformatted(title);
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Separator();

	const float labelW = ImGui::CalcTextSize("最大绝对频偏(KHz)").x + ImGui::GetStyle().ItemSpacing.x;
	if (ImGui::BeginTable("##rows", 2, ImGuiTableFlags_SizingFixedFit)) {
		ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelW);
		ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

		if (common) {
			ConfigSubHeader("公共部分");
			changed |= ConfigRowInt("重试次数", "仅非紧急停止的 ABORT 重试（FAIL 不重试），0~5；\n重试前先复位 DUT 并重新握手", &common->retryCount, 0, kMaxRetry);
			changed |= ConfigRowInt("业务超时(ms)", "启动指令发出到拿到结果的总时长，超时判 ABORT", &common->timeoutMs, 100, 600000);
			ConfigSubHeader("专用部分");
		}
		changed |= specific();

		ImGui::EndTable();
	}
	if (rule) ImGui::TextDisabled("判定：%s", rule);
	ImGui::PopID();
	return changed;
}

void DrawStationCard(TesterViewModel& vm, const StationVM& s, float cardW, float cardH)
{
	ImGui::PushID(s.no);
	ImGui::BeginChild("##card", ImVec2(cardW, cardH), true, ImGuiWindowFlags_NoScrollbar);

	const ImGuiStyle& st = ImGui::GetStyle();
	const float fullW = ImGui::GetContentRegionAvail().x;

	// 标题
	ImGui::SetWindowFontScale(1.25f);
	ImGui::AlignTextToFramePadding();
	ImGui::Text("工位#%d", s.no);
	ImGui::SetWindowFontScale(1.0f);

	// 端口类型 + 端口选择
	ImGui::BeginDisabled(!vm.canEditStation(s));
	const int typeIdx = static_cast<int>(s.portType);
	ImGui::SetNextItemWidth(ImGui::CalcTextSize("UART").x + ImGui::GetFrameHeight() + st.FramePadding.x * 2);
	if (ImGui::BeginCombo("##ptype", kPortTypeNames[typeIdx])) {
		for (int t = 0; t < 2; ++t) {
			if (ImGui::Selectable(kPortTypeNames[t], t == typeIdx) && t != typeIdx)
				vm.setPortType(s.no, static_cast<PortType>(t));
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-FLT_MIN);
	const std::string preview = s.port.empty() ? std::string("选择端口") : s.portLabel;
	if (ImGui::BeginCombo("##port", preview.c_str())) {
		bool any = false;
		for (const auto& p : vm.ports()) {
			if (p.type != s.portType) continue;
			any = true;
			char label[384];
			if (p.description.empty() || p.description == "n/a")
				snprintf(label, sizeof(label), "%s##%s", p.label.c_str(), p.name.c_str());
			else
				snprintf(label, sizeof(label), "%s  %s##%s", p.label.c_str(), p.description.c_str(), p.name.c_str());
			const bool selected = (p.name == s.port);
			const ImGuiSelectableFlags flags = p.usable ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled;
			if (ImGui::Selectable(label, selected, flags) && !selected)
				vm.selectPort(s.no, p.name);
			if (p.name != p.label && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.name.c_str());
			if (selected) ImGui::SetItemDefaultFocus();
		}
		if (!any) {
			if (s.portType == PortType::Usb)
				ImGui::TextDisabled("未发现 VID=%04X PID=%04X 的 USB 设备，请检查连接后点击「刷新端口」", vm.usbVid(), vm.usbPid());
			else
				ImGui::TextDisabled("未发现端口，请点击「刷新端口」");
		}
		ImGui::EndCombo();
	}
	if (!s.port.empty() && s.port != s.portLabel && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.port.c_str());
	ImGui::EndDisabled();

	// 操作按钮
	const float btnW = (fullW - st.ItemSpacing.x) * 0.5f;
	ImGui::BeginDisabled(!vm.canConnect(s));
	if (ImGui::Button("连接端口", ImVec2(btnW, 0))) vm.connect(s.no);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!vm.canDisconnect(s));
	if (ImGui::Button("断开端口", ImVec2(btnW, 0))) vm.disconnect(s.no);
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!vm.canEditStation(s));
	if (ImGui::Button("移除本工位", ImVec2(-FLT_MIN, 0))) vm.removeStation(s.no);
	ImGui::EndDisabled();

	// 下半部分：左列标签 + 右列状态条。用同一个 X 坐标对齐所有状态条
	float labelW = 0;
	for (const char* l : { kBoardLabel, kDutLabel, kStateLabel, kVerdictLabel, kFtFlagLabel })
		labelW = std::max(labelW, ImGui::CalcTextSize(l).x);
	for (const char* l : kItemLabels)
		labelW = std::max(labelW, ImGui::CalcTextSize(l).x);
	const float valueX = ImGui::GetCursorPosX() + labelW + st.ItemSpacing.x;

	auto rowLabel = [valueX](const char* label) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SameLine();
		ImGuiDCXAxisAlign(valueX);
	};
	auto rowChip = [](const char* text, ImU32 color) {
		StatusChip(text, color, ImVec2(ImGui::GetContentRegionAvail().x, 0));
	};

	{
		const Chip c = ChipOf(s.board);
		rowLabel(kBoardLabel);
		rowChip(c.text, c.color);
		if (!s.boardNote.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.boardNote.c_str());
	}
	{
		const Chip c = ChipOf(s.dut);
		rowLabel(kDutLabel);
		rowChip(c.text, c.color);
		if (!s.dutNote.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.dutNote.c_str());
	}
	{
		const Chip c = ChipOf(s.state);
		rowLabel(kStateLabel);
		rowChip(c.text, c.color);
	}
	{
		const Chip c = ChipOf(s.verdict);
		rowLabel(kVerdictLabel);
		rowChip(c.text, c.color);
	}

	ImGui::Separator();

	// 测试项 + 产测标志（悬停显示完整说明）
	auto itemRow = [&](const char* label, const ItemVM& it) {
		const Chip c = ChipOf(it.state);
		char text[96];
		if (it.valueText.empty())
			snprintf(text, sizeof(text), "%s", c.text);
		else if (it.state == ItemState::Running)
			snprintf(text, sizeof(text), "%s", it.valueText.c_str());   // 执行中：直接显示进度说明
		else
			snprintf(text, sizeof(text), "%s  %s", c.text, it.valueText.c_str());

		rowLabel(label);
		rowChip(text, c.color);
		if (!it.detailText.empty() && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", it.detailText.c_str());
	};
	for (int i = 0; i < kTestItemCount; ++i)
		itemRow(kItemLabels[i], s.items[i]);
	itemRow(kFtFlagLabel, s.ftFlag);

	ImGui::EndChild();
	ImGui::PopID();
}

} // namespace

float StationCardHeight()
{
	// 标题行 + 端口行 + 2 行按钮 + 4 行状态 + 分隔线 + 3 行测试项 + 产测标志
	const ImGuiStyle& st = ImGui::GetStyle();
	return ImGui::GetFrameHeight() * 1.25f + ImGui::GetFrameHeightWithSpacing() * 11
		+ st.ItemSpacing.y * 3 + st.WindowPadding.y * 2;
}

// ---------------------------------------------------------------- 控制台：标题 + 全局状态 + 全局按钮

void DrawConsole(TesterViewModel& vm, bool* p_open)
{
	if (!ImGui::Begin(kConsoleWindow, p_open)) {
		ImGui::End();
		return;
	}

	const ImGuiStyle& st = ImGui::GetStyle();

	// 标题行
	const float h = ImGui::GetFrameHeight() * 1.8f;
	ImGui::BeginChild("##title", ImVec2(0, h), false, ImGuiWindowFlags_NoScrollbar);
	ImGui::SetWindowFontScale(1.5f);
	const float lineH = ImGui::GetTextLineHeight();
	ImGui::SetCursorPosY((h - lineH) * 0.5f);
	ImGui::TextUnformatted(APP_TITLE "  " APP_VERSION);

	const Chip c = ChipOf(vm.globalState());
	const char* label = "全局状态：";
	const float chipW = ImGui::CalcTextSize("测试进行中").x + st.FramePadding.x * 4;
	const float chipH = ImGui::GetFrameHeight();
	const float labelW = ImGui::CalcTextSize(label).x;
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - chipW - labelW - st.ItemSpacing.x);
	ImGui::SetCursorPosY((h - lineH) * 0.5f);
	ImGui::TextUnformatted(label);
	ImGui::SameLine();
	ImGui::SetCursorPosY((h - chipH) * 0.5f);
	StatusChip(c.text, c.color, ImVec2(chipW, chipH));
	ImGui::SetWindowFontScale(1.0f);
	ImGui::EndChild();

	// 全局操作按钮
	const float bh = ImGui::GetFrameHeight() * 2.2f;
	const float bw = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * 3) / 4;
	ImGui::SetWindowFontScale(1.3f);
	if (BigButton("开始测试", ImVec2(bw, bh), kGreen, vm.canStart())) vm.start();
	ImGui::SameLine();
	if (BigButton("下一轮", ImVec2(bw, bh), kBlue, vm.canNextRound())) vm.nextRound();
	ImGui::SameLine();
	if (BigButton("紧急停止", ImVec2(bw, bh), kRed, vm.canEmergencyStop())) vm.emergencyStop();
	ImGui::SameLine();
	if (BigButton("清空日志", ImVec2(bw, bh), 0, true)) vm.clearLog();
	ImGui::SetWindowFontScale(1.0f);

	ImGui::End();
}

// ---------------------------------------------------------------- 工位管理

void DrawStations(TesterViewModel& vm, bool* p_open)
{
	if (!ImGui::Begin(kStationsWindow, p_open)) {
		ImGui::End();
		return;
	}

	const ImGuiStyle& st = ImGui::GetStyle();
	const float fh = ImGui::GetFrameHeight();

	if (BigButton("添加工位端口", ImVec2(ImGui::GetFontSize() * 9, fh * 1.6f), kBlue, vm.canAddStation()))
		vm.addStation();
	ImGui::SameLine();
	if (BigButton("刷新端口", ImVec2(ImGui::GetFontSize() * 6, fh * 1.6f), 0, true))
		vm.refreshPorts();
	ImGui::SameLine();
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + fh * 0.3f);
	ImGui::TextDisabled("已添加 %d / %d 个工位", (int)vm.stations().size(), kMaxStations);

	const float cardH = StationCardHeight();
	const float areaH = cardH + st.ScrollbarSize + st.ItemSpacing.y;

	ImGui::BeginChild("##stations", ImVec2(0, areaH), false, ImGuiWindowFlags_HorizontalScrollbar);
	const auto& stations = vm.stations();
	if (stations.empty()) {
		ImGui::Dummy(ImVec2(0, cardH * 0.4f));
		ImGui::TextDisabled("尚未添加工位，请点击「添加工位端口」。");
	}
	else {
		const float availW = ImGui::GetContentRegionAvail().x;
		const float minW = ImGui::GetFontSize() * 16;
		const float cardW = std::max(minW, (availW - st.ItemSpacing.x * (kMaxStations - 1)) / kMaxStations);
		for (size_t i = 0; i < stations.size(); ++i) {
			if (i > 0) ImGui::SameLine();
			DrawStationCard(vm, stations[i], cardW, cardH);
		}
	}
	ImGui::EndChild();

	ImGui::End();
}

// ---------------------------------------------------------------- 测试参数

void DrawConfig(TesterViewModel& vm, bool* p_open)
{
	if (!ImGui::Begin(kConfigWindow, p_open)) {
		ImGui::End();
		return;
	}

	TestConfig& cfg = vm.configEdit();
	bool changed = false;

	// 按面板宽度决定每行放几组（4 / 2 / 1）
	const float groupMinW = ImGui::GetFontSize() * 20;
	const float availW = ImGui::GetContentRegionAvail().x;
	const int cols = availW >= groupMinW * 4 ? 4 : availW >= groupMinW * 2 ? 2 : 1;
	const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame
		| (cols > 1 ? ImGuiTableFlags_BordersInnerV : ImGuiTableFlags_None);

	if (ImGui::BeginTable("##config", cols, flags)) {
		ImGui::TableNextColumn();
		changed |= ConfigGroup("无线收发包测试", "收包率、发包率、RSSI 均 >= 阈值为 PASS", &cfg.rfPacket.common, [&] {
			bool c = false;
			c |= ConfigRowDouble("最低收包率(%)", "DUT 收包率 = 被测板收包数 / 总发包", &cfg.rfPacket.minRxRatePct, 0.5, 0.0, 100.0);
			c |= ConfigRowDouble("最低发包率(%)", "DUT 发包率 = 测试板收包数 / 总发包", &cfg.rfPacket.minTxRatePct, 0.5, 0.0, 100.0);
			c |= ConfigRowDouble("最低RSSI(dBm)", "RSSI 均值 >= 该值为 PASS", &cfg.rfPacket.minRssiDbm, 1.0, -127.0, 127.0, "%.0f");
			return c;
		});
		if (cols == 1) ImGui::Spacing();

		ImGui::TableNextColumn();
		changed |= ConfigGroup("频偏测试", "|实测频偏| <= 阈值为 PASS", &cfg.freqOffset.common, [&] {
			bool c = false;
			c |= ConfigRowDouble("最大绝对频偏(KHz)", "|频偏| <= 该值为 PASS", &cfg.freqOffset.maxAbsOffsetKHz, 1.0, 0.0, 1e6);
			c |= ConfigRowDouble("CFO换算(Hz/LSB)", "频偏(Hz) = CFO 原始 int16 值 x 该系数（待协议确认）", &cfg.freqOffset.cfoHzPerLsb, 0.1, 0.001, 1e6, "%.3f");
			return c;
		});
		if (cols == 1) ImGui::Spacing();

		ImGui::TableNextColumn();
		changed |= ConfigGroup("功耗测试", "平均电流 <= 阈值为 PASS", &cfg.power.common, [&] {
			return ConfigRowDouble("最大功耗(uA)", "平均电流 <= 该值为 PASS", &cfg.power.maxPowerUA, 1.0, 0.0, 1e9);
		});
		if (cols == 1) ImGui::Spacing();

		ImGui::TableNextColumn();
		changed |= ConfigGroup("产测标志", "3 项全部 PASS 后写入，读回 1 为成功，否则本轮 FAIL", nullptr, [&] {
			bool c = false;
			c |= ConfigRowInt("指令重试次数", "复位 DUT、写入标志失败后的重试次数（握手、读回不重试）", &cfg.ftFlag.cmdRetryCount, 0, 10);
			c |= ConfigRowInt("读回延时(ms)", "写入成功后延时多久发送 GET_FT_FLAG", &cfg.ftFlag.readDelayMs, 0, 60000);
			return c;
		});
		if (cols == 1) ImGui::Spacing();

		ImGui::TableNextColumn();
		changed |= ConfigGroup("通信参数", nullptr, nullptr, [&] {
			bool c = false;
			c |= ConfigRowInt("指令超时(ms)", "普通指令应答超时；连续 3 次超时判定测试板断开", &cfg.comm.cmdTimeoutMs, 20, 10000);
			c |= ConfigRowInt("握手超时(ms)", "REQ_CONN_CHECK_DUT 应答超时（测试板 PING 最长 300ms）", &cfg.comm.connCheckTimeoutMs, 350, 10000);
			c |= ConfigRowInt("轮询间隔(ms)", "业务执行中查询 GET_TEST_STATUS 的间隔", &cfg.comm.pollIntervalMs, 10, 5000);
			c |= ConfigRowInt("UART波特率", "下次连接端口时生效", &cfg.comm.baudRate, 1200, 4000000);
			c |= ConfigRowHex16("USB VID", "测试板 USB 厂商 ID（十六进制），修改后自动刷新端口列表", &cfg.comm.usbVid);
			c |= ConfigRowHex16("USB PID", "测试板 USB 产品 ID（十六进制），修改后自动刷新端口列表", &cfg.comm.usbPid);
			return c;
		});

		ImGui::EndTable();
	}

	// 参数实时生效，无确认弹窗
	if (changed) vm.commitConfig();

	ImGui::End();
}

// ---------------------------------------------------------------- 统计

void DrawStats(TesterViewModel& vm, bool* p_open)
{
	if (!ImGui::Begin(kStatsWindow, p_open)) {
		ImGui::End();
		return;
	}

	if (ImGui::Button("清零统计")) vm.resetStats();
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("良率 = PASS / (PASS + FAIL)，ABORT 不计入");

	const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame;
	if (ImGui::BeginTable("##stats", 6, flags)) {
		ImGui::TableSetupColumn("工位");
		ImGui::TableSetupColumn("测试数");
		ImGui::TableSetupColumn("PASS");
		ImGui::TableSetupColumn("FAIL");
		ImGui::TableSetupColumn("ABORT");
		ImGui::TableSetupColumn("良率");
		ImGui::TableHeadersRow();

		const auto& rows = vm.statsRows();
		for (size_t i = 0; i < rows.size(); ++i) {
			const StatsRowVM& r = rows[i];
			const bool total = i + 1 == rows.size();
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(r.label.c_str());
			ImGui::TableNextColumn(); ImGui::Text("%d", r.tested);
			ImGui::TableNextColumn(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGreen), "%d", r.pass);
			ImGui::TableNextColumn(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kRed), "%d", r.fail);
			ImGui::TableNextColumn(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kOrange), "%d", r.abort);
			ImGui::TableNextColumn(); ImGui::TextUnformatted(r.yieldText.c_str());
			if (total) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0, 0, 0, 0x18));
		}
		ImGui::EndTable();
	}

	ImGui::Spacing();
	ImGui::TextDisabled("结果文件：");
	ImGui::SameLine();
	ImGui::TextWrapped("%s", vm.resultFile().empty() ? "--" : vm.resultFile().c_str());
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("右键复制路径");
	if (ImGui::BeginPopupContextItem("##csvpath")) {
		if (ImGui::MenuItem("复制路径")) ImGui::SetClipboardText(vm.resultFile().c_str());
		ImGui::EndPopup();
	}

	ImGui::End();
}

// ---------------------------------------------------------------- 日志

void DrawLog(TesterViewModel& vm, bool* p_open)
{
	if (!ImGui::Begin(kLogWindow, p_open)) {
		ImGui::End();
		return;
	}

	{
		const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
		g_logPanelCenter = ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f);
		g_logPanelFrame = ImGui::GetFrameCount();
	}

	// 工具栏
	ImGui::BeginDisabled(!vm.hasLogSelection());
	if (ImGui::Button("复制选中")) ImGui::SetClipboardText(vm.selectedLogText().c_str());
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("复制全部")) ImGui::SetClipboardText(vm.allLogText().c_str());
	ImGui::SameLine();
	if (ImGui::Button("清空")) vm.clearLog();
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("单击 / 拖拽 / Shift+单击选择行，Ctrl+C 复制，Ctrl+A 全选，右键菜单");

	ImGui::BeginChild("##rows", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

	static bool s_dragging = false;
	const ImGuiIO& io = ImGui::GetIO();
	const auto& logs = vm.logs();

	ImGuiListClipper clipper;
	clipper.Begin((int)logs.size());
	while (clipper.Step()) {
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			const LogLineVM& line = logs[i];
			ImGui::PushID((int)line.seq);

			// Selectable 只负责高亮和命中测试，文字单独绘制以保留按级别着色
			const ImVec2 pos = ImGui::GetCursorPos();
			const bool selected = vm.isLogSelected(line.seq);
			ImGui::Selectable("##row", selected);
			// 拖选时按下的是别的行，需要 AllowWhenBlockedByActiveItem 才能检测到悬停
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
					vm.selectLog(line.seq, io.KeyShift);
					s_dragging = true;
				}
				else if (s_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
					vm.selectLog(line.seq, true);
				}
				else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !selected) {
					vm.selectLog(line.seq, false);
				}
			}
			ImGui::SetCursorPos(pos);

			int pushed = 0;
			if (line.level == LogLevel::Warn)  { ImGui::PushStyleColor(ImGuiCol_Text, kOrange); pushed = 1; }
			if (line.level == LogLevel::Error) { ImGui::PushStyleColor(ImGuiCol_Text, kRed);    pushed = 1; }
			ImGui::TextUnformatted(line.text.c_str());
			ImGui::PopStyleColor(pushed);

			ImGui::PopID();
		}
	}
	clipper.End();

	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		s_dragging = false;

	// 点击空白处取消选择
	if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		vm.clearLogSelection();

	// 右键菜单
	if (ImGui::BeginPopupContextWindow("##logctx")) {
		if (ImGui::MenuItem("复制选中", "Ctrl+C", false, vm.hasLogSelection()))
			ImGui::SetClipboardText(vm.selectedLogText().c_str());
		if (ImGui::MenuItem("复制全部"))
			ImGui::SetClipboardText(vm.allLogText().c_str());
		if (ImGui::MenuItem("全选", "Ctrl+A"))
			vm.selectAllLogs();
		ImGui::Separator();
		if (ImGui::MenuItem("清空日志"))
			vm.clearLog();
		ImGui::EndPopup();
	}

	// 已在底部且没在拖选时，跟随新日志滚动
	if (!s_dragging && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
		ImGui::SetScrollHereY(1.0f);

	ImGui::EndChild();

	// 快捷键：日志面板（含子窗口）获得焦点时生效
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io.KeyCtrl) {
		if (ImGui::IsKeyPressed(ImGuiKey_C, false) && vm.hasLogSelection())
			ImGui::SetClipboardText(vm.selectedLogText().c_str());
		if (ImGui::IsKeyPressed(ImGuiKey_A, false))
			vm.selectAllLogs();
	}

	ImGui::End();
}

// ---------------------------------------------------------------- 业务弹窗

void DrawAlert(TesterViewModel& vm)
{
	const AlertEvent* a = vm.currentAlert();
	if (!a) return;

	// 每个 alert id 对应独立窗口 ID，新弹窗总是出现在固定位置并获得焦点
	static uint64_t s_focusedId = 0;
	char name[160];
	snprintf(name, sizeof(name), "%s###tester_alert_%llu", a->title.c_str(), (unsigned long long)a->id);

	// 停在日志面板中央（日志面板不可见时退回到窗口下部），避免遮挡全局操作按钮（紧急停止必须始终可点）
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImVec2 anchor(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.8f);
	if (g_logPanelFrame == ImGui::GetFrameCount())
		anchor = g_logPanelCenter;

	const float fs = ImGui::GetFontSize();
	ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(0.5f, 0.5f));   // 跟随日志面板（布局变化时也不会跑偏）
	ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 20, 0), ImVec2(FLT_MAX, FLT_MAX));
	if (s_focusedId != a->id) {
		ImGui::SetNextWindowFocus();
		s_focusedId = a->id;
	}

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
	ImGui::Begin(name, nullptr, flags);

	const Chip c = ChipOf(a->level);
	StatusChip(c.text, c.color);
	if (vm.pendingAlertCount() > 1) {
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("（还有 %d 条待处理）", (int)vm.pendingAlertCount() - 1);
	}

	ImGui::Spacing();
	ImGui::PushTextWrapPos(fs * 28);
	ImGui::TextUnformatted(a->message.c_str());
	ImGui::PopTextWrapPos();
	ImGui::Spacing();
	ImGui::Separator();

	// 注意：closeAlert 之后 a 失效，不能再访问
	const ImVec2 btn(fs * 6, ImGui::GetFrameHeight() * 1.3f);
	if (a->kind == AlertKind::Confirm) {
		if (ImGui::Button("确定", btn)) vm.closeAlert(true);
		else {
			ImGui::SameLine();
			if (ImGui::Button("取消", btn)) vm.closeAlert(false);
		}
	}
	else {
		if (ImGui::Button("确定", btn)) vm.closeAlert(true);
	}

	ImGui::End();
}

} // namespace Lx::Tester::View
