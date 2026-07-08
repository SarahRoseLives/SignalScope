// SignalScope - Inmarsat decoder
// Phase 1: RTL-SDR -> IQ ring -> FFT -> spectrum + scrolling waterfall.

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>

#include "core/app.h"
#include "i18n/i18n.h"
#include "util/log.h"
#include "version.h"
#include "gui/waterfall.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

bool openWavDialog(char* out, int outLen)
{
    char file[1024] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "WAV / IQ files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrInitialDir = "D:\\";
    if (GetOpenFileNameA(&ofn))
    {
        std::strncpy(out, file, outLen - 1);
        out[outLen - 1] = 0;
        return true;
    }
    return false;
}
#else
bool openWavDialog(char*, int) { return false; }
#endif

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}


#include "core/app.h"
#include "core/main_funcs.h"

int main(int, char**)
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "SignalScope", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    App app;

    // Load persisted settings BEFORE building the font atlas so fontSize
    // from signalscope.ini is available for the font loader.
    cfgRegisterHandler(app);
    io.IniFilename = "signalscope.ini";
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);

    // English-only strings pass through _L() as identity.
    i18nInit();

    // Font: use Roboto-Medium (vendored TTF, scales cleanly at any size).
    // Clamp to sensible range and scale the style sizes proportionally.
    if (app.fontSize < 8)  app.fontSize = 8;
    if (app.fontSize > 24) app.fontSize = 24;
    {
        const char* fontPaths[] = {
            "third_party/imgui/misc/fonts/Roboto-Medium.ttf",
            "../third_party/imgui/misc/fonts/Roboto-Medium.ttf",
        };
        bool loaded = false;
        for (auto& fp : fontPaths)
        {
            FILE* f = std::fopen(fp, "rb");
            if (f) { std::fclose(f); loaded = (io.Fonts->AddFontFromFileTTF(fp, (float)app.fontSize) != nullptr); break; }
        }
        if (!loaded)
            io.Fonts->AddFontDefault(); // fallback to built-in ProggyClean
    }
    ImGui::GetStyle().ScaleAllSizes((float)app.fontSize / 13.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // If the saved layout predates the current default (or none was saved),
    // rebuild the canonical default layout so users get the intended arrangement.
    if (app.layoutVersion != kLayoutVersion)
    {
        app.forceDefaultLayout = true;
        app.layoutVersion = kLayoutVersion;
    }

    buildWindow(app.viewA, kFftSizes[app.fftSizeIdx], app.dbMin);
    buildWindow(app.viewB, kFftSizes[app.fftSizeIdx], app.dbMin);
    app.devices = app.sdr.listDevices();

    // Audio output: start the shared playback device and connect it to both
    // decoder managers so WFM channels can be routed to the speaker.
    {
        std::string aerr;
        app.audioEnabled = app.audio.start(aerr);
        if (!app.audioEnabled)
            logWrite("audio disabled: %s", aerr.c_str());
        app.audio.setVolume(app.audioVolume);
        app.audio.setMuted(app.audioMuted);
        app.decoders.setAudioSink(&app.audio);
        app.decodersB.setAudioSink(&app.audio);
    }
    app.verCheck.start("signalscope", SIGNALSCOPE_VERSION);
    scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
    if (app.bandPlanIdx >= 0 && app.bandPlanIdx < (int)app.bandPlanPaths.size())
        app.bandPlanLoaded = loadBandPlan(app.bandPlanPaths[app.bandPlanIdx]);
    if (app.bandPlanIdxB >= 0 && app.bandPlanIdxB < (int)app.bandPlanPaths.size())
        app.bandPlanLoadedB = loadBandPlan(app.bandPlanPaths[app.bandPlanIdxB]);

    const ImVec4 clear_color = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawDockHost(app);

        // Finalize an async source start (LibreSDR/UHD) once the worker thread
        // has opened the device. Done on the GUI thread so startActive() only
        // touches shared state (rings, decoders, active source) from here.
        if (app.startReady.exchange(false))
        {
            if (app.startThread.joinable())
                app.startThread.join();
            if (app.startErr.empty())
                startActive(app); // device already prepared -> returns quickly
            else
                app.status = "Error: " + app.startErr;
            app.starting.store(false);
        }

        if (app.active->running())
            processFft(app.viewA, app, app.active->centerFreq(), app.active->sampleRate());
        if (app.dualMode && app.sdrB.running())
            processFft(app.viewB, app, app.sdrB.centerFreq(), app.sdrB.sampleRate());

        if (app.active->running())
            updateRateChange(app);

        // Refresh saved decoder list for persistent restart.
        if (app.saveDecoders && app.active->running())
        {
            app.savedDecoders.clear();
            for (auto& st : app.decoders.status())
                app.savedDecoders.push_back({st.freqMHz, st.typeId});
            app.savedDecodersB.clear();
            for (auto& st : app.decodersB.status())
                app.savedDecodersB.push_back({st.freqMHz, st.typeId});
        }

        drawControls(app);
        {
            std::string t = std::string(_L("Spectrum")) + "###Spectrum";
            drawSpectrum(app, app.viewA, app.decoders, t.c_str(), true, false);
        }
        {
            std::string t = std::string(_L("Waterfall")) + "###Waterfall";
            drawWaterfall(app, app.viewA, t.c_str());
        }
        if (app.dualMode)
        {
            {
                std::string t = std::string(_L("Spectrum")) + "###Spectrum (B)";
                drawSpectrum(app, app.viewB, app.decodersB, t.c_str(), true, true);
            }
            {
                std::string t = std::string(_L("Waterfall")) + "###Waterfall (B)";
                drawWaterfall(app, app.viewB, t.c_str());
            }
        }
        drawDecoders(app);
        drawMessages(app);
        drawAbout(app);

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Persist settings + dock layout to signalscope.ini before shutting down.
    ImGui::SaveIniSettingsToDisk(io.IniFilename);

    if (app.startThread.joinable())
        app.startThread.join();

    app.decoders.stop();
    app.decodersB.stop();
    app.sdr.stop();
    app.sdrB.stop();
    app.wav.stop();
    app.server.stop();
    app.hack.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
