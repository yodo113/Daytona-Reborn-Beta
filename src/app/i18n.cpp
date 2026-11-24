#include "app/i18n.h"

#include "imgui.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace app::i18n {

namespace {

std::string g_current = "fr";
std::unordered_map<std::string, std::string> g_text, g_english; // msgid -> msgstr
std::unordered_map<std::string, std::string> g_cache;           // composed "text##id" results

std::string lang_dir() {
    const char *base = SDL_GetBasePath();
    return std::string(base ? base : "") + "lang/";
}

bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

void load(const std::string &code, std::unordered_map<std::string, std::string> &into) {
    into.clear();
    std::string text;
    if (!read_file(lang_dir() + code + ".po", text)) return;
    for (auto &[id, str] : parse_po(text)) {
        if (id.empty() || str.empty()) continue;
        into[id] = str;
    }
}

// ---- UTF-8 ----
std::vector<uint32_t> decode(const std::string &s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) cp = c & 0x07, extra = 3;
        else if (c >= 0xE0) cp = c & 0x0F, extra = 2;
        else if (c >= 0xC0) cp = c & 0x1F, extra = 1;
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        out.push_back(cp);
    }
    return out;
}

void encode(uint32_t cp, std::string &out) {
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
    else if (cp < 0x10000)
        out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
    else
        out += char(0xF0 | (cp >> 18)), out += char(0x80 | ((cp >> 12) & 0x3F)), out += char(0x80 | ((cp >> 6) & 0x3F)),
            out += char(0x80 | (cp & 0x3F));
}

// ---- Arabic shaping (Presentation Forms-B: isolated, final, initial, medial) ----
struct Forms {
    uint16_t base, isolated;
    uint8_t count; // 2: joins to the right only (isolated, final); 4: both sides
};
constexpr Forms kForms[] = {
    {0x0622, 0xFE81, 2}, {0x0623, 0xFE83, 2}, {0x0624, 0xFE85, 2}, {0x0625, 0xFE87, 2}, {0x0626, 0xFE89, 4},
    {0x0627, 0xFE8D, 2}, {0x0628, 0xFE8F, 4}, {0x0629, 0xFE93, 2}, {0x062A, 0xFE95, 4}, {0x062B, 0xFE99, 4},
    {0x062C, 0xFE9D, 4}, {0x062D, 0xFEA1, 4}, {0x062E, 0xFEA5, 4}, {0x062F, 0xFEA9, 2}, {0x0630, 0xFEAB, 2},
    {0x0631, 0xFEAD, 2}, {0x0632, 0xFEAF, 2}, {0x0633, 0xFEB1, 4}, {0x0634, 0xFEB5, 4}, {0x0635, 0xFEB9, 4},
    {0x0636, 0xFEBD, 4}, {0x0637, 0xFEC1, 4}, {0x0638, 0xFEC5, 4}, {0x0639, 0xFEC9, 4}, {0x063A, 0xFECD, 4},
    {0x0641, 0xFED1, 4}, {0x0642, 0xFED5, 4}, {0x0643, 0xFED9, 4}, {0x0644, 0xFEDD, 4}, {0x0645, 0xFEE1, 4},
    {0x0646, 0xFEE5, 4}, {0x0647, 0xFEE9, 4}, {0x0648, 0xFEED, 2}, {0x0649, 0xFEEF, 2}, {0x064A, 0xFEF1, 4},
};
const Forms *forms_of(uint32_t cp) {
    for (const auto &f : kForms)
        if (f.base == cp) return &f;
    return nullptr;
}
bool is_arabic(uint32_t cp) { return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0xFB50 && cp <= 0xFEFF); }
bool is_mark(uint32_t cp) { return cp >= 0x064B && cp <= 0x0652; } // harakat: dropped (ImGui cannot place them)
bool joins_left(uint32_t cp) { // dual-joining letters and the tatweel
    if (cp == 0x0640) return true;
    const Forms *f = forms_of(cp);
    return f && f->count == 4;
}
bool joins_right(uint32_t cp) { return cp == 0x0640 || forms_of(cp) != nullptr; }

uint32_t lam_alef(uint32_t alef) { // isolated form of the ligature, final is +1
    switch (alef) {
    case 0x0622: return 0xFEF5;
    case 0x0623: return 0xFEF7;
    case 0x0625: return 0xFEF9;
    case 0x0627: return 0xFEFB;
    default: return 0;
    }
}

// End of an ASCII printf sequence, or its start for a literal percent.
template <typename Text>
size_t printf_end(const Text &s, size_t start) {
    size_t i = start + 1;
    if (i == s.size()) return start;
    if (s[i] == '%') return i + 1;
    auto contains = [](const char *set, uint32_t cp) { return cp > 0 && cp < 128 && std::strchr(set, int(cp)); };
    while (i < s.size() && contains("-+ #0", s[i])) ++i;
    if (i < s.size() && s[i] == '*') ++i;
    else while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i < s.size() && s[i] == '*') ++i;
        else while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    }
    if (i < s.size() && contains("hljztL", s[i])) {
        const auto length = s[i++];
        if ((length == 'h' || length == 'l') && i < s.size() && s[i] == length) ++i;
    }
    return i < s.size() && contains("diouxXfFeEgGaAcspn", s[i]) ? i + 1 : start;
}

