#include "Icons.h"

#include <cstring>

namespace kpdf {

const EmbeddedFile* iconFile(const char* name) {
    for (int i = 0; i < kIconFileCount; ++i)
        if (std::strcmp(kIconFiles[i].name, name) == 0) return &kIconFiles[i];
    return nullptr;
}

} // namespace kpdf
