// daytona: the recompiled game with its launcher, in one window. SDL3 for the
// window, input and GPU (SDL_GPU: Vulkan or Direct3D 12 on Windows, Vulkan
// on Linux, Metal on macOS); Dear ImGui for the launcher. The game runs on
// the native board runtime (rt::GameLoop), loaded straight from the user's
// ROM zip (rt::import_rom_set); each composed frame is uploaded to a GPU
// texture and scaled onto the swapchain, and the sound board's output is
// played through SDL audio.
//
//   daytona [--rom FILE.zip] [--autostart] [--gpu vulkan|direct3d12|metal]
//           (the launcher's "Skip launcher" is a saved --autostart)
//           [--fullscreen] [--window WxH] [--frames N] [--audio native|reference] [--profile NAME]
//           [--arcade] (Revision A desktop: use the cabinet launcher)
//           [--start-solo 1|2|3 [--transmission automatic|manual] [--race-mode normal|time-attack]] (Revision A desktop)
//           [--capture-home FILE.bmp]
//           [--inputs FILE] (bounded PC solo race, after the native start)
//           [--trace-player-inputs FILE | --trace-effective-inputs FILE]
//           [--trace-software-presents FILE.csv] (bounded software solo race)
//           [--trace-hardware-presents FILE.csv] (bounded hardware solo race at 1x)
//           [--interpolate-poses] (desktop PC solo, experimental hardware presentation)
//           [--hdr10 [--hdr10-paper-white N]] (desktop D3D12, N in cd/m², default 200)
//           (--profile: a separate data folder, e.g. a second cabinet for link play)
//
// In the game: Esc opens the launcher (resume, reset, controls), F11
// toggles fullscreen. Controls are set in the launcher and saved.

#include "app/config.h"
#include "app/i18n_mark.h"
#include "app/gpu_device.h"
#include "app/hdr10_choice.h"
#ifndef M2_MOBILE
#include "app/gpu/hdr10_present.h"
#endif
#include "app/startup_error.h"
#include "app/ffb.h"
#include "app/pacing.h"
#include "runtime/test_hold.h"
#include "app/link_socket.h"
#include "app/gpu/dxr_scene.h"
#include "app/gpu/gpu_renderer.h"
#include "app/launcher.h"
#include "app/i18n.h"
#include "app/native_audio.h"
#ifndef M2_MOBILE
#include "app/local_music.h"
#endif
#include "app/player_input_trace.h"
#include <ostream>
#include <streambuf>
#include "app/native_audio_sim_clock.h"
#include <charconv>
#include "runtime/native_sound_engine.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"
#include "runtime/race_request.h"
#include "runtime/checked_trace_file.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui.h"
#ifdef M2_MOBILE
#include "app/touch_overlay.h"
#else
#include "app/gpu/home_scene.h"
#include "app/gpu/hardware_present_trace.h"
#include "app/home_menu.h"
#include "common/input_script.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h> // AttachConsole
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> // Windows: the WinMain entry point a WIN32 (GUI) program links against

#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

#if defined(_WIN32) && !defined(M2_MOBILE) && defined(NTDDI_WIN11_GA) && NTDDI_VERSION >= NTDDI_WIN11_GA
#define DAYTONA_HAS_WINDOWS_COLOR_MODE
std::optional<bool> windows_display_hdr(SDL_PropertiesID props) {
    const auto monitor = static_cast<HMONITOR>(
        SDL_GetPointerProperty(props, SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER, nullptr));
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return std::nullopt;

    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(flags, &path_count, &mode_count) != ERROR_SUCCESS)
        return std::nullopt;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS)
        return std::nullopt;

    for (UINT32 i = 0; i < path_count; ++i) {
        const auto &path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                         path.sourceInfo.adapterId, path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0)
            continue;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color{};
        color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof(color),
                        path.targetInfo.adapterId, path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS)
            return color.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
    }
    return std::nullopt;
}
#endif

constexpr double kArcadeHz = rt::GameLoop::kFrameHz; // 16 MHz / (656 x 424), the board's frame rate

#ifndef M2_MOBILE
// Observe a single uninterrupted segment of accepted submissions. The digest
// is the software game's source surface, before GPU scaling and presentation.
class FileOutput final : public std::streambuf {
public:
    explicit FileOutput(FILE *file) : file_(file) {}
private:
    std::streamsize xsputn(const char *data, std::streamsize size) override {
        return static_cast<std::streamsize>(std::fwrite(data, 1, size_t(size), file_));
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const auto written = std::fputc(static_cast<unsigned char>(traits_type::to_char_type(value)), file_);
        return written == EOF ? traits_type::eof() : value;
    }

    int sync() override { return std::fflush(file_) == 0 ? 0 : -1; }

    FILE *file_;
};


class PlayerInputCapture {
public:
    explicit PlayerInputCapture(const char *path)
        : file_(path), buffer_(file_.get()), stream_(&buffer_), writer_(stream_) {
        if (!writer_.begin()) throw std::runtime_error("cannot start player input trace");
    }

    bool write(uint64_t tick, const rt::Inputs &input) {
        return writer_.write(tick, {input.steer, input.accel, input.brake, input.in0, input.in1, input.in2});
    }

    bool close(bool completed, uint64_t ticks) {
        const bool recorded = completed && writer_.next_tick() == ticks && writer_.finish();
        const bool closed = file_.close();
        return recorded && closed;
    }

private:
    rt::CheckedTraceFile file_;
    FileOutput buffer_;
    std::ostream stream_;
    app::PlayerInputTraceWriter writer_;
};

class SoftwarePresentTrace {
public:
    explicit SoftwarePresentTrace(const char *path) : file_(path) {
        if (std::fputs("present_ns,digest\n", file_.get()) < 0) {
            file_.close();
            throw std::runtime_error("cannot write presentation trace header");
        }
    }

    bool sample(bool submitted, bool eligible, Uint32 width, Uint32 height,
                int source_width, uint64_t digest) {
        if (!submitted) return false;
        if (!eligible || (samples_ && (width != width_ || height != height_ ||
                                      source_width != source_width_))) {
            if (samples_) end_reason_ = eligible ? "image size changed" : "left eligible race";
            return true;
        }
        const uint64_t now = SDL_GetTicksNS();
        if (samples_ && now <= last_ns_) return false;
        if (std::fprintf(file_.get(), "%llu,%dx%016llx\n",
                static_cast<unsigned long long>(now), source_width,
                static_cast<unsigned long long>(digest)) < 0) return false;
        width_ = width;
        height_ = height;
        source_width_ = source_width;
        last_ns_ = now;
        ++samples_;
        return true;
    }

    bool ended() const { return end_reason_ != nullptr; }

    bool finish() {
        std::printf("daytona: software present trace samples=%llu (%s)\n",
                    static_cast<unsigned long long>(samples_),
                    end_reason_ ? end_reason_ : "run ended");
        return file_.close() && samples_ >= 2;
    }

private:
    rt::CheckedTraceFile file_;
    uint64_t last_ns_ = 0, samples_ = 0;
    Uint32 width_ = 0, height_ = 0;
    int source_width_ = 0;
    const char *end_reason_ = nullptr;
};

// A BMP file as a sampled B8G8R8A8 texture (the colour test pattern, M2_MIRE).
SDL_GPUTexture *load_bmp_texture(SDL_GPUDevice *dev, const char *path, Uint32 &w, Uint32 &h) {
    SDL_Surface *loaded = SDL_LoadBMP(path);
    if (!loaded) return nullptr;
    SDL_Surface *s = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_BGRA32);
    SDL_DestroySurface(loaded);
    if (!s) return nullptr;
    w = Uint32(s->w), h = Uint32(s->h);
    const SDL_GPUTextureCreateInfo info{SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1, 1, SDL_GPU_SAMPLECOUNT_1, 0};
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(dev, &info);
    SDL_GPUTransferBufferCreateInfo tb{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, w * h * 4, 0};
    SDL_GPUTransferBuffer *upload = texture ? SDL_CreateGPUTransferBuffer(dev, &tb) : nullptr;
    bool ok = false;
    if (upload) {
        if (auto *dst = static_cast<Uint8 *>(SDL_MapGPUTransferBuffer(dev, upload, false))) {
            for (Uint32 y = 0; y < h; y++)
                std::memcpy(dst + size_t(y) * w * 4, static_cast<const Uint8 *>(s->pixels) + size_t(y) * s->pitch, w * 4);
            SDL_UnmapGPUTransferBuffer(dev, upload);
            if (SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev)) {
                SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
                const SDL_GPUTextureTransferInfo from{upload, 0, w, h};
                SDL_GPUTextureRegion to{};
                to.texture = texture;
                to.w = w, to.h = h, to.d = 1;
                SDL_UploadToGPUTexture(copy, &from, &to, false);
                SDL_EndGPUCopyPass(copy);
                ok = SDL_SubmitGPUCommandBuffer(cmd);
            }
        }
        SDL_ReleaseGPUTransferBuffer(dev, upload);
    }
    SDL_DestroySurface(s);
    if (!ok && texture) SDL_ReleaseGPUTexture(dev, texture), texture = nullptr;
    return texture;
}

bool save_home_capture(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *image,
                       SDL_GPUTextureFormat format, Uint32 w, Uint32 h, const char *path, Uint32 level = 0) {
    const SDL_PixelFormat pixels = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ? SDL_PIXELFORMAT_BGRA32
        : format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_UNKNOWN;
    if (pixels == SDL_PIXELFORMAT_UNKNOWN) {
        SDL_CancelGPUCommandBuffer(cmd);
        return SDL_SetError("Home capture requires an 8-bit SDR target");
    }
    const Uint32 stride = (w + 63) & ~63u; // D3D12 copy rows are aligned to 256 bytes
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, stride * h * 4, 0};
    auto *download = SDL_CreateGPUTransferBuffer(dev, &info);
    if (!download) {
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }
    SDL_GPUTextureRegion from{image,level,0,0,0,0,w,h,1};
    SDL_GPUTextureTransferInfo to{download,0,stride,h};
    auto *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    bool saved = false;
    if (fence && SDL_WaitForGPUFences(dev, true, &fence, 1)) {
        if (void *data = SDL_MapGPUTransferBuffer(dev, download, false)) {
            if (auto *surface = SDL_CreateSurfaceFrom(int(w), int(h), pixels, data, int(stride * 4))) {
                saved = SDL_SaveBMP(surface, path);
                SDL_DestroySurface(surface);
            }
            SDL_UnmapGPUTransferBuffer(dev, download);
        }
    }
    if (fence) SDL_ReleaseGPUFence(dev, fence);
    SDL_ReleaseGPUTransferBuffer(dev, download);
    return saved;
}
#endif

// The sound board's two outputs (the YM3438, and the two MultiPCMs mixed) go
// to their own SDL audio streams at the chips' own rates; SDL resamples and
// mixes them on the device. The game advances on the display's clock and the
// device plays on its own, so a small speed trim on both streams holds the
// queue near kLatency instead of letting it drift into a gap or a backlog.
class Audio {
public:
    static constexpr double kLatency = 0.06; // seconds queued
    ~Audio() { close(); }

    bool open(double fm_rate, double pcm_rate) {
        dev_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (!dev_) return false;
        fm_rate_ = int(fm_rate + 0.5);
        const SDL_AudioSpec fm{SDL_AUDIO_F32, 2, fm_rate_}, pcm{SDL_AUDIO_F32, 2, int(pcm_rate + 0.5)};
        fm_ = SDL_CreateAudioStream(&fm, nullptr);
        pcm_ = SDL_CreateAudioStream(&pcm, nullptr);
        if (!fm_ || !pcm_ || !SDL_BindAudioStream(dev_, fm_) || !SDL_BindAudioStream(dev_, pcm_)) {
            close(); return false;
        }
        return true;
    }
    // speed: the game's rate over the arcade's (Sync to display: 1.043 at 60
    // frames/s), so the sound keeps up with the game; the trim works around it.
    void push(snd::SoundBoard &sb, float gain, double speed = 1.0) {
        if (!fm_) return;
        const std::vector<float> fm = sb.take_fm(), pcm = sb.take_pcm();
        SDL_PutAudioStreamData(fm_, fm.data(), int(fm.size() * sizeof(float)));
        SDL_PutAudioStreamData(pcm_, pcm.data(), int(pcm.size() * sizeof(float)));
        const double queued = double(SDL_GetAudioStreamQueued(fm_)) / (8.0 * fm_rate_);
        if (queued > kLatency * 4) { // a stall (window drag, debugger): drop the backlog
            SDL_ClearAudioStream(fm_);
            SDL_ClearAudioStream(pcm_);
        }
        const double err = std::clamp((queued - kLatency) / kLatency, -1.0, 1.0);
        const float ratio = float(speed * (1.0 + 0.005 * err)); // the trim at most 0.5%: inaudible
        SDL_SetAudioStreamFrequencyRatio(fm_, ratio);
        SDL_SetAudioStreamFrequencyRatio(pcm_, ratio);
        SDL_SetAudioStreamGain(fm_, gain);
        SDL_SetAudioStreamGain(pcm_, gain);
    }
    void clear() {
        if (fm_) SDL_ClearAudioStream(fm_);
        if (pcm_) SDL_ClearAudioStream(pcm_);
    }
    void close() {
        if (fm_) SDL_DestroyAudioStream(fm_);
        if (pcm_) SDL_DestroyAudioStream(pcm_);
        if (dev_) SDL_CloseAudioDevice(dev_);
        fm_ = pcm_ = nullptr;
        dev_ = 0;
    }

private:
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioStream *fm_ = nullptr, *pcm_ = nullptr;
    int fm_rate_ = 1;
};