bool same_formats(std::string_view source, std::string_view translation) {
    auto next = [](std::string_view text, size_t &pos) {
        const size_t begin = text.find('%', pos);
        if (begin == std::string_view::npos) return std::string_view{};
        pos = std::max(begin + 1, printf_end(text, begin));
        return text.substr(begin, pos - begin);
    };
    size_t a = 0, b = 0;
    while (true) {
        const auto original = next(source, a), localized = next(translation, b);
        if (original != localized) return false;
        if (original.empty()) return true;
    }
}

bool digit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

enum Dir { L, R, N };
Dir dir_of(uint32_t cp) {
    if (is_arabic(cp)) return R;
    if (digit(cp) || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp < 0x0600))
        return L;
    return N;
}
uint32_t mirror(uint32_t cp) {
    switch (cp) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '<': return '>';
    case '>': return '<';
    case '{': return '}';
    case '}': return '{';
    default: return cp;
    }
}

std::string shape_line(const std::vector<uint32_t> &in) {
    std::vector<uint32_t> s;
    for (uint32_t cp : in)
        if (!is_mark(cp)) s.push_back(cp);
    // joined forms, in logical order
    std::vector<uint32_t> shaped;
    for (size_t i = 0; i < s.size(); ++i) {
        const uint32_t cp = s[i];
        const Forms *f = forms_of(cp);
        if (!f) {
            shaped.push_back(cp);
            continue;
        }
        const bool prev = i > 0 && joins_left(s[i - 1]);
        if (cp == 0x0644 && i + 1 < s.size() && lam_alef(s[i + 1])) { // lam-alef ligature
            shaped.push_back(lam_alef(s[i + 1]) + (prev ? 1 : 0));
            ++i;
            continue;
        }
        const bool next = f->count == 4 && i + 1 < s.size() && joins_right(s[i + 1]);
        int form = prev && next ? 3 : prev ? 1 : next ? 2 : 0; // isolated, final, initial, medial
        if (f->count == 2) form = prev ? 1 : 0;
        shaped.push_back(uint32_t(f->isolated + form));
    }
    // right to left: neutrals take their neighbours' direction when both
    // agree, else the line's (right to left); then the whole line is
    // reversed and each left-to-right run turned back
    std::vector<Dir> d(shaped.size());
    for (size_t i = 0; i < shaped.size(); ++i) d[i] = dir_of(shaped[i]);
    for (size_t i = 0; i < shaped.size(); ++i) {
        const auto cp = shaped[i];
        if (cp == '%') {
            // Keep unexpanded formats usable (including ImGui slider labels).
            const size_t end = printf_end(shaped, i);
            if (end > i) {
                std::fill(d.begin() + ptrdiff_t(i), d.begin() + ptrdiff_t(end), L);
                i = end - 1;
            } else if (i > 0 && digit(shaped[i - 1])) {
                d[i] = L;
            }
        } else if ((cp == '+' || cp == '-') && i + 1 < shaped.size() && digit(shaped[i + 1])) {
            d[i] = L;
        }
    }
    for (size_t i = 0; i < shaped.size(); ++i) {
        if (d[i] != N) continue;
        size_t j = i;
        while (j < shaped.size() && d[j] == N) ++j;
        const Dir before = i > 0 ? d[i - 1] : R, after = j < shaped.size() ? d[j] : R;
        const Dir fill = before == L && after == L ? L : R;
        for (size_t k = i; k < j; ++k) d[k] = fill;
        i = j - 1;
    }
    std::vector<uint32_t> visual(shaped.rbegin(), shaped.rend());
    std::vector<Dir> vd(d.rbegin(), d.rend());
    for (size_t i = 0; i < visual.size();) {
        if (vd[i] != L) {
            visual[i] = mirror(visual[i]);
            ++i;
            continue;
        }
        size_t j = i;
        while (j < visual.size() && vd[j] == L) ++j;
        std::reverse(visual.begin() + ptrdiff_t(i), visual.begin() + ptrdiff_t(j));
        i = j;
    }
    std::string out;
    for (uint32_t cp : visual) encode(cp, out);
    return out;
}

void unescape_into(const std::string &line, std::string &out) {
    // the quoted part of a PO line
    const size_t a = line.find('"'), b = line.rfind('"');
    if (a == std::string::npos || b <= a) return;
    for (size_t i = a + 1; i < b; ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < b) {
            const char e = line[++i];
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
        }
        out += c;
    }
}

} // namespace

const std::vector<Language> &languages() {
    static const std::string arabic = shape_arabic("العربية"); // drawn as it reads
    static const std::vector<Language> list = {
        {"fr", "Français"}, {"en", "English"}, {"pt", "Português"}, {"es", "Español"},
        {"zh", "中文"},      {"ja", "日本語"},   {"ar", arabic.c_str()},
    };
    return list;
}

