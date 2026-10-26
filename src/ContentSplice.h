#pragma once
// Zapis strony z zachowaniem oryginalnej treści.
//
// PDFium po każdej zmianie na stronie przepisuje cały jej strumień treści, a jego generator zapisuje
// tylko kolory RGB i szarości (CMYK, ICC, kolory dodatkowe stają się czarne), gubi maski przezroczystości
// i nadruk ze stanów graficznych, odstępy i kerning tekstu, obrazy wbudowane i cieniowania.
// Dlatego przy zapisie składamy treść strony sami: obiekty niezmienione (także tylko przesunięte albo
// ze zmienionym stylem) odtwarzamy z oryginalnych bajtów z pełnym stanem graficznym, a z wyniku PDFium
// bierzemy wyłącznie obiekty nowe i zmienione w inny sposób (oznaczone znacznikami treści).

#include "PdfUtil.h"

#include <fpdf_edit.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kpdf::detail {

// Obiekt strony po zmianach — w kolejności rysowania.
struct SpliceItem {
    int orig = -1;           // indeks oryginalnego obiektu (pierwotna kolejność) albo -1 dla nowego
    std::vector<int> path;   // obiekt z rozłożonej osadzonej grafiki: [indeks grafiki na stronie, indeks w grafice, …]
    bool replay = false;     // true: odtworzyć z oryginalnych bajtów; false: wziąć zapis PDFium (po znaczniku)
    std::string mark;        // replay == false: znacznik treści, którym oznaczono obiekt przed generowaniem
    bool ctx = false;        // replay == false: zapis PDFium w kontekście oryginału `orig`/`path` (zastąpiony obraz)
    bool moved = false;      // oryginał przesunięty/przeskalowany: dodatkowe przekształcenie w przestrzeni strony
    Xf move;
    // kształt z pliku ze zmienionym stylem: nadpisania (wartości z obiektu PDFium po zmianach)
    bool ovWidth = false, ovFill = false, ovStroke = false, ovMode = false;
    float width = 1;
    uint8_t fill[3] = {0, 0, 0}, stroke[3] = {0, 0, 0};
    int fillMode = 0;        // FPDF_FILLMODE_*
    bool strokeOn = false;
};

struct SplicePlan {
    int page = -1;                         // indeks strony w dokumencie
    std::vector<SpliceItem> items;
};

class Splicer {
public:
    Splicer();
    ~Splicer();
    Splicer(const Splicer&) = delete;
    Splicer& operator=(const Splicer&) = delete;

    // Plik zapisany przez PDFium przed nałożeniem zmian (te same numery obiektów co zapis po zmianach).
    bool load(std::vector<uint8_t> origBytes);

    // Interpretuje oryginalną treść strony i sprawdza, czy jej obiekty zgadzają się z obiektami PDFium
    // (rodzaj i położenie). Wywoływać przed zmianami na stronie. `why` — powód odmowy (diagnostyka).
    bool prepare(int page, FPDF_PAGE pg, std::string* why = nullptr);

    // Obiekt, który pochodzi z wnętrza osadzonej grafiki (po jej rozłożeniu na stronę): ścieżka do niego,
    // jeśli treść tej grafiki (i grafik nadrzędnych) zgadza się z obiektami PDFium. Wywoływać przed
    // wygenerowaniem treści strony.
    bool childPath(int page, FPDF_PAGEOBJECT obj, std::vector<int>& path);

    void setPlan(SplicePlan plan);

    // Podmienia treść przygotowanych stron w pliku zapisanym przez PDFium po zmianach (dopisuje
    // aktualizację przyrostową). Zwraca liczbę stron, których treść złożono z oryginału.
    int apply(std::vector<uint8_t>& saved, std::string* diag = nullptr);

    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

} // namespace kpdf::detail
