// SDRplay (RSP family) controls, driven through the SoapySDR C API and the
// SoapySDRPlay3 module. Adapted from InmarScope (GPL-3.0-or-later, Sarah Rose)
// for SignalScope's single-receiver model.
#include "core/app.h"
#include "core/main_funcs.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
bool stringCombo(const char* label, std::string& value, const std::vector<std::string>& options,
                 const std::vector<std::string>& labels = {}) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.empty() ? "Device default" : value.c_str())) {
        for (size_t i = 0; i < options.size(); ++i)
            if (ImGui::Selectable((i < labels.size() ? labels[i] : options[i]).c_str(), value == options[i])) {
                value = options[i]; changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}
void rateCombo(const char* label, double& value, const std::vector<double>& values, bool automatic = false) {
    char preview[64]; std::snprintf(preview, sizeof(preview), "%.6g MHz", value / 1e6);
    if (ImGui::BeginCombo(label, value == 0 ? "Automatic" : preview)) {
        if (automatic && ImGui::Selectable("Automatic", value == 0)) value = 0;
        for (double rate : values) {
            char text[64]; std::snprintf(text, sizeof(text), "%.6g MHz", rate / 1e6);
            if (ImGui::Selectable(text, value == rate)) value = rate;
        }
        ImGui::EndCombo();
    }
}
}

void drawSdrplayControls(App& app)
{
#ifndef HAS_SOAPYSDR
    ImGui::TextWrapped("SDRplay requires a build with SoapySDR, the SoapySDRPlay3 module "
                       "and the SDRplay API service. See COMPILE.md.");
#endif
    SdrplaySource& source = app.rsp;
    RspConfig& cfg = app.rspConfig;
    const bool running = source.running();

    ImGui::BeginDisabled(running);
    if (ImGui::Button("Find SDRplay devices")) {
        source.close();
        app.rspDevices = source.listDevices();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("(%d found)", (int)app.rspDevices.size());

    ImGui::BeginDisabled(running);
    if (ImGui::BeginCombo("Device serial", cfg.serial.empty() ? "Select device" : cfg.serial.c_str())) {
        for (const auto& d : app.rspDevices) {
            if (ImGui::Selectable((d.name + " [" + d.serial + "]").c_str(), cfg.serial == d.serial)) {
                source.close();
                cfg = RspConfig{};
                cfg.serial = d.serial;
            }
        }
        ImGui::EndCombo();
    }
    const auto selected = std::find_if(app.rspDevices.begin(), app.rspDevices.end(),
        [&](const SdrDeviceInfo& d) { return d.serial == cfg.serial; });
    if (selected != app.rspDevices.end() && selected->hasTunerModes) {
        if (stringCombo("RSPduo mode", cfg.mode, {"ST", "MA", "MA8", "SL"},
                        {"Single tuner", "Master (6 MHz clock)", "Master (8 MHz clock)", "Slave"})) {
            source.close();
            cfg.antenna.clear();
        }
    }
    if (ImGui::Button("Load device controls")) {
        source.close();
        std::string err;
        source.setCenterFreq(app.centerFreqMHz * 1e6);
        if (!source.prepare(cfg, err)) app.status = err;
    }
    ImGui::EndDisabled();

    if (ImGui::InputDouble("Center (MHz)", &app.centerFreqMHz, 0.1, 1.0, "%.6f")) {
        app.centerFreqMHz = std::clamp(app.centerFreqMHz, 0.001, 2000.0);
        if (running)
            retunePreserving(app, app.centerFreqMHz);
        app.viewA.resetView = true;
    }

    if (!source.prepared()) {
        ImGui::TextWrapped("Load device controls to select antennas, gain and model-specific "
                           "features. Then press Start.");
    } else {
        ImGui::BeginDisabled(running);
        stringCombo("Antenna / input", cfg.antenna, source.antennas());
        rateCombo("Sample rate", cfg.rate, source.rates());
        rateCombo("IF bandwidth", cfg.bandwidth, source.bandwidths(), true);
        if (source.hasPpm) {
            ImGui::InputDouble("Frequency correction (PPM)", &cfg.ppm, 0.1, 1.0, "%.2f");
            cfg.ppm = std::clamp(cfg.ppm, -200.0, 200.0);
        }
        if (source.hasAgc) ImGui::Checkbox("Automatic gain", &cfg.agc);
        if (source.hasDc) ImGui::Checkbox("DC correction", &cfg.dc);
        if (source.hasIq) ImGui::Checkbox("IQ balance correction", &cfg.iq);
        ImGui::EndDisabled();

        for (const auto& gain : source.gains()) {
            double v = cfg.gains.count(gain.key) ? cfg.gains.at(gain.key) : std::atof(gain.value.c_str());
            ImGui::BeginDisabled(cfg.agc && gain.key == "IFGR");
            const std::string label = gain.key == "RFGR" ? "RF gain reduction (LNA state)"
                                                          : gain.name + " gain (driver units)";
            if (ImGui::SliderScalar(label.c_str(), ImGuiDataType_Double, &v,
                                    &gain.minimum, &gain.maximum, "%.0f")) {
                if (!source.running() || source.setGainElement(gain.key, v)) {
                    cfg.gains[gain.key] = v;
                    // This driver setting aliases RFGR; don't restore an old
                    // value over the slider's saved gain on the next Start.
                    if (gain.key == "RFGR") cfg.settings.erase("device:rfgain_sel");
                }
            }
            ImGui::EndDisabled();
        }

        ImGui::BeginDisabled(running);
        if (ImGui::CollapsingHeader("Model-specific controls", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& c : source.controls()) {
                if (c.key == "rfgain_sel") continue; // RFGR slider is the single RF gain control.
                const auto key = (c.channel ? "channel:" : "device:") + c.key;
                auto value = cfg.settings.count(key) ? cfg.settings.at(key) : c.value;
                bool changed = false;
                ImGui::PushID(key.c_str());
                const char* label = c.name.empty() ? c.key.c_str() : c.name.c_str();
                if (!c.options.empty()) changed = stringCombo(label, value, c.options, c.labels);
                else if (c.type == 0) {
                    bool v = value == "true"; changed = ImGui::Checkbox(label, &v); value = v ? "true" : "false";
                } else {
                    char text[256]; std::snprintf(text, sizeof(text), "%s", value.c_str());
                    changed = ImGui::InputText(label, text, sizeof(text)); value = text;
                }
                if (ImGui::IsItemHovered() && !c.description.empty()) ImGui::SetTooltip("%s", c.description.c_str());
                if (changed) cfg.settings[key] = value;
                ImGui::PopID();
            }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Gain sliders apply live. Stop reception to edit other hardware settings.");
    }

    auto error = source.error();
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    if (running)
        ImGui::Text("Actual rate: %.6g MHz | Overflows: %llu", source.sampleRate() / 1e6,
                    (unsigned long long)source.overflows());
    ImGui::TextWrapped("Detects RSP1, RSP1A, RSP1B, RSP2 / RSP2pro, RSPduo, RSPdx and RSPdx-R2 "
                       "through SDRplay API 3.15 or newer.");
}
