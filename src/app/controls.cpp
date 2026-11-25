#include "app/controls.h"
#include "app/i18n_mark.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace app {

namespace {
const struct {
    const char *name, *key;
} kActions[kNumActions] = {
    {TXR("Tourner à gauche"), "steer_left"}, {TXR("Tourner à droite"), "steer_right"}, {TXR("Accélérer"), "accelerate"}, {TXR("Freiner"), "brake"},
    {TXR("Vitesse 1"), "gear1"},          {TXR("Vitesse 2"), "gear2"},            {TXR("Vitesse 3"), "gear3"},           {TXR("Vitesse 4"), "gear4"},
    {TXR("Monter un rapport"), "gear_up"},      {TXR("Descendre un rapport"), "gear_down"},    {TXR("Vue 1 (rouge)"), "view1"},    {TXR("Vue 2 (bleue)"), "view2"},
    {TXR("Vue 3 (jaune)"), "view3"}, {TXR("Vue 4 (verte)"), "view4"},    {TXR("Pièce"), "coin"},              {TXR("Start"), "start"},
    {TXR("Test"), "test"},             {TXR("Service"), "service"},
};
} // namespace

const char *action_name(Action a) { return TXR(kActions[a].name); }
const char *action_key(Action a) { return kActions[a].key; }

std::string PadInput::describe() const {
    switch (kind) {
    case Button: {
        const char *n = SDL_GetGamepadStringForButton(SDL_GamepadButton(index));
        return std::string(TXR("Bouton ")) + (n ? n : "?");
    }
    case Axis: {
        const char *n = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(index));
        const bool trigger = index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || index == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        return std::string(trigger ? TXR("Gâchette ") : TXR("Axe ")) + (n ? n : "?") + (trigger ? "" : (dir > 0 ? " +" : " -"));
    }
    default: return "-";
    }
}

std::string PadInput::save() const {
    switch (kind) {
    case Button: return std::string("button:") + SDL_GetGamepadStringForButton(SDL_GamepadButton(index));
    case Axis: return std::string("axis:") + SDL_GetGamepadStringForAxis(SDL_GamepadAxis(index)) + (dir > 0 ? ":+" : ":-");
    default: return "none";
    }
}

PadInput PadInput::parse(const std::string &s) {
    PadInput p;
    if (s.compare(0, 7, "button:") == 0) {
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s.substr(7).c_str());
        if (b != SDL_GAMEPAD_BUTTON_INVALID) p.kind = Button, p.index = b;
    } else if (s.compare(0, 5, "axis:") == 0) {
        const auto c = s.find(':', 5);
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s.substr(5, c - 5).c_str());
        if (a != SDL_GAMEPAD_AXIS_INVALID) p.kind = Axis, p.index = a, p.dir = (c != std::string::npos && s[c + 1] == '-') ? -1 : 1;
    }
    return p;
}

void Devices::handle_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_JOYSTICK_ADDED:
        if (SDL_Joystick *j = SDL_OpenJoystick(e.jdevice.which)) { // our own reference (a gamepad holds another)
            if (std::find(joys.begin(), joys.end(), j) == joys.end()) joys.push_back(j);
            else SDL_CloseJoystick(j);
        }
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        for (auto it = joys.begin(); it != joys.end(); ++it)
            if (SDL_GetJoystickID(*it) == e.jdevice.which) {
                SDL_CloseJoystick(*it);
                joys.erase(it);
                break;
            }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && e.gdevice.which == SDL_GetGamepadID(pad)) SDL_CloseGamepad(pad), pad = nullptr;
        break;
    default: break;
    }
}

void Devices::close_all() {
    if (pad) SDL_CloseGamepad(pad), pad = nullptr;
    for (SDL_Joystick *j : joys) SDL_CloseJoystick(j);
    joys.clear();
}

namespace {
std::string guid_string(SDL_Joystick *j) {
    char b[64];
    SDL_GUIDToString(SDL_GetJoystickGUID(j), b, sizeof b);
    return b;
}
} // namespace

SDL_Joystick *Devices::find(const std::string &guid) const {
    for (SDL_Joystick *j : joys)
        if (guid_string(j) == guid) return j;
    return nullptr;
}

float JoyInput::value(const Devices &d, float deadzone) const {
    if (kind == None) return 0.f;
    SDL_Joystick *j = d.find(guid);
    if (!j) return 0.f;
    switch (kind) {
    case Button: return SDL_GetJoystickButton(j, index) ? 1.f : 0.f;
    case Hat: return (SDL_GetJoystickHat(j, index) & mask) ? 1.f : 0.f;
    case Axis: {
        if (full == rest) return 0.f;
        const float x = std::clamp(float(SDL_GetJoystickAxis(j, index) - rest) / float(full - rest), 0.f, 1.f);
        return x <= deadzone ? 0.f : (x - deadzone) / (1.f - deadzone);
    }
    default: return 0.f;
    }
}

