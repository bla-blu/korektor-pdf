#pragma once
// Model dokumentu Korektora PDF.
//
// Strony pochodzą z jednego lub wielu plików źródłowych (scalanie). Edycje użytkownika są
// trzymane jako obiekty modelu (Obj) — dzięki temu wszystko da się przesuwać, zmieniać
// i cofać aż do zapisu.
//
// Dwa rodzaje edycji:
//  * nakładki (pióro, zakreślacz, obraz) — rysowane nad stroną,
//  * zmiany treści (tekst — nowy albo poprawiony istniejący, oraz zamazanie z usunięciem
//    treści) — nakładane na żywą kopię strony w PDFium. Podgląd na ekranie pochodzi z tego
//    samego silnika, który zapisuje plik, więc to, co widać, jest tym, co trafi do pliku.
//
// Układ współrzędnych obiektów ("bazowy"): punkty PDF (1/72 cala), początek w lewym górnym
// rogu strony widzianej z jej własnym /Rotate, oś Y w dół. Obrót dodany w edytorze
// (Page::extraRot) nie zmienia współrzędnych obiektów — obracają się razem ze stroną.

#include "Fonts.h"
#include "ImageLoad.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kpdf {

struct Vec2 { float x = 0, y = 0; };
struct Color { float r = 0, g = 0, b = 0, a = 1; };
struct Rect { float x = 0, y = 0, w = 0, h = 0; };

// Image — obraz wstawiony w edytorze; PageImage — obraz z pliku (przesunięty, obrócony, usunięty, podmieniony).
// PageShape — kształt z pliku (linia, prostokąt, krzywa): przesunięty, przeskalowany, przekolorowany, usunięty.
// Shape — kształt wektorowy dodany w edytorze (np. kopia kształtu z pliku).
enum class ObjType { Text, Ink, Highlight, Whiteout, Image, PageImage, PageShape, Shape };

// Czcionka tekstu: oryginalna z pliku (gdy pozwalają na to znaki) albo jedna z osadzonych.
struct FontChoice {
    int family = FamilySans;
    bool bold = false, italic = false;
    int origObj = -1;        // indeks oryginalnego obiektu tekstowego, którego czcionkę używamy
    std::string okChars;     // UTF-8: znaki, które ta oryginalna czcionka na pewno zawiera
    float fit = 1.f;         // poziome skalowanie czcionki zastępczej, by pasowała szerokością
    bool operator==(const FontChoice& o) const {
        return family == o.family && bold == o.bold && italic == o.italic && origObj == o.origObj &&
               okChars == o.okChars && fit == o.fit;
    }
};

// Wygląd fragmentu tekstu.
struct TextStyle {
    FontChoice font;
    float size = 12;          // rozmiar czcionki w jednostkach tekstu
    Color color{0, 0, 0, 1};
    int renderMode = 0;       // tryb renderowania tekstu PDF (0 = wypełnienie)
    bool operator==(const TextStyle& o) const {
        return font == o.font && size == o.size && color.r == o.color.r && color.g == o.color.g &&
               color.b == o.color.b && color.a == o.color.a && renderMode == o.renderMode;
    }
};

enum class Align { Left = 0, Center = 1, Right = 2, Justify = 3 };

struct Obj {
    ObjType type = ObjType::Text;
    float x = 0, y = 0, w = 0, h = 0; // prostokąt; dla tekstu (x,y) = początek linii bazowej pierwszego wiersza
    float size = 12;                  // grubość linii (pióro), w pt
    Color color;                      // kolor pióra i zakreślacza
    std::string text;                 // UTF-8; '\n' kończy akapit
    std::vector<Vec2> pts;            // punkty pióra
    ImagePtr image;

