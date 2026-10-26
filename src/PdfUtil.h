#pragma once
// Wewnętrzne narzędzia rdzenia (nie są częścią interfejsu dla aplikacji).

#include "Document.h"

#include <fpdf_edit.h>
#include <fpdfview.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kpdf::detail {

// Przekształcenie afiniczne 2D, jak macierz PDF: x' = a*x + c*y + e, y' = b*x + d*y + f.
struct Xf {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Vec2 lin(Vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
    Xf inverse() const;
    Xf then(const Xf& next) const;   // najpierw this, potem next
    FS_MATRIX fs() const { return {a, b, c, d, e, f}; }
    static Xf from(const FS_MATRIX& m) { return {m.a, m.b, m.c, m.d, m.e, m.f}; }
};

// Współrzędne bazowe strony (oś Y w dół) -> przestrzeń PDF strony źródłowej.
Xf baseToPdf(const Page& p);

std::vector<unsigned short> toUtf16z(const std::u32string& s);

// Czcionki osadzone, ładowane do danego dokumentu PDFium przy pierwszym użyciu.
struct FontCache {
    FPDF_DOCUMENT doc = nullptr;
    FPDF_FONT fonts[12] = {};
    FPDF_FONT get(int family, bool bold, bool italic);
    void close();
    ~FontCache() { close(); }
};

// Stan strony w trakcie nakładania zmian: obiekty w pierwotnej kolejności i to, co zmieniono.
struct PageEdit {
    FPDF_DOCUMENT doc = nullptr;
    FPDF_PAGE page = nullptr;
    std::vector<FPDF_PAGEOBJECT> pristine;           // obiekty najwyższego poziomu przed zmianami
    std::vector<FPDF_PAGEOBJECT> removed;            // zdjęte ze strony (zwalniane na końcu)
    std::unordered_set<FPDF_PAGEOBJECT> added;       // dodane przez edytor
    FontCache* fonts = nullptr;
    // Zmiany oryginalnych obiektów w miejscu (zapis odtwarza je z oryginalnych bajtów — ContentSplice):
    std::unordered_map<FPDF_PAGEOBJECT, Xf> moved;   // przesunięte/przeskalowane: przekształcenie w przestrzeni strony
    std::unordered_map<FPDF_PAGEOBJECT, int> styled; // kształty ze zmienionym stylem: bity kStyled*
    std::unordered_set<FPDF_PAGEOBJECT> touchedForms;   // osadzone grafiki, z których usunięto obiekt
    std::unordered_map<FPDF_PAGEOBJECT, FPDF_PAGEOBJECT> replaced;   // nowy obraz -> zastąpiony (zamazanie, podmiana)

    void remove(FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT parentForm = nullptr);
    void insertAt(FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT before);   // przed obiektem `before` (albo na końcu)
    int indexOf(FPDF_PAGEOBJECT obj) const;
    void destroyRemoved();
};

enum : int { kStyledWidth = 1, kStyledFill = 2, kStyledStroke = 4, kStyledMode = 8 };

// Czcionka wybrana dla jednego stylu tekstu.
struct StyleFont {
    FPDF_FONT font = nullptr;
    float hscale = 1;        // poziome skalowanie (dopasowanie czcionki zastępczej)
    bool original = false;   // czcionka z pliku
    float spaceW = 0;        // szerokość spacji (jednostki tekstu, ze skalowaniem)
    float asc = 0, desc = 0; // wysokość nad i pod linią bazową (desc < 0)
};

// Wiersz po zawinięciu akapitu.
struct LaidLine {
    int first = 0, count = 0;     // znaki wiersza (bez znaku końca akapitu)
    float y = 0;                  // przesunięcie linii bazowej w dół od pierwszego wiersza (jednostki tekstu)
    float asc = 0, desc = 0;
    std::vector<float> caret;     // pozycje kursora wzdłuż wiersza: count + 1 wartości
};

struct Laid {
    std::u32string text;
    std::vector<LaidLine> lines;
    std::vector<StyleFont> fonts; // dla każdego stylu obiektu
};

// Układ akapitu — wspólny dla podglądu, kursora i zapisu.
Laid layoutObj(const PageEdit& pe, const Obj& o);

// Nakłada zmiany treści strony (tekst, zamazania) — wspólne dla podglądu i zapisu.
void applyContentEdits(PageEdit& pe, const Page& p);

// Przenosi zawartość osadzonej grafiki (Form XObject) na stronę (jako zwykłe obiekty strony).
void flattenForm(PageEdit& pe, FPDF_PAGEOBJECT form, int index);

// Zmiany obrazów z pliku (PageImage): przesunięcie, skala, obrót, usunięcie, podmiana.
void applyImageEdits(PageEdit& pe, const Page& p);

// Usuwa treść spod prostokąta (przestrzeń PDF: l, b, r, t) i zakrywa go na biało.
void redactArea(PageEdit& pe, float l, float b, float r, float t);

// Znaki, które czcionka na pewno poprawnie zapisze (zebrane z tekstu strony).
std::u32string observedChars(FPDF_TEXTPAGE tp, FPDF_FONT font, bool embedded);

bool charsOk(const std::u32string& text, const std::u32string& ok);

void guessStyle(FPDF_FONT font, int& family, bool& bold, bool& italic);

} // namespace kpdf::detail
