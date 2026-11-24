#include "app/config.h"
#include "runtime/scenery.h"

#include <algorithm>
#include <cmath>
#include <cmath>

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace app {

std::string Config::pref_dir() {
    // per ROM set (and profile): settings, EEPROM, backup RAM
    const std::string app = profile.empty() ? std::string(M2_ROMSET) : std::string(M2_ROMSET) + "-" + profile;
    char *base = SDL_GetPrefPath("daytona-recomp", app.c_str());
    std::string p = base ? std::string(base) : std::string();
    SDL_free(base);
    return p;
}

std::string Config::path() { return pref_dir() + "launcher.ini"; }

void Config::load() {
    std::ifstream f(path());
    read(f);
}

void Config::read(std::istream &f) {
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "rom") rom_path = v;
        else if (k == "gpu") gpu = v;
        else if (k == "renderer") renderer = v == "hardware" ? v : "software";
        else if (k == "fullscreen") fullscreen = v == "1";
        else if (k == "skip_launcher") skip_launcher = v == "1";
        else if (k == "aspect") aspect = v;
        else if (k == "language") language = v;
        else if (k == "hud_edges") hud_edges = v == "1";
        else if (k == "stretch_backdrop") stretch_backdrop = v == "1";
        else if (k == "draw_distance") draw_distance = std::clamp(std::atoi(v.c_str()), -2, 2);
        else if (k == "scenery_3d") scenery_3d = v == "1";
        else if (k == "interpolate") interpolate = v == "1";
        else if (k == "cars_near_model") cars_near_model = v == "1";
        else if (k == "sun_shadows") sun_shadows = v == "1";
        else if (k == "sky_light") sky_light = v == "1";
        else if (k == "sun_height") {
            const int value = std::atoi(v.c_str());
            sun_height = value <= 0 ? 0 : std::clamp(value, 10, 80);
        }
        else if (k == "display_gamma") {
            const float value = std::strtof(v.c_str(), nullptr);
            display_gamma = std::isfinite(value) && value >= 2.0f ? std::clamp(value, 2.0f, 2.8f) : 0.0f;
        }
        else if (k == "display_saturation") {
            const float value = std::strtof(v.c_str(), nullptr);
            display_saturation = std::isfinite(value) ? std::clamp(value, 0.5f, 1.5f) : 1.0f;
        }
        else if (k == "draw_budget") {
            if (!rt::parse_scenery_budget(v, draw_budget)) {
                draw_budget = 0;
                std::fprintf(stderr, "Ignoring invalid draw_budget; using Automatic\n");
            }
        }
        else if (k == "draw_mode") draw_mode = std::clamp(std::atoi(v.c_str()), 0, 2);
        else if (k == "supersampling") {
            if (v == "native") supersampling = kNativeResolution;
            else if (v == "native-direct") supersampling = kNativeDirect;
            else supersampling = std::clamp(std::atoi(v.c_str()), 1, 4);
        }
        else if (k == "volume") volume = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "mute") mute = v == "1";
        else if (k == "fullscreen_mode") fullscreen_mode = v;
        else if (k == "hide_cursor") hide_cursor = v == "1";
        else if (k == "pace_smooth") pace_smooth = v == "1";
        else if (k == "pace_sync_display") pace_sync_display = v == "1";
        else if (k == "pace_vrr") pace_vrr = v == "1";
        else if (k == "music_volume") music_volume = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "effects_volume") effects_volume = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "music_version") music_version = music_version_from(v);
        else if (k == "music_directory") music_directory = v;
        else if (k == "native_audio") native_audio = v == "1";
        else if (k == "deadzone") controls.deadzone = std::strtof(v.c_str(), nullptr);
        else if (k == "joy_deadzone") controls.joy_deadzone = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 0.4f);
        else if (k == "wheel_sensitivity") {
            const float value = std::strtof(v.c_str(), nullptr);
            controls.wheel_sensitivity = std::isfinite(value) ? std::clamp(value, 0.25f, 3.0f) : 1.0f;
        }
        else if (k == "steer_invert") controls.steer_invert = v == "1";
        else if (k == "link") link = v == "1";
        else if (k == "link_port") link_port = std::clamp(std::atoi(v.c_str()), 1, 65535);
        else if (k == "link_next") link_next = v;
        else if (k == "link_framesync") link_framesync = v == "1";
        else if (k == "ffb_strength") ffb_strength = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "ffb_invert") ffb_invert = v == "1";
        else if (k == "pad_steer_sensitivity") {
            const float value = std::strtof(v.c_str(), nullptr);
            controls.pad_steer_sensitivity = std::isfinite(value) ? std::clamp(value, 0.25f, 1.0f) : 0.55f;
        }
        else if (k == "pad_steer_speed") {
            const float value = std::strtof(v.c_str(), nullptr);
            controls.pad_steer_speed = std::isfinite(value) ? std::clamp(value, 0.02f, 1.0f) : 0.07f;
        }
        else if (k == "ffb_log") ffb_log = v == "1";
        else if (k == "ffb_centring_override") ffb_centring_override = v == "1";
        else if (k == "legacy_logitech_wheels") legacy_logitech_wheels = v == "1";
        else
            for (int a = 0; a < kNumActions; a++) {
                const std::string base = action_key(Action(a));
                if (k == base + ".key") controls.bind[a].key = v.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(v.c_str());
                else if (k == base + ".pad") controls.bind[a].pad = PadInput::parse(v);
                else if (k == base + ".joy") controls.bind[a].joy = JoyInput::parse(v);
            }
    }
}

