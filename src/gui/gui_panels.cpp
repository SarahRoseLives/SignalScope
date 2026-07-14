#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include "core/app.h"
#include "core/main_funcs.h"
#include "decode/band_plan.h"
#include "decode/channel/channel_registry.h"
#include "decode/channel/message_bus.h"
#include "decode/oob/oob_core.h"
#include "i18n/i18n.h"
#include "util/log.h"
#include "version.h"
#include "gui/waterfall.h"
#include <algorithm>
#include <cfloat>
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
#include <shellapi.h>
#endif

// Map the "Decode type" combo index (into ChannelRegistry::all()) to a typeId.
static int typeIdFromComboIndex(int idx)
{
    const auto& all = ChannelRegistry::instance().all();
    if (idx < 0 || idx >= (int)all.size())
        return all.empty() ? 0 : all.front().typeId;
    return all[idx].typeId;
}

// Kick off source startup. Most backends open instantly, but LibreSDR/UHD
// blocks for several seconds in multi_usrp::make() (firmware + FPGA upload), so
// open the device on a worker thread (prepare()) and surface an animated
// "Initializing..." bar. When the worker finishes, the main loop finalizes the
// start on the GUI thread (startActive), so shared app state is only ever
// touched from the render thread.
static void beginStartActive(App& app)
{
#ifdef HAS_LIBRESDR
    if (app.sourceMode == 6)
    {
        if (app.startThread.joinable())
            app.startThread.join();

        // Apply config up front so the worker's open() uses the right settings.
        app.libre.setFpgaImage(app.libreFpgaPath);
        app.libre.setSampleRate(app.libreSampleRateMHz * 1e6);
        app.libre.setCenterFreq(app.centerFreqMHz * 1e6);
        app.libre.setGain((double)app.libreGainDb);
        app.libre.setPort(app.libreAntennaIdx);
        app.libre.setDcBlock(app.dcBlock);

        app.status = "Initializing LibreSDR...";
        app.startErr.clear();
        app.startReady.store(false);
        app.starting.store(true);
        app.startThread = std::thread([&app]() {
            std::string err;
            bool ok = app.libre.prepare(app.deviceIndex, err);
            app.startErr = ok ? std::string()
                              : (err.empty() ? std::string("open failed") : err);
            app.startReady.store(true);
        });
        return;
    }
#endif
    startActive(app);
}

