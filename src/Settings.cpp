#include "Settings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#  include <cwchar>
#endif

namespace kpdf {

namespace fs = std::filesystem;

static fs::path configDir() {
#if defined(_WIN32)
    if (const wchar_t* app = _wgetenv(L"APPDATA")) return fs::path(app) / L"KorektorPDF";
    return {};
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) return fs::path(home) / "Library" / "Application Support" / "KorektorPDF";
    return {};
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "korektor-pdf";
    if (const char* home = std::getenv("HOME")) return fs::path(home) / ".config" / "korektor-pdf";
    return {};
#endif
}

std::string Settings::path() {
    const fs::path d = configDir();
    if (d.empty()) return {};
    return (d / "ustawienia.txt").u8string();
}

void Settings::load() {
    const fs::path d = configDir();
    if (d.empty()) return;
    std::ifstream f(d / "ustawienia.txt", std::ios::binary);
    std::string line;
    recent.clear();
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "ostatni" && !v.empty() && recent.size() < 10) recent.push_back(v);
        else if (k == "miniatury") showThumbs = v != "0";
        else if (k == "miniatury_auto") thumbsAutoHide = v != "0";
        else if (k == "wlasciwosci") showProps = v != "0";
        else if (k == "odnosniki") showLinks = v != "0";
        else if (k == "podpisy") iconLabels = v != "0";
    }
}

void Settings::save() const {
    const fs::path d = configDir();
    if (d.empty()) return;
    std::error_code ec;
    fs::create_directories(d, ec);
    std::ofstream f(d / "ustawienia.txt", std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << "# Korektor PDF — ustawienia\n";
    f << "miniatury=" << (showThumbs ? 1 : 0) << "\n";
    f << "miniatury_auto=" << (thumbsAutoHide ? 1 : 0) << "\n";
    f << "wlasciwosci=" << (showProps ? 1 : 0) << "\n";
    f << "odnosniki=" << (showLinks ? 1 : 0) << "\n";
    f << "podpisy=" << (iconLabels ? 1 : 0) << "\n";
    for (const auto& r : recent) f << "ostatni=" << r << "\n";
}

void Settings::addRecent(const std::string& p) {
    std::string abs = p;
    std::error_code ec;
    const fs::path a = fs::absolute(fs::u8path(p), ec);
    if (!ec) abs = a.u8string();
    removeRecent(abs);
    recent.insert(recent.begin(), abs);
    if (recent.size() > 10) recent.resize(10);
}

void Settings::removeRecent(const std::string& p) {
    recent.erase(std::remove(recent.begin(), recent.end(), p), recent.end());
}

} // namespace kpdf
