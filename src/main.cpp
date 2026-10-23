// Korektor PDF — punkt wejścia: okno GLFW, kontekst OpenGL i pętla Dear ImGui.

#include "App.h"
#include "Fonts.h"
#include "ImageLoad.h"
#if defined(__APPLE__)
#include "MacOpen.h"
#endif

#include "imgui.h"
#include "imgui_internal.h"   // kolejka zdarzeń wejścia: szybko wpisane znaki są rozkładane na kolejne klatki
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GL_SILENCE_DEPRECATION
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static App* gApp = nullptr;

static void onGlfwError(int code, const char* desc) { std::fprintf(stderr, "GLFW %d: %s\n", code, desc); }

static void onDrop(GLFWwindow*, int count, const char** paths) {
    std::vector<std::string> v(paths, paths + count);
    if (gApp) gApp->onDrop(v);
}

static void onClose(GLFWwindow*) {
    if (gApp) gApp->onCloseRequested();
}

// Argumenty wiersza poleceń jako UTF-8 (na Windows z CommandLineToArgvW).
static std::vector<std::string> utf8Args(int argc, char** argv) {
    std::vector<std::string> out;
#if defined(_WIN32)
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; w && i < n; ++i) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(len > 0 ? len - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(s);
    }
    if (w) LocalFree(w);
#else
    for (int i = 1; i < argc; ++i) {
#if defined(__APPLE__)
        if (std::string(argv[i]).rfind("-psn_", 0) == 0) continue;   // dawne macOS: numer procesu od Findera
#endif
        out.emplace_back(argv[i]);
    }
#endif
    return out;
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(onGlfwError);
#if defined(__APPLE__)
    macInstallOpenHandler();
#endif
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    GLFWwindow* window = glfwCreateWindow(int(1280 * scale), int(860 * scale), "Korektor PDF", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Nie można utworzyć okna OpenGL 3.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;   // Ctrl+Tab przełącza karty dokumentów, nie okna ImGui
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;

    // Czcionki interfejsu z polskimi znakami (te same pliki DejaVu służą też do tekstu w PDF).
    auto addFont = [&](const kpdf::FontFile& f) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(f.data), int(f.size), 15.f, &cfg);
    };
    ImFont* font = addFont(kpdf::uiFont());
    ImFont* fontBold = addFont(kpdf::fontFile(kpdf::FamilySans, true, false));
    ImFont* fontItalic = addFont(kpdf::fontFile(kpdf::FamilySans, false, true));

    kpdf::Document::initLibrary();
    {
        App app(window, font, fontBold, fontItalic);   // ustawia też styl
        ImGuiStyle& style = ImGui::GetStyle();
        style.FontSizeBase = 15.f;
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init(glsl);

        gApp = &app;
        glfwSetDropCallback(window, onDrop);
        glfwSetWindowCloseCallback(window, onClose);

        const auto args = utf8Args(argc, argv);
        if (!args.empty()) app.openPaths(args, true);

        // Tryb diagnostyczny: KOREKTOR_SCREENSHOT=plik.png zapisuje zrzut okna po kilkudziesięciu klatkach i kończy.
        const char* shot = std::getenv("KOREKTOR_SCREENSHOT");
        int frames = 0, extraFrames = 3;

        while (!app.wantsQuit()) {
            if (glfwWindowShouldClose(window)) { glfwSetWindowShouldClose(window, GLFW_FALSE); app.onCloseRequested(); }
            // Po każdym zdarzeniu kilka klatek bez czekania: ImGui rozkłada szybkie sekwencje
            // klawiszy (np. Ctrl+Z) na kolejne klatki. Bezczynny program nie zużywa procesora.
            if (app.busy() || extraFrames > 0 || shot || !ImGui::GetCurrentContext()->InputEventsQueue.empty()) { glfwPollEvents(); if (extraFrames > 0) --extraFrames; }
            else { glfwWaitEventsTimeout(0.5); extraFrames = 3; }
#if defined(__APPLE__)
            {
                const auto opened = macTakeOpenedFiles();   // z Findera albo z ikony w Docku
                if (!opened.empty()) { app.openPaths(opened, true); extraFrames = 3; }
            }
#endif

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();

            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.865f, 0.886f, 0.902f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            ++frames;
            if (shot && frames == 40) {
                std::vector<uint8_t> px(size_t(w) * h * 4), flipped(px.size());
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                for (int y = 0; y < h; ++y)
                    std::copy_n(&px[size_t(h - 1 - y) * w * 4], size_t(w) * 4, &flipped[size_t(y) * w * 4]);
                kpdf::writePng(shot, w, h, flipped.data());
                break;
            }
            glfwSwapBuffers(window);
        }
        gApp = nullptr;
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
    kpdf::Document::destroyLibrary();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