void drawControls(App& app)
{
    ImGui::Begin((std::string(_L("Control")) + "###Control").c_str());

    bool running = app.active->running();
    bool starting = app.starting.load();

    ImGui::BeginDisabled(running || starting);
    {
        // Explicit (label, mode) pairs so indices stay stable regardless of
        // which optional backends are compiled in.
        struct SrcOpt { const char* label; int mode; };
        static const SrcOpt opts[] = {
            {"RTL-SDR", 0}, {"WAV file", 1}, {"SDR++ Server", 2},
            {"HackRF", 3}, {"Dual RTL", 4},
#ifdef HAS_AIRSPY
            {"Airspy", 5},
#endif
#ifdef HAS_LIBRESDR
            {"LibreSDR", 6},
#endif
        };
        const int nOpts = (int)(sizeof(opts) / sizeof(opts[0]));
        const char* cur = "RTL-SDR";
        for (int i = 0; i < nOpts; ++i)
            if (opts[i].mode == app.sourceMode) cur = opts[i].label;
        if (ImGui::BeginCombo(_L("Source"), cur))
        {
            for (int i = 0; i < nOpts; ++i)
                if (ImGui::Selectable(opts[i].label, opts[i].mode == app.sourceMode))
                    app.sourceMode = opts[i].mode;
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    if (starting)
    {
        ImGui::BeginDisabled(true);
        ImGui::Button(_L("Start"), ImVec2(120, 0));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", _L("Initializing LibreSDR"));
        // Indeterminate animated bar: UHD's multi_usrp::make() gives no progress,
        // so a negative fraction drives ImGui's scrolling "busy" animation.
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-FLT_MIN, 0.0f),
                           _L("Loading firmware + FPGA..."));
    }
    else if (!running)
    {
        bool canStart = (app.sourceMode == 1) ? (app.wavPath[0] != '\0') : true;
        ImGui::BeginDisabled(!canStart);
        if (ImGui::Button(_L("Start"), ImVec2(120, 0)))
            beginStartActive(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(app.status.c_str());
    }
    else
    {
        if (ImGui::Button(_L("Stop"), ImVec2(120, 0)))
        {
            app.active->stop();
            app.decoders.stop();
            app.decoders.removeAll();
            if (app.dualMode)
            {
                app.sdrB.stop();
                app.decodersB.stop();
                app.decodersB.removeAll();
            }
            app.dualMode = false;
            app.status = "Idle";
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(app.status.c_str());
    }

    // Version + update banner.
    ImGui::TextDisabled("SignalScope v" SIGNALSCOPE_VERSION);
    {
        VersionCheck::State st = app.verCheck.state();
        if (st == VersionCheck::UpdateAvailable)
        {
            std::string latest = app.verCheck.latestVersion();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "  Update available: v%s",
                               latest.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Get update"))
            {
#if defined(_WIN32)
                std::string url = app.verCheck.productUrl();
                if (url.empty()) url = "https://sarahsforge.dev/login";
                ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
            }
        }
        else if (st == VersionCheck::UpToDate)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "  (up to date)");
        }
        else if (st == VersionCheck::Unreleased)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.5f, 1.0f, 1.0f), "  (unreleased)");
        }
        else if (st == VersionCheck::Checking)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("  checking for updates...");
        }
    }

    ImGui::Separator();

    if (app.sourceMode == 0)
    {
        // ---- RTL-SDR ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.sdr.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());

        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        else
        {
            ImGui::TextDisabled("No RTL-SDR devices. Click Refresh.");
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.sampleRateIdx, kRateLabels, kNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.sdr.setSampleRate(kRates[app.sampleRateIdx]);
        }
        if (ImGui::Checkbox(_L("Auto gain (AGC)"), &app.autoGain))
        {
            if (running)
                app.sdr.setGain(app.autoGain ? -1.0 : (double)app.gainDb);
        }
        if (!app.autoGain)
        {
            if (ImGui::SliderFloat("Gain (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f"))
            {
                if (running)
                    app.sdr.setGain((double)app.gainDb);
            }
        }
        if (ImGui::Checkbox(_L("Bias-T"), &app.biasTee))
        {
            if (running)
                app.sdr.setBiasTee(app.biasTee);
        }
        if (ImGui::InputFloat("PPM", &app.ppm, 0.1f, 1.0f, "%.2f"))
        {
            if (running)
                app.sdr.setPpm((double)app.ppm);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running)
                app.sdr.setDcBlock(app.dcBlock);
        }
    }
    else if (app.sourceMode == 1)
    {
        // ---- WAV file ----
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##wavpath", app.wavPath, sizeof(app.wavPath));
        ImGui::SameLine();
        if (ImGui::Button(_L("Browse...")))
            openWavDialog(app.wavPath, sizeof(app.wavPath));

        if (ImGui::Checkbox(_L("Loop"), &app.wavLoop))
        {
            if (running)
                app.wav.setLoop(app.wavLoop);
        }
        if (ImGui::InputDouble("Center label (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
            app.viewA.resetView = true;

        if (running)
        {
            ImGui::ProgressBar((float)app.wav.progress(), ImVec2(-1, 0));
            ImGui::Text("WAV: %d ch, %d-bit, %.1f kHz",
                        app.wav.channels(), app.wav.bits(), app.wav.sampleRate() / 1e3);
        }
    }
    else if (app.sourceMode == 2)
    {
        // ---- SDR++ Server ----
        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("Host", app.serverHost, sizeof(app.serverHost));
        ImGui::InputInt("Port", &app.serverPort);
        const char* sampleTypes[] = {"int8 (low BW)", "int16", "float32 (high BW)"};
        ImGui::Combo("Sample type", &app.serverSampleType, sampleTypes, 3);
        ImGui::Checkbox("Compression (zstd)", &app.serverCompression);
        ImGui::EndDisabled();

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.server.setCenterFreq(app.centerFreqMHz * 1e6);
        }

        // Sample-rate combo, populated from the server's source-module UI once
        // connected. Selecting one drives the server's rate via a UI action.
        if (running)
        {
            auto labels = app.server.sampleRateLabels();
            auto values = app.server.sampleRateValues();
            int curIdx = app.server.currentSampleRateIndex();
            if (!labels.empty())
            {
                const char* preview = (curIdx >= 0 && curIdx < (int)labels.size())
                                          ? labels[curIdx].c_str()
                                          : "(select)";
                if (ImGui::BeginCombo("Sample rate", preview))
                {
                    for (int i = 0; i < (int)labels.size(); ++i)
                    {
                        bool sel = (i == curIdx);
                        if (ImGui::Selectable(labels[i].c_str(), sel) && i < (int)values.size())
                        {
                            app.server.setSampleRate(values[i]);
                            app.viewA.resetView = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                ImGui::TextDisabled("Sample rate: (no rate control exposed by server)");
            }
            ImGui::Text("Server sample rate: %.4f MHz", app.server.sampleRate() / 1e6);
        }
        else
        {
            ImGui::TextDisabled("Connect to list the server's sample rates.");
        }
        ImGui::TextDisabled("Gain and device are configured on the SDR++ server.");
    }
    else if (app.sourceMode == 3)
    {
        // ---- HackRF (native) ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.hack.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.hack.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::InputDouble("Sample rate (MHz)", &app.hackSampleRateMHz, 1.0, 2.0, "%.3f"))
        {
            if (app.hackSampleRateMHz < 2.0) app.hackSampleRateMHz = 2.0;
            if (app.hackSampleRateMHz > 20.0) app.hackSampleRateMHz = 20.0;
            app.viewA.resetView = true;
            if (running)
                app.hack.setSampleRate(app.hackSampleRateMHz * 1e6);
        }
        if (ImGui::SliderInt("LNA (IF) dB", &app.hackLna, 0, 40, "%d"))
        {
            app.hackLna = (app.hackLna / 8) * 8;
            if (running) app.hack.setLnaGain(app.hackLna);
        }
        if (ImGui::SliderInt("VGA (BB) dB", &app.hackVga, 0, 62, "%d"))
        {
            app.hackVga = (app.hackVga / 2) * 2;
            if (running) app.hack.setVgaGain(app.hackVga);
        }
        if (ImGui::Checkbox("RF amp (+~11 dB)", &app.hackAmp))
        {
            if (running) app.hack.setAmpEnable(app.hackAmp);
        }
        if (ImGui::Checkbox("Bias-T (antenna power)", &app.hackBias))
        {
            if (running) app.hack.setBiasTee(app.hackBias);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.hack.setDcBlock(app.dcBlock);
        }
    }
#ifdef HAS_AIRSPY
    else if (app.sourceMode == 5)
    {
        // ---- Airspy (native) ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.airspy.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.airspy.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.airspySampleRateIdx, kAirspyRateLabels, kAirspyNumRates))
        {
            app.viewA.resetView = true;
            if (running)
                app.airspy.setSampleRate(kAirspyRates[app.airspySampleRateIdx]);
        }

        ImGui::Separator();
        if (ImGui::RadioButton("Sensitive", app.airspyGainMode == 0)) app.airspyGainMode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Linear", app.airspyGainMode == 1)) app.airspyGainMode = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Free", app.airspyGainMode == 2)) app.airspyGainMode = 2;

        if (app.airspyGainMode == 0)
        {
            if (ImGui::SliderInt("Sensitivity gain", &app.airspySenseGain, 0, 21))
            {
                if (running) app.airspy.setGainMode(0), app.airspy.setSenseGain(app.airspySenseGain);
            }
        }
        else if (app.airspyGainMode == 1)
        {
            if (ImGui::SliderInt("Linearity gain", &app.airspyLinearGain, 0, 21))
            {
                if (running) app.airspy.setGainMode(1), app.airspy.setLinearGain(app.airspyLinearGain);
            }
        }
        else
        {
            if (ImGui::Checkbox("LNA AGC", &app.airspyLnaAgc))
            {
                if (running) app.airspy.setLnaAgc(app.airspyLnaAgc);
            }
            ImGui::BeginDisabled(app.airspyLnaAgc);
            if (ImGui::SliderInt("LNA gain", &app.airspyLnaGain, 0, 15))
            {
                if (running) app.airspy.setLnaGain(app.airspyLnaGain);
            }
            ImGui::EndDisabled();

            if (ImGui::Checkbox("Mixer AGC", &app.airspyMixerAgc))
            {
                if (running) app.airspy.setMixerAgc(app.airspyMixerAgc);
            }
            ImGui::BeginDisabled(app.airspyMixerAgc);
            if (ImGui::SliderInt("Mixer gain", &app.airspyMixerGain, 0, 15))
            {
                if (running) app.airspy.setMixerGain(app.airspyMixerGain);
            }
            ImGui::EndDisabled();

            if (ImGui::SliderInt("VGA gain", &app.airspyVgaGain, 0, 15))
            {
                if (running) app.airspy.setVgaGain(app.airspyVgaGain);
            }
        }
        if (ImGui::Checkbox("Bias T (antenna power)", &app.airspyBias))
        {
            if (running) app.airspy.setBiasTee(app.airspyBias);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.airspy.setDcBlock(app.dcBlock);
        }
    }
#endif
#ifdef HAS_LIBRESDR
    else if (app.sourceMode == 6)
    {
        // ---- LibreSDR (USRP B210 clone via UHD) ----
        if (ImGui::Button(_L("Refresh devices")))
            app.devices = app.libre.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name +
                                        " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel))
                        app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }

        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("FPGA image", app.libreFpgaPath, sizeof(app.libreFpgaPath));
        ImGui::EndDisabled();

        if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
        {
            app.viewA.resetView = true;
            if (running)
                app.libre.setCenterFreq(app.centerFreqMHz * 1e6);
        }
        if (ImGui::Combo(_L("Sample rate (MHz)"), &app.libreSampleRateIdx, kLibreRateLabels, kLibreNumRates))
        {
            app.libreSampleRateMHz = kLibreRates[app.libreSampleRateIdx];
            app.viewA.resetView = true;
            if (running)
                app.libre.setSampleRate(app.libreSampleRateMHz * 1e6);
        }
        const char* libreAnts[] = {"TRXA", "RXA", "TRXB", "RXB"};
        ImGui::BeginDisabled(running);
        ImGui::Combo("Antenna", &app.libreAntennaIdx, libreAnts, 4);
        ImGui::EndDisabled();
        if (ImGui::SliderFloat("Gain (dB)", &app.libreGainDb, 0.0f, 76.0f, "%.1f"))
        {
            if (running)
                app.libre.setGain((double)app.libreGainDb);
        }
        if (ImGui::Checkbox(_L("DC block"), &app.dcBlock))
        {
            if (running) app.libre.setDcBlock(app.dcBlock);
        }
    }
#endif
    if (app.sourceMode == 4)
    {
        // ---- Dual RTL: two independent RTL-SDRs ----
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "RTL A (Spectrum / Waterfall A)");
        ImGui::Separator();
        if (ImGui::Button("Refresh A"))
            app.devices = app.sdr.listDevices();
        ImGui::SameLine();
        ImGui::Text("(%d found)", (int)app.devices.size());
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndex, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device A", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndex == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name + " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel)) app.deviceIndex = i;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputDouble("Center A (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.4f"))
            app.viewA.resetView = true;
        ImGui::Combo("Rate A (MHz)", &app.sampleRateIdx, kRateLabels, kNumRates);
        if (ImGui::Checkbox("Auto gain A", &app.autoGain)) {}
        if (!app.autoGain)
            ImGui::SliderFloat("Gain A (dB)", &app.gainDb, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Bias-T A", &app.biasTee);
        ImGui::InputFloat("PPM A", &app.ppm, 0.1f, 1.0f, "%.2f");
        ImGui::Checkbox("DC block A", &app.dcBlock);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "RTL B (Spectrum / Waterfall B)");
        ImGui::Separator();
        if (ImGui::Button("Refresh B"))
            app.devices = app.sdrB.listDevices();
        if (!app.devices.empty())
        {
            std::string preview = app.devices[std::min(app.deviceIndexB, (int)app.devices.size() - 1)].name;
            if (ImGui::BeginCombo("Device B", preview.c_str()))
            {
                for (int i = 0; i < (int)app.devices.size(); ++i)
                {
                    bool sel = (app.deviceIndexB == i);
                    std::string label = std::to_string(i) + ": " + app.devices[i].name + " [" + app.devices[i].serial + "]";
                    if (ImGui::Selectable(label.c_str(), sel)) app.deviceIndexB = i;
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::InputDouble("Center B (MHz)", &app.centerFreqMHzB, 0.1, 1.0, "%.4f"))
            app.viewB.resetView = true;
        ImGui::Combo("Rate B (MHz)", &app.sampleRateIdxB, kRateLabels, kNumRates);
        if (ImGui::Checkbox("Auto gain B", &app.autoGainB)) {}
        if (!app.autoGainB)
            ImGui::SliderFloat("Gain B (dB)", &app.gainDbB, 0.0f, 50.0f, "%.1f");
        ImGui::Checkbox("Bias-T B", &app.biasTeeB);
        ImGui::InputFloat("PPM B", &app.ppmB, 0.1f, 1.0f, "%.2f");
        ImGui::Checkbox("DC block B", &app.dcBlock); // same dcblock toggle
    }

    ImGui::Separator();
    ImGui::Combo("FFT size", &app.fftSizeIdx, kFftLabels, kNumFftSizes);
    ImGui::SliderFloat(_L("Averaging"), &app.avgAlpha, 0.0f, 0.98f, "%.2f");
    ImGui::Checkbox(_L("Auto-scale dB"), &app.autoScale);
    ImGui::SliderFloat(_L("dB min"), &app.dbMin, -140.0f, 0.0f, "%.0f");
    ImGui::SliderFloat(_L("dB max"), &app.dbMax, -140.0f, 20.0f, "%.0f");
    if (app.dbMax < app.dbMin + 5.0f)
        app.dbMax = app.dbMin + 5.0f;

    if (ImGui::Button(_L("Reset view (fit band)")))
    {
        app.viewA.resetView = true;
        app.viewB.resetView = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("drag=pan  scroll=zoom  dbl-click=fit");

    ImGui::BeginDisabled(app.sourceMode == 1);
    ImGui::Checkbox("Pan/scroll retunes SDR (browse band)", &app.bandBrowse);
    ImGui::EndDisabled();
    if (app.sourceMode == 1)
        ImGui::TextDisabled("  (WAV: tuning is fixed to the file)");

    // Band plan bar along bottom of spectrum
    if (ImGui::Checkbox(_L("Band Plan"), &app.showBandPlan));
    if (app.showBandPlan)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload##bpr"))
            scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
        ImGui::SameLine();
        if (ImGui::SmallButton("Folder##bpf"))
        {
#if defined(_WIN32)
            ShellExecuteA(nullptr, "open", app.bandPlanDir, nullptr, nullptr, SW_SHOW);
#endif
        }
        if (app.bandPlanNames.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(no .json bandplans in bandplans/)");
        }
        else
        {
            if (app.bandPlanIdx >= (int)app.bandPlanNames.size())
                app.bandPlanIdx = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bplan-sel", &app.bandPlanIdx,
                             [](void* data, int idx) -> const char* {
                                 auto& v = *(std::vector<std::string>*)data;
                                 return idx >= 0 && idx < (int)v.size() ? v[idx].c_str() : "";
                             },
                             &app.bandPlanNames, (int)app.bandPlanNames.size()))
            {
                if (app.bandPlanIdx >= 0 && app.bandPlanIdx < (int)app.bandPlanPaths.size())
                    app.bandPlanLoaded = loadBandPlan(app.bandPlanPaths[app.bandPlanIdx]);
            }
        }
    }

    if (app.dualMode)
    {
        if (ImGui::Checkbox(_L("Band Plan (B)"), &app.showBandPlanB));
        if (app.showBandPlanB && !app.bandPlanNames.empty())
        {
            if (app.bandPlanIdxB >= (int)app.bandPlanNames.size())
                app.bandPlanIdxB = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bplan-sel-b", &app.bandPlanIdxB,
                             [](void* data, int idx) -> const char* {
                                 auto& v = *(std::vector<std::string>*)data;
                                 return idx >= 0 && idx < (int)v.size() ? v[idx].c_str() : "";
                             },
                             &app.bandPlanNames, (int)app.bandPlanNames.size()))
            {
                if (app.bandPlanIdxB >= 0 && app.bandPlanIdxB < (int)app.bandPlanPaths.size())
                    app.bandPlanLoadedB = loadBandPlan(app.bandPlanPaths[app.bandPlanIdxB]);
            }
        }
    }

    ImGui::Separator();
    {
        const auto& reg = ChannelRegistry::instance().all();
        if (app.newTypeIdx < 0 || app.newTypeIdx >= (int)reg.size())
            app.newTypeIdx = 0;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.18f, 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.28f, 0.60f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.12f, 0.22f, 0.50f, 1.0f));

        const char* curLabel = (app.newTypeIdx < (int)reg.size()) ? reg[app.newTypeIdx].name.c_str() : "";
        if (ImGui::BeginCombo(_L("Decode type"), curLabel))
        {
            std::string lastCat;
            for (int i = 0; i < (int)reg.size(); i++)
            {
                // Insert category header if it changed
                if (!reg[i].category.empty() && reg[i].category != lastCat)
                {
                    lastCat = reg[i].category;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.6f, 0.8f, 1.0f));
                    ImGui::TextDisabled("%s", lastCat.c_str());
                    ImGui::PopStyleColor();
                    ImGui::Separator();
                }
                // Indented decoder entry
                char label[128];
                snprintf(label, sizeof(label), "    %s", reg[i].name.c_str());
                if (ImGui::Selectable(label, i == app.newTypeIdx))
                    app.newTypeIdx = i;
                if (i == app.newTypeIdx)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::TextDisabled("Ctrl+click the spectrum to add a decoder there");

    ImGui::Separator();
    if (ImGui::CollapsingHeader(_L("Display")))
    {
        if (ImGui::SliderInt(_L("Font size"), &app.fontSize, 8, 24, "%d", ImGuiSliderFlags_AlwaysClamp))
        {
            if (app.fontSize < 8)  app.fontSize = 8;
            if (app.fontSize > 24) app.fontSize = 24;
        }
        ImGui::TextDisabled("  Restart to apply");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("IQ Recorder"))
    {
        ImGui::SetNextItemWidth(-70.0f);
        ImGui::InputText("IQ file", app.iqRecPath, sizeof(app.iqRecPath));
        // Pre-buffer slider (disabled above 3 Msps)
        double fs = app.active->running() ? app.active->sampleRate() : 0.0;
        if (!app.active->running())
            ImGui::BeginDisabled();
        bool overLimit = (fs > 3.0e6);
        if (overLimit)
            ImGui::BeginDisabled();
        if (ImGui::SliderFloat("Pre-buffer (s)", &app.iqBufferSec, 0.0f, 60.0f, "%.0f"))
        {
            app.iqRecorder.configurePrebuffer(fs, app.iqBufferSec);
        }
        if (overLimit)
        {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(disabled > 3 Msps)");
        }
        else if (app.iqBufferSec > 0.0f && fs > 0.0)
        {
            size_t bytes = (size_t)(fs * app.iqBufferSec * 2 * sizeof(float));
            char mem[32];
            if (bytes >= 1024 * 1024 * 1024)
                std::snprintf(mem, sizeof(mem), "(~%.1f GB)", (double)bytes / (1024.0 * 1024.0 * 1024.0));
            else
                std::snprintf(mem, sizeof(mem), "(~%.0f MB)", (double)bytes / (1024.0 * 1024.0));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mem);
        }
        if (!app.active->running())
            ImGui::EndDisabled();
        bool iqRec = app.iqRecorder.isRecording();
        if (iqRec)
        {
            if (ImGui::Button("Stop##iqrec"))
                app.iqRecorder.stop();
        }
        else
        {
            if (ImGui::Button("Start##iqrec"))
            {
                if (app.active && app.active->running())
                    app.iqRecorder.start(app.iqRecPath, app.active->sampleRate());
            }
        }
        if (app.iqRecorder.isRecording())
        {
            double sec = app.iqRecorder.elapsed();
            int m = (int)(sec / 60), s = (int)(sec) % 60;
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "REC %02d:%02d  —  %s",
                               m, s, app.iqRecorder.path().c_str());
        }
    }

    if (running)
    {
        if (app.sourceMode == 0)
        {
            double maxF = app.sdr.tunerMaxFreq();
            if (maxF > 0.0 && app.centerFreqMHz * 1e6 > maxF)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 90, 90, 255));
                ImGui::TextWrapped("  WARNING: %.0f MHz is above this tuner's ~%.0f MHz "
                                   "ceiling. The PLL can't lock here - pick a lower "
                                   "frequency or use an R820T2/R828D dongle.",
                                   app.centerFreqMHz, maxF / 1e6);
                ImGui::PopStyleColor();
            }
        }
    }

    ImGui::End();
}