    // --- tylko tekst
    Vec2 adv{1, 0};                   // kierunek linii bazowej na jednostkę tekstu (z poziomym skalowaniem)
    Vec2 up{0, -1};                   // kierunek "w górę" na jednostkę tekstu
    std::vector<TextStyle> styles;    // style użyte w tekście (co najmniej jeden)
    std::vector<uint16_t> charStyle;  // styl każdego znaku (indeks w `styles`)
    float boxWidth = 0;               // szerokość akapitu do zawijania (0 = bez zawijania)
    Align align = Align::Left;
    float lineStep = 0;               // odstęp linii bazowych (0 = 1,2 × największy rozmiar w wierszu)
    float firstIndent = 0;            // wcięcie pierwszego wiersza akapitu
    std::vector<int> srcObjs;         // zastąpione oryginalne obiekty strony (indeksy w pierwotnej kolejności)
    Rect origBox;                     // obszar zajmowany przez zastąpiony tekst albo obraz z pliku

    // --- obrazy (Image i PageImage)
    int imgRot = 0;                   // obrót o 90° w prawo (0..3) względem pierwotnego położenia
    bool hidden = false;              // PageImage: obraz usunięty ze strony
    std::vector<int> childPath;       // PageImage: indeksy obiektu wewnątrz osadzonych grafik (pusty = obiekt strony)
    int pixelW = 0, pixelH = 0;       // PageImage: rozmiar obrazu w pikselach (informacyjnie)

    // --- kształty (PageShape, Shape). Geometria w `pts` (współrzędne bazowe) odpowiada prostokątowi
    // origBox; obecny prostokąt (x, y, w, h) wyznacza przesunięcie i skalę. Kolor wypełnienia: `color`,
    // grubość linii w pt: `size`.
    std::vector<uint8_t> segs;        // rodzaj punktu: 0 początek, 1 odcinek, 2 krzywa (3 kolejne punkty); +4 zamyka figurę
    Color stroke{0, 0, 0, 1};         // kolor linii (konturu)
    int fillMode = 0;                 // 0 bez wypełnienia, 1 parzysto-nieparzyste, 2 niezerowe
    bool strokeOn = false;            // czy kształt ma kontur
    int lineCap = 0, lineJoin = 0;
    std::vector<float> dash;          // przerywana linia (pt); pusta = ciągła
    float dashPhase = 0;
    int shapeEdits = 0;               // PageShape: co zmieniono w wyglądzie (kShapeFill | kShapeStroke | kShapeWidth)

    // Styl znaku o danym indeksie (w znakach, nie bajtach).
    const TextStyle& styleAt(size_t ch) const;
    // Uzgadnia liczbę stylów znaków z długością tekstu i usuwa nieużywane style.
    void normalizeStyles();

    // Skrót wyglądu tekstu (treść, style, położenie, ramka). Akapit z pliku, który tylko
    // zaznaczono, ma contentHash() == pickHash — wtedy strona zostaje dokładnie taka jak w pliku.
    uint64_t contentHash() const;
    uint64_t pickHash = 0;
    bool untouched() const { return pickHash != 0 && contentHash() == pickHash; }

    // Kształt z pliku tylko zaznaczony (bez żadnej zmiany).
    bool shapeIdle() const;
};

constexpr int kShapeFill = 1, kShapeStroke = 2, kShapeWidth = 4;

// Punkt geometrii kształtu (z `pts`) po przesunięciu i skali do obecnego prostokąta.
Vec2 shapePoint(const Obj& o, Vec2 p);

// Odnośnik na stronie: adres (www, e-mail) albo miejsce w dokumencie.
struct Link {
    Rect box;                 // obszar klikalny we współrzędnych bazowych strony
    std::string uri;          // adres zewnętrzny (UTF-8); pusty = odnośnik wewnętrzny
    int targetPage = -1;      // odnośnik wewnętrzny: id strony docelowej (-1 = brak / usunięta)
    bool hasTargetY = false;  // czy znane jest miejsce na stronie docelowej
    float targetY = 0;        // górna krawędź widoku na stronie docelowej (współrzędne bazowe)
    bool internal() const { return uri.empty(); }
};