// The launcher's link play status line.
std::string link_status(const rt::CommBoard *board, const app::TcpLink *link, const app::Config &cfg) {
    if (!cfg.link) return "coupé";
    if (!board || !link) return "pas démarré";
    switch (board->link()) {
    case rt::CommBoard::Link::Off: return "actif (le jeu n'a pas lancé la liaison : régler LINK ID en mode test)";
    case rt::CommBoard::Link::Waiting: {
        // which half of the ring is missing
        std::string s = "en attente : ";
        s += link->tx_open() ? "borne suivante " + cfg.link_next + " jointe ; "
                             : "borne suivante " + cfg.link_next + " pas encore jointe (écoute-t-elle sur ce port ?) ; ";
        s += link->rx_open() ? "une borne est connectée au port " + std::to_string(cfg.link_port)
                             : "aucune borne ne s'est encore connectée au port " + std::to_string(cfg.link_port);
        return s;
    }
    case rt::CommBoard::Link::Up:
        return "relié : borne " + std::to_string(board->id()) + " sur " + std::to_string(board->count());
    case rt::CommBoard::Link::Lost: return "perdu (réinitialiser pour réessayer)";
    }
    return "";
}

std::string pref_file(const char *name) { return app::Config::pref_dir() + name; } // per ROM set and profile
template <typename C> void load_file(const std::string &path, C &into) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
    if (d.size() == into.size()) std::copy(d.begin(), d.end(), into.begin());
}
template <typename C> void save_file(const std::string &path, const C &from) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(from.data()), std::streamsize(from.size()));
}

int fail(const char *what) {
	char msg[4096];

    std::snprintf(
        msg,
        sizeof(msg),
        "%s failed: %s",
        what,
        SDL_GetError());

	SDL_Log("%s failed: %s", what, SDL_GetError());   // add this line
    std::fprintf(stderr, "daytona: %s: %s\n", what, SDL_GetError());
	SDL_ShowSimpleMessageBox(
	SDL_MESSAGEBOX_ERROR,
	"Daytona Error",
	msg,
	nullptr);
    return 1;
}

} // namespace

// Windows builds daytona as a GUI program, which has no console of its own:
// nothing it prints is seen. Started from a command window, attach to that
// window; otherwise write to daytona.log beside the settings.
void open_log() {
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        std::printf("\n");
        return;
    }
    const std::string log = pref_file("daytona.log");
    std::freopen(log.c_str(), "w", stdout);
    std::freopen(log.c_str(), "a", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#endif
}

int main(int argc, char **argv) {
    bool autostart = false;
#ifndef M2_MOBILE
    bool pc_home = std::strcmp(rt::rom_set_name(), "daytona") == 0;
#endif
    for (int i = 1; i < argc; i++) { // before anything reads or writes the data folder
        if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) app::Config::profile = argv[++i];
        else if (!std::strcmp(argv[i], "--autostart")) autostart = true;
#ifndef M2_MOBILE
        else if (!std::strcmp(argv[i], "--arcade")) pc_home = false;
#endif
    }
#ifndef M2_MOBILE
    pc_home &= !autostart;
    if (pc_home && app::Config::profile.empty()) app::Config::profile = "pc-solo";
#endif
    open_log();
    app::Config cfg;
    cfg.load();
    app::i18n::set(cfg.language.empty() ? app::i18n::system_default() : cfg.language); // before the fonts
    app::i18n::translate = &app::i18n::tr; // controls and force feedback labels
    app::i18n::translate_logical = &app::i18n::logical; // pacing status, formatted in the launcher
    // Launcher enhancements the runtime reads from the environment once, on
    // first use; a variable the player set by hand wins.
    if (cfg.scenery_3d)
        for (const char *name : {"M2_TREE_STAR", "M2_TREE_VOLUME", "M2_STAND_FILL", "M2_CANYON_FILL"})
            if (!std::getenv(name)) {
#ifdef _WIN32
                _putenv_s(name, "1");
#else
                setenv(name, "1", 0);
#endif
            }
    if (cfg.cars_near_model && !std::getenv("M2_CAR_NEAR_MODEL")) {
#ifdef _WIN32
        _putenv_s("M2_CAR_NEAR_MODEL", "1");
#else
        setenv("M2_CAR_NEAR_MODEL", "1", 0);
#endif
    }
    bool audio_timing_probe = false;
    uint64_t max_frames = 0;
    int window_width = 0, window_height = 0;
#ifndef M2_MOBILE
    bool hdr10 = false;
    const char *raster_capture = nullptr;
    bool interpolate_poses = cfg.interpolate; // the launcher option, or --interpolate-poses
    bool interpolate_flag = false;           // --interpolate-poses given on the command line
    std::optional<float> hdr10_paper_white;
    const char *capture_home = nullptr;
    const char *present_trace_path = nullptr;
    const char *hardware_trace_path = nullptr;
    const char *input_script_path = nullptr;
    const char *input_trace_path = nullptr;
    const char *effective_input_trace_path = nullptr;
    const char *max_frames_text = nullptr;
    std::optional<rt::StartRaceRequest> start_solo;
    std::optional<rt::RaceTransmission> solo_transmission;
    std::optional<rt::RaceMode> solo_mode;
#endif
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!std::strcmp(argv[i], "--gpu") && i + 1 < argc) cfg.gpu = argv[++i];
        else if (!std::strcmp(argv[i], "--audio")) {
            if (i + 1 == argc || (std::strcmp(argv[i + 1], "native") && std::strcmp(argv[i + 1], "reference"))) {
                std::fprintf(stderr, "daytona: --audio expects native or reference\n");
                return 1;
            }
            cfg.native_audio = !std::strcmp(argv[++i], "native");
        }
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            const char *value = argv[++i];
#ifndef M2_MOBILE
            max_frames_text = value;
#endif
            max_frames = std::strtoull(value, nullptr, 10);
        }
        else if (!std::strcmp(argv[i], "--window")) {
            char extra;
            if (i + 1 == argc || std::sscanf(argv[++i], "%dx%d%c", &window_width, &window_height, &extra) != 2 ||
                window_width < 496 || window_width > 7680 || window_height < 360 || window_height > 4320) {
                std::fprintf(stderr, "daytona: --window expects a size between 496x360 and 7680x4320\n");
                return 1;
            }
        }
#ifndef M2_MOBILE
        else if (!std::strcmp(argv[i], "--capture-home") && i + 1 < argc) capture_home = argv[++i];
        else if (!std::strcmp(argv[i], "--capture-raster")) {
            if (i + 1 >= argc) return 1;
            raster_capture = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--trace-software-presents")) {
            if (i + 1 == argc) {
                std::fprintf(stderr, "daytona: --trace-software-presents requires a CSV path\n");
                return 1;
            }
            present_trace_path = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--trace-hardware-presents")) {
            if (i + 1 == argc) {
                std::fprintf(stderr, "daytona: --trace-hardware-presents requires a CSV path\n");
                return 1;
            }
            hardware_trace_path = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--inputs")) {
            if (input_script_path || i + 1 == argc || !argv[i + 1][0] || argv[i + 1][0] == '-') {
                std::fprintf(stderr, "daytona: --inputs expects one file\n");
                return 1;
            }
            input_script_path = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--trace-player-inputs") ||
                 !std::strcmp(argv[i], "--trace-effective-inputs")) {
            const char *flag = argv[i];
            if (input_trace_path || effective_input_trace_path) {
                std::fprintf(stderr, "daytona: choose one input trace option once\n");
                return 1;
            }
            if (i + 1 == argc || !argv[i + 1][0] || argv[i + 1][0] == '-') {
                std::fprintf(stderr, "daytona: %s expects a file\n", flag);
                return 1;
            }
            if (!std::strcmp(flag, "--trace-effective-inputs")) effective_input_trace_path = argv[++i];
            else input_trace_path = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--native-audio-timing-probe")) audio_timing_probe = true;
        else if (!std::strcmp(argv[i], "--hdr10")) hdr10 = true;
        else if (!std::strcmp(argv[i], "--interpolate-poses")) interpolate_poses = interpolate_flag = true;
        else if (!std::strcmp(argv[i], "--hdr10-paper-white")) {
            if (hdr10_paper_white || i + 1 == argc) {
                std::fprintf(stderr, "daytona: --hdr10-paper-white expects one value in (0, 10000] cd/m2\n");
                return 1;
            }
            const char *first = argv[++i];
            const char *end = first + std::strlen(first);
            float nits = 0.f;
            const auto parsed = std::from_chars(first, end, nits);
            if (parsed.ec != std::errc{} || parsed.ptr != end ||
                !(nits > 0.f && nits <= app::kHdr10PeakNits)) {
                std::fprintf(stderr, "daytona: --hdr10-paper-white expects a value in (0, 10000] cd/m2\n");
                return 1;
            }
            hdr10_paper_white = nits;
        }
        else if (!std::strcmp(argv[i], "--start-solo")) {
            if (i + 1 == argc || std::strlen(argv[i + 1]) != 1 || argv[i + 1][0] < '1' || argv[i + 1][0] > '3') {
                std::fprintf(stderr, "daytona: --start-solo expects 1, 2 or 3\n");
                return 1;
            }
            start_solo = rt::StartRaceRequest{rt::RaceCourse(argv[++i][0] - '1')};
        }
        else if (!std::strcmp(argv[i], "--transmission")) {
            if (solo_transmission || i + 1 == argc ||
                !(solo_transmission = rt::parse_race_transmission(argv[i + 1]))) {
                std::fprintf(stderr, "daytona: --transmission expects automatic or manual once\n");
                return 1;
            }
            ++i;
        }
        else if (!std::strcmp(argv[i], "--race-mode")) {
            if (solo_mode || i + 1 == argc || !(solo_mode = rt::parse_race_mode(argv[i + 1]))) {
                std::fprintf(stderr, "daytona: --race-mode expects normal or time-attack once\n");
                return 1;
            }
            ++i;
        }
#endif
        else if (!std::strcmp(argv[i], "--fullscreen")) cfg.fullscreen = true;
        else if (!std::strcmp(argv[i], "--autostart")) autostart = true;
        else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) ++i; // read above
    }