void drawSpectrum(App& app, SpectrumView& v, DecoderManager& mgr, const char* title,
                         bool allowBandBrowse, bool voiceView)
{
    ImGui::Begin(title);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float availW = ImGui::GetContentRegionAvail().x;
    std::string plotId = std::string("##plot_") + title;
    if (ImPlot::BeginPlot(plotId.c_str(), ImVec2(-1, -1), ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes("MHz", "dB", 0, 0);

        bool bandValid = (v.curN > 0 && v.freqMHz.front() < v.freqMHz.back());
        if (bandValid)
        {
            double bandSpan = v.freqMHz.back() - v.freqMHz.front();
            ImPlot::SetupAxisZoomConstraints(ImAxis_X1, bandSpan * 1e-4, bandSpan);
        }
        if (v.resetView && bandValid)
            ImPlot::SetupAxisLimits(ImAxis_X1, v.freqMHz.front(), v.freqMHz.back(), ImGuiCond_Always);
        if (app.autoScale || v.resetView)
            ImPlot::SetupAxisLimits(ImAxis_Y1, app.dbMin, app.dbMax, ImGuiCond_Always);

        if (v.curN > 0)
        {
            ImPlot::PlotLine("PSD", v.freqMHz.data(), v.avg.data(), v.curN);
        }

        auto decs = mgr.status();
        for (auto& d : decs)
        {
            double x = d.freqMHz;
            ImVec4 col = d.locked ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f)  // green = locked
                                  : ImVec4(0.9f, 0.7f, 0.2f, 1.0f); // orange = unlocked
            if (ImPlot::DragLineX(d.channelId, &x, col, 2.0f))
                mgr.setDecoderFreq(d.channelId, x * 1e6);
        }

        ImPlotRect lim = ImPlot::GetPlotLimits();
        v.viewXminMHz = lim.X.Min;
        v.viewXmaxMHz = lim.X.Max;

        // Band-browse retuning: in dual mode use explicit SDR pointers,
        // otherwise use app.active (which covers RTL/WAV/SDR++/HackRF).
        SdrSource* browseSdr;
        if (app.dualMode)
            browseSdr = voiceView ? static_cast<SdrSource*>(&app.sdrB)
                                  : static_cast<SdrSource*>(&app.sdr);
        else
            browseSdr = app.active;
        if (allowBandBrowse && app.bandBrowse && app.sourceMode != 1 &&
            browseSdr->running() && !v.resetView)
        {
            double viewCtr = 0.5 * (v.viewXminMHz + v.viewXmaxMHz);
            double viewHalf = 0.5 * (v.viewXmaxMHz - v.viewXminMHz);
            double sdrCtr = browseSdr->centerFreq() / 1e6;
            double fsMHz = browseSdr->sampleRate() / 1e6;
            double halfBand = 0.5 * fsMHz;
            double marginL = (viewCtr - viewHalf) - (sdrCtr - halfBand);
            double marginR = (sdrCtr + halfBand) - (viewCtr + viewHalf);
            double minMargin = std::min(marginL, marginR);
            double trigger = fsMHz * (app.browseEdgePct * 0.01);
            bool moved = std::fabs(viewCtr - app.lastRetuneCtr) > fsMHz * (app.browseMinMovePct * 0.01);
            auto now = std::chrono::steady_clock::now();
            double sinceMs =
                std::chrono::duration<double, std::milli>(now - app.lastRetune).count();
            if (fsMHz > 0.0 && minMargin < trigger && moved && sinceMs > app.browseThrottleMs)
            {
                if (voiceView)
                {
                    // Retune SDR B preserving decoders on manager B
                    std::vector<std::pair<double, int>> keep;
                    for (auto& s : app.decodersB.status())
                        keep.push_back({s.freqMHz, s.typeId});
                    app.centerFreqMHzB = viewCtr;
                    app.sdrB.setCenterFreq(viewCtr * 1e6);
                    app.decodersB.removeAll();
                    app.decodersB.configure(app.sdrB.sampleRate(), app.sdrB.centerFreq());
                    for (auto& k : keep)
                        app.decodersB.addDecoder(k.first * 1e6, k.second);
                }
                else
                {
                    retunePreserving(app, viewCtr);
                }
                app.lastRetune = now;
                app.lastRetuneCtr = viewCtr;
            }
        }

        ImVec2 pp = ImPlot::GetPlotPos();
        ImVec2 ps = ImPlot::GetPlotSize();
        v.specLeftInset = pp.x - origin.x;
        v.specRightInset = (origin.x + availW) - (pp.x + ps.x);

        if (bandValid)
            v.resetView = false;

        // Drag-to-place decoder: Ctrl+mousedown starts placing, move shows a white
        // preview line through the spectrum and waterfall, release creates the decoder.
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().KeyCtrl)
        {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                app.placingDecoder = true;
                app.placingVoiceView = voiceView;
                app.placingFreqMHz = mp.x;
            }
        if (app.placingDecoder && app.placingVoiceView == voiceView)
            {
                app.placingFreqMHz = mp.x;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && app.placingDecoder)
            {
                app.placingDecoder = false;
                mgr.addDecoder(mp.x * 1e6, typeIdFromComboIndex(app.newTypeIdx));
            }
        }
        else if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            // Ctrl released or cursor left the plot that started the placement.
            app.placingDecoder = false;
        }

        // Drag-to-place preview line redraw
        if (app.placingDecoder && app.placingVoiceView == voiceView)
        {
            ImPlotRect lim = ImPlot::GetPlotLimits();
            float xMin = (float)lim.X.Min;
            float xMax = (float)lim.X.Max;
            if (xMax > xMin)
            {
                float frac = ((float)app.placingFreqMHz - xMin) / (xMax - xMin);
                ImVec2 pp = ImPlot::GetPlotPos();
                ImVec2 ps = ImPlot::GetPlotSize();
                float px = pp.x + frac * ps.x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddLine(ImVec2(px, pp.y), ImVec2(px, pp.y + ps.y),
                            IM_COL32(255, 40, 40, 200), 1.5f);
            }
        }

        // --- Band plan: solid coloured bar along the bottom ---
        bool showBp = voiceView ? app.showBandPlanB : app.showBandPlan;
        const BandPlan& bp = voiceView ? app.bandPlanLoadedB : app.bandPlanLoaded;
        if (showBp && bp.valid && v.curN > 0)
        {
            const ImPlotRect vp = ImPlot::GetPlotLimits();
            double viewLo = vp.X.Min, viewHi = vp.X.Max;
            if (viewHi <= viewLo) { viewLo = v.freqMHz.front(); viewHi = v.freqMHz.back(); }
            auto* dl = ImPlot::GetPlotDrawList();
            constexpr float kBandH = 28.0f;
            ImVec2 pp = ImPlot::GetPlotPos(), ps = ImPlot::GetPlotSize();
            float bandTop = pp.y + ps.y - kBandH;
            float bandBot = bandTop + kBandH;
            float pxPerMHz = (float)(ps.x / (viewHi - viewLo));
            for (auto& e : bp.entries)
            {
                if (e.hiMHz < viewLo || e.loMHz > viewHi) continue;
                float loPx = pp.x + (float)((std::max(e.loMHz, viewLo) - viewLo) * pxPerMHz);
                float hiPx = pp.x + (float)((std::min(e.hiMHz, viewHi) - viewLo) * pxPerMHz);
                dl->AddRectFilled(ImVec2(loPx, bandTop), ImVec2(hiPx, bandBot), e.color);
                float segW = hiPx - loPx;
                if (segW > 50 && !e.label.empty())
                {
                    float lw = ImGui::CalcTextSize(e.label.c_str()).x;
                    if (lw < segW - 4)
                    {
                        float cx = loPx + (segW - lw) * 0.5f;
                        float cy = bandTop + (kBandH - ImGui::GetTextLineHeight()) * 0.5f;
                        dl->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 230), e.label.c_str());
                    }
                }
            }
        }

        ImPlot::EndPlot();
        v.fftSkip = false;
    }
    ImGui::End();
}