const std::string &current() { return g_current; }

void set(const std::string &code) {
    bool known = false;
    for (const auto &l : languages()) known = known || code == l.code;
    g_current = known ? code : "fr";
    g_cache.clear();
    if (g_current == "fr") {
        g_text.clear();
        g_english.clear();
        return;
    }
    load("en", g_english);
    if (g_current == "en") g_text.clear();
    else load(g_current, g_text);
}

bool available(const char *code) {
    if (std::strcmp(code, "fr") == 0) return true;
    return std::ifstream(lang_dir() + code + ".po", std::ios::binary).good();
}

std::string system_default() {
    int count = 0;
    std::string pick = "en";
    if (SDL_Locale **locales = SDL_GetPreferredLocales(&count)) {
        for (int i = 0; i < count && locales[i]; ++i) {
            const char *lang = locales[i]->language;
            if (!lang) continue;
            bool found = false;
            for (const auto &l : languages())
                if (std::strcmp(lang, l.code) == 0 && available(l.code)) pick = l.code, found = true;
            if (found) break;
        }
        SDL_free(locales);
    }
    return pick;
}

namespace {

struct Translation {
    const std::string *value = nullptr;
    bool arabic = false;
};

Translation lookup(const std::string &key) {
    if (auto it = g_text.find(key); it != g_text.end()) return {&it->second, g_current == "ar"};
    if (auto it = g_english.find(key); it != g_english.end()) return {&it->second, false};
    return {};
}

} // namespace

const char *logical(const char *french) {
    if (g_current == "fr" || !french || !*french) return french;
    const auto found = lookup(french);
    return found.value && same_formats(french, *found.value) ? found.value->c_str() : french;
}

const char *tr(const char *french) {
    if (g_current == "fr" || !french || !*french) return french;
    const char *hash = std::strstr(french, "##");
    const std::string key = hash ? std::string(french, size_t(hash - french)) : std::string(french);
    const auto found = lookup(key);
    if (!hash && !found.arabic) return found.value ? found.value->c_str() : french;
    auto [it, added] = g_cache.try_emplace(french);
    if (added) {
        it->second = found.arabic ? shape_arabic(*found.value) : found.value ? *found.value : key;
        if (hash) it->second += hash;
    }
    return it->second.c_str();
}

std::string format(const char *french, ...) {
    const auto found = g_current == "fr" ? Translation{} : lookup(french);
    const bool translated = found.value && same_formats(french, *found.value);
    const char *pattern = translated ? found.value->c_str() : french;
    std::va_list args, count_args;
    va_start(args, french);
    va_copy(count_args, args);
    const int length = std::vsnprintf(nullptr, 0, pattern, count_args);
    va_end(count_args);
    std::string text;
    if (length >= 0) {
        text.resize(size_t(length));
        std::vsnprintf(text.data(), text.size() + 1, pattern, args);
    }
    va_end(args);
    return translated && found.arabic ? shape_arabic(text) : text;
}

void add_fonts() {
#ifdef _WIN32
    auto *atlas = ImGui::GetIO().Fonts;
    // std::getenv: Windows names it "windir", and SDL's lookup minds the case
    const char *windows = std::getenv("WINDIR");
    if (!windows) return;
    const std::string folder = std::string(windows) + "/Fonts/";
    // Japanese first when chosen (its own forms of the shared ideographs),
    // then Chinese, then Arabic (Presentation Forms-B)
    std::vector<std::string> files;
    if (g_current == "ja") files = {"YuGothM.ttc", "meiryo.ttc", "msyh.ttc", "arial.ttf"};
    else files = {"msyh.ttc", "YuGothM.ttc", "meiryo.ttc", "arial.ttf"};
    for (const auto &name : files) {
        const std::string path = folder + name;
        if (!std::ifstream(path, std::ios::binary)) continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        atlas->AddFontFromFileTTF(path.c_str(), 0.0f, &cfg);
    }
#endif
}

std::string shape_arabic(const std::string &logical) {
    std::string out;
    size_t start = 0;
    while (true) {
        const size_t end = logical.find('\n', start);
        out += shape_line(decode(logical.substr(start, end == std::string::npos ? std::string::npos : end - start)));
        if (end == std::string::npos) break;
        out += '\n';
        start = end + 1;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parse_po(const std::string &text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string id, str, line;
    int field = 0; // 1 msgid, 2 msgstr
    auto flush = [&] {
        if (field == 2) out.emplace_back(id, str);
        id.clear(), str.clear(), field = 0;
    };
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line[p] == '#') continue;
        if (line.compare(p, 6, "msgid ") == 0) {
            flush();
            field = 1;
            unescape_into(line.substr(p + 6), id);
        } else if (line.compare(p, 7, "msgstr ") == 0) {
            field = 2;
            unescape_into(line.substr(p + 7), str);
        } else if (line[p] == '"') {
            unescape_into(line.substr(p), field == 1 ? id : str);
        }
    }
    flush();
    return out;
}

} // namespace app::i18n