#ifndef M2_MOBILE
    if (raster_capture && (cfg.renderer != "hardware" || !start_solo || !max_frames ||
                           hdr10 || capture_home || hardware_trace_path || present_trace_path)) {
        std::fprintf(stderr, "daytona: raster capture requires hardware, SDR and a bounded solo race\n");
        return 1;
    }
    if (hdr10_paper_white && !hdr10) {
        std::fprintf(stderr, "daytona: --hdr10-paper-white requires --hdr10\n");
        return 1;
    }
    if (interpolate_poses && (!pc_home || cfg.renderer != "hardware" || cfg.draw_mode != 0)) {
        // The launcher option is simply not applied there; the flag is an error.
        if (!interpolate_flag) {
            std::fprintf(stderr, "daytona: smoothing needs PC mode, hardware rendering and draw mode 0; off\n");
            interpolate_poses = false;
        } else {
            std::fprintf(stderr, "daytona: --interpolate-poses requires PC mode, hardware rendering and draw mode 0\n");
            return 1;
        }
    }
    if (solo_transmission && !start_solo) {
        std::fprintf(stderr, "daytona: --transmission requires --start-solo\n");
        return 1;
    }
    if (solo_mode && !start_solo) {
        std::fprintf(stderr, "daytona: --race-mode requires --start-solo\n");
        return 1;
    }
    if (start_solo && (!pc_home || capture_home)) {
        std::fprintf(stderr, "daytona: --start-solo requires the Revision A PC home\n");
        return 1;
    }
    if (solo_transmission) start_solo->transmission = *solo_transmission;
    if (solo_mode) start_solo->mode = *solo_mode;
    std::unique_ptr<SoftwarePresentTrace> present_trace;
    std::unique_ptr<app::HardwarePresentTrace> hardware_trace;
    const char *const capture_trace_path = input_trace_path ? input_trace_path : effective_input_trace_path;
    if (audio_timing_probe || capture_trace_path || input_script_path) {
        const char *first = max_frames_text ? max_frames_text : "";
        const char *end = first + std::strlen(first);
        const auto parsed = std::from_chars(first, end, max_frames);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !max_frames) {
            std::fprintf(stderr, "daytona: a positive decimal --frames is required for this diagnostic\n");
            return 1;
        }
    }
    if (audio_timing_probe && (!pc_home || !start_solo || !max_frames || capture_home)) {
        std::fprintf(stderr, "daytona: audio timing probe requires PC solo and --frames\n");
        return 1;
    }
    if (capture_trace_path && (!pc_home || !start_solo || !max_frames || capture_home)) {
        std::fprintf(stderr, "daytona: input trace requires PC solo and --frames\n");
        return 1;
    }
    std::optional<tools::Script> input_script;
    if (input_script_path) {
        if (!start_solo) {
            std::fprintf(stderr, "daytona: --inputs requires PC solo and --frames\n");
            return 1;
        }
        try {
            input_script.emplace().load(input_script_path);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "daytona: input script: %s\n", e.what());
            return 1;
        }
    }
    std::optional<PlayerInputCapture> player_input_capture;
    if (hardware_trace_path &&
        (present_trace_path || !pc_home || !start_solo || !max_frames ||
         capture_home || cfg.renderer != "hardware" || cfg.supersampling != 1)) {
        std::fprintf(stderr, "daytona: hardware present trace requires hardware renderer at 1x, "
                             "--start-solo and --frames\n");
        return 1;
    }
    uint64_t software_digest = 0;
    if (present_trace_path) {
        if (!pc_home || !start_solo || !max_frames || capture_home || cfg.renderer != "software") {
            std::fprintf(stderr, "daytona: software present trace requires software renderer, "
                                 "--start-solo and --frames\n");
            return 1;
        }
        try {
            present_trace = std::make_unique<SoftwarePresentTrace>(present_trace_path);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "daytona: %s\n", e.what());
            return 1;
        }
    }
#endif

    // the name graphics overlays and drivers see (patches/sdl3: Vulkan's application name)
    SDL_SetAppMetadata("Daytona USA", nullptr, "daytona-recomp");
#ifdef M2_MOBILE
    // Mobile shells are landscape-only and always occupy the display.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    cfg.fullscreen = true;
#endif
    if (!cfg.gpu.empty()) SDL_SetHint(SDL_HINT_GPU_DRIVER, cfg.gpu.c_str());
    if (cfg.legacy_logitech_wheels) { // before the joysticks start: SDL reads it when it finds the devices
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_LG4FF, "0");
        std::fprintf(stderr, "daytona: Logitech wheels through the system's driver\n");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail("SDL_Init");
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) std::fprintf(stderr, "daytona: no force feedback (%s)\n", SDL_GetError());
    Audio audio;
#ifndef M2_MOBILE
    app::LocalMusic local_music;