void drawWaterfall(App& app, SpectrumView& v, const char* title)
{
    (void)app;
    ImGui::Begin(title);

    float uMin = 0.0f, uMax = 1.0f;
    float xLo = 0.0f, xHi = 1.0f;
    if (v.curN > 0)
    {
        double bandMin = v.freqMHz.front();
        double bandMax = v.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = v.viewXmaxMHz - v.viewXminMHz;
        if (bandSpan > 0.0 && viewSpan > 0.0)
        {
            double visLo = std::max(bandMin, v.viewXminMHz);
            double visHi = std::min(bandMax, v.viewXmaxMHz);
            if (visHi > visLo)
            {
                uMin = (float)((visLo - bandMin) / bandSpan);
                uMax = (float)((visHi - bandMin) / bandSpan);
                xLo = (float)((visLo - v.viewXminMHz) / viewSpan);
                xHi = (float)((visHi - v.viewXminMHz) / viewSpan);
            }
            else
            {
                xLo = xHi = 0.0f;
            }
        }
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float left = std::max(0.0f, v.specLeftInset);
    float right = std::max(0.0f, v.specRightInset);
    float w = avail.x - left - right;
    if (w < 1.0f)
    {
        w = avail.x;
        left = 0.0f;
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left);

    ImVec2 wfP0 = ImGui::GetCursorScreenPos();
    v.waterfall.draw(ImVec2(w, avail.y), uMin, uMax, xLo, xHi);
    v.fftSkip = false;

    // Drag-to-place preview line: white vertical line through the waterfall
    // at the frequency the user is hovering, so they can centre on a signal.
    if (app.placingDecoder && app.placingVoiceView == (&v == &app.viewB) && v.curN > 0)
    {
        double bandMin = v.freqMHz.front();
        double bandMax = v.freqMHz.back();
        double bandSpan = bandMax - bandMin;
        double viewSpan = v.viewXmaxMHz - v.viewXminMHz;
        double visLo = std::max(bandMin, v.viewXminMHz);
        double visHi = std::min(bandMax, v.viewXmaxMHz);
        if (bandSpan > 0 && viewSpan > 0 &&
            app.placingFreqMHz >= visLo && app.placingFreqMHz <= visHi)
        {
            float u = (float)((app.placingFreqMHz - visLo) / (visHi - visLo));
            float pixFrac = xLo + u * (xHi - xLo);
            float px = wfP0.x + pixFrac * w;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(px, wfP0.y), ImVec2(px, wfP0.y + avail.y),
                        IM_COL32(255, 40, 40, 200), 1.5f);
        }
    }
    ImGui::End();
}

