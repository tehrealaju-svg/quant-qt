// Quant desktop client entry point: window, OpenGL, Dear ImGui setup, main loop.
#include <cstdio>
#include <filesystem>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "app.h"
#include "png.h"

static void glfw_error(int code, const char* msg) { fprintf(stderr, "GLFW %d: %s\n", code, msg); }

static void load_fonts(ImGuiIO& io, float scale) {
    const char* candidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    };
    const char* mono[] = {
        "C:\\Windows\\Fonts\\consola.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    };
    bool ok = false;
    for (auto* f : candidates)
        if (std::filesystem::exists(f)) { io.Fonts->AddFontFromFileTTF(f, 17.0f * scale); ok = true; break; }
    if (!ok) io.Fonts->AddFontDefault();
    ImFont* big = nullptr;
    for (auto* f : candidates)
        if (std::filesystem::exists(f)) { big = io.Fonts->AddFontFromFileTTF(f, 30.0f * scale); break; }
    ImFont* m = nullptr;
    for (auto* f : mono)
        if (std::filesystem::exists(f)) { m = io.Fonts->AddFontFromFileTTF(f, 15.0f * scale); break; }
    App::set_fonts(big ? big : io.Fonts->Fonts[0], m ? m : io.Fonts->Fonts[0]);
}

static void apply_style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 6; s.FrameRounding = 5; s.GrabRounding = 5; s.TabRounding = 5; s.ChildRounding = 6;
    s.PopupRounding = 6; s.ScrollbarRounding = 6;
    s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 8); s.WindowPadding = ImVec2(14, 14);
    ImVec4* c = s.Colors;
    ImVec4 bg(0.07f, 0.08f, 0.10f, 1), panel(0.11f, 0.12f, 0.15f, 1), accent(0.40f, 0.36f, 0.95f, 1), accent2(0.52f, 0.48f, 1.0f, 1);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.17f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.21f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.21f, 0.27f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.25f, 0.32f, 1);
    c[ImGuiCol_Button] = accent;
    c[ImGuiCol_ButtonHovered] = accent2;
    c[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.28f, 0.85f, 1);
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.22f, 0.32f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.27f, 0.42f, 1);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_CheckMark] = accent2;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent2;
    c[ImGuiCol_PlotLines] = accent2;
    c[ImGuiCol_PlotHistogram] = accent2;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.15f, 0.19f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.23f, 0.28f, 1);
    s.ScaleAllSizes(scale);
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* win = glfwCreateWindow(1280, 800, "Quant Wallet", nullptr, nullptr);
    if (!win) {
        // Fallback for old GPUs / Raspberry Pi drivers.
        glfwDefaultWindowHints();
        win = glfwCreateWindow(1280, 800, "Quant Wallet", nullptr, nullptr);
        if (!win) return 1;
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    float xs = 1, ys = 1;
    glfwGetWindowContentScale(win, &xs, &ys);
    float scale = xs > 0.5f ? xs : 1.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    load_fonts(io, scale);
    apply_style(scale);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(nullptr);

    App app(argc, argv);
    while (!glfwWindowShouldClose(win) && !app.quit_requested()) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) { glfwWaitEventsTimeout(0.25); app.tick_background(); continue; }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.07f, 0.08f, 0.10f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        std::string cap = app.capture_request();
        if (!cap.empty()) {
            std::vector<uint8_t> px(size_t(w) * h * 4);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            write_png(cap, w, h, px);
        }
        glfwSwapBuffers(win);
    }
    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
