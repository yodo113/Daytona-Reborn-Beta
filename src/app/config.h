// Launcher settings, saved to launcher.ini in the user's data folder (SDL's
// pref path): the ROM set, the GPU backend, fullscreen, audio volume, and
// the control bindings. Plain key=value lines, so it can be edited by hand.
#pragma once

#include "app/controls.h"
#include "app/music_cue.h"

#include <string>
#include <cstdint>
#include <iosfwd>

namespace app {

struct Config {
    std::string rom_path;
    std::string gpu;          // "" (automatic), vulkan, direct3d12, metal
    std::string renderer = "software"; // the 3D: software (CPU, exact) or hardware (SDL_GPU)
    bool fullscreen = false;
    bool hide_cursor = false; // no mouse cursor while the game plays; it shows again in the launcher
    // Exclusive fullscreen in this mode ("WxH@Hz", with "*density" when not
    // 1), or "" for borderless at the desktop's mode.
    std::string fullscreen_mode;
    // Frame pacing (#7, app/pacing.h); all off: the arcade's speed on any display.
    bool pace_smooth = false;       // a display at a multiple of 57.52 Hz: one frame per refresh(es)
    bool pace_sync_display = false; // the game at a rate dividing the refresh (60 on 60/120/240 Hz: 4% fast)
    bool pace_vrr = false;          // each frame held to 1/57.52 s, for a variable-refresh display
    bool skip_launcher = false; // start the game straight away (as --autostart); Esc still opens the launcher
    // Hold the cabinet's Test button for 3 s once the game runs (rt::TestHold),
    // for the test menu without an F2 key: mobile, consoles. Not saved; it
    // clears itself when done.
    bool hold_test = false;
    float volume = 0.8f;      // 0..1
    bool mute = false;
    float music_volume = 1.0f;   // 0..1: the music against the effects (both 1: as the arcade mixes them)
    float effects_volume = 1.0f; // 0..1
    MusicVersion music_version = MusicVersion::Arcade;
    std::string music_directory; // empty: musiques-locales beside the executable
    bool native_audio = false; // applies on reset; reference remains the default
    // Enhancements (off by default).
    std::string language;      // interface (app/i18n.h): "fr", "en", ...; "" the system's (first run)
    std::string aspect;        // "4:3" (the cabinet's, in every mode), "" (automatic: the window's shape in the
                               // native mode, else the original), "16:10", "16:9", "21:9"
    bool arcade_43() const { return aspect == "4:3"; }
    bool hud_edges = false;    // with widescreen: lap times, position and maps at the screen edges
    bool stretch_backdrop = false; // with widescreen, in-game: the tile backdrop stretched across the width, else plain sky
    int draw_distance = 0;     // scenery: 0 = the game's own, -2..+2 (rt::Enhance)
    bool scenery_3d = false;   // fir trees in volume, filled stands, Dinosaur Canyon piers (next start)
    bool interpolate = false;  // pose smoothing between game frames, as --interpolate-poses (next start)
    bool cars_near_model = false; // every opponent drawn with its near model (next start)
    bool sun_shadows = false;  // ray-traced sun shadows (Direct3D 12, ray tracing tier 1.1; next start)
    bool sky_light = false;    // with the shadows: the sky's light, hidden under and between things (next start)
    int sun_height = 0;        // the shadows' sun above the horizon, degrees (10..80); 0 = the game's own
    // Display curve (Direct3D 12): 0 = the PC sRGB curve; 2.2..2.6 = the
    // game's levels shown as on an arcade CRT. Restart required. A new
    // profile starts at 2.4, the cabinet's look (viewers asked for the arcade
    // palette, 23/11); a saved profile keeps its value, 0 included.
    float display_gamma = 2.4f;
    // Colour saturation of the final image (Direct3D 12): 1 = unchanged, 0.5..1.5.
    float display_saturation = 1.0f;
    uint32_t draw_budget = 0;   // 0 Automatic; positive Custom allowance (runtime/scenery.h)
    int draw_mode = 0;         // 0 double buffered (every frame), 1 single buffered (every 2nd), 2 every third frame
    static constexpr int kNativeResolution = 0;
    static constexpr int kNativeDirect = 5;
    int supersampling = 1;     // 1-4 times arcade resolution, or a native mode
    bool uses_native_resolution() const {
        return supersampling == kNativeResolution || supersampling == kNativeDirect;
    }
    static constexpr double kMaxAspect = 21.0 / 9.0;
    double aspect_ratio() const; // width / height; 0 = original
    Controls controls;
    // Link play (the communication board; Revision A): this cabinet listens
    // for the one before it in the ring and connects to the next. Master or
    // slave, and the car number, are the game's own settings (test mode).
    bool link = false;
    int link_port = 15112;               // where the cabinet before this one connects
    std::string link_next = "127.0.0.1:15113"; // host:port of the next cabinet
    bool link_framesync = false;         // hold every cabinet to the master's frame
    float ffb_strength = 0.7f; // force feedback (the drive board) on the steering device: 0 off .. 1
    bool ffb_invert = false;   // turn the wheel the other way
    bool ffb_log = false;      // log the force feedback device, failures and the commands (daytona.log on Windows)
    bool ffb_centring_override = false; // Linux: the game's centring worked out here, sent as constant force (#16)
    // Linux and macOS: Logitech wheels through the system's driver, not SDL's
    // own (its HIDAPI lg4ff driver, off on Windows already). Some, like the
    // original Driving Force, send SDL's driver nothing (issue #4). On by
    // default on Linux, whose kernel driver gives the wheels force feedback;
    // off on macOS, where only SDL's driver does. Applies at start-up; SDL
    // gives the wheel another GUID, so it is bound again.
#ifdef __linux__
    bool legacy_logitech_wheels = true;
#else
    bool legacy_logitech_wheels = false;
#endif

    Config() { controls.set_defaults(); }
    static std::string path();  // <pref path>/launcher.ini
    // --profile NAME: a separate data folder (settings, EEPROM, backup RAM),
    // e.g. a second cabinet on the same computer for link play.
    static inline std::string profile;
    static std::string pref_dir(); // where this set's (and profile's) data lives, with a trailing separator
    void load();
    void save() const;
    void read(std::istream &input);
    void write(std::ostream &output) const;
};

} // namespace app