void drawDecoders(App& app)
{
    ImGui::Begin((std::string(_L("Decoders")) + "###Decoders").c_str());

    auto decs = app.decoders.status();
    if (app.dualMode)
    {
        auto decsB = app.decodersB.status();
        for (auto& d : decsB) d.isB = true;
        decs.insert(decs.end(), decsB.begin(), decsB.end());
    }
    ImGui::Text("%d active  |  %d sub-band(s)  %d threads", (int)decs.size(),
                app.decoders.subbandCount() + (app.dualMode ? app.decodersB.subbandCount() : 0),
                app.decoders.workerCount() + (app.dualMode ? app.decodersB.workerCount() : 0));
    uint64_t drops = app.decoders.drops() + (app.dualMode ? app.decodersB.drops() : 0);
    ImGui::SameLine();
    if (drops > 0)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "  drops: %llu",
                           (unsigned long long)drops);
    else
        ImGui::TextDisabled("  drops: 0");
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Remove all")))
    {
        app.decoders.removeAll();
        if (app.dualMode) app.decodersB.removeAll();
    }

    if (ImGui::Checkbox("Save decoders on restart", &app.saveDecoders))
    {
    }

    // ---- Audio output (WFM listening) ----
    ImGui::Separator();
    if (!app.audioEnabled)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Audio device unavailable");
    else
    {
        if (ImGui::Checkbox("Mute", &app.audioMuted))
            app.audio.setMuted(app.audioMuted);
        ImGui::SameLine();
        // Level meter (peak with decay) next to Mute.
        float lvl = app.audio.level();
        ImVec4 mcol = lvl > 0.95f ? ImVec4(1.0f, 0.3f, 0.2f, 1.0f)   // clip - red
                                  : ImVec4(0.3f, 0.9f, 0.4f, 1.0f);  // normal - green
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, mcol);
        ImGui::ProgressBar(lvl, ImVec2(120.0f, 0.0f), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##vol", &app.audioVolume, 0.0f, 1.0f, "Vol %.2f"))
            app.audio.setVolume(app.audioVolume);
        int ac = app.decoders.audioChannel();
        if (ac != 0)
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Listening: ch %d", ac);
        else
            ImGui::TextDisabled("Drop a WFM/AM/NFM channel on the spectrum to hear audio");
    }

    ImGui::Separator();

    if (ImGui::BeginTable("##decs", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn("Freq MHz");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        int toRemove = -1;
    bool toRemoveB = false;
        for (auto& d : decs)
        {
            int uid = d.channelId + (d.isB ? 100000 : 0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImVec4 c = d.locked ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f)
                                : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImGui::TextColored(c, "%s", d.locked ? "LOCK" : "--");
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", d.freqMHz);
            ImGui::TableNextColumn();
            {
                const ChannelDecoderInfo* info = ChannelRegistry::instance().byType(d.typeId);
                ImGui::TextUnformatted(info ? info->shortLabel.c_str() : "?");
            }
            ImGui::TableNextColumn();
            if (d.isAudio && !d.isB)
            {
                char lbtn[24];
                std::snprintf(lbtn, sizeof(lbtn), "%s##L%d", d.audioOn ? "Playing" : "Play", uid);
                if (d.audioOn)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.3f, 1.0f));
                if (ImGui::SmallButton(lbtn))
                    app.decoders.setAudioChannel(d.channelId);
                if (d.audioOn)
                    ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            char btn[24];
            std::snprintf(btn, sizeof(btn), "X##%d", uid);
            if (ImGui::SmallButton(btn))
            {
                toRemove = d.channelId;
                toRemoveB = d.isB;
            }
        }
        ImGui::EndTable();
        if (toRemove >= 0)
        {
            if (toRemoveB) app.decodersB.removeDecoder(toRemove);
            else           app.decoders.removeDecoder(toRemove);
        }
    }

    ImGui::End();
}