#endif
    app::NativeAudio<snd::NativeSoundEngine> native_audio;
    const bool audio_initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
    bool have_audio = false, native_active = false, native_fault = false;
    bool timed_audio_active = false;
    if (audio_initialized) std::printf("daytona: audio driver %s\n", SDL_GetCurrentAudioDriver());
    else std::fprintf(stderr, "daytona: no audio output (%s)\n", SDL_GetError());

    constexpr int W = rt::GameLoop::kWidth, H = rt::GameLoop::kHeight;
    const int kMaxW = W + 2 * rt::GameLoop::wide_margin(app::Config::kMaxAspect);
    int screen_w = W; // this frame's width: W, or wider with widescreen
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
#ifndef M2_MOBILE
    if (pc_home) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (!window_width) {
            window_width = 1440;
            window_height = 810;
        }
    }
    if (capture_home) {
        if (!pc_home) {
            std::fprintf(stderr, "daytona: --capture-home requires the Revision A PC home\n");
            return 1;
        }
        flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
#endif
    SDL_Window *window = SDL_CreateWindow("Daytona USA", window_width ? window_width : W * 2,
                                          window_height ? window_height : H * 2, flags);
    if (!window) return fail("SDL_CreateWindow");
    app::apply_fullscreen_mode(window, cfg.fullscreen_mode);
    SDL_Log("SDL %d; compiled GPU backends: %d", SDL_GetVersion(), SDL_GetNumGPUDrivers());
    for (int i = 0; i < SDL_GetNumGPUDrivers(); ++i)
        SDL_Log("GPU backend: %s", SDL_GetGPUDriver(i));
    SDL_GPUDevice *dev = app::create_gpu_device();
    if (!dev && !cfg.gpu.empty()) {
        // The chosen API is not available here (Vulkan on a Mac without MoltenVK): use the automatic choice.
        std::fprintf(stderr, "daytona: %s; falling back to automatic\n", SDL_GetError());
        SDL_ResetHint(SDL_HINT_GPU_DRIVER);
        cfg.gpu.clear();
        dev = app::create_gpu_device();
    }
    if (!dev) return fail("SDL_CreateGPUDevice");
    if (!SDL_ClaimWindowForGPUDevice(dev, window)) return fail("SDL_ClaimWindowForGPUDevice");
    if (const char *v = std::getenv("M2_FRAMES_IN_FLIGHT")) // experiment: 1 = each pass waits for the refresh
        SDL_SetGPUAllowedFramesInFlight(dev, Uint32(std::clamp(std::atoi(v), 1, 3)));
    std::printf("daytona: GPU driver %s\n", SDL_GetGPUDeviceDriver(dev));
#ifndef M2_MOBILE
    const float paper_white = hdr10_paper_white.value_or(app::kHdr10PaperWhiteNits);
    // Display curve: the launcher setting, or M2_DISPLAY_GAMMA (0 = the PC
    // sRGB curve, the reference; 2.0..2.8 = a CRT-like power law).
    float display_gamma = cfg.display_gamma;
    if (const char *v = std::getenv("M2_DISPLAY_GAMMA"); v && *v) {
        const float g = std::strtof(v, nullptr);
        display_gamma = std::isfinite(g) && g >= 2.0f ? std::min(g, 2.8f) : 0.0f;
    }
    float display_saturation = cfg.display_saturation;
    if (const char *v = std::getenv("M2_DISPLAY_SATURATION"); v && *v) {
        const float s = std::strtof(v, nullptr);
        display_saturation = std::isfinite(s) ? std::clamp(s, 0.5f, 1.5f) : 1.0f;
    }
    const bool display_adjusted = display_gamma > 0.0f || std::fabs(display_saturation - 1.0f) > 0.004f;
    app::Hdr10Present hdr_present(dev, paper_white, display_gamma, false, display_saturation);
    bool hdr_active = false;
    const char *gpu_driver = SDL_GetGPUDeviceDriver(dev);
    // The same curve without HDR10: a last pass into the SDR swapchain.
    app::Hdr10Present sdr_curve(dev, 0.0f, display_gamma, true, display_saturation);
    const bool sdr_curve_ready = display_adjusted && gpu_driver && std::strcmp(gpu_driver, "direct3d12") == 0 &&
                                 SDL_GetGPUSwapchainTextureFormat(dev, window) == app::Hdr10Present::scene_format &&
                                 sdr_curve.init();
    // M2_MIRE=FILE.bmp: show a colour test pattern instead of the game, through
    // the same display passes (curve, saturation, HDR10), to measure a capture chain.
    Uint32 mire_w = 0, mire_h = 0;
    SDL_GPUTexture *mire = nullptr;
    if (const char *path = std::getenv("M2_MIRE"); path && *path) {
        mire = load_bmp_texture(dev, path, mire_w, mire_h);
        if (!mire) SDL_Log("Test pattern %s not loaded: %s", path, SDL_GetError());
    }
    if (display_adjusted)
        std::printf("daytona: display curve gamma %.2f (0 = sRGB), saturation %.2f (%s)\n", display_gamma,
                    display_saturation, sdr_curve_ready ? "SDR pass ready" : "SDR pass unavailable; HDR10 only");
    const bool follow_hdr = hdr10 && !capture_home && gpu_driver &&
                            std::strcmp(gpu_driver, "direct3d12") == 0;
    auto update_hdr_output = [&](const char *event, bool window_hdr_changed = false) {
        const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        const SDL_PropertiesID props = display ? SDL_GetDisplayProperties(display) : 0;
        bool display_hdr = props &&
            SDL_GetBooleanProperty(props, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        const char *hdr_source = "SDL display";
        // SDL updates the window property before sending an HDR-state event.
        // On a display move, the window property may still describe the old display.
        if (window_hdr_changed) {
            const auto window_props = SDL_GetWindowProperties(window);
            if (SDL_HasProperty(window_props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN)) {
                display_hdr = SDL_GetBooleanProperty(window_props,
                    SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, display_hdr);
                hdr_source = "SDL window";
            }
        }
#ifdef DAYTONA_HAS_WINDOWS_COLOR_MODE
        // SDL's display-level property can retain the startup value on Windows.
        if (follow_hdr) {
            if (const auto windows_hdr = windows_display_hdr(props)) {
                display_hdr = *windows_hdr;
                hdr_source = "DisplayConfig";
            }
        }
#endif
        const bool supported = follow_hdr && SDL_WindowSupportsGPUSwapchainComposition(
            dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
        const auto choice = app::choose_hdr10(true, supported, display_hdr, gpu_driver);
        const bool want_hdr = choice.composition == app::Hdr10Composition::Hdr10St2084;
        if (want_hdr && !hdr_active && !hdr_present.init()) {
            SDL_Log("HDR10 compose setup failed, using SDR: %s", SDL_GetError());
            hdr_present.reset();
        } else if (want_hdr != hdr_active) {
            const auto composition = want_hdr ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084
                                              : SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
            const auto expected_format = want_hdr ? app::Hdr10Present::output_format
                                                  : app::Hdr10Present::scene_format;
            if (!SDL_SetGPUSwapchainParameters(dev, window, composition, SDL_GPU_PRESENTMODE_VSYNC) ||
                SDL_GetGPUSwapchainTextureFormat(dev, window) != expected_format) {
                SDL_Log("HDR10 swapchain transition failed, restoring SDR: %s", SDL_GetError());
                // D3D12 can destroy the previous swapchain before a failed recreation.
                SDL_ReleaseWindowFromGPUDevice(dev, window);
                if (!SDL_ClaimWindowForGPUDevice(dev, window) ||
                    SDL_GetGPUSwapchainTextureFormat(dev, window) != app::Hdr10Present::scene_format)
                    return false;
                hdr_active = false;
            } else {
                hdr_active = want_hdr;
            }
            if (!hdr_active) hdr_present.reset();
        }
        const char *name = display ? SDL_GetDisplayName(display) : nullptr;
        const auto format = SDL_GetGPUSwapchainTextureFormat(dev, window);
        std::printf("daytona: %s, display %s id %u, display HDR %s, active output %s, "
                    "swapchain SDL_GPUTextureFormat=%u%s, HDR state source %s\n",
                    event, name ? name : "unknown", static_cast<unsigned>(display),
                    display_hdr ? "on" : "off", hdr_active ? "HDR10_ST2084" : "SDR",
                    static_cast<unsigned>(format),
                    format == app::Hdr10Present::output_format ? " (R10G10B10A2_UNORM)" : "",
                    hdr_source);
        return true;
    };
    if (hdr10 && !capture_home) {
        SDL_SyncWindow(window);
        if (!update_hdr_output("HDR10 startup")) return fail("restore SDR swapchain");
        std::printf("daytona: HDR10 paper white %.5g cd/m2\n", paper_white);
    }
#endif

    // Dear ImGui on SDL3 + SDL_GPU (its shaders ship precompiled for every backend)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
#ifdef SDL_PLATFORM_IOS
    // UIKit reports logical points; the SDL backend handles Retina framebuffer
    // scaling. Applying the display scale here again makes controls oversized.
    ImGui::GetStyle().FontSizeBase = 17.0f;
    ImGui::GetStyle().FramePadding = ImVec2(8, 6);
    ImGui::GetStyle().ScrollbarSize = 20.0f;
#else
    ImGui::GetStyle().ScaleAllSizes(SDL_GetWindowDisplayScale(window));
#endif
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo ii;
    ii.Device = dev;
    ii.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, window);
#ifndef M2_MOBILE
    if (hdr_active) ii.ColorTargetFormat = app::Hdr10Present::scene_format;
#endif
    ImGui_ImplSDLGPU3_Init(&ii);

#ifndef M2_MOBILE
    app::HomeMenu home_menu;
    app::HomeScene home_scene;
    const bool home_3d = pc_home && home_scene.init(dev, ii.ColorTargetFormat);
    if (pc_home) {
        if (!home_3d) SDL_Log("3D home unavailable on this GPU backend: %s", SDL_GetError());
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        home_menu.init();
        if (start_solo) home_menu.select(*start_solo);
    } else {
        ImGui::GetIO().Fonts->AddFontDefaultVector(); // the launcher's, with the other scripts merged in
        app::i18n::add_fonts();
    }
    bool settings = false, pc_race = false;
    bool solo = false;
    std::string home_message;
    uint64_t reveal_at = 0;
    SDL_GPUTexture *capture_image = nullptr;
    int capture_status = 0;
    if (capture_home) {
        const SDL_GPUTextureCreateInfo info{SDL_GPU_TEXTURETYPE_2D, ii.ColorTargetFormat,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, Uint32(window_width), Uint32(window_height),
            1, 1, SDL_GPU_SAMPLECOUNT_1, 0};
        capture_image = home_3d ? SDL_CreateGPUTexture(dev, &info) : nullptr;
        if (!capture_image) {
            SDL_Log("Home capture setup failed: %s", SDL_GetError());
            capture_status = 1;
        }
    }
#endif

    // Hardware renderer (launcher > Renderer): the 3D on the GPU. If it cannot
    // start here, the software renderer is used and the launcher says why.
    app::GpuRenderer gpu;
    if (!gpu.init(dev, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM))
        std::fprintf(stderr, "daytona: hardware renderer unavailable: %s\n", gpu.error().c_str());
    // The experimental ray-tracing options are both off by
    // default. M2_DXR_SCENE=1 builds the frame's acceleration structure after
    // each hardware render (nothing reads it; the image is unchanged).
    // M2_DXR_SHADOWS=1 also casts sun shadows on the native-resolution path:
    // the frame is drawn in its own command buffer, submitted, the DXR pass
    // runs on the same queue, then the frame is multiplied by its light.
    // M2_DXR_SUN=x,y,z replaces the game's light vector (camera space),
    // M2_DXR_STRENGTH the light left in shadow (0.6). The GPU time is
    // reported at exit, where it is released before SDL destroys the device
    // (and unloads d3d12.dll: releasing after that crashed).
    app::DxrScene dxr_scene;
    std::vector<float> dxr_triangles;
    bool dxr_shadows = false;
    constexpr int kShadowTopMargin = 384; // Geo::set_top_margin, pixels above full-screen viewports
    float dxr_strength = 0.6f;
    std::array<float, 3> dxr_sun_override{};
    // sky light: its share of the light (M2_DXR_SKY, 0..1) and reach (M2_DXR_SKY_RADIUS)
    float dxr_sky = cfg.sky_light ? 0.45f : 0.0f, dxr_sky_radius = 3.0f;
    // the sun's disc, degrees across (the real one 0.53: slightly soft edges; M2_DXR_SUN_SIZE)
    float dxr_sun_size = 0.53f;
    // light shafts: sunlit air between shadows (M2_DXR_SHAFTS strength, 0 = off,
    // M2_DXR_SHAFT_DIST how far the eye ray is marched, camera units)
    float dxr_shafts = 0.2f, dxr_shaft_dist = 60.0f; // with the shadows; 0.8, then 0.4 until 24/11: a veil that moved with the car; weighted by the angle to the sun
    int dxr_sun_height = cfg.sun_height; // degrees, 0 = the game's own (M2_DXR_SUN_HEIGHT overrides)
    bool dxr_sun_fixed = false;
    const char *dxr_scene_env = std::getenv("M2_DXR_SCENE");
    const char *dxr_shadows_env = std::getenv("M2_DXR_SHADOWS");
    // the launcher's "Ombres du soleil" (sun_shadows), unless the variable says otherwise
    const bool dxr_shadows_wanted = dxr_shadows_env ? dxr_shadows_env[0] == '1' : cfg.sun_shadows;
#ifdef M2_RAY_TRACING
    const bool dxr_wanted = (dxr_scene_env && dxr_scene_env[0] == '1') || dxr_shadows_wanted;
#else
    const bool dxr_wanted = false; // built without ray tracing (CMake M2_RAY_TRACING)
    (void)dxr_scene_env;
#endif
    if (dxr_wanted && gpu.ok()) {
        if (dxr_scene.init(dev)) {
            gpu.collect_triangles(&dxr_triangles);
            dxr_shadows = dxr_shadows_wanted && gpu.enable_shadows();
            if (const char *v = std::getenv("M2_DXR_SUN_SIZE")) dxr_sun_size = std::clamp(float(std::atof(v)), 0.0f, 10.0f);
            if (const char *v = std::getenv("M2_DXR_SKY")) dxr_sky = std::clamp(float(std::atof(v)), 0.0f, 1.0f);
            if (const char *v = std::getenv("M2_DXR_SKY_RADIUS")) dxr_sky_radius = std::max(float(std::atof(v)), 0.1f);
            if (const char *v = std::getenv("M2_DXR_SHAFTS")) dxr_shafts = std::clamp(float(std::atof(v)), 0.0f, 1.0f);
            if (const char *v = std::getenv("M2_DXR_SHAFT_DIST")) dxr_shaft_dist = std::max(float(std::atof(v)), 1.0f);
            if (const char *v = std::getenv("M2_DXR_SUN_HEIGHT")) dxr_sun_height = std::clamp(std::atoi(v), 0, 89);
            if (const char *v = std::getenv("M2_DXR_STRENGTH")) dxr_strength = std::clamp(float(std::atof(v)), 0.0f, 1.0f);
            if (const char *v = std::getenv("M2_DXR_SUN"))
                dxr_sun_fixed = std::sscanf(v, "%f,%f,%f", &dxr_sun_override[0], &dxr_sun_override[1],
                                            &dxr_sun_override[2]) == 3;
            std::printf("daytona: DXR scene on (ray tracing tier 1.1), shadows %s\n", dxr_shadows ? "on" : "off");
        } else {
            std::fprintf(stderr, "daytona: DXR scene unavailable: %s\n", dxr_scene.error().c_str());
        }
    }
#ifndef M2_MOBILE
    if (hardware_trace_path) {
        if (!gpu.ok()) return 1;
        try {
            hardware_trace = std::make_unique<app::HardwarePresentTrace>(dev, Uint32(kMaxW), H, hardware_trace_path);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "daytona: %s\n", e.what());
            return 1;
        }
    }
#endif

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the screen's 0xAARRGGBB words, little-endian
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; // the hardware renderer draws into it
    ti.width = kMaxW; // the widest screen (widescreen); each frame uses its own width
    ti.height = H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *screen = SDL_CreateGPUTexture(dev, &ti);
#ifndef M2_MOBILE
    SDL_GPUTexture *raster_target = nullptr;
    Uint32 raster_width = 0, raster_height = 0, raster_scale = 1;
    bool raster_reported = false;
    bool raster_captured = false;
#endif
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = Uint32(kMaxW) * H * 4;
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!screen || !upload) return fail("SDL_CreateGPUTexture");
    // The hardware renderer at a higher internal resolution (launcher >
    // Super sampling) draws into its own texture, made when the scale changes,
    // with mip levels: the window shows the level nearest its size, so a
    // frame bigger than the window is averaged down (supersampled).
    SDL_GPUTexture *hires = nullptr;
    int hires_scale = 1, hires_levels = 1;
    SDL_GPUTexture *shown = screen; // the last frame's texture, and its scale
    int shown_scale = 1;

    app::Launcher launcher(cfg, window);
    app::ForceFeedback ffb; // the drive board on the steering device
    std::unique_ptr<app::TcpLink> link; // link play (declared first: outlives the game, which holds it)
    std::unique_ptr<rt::GameLoop> game;
    bool pose_session = false, last_render_was_pose = false;
    uint64_t pose_interval_ns = 0;
    double pose_lag_mean = 0, pose_lag_dev = 0; // snapshot availability after its due instant, ns
    double pose_sample_ns = 0;          // the drawn instant, on its own steady clock
    double pose_shown_ns = 0, pose_shown_lead = 0; // when the pass will be shown (fixed refresh)
    uint64_t pose_presentations = 0, pose_fallbacks = 0;
    struct PoseReport { // printed at exit: how often smoothing gave up for the latest tick
        const uint64_t &total, &fallbacks;
        ~PoseReport() {
            if (total) std::printf("daytona: smoothing fell back to the latest tick on %llu of %llu presentations\n",
                                   (unsigned long long)fallbacks, (unsigned long long)total);
            std::fflush(stdout);
        }
    } pose_report{pose_presentations, pose_fallbacks};
    bool pose_discontinuity = true;
    const std::string eeprom_path = pref_file("ioboard_eeprom.bin"), backup_path = pref_file("backup_ram.bin");
    auto save_nv = [&] {
        if (!game) return;
        save_file(eeprom_path, game->board().io().eeprom);
        save_file(backup_path, game->board().backup_ram());
    };
    auto start_game = [&](bool solo_mode = false) {
        save_nv();
        pose_session = last_render_was_pose = false;
        pose_sample_ns = pose_shown_ns = pose_shown_lead = 0;
        pose_discontinuity = true;
        ffb.reset(); // a new game: the drive board as at power-on
        native_audio.close(); // joins callback before replacing its ROMs/engine
#ifndef M2_MOBILE
        local_music.reset();
#endif
        audio.close();
        game.reset();
        link.reset();
        have_audio = false;
        native_fault = false;
        timed_audio_active = audio_timing_probe && solo_mode;
        native_active = cfg.native_audio || timed_audio_active;
        try {
            auto images = rt::import_rom_set(cfg.rom_path);
            if (native_active) {
                if (!audio_initialized) throw std::runtime_error("Native audio needs an available audio device.");
                auto engine = std::make_unique<snd::NativeSoundEngine>(std::move(images.sound_program),
                    std::move(images.pcm1), std::move(images.pcm2),
                    timed_audio_active ? snd::NativeSoundEngine::Timing::SerializedUart
                                       : snd::NativeSoundEngine::Timing::Immediate);
                constexpr uint64_t kProbeLeadSamples = 2048;
                const bool opened = timed_audio_active ? native_audio.open_timed(std::move(engine), kProbeLeadSamples)
                                                       : native_audio.open(std::move(engine));
                if (!opened)
                    throw std::runtime_error(std::string("Cannot open native audio: ") + SDL_GetError());
                native_audio.volume(cfg.volume);
                native_audio.volumes(cfg.music_volume, cfg.effects_volume);
                native_audio.mute(cfg.mute);
            } else if (audio_initialized) {
                have_audio = audio.open(snd::SoundBoard::kYmClock / 144.0, snd::SoundBoard::kPcmClock / 224.0);
                if (!have_audio) std::fprintf(stderr, "daytona: reference audio unavailable (%s)\n", SDL_GetError());
            }
            game = std::make_unique<rt::GameLoop>(std::move(images), !native_active);
            if (timed_audio_active) std::printf("daytona: experimental UART audio timing probe\n");
            std::printf("daytona: audio backend %s\n", native_active
                ? "native (experimental, 48000 Hz device clock; no reference sound board)" : "reference");
            load_file(eeprom_path, game->board().io().eeprom);
            load_file(backup_path, game->board().backup_ram());
            if (cfg.link && !solo_mode) { // link play: the communication board on TCP
                link = std::make_unique<app::TcpLink>(uint16_t(cfg.link_port), cfg.link_next);
                if (!link->error().empty()) throw std::runtime_error("Link play: " + link->error());
                game->board().set_link(link.get(), cfg.link_framesync);
                std::printf("daytona: link play: listening on port %d, next cabinet %s\n", cfg.link_port, cfg.link_next.c_str());
            }
            launcher.set_error("");
            return true;
        } catch (const std::exception &e) {
            native_audio.close();
            audio.close();
            game.reset();
            link.reset();
            launcher.set_error(e.what());
            return false;
        }
    };

    bool in_launcher = true, running = true, have_frame = false, new_frame = false;
#ifndef M2_MOBILE
    if (capture_status) running = false;
