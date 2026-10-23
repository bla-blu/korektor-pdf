#pragma once
// Ustawienia programu zapamiętywane między uruchomieniami (ostatnie pliki, widok).
// Plik tekstowy „klucz=wartość” w katalogu ustawień użytkownika:
//   Windows: %APPDATA%\KorektorPDF\ustawienia.txt
//   macOS:   ~/Library/Application Support/KorektorPDF/ustawienia.txt
//   Linux:   $XDG_CONFIG_HOME/korektor-pdf/ustawienia.txt (domyślnie ~/.config/...)

#include <string>
#include <vector>

namespace kpdf {

struct Settings {
    std::vector<std::string> recent;   // ostatnio otwierane pliki (UTF-8), najnowszy pierwszy
    bool showThumbs = true;
    bool thumbsAutoHide = false;       // panel miniatur wysuwa się po najechaniu na lewą krawędź
    bool showProps = true;
    bool showLinks = true;             // obramowanie odnośników na stronie
    bool iconLabels = false;           // podpisy przy ikonach paska narzędzi

    void load();
    void save() const;
    void addRecent(const std::string& utf8Path);
    void removeRecent(const std::string& utf8Path);
    static std::string path();          // pełna ścieżka pliku ustawień (UTF-8), pusta gdy nieznana
};

} // namespace kpdf