// Generic decoded-messages panel, fed by every decoder via the MessageBus.
void drawMessages(App& app)
{
    ImGui::Begin((std::string(_L("Pager")) + "###Messages").c_str());

    unsigned long long total = app.decoders.bus().count();
    if (app.dualMode) total += app.decodersB.bus().count();
    ImGui::Text("%llu total", total);
    ImGui::SameLine();
    if (ImGui::SmallButton(_L("Clear")))
    {
        app.decoders.bus().clear();
        if (app.dualMode) app.decodersB.bus().clear();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##searchmsg", "Search...", app.searchBuf, sizeof(app.searchBuf));

    ImGui::Separator();

    auto msgs = app.decoders.bus().snapshot();
    if (app.dualMode)
    {
        auto b = app.decodersB.bus().snapshot();
        msgs.insert(msgs.end(), b.begin(), b.end());
    }
    std::sort(msgs.begin(), msgs.end(),
              [](const DecodedRecord& a, const DecodedRecord& b) { return a.timeSec > b.timeSec; });
    std::string searchLower;
    bool hasSearch = (app.searchBuf[0] != 0);
    if (hasSearch)
    {
        searchLower = app.searchBuf;
        for (auto& ch : searchLower) ch = (char)std::tolower((unsigned char)ch);
    }
    if (ImGui::BeginTable("##busmsgs", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Freq", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Message");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (auto it = msgs.begin(); it != msgs.end(); ++it)
        {
            // Only show pager messages (POCSAG / FLEX) in this panel
            if (it->source != "POCSAG" && it->source != "FLEX")
                continue;
            if (hasSearch)
            {
                std::string hay = it->text + "|" + it->source;
                for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
                if (hay.find(searchLower) == std::string::npos)
                    continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", it->freqMHz);
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", it->source.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it->text.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void drawEpg(App& app)
{
    ImGui::Begin((std::string(_L("EPG")) + "###EPG").c_str());

    auto snap = oob::EpgStore::instance().snapshot();

    if (snap.gotLock)
    {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.35f, 1.0f), "LOCKED");
        ImGui::SameLine();
        ImGui::Text(" — %d SF, %d cells (clean:%d corr:%d fail:%d)",
                    snap.totalSuperframes, snap.totalCells,
                    snap.cleanCells, snap.correctedCells, snap.failedCells);
        ImGui::Text("HEC pass: %d  fail: %d  |  Flow: VPI=%02X VCI=%04X  |  AAL5:%d  IP:%d  DSMCC:%d",
                    snap.hecPass, snap.hecFail,
                    snap.carouselVpi, snap.carouselVci,
                    snap.aal5Frames, snap.ipPkts, snap.dsmccMods);
        if (snap.lastUpdate > 0.0) {
            double age = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count() - snap.lastUpdate;
            ImGui::SameLine();
            ImGui::TextDisabled("  %.0fs ago", age);
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "WAITING FOR LOCK");
    }

    ImGui::Separator();

    // Channel Lineup — always visible
    if (ImGui::CollapsingHeader("Channel Lineup", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("%d channels", (int)snap.channels.size());
        if (!snap.channels.empty()) {
            if (ImGui::Button("Copy CSV##epg")) {
                std::string csv;
                for (const auto& ch : snap.channels)
                    csv += ch.first + "," + std::to_string(ch.second) + "\n";
                ImGui::SetClipboardText(csv.c_str());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##epgclr")) {
                auto s = oob::EpgStore::instance().snapshot();
                s.channels.clear();
                oob::EpgStore::instance().update(std::move(s));
            }
        }
        if (ImGui::BeginTable("##epgchan", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Channel #");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            if (snap.channels.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("—");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("—");
            }
            for (const auto& ch : snap.channels)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ch.first.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", ch.second);
            }
            ImGui::EndTable();
        }
    }

    // Service Names — always visible
    if (ImGui::CollapsingHeader("Service Names"))
    {
        ImGui::Text("%d names", (int)snap.serviceNames.size());
        if (ImGui::BeginTable("##epgsvc", 1,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            if (snap.serviceNames.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("—");
            }
            for (const auto& s : snap.serviceNames)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(s.c_str());
            }
            ImGui::EndTable();
        }
    }

    // MAC Control Channel — VCI 0x0021 provisioning/config messages
    if (ImGui::CollapsingHeader("MAC Control"))
    {
        ImGui::Text("%zu chars", snap.macText.size());
        if (!snap.macText.empty()) {
            if (ImGui::Button("Copy##mac"))
                ImGui::SetClipboardText(snap.macText.c_str());
        }
        ImGui::BeginChild("##macdata", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::TextUnformatted(snap.macText.c_str());
        ImGui::EndChild();
    }

    // Host Config — OCAP bootstrap (VCI 0x0FA2)
    if (ImGui::CollapsingHeader("Host Config"))
    {
        ImGui::Text("%zu chars", snap.hostConfigText.size());
        if (!snap.hostConfigText.empty()) {
            if (ImGui::Button("Copy##hostcfg"))
                ImGui::SetClipboardText(snap.hostConfigText.c_str());
        }
        ImGui::BeginChild("##hostcfgdata", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::TextUnformatted(snap.hostConfigText.c_str());
        ImGui::EndChild();
    }

    // EPG Data — decompressed BIOP content (show names, times, descriptions)
    if (ImGui::CollapsingHeader("EPG Data"))
    {
        ImGui::Text("%zu chars", snap.epgText.size());
        if (!snap.epgText.empty()) {
            if (ImGui::Button("Copy##epgdata"))
                ImGui::SetClipboardText(snap.epgText.c_str());
        }
        ImGui::BeginChild("##epgdata", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::TextUnformatted(snap.epgText.c_str());
        ImGui::EndChild();
    }

    // Readable Strings — always visible
    if (ImGui::CollapsingHeader("Readable Strings"))
    {
        ImGui::Text("%d unique strings", (int)snap.readableStrings.size());
        if (!snap.readableStrings.empty()) {
            if (ImGui::Button("Copy All##epgstr"))
            {
                std::string all;
                for (const auto& s : snap.readableStrings) all += s + "\n";
                ImGui::SetClipboardText(all.c_str());
            }
        }
        ImGui::BeginChild("##epgstrlist", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (const auto& s : snap.readableStrings)
            ImGui::TextUnformatted(s.c_str());
        ImGui::EndChild();
    }

    ImGui::End();
}

void drawHdRadio(App& app)
{
#ifdef HAS_NRSC5
    ImGui::Begin((std::string(_L("HD Radio")) + "###HDRadio").c_str());

    auto info = oob::Nrsc5Store::instance().snapshot();

    ImVec4 syncCol = info.sync ? ImVec4(0.2f, 1.0f, 0.35f, 1.0f)
                               : ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
    ImGui::TextColored(syncCol, "%s", info.sync ? "LOCKED" : "NO SYNC");
    ImGui::SameLine();
    ImGui::Text("  MER L %.1f U %.1f dB  BER %.4f", info.merLower, info.merUpper, info.ber);

    if (!info.stationName.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(info.stationName.c_str());
    }
    if (!info.stationId.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  [%s]", info.stationId.c_str());
    }
    if (!info.stationSlogan.empty()) {
        ImGui::TextDisabled("%s", info.stationSlogan.c_str());
    }
    if (!info.stationMessage.empty()) {
        ImGui::TextWrapped("%s", info.stationMessage.c_str());
    }
    if (!info.stationLocation.empty()) {
        ImGui::TextDisabled("Location: %s", info.stationLocation.c_str());
    }

    if (!info.trackTitle.empty()) {
        ImGui::Separator();
        ImGui::Text("Now Playing:");
        ImGui::TextUnformatted(info.trackTitle.c_str());
        if (!info.trackArtist.empty()) ImGui::TextDisabled("  %s", info.trackArtist.c_str());
        if (!info.trackAlbum.empty()) ImGui::TextDisabled("  [%s]", info.trackAlbum.c_str());
        if (!info.trackGenre.empty()) ImGui::TextDisabled("  genre: %s", info.trackGenre.c_str());
        if (!info.programType.empty()) ImGui::TextDisabled("  type: %s", info.programType.c_str());
    }

    if (!info.programs.empty()) {
        ImGui::Separator();
        ImGui::Text("Programs (%d):", (int)info.programs.size());
        if (ImGui::BeginCombo("##hdprog", info.programs[info.activeProgram].c_str())) {
            for (int i = 0; i < (int)info.programs.size(); i++) {
                if (ImGui::Selectable(info.programs[i].c_str(), i == info.activeProgram)) {
                    oob::Nrsc5Store::instance().setProgram(i);
                }
            }
            ImGui::EndCombo();
        }
    }

    if (!info.codecInfo.empty()) {
        ImGui::TextDisabled("Codec: %s", info.codecInfo.c_str());
    }
    if (!info.localTime.empty()) {
        ImGui::TextDisabled("Time: %s", info.localTime.c_str());
    }

    ImGui::End();
#endif
}

void drawAbout(App& app)
{
    if (!app.showAbout)
        return;

    ImGui::SetNextWindowSize(ImVec2(420, 340), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin((std::string(_L("About SignalScope")) + "###About SignalScope").c_str(), &app.showAbout,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoCollapse))
    {
        ImGui::TextWrapped("SignalScope v" SIGNALSCOPE_VERSION);
        ImGui::Separator();
        ImGui::TextWrapped("SignalScope was created by Sarah Rose.");
        ImGui::Spacing();
        ImGui::TextWrapped("Built with components from:");
        ImGui::TextDisabled("  InmarScope (Sarah Rose)");
        ImGui::TextDisabled("  multimon-ng");
        ImGui::TextDisabled("  PDW (POCSAG/Flex)");
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Persistent settings: serialized into signalscope.ini alongside the ImGui dock
// layout via a custom settings handler.
// ---------------------------------------------------------------------------

void drawDockHost(App& app)
{
    // Default to NOT forcing a rebuild: if signalscope.ini holds a saved layout,
    // the dock node already exists and we keep it. Only build the default layout
    // on first run (no node) or when explicitly forced (Reset Layout / dual /
    // a layout-version bump).
    static bool forceLayout = false;
    if (app.forceDefaultLayout) { forceLayout = true; app.forceDefaultLayout = false; }
    static bool lastDual = false;
    if (app.dualMode != lastDual) { forceLayout = true; lastDual = app.dualMode; }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##SignalScopeHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockId = ImGui::GetID("SignalScopeDockSpace");
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_NoUndocking);

    if (forceLayout || ImGui::DockBuilderGetNode(dockId) == nullptr)
    {
        forceLayout = false;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);

        ImGuiID left, right, rtop, rrest, rmid, rbot, ctrl, dec;
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.32f, &left, &right);
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.62f, &ctrl, &dec);
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.30f, &rtop, &rrest);
        ImGui::DockBuilderSplitNode(rrest, ImGuiDir_Up, 0.58f, &rmid, &rbot);

        ImGui::DockBuilderDockWindow((std::string(_L("Control")) + "###Control").c_str(), ctrl);
        ImGui::DockBuilderDockWindow((std::string(_L("Decoders")) + "###Decoders").c_str(), dec);

        ImGui::DockBuilderDockWindow((std::string(_L("Spectrum")) + "###Spectrum").c_str(), rtop);
        ImGui::DockBuilderDockWindow((std::string(_L("Waterfall")) + "###Waterfall").c_str(), rmid);
        // Always split for potential dual-mode: B windows are invisible when
        // not in dual mode, and the A windows fill the space.
        {
            ImGuiID rtopR, rmidR;
            ImGui::DockBuilderSplitNode(rtop, ImGuiDir_Right, 0.5f, &rtopR, &rtop);
            ImGui::DockBuilderSplitNode(rmid, ImGuiDir_Right, 0.5f, &rmidR, &rmid);
            ImGui::DockBuilderDockWindow((std::string(_L("Spectrum")) + "###Spectrum").c_str(), rtop);
            ImGui::DockBuilderDockWindow((std::string(_L("Waterfall")) + "###Waterfall").c_str(), rmid);
            ImGui::DockBuilderDockWindow((std::string(_L("Spectrum (B)")) + "###Spectrum (B)").c_str(), rtopR);
            ImGui::DockBuilderDockWindow((std::string(_L("Waterfall (B)")) + "###Waterfall (B)").c_str(), rmidR);
        }
        ImGui::DockBuilderDockWindow((std::string(_L("Pager")) + "###Messages").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("EPG")) + "###EPG").c_str(), rbot);
        ImGui::DockBuilderDockWindow((std::string(_L("HD Radio")) + "###HDRadio").c_str(), rbot);
        ImGui::DockBuilderFinish(dockId);
    }

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu(_L("View")))
        {
            if (ImGui::MenuItem(_L("Reset Layout")))
                forceLayout = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(_L("Help")))
        {
            if (ImGui::MenuItem(_L("About")))
                app.showAbout = true;
            ImGui::EndMenu();
        }
        // Patreon button on the right side of the menu bar
        float avail = ImGui::GetContentRegionAvail().x;
        float btnW = ImGui::CalcTextSize("Support on Patreon").x + 20;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btnW - 4);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.42f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.50f, 0.20f, 1.0f));
        if (ImGui::SmallButton("Support on Patreon"))
        {
#if defined(_WIN32)
            ShellExecuteA(nullptr, "open", "https://www.patreon.com/c/SarahRoseLives",
                          nullptr, nullptr, SW_SHOWNORMAL);
#endif
        }
        ImGui::PopStyleColor(2);
        ImGui::EndMenuBar();
    }

    ImGui::End();
}