std::string JoyInput::describe(const Devices &d) const {
    if (kind == None) return "-";
    SDL_Joystick *j = d.find(guid);
    std::string what;
    if (kind == Button) what = TXR("Bouton ") + std::to_string(index + 1);
    else if (kind == Hat) what = std::string(TXR("Croix ")) + (mask == SDL_HAT_UP ? TXR("haut") : mask == SDL_HAT_DOWN ? TXR("bas") : mask == SDL_HAT_LEFT ? TXR("gauche") : TXR("droite"));
    else what = TXR("Axe ") + std::to_string(index + 1) + (full > rest ? " +" : " -");
    const char *name = j ? SDL_GetJoystickName(j) : nullptr;
    return what + (j ? std::string(" (") + (name ? name : "joystick") + ")" : TXR(" (non connecté)"));
}

std::string JoyInput::save() const {
    switch (kind) {
    case Button: return "button:" + guid + ":" + std::to_string(index);
    case Hat: return "hat:" + guid + ":" + std::to_string(index) + ":" + std::to_string(mask);
    case Axis: return "axis:" + guid + ":" + std::to_string(index) + ":" + std::to_string(rest) + ":" + std::to_string(full);
    default: return "none";
    }
}

JoyInput JoyInput::parse(const std::string &s) {
    JoyInput j;
    std::vector<std::string> f;
    for (size_t at = 0;;) {
        const size_t c = s.find(':', at);
        f.push_back(s.substr(at, c - at));
        if (c == std::string::npos) break;
        at = c + 1;
    }
    auto num = [&](size_t i) { return i < f.size() ? std::atoi(f[i].c_str()) : 0; };
    if (f.size() >= 3 && f[0] == "button") j.kind = Button, j.guid = f[1], j.index = num(2);
    else if (f.size() >= 4 && f[0] == "hat") j.kind = Hat, j.guid = f[1], j.index = num(2), j.mask = num(3);
    else if (f.size() >= 5 && f[0] == "axis") j.kind = Axis, j.guid = f[1], j.index = num(2), j.rest = num(3), j.full = num(4);
    return j;
}

