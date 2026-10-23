#include "Fonts.h"

#include <cstring>
#include <string>

namespace kpdf {

static const FontFile* find(const std::string& name) {
    for (int i = 0; i < kFontFileCount; ++i)
        if (name == kFontFiles[i].name) return &kFontFiles[i];
    return nullptr;
}

const FontFile& fontFile(int family, bool bold, bool italic) {
    std::string name = family == FamilySerif ? "DejaVuSerif" : family == FamilyMono ? "DejaVuSansMono" : "DejaVuSans";
    const char* slant = family == FamilySerif ? "Italic" : "Oblique";
    if (bold && italic) name += std::string("-Bold") + slant;
    else if (bold) name += "-Bold";
    else if (italic) name += std::string("-") + slant;
    if (const FontFile* f = find(name)) return *f;
    if (const FontFile* f = find("DejaVuSans")) return *f;
    return kFontFiles[0];
}

const FontFile& uiFont() { return fontFile(FamilySans, false, false); }

} // namespace kpdf
