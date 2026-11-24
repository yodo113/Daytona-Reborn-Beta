#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace app {

enum class MusicVersion { Arcade, SaturnWindows, Dreamcast };

inline constexpr int music_slot(uint8_t index) {
    switch (index) {
    case 7: return 0;
    case 4: return 1;
    case 9: return 2;
    default: return -1;
    }
}

using MusicPaths = std::array<const char *, 4>;

inline constexpr MusicPaths music_paths(MusicVersion version, int slot) {
    if (slot < 0 || slot >= 3) return {};
    constexpr std::array<MusicPaths, 3> cd{{
        {"saturn/07-lets-go-away.wav", "saturn/07.wav", "windows/07-lets-go-away.wav", "windows/07.wav"},
        {"saturn/06-the-king-of-speed.wav", "windows/06-the-king-of-speed.wav"},
        {"saturn/08-sky-high.wav", "windows/08-sky-high.wav"},
    }};
    constexpr std::array<MusicPaths, 3> dreamcast{{
        {"dreamcast/adxb_055.wav"}, {"dreamcast/adxb_063.wav"}, {"dreamcast/adxb_060.wav"},
    }};
    if (version == MusicVersion::SaturnWindows) return cd[slot];
    if (version == MusicVersion::Dreamcast) return dreamcast[slot];
    return {};
}

inline constexpr MusicVersion music_version_from(std::string_view value) {
    if (value == "saturn-windows") return MusicVersion::SaturnWindows;
    if (value == "dreamcast") return MusicVersion::Dreamcast;
    return MusicVersion::Arcade;
}

inline constexpr const char *music_version_name(MusicVersion version) {
    switch (version) {
    case MusicVersion::SaturnWindows: return "saturn-windows";
    case MusicVersion::Dreamcast: return "dreamcast";
    default: return "arcade";
    }
}

// Commit a music cue only after both UART data bytes arrive. Keep running
// status across packets and ignore the serial timing bytes in between.
class MusicCue {
public:
    std::optional<int> push(uint8_t byte) {
        if (byte >= 0xf8) {
            if (byte != 0xff) return {};
            *this = {};
            return -1;
        }
        if (byte & 0x80) {
            status_ = byte < 0xf0 ? byte : 0;
            have_first_ = false;
            return {};
        }
        if (status_ != 0xae && status_ != 0xbe) return {};
        if (!have_first_) {
            first_ = byte;
            have_first_ = true;
            return {};
        }
        have_first_ = false;
        if (status_ == 0xae && first_ == 0x10) return music_slot(byte);
        if (status_ == 0xbe && first_ == 0x1a && byte != 0) return -1;
        return {};
    }

private:
    uint8_t status_ = 0, first_ = 0;
    bool have_first_ = false;
};

} // namespace app