#endif
    bool cursor_hidden = false; // the launcher's "Hide mouse cursor in game", in effect now
    int reported_hardware = -1; // the renderer last reported (-1: none yet)
    // Skip launcher (saved) or --autostart: straight into the game when the ROM
    // set checks out; otherwise the launcher shows, with the reason.
#ifndef M2_MOBILE
    if (!pc_home)
#endif
        if ((autostart || cfg.skip_launcher) && launcher.rom_ok() && start_game()) in_launcher = false;
#ifndef M2_MOBILE
    auto begin_solo = [&](rt::StartRaceRequest request) {
        if (!rt::valid_course(request.course) || !rt::valid_transmission(request.transmission)) {
            home_message = "Paramètres de course invalides.";
            return false;
        }
        if (!launcher.rom_ok()) {
            home_message = "Choisis une archive Daytona Revision A valide dans Réglages.";
            return false;
        }
        // start_game replaces the old session even if loading fails. Once that
        // destructive attempt begins, Resume must not refer to the old race.
        // Keep it available when the earlier ROM preflight alone fails.
        solo = pc_race = have_frame = false;
        if (!start_game(true)) {
            home_message = launcher.error();
            return false;
        }
        const auto accepted = game->request_race(request);
        if (accepted != rt::RaceStartResult::Accepted) {
            // No accepted PC session: the diagnostic launcher must not resume
            // this fresh game either. Close audio before discarding its owner.
            native_audio.close();
            audio.close();
            game.reset();
            link.reset();
            have_audio = false;
            home_message = "La demande de course a été refusée.";
            return false;
        }
        solo = true;
        pc_race = have_frame = false;
        in_launcher = false;
        home_message.clear();
        return true;
    };
    if (capture_trace_path) {
        try {
            player_input_capture.emplace(capture_trace_path);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "%s\n", error.what());
            capture_status = 1;
            running = false;
        }
    }
    bool start_solo_ok = !start_solo;
    if (running && start_solo && !begin_solo(*start_solo)) {
        std::fprintf(stderr, "daytona: %s\n", home_message.c_str());
        running = false;
    }
#endif
    auto arcade_music_volume = [&] {
#ifndef M2_MOBILE
        if (local_music.playing()) return 0.f;
#endif
        return cfg.music_volume;
    };
    auto sync_native_audio = [&] {
        if (native_fault) in_launcher = true;
#ifndef M2_MOBILE
        if (audio_initialized && std::strcmp(rt::rom_set_name(), "daytona") == 0) {
            local_music.configure(cfg.music_version, cfg.music_directory);
            local_music.sync(game && !in_launcher && running, cfg.mute || solo ? 0.f : cfg.volume * cfg.music_volume);
        }
#endif
        if (!native_audio.available()) return;
        native_audio.volume(
#ifndef M2_MOBILE
            solo ? 0.f :
#endif
            cfg.volume);
        native_audio.volumes(arcade_music_volume(), cfg.effects_volume);
        native_audio.mute(cfg.mute);
        if (in_launcher || !running) {
            if (!native_audio.pause() && timed_audio_active) {
                launcher.set_error(std::string("Timed native audio pause failed: ") + SDL_GetError());
                native_fault = true;
            }
        } else if (!native_audio.resume()) {
            launcher.set_error(std::string("Native audio resume failed; reset the game: ") + SDL_GetError());
            in_launcher = native_fault = true;
            native_audio.pause();
        }
    };
    sync_native_audio();
    app::Devices devices; // the gamepad and every joystick (wheels, pedals, shifters)
    // Frame pacing (launcher > Display; app/pacing.h): the arcade's speed on
    // the wall clock unless a setting syncs to the display or holds frames for VRR.
    static_assert(app::kArcadeFrameHz == kArcadeHz);
    auto display_refresh = [&] {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        if (!mode) return 0.0;
        if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
            return double(mode->refresh_rate_numerator) / mode->refresh_rate_denominator;
        return double(mode->refresh_rate);
    };
    app::Pacer pacer;
    pacer.set(app::Pacing{}, SDL_GetTicksNS());
    rt::TestHold test_hold; // the launcher's "Hold Test button"
#ifdef M2_MOBILE
    // Preserve startup detection for a controller paired before launching.
    int pad_count = 0;
    SDL_JoystickID *pads = SDL_GetGamepads(&pad_count);
    if (pads && pad_count > 0) devices.pad = SDL_OpenGamepad(pads[0]);
    SDL_free(pads);
#endif

#ifdef M2_MOBILE
    app::TouchControls touch;
    SDL_Rect touch_area{};
    int touch_width = 0, touch_height = 0;
#endif
#ifndef M2_MOBILE
    bool ffb_focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
    while (running) {
#ifndef M2_MOBILE
        auto cancel_start = [&] {
            if (game) game->cancel_race_start();
            solo = false;
            pc_race = false;
            in_launcher = true;
            have_frame = false;
        };
#endif
#ifdef M2_MOBILE
        int tw = 0, th = 0;
        SDL_GetWindowSize(window, &tw, &th);
        SDL_Rect area{0, 0, tw, th};
        SDL_GetWindowSafeArea(window, &area);
        if (tw != touch_width || th != touch_height || area.x != touch_area.x || area.y != touch_area.y ||
            area.w != touch_area.w || area.h != touch_area.h) {
            touch.layout(float(area.x), float(area.y), float(area.w), float(area.h));
            touch_area = area; touch_width = tw; touch_height = th;
        }
        if (in_launcher) touch.clear();
#endif
#ifndef M2_MOBILE
        const char *hdr_output_event = nullptr;
        bool hdr_window_changed = false;
#endif
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
#ifndef M2_MOBILE
            if (follow_hdr && (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
                               e.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED) &&
                e.window.windowID == SDL_GetWindowID(window)) {
                hdr_window_changed = e.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED;
                hdr_output_event = hdr_window_changed ? "HDR10 display state changed"
                                                      : "HDR10 display changed";
            }
#endif
#ifdef M2_MOBILE
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST || e.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
                touch.clear();
                in_launcher = true;
            }
            if (!in_launcher && (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION ||
                e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED)) {
                const float tx = e.tfinger.x * touch_width, ty = e.tfinger.y * touch_height;
                if (e.type == SDL_EVENT_FINGER_DOWN)
                    in_launcher = touch.down(e.tfinger.touchID, e.tfinger.fingerID, tx, ty);
                else if (e.type == SDL_EVENT_FINGER_MOTION)
                    touch.move(e.tfinger.touchID, e.tfinger.fingerID, tx, ty);
                else if (e.type == SDL_EVENT_FINGER_UP)
                    touch.up(e.tfinger.touchID, e.tfinger.fingerID);
                else if (e.type == SDL_EVENT_FINGER_CANCELED)
                    touch.cancel(e.tfinger.touchID, e.tfinger.fingerID);
            }
#endif
            // Release host effects before Devices drops its SDL reference.
            if (e.type == SDL_EVENT_JOYSTICK_REMOVED) ffb.device_removed(e.jdevice.which);
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED) ffb.device_removed(e.gdevice.which);
#ifndef M2_MOBILE
            if ((e.type == SDL_EVENT_WINDOW_FOCUS_LOST || e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) &&
                e.window.windowID == SDL_GetWindowID(window)) {
                ffb_focused = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                if (!ffb_focused) ffb.stop();
            }
#endif
            devices.handle_event(e);
            if (e.type == SDL_EVENT_QUIT) {
                ffb.stop();
                running = false;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11) {
                cfg.fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(window, cfg.fullscreen);
                cfg.save();
            }
#ifndef M2_MOBILE
            const bool back = (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE) ||
                (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST);
            if (pc_home && back && solo) cancel_start();
            else if (pc_home && back && settings) settings = false;
            else if (pc_home && pc_race && !in_launcher && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                     e.gbutton.button == SDL_GAMEPAD_BUTTON_START) in_launcher = true;
            else if (pc_home && (solo || (in_launcher && !settings))) ImGui_ImplSDL3_ProcessEvent(&e);
            else
#endif
            if (in_launcher) {
                if (!launcher.handle_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
                in_launcher = true; // pause and show the launcher
            }
        }

#ifndef M2_MOBILE
        // D3D12 SDR and the HDR intermediate use the same BGRA8 target.
        // Switch after draining events and before acquiring a command buffer.
        if (running && hdr_output_event && !update_hdr_output(hdr_output_event, hdr_window_changed)) {
            SDL_Log("Could not restore the SDR swapchain: %s", SDL_GetError());
            capture_status = 1;
            break;
        }
#endif
#ifdef M2_MOBILE
        if (in_launcher) touch.clear();
        cfg.controls.touch = touch.values;
        cfg.controls.touch_steering = touch.steering;
#endif
        // The mouse cursor: hidden while the game plays if the launcher says so,
        // shown whenever the launcher is open. ImGui sets the cursor on every
        // frame it draws (mobile draws its touch controls in play), so it is
        // told to leave the cursor alone while hidden.
        if (const bool hide = cfg.hide_cursor && game && !in_launcher; hide != cursor_hidden) {
            hide ? SDL_HideCursor() : SDL_ShowCursor();
            if (hide) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            else ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            cursor_hidden = hide;
        }

        sync_native_audio();
        if (audio_timing_probe && native_fault) break;
        if (native_active) {
            const auto health = native_audio.stats();
            // Unsupported audio operations leave playback running.
            static uint32_t unsupported_reported = 0;
            if (health.unsupported > unsupported_reported) {
                if (!unsupported_reported)
                    std::fprintf(stderr, "daytona: native audio: skipped unsupported operations\n");
                unsupported_reported = health.unsupported;
            }
            if (health.failed || health.invalid) {
                native_audio.pause();
                char message[192];
                std::snprintf(message, sizeof message,
                    "Native audio fault (callback %u, invalid %u, unsupported %u); reset or select reference audio.",
                    health.failed, health.invalid, health.unsupported);
                launcher.set_error(message);
                in_launcher = native_fault = true;
                if (audio_timing_probe) break;
            }
        }

        // Game: arcade speed (57.52 frames/s), presented at the display's rate,
        // unless a frame pacing setting says otherwise.
        const double refresh = display_refresh();
        if (const app::Pacing p = app::choose_pacing(refresh,
#ifndef M2_MOBILE
            pc_home ? app::PacingSettings{} :
#endif
            app::PacingSettings{cfg.pace_smooth, cfg.pace_sync_display, cfg.pace_vrr});
            !(p == pacer.pacing())) {
            pacer.set(p, SDL_GetTicksNS());
            std::printf("daytona: frame pacing: %s\n", p.describe(refresh).c_str());
        }
        launcher.set_pacing_status(pacer.pacing().describe(refresh));
        if (game && !in_launcher)
            if (const uint64_t wait = pacer.wait_ns(SDL_GetTicksNS())) SDL_DelayPrecise(wait); // VRR: hold the frame
        const uint64_t now = SDL_GetTicksNS();
#ifndef M2_MOBILE
        int pixel_width = 0, pixel_height = 0;
        // Native mode uses the DXIL viewport-aware tile shaders.
        const bool native_resolution = !capture_image && cfg.renderer == "hardware" && gpu.ok() && gpu_driver &&
            std::strcmp(gpu_driver, "direct3d12") == 0 &&
            cfg.uses_native_resolution() &&
            SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) &&
            pixel_width > 0 && pixel_height > 0;
