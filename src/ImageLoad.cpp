#include "ImageLoad.h"

#include <atomic>
#include <filesystem>
#include <fstream>

// stb to kod zewnętrzny: bez ostrzeżeń tylko na czas jego dołączenia
#if defined(_MSC_VER)
#  pragma warning(push, 0)
#  pragma warning(disable : 4244 4267 4456 4457 4996)
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#  pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

namespace kpdf {

namespace fs = std::filesystem;

static fs::path u8path(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

bool readFileBytes(const std::string& utf8Path, std::vector<uint8_t>& out) {
    std::ifstream f(u8path(utf8Path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize(size_t(n));
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return bool(f);
}

ImagePtr makeImage(int w, int h, std::vector<uint8_t> rgba) {
    static std::atomic<uint64_t> counter{1};
    auto img = std::make_shared<ImageData>();
    img->width = w;
    img->height = h;
    img->rgba = std::move(rgba);
    img->uid = counter++;
    return img;
}

ImagePtr loadImageFile(const std::string& utf8Path, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(utf8Path, bytes)) { err = "Nie można odczytać pliku."; return nullptr; }
    return decodeImage(bytes.data(), bytes.size(), err);
}

ImagePtr decodeImage(const uint8_t* data, size_t size, std::string& err) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, int(size), &w, &h, &comp, 4);
    if (!px) { err = std::string("Nieobsługiwany lub uszkodzony obraz (") + stbi_failure_reason() + ")."; return nullptr; }
    std::vector<uint8_t> rgba(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return makeImage(w, h, std::move(rgba));
}

static void writeToVector(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

bool writePng(const std::string& utf8Path, int w, int h, const uint8_t* rgba) {
    std::vector<uint8_t> buf;
    if (!stbi_write_png_to_func(writeToVector, &buf, w, h, 4, rgba, w * 4)) return false;
    std::ofstream f(u8path(utf8Path), std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
    return bool(f);
}

} // namespace kpdf