void Controls::set_defaults() {
    auto b = [&](Action a, SDL_Scancode k, PadInput::Kind kind, int index, int dir = 1) {
        bind[a].key = k;
        bind[a].pad.kind = kind;
        bind[a].pad.index = index;
        bind[a].pad.dir = dir;
        bind[a].joy = JoyInput{};
    };
    b(SteerLeft, SDL_SCANCODE_LEFT, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFTX, -1);
    b(SteerRight, SDL_SCANCODE_RIGHT, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFTX, +1);
    b(Accelerate, SDL_SCANCODE_UP, PadInput::Axis, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    b(Brake, SDL_SCANCODE_DOWN, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    b(Gear1, SDL_SCANCODE_1, PadInput::None, 0);
    b(Gear2, SDL_SCANCODE_2, PadInput::None, 0);
    b(Gear3, SDL_SCANCODE_3, PadInput::None, 0);
    b(Gear4, SDL_SCANCODE_4, PadInput::None, 0);
    b(GearUp, SDL_SCANCODE_W, PadInput::Button, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    b(GearDown, SDL_SCANCODE_Q, PadInput::Button, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    b(View1, SDL_SCANCODE_A, PadInput::Button, SDL_GAMEPAD_BUTTON_SOUTH);
    b(View2, SDL_SCANCODE_S, PadInput::Button, SDL_GAMEPAD_BUTTON_EAST);
    b(View3, SDL_SCANCODE_D, PadInput::Button, SDL_GAMEPAD_BUTTON_WEST);
    b(View4, SDL_SCANCODE_F, PadInput::Button, SDL_GAMEPAD_BUTTON_NORTH);
    b(Coin, SDL_SCANCODE_5, PadInput::Button, SDL_GAMEPAD_BUTTON_BACK);
    b(Start, SDL_SCANCODE_RETURN, PadInput::Button, SDL_GAMEPAD_BUTTON_START);
    b(Test, SDL_SCANCODE_F2, PadInput::None, 0);
    b(Service, SDL_SCANCODE_F3, PadInput::None, 0);
    deadzone = 0.08f;
    joy_deadzone = 0.02f;
    wheel_sensitivity = 1.0f;
    pad_steer_sensitivity = 0.55f;
    pad_steer_speed = 0.07f;
    steer_invert = false;
}

float Controls::value(Action a, const bool *keys, const Devices &d) const {
    const Binding &b = bind[a];
    SDL_Gamepad *pad = d.pad;
    float v = (b.key != SDL_SCANCODE_UNKNOWN && keys && keys[b.key]) ? 1.f : 0.f;
    if (pad && b.pad.kind == PadInput::Button && SDL_GetGamepadButton(pad, SDL_GamepadButton(b.pad.index))) v = 1.f;
    if (pad && b.pad.kind == PadInput::Axis) {
        const auto axis = SDL_GamepadAxis(b.pad.index);
        const bool trigger = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        // Triggers have a mechanical stop. Keep a small rest margin without
        // applying the stick's wider drift compensation to the triggers.
        const float zone = trigger ? std::min(deadzone, 0.02f) : deadzone;
        float x = SDL_GetGamepadAxis(pad, axis) / 32767.f * float(b.pad.dir);
        x = std::clamp(x, 0.f, 1.f);
        x = x <= zone ? 0.f : (x - zone) / (1.f - zone); // full travel still reaches 1
        v = std::max(v, x);
    }
    float joy = b.joy.value(d, joy_deadzone);
    if ((a == SteerLeft || a == SteerRight) && b.joy.kind == JoyInput::Axis) {
        const float sensitivity = std::isfinite(wheel_sensitivity)
            ? std::clamp(wheel_sensitivity, 0.25f, 3.0f) : 1.0f;
        // Apply after calibration/dead zone, preserving centre and full lock.
        if (sensitivity != 1.0f) joy = std::pow(joy, 1.0f / sensitivity);
    }
    return std::max({v, joy, touch[a]});
}

bool Controls::analog_source(Action a, const Devices &d) const {
    return (touch_steering && (a == SteerLeft || a == SteerRight)) ||
           (d.pad && bind[a].pad.kind == PadInput::Axis) || (bind[a].joy.kind == JoyInput::Axis && d.find(bind[a].joy.guid));
}

rt::Inputs Controls::sample(const bool *keys, const Devices &d) {
    rt::Inputs in;
    // Steering: an analogue stick sets the position directly; keys ramp toward full lock and back.
    const float l = value(SteerLeft, keys, d), r = value(SteerRight, keys, d);
    float target = r - l;
    if (steer_invert) target = -target;
    const bool analog = (analog_source(SteerLeft, d) || analog_source(SteerRight, d)) &&
                        !(keys && (keys[bind[SteerLeft].key] || keys[bind[SteerRight].key]));
    const bool wheel = (bind[SteerLeft].joy.kind == JoyInput::Axis && d.find(bind[SteerLeft].joy.guid)) ||
                       (bind[SteerRight].joy.kind == JoyInput::Axis && d.find(bind[SteerRight].joy.guid));
    const bool pad_stick = analog && !wheel && !touch_steering && d.pad &&
                           (bind[SteerLeft].pad.kind == PadInput::Axis || bind[SteerRight].pad.kind == PadInput::Axis);
    if (pad_stick) {
        // Gamepad: a response curve (full lock kept), then the wheel turned
        // toward the stick at a limited rate, as a hand turns a wheel.
        const float s = std::clamp(std::isfinite(pad_steer_sensitivity) ? pad_steer_sensitivity : 1.0f, 0.25f, 1.0f);
        // Blend linear and cubic response. The linear share keeps small
        // corrections visible after conversion to the cabinet's 8-bit ADC.
        const float curved = std::lerp(target * target * target, target, s);
        const float rate = std::clamp(std::isfinite(pad_steer_speed) ? pad_steer_speed : 1.0f, 0.02f, 1.0f);
        steer += std::clamp(curved - steer, -rate, rate);
    } else {
        steer = analog ? target : steer + std::clamp(target - steer, -0.12f, 0.12f);
    }
    accel = value(Accelerate, keys, d);
    brake = value(Brake, keys, d);
    // ADC ranges (MAME's daytona ports): steering 0x20-0xe0 centred on 0x80, pedals 0x20 (up) to 0xe0 (floored)
    in.steer = uint8_t(std::lround(0x80 + std::clamp(steer, -1.f, 1.f) * 0x60));
    in.accel = uint8_t(std::lround(0x20 + std::clamp(accel, 0.f, 1.f) * 0xc0));
    in.brake = uint8_t(std::lround(0x20 + std::clamp(brake, 0.f, 1.f) * 0xc0));

    // Gears: direct selection, or sequential shifts on the press
    for (int g = 0; g < 4; g++)
        if (value(Action(Gear1 + g), keys, d) > 0.5f) gear = g + 1;
    for (Action a : {GearUp, GearDown}) {
        const bool now = value(a, keys, d) > 0.5f;
        if (now && !held_[a]) gear = std::clamp(gear + (a == GearUp ? 1 : -1), 1, 4);
        held_[a] = now;
    }
    static const uint8_t gearvalue[5] = {0, 2, 1, 6, 5}; // MAME daytona_gearbox_r: neutral, 1-4
    in.in1 = uint8_t((in.in1 & ~0x70) | (gearvalue[gear] << 4));

    auto low = [&](uint8_t &port, uint8_t bit, Action a) { if (value(a, keys, d) > 0.5f) port &= uint8_t(~bit); };
    low(in.in0, 0x01, Coin);
    low(in.in0, 0x04, Test);
    low(in.in0, 0x08, Service);
    low(in.in0, 0x10, Start);
    low(in.in0, 0x20, View1);
    low(in.in0, 0x40, View2);
    low(in.in0, 0x80, View3);
    low(in.in1, 0x01, View4);
    return in;
}

} // namespace app
