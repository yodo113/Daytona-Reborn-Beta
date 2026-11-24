#include "app/launcher.h"
#include "app/i18n.h"
#include "app/rom_file.h"
#ifdef SDL_PLATFORM_IOS
#include "rom_picker.h"
#endif
#include "runtime/game_loop.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace app {

namespace {

std::string mode_key(const SDL_DisplayMode &m) {
    char s[64];
    if (m.pixel_density == 1.0f) std::snprintf(s, sizeof s, "%dx%d@%.3f", m.w, m.h, double(m.refresh_rate));
    else std::snprintf(s, sizeof s, "%dx%d@%.3f*%.2f", m.w, m.h, double(m.refresh_rate), double(m.pixel_density));
    return s;
}

std::string mode_label(const SDL_DisplayMode &m) {
    char s[64];
    std::snprintf(s, sizeof s, "%d x %d, %.2f Hz%s", m.w, m.h, double(m.refresh_rate), m.pixel_density == 1.0f ? "" : " (HiDPI)");
    return s;
}

} // namespace

void apply_fullscreen_mode(SDL_Window *window, const std::string &key) {
    int w = 0, h = 0;
    float hz = 0, density = 1;
    SDL_DisplayMode mode;
    if (!key.empty() && std::sscanf(key.c_str(), "%dx%d@%f*%f", &w, &h, &hz, &density) >= 3 &&
        SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window), w, h, hz, density > 1, &mode))
        SDL_SetWindowFullscreenMode(window, &mode);
    else
        SDL_SetWindowFullscreenMode(window, nullptr); // borderless, at the desktop's mode
}

Launcher::Launcher(Config &cfg, SDL_Window *window) : cfg_(cfg), window_(window) {
    std::snprintf(link_next_buf_, sizeof link_next_buf_, "%s", cfg_.link_next.c_str());
#ifdef SDL_PLATFORM_ANDROID
    // Older mobile builds saved the picker result (content://...) directly.
    // Do not reopen such a URI during startup: Android's temporary document
    // grant may be stale, and the picker/JNI path is not part of launcher
    // construction. Ask the user to browse again and import it then.
    if (RomFile::is_content_uri(cfg_.rom_path)) {
        cfg_.rom_path.clear();
        path_buf_[0] = '\0';
        cfg_.save();
        rom_message_ = TX("Choisissez de nouveau le jeu de ROM avec Parcourir.");
        return;
    }
#endif
    std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
    check_rom();
}

void Launcher::check_rom() {
    checks_.clear();
    rom_ok_ = false;
    if (cfg_.rom_path.empty()) {
        rom_message_ = TXF("Choisissez le jeu de ROM %s (.zip ou .7z).", M2_ROMSET);
        return;
    }
    try {
        // Android's picker grants access to a content URI, not a raw /sdcard
        // path. Stage it through SDL's Android reader before the plain-file
        // archive code verifies it. Invalid imports never replace a good copy.
        RomFile selected(cfg_.rom_path);
        checks_ = rt::check_rom_set(selected.path());
        int good = 0;
        for (const auto &c : checks_) good += c.ok;
        const bool verified = !checks_.empty() && good == int(checks_.size());
        if (verified) {
            std::string path = selected.commit();
            // Keep ordinary paths absolute, including the saved Android copy.
            std::error_code ec;
            const auto abs = std::filesystem::absolute(path, ec);
            if (!ec && std::filesystem::exists(abs, ec)) path = abs.lexically_normal().string();
            if (cfg_.rom_path != path) {
                cfg_.rom_path = path;
                std::snprintf(path_buf_, sizeof path_buf_, "%s", path.c_str());
                cfg_.save();
            }
        }
        // Do not enable Start if committing the imported archive failed.
        rom_ok_ = verified;
        if (rom_ok_) rom_message_ = TXF("Les %d fichiers sont vérifiés.", good);
        else rom_message_ = TXF("%d fichiers sur %d absents ou incorrects : ce n'est pas le jeu %s.",
                               int(checks_.size()) - good, int(checks_.size()), M2_ROMSET);
    } catch (const std::exception &e) {
        rom_message_ = e.what();
    }
}

void SDLCALL Launcher::dialog_done(void *self, const char *const *files, int) {
    auto *l = static_cast<Launcher *>(self);
    std::lock_guard<std::mutex> g(l->dialog_mutex_);
    l->dialog_pending_ = false;
    if (files && files[0]) {
        l->dialog_result_ = files[0];
        l->error_.clear();
    } else if (!files) l->error_ = std::string(TX("Choix de la ROM : ")) + SDL_GetError();
}

void Launcher::browse() {
    static SDL_DialogFileFilter filters[2]; // kept until the dialog answers; in the current language
    filters[0] = {TX("Jeu de ROM (zip, 7z)"), "zip;7z"};
    filters[1] = {TX("Tous les fichiers"), "*"};
    dialog_pending_ = true;
#ifdef SDL_PLATFORM_IOS
    ios_browse_rom(window_, dialog_done, this);
#else
    SDL_ShowOpenFileDialog(dialog_done, this, window_, filters, 2, cfg_.rom_path.empty() ? nullptr : cfg_.rom_path.c_str(), false);
#endif
}