#endif
        if (game && !in_launcher) {
#ifndef M2_MOBILE
            // "4:3": the cabinet's picture in every mode, centred with bars. In the
            // native mode, "" (automatic) takes the window's shape, as before, and
            // 16:10, 16:9, 21:9 their own, centred with bars on a wider window
            // (a narrower one keeps its own shape: no bars above and below).
            {
                const double window = native_resolution ? double(pixel_width) / pixel_height : 0.0;
                const double chosen = cfg.aspect_ratio();
                game->set_aspect(cfg.arcade_43() ? 0.0
                                 : !native_resolution ? chosen
                                 : chosen > 0 ? std::min(chosen, window) : window);
            }
#else
            game->set_aspect(cfg.arcade_43() ? 0.0 : cfg.aspect_ratio());
#endif
            game->set_hud_edges(cfg.hud_edges);
            // shadows: a screen height above the top edge stays in the scene
            // (gantry, arch overhead), hidden by the clip rectangle
            static const int top_margin = [] { // M2_DXR_TOP_MARGIN: pixels (diagnostic)
                const char *v = std::getenv("M2_DXR_TOP_MARGIN");
                return v && *v ? std::atoi(v) : kShadowTopMargin;
            }();
            game->board().set_top_margin(dxr_shadows && native_resolution ? top_margin : 0);
            game->set_frame_skip(cfg.draw_mode);
            game->board().video().set_external_3d(cfg.renderer == "hardware" && gpu.ok(), true);
            game->set_stretch_backdrop(cfg.stretch_backdrop);
            rt::GameLoop::set_draw_distance(cfg.draw_distance);
            game->set_draw_budget(cfg.draw_budget);
#ifndef M2_MOBILE
            const bool track_shafts = dxr_shadows && dxr_shafts > 0.0f && native_resolution &&
                                      cfg.renderer == "hardware" && gpu.ok();
            game->board().track_shaft_route(track_shafts);
#endif
            for (int frames = pacer.frames(now); frames > 0; --frames) {
                rt::Inputs inputs = cfg.controls.sample(SDL_GetKeyboardState(nullptr), devices);
#ifndef M2_MOBILE
                if (solo) inputs = {}; // the runtime owns native startup inputs
                else if (pc_home) {
                    // Replays use absolute simulation ticks after the native start.
                    if (input_script && game->race_status() == rt::RaceStatus::Racing)
                        inputs = input_script->at(game->frames());
                    inputs.in0 |= 0x0d; // cabinet-only coin, test and service signals
                }
                else
#endif
                {
                    if (cfg.hold_test != test_hold.armed()) cfg.hold_test ? test_hold.arm() : test_hold.cancel();
                    if (test_hold.apply(game->frames(), inputs.in0)) cfg.hold_test = false; // held: the box clears
                }
#ifndef M2_MOBILE
                if (local_music.available() && game->sound()) {
                    game->run_frame_deferred_sound(inputs);
                    local_music.consume(game->deferred_sound_bytes());
                    game->sound()->set_volumes(arcade_music_volume(), cfg.effects_volume);
                    game->complete_deferred_sound(game->execute_deferred_sound());
                } else
#endif
                    game->run_frame(inputs);
#ifndef M2_MOBILE
                if (interpolate_poses && !pose_session && (solo || pc_race)) {
                    pose_session = game->board().enable_pose_presentation();
                    if (!pose_session) {
                        std::fprintf(stderr, "daytona: interpolation unavailable for this program revision\n");
                        interpolate_poses = false;
                    }
                }
                if (pose_session || track_shafts) {
                    pose_interval_ns = uint64_t(std::llround(1e9 / pacer.pacing().game_hz));
                    const uint64_t owed = uint64_t(frames - 1) * pose_interval_ns;
                    // Clock mode may process the tick after its deadline.
                    // Use the scheduled instant when sampling the delayed pose.
                    const uint64_t last_due = pacer.last_due_ns(now);
                    const uint64_t tick_time = last_due > owed ? last_due - owed : 0;
                    if (auto stamp = rt::completed_snapshot_stamp(game->frames(), tick_time,
                                                                 game->board().video().instance())) {
                        stamp->discontinuity_before |= pose_discontinuity || !pc_race || solo;
                        game->board().stamp_pose_scene(*stamp);
                        pose_discontinuity = !pc_race || solo;
                        // How long after its due instant this snapshot became
                        // available (the loop waits on vsync: about 7 ms at
                        // 119 Hz). Sampling exactly one interval back ran past
                        // the latest snapshot for that long every tick: 38% of
                        // the presentations froze at alpha 1, then the cars
                        // jumped ahead, a car doubled forward (22/11). Sample
                        // that much further back: the mean lag (the
                        // deviation is measured, not added).
                        if (pose_session && frames == 1) {
                            const double lag = double(SDL_GetTicksNS() - std::min(SDL_GetTicksNS(), tick_time));
                            const double clamped = std::min(lag, double(pose_interval_ns));
                            pose_lag_mean += (clamped - pose_lag_mean) * 0.05;
                            pose_lag_dev += (std::abs(clamped - pose_lag_mean) - pose_lag_dev) * 0.05;
                        }
                    }
                }
#endif
#ifndef M2_MOBILE
                if (player_input_capture && !player_input_capture->write(game->frames() - 1,
                        effective_input_trace_path ? game->board().io().inputs : inputs)) {
                    std::fprintf(stderr, "daytona: input trace write failed\n");
                    capture_status = 1;
                    running = false;
                    break;
                }
#endif
#ifdef M2_MOBILE
                touch.consumed();
                cfg.controls.touch = touch.values;
                cfg.controls.touch_steering = touch.steering;
#endif
                if (native_active) {
                    const auto bytes = game->board().take_sound_bytes();
#ifndef M2_MOBILE
                    if (local_music.available()) {
                        local_music.consume(bytes);
                        native_audio.volumes(arcade_music_volume(), cfg.effects_volume);
                    }
#endif
                    bool accepted;
                    if (timed_audio_active) {
                        const auto stamp = app::NativeAudioSimClock{}.frame_for(game->frames() - 1);
                        accepted = stamp && native_audio.send_at(*stamp, bytes.data(), bytes.size());
                    } else {
                        accepted = native_audio.send(bytes.data(), bytes.size());
                    }
                    if (!accepted) {
                        native_audio.pause();
                        launcher.set_error("Native audio command queue failed; reset the game or select reference audio.");
                        in_launcher = native_fault = true;
                        break;
                    }
                }
                new_frame = have_frame = true;
                if (max_frames && game->frames() >= max_frames) running = false;
#ifndef M2_MOBILE
                if (solo) {
                    const auto status = game->race_status();
                    if (status == rt::RaceStatus::Racing) {
                        std::printf("daytona: PC solo race started at frame %llu\n", (unsigned long long)game->frames());
                        solo = false;
                        pc_race = true;
                        start_solo_ok = true;
                        reveal_at = now;
                    } else if (status != rt::RaceStatus::Starting) {
                        if (status == rt::RaceStatus::WrongCourse)
                            home_message = "Le jeu a sélectionné un autre circuit.";
                        else if (status == rt::RaceStatus::WrongMode)
                            home_message = "Le jeu a sélectionné un autre mode.";
                        else if (status == rt::RaceStatus::WrongTransmission)
                            home_message = "Le jeu a sélectionné une autre boîte de vitesses.";
                        else
                            home_message = "Le jeu n’a pas confirmé le départ.";
                        cancel_start();
                        if (start_solo && max_frames) running = false;
                        break;
                    }
                } else if (pc_race && game->race_status() == rt::RaceStatus::Finished) {
                    if (player_input_capture) running = false;
                    cancel_start();
                    break;
                }
#endif
                if (!running) break;
            }
            // Keep the emulated drive board current while host output is disabled.
            bool enable_ffb = running && !in_launcher;
#ifndef M2_MOBILE
            enable_ffb = enable_ffb && !solo && ffb_focused && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
#endif
            ffb.update(std::exchange(game->board().io().drive_commands, {}), devices, cfg.controls,
                       enable_ffb ? cfg.ffb_strength : 0.f,
                       cfg.ffb_invert, cfg.ffb_log, game->frames(),
                       (float(game->board().io().inputs.steer) - 128.0f) / 96.0f,
#ifdef __linux__
                       cfg.ffb_centring_override); // Linux only: drivers without a spring effect
#else
                       false);
#endif
            launcher.set_ffb_device(ffb.device_kind());
            launcher.set_link_status(link_status(game->board().comm_board(), link.get(), cfg));
            if (game->sound()) {
                game->sound()->set_volumes(arcade_music_volume(), cfg.effects_volume);
                if (have_audio) audio.push(*game->sound(),
#ifndef M2_MOBILE
                    solo || in_launcher ? 0.f :
#endif
                    cfg.mute ? 0.f : cfg.volume, pacer.pacing().game_hz / kArcadeHz);
                else game->sound()->take_fm(), game->sound()->take_pcm(); // nowhere to play it
            }
        } else {
            pacer.pause(now);
            if (pose_session && game) {
                game->board().discard_pose_history();
                pose_discontinuity = true;
            }
            ffb.stop(); // paused in the launcher: let the wheel go
        }

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (!cmd) return fail("SDL_AcquireGPUCommandBuffer");
#ifndef M2_MOBILE
        SDL_GPUTexture *swap = nullptr;
        Uint32 sw = 0, sh = 0;
        if (native_resolution &&
            !SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh))
            return fail("swapchain");
        if (native_resolution && !swap) {
            // SDL may return no texture while the window is minimized.
            // Keep the pending game image for the first drawable iteration:
            // this viewport can be wider than the legacy fallback texture.
            if (!SDL_SubmitGPUCommandBuffer(cmd)) return fail("submit without swapchain");
            SDL_Delay(1);
            continue;
        }
#endif
        const bool hardware = game && game->board().video().external_3d();
        if (game && hardware != reported_hardware) { // say which renderer is drawing, whenever it changes
            std::printf("daytona: renderer %s\n", hardware ? "hardware (GPU)" : "software (CPU)");
            reported_hardware = hardware;
        }
        const std::vector<rt::GeoPoly> *pose_polys = nullptr;
#ifndef M2_MOBILE
        if (pose_session && hardware && pc_race && !in_launcher && cfg.draw_mode == 0) {
            // The drawn instant is the real time of this pass minus a fixed
            // delay: 1.25 intervals plus the measured lag (1.5 before the display clock: centred, 23/11). Snapshots n-2, n-1
            // and n are kept (the third snapshot), so the instant stays
            // between n-2 and n with half an interval of margin either side.
            // The passes are not locked to the refresh (4 to 17 ms apart,
            // about 60 per second in heavy traffic): a clock stepping one
            // refresh per pass fell behind there, then caught up in jumps,
            // sometimes backwards, a car moving back and forth (5-lap runs,
            // 22/11). The real time is also what a variable-refresh screen shows.
            //
            // On a fixed-refresh screen the passes are not shown when they run
            // but one per refresh: with every opponent at the near model they
            // came in pairs 3 then 14 ms apart, shown 8.3 ms apart at 120 Hz,
            // so a car moved a small step then a large one, every other
            // refresh: it was seen to shake in play, not in a 60 frames/s video
            // (23/11). Sample for when the pass will be shown instead: the
            // refresh after the previous one shown, or the next one after now
            // if the queue ran dry, at most two ahead (frames in flight).
            // Its mean lead on the real time is taken off, so the delay to the
            // snapshots stays as measured. M2_POSE_DISPLAY_CLOCK=0: real time.
            const uint64_t present_now = SDL_GetTicksNS();
            static const bool display_clock = [] {
                const char *v = std::getenv("M2_POSE_DISPLAY_CLOCK");
                return !(v && v[0] == '0');
            }();
            double shown = double(present_now);
            if (display_clock && refresh > 1.0 && !cfg.pace_vrr) {
                const double period = 1e9 / refresh;
                const double next = std::ceil(double(present_now) / period) * period;
                pose_shown_ns = std::min(std::max(pose_shown_ns + period, next), double(present_now) + 2.0 * period);
                pose_shown_lead += (pose_shown_ns - double(present_now) - pose_shown_lead) * 0.02;
                shown = pose_shown_ns - pose_shown_lead;
            }
            static const double delay_intervals = [] { // M2_POSE_DELAY (experiment): intervals before the lag
                const char *v = std::getenv("M2_POSE_DELAY");
                return v && *v ? std::clamp(std::atof(v), 0.0, 2.0) : 1.25;
            }();
            const double delay = delay_intervals * double(pose_interval_ns) + std::min(pose_lag_mean, double(pose_interval_ns));
            const double target = shown - delay;
            pose_sample_ns = std::max(pose_sample_ns, target); // never back
            ++pose_presentations;
            pose_polys = game->board().present_pose_scene(uint64_t(std::max(pose_sample_ns, 0.0)), 0);
            if (!pose_polys) ++pose_fallbacks; // the game's own latest tick instead
        }
        const bool hardware_rasterized = hardware && (new_frame || pose_polys || last_render_was_pose);
#else
        const bool hardware_rasterized = hardware && new_frame;
