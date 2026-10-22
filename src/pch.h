

#define APP_TITLE           "VtWl 产测上位机"
#define APP_VERSION         "v0.2.0"
#define DEFAULT_FONT		"c:\\Windows\\Fonts\\msyh.ttc"

#define ImGuiDCXAxisAlign(v) ImGui::SetCursorPos(ImVec2((v), ImGui::GetCursorPos().y))
#define ImGuiDCYMargin(v)    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPos().x, ImGui::GetCursorPos().y + (v)))
#define IMIDTEXT(name, i)    ((std::string(name) + std::to_string(i)).c_str())