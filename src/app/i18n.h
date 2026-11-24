// Interface languages (23/11: English, French, Portuguese, Spanish,
// Chinese, Japanese, Arabic). The French text is the key: T("Plein écran")
// gives the current language's text from lang/<code>.po next to the game,
// else English's, else the French itself. A "##id" suffix (ImGui) is kept
// as is. Arabic is shaped (joined letter forms) and laid out right to left
// when the string is displayed, after formatting dynamic values.
#pragma once

#include <string>
#include <vector>

namespace app::i18n {

struct Language {
    const char *code; // "fr", "en", ...
    const char *name; // in its own script: "Français", "English", ...
};
const std::vector<Language> &languages();

// The language in use ("fr", "en", ...). set() loads its file (and
// English's, the fallback); an unknown code means French.
const std::string &current();
void set(const std::string &code);
// Whether `code` can be shown: French, or a lang/<code>.po next to the game.
bool available(const char *code);
// The system's preferred language among ours, else English (the first run).
std::string system_default();

// The text in the current language (stable pointer, valid until set()).
const char *tr(const char *french);
// Logical text for composing a larger message.
const char *logical(const char *french);
// Format the logical translation first, then shape the result for display.
std::string format(const char *french, ...);

// The fonts the languages need, merged into the default font: CJK and
// Arabic glyphs (Windows: the system's own fonts). Call once, before the
// font atlas is built.
void add_fonts();

// For tests: Arabic shaping and right-to-left order of one line (UTF-8).
std::string shape_arabic(const std::string &logical);
// For tests: parse a PO file's text into msgid -> msgstr pairs.
std::vector<std::pair<std::string, std::string>> parse_po(const std::string &text);

} // namespace app::i18n

#define TX(text) ::app::i18n::tr(text)
#define TXF(...) ::app::i18n::format(__VA_ARGS__)
#define TXL(text) ::app::i18n::logical(text)
