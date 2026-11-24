#include "app/home_menu.h"
#include "app/i18n.h"

#include "imgui.h"
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace app {

using Course = rt::RaceCourse;

void HomeMenu::init() {
    auto *atlas = ImGui::GetIO().Fonts;
    auto *fallback = atlas->AddFontDefaultVector();
    i18n::add_fonts(); // Chinese, Japanese, Arabic glyphs merged into it (the launcher's font)
    display_ = body_ = fallback;
#ifdef _WIN32
    if (const char *windows = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "WINDIR")) {
        const std::string folder = std::string(windows) + "/Fonts/";
        const std::string headline = folder + "bahnschrift.ttf", body = folder + "segoeuib.ttf";
        if (std::ifstream(headline, std::ios::binary))
            if (auto *font = atlas->AddFontFromFileTTF(headline.c_str())) display_ = font;
        if (std::ifstream(body, std::ios::binary))
            if (auto *font = atlas->AddFontFromFileTTF(body.c_str())) {
                body_ = font;
                i18n::add_fonts(); // and into the menu's text
            }
    }
#endif
}

HomeMenu::Action HomeMenu::draw(bool has_race, bool loading, const char *message, float opacity) {
    const auto *view = ImGui::GetMainViewport();
    const float w = view->Size.x, h = view->Size.y;
    const float scale = std::max(0.13f, std::min(h / 900.f, w / 980.f));
    const float left = std::max(w * 0.052f, 31.f * scale);
    const float top = std::max(30.f * scale, h * 0.5f - 315.f * scale);
    const float panel_width = 363.f * scale;
    const auto px = [&](float x, float y) { return ImVec2(view->Pos.x + x, view->Pos.y + y); };
    ImGui::SetNextWindowPos(view->Pos);
    ImGui::SetNextWindowSize(view->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    ImGui::Begin("Daytona accueil", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    auto *draw = ImGui::GetWindowDrawList();
    const auto color = [](ImVec4 value) { return ImGui::GetColorU32(value); };
    const ImU32 ice = color({0.63f,0.83f,0.93f,1}), cream = color({0.96f,0.98f,1,1});
    const ImU32 mint = color({0.48f,0.98f,0.79f,1}), subdued = color({0.68f,0.77f,0.82f,1});
    auto text = [&](ImFont *font, float size, float x, float y, ImU32 tint, const char *label) {
        ImGui::PushFont(font, size * scale);
        draw->AddText(px(x,y), tint, label);
        ImGui::PopFont();
    };
    auto at = [&](float x, float y) { ImGui::SetCursorScreenPos(px(x,y)); };
    auto button = [&](const char *label, float x, float y, float width, float height, bool primary, float font_size = 0.f) {
        at(x,y);
        ImGui::PushFont(body_, (font_size > 0 ? font_size : primary ? 25.f : 16.f) * scale);
        ImGui::PushStyleColor(ImGuiCol_Button, primary ? ImVec4(0.57f,0.98f,0.80f,1) : ImVec4(0.08f,0.17f,0.24f,0.86f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? ImVec4(0.78f,1,0.90f,1) : ImVec4(0.14f,0.34f,0.40f,0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.34f,0.70f,0.64f,1));
        ImGui::PushStyleColor(ImGuiCol_Text, primary ? ImVec4(0.025f,0.12f,0.14f,1) : ImVec4(0.91f,0.97f,1,1));
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(1,0.79f,0.39f,1));
        const bool clicked = ImGui::Button(label, {width,height});
        ImGui::PopStyleColor(5);
        ImGui::PopFont();
        return clicked;
    };
    draw->AddRectFilled(px(left,top), px(left+31*scale,top+3*scale), mint);
    text(body_, 15, left+43*scale, top-7*scale, ice, TX("M O D E L   2    /    É D I T I O N   P C"));
    text(display_, 94, left-4*scale, top+24*scale, cream, "DAYTONA");
    text(body_, 26, left, top+123*scale, mint, "A R C A D E   N A T I V E");
    text(body_, 18, left, top+184*scale, cream, loading ? TX("Préparation de la course...") : TX("Course solo"));
    const char *tracks[] = {"Three Seven Speedway", "Dinosaur Canyon", "Seaside Street Galaxy"};
    const unsigned course = unsigned(course_);
    text(display_, 24, left, top+211*scale, ice, tracks[course]);

    const Screen screen = loading ? Screen::Loading : has_race ? Screen::Paused : Screen::Ready;
    const bool focus_primary = screen != screen_;
    screen_ = screen;
    Action action = Stay;
    const float row = top + 400.f * scale;
    if (loading) {
        const float phase = float(ImGui::GetTime() * 0.72 - int(ImGui::GetTime() * 0.72));
        draw->AddRectFilled(px(left,row), px(left+panel_width,row+4*scale), color({0.19f,0.32f,0.34f,1}));
        const float progress = phase * (panel_width - 68 * scale);
        draw->AddRectFilled(px(left+progress,row), px(left+progress+68*scale,row+4*scale), mint);
        if (focus_primary) ImGui::SetKeyboardFocusHere();
        if (button(TX("Retour"),left,row+24*scale,panel_width,46*scale,false)) action = Cancel;
        ImGui::SetItemDefaultFocus();
    } else {
        text(body_, 15, left, top+247*scale, subdued,
             has_race ? TX("Mode de la prochaine course") : TX("Mode de jeu"));
        if (button(TX("Course normale"),left,top+270*scale,174*scale,38*scale,
                   mode_==rt::RaceMode::Normal,16.f))
            mode_ = rt::RaceMode::Normal;
        if (button(TX("Time Attack"),left+189*scale,top+270*scale,174*scale,38*scale,
                   mode_==rt::RaceMode::TimeAttack,16.f))
            mode_ = rt::RaceMode::TimeAttack;
        text(body_, 15, left, top+319*scale, subdued,
             has_race ? TX("Boîte de la prochaine course") : TX("Boîte de vitesses"));
        if (button(TX("Automatique"),left,top+343*scale,174*scale,42*scale,
                   transmission_==rt::RaceTransmission::Automatic,16.f))
            transmission_ = rt::RaceTransmission::Automatic;
        if (button(TX("Manuelle"),left+189*scale,top+343*scale,174*scale,42*scale,
                   transmission_==rt::RaceTransmission::Manual,16.f))
            transmission_ = rt::RaceTransmission::Manual;
        if (focus_primary) ImGui::SetKeyboardFocusHere();
        if (button(has_race ? TX("Reprendre") : TX("Jouer"),left,row,panel_width,63*scale,true))
            action = has_race ? Resume : Play;
        ImGui::SetItemDefaultFocus();
        if (button("01",left,row+76*scale,111*scale,42*scale,course==0)) course_ = Course::Beginner;
        if (button("02",left+126*scale,row+76*scale,111*scale,42*scale,course==1)) course_ = Course::Advanced;
        if (button("03",left+252*scale,row+76*scale,111*scale,42*scale,course==2)) course_ = Course::Expert;
        if (button(TX("Réglages"),left,row+131*scale,174*scale,47*scale,false)) action = Settings;
        if (button(TX("Quitter"),left+189*scale,row+131*scale,174*scale,47*scale,false)) action = Quit;
        if (has_race && button(TX("Nouvelle course"),left,row+190*scale,panel_width,42*scale,false)) action = Play;
    }
    if (message && *message) {
        at(left, row + (loading ? 85.f : 243.f)*scale);
        ImGui::PushFont(body_, 16*scale);
        ImGui::PushTextWrapPos(left + panel_width);
        ImGui::TextColored({1,0.74f,0.45f,1}, "%s", message);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    const float bottom = h - 51 * scale;
    draw->AddLine(px(left,bottom), px(w-left,bottom), color({0.72f,0.85f,0.91f,0.38f}), scale);
    text(body_, 13, left, bottom+12*scale, subdued, TX("SOLO   /   JEU LIBRE"));
    const auto ratio = ImGui::GetIO().DisplayFramebufferScale;
    const std::string display = TXF("SDR   /   %d × %d", int(w*ratio.x), int(h*ratio.y));
    text(body_, 13, w-left-150*scale, bottom+12*scale, cream, display.c_str());
    ImGui::End();
    ImGui::PopStyleVar(3);
    return action;
}

} // namespace app
