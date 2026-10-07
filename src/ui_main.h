#pragma once
#include <iostream>
#include <vector>
#include <functional>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

using UITask = std::function<void()>;
void PostToUI(UITask&& task);
void PostToUI(UITask const& task);

void* CreateTexture(uint8_t* data, int w, int h, char fmt);
void DeleteTexture(void* tex);

class UiDpiContext {
public:
    static UiDpiContext& instance() {
        static UiDpiContext inst;
        return inst;
    }

    void updateDpiInfo(GLFWwindow* window)
    {
        GLFWmonitor* monitor = glfwGetWindowMonitor(window);
        if (!monitor) {
            monitor = glfwGetPrimaryMonitor();
        }

        int widthMM, heightMM;
        glfwGetMonitorPhysicalSize(monitor, &widthMM, &heightMM);

        const GLFWvidmode* mode = glfwGetVideoMode(monitor);

        // 毫米转英寸：1英寸 = 25.4毫米
        double inchesW = widthMM / 25.4;
        double inchesH = heightMM / 25.4;

        m_dpiX = (float)(mode->width / inchesW);
        m_dpiY = (float)(mode->height / inchesH);

        glfwGetMonitorContentScale(monitor, &m_scaleX, &m_scaleY);

        glfwGetFramebufferSize(window, &m_fw, &m_fh);
        glfwGetWindowSize(window, &m_w, &m_h);
    }

    void onWindowResize(int w, int h) {
        m_w = w;
        m_h = h;
    }

    void onDpiScaleChange(float scaleX, float scaleY) {
        m_scaleX_new = scaleX;
        m_scaleY_new = scaleY;
        m_needRebuildNormalFont = true;
        m_needRebuildBigFont = true;
        m_needScaleImGuiStyle = true;
    }

    void setWordList(const std::vector<std::string>& wordList) {
        m_wordList = wordList;
        m_wordIndex = 0;
        m_needRebuildBigFont = true;
    }

	void setWordIndex(int index) {
		if (index >= 0 && index < m_wordList.size()) {
			m_wordIndex = index;
            m_needRebuildBigFont = true;
		}
	}

    void doDpiScaleChage()
    {
        if (m_needRebuildNormalFont) {
            m_needRebuildNormalFont = false;
            rebuildNormalFont();
        }
        if (m_needRebuildBigFont) {
            m_needRebuildBigFont = false;
            rebuildBigFont();
        }
        if (m_needScaleImGuiStyle) {
            m_needScaleImGuiStyle = false;

            float sc = m_scaleX_new / m_scaleX;

            ImGuiStyle& style = ImGui::GetStyle();
            style = ImGuiStyle(); // 重置为原生默认样式
            ImGui::StyleColorsLight();
            style.ScaleAllSizes(sc);
        }
    }

	ImFont* getNormalFont() { return m_normalFont; }
    ImFont* getBigFont() { return m_bigFont; }

	void setClearColor(const ImVec4& color) { clear_color = color; }
	void getClearColor(ImVec4& color) const { color = clear_color; }

	void getViewportSize(int& w, int& h) const {
		w = m_w;
		h = m_h;
	}

	void getWindowSize(int& w, int& h) const {
		w = m_w;
		h = m_h;
	}

    float getScale() const {
		return m_scaleX; // 一般情况下，X和Y的缩放比例是相同的
    }

private:
    UiDpiContext() = default;
    ~UiDpiContext() { release(); }

    // 释放资源
    void release() {
        if (m_bigFontTexID) {
            DeleteTexture(m_bigFontTexID);
            m_bigFontTexID = 0;
        }
        if (m_bigFontAtlas) {
            IM_DELETE(m_bigFontAtlas);
            m_bigFontAtlas = nullptr;
        }
        m_bigFont = nullptr;
    }

    // 【常规字体】全中文，只在DPI变化时重建
    void rebuildNormalFont();

    // 【大字】从词表构建大字字体（自动销毁旧资源）
    bool rebuildBigFont();

	ImVec4 clear_color = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);

    // DPI scaling
    float m_scaleX = 1.0f, m_scaleY = 1.0f;
    float m_scaleX_new = 1.0f, m_scaleY_new = 1.0f;
	float m_dpiX = 96.0f, m_dpiY = 96.0f;
    int m_w = 0, m_h = 0, m_fw = 0, m_fh = 0;
    bool m_needScaleImGuiStyle = false;

    // normal font
    float           m_normalFontSize = 18.0f;
    const char*     m_normalFontFile = "C:/Windows/Fonts/msyh.ttc";
    ImFont*         m_normalFont = nullptr;
    bool            m_needRebuildNormalFont = true;

    // big font
    float           m_bigFontSize = 144.0f;
    const char*     m_bigFontFile = "C:/Windows/Fonts/msyh.ttc";
    ImFontAtlas*    m_bigFontAtlas = nullptr;
    ImFont*         m_bigFont = nullptr;
    void*           m_bigFontTexID = nullptr;
    bool            m_needRebuildBigFont = false;
    int             m_wordIndex = 0;
    std::vector<std::string> m_wordList = {};
};