struct Page {
    int id = 0;          // stały identyfikator (nie zmienia się przy przestawianiu)
    int source = 0;      // indeks pliku źródłowego
    int index = 0;       // numer strony w pliku źródłowym (od 0)
    int baseRot = 0;     // własny obrót strony w pliku: 0..3 (×90° w prawo)
    int extraRot = 0;    // obrót dodany w edytorze: 0..3
    float width = 0;     // wymiary strony z baseRot, w pt
    float height = 0;
    float cropX = 0, cropY = 0; // lewy dolny róg widocznego obszaru strony w przestrzeni PDF
    std::vector<Obj> objs;
    std::vector<Link> links;   // odnośniki www/e-mail i wewnętrzne (inne rodzaje zostają w pliku bez zmian)

    int totalRot() const { return (baseRot + extraRot) % 4; }
    float shownWidth() const { return extraRot % 2 ? height : width; }
    float shownHeight() const { return extraRot % 2 ? width : height; }
};

// Układ tekstu do rysowania kursora i zaznaczenia (współrzędne bazowe).
struct TextLayout {
    struct Line {
        int first = 0;                 // indeks pierwszego znaku wiersza
        std::vector<Vec2> carets;      // pozycje kursora przed każdym znakiem i za ostatnim
        Vec2 ascent, descent;          // wektory od linii bazowej do góry i do dołu wiersza
    };
    std::vector<Line> lines;
    std::vector<Vec2> extent;          // dodatkowe punkty granic (prawa krawędź ramki akapitu)
    std::vector<std::string> styleFont;     // nazwa czcionki użytej dla każdego stylu
    std::vector<bool> styleOriginal;        // czy styl używa czcionki z pliku
};

enum class LoadResult { Ok, NeedPassword, WrongPassword, Error };
enum class Pick { None, Ok, NotEditable };

class Document {
public:
    static void initLibrary();
    static void destroyLibrary();

    Document();
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    // Zamiana zawartości dwóch dokumentów (karty: dokument nieaktywnej karty czeka w schowku karty).
    void swap(Document& other) noexcept;

    LoadResult appendFile(const std::string& utf8Path, const std::string& password, std::string& err);
    LoadResult appendBytes(std::vector<uint8_t> bytes, const std::string& name,
                           const std::string& password, std::string& err);
    int insertBlankPage(int at = -1, float w = 595.28f, float h = 841.89f);
    void clear();
    bool empty() const { return pages.empty(); }

    // Renderuje stronę w orientacji z ekranu (z extraRot), ze wszystkimi zmianami treści.
    // `scale` = piksele na punkt. `region` (x, y, w, h w pikselach) pozwala wyrenderować fragment.
    bool render(const Page& page, float scale, std::vector<uint8_t>& rgba, int& w, int& h,
                const int* region = nullptr) const;

    // Wersja zmian treści strony — zmienia się, gdy trzeba ją wyrenderować na nowo.
    uint64_t contentVersion(const Page& page) const;

    // Układ tekstu obiektu (kursor, zaznaczenie, granice) zgodny z tym, co narysuje PDFium.
    TextLayout layoutText(const Page& page, const Obj& obj) const;

    // Szuka akapitu tekstu pod punktem; przygotowuje obiekt, który go zastąpi
    // (tekst ze stylami znaków, szerokość, wyrównanie, interlinia, wcięcie).
    Pick pickText(const Page& page, Vec2 base, Obj& out, int& caretChar, std::string& why) const;

    // Szuka na stronie czcionki z pliku o danej rodzinie i odmianie (np. pogrubionej wersji
    // czcionki akapitu); gdy jest, ustawia ją w `fc`. Inaczej zostaje czcionka zastępcza.
    bool matchPageFont(const Page& page, FontChoice& fc) const;

    // Szuka obrazu z pliku pod punktem (najwyższego, jeszcze nie zmienionego) i przygotowuje
    // obiekt, który pozwoli go przesuwać, skalować, obracać, usunąć albo podmienić.
    // `z` (opcjonalnie) — kolejność rysowania znalezionego obiektu, do porównania z pickShape.
    bool pickImage(const Page& page, Vec2 base, Obj& out, std::vector<int>* z = nullptr) const;

    // Szuka kształtu z pliku (linii, prostokąta, krzywej) pod punktem, z tolerancją `tol` (pt).
    // Pomija niewidoczne ścieżki i tła zajmujące większość strony.
    bool pickShape(const Page& page, Vec2 base, float tol, Obj& out, std::vector<int>* z = nullptr) const;