void Launcher::start_capture(int action, CaptureKind kind, const Devices &devices) {
    capture_action_ = action;
    capture_kind_ = kind;
    joy_tracking_ = false;
    joy_rest_.clear();
    if (kind == CaptureJoy)
        for (SDL_Joystick *j : devices.joys)
            for (int i = 0; i < SDL_GetNumJoystickAxes(j); i++) joy_rest_[{SDL_GetJoystickID(j), i}] = SDL_GetJoystickAxis(j, i);
}

bool Launcher::handle_event(const SDL_Event &e) {
    if (capture_action_ < 0) return false;
    Binding &b = cfg_.controls.bind[capture_action_];
    auto done = [&] {
        capture_action_ = -1;
        joy_tracking_ = false;
        cfg_.save();
    };
    auto guid_of = [](SDL_JoystickID id) {
        char g[64] = "";
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), g, sizeof g);
        return std::string(g);
    };
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
            capture_action_ = -1; // cancel
            joy_tracking_ = false;
        } else if (capture_kind_ == CaptureKey) {
            b.key = (e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE) ? SDL_SCANCODE_UNKNOWN
                                                                                                         : e.key.scancode;
            done();
        }
        return true;
    }
    if (capture_kind_ == CapturePad && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        b.pad.kind = PadInput::Button;
        b.pad.index = e.gbutton.button;
        done();
        return true;
    }
    if (capture_kind_ == CapturePad && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(e.gaxis.value)) > 20000) {
        b.pad.kind = PadInput::Axis;
        b.pad.index = e.gaxis.axis;
        b.pad.dir = e.gaxis.value > 0 ? 1 : -1;
        done();
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && !joy_tracking_) {
        b.joy = JoyInput{};
        b.joy.kind = JoyInput::Button;
        b.joy.guid = guid_of(e.jbutton.which);
        b.joy.index = e.jbutton.button;
        done();
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && !joy_tracking_) {
        const int v = e.jhat.value;
        if (v == SDL_HAT_UP || v == SDL_HAT_DOWN || v == SDL_HAT_LEFT || v == SDL_HAT_RIGHT) {
            b.joy = JoyInput{};
            b.joy.kind = JoyInput::Hat;
            b.joy.guid = guid_of(e.jhat.which);
            b.joy.index = e.jhat.hat;
            b.joy.mask = v;
            done();
        }
        return true;
    }
    if (capture_kind_ == CaptureJoy && e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
        // An axis binds once it has moved well away from where it rested and
        // come back: its rest value and the furthest it went are its range.
        const auto key = std::make_pair(e.jaxis.which, int(e.jaxis.axis));
        const auto it = joy_rest_.find(key);
        const int rest = it != joy_rest_.end() ? it->second : 0, v = e.jaxis.value;
        if (!joy_tracking_) {
            if (std::abs(v - rest) > 12000) {
                joy_tracking_ = true;
                joy_id_ = e.jaxis.which, joy_axis_ = e.jaxis.axis, joy_from_ = rest, joy_extreme_ = v;
            }
        } else if (e.jaxis.which == joy_id_ && e.jaxis.axis == joy_axis_) {
            if (std::abs(v - joy_from_) > std::abs(joy_extreme_ - joy_from_)) joy_extreme_ = v;
            if (std::abs(v - joy_from_) * 10 < std::abs(joy_extreme_ - joy_from_) * 3) { // let go: back within 30%
                b.joy = JoyInput{};
                b.joy.kind = JoyInput::Axis;
                b.joy.guid = guid_of(joy_id_);
                b.joy.index = joy_axis_;
                b.joy.rest = joy_from_;
                b.joy.full = joy_extreme_;
                done();
            }
        }
        return true;
    }
    return e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP || e.type == SDL_EVENT_JOYSTICK_BUTTON_UP;
}

Launcher::Result Launcher::draw(bool game_running, const Devices &devices, bool from_home) {
    SDL_Gamepad *pad = devices.pad;
    {
        std::lock_guard<std::mutex> g(dialog_mutex_);
        if (!dialog_result_.empty()) {
            cfg_.rom_path = dialog_result_;
            dialog_result_.clear();
            std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
            cfg_.save();
            check_rom();
        }
    }

    Result result = Stay;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
#ifdef SDL_PLATFORM_IOS
    SDL_Rect safe{};
    if (SDL_GetWindowSafeArea(window_, &safe) && safe.w > 0 && safe.h > 0) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + safe.x, vp->Pos.y + safe.y));
        ImGui::SetNextWindowSize(ImVec2(float(safe.w), float(safe.h)));
    }
