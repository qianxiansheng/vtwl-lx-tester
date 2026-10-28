cmake_minimum_required(VERSION 3.21)
project(lxTester LANGUAGES C CXX)

find_package(glfw3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(serial CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

## Sources
set(lxTester_SRCS
    src/main.cpp
    src/ui_main.cpp
    src/protocol/BoardProtocol.cpp
    src/link/BoardLink.cpp
    src/link/SerialTransport.cpp
    src/link/SimTransport.cpp
    src/link/UsbTransport.cpp
    src/tester/ResultCsv.cpp
    src/tester/StationController.cpp
    src/tester/TesterConfigStore.cpp
    src/tester/TesterModel.cpp
    src/tester/TesterService.cpp
    src/tester/TesterSpec.cpp
    src/tester/TesterView.cpp
    src/tester/TesterViewModel.cpp
    src/utils/ini.c
    src/utils/INIReader.cpp
    src/utils/utils.cpp
)

set(imgui_SRCS
    src/imgui/imgui.cpp
    src/imgui/imgui_demo.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_tables.cpp
    src/imgui/imgui_widgets.cpp
    src/imgui/backends/imgui_impl_glfw.cpp
    src/imgui/backends/imgui_impl_opengl3.cpp
    src/imgui/backends/imgui_impl_win32.cpp
    src/imgui/extensions/cimspinner.cpp
    src/imgui/extensions/ImConsole.cpp
    src/imgui/extensions/ImFileDialog.cpp
    src/imgui/extensions/TextEditor.cpp
)

## Executable
add_executable(lxTester ${lxTester_SRCS} ${imgui_SRCS} app.manifest)

target_compile_features(lxTester PRIVATE cxx_std_20)

target_include_directories(lxTester PRIVATE
    src
    src/imgui
    src/imgui/backends
    src/imgui/extensions
)

target_compile_definitions(lxTester PRIVATE
    UNICODE
    _UNICODE
    _CONSOLE
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    _SILENCE_CXX17_RESULT_OF_DEPRECATION_WARNING
)

if(MSVC)
    target_compile_options(lxTester PRIVATE /W3 /sdl /permissive- /utf-8
        $<$<CONFIG:Release>:/Gy /Oi>
    )
    set_property(TARGET lxTester PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

target_link_libraries(lxTester PRIVATE
    glfw
    nlohmann_json::nlohmann_json
    serial::serial
    OpenGL::GL
)

# Run from the source dir so config.json, imgui.ini and results/ stay where they were
set_property(TARGET lxTester PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT lxTester)
