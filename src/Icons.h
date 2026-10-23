#pragma once
// Ikony paska narzędzi: pliki PNG 128×128 z katalogu assets/icons osadzone w programie.
#include "Fonts.h"   // EmbeddedFile

namespace kpdf {

extern const EmbeddedFile kIconFiles[];
extern const int kIconFileCount;

// Ikona o danej nazwie (np. "zapisz") albo nullptr.
const EmbeddedFile* iconFile(const char* name);

} // namespace kpdf
