// The interface's words: every one in English and Spanish (French, German and Italian may fall back to
// English), placeholders filled.
#include "test.h"

#include "strings.h"

#include <cstdio>
#include <string>

using namespace studio;

TEST(every_word_in_english_and_spanish) {
    int missing = 0;
    for (const Language l : {Language::en, Language::es}) {
        for (int s = 0; s < static_cast<int>(S::count); ++s) {
            if (!translated(l, static_cast<S>(s))) {
                std::fprintf(stderr, "  no %s word for string %d\n", language_code(l), s);
                ++missing;
            }
        }
    }
    CHECK(missing == 0);
}

TEST(placeholders) {
    CHECK(fmt(Language::en, S::saved, {"mods/x.png"}) == "Saved: mods/x.png");
    CHECK(fmt(Language::es, S::size, {"16", "8"}) == "16 x 8");
    CHECK(fmt(Language::en, S::size, {"16"}) == "16 x ");  // a missing argument is left empty
}

TEST(language_codes) {
    CHECK(language_from_code("es", Language::en) == Language::es);
    CHECK(language_from_code("de-AT", Language::en) == Language::de);
    CHECK(language_from_code("pt", Language::en) == Language::en);
    CHECK(std::string(language_code(Language::it)) == "it");
}