void Config::save() const {
    std::ofstream f(path());
    write(f);
}

void Config::write(std::ostream &f) const {
    f << "# Daytona USA launcher settings\n";
    f << "rom=" << rom_path << "\n";
    f << "gpu=" << gpu << "\n";
    f << "renderer=" << renderer << "\n";
    f << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
    f << "skip_launcher=" << (skip_launcher ? 1 : 0) << "\n";
    f << "aspect=" << aspect << "\n";
    f << "language=" << language << "\n";
    f << "hud_edges=" << (hud_edges ? 1 : 0) << "\n";
    f << "stretch_backdrop=" << (stretch_backdrop ? 1 : 0) << "\n";
    f << "draw_distance=" << draw_distance << "\n";
    f << "scenery_3d=" << (scenery_3d ? 1 : 0) << "\n";
    f << "interpolate=" << (interpolate ? 1 : 0) << "\n";
    f << "cars_near_model=" << (cars_near_model ? 1 : 0) << "\n";
    f << "sun_shadows=" << (sun_shadows ? 1 : 0) << "\n";
    f << "sun_height=" << sun_height << "\n";
    f << "sky_light=" << (sky_light ? 1 : 0) << "\n";
    f << "display_gamma=" << display_gamma << "\n";
    f << "display_saturation=" << display_saturation << "\n";
    f << "draw_budget=" << draw_budget << "\n";
    f << "draw_mode=" << draw_mode << "\n";
    f << "supersampling=";
    if (supersampling == kNativeResolution) f << "native\n";
    else if (supersampling == kNativeDirect) f << "native-direct\n";
    else f << supersampling << '\n';
    f << "volume=" << volume << "\n";
    f << "mute=" << (mute ? 1 : 0) << "\n";
    f << "fullscreen_mode=" << fullscreen_mode << "\n";
    f << "hide_cursor=" << (hide_cursor ? 1 : 0) << "\n";
    f << "pace_smooth=" << (pace_smooth ? 1 : 0) << "\n";
    f << "pace_sync_display=" << (pace_sync_display ? 1 : 0) << "\n";
    f << "pace_vrr=" << (pace_vrr ? 1 : 0) << "\n";
    f << "music_volume=" << music_volume << "\n";
    f << "music_version=" << music_version_name(music_version) << "\n";
    if (!music_directory.empty()) f << "music_directory=" << music_directory << "\n";
    f << "effects_volume=" << effects_volume << "\n";
    f << "native_audio=" << (native_audio ? 1 : 0) << "\n";
    f << "deadzone=" << controls.deadzone << "\n";
    f << "joy_deadzone=" << controls.joy_deadzone << "\n";
    f << "wheel_sensitivity=" << controls.wheel_sensitivity << "\n";
    f << "steer_invert=" << (controls.steer_invert ? 1 : 0) << "\n";
    f << "link=" << (link ? 1 : 0) << "\n";
    f << "link_port=" << link_port << "\n";
    f << "link_next=" << link_next << "\n";
    f << "link_framesync=" << (link_framesync ? 1 : 0) << "\n";
    f << "ffb_strength=" << ffb_strength << "\n";
    f << "ffb_invert=" << (ffb_invert ? 1 : 0) << "\n";
    f << "pad_steer_sensitivity=" << controls.pad_steer_sensitivity << "\n";
    f << "pad_steer_speed=" << controls.pad_steer_speed << "\n";
    f << "ffb_log=" << (ffb_log ? 1 : 0) << "\n";
    f << "ffb_centring_override=" << (ffb_centring_override ? 1 : 0) << "\n";
    f << "legacy_logitech_wheels=" << (legacy_logitech_wheels ? 1 : 0) << "\n";
    for (int a = 0; a < kNumActions; a++) {
        const Binding &b = controls.bind[a];
        f << action_key(Action(a)) << ".key=" << (b.key == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(b.key)) << "\n";
        f << action_key(Action(a)) << ".pad=" << b.pad.save() << "\n";
        f << action_key(Action(a)) << ".joy=" << b.joy.save() << "\n";
    }
}

double Config::aspect_ratio() const {
    double w = 0, h = 0;
    if (std::sscanf(aspect.c_str(), "%lf:%lf", &w, &h) != 2 || !std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0) return 0;
    return std::min(w / h, kMaxAspect);
}

} // namespace app
