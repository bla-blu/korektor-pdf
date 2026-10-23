#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kpdf {

// Obraz w pamięci: piksele RGBA 8 bit, wiersze od góry.
struct ImageData {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    uint64_t uid = 0; // unikalny identyfikator (np. do cache tekstur)
};

using ImagePtr = std::shared_ptr<const ImageData>;

// Wczytuje PNG, JPG, BMP, GIF, TGA. Zwraca nullptr i opis błędu w err.
ImagePtr loadImageFile(const std::string& utf8Path, std::string& err);
ImagePtr decodeImage(const uint8_t* data, size_t size, std::string& err);
ImagePtr makeImage(int w, int h, std::vector<uint8_t> rgba);

// Zapis PNG (używany przez test i narzędzia diagnostyczne).
bool writePng(const std::string& utf8Path, int w, int h, const uint8_t* rgba);

// Czyta cały plik do pamięci (ścieżka w UTF-8, działa też na Windows).
bool readFileBytes(const std::string& utf8Path, std::vector<uint8_t>& out);

} // namespace kpdf