#endif
        if (hardware_rasterized) {
            screen_w = game->screen_width();
            const int scale = cfg.uses_native_resolution() ? 1 : std::clamp(cfg.supersampling, 1, 4);
            if (scale > 1 && scale != hires_scale) {
                if (hires) SDL_ReleaseGPUTexture(dev, hires);
                SDL_GPUTextureCreateInfo hi = ti;
                hi.width = Uint32(kMaxW * scale);
                hi.height = Uint32(H * scale);
                hires_levels = scale >= 4 ? 3 : 2;
                hi.num_levels = Uint32(hires_levels);
                hires = SDL_CreateGPUTexture(dev, &hi);
                hires_scale = hires ? scale : 1;
                if (!hires) std::fprintf(stderr, "daytona: %dx super sampling unavailable: %s\n", scale, SDL_GetError());
            }
#ifndef M2_MOBILE
            if (native_resolution && swap) {
                // Round the logical viewport to fit, allowing the even-pixel
                // FOV rounding. Preserve proportions outside the supported range.
                Uint32 w = sw, h = sh;
                if (std::abs(double(sw) / sh - double(screen_w) / H) > 1.0 / H) {
                    const double fit = std::min(double(sw) / screen_w, double(sh) / H);
                    w = std::max(1u, Uint32(std::lround(screen_w * fit)));
                    h = std::max(1u, Uint32(std::lround(H * fit)));
                }
                // Mip 1 averages four samples per output pixel. Leave the
                // exact arcade raster alone and bound the intermediate size.
                const bool supersample = cfg.supersampling == app::Config::kNativeResolution &&
                    (w != Uint32(screen_w) || h != H) && w <= 4096 && h <= 4096;
                const Uint32 aa = supersample ? 2u : 1u;
                if (!raster_target || w != raster_width || h != raster_height || aa != raster_scale) {
                    auto info = ti;
                    info.width = w * aa;
                    info.height = h * aa;
                    info.num_levels = aa;
                    auto *replacement = SDL_CreateGPUTexture(dev, &info);
                    if (!replacement) {
                        SDL_SubmitGPUCommandBuffer(cmd);
                        return fail("native resolution texture");
                    }
                    if (raster_target) SDL_ReleaseGPUTexture(dev, raster_target);
                    raster_target = replacement;
                    raster_width = w;
                    raster_height = h;
                    raster_scale = aa;
                    std::printf("daytona: native raster %ux%u, output %ux%u, drawable %ux%u\n",
                                info.width, info.height, w, h, sw, sh);
                }
                // With shadows the frame goes in its own command buffer:
                // the DXR pass must run after it and before what reads it.
                SDL_GPUCommandBuffer *frame_cmd = dxr_shadows ? SDL_AcquireGPUCommandBuffer(dev) : nullptr;
                gpu.set_shadow_casters(&game->board().shadow_casters(pose_polys != nullptr)); // behind the camera
                if (!gpu.render(frame_cmd ? frame_cmd : cmd, raster_target, screen_w, H, game->board().video(),
                                1, int(w * aa), int(h * aa), aa > 1, pose_polys)) {
                    if (frame_cmd) SDL_SubmitGPUCommandBuffer(frame_cmd);
                    SDL_SubmitGPUCommandBuffer(cmd);
                    return fail("native resolution render");
                }
                if (frame_cmd) {
                    SDL_SubmitGPUCommandBuffer(frame_cmd);
                    const auto *frame = gpu.shadow_frame();
                    bool lit = false;
                    if (frame) {
                        app::DxrScene::Shade shade;
                        shade.view_z = frame->view_z;
                        shade.light = frame->light;
                        shade.width = frame->width;
                        shade.height = frame->height;
                        // At large 2x targets, the shadow queries still cover
                        // every two displayed pixels in each direction.
                        static const bool full_dxr_rate = [] {
                            const char *v = std::getenv("M2_DXR_FULL_RATE");
                            return v && v[0] == '1';
                        }();
                        shade.large_supersampled = !full_dxr_rate && raster_scale == 2 &&
                                                  uint64_t(frame->width) * frame->height >= 5120ull * 2160;
                        std::copy(std::begin(frame->to_screen), std::end(frame->to_screen), shade.to_screen);
                        std::copy(std::begin(frame->origin), std::end(frame->origin), shade.origin);
                        std::copy(std::begin(frame->origin2), std::end(frame->origin2), shade.origin2);
                        std::copy(std::begin(frame->focus2), std::end(frame->focus2), shade.focus2);
                        shade.cutout_xyz = frame->cutout_xyz;
                        shade.cutout_records = frame->cutout_records;
                        shade.tex0 = frame->tex0;
                        shade.tex1 = frame->tex1;
                        shade.tex_generation = frame->tex_generation;
                        // towards the sun in the polygons' scaled camera space:
                        // the game's light vector is where the light goes (it
                        // points down, y -0.94 on Beginner), so the opposite
                        auto light = dxr_sun_fixed ? dxr_sun_override : game->board().light();
                        if (dxr_sun_height > 0) {
                            // the game's direction, another height: keep the horizontal
                            // part, set the angle above it. Up is taken as the camera's y
                            // (a level camera; banked turns tilt it a little).
                            const float h = std::hypot(light[0], light[2]);
                            if (h > 1e-6f) {
                                const float e = float(dxr_sun_height) * 3.14159265f / 180.0f;
                                light = {light[0] / h * std::cos(e), -std::sin(e), light[2] / h * std::cos(e)};
                            }
                        }
                        const float sun[3] = {-light[0] * frame->focus[0], -light[1] * frame->focus[1], -light[2]};
                        const float length = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
                        if (length > 0) {
                            for (int k = 0; k < 3; ++k) shade.sun[k] = sun[k] / length;
                            // what the pass works with, whenever it changes (40 lines at most)
                            static std::array<float, 7> told{};
                            static int told_lines = 0;
                            const std::array<float, 7> now{light[0], light[1], light[2], frame->focus[0],
                                                           frame->focus[1], frame->origin[0], frame->origin[1]};
                            if (const char *path = std::getenv("M2_DXR_SCENE_REPORT");
                                now != told && told_lines < 40 && path && *path)
                                if (std::FILE *f = std::fopen(path, "a")) {
                                    std::fprintf(f, "daytona: DXR frame %llu light %.4f %.4f %.4f focus %.2f %.2f"
                                                 " origin %.1f %.1f to_screen %.4f %.4f size %dx%d\n",
                                                 (unsigned long long)game->board().frame(), light[0], light[1], light[2],
                                                 frame->focus[0], frame->focus[1], frame->origin[0], frame->origin[1],
                                                 frame->to_screen[0], frame->to_screen[1], frame->width, frame->height);
                                    std::fclose(f);
                                    told = now;
                                    ++told_lines;
                                }
                            // diagnostic: M2_DXR_CAMERAS=FILE lists the frame's cameras every 30 frames
                            if (const char *path = std::getenv("M2_DXR_CAMERAS");
                                path && *path && game->board().frame() % 30 == 0)
                                if (std::FILE *f = std::fopen(path, "a")) {
                                    const auto &cams = gpu.shadow_cameras();
                                    for (size_t c = 0; c < cams.size(); ++c)
                                        std::fprintf(f, "%llu,%s,%.1f,%.1f,%.2f,%.2f,%.0f,%u\n",
                                                     (unsigned long long)game->board().frame(),
                                                     c == gpu.shadow_main_camera() ? "main" : "other",
                                                     cams[c].first[0], cams[c].first[1], cams[c].first[2],
                                                     cams[c].first[3], cams[c].first[4], cams[c].second);
                                    std::fclose(f);
                                }
                            shade.strength = dxr_strength;
                            shade.sky = dxr_sky;
                            shade.sky_radius = dxr_sky_radius;
                            shade.sun_radius = dxr_sun_size * 0.5f * 3.14159265f / 180.0f;
                            shade.shaft = dxr_shafts;
                            shade.shaft_dist = dxr_shaft_dist;
                            if (shade.shaft > 0.0f) {
                                const double steps = game->board().shaft_route(pose_polys != nullptr) * 12.0 / shade.shaft_dist;
                                shade.shaft_phase = float(steps - std::floor(steps));
                            }
                            shade.focus[0] = frame->focus[0];
                            shade.focus[1] = frame->focus[1];
                            lit = dxr_scene.build(dxr_triangles, &shade);
                        }
                    }
                    if (!frame) dxr_scene.build(dxr_triangles);
                    if (lit) gpu.apply_shadow(cmd, raster_target);
                }
                if (aa > 1) SDL_GenerateMipmapsForGPUTexture(cmd, raster_target);
                shown = raster_target;
                shown_scale = 1;
            } else
#endif
            if (scale > 1 && hires) {
                gpu.render(cmd, hires, screen_w, H, game->board().video(), scale, 0, 0, false, pose_polys);
                SDL_GenerateMipmapsForGPUTexture(cmd, hires);
                shown = hires, shown_scale = scale;
            } else {
                const bool rendered = gpu.render(cmd, screen, screen_w, H, game->board().video(), 1, 0, 0, false, pose_polys);
#ifndef M2_MOBILE
                if (!rendered && raster_capture) {
                    SDL_SubmitGPUCommandBuffer(cmd);
                    return fail("GPU raster");
                }
#else
                (void)rendered;
#endif
                shown = screen, shown_scale = 1;
            }
            if (dxr_scene.ok() && !dxr_shadows) dxr_scene.build(dxr_triangles);
            last_render_was_pose = pose_polys != nullptr;
            new_frame = false;
        }
        if (new_frame) {
            void *p = SDL_MapGPUTransferBuffer(dev, upload, true);
            screen_w = game->screen_width();
            std::memcpy(p, game->screen().data(), size_t(screen_w) * H * 4);
#ifndef M2_MOBILE
            if (present_trace) software_digest = game->board().video().screen_hash();
#endif
            SDL_UnmapGPUTransferBuffer(dev, upload);
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = upload;
            src.pixels_per_row = Uint32(screen_w);
            SDL_GPUTextureRegion dst{};
            dst.texture = screen;
            dst.w = Uint32(screen_w);
            dst.h = H;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, true);
            SDL_EndGPUCopyPass(copy);
            shown = screen, shown_scale = 1;
            new_frame = false;
        }

        ImDrawData *draw = nullptr;
#ifndef M2_MOBILE
        if (pc_home && solo && native_fault) {
            home_message = launcher.error();
            cancel_start();
        }
        const bool draw_home = pc_home && !settings && (in_launcher || solo);
        const float reveal = reveal_at ? std::clamp(float(now - reveal_at) / 240'000'000.f, 0.f, 1.f) : 1.f;
#endif
        if (in_launcher
#ifdef M2_MOBILE
            || game != nullptr
#else
            || draw_home || reveal < 1.f
#endif
        ) {
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
#ifndef M2_MOBILE
            if (capture_image) {
                ImGui::GetIO().DisplaySize = {float(window_width), float(window_height)};
                ImGui::GetIO().DisplayFramebufferScale = {1.f, 1.f};
            }
#endif
            ImGui::NewFrame();
#ifndef M2_MOBILE
            if (draw_home) {
                switch (home_menu.draw(pc_race, solo, home_message.c_str(), 1.f)) {
                case app::HomeMenu::Play: begin_solo(home_menu.request()); break;
                case app::HomeMenu::Resume: in_launcher = false; break;
                case app::HomeMenu::Settings: settings = true; break;
                case app::HomeMenu::Quit: running = false; break;
                case app::HomeMenu::Cancel: cancel_start(); break;
                default: break;
                }
            } else
#endif
            if (in_launcher) switch (launcher.draw(game != nullptr, devices
#ifndef M2_MOBILE
                , pc_home
#endif
                )) {
            case app::Launcher::StartGame:
            case app::Launcher::Reset:
                if (start_game()) in_launcher = false, have_frame = false;
                break;
            case app::Launcher::Resume: in_launcher = false; break;
            case app::Launcher::Quit: running = false; break;
#ifndef M2_MOBILE
            case app::Launcher::Back: settings = false; break;
#endif
            default: break;
            }
#ifdef M2_MOBILE
            else app::draw_touch_controls(touch);
#else
            else if (reveal < 1.f) {
                const auto *view = ImGui::GetMainViewport();
                ImGui::GetForegroundDrawList()->AddRectFilled(view->Pos,
                    {view->Pos.x + view->Size.x, view->Pos.y + view->Size.y},
                    IM_COL32(0, 0, 0, int(255 * (1 - reveal))));
            }
#endif
            sync_native_audio();
            ImGui::Render();
            draw = ImGui::GetDrawData();
            ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
        }

#ifndef M2_MOBILE
        if (capture_image) {
            swap = capture_image;
            sw = Uint32(window_width);
            sh = Uint32(window_height);
        } else if (!native_resolution)
#else
        SDL_GPUTexture *swap = nullptr;
        Uint32 sw = 0, sh = 0;