#endif
    ImGui::SetNextWindowBgAlpha(game_running ? 0.85f : 1.0f);
    ImGui::Begin("Daytona USA", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushTextWrapPos();

    if (from_home) {
        if (ImGui::Button(TX("Retour à l'accueil"))) result = Back;
        ImGui::SameLine();
    }
    ImGui::TextUnformatted("DAYTONA USA");
    ImGui::SameLine();
    ImGui::TextDisabled(TX("  recompilation statique, moteur natif"));
    {   // the interface language, each in its own script (app/i18n.h)
        const auto &langs = i18n::languages();
        size_t sel = 0;
        for (size_t i = 0; i < langs.size(); ++i)
            if (i18n::current() == langs[i].code) sel = i;
        // only the languages with a translation next to the game (and French)
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("Langue / Language", langs[sel].name)) {
            for (size_t i = 0; i < langs.size(); ++i)
                if (i18n::available(langs[i].code) && ImGui::Selectable(langs[i].name, i == sel) && i != sel) {
                    cfg_.language = langs[i].code;
                    cfg_.save();
                    i18n::set(cfg_.language);
                    check_rom(); // its message, in the new language
                }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem(TX("Jeu"))) {
            ImGui::Spacing();
            ImGui::TextUnformatted(TX("Jeu de ROM"));
#ifdef SDL_PLATFORM_ANDROID
            ImGui::TextWrapped(TX("Parcourir donne accès à votre ZIP/7z. Une copie vérifiée est gardée dans l'application."));
#endif
            ImGui::SetNextItemWidth(-200);
            if (ImGui::InputText("##rom", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::SameLine();
            if (ImGui::Button(TX("Parcourir..."), ImVec2(100, 0)) && !dialog_pending_) browse();
            ImGui::SameLine();
            if (ImGui::Button(TX("Vérifier"), ImVec2(90, 0))) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::TextColored(rom_ok_ ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(1, 0.6f, 0.3f, 1), "%s", rom_message_.c_str());
            if (!error_.empty()) {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
            }
            if (!checks_.empty() && ImGui::TreeNode(TX("Fichiers"))) {
                if (ImGui::BeginTable("files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    for (const auto &c : checks_) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(c.file.c_str());
                        ImGui::TableNextColumn();
                        if (c.ok) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "ok");
                        else ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", c.problem.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted(TX("Affichage"));
            {   // the picture's format first, easy to find (viewers' request, Daytona-Reborn issues 1 and 2)
                const char *aspects[] = {TX("Arcade 4:3 (la borne d'origine)"), TX("Automatique"), "16:10", "16:9",
                                         TX("21:9 (ultra-large)")};
                static const char *aspect_ids[] = {"4:3", "", "16:10", "16:9", "21:9"};
                int aspect = 1;
                for (int i = 0; i < 5; i++)
                    if (cfg_.aspect == aspect_ids[i]) aspect = i;
                ImGui::SetNextItemWidth(260);
                if (ImGui::Combo(TX("Format de l'image"), &aspect, aspects, 5)) {
                    cfg_.aspect = aspect_ids[aspect];
                    cfg_.save();
                }
                ImGui::SetItemTooltip("%s", TX("Arcade 4:3 : le cadrage de la borne, centré, avec des bandes sur les côtés d'un écran large.\n"
                                               "Automatique : la forme de la fenêtre en rendu natif, sinon le 4:3 d'origine.\n"
                                               "16:10, 16:9, 21:9 : la scène s'élargit sur les côtés, la voiture garde ses proportions."));
            }
            const char *apis[] = {TX("Automatique"), "Vulkan", "Direct3D 12", "Metal"};
            static const char *api_ids[] = {"", "vulkan", "direct3d12", "metal"};
            int api = 0;
            for (int i = 0; i < 4; i++)
                if (cfg_.gpu == api_ids[i]) api = i;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(TX("API graphique (redémarrer le jeu)"), &api, apis, 4)) {
                cfg_.gpu = api_ids[api];
                cfg_.save();
            }
            const char *renderers[] = {TX("Logiciel (exact)"), TX("Matériel (expérimental)")};
            int rd = cfg_.renderer == "hardware" ? 1 : 0;
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(TX("Rendu"), &rd, renderers, 2)) {
                cfg_.renderer = rd ? "hardware" : "software";
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Logiciel : la 3D est calculée par le processeur, comme sur la carte d'arcade. Matériel :\n"
                                "par la carte graphique ; en développement, très proche mais pas encore exact au pixel près."));
            if (ImGui::Checkbox(TX("Plein écran"), &cfg_.fullscreen)) {
                SDL_SetWindowFullscreen(window_, cfg_.fullscreen);
                cfg_.save();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Cacher la souris pendant le jeu"), &cfg_.hide_cursor)) cfg_.save();
            ImGui::SetItemTooltip(TX("Pas de pointeur de souris pendant la partie. Il revient quand ce lanceur\n"
                                  "s'ouvre (Échap) et disparaît de nouveau avec Reprendre."));
            {   // exclusive fullscreen: the display's own modes, a custom 57.52 Hz one among them
                int count = 0;
                SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(window_), &count);
                std::string preview = TX("Bureau (sans bordure)");
                for (int i = 0; i < count; ++i)
                    if (mode_key(*modes[i]) == cfg_.fullscreen_mode) preview = mode_label(*modes[i]);
                auto choose = [&](const std::string &key) {
                    cfg_.fullscreen_mode = key;
                    apply_fullscreen_mode(window_, key);
                    cfg_.save();
                };
                ImGui::SetNextItemWidth(200);
                if (ImGui::BeginCombo(TX("Mode plein écran"), preview.c_str())) {
                    if (ImGui::Selectable(TX("Bureau (sans bordure)"), cfg_.fullscreen_mode.empty())) choose("");
                    for (int i = 0; i < count; ++i) {
                        const std::string key = mode_key(*modes[i]);
                        ImGui::PushID(i);
                        if (ImGui::Selectable(mode_label(*modes[i]).c_str(), key == cfg_.fullscreen_mode)) choose(key);
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip(TX("Bureau : plein écran à la résolution et à la fréquence du bureau.\n"
                                      "Un mode : plein écran exclusif à cette résolution et cette fréquence, par exemple\n"
                                      "un mode à 57,52 Hz créé dans les réglages du pilote graphique."));
                SDL_free(modes);
            }
            ImGui::TextUnformatted(TX("Cadence d'affichage"));
            ImGui::SameLine();
            ImGui::TextDisabled(TX("(tout décoché : la vitesse de la borne sur tout écran)"));
            if (ImGui::Checkbox(TX("Cadence fluide sur un écran à 57,52 Hz"), &cfg_.pace_smooth)) cfg_.save();
            ImGui::SetItemTooltip(TX("Sur un écran réglé à 57,52 Hz (ou 115,05 Hz), une image du jeu par rafraîchissement (ou deux) :\n"
                                  "aucune image doublée ou sautée. La vitesse reste celle de la borne, à 1 %% près.\n"
                                  "Sans effet aux autres fréquences."));
            if (ImGui::Checkbox(TX("Caler sur l'écran (change la vitesse du jeu)"), &cfg_.pace_sync_display)) cfg_.save();
            ImGui::SetItemTooltip(TX("Fait tourner le jeu à une cadence qui divise exactement celle de l'écran : chaque image\n"
                                  "reste affichée le même temps et le mouvement est parfaitement fluide. Sur un écran à\n"
                                  "60, 120, 180 ou 240 Hz, le jeu tourne à 60 images/s, environ 4 %% plus vite que la borne\n"
                                  "(57,52) ; avec le son de référence, le son est aussi un peu plus rapide. Sans effet\n"
                                  "sur un écran à 144 ou 165 Hz, qui ne peut pas caler une cadence proche de la borne :\n"
                                  "utilisez alors la cadence VRR si l'écran gère G-Sync ou FreeSync.\n"
                                  "Décoché : le jeu tourne toujours à la vitesse de la borne."));
            if (ImGui::Checkbox(TX("Cadence VRR (G-Sync / FreeSync)"), &cfg_.pace_vrr)) cfg_.save();
            ImGui::SetItemTooltip(TX("Pour un écran à fréquence variable : chaque image dure exactement 1/57,52 s, l'écran\n"
                                  "se rafraîchit au rythme du jeu et chaque image reste affichée le même temps.\n"
                                  "Le VRR doit être actif dans l'écran et le pilote graphique ; sur un écran à\n"
                                  "fréquence fixe, le résultat ressemble au réglage par défaut."));
            ImGui::TextDisabled("%s", TXF("Actuellement : %s.", pacing_status_.c_str()).c_str());
            if (ImGui::Checkbox(TX("Passer le lanceur"), &cfg_.skip_launcher)) cfg_.save();
            ImGui::SameLine();
            ImGui::TextDisabled(TX("(lance le jeu tout de suite ; Échap ouvre ce lanceur)"));
            const char *draw_modes[] = {TX("Double tampon"), TX("Simple tampon"), TX("Une image sur trois")};
            ImGui::SetNextItemWidth(200);
            int dm = std::clamp(cfg_.draw_mode, 0, 2);
            if (ImGui::Combo(TX("Mode de dessin"), &dm, draw_modes, 3)) {
                cfg_.draw_mode = dm;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Double tampon : chaque image est dessinée, comme dans le jeu. Simple tampon : une sur deux ;\n"
                                "une image sur trois : une sur trois. Plus rapide ; le jeu lui-même n'est pas ralenti."));
            const char *supersampling[] = {TX("Natif 2x"), TX("Désactivé"), "2x", "3x", "4x", TX("Natif direct")};
            ImGui::BeginDisabled(cfg_.renderer != "hardware");
            ImGui::SetNextItemWidth(200);
            int ss = std::clamp(cfg_.supersampling, 0, app::Config::kNativeDirect);
            if (ImGui::Combo(TX("Résolution du rendu"), &ss, supersampling, IM_ARRAYSIZE(supersampling))) {
                cfg_.supersampling = ss;
                cfg_.save();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled(TX("Les modes natifs suivent la fenêtre sous Direct3D 12.\n"
                                "2x à 4x agrandissent la résolution d'origine."));

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted(TX("Améliorations"));
            ImGui::BeginDisabled(cfg_.arcade_43());
            if (ImGui::Checkbox(TX("Compteurs au bord de l'écran (expérimental)"), &cfg_.hud_edges)) cfg_.save();
            if (ImGui::Checkbox(TX("Étirer le fond du ciel (expérimental)"), &cfg_.stretch_backdrop)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::TextDisabled(TX("Montre davantage de la scène sur les côtés. Les compteurs restent en 4:3 au centre, ou bien\n"
                                "les temps au tour, la position et les cartes passent sur les bords. En course, le ciel\n"
                                "des côtés est bleu uni, ou bien l'image du ciel du jeu étirée sur tout l'écran."));
            const char *distances[] = {TX("La plus courte"), TX("Plus courte"), TX("Par défaut"), TX("Plus loin"), TX("Le plus loin")};
            ImGui::SetNextItemWidth(200);
            int dd = std::clamp(cfg_.draw_distance, -2, 2);
            if (ImGui::SliderInt(TX("Distance d'affichage"), &dd, -2, 2, distances[dd + 2], ImGuiSliderFlags_AlwaysClamp)) {
                cfg_.draw_distance = dd;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Le décor autour du circuit. L'écran large ajoute aussi le décor sur les côtés.\n"
                                "Plus court réduit la portée ; plus loin ajoute arbres et bâtiments, pas la route."));
            if (ImGui::Checkbox(TX("Décor en volume"), &cfg_.scenery_3d)) cfg_.save();
            ImGui::TextDisabled(TX("Sapins en volume, tribunes pleines, piliers sous le pont de Dinosaur Canyon.\n"
                                "Pris en compte au prochain lancement du jeu."));
            if (ImGui::Checkbox(TX("Voitures adverses toujours détaillées"), &cfg_.cars_near_model)) cfg_.save();
            ImGui::TextDisabled(TX("Un seul modèle, le plus détaillé, à toutes les distances : plus de voitures\n"
                                "qui changent d'aspect en approchant. Pris en compte au prochain lancement du jeu."));
            ImGui::BeginDisabled(cfg_.renderer != "hardware" || cfg_.draw_mode != 0);
#ifdef M2_RAY_TRACING
            if (ImGui::Checkbox(TX("Ombres du soleil (ray tracing, expérimental)"), &cfg_.sun_shadows)) cfg_.save();
            ImGui::TextDisabled(TX("Ombres projetées par le soleil du jeu (Direct3D 12, carte compatible ray tracing,\n"
                                "résolution native). Pris en compte au prochain lancement du jeu."));
            if (cfg_.sun_shadows) {
                if (ImGui::Checkbox(TX("Lumière du ciel (expérimental)"), &cfg_.sky_light)) cfg_.save();
                ImGui::TextDisabled(TX("Le ciel éclaire de partout : plus sombre sous les voitures, sous le portique,\n"
                                    "au pied des murs. Pris en compte au prochain lancement du jeu."));
                ImGui::SetNextItemWidth(200);
                int tens = cfg_.sun_height / 10;
                const std::string sun_label = tens ? TXF("%d degr\xc3\xa9s", tens * 10) : TX("celle du jeu");
                if (ImGui::SliderInt(TX("Hauteur du soleil"), &tens, 0, 8, sun_label.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                    cfg_.sun_height = tens * 10;
                    cfg_.save();
                }
                ImGui::TextDisabled(TX("La direction reste celle du soleil du jeu. Plus bas, les ombres s'allongent\n"
                                    "(comme en fin d'apr\xc3\xa8s-midi). Pris en compte au prochain lancement du jeu."));
            }
#endif
            if (ImGui::Checkbox(TX("Lissage 120 images/s (expérimental)"), &cfg_.interpolate)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::TextDisabled(TX("Images intermédiaires entre celles du jeu. Demande le rendu Matériel et le double\n"
                                "tampon.\n"
                                "Pris en compte au prochain lancement du jeu."));
            ImGui::SetNextItemWidth(200);
            int crt = cfg_.display_gamma >= 2.0f ? int(cfg_.display_gamma * 10.0f + 0.5f) : 20;
            const std::string crt_label = crt <= 20 ? TX("PC (sRGB)") : TXF("Borne %d,%d", crt / 10, crt % 10);
            if (ImGui::SliderInt(TX("Couleurs de l'\xc3\xa9" "cran (red\xc3\xa9marrer le jeu)"), &crt, 20, 26, crt_label.c_str(),
                                 ImGuiSliderFlags_AlwaysClamp)) {
                cfg_.display_gamma = crt <= 20 ? 0.0f : float(crt) / 10.0f;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Borne 2,2 \xc3\xa0 2,6 : les couleurs du jeu comme sur l'\xc3\xa9" "cran \xc3\xa0 tube de la borne,\n"
                                "plus profondes et moins d\xc3\xa9lav\xc3\xa9" "es. Conseill\xc3\xa9 : 2,4 (r\xc3\xa9glage par d\xc3\xa9" "faut).\n"
                                "PC : les m\xc3\xaa" "mes couleurs avec la courbe d'un \xc3\xa9" "cran de PC, plus claires."));
            ImGui::SetNextItemWidth(200);
            int saturation = int(cfg_.display_saturation * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Saturation des couleurs (redémarrer le jeu)"), &saturation, 50, 150,
                                 saturation == 100 ? TX("100 %% (origine)") : "%d %%", ImGuiSliderFlags_AlwaysClamp)) {
                cfg_.display_saturation = float(saturation) / 100.0f;
                cfg_.save();
            }
            ImGui::TextDisabled("%s", TXF("Plus de 100 %% : des couleurs plus vives. 100 %% : les couleurs du jeu telles quelles.").c_str());
            const int scenery_width = 496 + 2 * rt::GameLoop::wide_margin(cfg_.arcade_43() ? 0.0 : cfg_.aspect_ratio());
            const uint32_t automatic_budget = rt::automatic_scenery_budget(scenery_width, cfg_.draw_distance);
            int budget_mode = cfg_.draw_budget ? 1 : 0;
            const char *budget_modes[] = {TX("Automatique"), TX("Personnalisé")};
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(TX("Budget de polygones"), &budget_mode, budget_modes, 2)) {
                cfg_.draw_budget = budget_mode ? automatic_budget : 0;
                cfg_.save();
            }
            if (cfg_.draw_budget) {
                int value = int(cfg_.draw_budget);
                ImGui::SetNextItemWidth(200);
                if (ImGui::InputInt(TX("Allocation personnalisée"), &value, 500, 5000)) {
                    cfg_.draw_budget = uint32_t(std::clamp(value, 1, int(rt::kMaxSceneryBudget)));
                    cfg_.save();
                }
                ImGui::TextDisabled(TX("Remplace l'allocation automatique. De 1 à 1 000 000.\n"
                                    "Trop bas, du décor peut manquer ; trop haut, le jeu peut ralentir."));
            } else {
                ImGui::TextDisabled("%s", TXF("Allocation actuelle : %u. Elle suit l'écran large et la distance d'affichage.\n"
                                          "La résolution et le suréchantillonnage ne la changent pas.", automatic_budget).c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted(TX("Jeu en réseau (expérimental, réinitialiser)"));
            if (ImGui::Checkbox(TX("Relier à d'autres bornes"), &cfg_.link)) cfg_.save();
            ImGui::BeginDisabled(!cfg_.link);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputInt(TX("Port de cette borne"), &cfg_.link_port, 0, 0)) {
                cfg_.link_port = std::clamp(cfg_.link_port, 1, 65535);
                cfg_.save();
            }
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText(TX("Borne suivante (hôte:port)"), link_next_buf_, sizeof link_next_buf_)) {
                cfg_.link_next = link_next_buf_;
                cfg_.save();
            }
            if (ImGui::Checkbox(TX("Synchroniser les images (chaque borne attend la principale)"), &cfg_.link_framesync)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640);
            ImGui::TextWrapped("%s", TXF("Réseau : %s", link_status_.c_str()).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::TextDisabled(TX("Les bornes se relient en anneau par le réseau (Wi-Fi ou câble) : chacune écoute sur son\n"
                                "port et se connecte à la suivante ; à deux, chacune a l'autre pour suivante. Tous les\n"
                                "ordinateurs peuvent prendre le même port, par exemple 15112, et Borne suivante =\n"
                                "adresse de l'autre ordinateur:15112. En mode test (F2) > GAME SYSTEM, régler LINK ID\n"
                                "(une MASTER, les autres SLAVE) et un CAR NUMBER différent sur chacune. Le jeu de 1994\n"
                                "(révision A) permet le jeu en réseau."));

            ImGui::Spacing();
            ImGui::Separator();
            if (!error_.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                ImGui::TextWrapped("%s", error_.c_str());
                ImGui::PopStyleColor();
            }
            if (!from_home) {
                ImGui::Checkbox(TX("Maintenir le bouton Test"), &cfg_.hold_test); // not saved: one press, then it clears
#ifdef M2_MOBILE
                ImGui::TextWrapped(TX("Ouvre le menu de test : réglages des pièces, type de partie ; sans touche F2."));
#else
                ImGui::SameLine();
                ImGui::TextDisabled(TX("(ouvre le menu de test : réglages des pièces, type de partie ; sans touche F2)"));
#endif
                ImGui::SetItemTooltip(TX("Cochez, puis Démarrer ou Reprendre : une fois le jeu lancé, le bouton Test est maintenu\n"
                                      "3 secondes, ce qui ouvre le menu de test, puis la case se décoche toute seule.\n"
                                      "F2, ou tout bouton affecté à Test, fonctionne toujours."));
                ImGui::BeginDisabled(!rom_ok_);
                if (game_running) {
                    if (ImGui::Button(TX("Reprendre"), ImVec2(140, 40))) result = Resume;
                    ImGui::SameLine();
                    if (ImGui::Button(TX("Réinitialiser"), ImVec2(140, 40))) result = Reset;
                } else if (ImGui::Button(TX("Démarrer"), ImVec2(140, 40))) {
                    result = StartGame;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button(TX("Quitter"), ImVec2(140, 40))) result = Quit;
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem(TX("Commandes"))) {
            Controls &c = cfg_.controls;
            ImGui::Spacing();
            ImGui::Text("%s", TXF("Manette : %s", pad ? SDL_GetGamepadName(pad) : TXL("aucune connectée")).c_str());
            std::string joys;
            for (SDL_Joystick *j : devices.joys) {
                const char *n = SDL_GetJoystickName(j);
                joys += (joys.empty() ? "" : ", ") + std::string(n ? n : "joystick");
            }
            ImGui::TextWrapped("%s", TXF("Volants et joysticks : %s", joys.empty() ? TXL("aucun connecté") : joys.c_str()).c_str());
            // live meters
            const bool *keys = SDL_GetKeyboardState(nullptr);
            const float steer = c.value(SteerRight, keys, devices) - c.value(SteerLeft, keys, devices);
            char label[32];
            std::snprintf(label, sizeof label, "%+.2f", c.steer_invert ? -steer : steer);
            ImGui::ProgressBar(0.5f + 0.5f * (c.steer_invert ? -steer : steer), ImVec2(200, 0), label);
            ImGui::SameLine();
            ImGui::TextUnformatted(TX("Direction"));
            const float acc = c.value(Accelerate, keys, devices), brk = c.value(Brake, keys, devices);
            ImGui::ProgressBar(acc, ImVec2(200, 0));
            ImGui::SameLine();
            ImGui::TextUnformatted(TX("Accélérateur"));
            ImGui::ProgressBar(brk, ImVec2(200, 0));
            ImGui::SameLine();
            ImGui::TextUnformatted(TX("Frein"));
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat(TX("Zone morte"), &c.deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Inverser la direction"), &c.steer_invert)) cfg_.save();
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat(TX("Zone morte du volant (expérimental)"), &c.joy_deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
            ImGui::SetNextItemWidth(200);
            int wheel_sensitivity = int(c.wheel_sensitivity * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Sensibilité du volant"), &wheel_sensitivity, 25, 300, "%d%%")) {
                c.wheel_sensitivity = float(wheel_sensitivity) / 100.0f;
                cfg_.save();
            }
            ImGui::TextDisabled("%s", TXF("100 %% : linéaire. Plus bas : plus doux près du centre. Plus haut : plus vif.\n"
                                "Seulement pour l'axe de direction d'un volant ou joystick ; le braquage complet est conservé.").c_str());
            ImGui::SetNextItemWidth(200);
            int pad_sensitivity = int(c.pad_steer_sensitivity * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Sensibilité du stick (manette)"), &pad_sensitivity, 25, 100, "%d%%")) {
                c.pad_steer_sensitivity = float(pad_sensitivity) / 100.0f;
                cfg_.save();
            }
            ImGui::SetNextItemWidth(200);
            int pad_speed = int(c.pad_steer_speed * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Vitesse de braquage (manette)"), &pad_speed, 2, 100, pad_speed >= 100 ? TX("Immédiate") : "%d%%")) {
                c.pad_steer_speed = float(pad_speed) / 100.0f;
                cfg_.save();
            }
            ImGui::TextDisabled(TX("Stick de la manette seulement. Sensibilité : plus bas, plus doux près du centre (braquage complet gardé).\n"
                                "Vitesse : à quelle vitesse le volant du jeu suit le stick, par image (100 %% : aussitôt)."));
            ImGui::TextDisabled(TX("Gâchettes, sticks, volants et pédales sont analogiques. Pour affecter un axe de volant ou de pédale,\n"
                                "cliquez sur son bouton, puis tournez le volant ou appuyez sur la pédale aussi loin que vous voulez\n"
                                "le braquage ou la course complète, et relâchez : cela fixe sa plage."));
#ifndef _WIN32 // SDL's own Logitech driver is off on Windows already
            if (ImGui::Checkbox(TX("Ancienne prise en charge des volants Logitech (redémarrer le jeu)"), &cfg_.legacy_logitech_wheels)) cfg_.save();
            ImGui::TextDisabled(TX("Volants Logitech par le pilote du système : nécessaire pour un volant listé mais qui ne fait\n"
                                "rien quand on l'affecte (le Driving Force d'origine). Décoché : le pilote de SDL, qui donne\n"
                                "le retour de force aux volants récents sous macOS. Après un changement, redémarrez et\n"
                                "affectez de nouveau les commandes du volant."));
#endif

            ImGui::SetNextItemWidth(200);
            int ffb = int(cfg_.ffb_strength * 100.0f + 0.5f);
            if (ImGui::SliderInt(TX("Retour de force (expérimental)"), &ffb, 0, 100, ffb ? "%d%%" : TX("Coupé"))) {
                cfg_.ffb_strength = float(ffb) / 100.0f;
                cfg_.save();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Inverser la force"), &cfg_.ffb_invert)) cfg_.save();
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Journal du retour de force"), &cfg_.ffb_log)) cfg_.save();
#ifdef __linux__ // for Linux drivers without a spring effect (the kernel's Logitech driver)
            if (ImGui::Checkbox(TX("Centrage calculé par le jeu"), &cfg_.ffb_centring_override)) cfg_.save();
            ImGui::SetItemTooltip(TX("Pour un volant qui pousse aux chocs mais ne se recentre pas dans les virages : son pilote\n"
                                  "Linux n'a pas d'effet de ressort (la 1re ligne du journal dit \"spring no\").\n"
                                  "Le recentrage du jeu est alors calculé ici d'après la position du volant et\n"
                                  "envoyé comme une force constante. Laissez décoché si le volant se recentre déjà."));
#endif
            ImGui::TextDisabled(TX("Le moteur du volant de la borne (recentrage, résistance, volant qui tire), sur l'appareil\n"
                                "affecté à la direction ; une manette vibre quand la voiture est poussée. Actuellement : %s.\n"
                                "Volants et retour de force ne sont pas encore testés sur du vrai matériel : avis bienvenus.\n"
                                "Le journal va dans daytona.log, à côté de launcher.ini sous Windows, sinon dans le terminal."), ffb_device_);

            if (ImGui::BeginTable("binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn(TX("Commande"), ImGuiTableColumnFlags_WidthFixed, 130);
                ImGui::TableSetupColumn(TX("Clavier"), ImGuiTableColumnFlags_WidthFixed, 130);
                ImGui::TableSetupColumn(TX("Manette"), ImGuiTableColumnFlags_WidthFixed, 250);
                ImGui::TableSetupColumn(TX("Volant / joystick (expérimental)"));
                ImGui::TableHeadersRow();
                for (int a = 0; a < kNumActions; a++) {
                    ImGui::PushID(a);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(action_name(Action(a)));
                    ImGui::TableNextColumn();
                    const bool cap_key = capture_action_ == a && capture_kind_ == CaptureKey;
                    const char *kn = c.bind[a].key == SDL_SCANCODE_UNKNOWN ? "-" : SDL_GetScancodeName(c.bind[a].key);
                    if (ImGui::Button(cap_key ? TX("Appuyez sur une touche...") : kn, ImVec2(120, 0))) start_capture(a, CaptureKey, devices);
                    ImGui::TableNextColumn();
                    const bool cap_pad = capture_action_ == a && capture_kind_ == CapturePad;
                    const std::string pn = cap_pad ? TX("Appuyez ou bougez...") : c.bind[a].pad.describe();
                    if (ImGui::Button(pn.c_str(), ImVec2(190, 0))) start_capture(a, CapturePad, devices);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(TX("effacer"))) {
                        c.bind[a].pad = PadInput{};
                        cfg_.save();
                    }
                    ImGui::TableNextColumn();
                    const bool cap_joy = capture_action_ == a && capture_kind_ == CaptureJoy;
                    const std::string jn = !cap_joy ? c.bind[a].joy.describe(devices)
                                           : joy_tracking_ ? TX("Aussi loin que voulu, puis relâchez...")
                                                           : TX("Appuyez, ou bougez à fond et relâchez...");
                    if (ImGui::Button(jn.c_str(), ImVec2(300, 0))) start_capture(a, CaptureJoy, devices);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(TX("effacer##joy"))) {
                        c.bind[a].joy = JoyInput{};
                        cfg_.save();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled(TX("Échap annule une affectation ; Retour arrière efface une touche."));
            if (ImGui::Button(TX("Remettre les réglages par défaut"))) {
                c.set_defaults();
                cfg_.save();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TX("Son"))) {
            ImGui::Spacing();
            auto percent = [&](const char *label, float &value) { // a 0..1 setting as 0-100%
                ImGui::SetNextItemWidth(200);
                int v = int(value * 100.0f + 0.5f);
                if (ImGui::SliderInt(label, &v, 0, 100, "%d%%")) {
                    value = float(v) / 100.0f;
                    cfg_.save();
                }
            };
            percent(TX("Volume"), cfg_.volume);
            ImGui::SameLine();
            if (ImGui::Checkbox(TX("Muet"), &cfg_.mute)) cfg_.save();
            ImGui::Spacing();
            percent(TX("Musique"), cfg_.music_volume);
            percent(TX("Effets"), cfg_.effects_volume);
#ifndef M2_MOBILE
            const char *versions[] = {TX("Arcade"), TX("Saturn / Windows (CD)"), TX("Dreamcast")};
            int version = static_cast<int>(cfg_.music_version);
            ImGui::SetNextItemWidth(250);
            if (ImGui::Combo(TX("Version de la musique"), &version, versions, IM_ARRAYSIZE(versions))) {
                cfg_.music_version = static_cast<MusicVersion>(version);
                cfg_.save();
            }
#endif
            ImGui::TextDisabled(TX("L'équilibre entre la musique et tout le reste (moteur, dérapages, chocs).\n"
                                "100 %% et 100 %% : le mélange de la carte son de la borne."));

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Checkbox(TX("Son natif (expérimental, réinitialiser)"), &cfg_.native_audio)) cfg_.save();
            ImGui::TextDisabled(TX("Séquenceur et mixeur natifs ; le son de référence reste disponible pour comparer."));
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::PopTextWrapPos();
#ifdef M2_MOBILE
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && ImGui::IsMouseDragging(0))
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
#endif
    ImGui::End();
    return result;
}

} // namespace app
