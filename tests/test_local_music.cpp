#include "app/local_music.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using app::MusicVersion;

static bool make_wav(const fs::path &path) {
    // 32 frames of entirely synthetic stereo PCM, 16-bit at 44.1 kHz.
    constexpr unsigned char header[] = {
        'R','I','F','F',164,0,0,0,'W','A','V','E',
        'f','m','t',' ',16,0,0,0,1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,
        'd','a','t','a',128,0,0,0,
    };
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(header), sizeof header);
    for (int i = 0; i < 64; ++i) {
        file.put(0);
        file.put(32);
    }
    return bool(file);
}

int main() {
    unsigned failures = 0;
    auto check = [&](bool ok, const char *name) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s (%s)\n", name, SDL_GetError()); ++failures; }
    };
    check(app::music_slot(7) == 0 && app::music_slot(4) == 1 && app::music_slot(9) == 2, "circuit mapping");
    check(app::music_slot(8) == -1 && app::music_slot(1) == -1, "unmapped tracks default to Arcade");
    check(std::string_view(app::music_paths(MusicVersion::Dreamcast, 0)[0]) == "dreamcast/adxb_055.wav" &&
          std::string_view(app::music_paths(MusicVersion::Dreamcast, 1)[0]) == "dreamcast/adxb_063.wav" &&
          std::string_view(app::music_paths(MusicVersion::Dreamcast, 2)[0]) == "dreamcast/adxb_060.wav",
          "verified Dreamcast files");

    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL audio init: %s\n", SDL_GetError());
        return 1;
    }
    const fs::path root = "local_music_test_" + std::to_string(SDL_GetTicksNS());
    fs::create_directories(root / "saturn");
    fs::create_directories(root / "windows");
    const auto good = root / "windows/06-the-king-of-speed.wav";
    const auto bad = root / "saturn/06-the-king-of-speed.wav";
    {
        std::ofstream invalid(bad, std::ios::binary);
        invalid << "invalid";
    }
    check(make_wav(good), "create synthetic test clip");
    app::LocalMusic music;
    music.configure(MusicVersion::Arcade, root.string());
    check(!music.available(), "Arcade does not open a stream");
    music.configure(MusicVersion::SaturnWindows, root.string());
    check(music.available() && !music.playing(), "valid Windows file survives corrupt Saturn preference");

    const std::array<uint8_t, 2> partial{0xae, 0x10};
    music.consume(partial);
    check(!music.playing(), "partial UART packet cannot select music");
    const std::array<uint8_t, 2> end{0xf8, 4};
    music.consume(end);
    check(music.playing(), "Advanced cue selects the local file");
    music.sync(true, .25f);
    SDL_Delay(40); // the 32-frame fixture already had to wrap many times
    check(music.playing(), "clip loops while output is running");
    const std::array<uint8_t, 3> effect{0xae, 0x20, 7};
    music.consume(effect);
    check(music.playing(), "effect bank does not change the music");
    const std::array<uint8_t, 3> missing{0xae, 0x10, 7};
    music.consume(missing);
    check(!music.playing(), "missing Beginner file restores Arcade");
    const std::array<uint8_t, 2> repeat{0x10, 4};
    music.consume(repeat);
    check(music.playing(), "running status preserves a later Advanced cue");
    const std::array<uint8_t, 3> attract{0xae, 0x10, 8};
    music.consume(attract);
    check(!music.playing(), "attract remains Arcade");
    const std::array<uint8_t, 3> advanced{0xae, 0x10, 4};
    music.consume(advanced);
    const std::array<uint8_t, 3> cancel_fade{0xbe, 0x1a, 0};
    music.consume(cancel_fade);
    check(music.playing(), "zero fade value leaves the clip playing");
    const std::array<uint8_t, 3> fade{0xbe, 0x1a, 0x10};
    music.consume(fade);
    check(!music.playing(), "arcade fade command stops the replacement");
    music.consume(advanced);
    music.reset();
    check(!music.playing(), "reset stops the previous race clip");
    music.close();
    SDL_Quit();
    fs::remove(good);
    fs::remove(bad);
    fs::remove(root / "saturn");
    fs::remove(root / "windows");
    fs::remove(root);
    return failures ? 1 : 0;
}
