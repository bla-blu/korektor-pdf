#pragma once
#include <cstddef>

namespace kpdf {

// Plik osadzony w programie (zamieniony na tablicę podczas kompilacji przez korektor_embed).
struct EmbeddedFile {
    const char* name;
    const unsigned char* data;
    std::size_t size;
};
using FontFile = EmbeddedFile;   // czcionki TTF

extern const FontFile kFontFiles[];
extern const int kFontFileCount;

enum FontFamily { FamilySans = 0, FamilySerif = 1, FamilyMono = 2 };

// Rodzina DejaVu w danej odmianie (zawsze zwraca poprawny plik).
const FontFile& fontFile(int family, bool bold, bool italic);
// Czcionka interfejsu.
const FontFile& uiFont();

} // namespace kpdf
