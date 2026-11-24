#pragma once

#include "app/music_cue.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <span>
#include <string>

namespace app {

// Desktop-only playback of the user's local CD/ADX WAVs. File loading and
// device lifecycle are on the main thread, never on SDL's audio callback.
class LocalMusic {
public:
    LocalMusic() = default;
    LocalMusic(const LocalMusic &) = delete;
    LocalMusic &operator=(const LocalMusic &) = delete;
    ~LocalMusic() { close(); }

    void configure(MusicVersion version, const std::string &directory) {
        if (version_ == version && directory_ == directory) return;
        close();
        version_ = version;
        directory_ = directory;
        if (version == MusicVersion::Arcade) return;
        std::string root = directory;
        if (root.empty()) {
            const char *base = SDL_GetBasePath();
            root = std::string(base ? base : "") + "musiques-locales";
        }
        if (root.back() != '/' && root.back() != '\\') root += '/';
        bool found = false;
        for (int i = 0; i < int(tracks_.size()); ++i) {
            for (const char *name : music_paths(version, i)) {
                if (!name) break;
                if (load(tracks_[i], root + name)) { found = true; break; }
            }
        }
        if (!found) return;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kFrequency};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, this);
        if (!stream_) tracks_ = {};
    }

    void close() {
        if (stream_) {
            SDL_SetAudioStreamGetCallback(stream_, nullptr, nullptr);
            SDL_DestroyAudioStream(stream_);
        }
        stream_ = nullptr;
        tracks_ = {};
        cue_ = {};
        slot_ = -1;
        playing_ = running_ = failed_ = false;
        version_ = MusicVersion::Arcade;
        directory_.clear();
        gain_ = -1.f;
    }

    void reset() {
        cue_ = {};
        select(-1);
    }

    bool available() const { return stream_ && !failed_.load(std::memory_order_relaxed); }
    bool playing() const { return available() && playing_; }

    void consume(std::span<const uint8_t> bytes) {
        if (!available()) return;
        for (uint8_t byte : bytes)
            if (const auto slot = cue_.push(byte)) select(*slot);
    }

    void sync(bool running, float gain) {
        if (!available()) return;
        gain = std::isfinite(gain) ? std::clamp(gain, 0.f, 1.f) : 0.f;
        if (gain != gain_) {
            if (!SDL_SetAudioStreamGain(stream_, gain)) { failed_ = true; return; }
            gain_ = gain;
        }
        if (running != running_) {
            const bool result = running ? SDL_ResumeAudioStreamDevice(stream_) : SDL_PauseAudioStreamDevice(stream_);
            if (!result) { failed_ = true; return; }
            running_ = running;
        }
    }

private:
    static constexpr int kFrequency = 44100;
    static constexpr unsigned kFadeFrames = kFrequency * 80 / 1000;
    struct Clip {
        struct Free { void operator()(Uint8 *data) const { SDL_free(data); } };
        std::unique_ptr<Uint8, Free> data;
        unsigned frames = 0;
    };

    static bool load(Clip &clip, const std::string &path) {
        SDL_PathInfo info{};
        if (!SDL_GetPathInfo(path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE ||
            info.size > 128ull * 1024 * 1024) return false;
        SDL_AudioSpec spec{};
        Uint8 *data = nullptr;
        Uint32 size = 0;
        if (!SDL_LoadWAV(path.c_str(), &spec, &data, &size)) return false;
        Clip candidate{std::unique_ptr<Uint8, Clip::Free>(data), unsigned(size / 4)};
        if (spec.freq != kFrequency || spec.channels != 2 || spec.format != SDL_AUDIO_S16 ||
            size < 4 || size % 4) return false;
        clip = std::move(candidate);
        return true;
    }

    void select(int slot) {
        if (!available()) return;
        if (slot < 0 || slot >= int(tracks_.size()) || !tracks_[slot].data) slot = -1;
        if (slot == slot_) return;
        if (!SDL_LockAudioStream(stream_)) { failed_ = true; return; }
        slot_ = slot;
        offset_ = faded_ = 0;
        playing_ = slot >= 0;
        const bool cleared = SDL_ClearAudioStream(stream_);
        const bool unlocked = SDL_UnlockAudioStream(stream_);
        if (!cleared || !unlocked) failed_ = true;
    }

    static void SDLCALL callback(void *context, SDL_AudioStream *stream, int additional, int) noexcept {
        auto &self = *static_cast<LocalMusic *>(context);
        if (additional <= 0 || self.slot_ < 0 || self.failed_.load(std::memory_order_relaxed)) return;
        const auto &clip = self.tracks_[self.slot_];
        const auto *samples = reinterpret_cast<const Sint16 *>(clip.data.get());
        unsigned remaining = (unsigned(additional) + 7) / 8;
        std::array<float, 512> mix;
        while (remaining) {
            const unsigned count = std::min(remaining, unsigned(mix.size() / 2));
            for (unsigned i = 0; i < count; ++i) {
                const float fade = float(self.faded_) / kFadeFrames;
                const unsigned offset = self.offset_ * 2;
                mix[2 * i] = float(samples[offset]) * (fade / 32768.f);
                mix[2 * i + 1] = float(samples[offset + 1]) * (fade / 32768.f);
                if (self.faded_ < kFadeFrames) ++self.faded_;
                if (++self.offset_ == clip.frames) self.offset_ = 0;
            }
            if (!SDL_PutAudioStreamData(stream, mix.data(), int(count * 2 * sizeof(float)))) {
                self.failed_ = true;
                return;
            }
            remaining -= count;
        }
    }

    MusicVersion version_ = MusicVersion::Arcade;
    std::string directory_;
    std::array<Clip, 3> tracks_;
    MusicCue cue_;
    SDL_AudioStream *stream_ = nullptr;
    int slot_ = -1; // protected by the SDL stream lock
    unsigned offset_ = 0, faded_ = 0;
    bool playing_ = false, running_ = false;
    std::atomic<bool> failed_{false};
    float gain_ = -1.f;
};

} // namespace app