    // Tekst strony (z bieżącymi zmianami) w prostokącie — np. napis pod odnośnikiem. Białe znaki
    // zwinięte do pojedynczych spacji, najwyżej `maxChars` znaków (dłuższy kończy się „…”).
    std::string textInRect(const Page& page, const Rect& base, size_t maxChars = 120) const;

    // Szybkie sprawdzenie, czy pod punktem jest tekst z pliku (bez przygotowywania akapitu).
    bool hasTextAt(const Page& page, Vec2 base) const;

    // Obraz z pliku tak, jak wygląda na stronie (z maską i obrotem z pliku), do skopiowania.
    // `rot` — obrót (×90° w prawo), który trzeba nadać kopii, by wyglądała jak oryginał.
    ImagePtr pageImageAppearance(const Page& page, const Obj& obj, int& rot) const;

    // Zapisuje obraz z pliku (PageImage) jako PNG — w oryginalnej rozdzielczości.
    bool exportImage(const Page& page, const Obj& obj, const std::string& utf8Path, std::string& err) const;

    // Czy tekst obiektu nachodzi na pozostały (niezmieniony) tekst strony.
    bool overlapsOtherText(const Page& page, const Obj& obj) const;

    bool saveToBytes(std::vector<uint8_t>& out, std::string& err) const;
    bool saveToFile(const std::string& utf8Path, std::string& err) const;

    Page* pageById(int id);
    int indexOfPage(int id) const;
    const std::string& sourceName(int source) const;

    std::vector<Page> pages;

private:
    struct Source;
    struct Live;
    Live& live(const Page& page) const;
    void dropLive(int pageId) const;

    std::vector<std::unique_ptr<Source>> sources_;
    mutable std::unordered_map<int, std::unique_ptr<Live>> live_;
    mutable int liveClock_ = 0;
    int nextPageId_ = 1;
};

// Przeliczanie współrzędnych bazowych <-> ekranowych strony (z extraRot), oba w pt.
Vec2 baseToShown(const Page& p, Vec2 b);
Vec2 shownToBase(const Page& p, Vec2 s);

// Narożniki obrazu w kolejności: lewy górny, prawy górny, prawy dolny, lewy dolny obrazu
// (po obrocie imgRot), we współrzędnych bazowych.
void imageCorners(const Obj& o, Vec2 out[4]);

// Granice tekstu z układu (współrzędne bazowe).
Rect layoutBounds(const TextLayout& l);

// Typowe formaty papieru (wymiary w mm, w orientacji pionowej: szerokość <= wysokość).
struct PaperSize {
    const char* name;
    const char* group;
    float wMm, hMm;
    bool landscape;     // zwykle używany poziomo (np. slajd, wizytówka)
};
extern const PaperSize kPaperSizes[];
extern const int kPaperSizeCount;

inline float mmToPt(float mm) { return mm * 72.f / 25.4f; }
inline float ptToMm(float pt) { return pt * 25.4f / 72.f; }
// Dopuszczalny rozmiar strony PDF: 3..14400 pt (ok. 1 mm .. 5 m).
constexpr float kMinPagePt = 3.f, kMaxPagePt = 14400.f;

// Nazwa formatu papieru ("A4", "Letter") albo wymiary w mm.
std::string paperName(float wPt, float hPt);

// Adres do zapisania w PDF: znaki spoza ASCII kodowane jako %XX (UTF-8); dopisuje https:// do adresów www
// i mailto: do adresów e-mail podanych bez schematu.
std::string normalizeUri(const std::string& utf8);
// Adres do pokazania: %XX z powrotem jako znaki UTF-8.
std::string displayUri(const std::string& uri);
// Odnośnik przykryty zamazaniem (co najmniej w 30%) — przy zapisie znika razem z treścią.
bool linkCovered(const Page& p, const Link& l);

// UTF-8 <-> kody znaków
std::u32string utf8To32(const std::string& s);
std::string utf32To8(const std::u32string& s);

} // namespace kpdf
