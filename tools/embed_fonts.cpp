// Narzędzie budowania: zamienia pliki (czcionki, ikony) na tablice C++ osadzane w programie.
// Użycie: korektor_embed wyjście.cpp [--decl nagłówek.h TABLICA LICZNIK] NAZWA1 plik1 [NAZWA2 plik2 ...]
// Bez --decl: czcionki (Fonts.h, kFontFiles, kFontFileCount).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string header = "Fonts.h", table = "kFontFiles", count = "kFontFileCount";
    int first = 2;
    if (argc > 5 && std::string(argv[2]) == "--decl") {
        header = argv[3];
        table = argv[4];
        count = argv[5];
        first = 6;
    }
    if (argc < first + 2 || (argc - first) % 2) {
        std::fprintf(stderr, "użycie: %s wyjście.cpp [--decl nagłówek.h TABLICA LICZNIK] NAZWA plik ...\n", argv[0]);
        return 1;
    }
    std::string out = "// Wygenerowane podczas kompilacji przez korektor_embed — nie edytować.\n"
                      "#include \"" + header + "\"\n\nnamespace {\n";
    std::vector<std::pair<std::string, size_t>> entries;
    for (int i = first; i + 1 < argc; i += 2) {
        std::ifstream f(argv[i + 1], std::ios::binary);
        if (!f) { std::fprintf(stderr, "nie można otworzyć %s\n", argv[i + 1]); return 1; }
        std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const std::string id = "f" + std::to_string(entries.size());
        out += "const unsigned char " + id + "[] = {\n";
        char buf[8];
        for (size_t k = 0; k < data.size(); ++k) {
            std::snprintf(buf, sizeof buf, "%u,", unsigned(data[k]));
            out += buf;
            if (k % 40 == 39) out += '\n';
        }
        out += "};\n";
        entries.push_back({argv[i], data.size()});
    }
    out += "} // namespace\n\nnamespace kpdf {\nconst EmbeddedFile " + table + "[] = {\n";
    for (size_t i = 0; i < entries.size(); ++i)
        out += "    {\"" + entries[i].first + "\", f" + std::to_string(i) + ", " + std::to_string(entries[i].second) + "},\n";
    out += "};\nconst int " + count + " = " + std::to_string(entries.size()) + ";\n} // namespace kpdf\n";

    // zapis tylko przy zmianie treści, żeby nie wymuszać zbędnej rekompilacji
    std::ifstream old(argv[1], std::ios::binary);
    if (old) {
        std::string prev((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
        if (prev == out) return 0;
    }
    // katalog wyjściowy zakłada samo narzędzie (Ninja robi to za nas, Makefile i Xcode — nie)
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(argv[1]).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    std::ofstream o(argv[1], std::ios::binary);
    o << out;
    o.close();
    if (!o) { std::fprintf(stderr, "nie można zapisać %s\n", argv[1]); return 1; }
    return 0;
}