#endif
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh)) return fail("swapchain");
        if (swap) {
            SDL_GPUTexture *target = swap;
#ifndef M2_MOBILE
            if (hdr_active) {
                target = hdr_present.scene(sw, sh);
                if (!target) {
                    SDL_Log("HDR10 intermediate allocation failed: %s", SDL_GetError());
                    SDL_GPUColorTargetInfo clear{};
                    clear.texture = swap;
                    clear.load_op = SDL_GPU_LOADOP_CLEAR;
                    clear.store_op = SDL_GPU_STOREOP_STORE;
                    clear.clear_color = SDL_FColor{0, 0, 0, 1};
                    if (auto *pass = SDL_BeginGPURenderPass(cmd, &clear, 1, nullptr))
                        SDL_EndGPURenderPass(pass);
                    SDL_SubmitGPUCommandBuffer(cmd);
                    capture_status = 1;
                    break;
                }
            } else if (sdr_curve_ready) {
                if (SDL_GPUTexture *scene = sdr_curve.scene(sw, sh)) target = scene;
            }
#endif
            bool painted = false;
#ifndef M2_MOBILE
            if (draw_home && home_3d)
                painted = home_scene.render(cmd, target, sw, sh, capture_image ? 3.0 : double(now) / 1e9, 1.f);
            if (capture_image && !painted) capture_status = 1;
            if (have_frame && !draw_home)
#else
            if (have_frame)
#endif
            { // the game's screen at its own shape, letterboxed
                const double scale = std::min(double(sw) / screen_w, double(sh) / H);
                const Uint32 dw = Uint32(screen_w * scale), dh = Uint32(H * scale);
                // a higher resolution frame: from its smallest level still at least the window's size
                Uint32 level = 0;
                while (shown == hires && int(level) + 1 < hires_levels && Uint32((H * shown_scale) >> (level + 1)) >= dh)
                    ++level;
                SDL_GPUBlitInfo blit{};
                blit.source.texture = shown;
                blit.source.mip_level = level;
                blit.source.w = Uint32(screen_w * shown_scale) >> level;
                blit.source.h = Uint32(H * shown_scale) >> level;
                blit.destination.texture = target;
                blit.destination.x = (sw - dw) / 2;
                blit.destination.y = (sh - dh) / 2;
                blit.destination.w = dw;
                blit.destination.h = dh;
#ifndef M2_MOBILE
                if (raster_target && shown == raster_target) {
                    blit.source.mip_level = raster_scale > 1 ? 1u : 0u;
                    blit.source.w = raster_width;
                    blit.source.h = raster_height;
                    const double fit = std::min(double(sw) / raster_width, double(sh) / raster_height);
                    blit.destination.w = std::min(sw, Uint32(std::lround(raster_width * fit)));
                    blit.destination.h = std::min(sh, Uint32(std::lround(raster_height * fit)));
                    blit.destination.x = (sw - blit.destination.w) / 2;
                    blit.destination.y = (sh - blit.destination.h) / 2;
                }
#endif
                if (cfg.arcade_43()) {
                    // Present the original raster on a 4:3 canvas without changing
                    // its logical resolution or the view used to render the scene.
                    constexpr double arcade_aspect = 4.0 / 3.0;
                    blit.destination.w = Uint32(std::lround(std::min(double(sw), sh * arcade_aspect)));
                    blit.destination.h = Uint32(std::lround(std::min(double(sh), sw / arcade_aspect)));
                    blit.destination.x = (sw - blit.destination.w) / 2;
                    blit.destination.y = (sh - blit.destination.h) / 2;
                }
#ifndef M2_MOBILE
                if (pc_race && hardware && !raster_reported && raster_capture) {
                    std::printf("daytona: race source %ux%u, swapchain %ux%u, destination %ux%u at %u,%u\n",
                                blit.source.w, blit.source.h, sw, sh, blit.destination.w,
                                blit.destination.h, blit.destination.x, blit.destination.y);
                    raster_reported = true;
                }
#endif
                blit.load_op = SDL_GPU_LOADOP_CLEAR;
                blit.clear_color = SDL_FColor{0, 0, 0, 1};
                blit.filter = SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(cmd, &blit);
                painted = true;
            }
            if (draw || !painted) { // the launcher on top (or a clear screen)
                SDL_GPUColorTargetInfo ct{};
                ct.texture = target;
                ct.load_op = painted ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
                ct.store_op = SDL_GPU_STOREOP_STORE;
                ct.clear_color = SDL_FColor{0.05f, 0.05f, 0.08f, 1};
                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
                if (draw) ImGui_ImplSDLGPU3_RenderDrawData(draw, cmd, pass);
                SDL_EndGPURenderPass(pass);
            }
#ifndef M2_MOBILE
            if (mire) { // the colour test pattern over everything, then the same display passes
                SDL_GPUBlitInfo blit{};
                blit.source.texture = mire;
                blit.source.w = mire_w;
                blit.source.h = mire_h;
                blit.destination.texture = target;
                blit.destination.w = sw;
                blit.destination.h = sh;
                blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                blit.filter = SDL_GPU_FILTER_NEAREST;
                SDL_BlitGPUTexture(cmd, &blit);
            }
            if (hdr_active && !hdr_present.compose(cmd, swap)) {
                SDL_Log("HDR10 composition failed: %s", SDL_GetError());
                SDL_SubmitGPUCommandBuffer(cmd);
                capture_status = 1;
                break;
            }
            if (!hdr_active && target != swap && !sdr_curve.compose(cmd, swap)) {
                SDL_Log("Display curve composition failed: %s", SDL_GetError());
                SDL_SubmitGPUCommandBuffer(cmd);
                capture_status = 1;
                break;
            }
#endif
        }
#ifndef M2_MOBILE
        if (raster_capture && !running && swap && hardware_rasterized && pc_race) {
            const bool native = raster_target && shown == raster_target;
            raster_captured = save_home_capture(dev, cmd, shown,
                SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                native ? raster_width : Uint32(screen_w * shown_scale),
                native ? raster_height : Uint32(H * shown_scale),
                raster_capture, native && raster_scale > 1 ? 1u : 0u);
            if (!raster_captured) SDL_Log("Race raster capture failed: %s", SDL_GetError());
            // Diagnostic: M2_CAPTURE_DISPLAYED=FILE.bmp also saves the window's
            // image after the SDR display curve (the frame above is submitted).
            const char *displayed = std::getenv("M2_CAPTURE_DISPLAYED");
            if (raster_captured && displayed && *displayed && !hdr_active && sdr_curve_ready) {
                const SDL_GPUTextureCreateInfo info{SDL_GPU_TEXTURETYPE_2D, app::Hdr10Present::scene_format,
                    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, sw, sh, 1, 1,
                    SDL_GPU_SAMPLECOUNT_1, 0};
                SDL_GPUCommandBuffer *again = SDL_AcquireGPUCommandBuffer(dev);
                SDL_GPUTexture *copy = again ? SDL_CreateGPUTexture(dev, &info) : nullptr;
                if (!copy || !sdr_curve.compose(again, copy) ||
                    !save_home_capture(dev, again, copy, app::Hdr10Present::scene_format, sw, sh, displayed, 0u))
                    SDL_Log("Displayed capture failed: %s", SDL_GetError());
                if (copy) SDL_ReleaseGPUTexture(dev, copy);
            }
        } else if (capture_image) {
            if (!save_home_capture(dev, cmd, capture_image, ii.ColorTargetFormat, sw, sh, capture_home)) {
                SDL_Log("Home capture failed: %s", SDL_GetError());
                capture_status = 1;
            }
            running = false;
        } else if (hardware_trace) {
            const bool eligible = swap && pc_race && !in_launcher && have_frame &&
                                  !draw_home && !draw && hardware && shown_scale == 1 && reveal >= 1.f;
            if (!hardware_trace->submit(cmd, shown, eligible, hardware_rasterized, sw, sh, Uint32(screen_w))) {
                capture_status = 1;
                running = false;
            } else if (hardware_trace->ended() || (!solo && !pc_race)) {
                running = false;
            }
        } else {
            // Diagnostic: M2_CAPTURE_EVERY=N with M2_CAPTURE_DIR=FOLDER saves one
            // presented frame in N (the raster, as --capture-raster) while the
            // game runs, attract mode included: a place the scripted race does
            // not reach (the Canyon tunnel, 23/11). It waits for the GPU.
            static const unsigned capture_every = [] {
                const char *v = std::getenv("M2_CAPTURE_EVERY");
                return v ? unsigned(std::max(std::atoi(v), 0)) : 0u;
            }();
            static const char *capture_dir = std::getenv("M2_CAPTURE_DIR");
            static const unsigned long long capture_from = [] { // M2_CAPTURE_FROM: first one counted
                const char *v = std::getenv("M2_CAPTURE_FROM");
                return v ? std::strtoull(v, nullptr, 10) : 0ull;
            }();
            static unsigned long long capture_count = 0;
            bool submitted;
            if (capture_every && capture_dir && *capture_dir && swap && shown && hardware_rasterized &&
                ++capture_count >= capture_from && capture_count % capture_every == 0) {
                const bool native = raster_target && shown == raster_target;
                char path[600];
                std::snprintf(path, sizeof path, "%s/f%07llu.bmp", capture_dir, capture_count);
                submitted = save_home_capture(dev, cmd, shown, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                    native ? raster_width : Uint32(screen_w * shown_scale),
                    native ? raster_height : Uint32(H * shown_scale), path, native && raster_scale > 1 ? 1u : 0u);
            } else {
                submitted = SDL_SubmitGPUCommandBuffer(cmd);
            }
            if (present_trace) {
                const bool eligible = swap && pc_race && !in_launcher && have_frame &&
                                      !draw_home && !draw && !hardware && reveal >= 1.f;
                if (!present_trace->sample(submitted, eligible, sw, sh, screen_w, software_digest)) {
                    std::fprintf(stderr, "daytona: software present trace interrupted\n");
                    capture_status = 1;
                    running = false;
                } else if (present_trace->ended() || (!solo && !pc_race)) {
                    running = false;
                }
            }
        }
#else
        SDL_SubmitGPUCommandBuffer(cmd);
#endif
    }

#ifndef M2_MOBILE
    if (present_trace && !present_trace->finish()) {
        std::fprintf(stderr, "daytona: incomplete software present trace\n");
        capture_status = 1;
    }
#endif
    save_nv();
#ifndef M2_MOBILE
    if (!capture_home)
#endif
        cfg.save();
    if (game) std::printf("daytona: %llu frames\n", (unsigned long long)game->frames());
    if (native_audio.available()) {
        const bool paused = native_audio.pause();
        const auto stats = native_audio.stats();
        native_fault |= !paused || stats.failed || stats.invalid;
        if (timed_audio_active) {
            if (!stats.callbacks || !stats.frames) {
                std::fprintf(stderr, "daytona: audio timing probe received no completed callback\n");
                native_fault = true;
            }
            std::printf("daytona: audio timing packets pending=%u retimed=%u\n",
                        stats.queued_packets, stats.late_packets);
        }
        std::printf("daytona: native audio callbacks=%u frames=%u queued=%u overflows=%u failed=%u invalid=%u unsupported=%u unsupported_modulation=%u\n",
            stats.callbacks, stats.frames, stats.queued, stats.overflows, stats.failed, stats.invalid, stats.unsupported,
            stats.unsupported_modulation);
    }
    native_audio.close();
#ifndef M2_MOBILE
    local_music.close();
#endif
    audio.close();
    SDL_WaitForGPUIdle(dev);
#ifndef M2_MOBILE
    if (hardware_trace) {
        if (!hardware_trace->finish()) {
            std::fprintf(stderr, "daytona: incomplete hardware present trace\n");
            capture_status = 1;
        }
        hardware_trace.reset();
    }
    home_scene.shutdown();
    hdr_present.reset();
    sdr_curve.reset(); // before the device goes
    if (mire) SDL_ReleaseGPUTexture(dev, mire);
    if (capture_image) SDL_ReleaseGPUTexture(dev, capture_image);
    if (raster_target) SDL_ReleaseGPUTexture(dev, raster_target);
    if (raster_capture && !raster_captured) capture_status = 1;
#endif
    dxr_scene.shutdown();
    gpu.shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseGPUTransferBuffer(dev, upload);
    SDL_ReleaseGPUTexture(dev, screen);
    if (hires) SDL_ReleaseGPUTexture(dev, hires);
    SDL_ReleaseWindowFromGPUDevice(dev, window);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(window);
    ffb.close();
    devices.close_all();
    SDL_Quit();
#ifndef M2_MOBILE
    if (player_input_capture) {
        const bool completed = !capture_status && start_solo_ok && !native_fault && game &&
            (game->frames() == max_frames || game->race_status() == rt::RaceStatus::Finished);
        if (!player_input_capture->close(completed, game ? game->frames() : 0)) {
            std::fprintf(stderr, "daytona: incomplete input trace\n");
            return 1;
        }
    }
    return capture_status || !start_solo_ok || (audio_timing_probe && native_fault) ? 1 : 0;
#else
    return 0;
#endif
}
