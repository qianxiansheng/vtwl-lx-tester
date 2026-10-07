#include "pch.h"
#include <Windows.h>
#include "ui_main.h"
#if defined(WITH_SOURCE)
#include "../resource.h"
#endif
#include "utils/concurrentqueue/BlockingConcurrentQueue.h"

// Dear ImGui: standalone example application for GLFW + OpenGL 3, using programmable pipeline
// (GLFW is a cross-platform general purpose library for handling windows, inputs, OpenGL/Vulkan/Metal graphics context creation, etc.)
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and compatibility with old VS compilers.
// To link with VS2010-era libraries, VS2015+ requires linking with legacy_stdio_definitions.lib, which we do using this pragma.
// Your own project should not be affected, as you are likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif
static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

// 【常规字体】全中文，只在DPI变化时重建
void UiDpiContext::rebuildNormalFont() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.Fonts->ClearTexData();

    float sc = m_scaleX;
    m_normalFont = io.Fonts->AddFontFromFileTTF(
        m_normalFontFile,
        m_normalFontSize * sc,
        nullptr,
        io.Fonts->GetGlyphRangesChineseFull()
    );

    io.Fonts->Build();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

// 【大字】从词表构建大字字体（自动销毁旧资源）
bool UiDpiContext::rebuildBigFont() {
    release(); // 先销毁旧的

    if (m_wordList.empty())
        return false;

    // 1. 拼接所有文本
    std::u8string allText;
    //for (auto& w : m_wordList) {
    //    allText += (const char8_t*)w.c_str();
    //}
    allText += (const char8_t*)m_wordList[m_wordIndex].c_str();

    // 2. 构建字符集范围
    ImFontGlyphRangesBuilder builder;
    builder.AddText((const char*)allText.c_str());
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);

    // 3. 创建字体图集
    m_bigFontAtlas = IM_NEW(ImFontAtlas)();
    m_bigFontAtlas->TexDesiredWidth = 2048;
    m_bigFontAtlas->Flags |= ImFontAtlasFlags_NoMouseCursors;

    ImFontConfig cfg{};
    cfg.OversampleH = 4;
    cfg.OversampleV = 4;
    cfg.RasterizerMultiply = 1.05f;

    float sc = m_scaleX;
    m_bigFont = m_bigFontAtlas->AddFontFromFileTTF(
        m_bigFontFile,
        m_bigFontSize * sc,
        &cfg,
        ranges.Data
    );

    m_bigFontAtlas->Build();

    // 4. 上传纹理到GPU
    unsigned char* pixels;
    int w, h;
    m_bigFontAtlas->GetTexDataAsRGBA32(&pixels, &w, &h);

    m_bigFontTexID = CreateTexture(pixels, w, h, 1);

    m_bigFontAtlas->SetTexID((ImTextureID)(intptr_t)m_bigFontTexID);
    return m_bigFont != nullptr;
}

static GLFWwindow* window = NULL;

// UI task queue
moodycamel::BlockingConcurrentQueue<UITask, moodycamel::ConcurrentQueueDefaultTraits> g_ui_queue;
void PostToUI(UITask&& task)
{
    if (!g_ui_queue.try_enqueue(std::move(task))) {
		std::cerr << "UI task queue is full, dropping task" << std::endl;
		return;
    }
}
void PostToUI(UITask const& task)
{
    if (!g_ui_queue.try_enqueue(task)) {
		std::cerr << "UI task queue is full, dropping task" << std::endl;
		return;
    }
}


#if defined(WITH_SOURCE)
// 在创建 GLFW 窗口后调用
void SetGLFWWindowIconFromResource(GLFWwindow* window)
{
    // 1. 从 EXE 资源加载图标
    HICON hIcon = LoadIcon(
        GetModuleHandle(NULL),
        MAKEINTRESOURCE(IDI_LX)
    );

    if (hIcon == NULL)
        return;

    // 2. 获取图标像素数据（GLFW 需要原始像素）
    ICONINFO iconInfo = { 0 };
    GetIconInfo(hIcon, &iconInfo);

    BITMAP bmp = { 0 };
    GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bmp);

    int width = bmp.bmWidth;
    int height = bmp.bmHeight;

    // 3. 提取 RGBA 像素数据
    unsigned char* pixels = new unsigned char[width * height * 4];
    HDC hdc = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(hdc);
    SelectObject(memDC, iconInfo.hbmColor);

    BITMAPINFOHEADER bih = { 0 };
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = -height;  // 从上到下
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    GetDIBits(memDC, iconInfo.hbmColor, 0, height, pixels, (BITMAPINFO*)&bih, DIB_RGB_COLORS);

    // 4. 传给 GLFW 设置窗口图标
    GLFWimage image;
    image.width = width;
    image.height = height;
    image.pixels = pixels;

    glfwSetWindowIcon(window, 1, &image);

    // 5. 清理
    delete[] pixels;
    DeleteDC(memDC);
    ReleaseDC(NULL, hdc);
    DeleteObject(iconInfo.hbmColor);
    DeleteObject(iconInfo.hbmMask);
}
#endif

extern bool main_init(int argc, char* argv[]);
extern void main_shutdown(void);
extern int main_gui();

static void GLFWwindowsizeCallback(GLFWwindow* window, int width, int height)
{
    UiDpiContext::instance().onWindowResize(width, height);
}
static void GLFWwindowcontentscaleCallback(GLFWwindow* window, float xscale, float yscale)
{
    UiDpiContext::instance().onDpiScaleChange(xscale, yscale);
}

int main_(int argc, char** argv)
{
    // Setup window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100
    const char* glsl_version = "#version 100";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    //glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
#endif

    // Create window with graphics context
    window = glfwCreateWindow(1280, 720, APP_TITLE, NULL, NULL);
    if (window == NULL)
        return 1;

#if defined(WITH_SOURCE)
    // Load ICON
    SetGLFWWindowIconFromResource(window);
#endif

    //glfwMaximizeWindow(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/ProggyTiny.ttf", 10.0f);
    //io.Fonts->AddFontDefault();
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\Arial.ttf", FONT_BASE_SIZE, NULL, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != NULL);

	UiDpiContext::instance().updateDpiInfo(window);

    glfwSetWindowSizeCallback(window, GLFWwindowsizeCallback);
    glfwSetWindowContentScaleCallback(window, GLFWwindowcontentscaleCallback);

    if (!main_init(argc, argv))
        glfwSetWindowShouldClose(window, 1);

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwPollEvents();

        // Process UI tasks
        UITask task;
        while (g_ui_queue.try_dequeue(task)) {
            task();
        }

        UiDpiContext::instance().doDpiScaleChage();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int r = main_gui();
        if (r) glfwSetWindowShouldClose(window, 1);

		int fw, fh;
        ImVec4 clear_color;
        UiDpiContext::instance().getViewportSize(fw, fh);
		UiDpiContext::instance().getClearColor(clear_color);

        // Rendering
        ImGui::Render();
        glViewport(0, 0, fw, fh);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        static bool first_draw = true;
        if (first_draw) {
            first_draw = false;
            glfwShowWindow(window);
        }
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    main_shutdown();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

void* CreateTexture(uint8_t* data, int w, int h, char fmt)
{
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, (fmt == 0) ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, data);
    //glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    return reinterpret_cast<void*>(static_cast<intptr_t>(tex));
}

void DeleteTexture(void* tex)
{
    GLuint texID = (GLuint)((uintptr_t)tex);
    glDeleteTextures(1, &texID);
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR, _In_ int nShowCmd)
{
    return main_(__argc, __argv);
}

int main()
{
    return main_(__argc, __argv);
}