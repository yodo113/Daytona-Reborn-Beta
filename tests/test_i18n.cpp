// app/i18n: PO parsing and the Arabic shaping (joined forms, right to left).
#include "app/i18n.h"

#include <cstdio>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

int main() {
    using namespace app::i18n;
    const auto po = parse_po("# comment\nmsgid \"\"\nmsgstr \"header\"\n\nmsgid \"Plein \u00e9cran\"\nmsgstr \"Full\"\n"
                             "msgid \"A\\nB\"\nmsgstr \"\"\n\"C\\n\"\n\"D\"\n");
    CHECK(po.size() == 3);
    CHECK(po[2].first == "A\nB" && po[2].second == "C\nD");
    // French is the source: no translation, the same pointer
    const char *fr = "Plein écran";
    CHECK(tr(fr) == fr);
    // salam (sin, lam, alef, mim): sin initial, lam-alef ligature (final),
    // mim isolated; shown right to left: mim, lam-alef, sin
    CHECK(shape_arabic("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85") ==
          "\xEF\xBB\xA1" "\xEF\xBB\xBC" "\xEF\xBA\xB3");
    // a Latin run keeps its order inside the right-to-left line
    const std::string mixed = shape_arabic("\xD8\xA8 ABC");
    CHECK(mixed.find("ABC") != std::string::npos);
    const std::string formats = shape_arabic("العدد %d والنص %s والنسبة %.2f ثم %%");
    CHECK(formats.find("%d") != std::string::npos);
    CHECK(formats.find("%s") != std::string::npos);
    CHECK(formats.find("%.2f") != std::string::npos);
    CHECK(formats.find("%%") != std::string::npos);
    CHECK(shape_arabic("السرعة -12.50%").find("-12.50%") != std::string::npos);
    CHECK(format("Valeurs %d, %s, %.2f, %%.", 42, "Daytona", 2.75) == "Valeurs 42, Daytona, 2.75, %.");
    if (available("ar")) {
        set("ar");
        CHECK(std::string(logical("Les %d fichiers sont vérifiés.")).find("%d") != std::string::npos);
        CHECK(format("Les %d fichiers sont vérifiés.", 42) ==
              shape_arabic("تم التحقق من الملفات وعددها 42."));
        set("fr");
    }
    CHECK(languages().size() == 7);
    std::puts("i18n ok");
    return 0;
}
