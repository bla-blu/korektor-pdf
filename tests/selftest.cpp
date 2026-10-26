// Test rdzenia bez interfejsu.
//  1. Strony obrócone o 0/90/180/270°, wszystkie typy edycji, zapis i sprawdzenie położenia tekstu.
//  2. Poprawianie istniejącego tekstu: czcionka z pliku, czcionka zastępcza (polskie znaki), położenie.
//  3. Zamazywanie z usunięciem treści: tekst (częściowo i w całości, także wewnątrz grafiki
//     osadzonej), piksele obrazu, grafika wektorowa.
// Podglądy stron zapisuje jako PNG do wskazanego katalogu.

#include "Document.h"

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using namespace kpdf;

static int failures = 0;
#define CHECK(cond, ...)                                                                    \
    do {                                                                                    \
        const bool ok_ = (cond);                                                            \
        if (!ok_) ++failures;                                                               \
        std::printf(ok_ ? "ok:   " : "BŁĄD: ");                                             \
        std::printf(__VA_ARGS__);                                                           \
        std::printf("\n");                                                                  \
    } while (0)

struct VecWriter : FPDF_FILEWRITE { std::vector<uint8_t>* out; };
static int wb(FPDF_FILEWRITE* s, const void* d, unsigned long n) {
    auto* w = static_cast<VecWriter*>(s);
    w->out->insert(w->out->end(), (const uint8_t*)d, (const uint8_t*)d + n);
    return 1;
}
static std::vector<uint8_t> saveDoc(FPDF_DOCUMENT d) {
    std::vector<uint8_t> out;
    VecWriter w{};
    w.version = 1; w.WriteBlock = wb; w.out = &out;
    FPDF_SaveAsCopy(d, &w, 0);
    return out;
}

static std::string utf16ToUtf8(const unsigned short* w) {
    std::u32string s;
    for (size_t i = 0; w[i]; ++i) {
        char32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && w[i + 1]) c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
        s.push_back(c);
    }
    return utf32To8(s);
}

static FPDF_PAGEOBJECT addText(FPDF_DOCUMENT d, FPDF_PAGE p, FPDF_FONT f, float size, const char* utf8, float x, float y) {
    FPDF_PAGEOBJECT t = FPDFPageObj_CreateTextObj(d, f, size);
    std::u32string s = utf8To32(utf8);
    std::vector<unsigned short> w;
    for (char32_t c : s) w.push_back((unsigned short)c);
    w.push_back(0);
    FPDFText_SetText(t, reinterpret_cast<FPDF_WIDESTRING>(w.data()));
    FPDFPageObj_Transform(t, 1, 0, 0, 1, x, y);
    FPDFPage_InsertObject(p, t);
    return t;
}

// Tekst całej strony zapisanego pliku.
static std::string pageText(FPDF_PAGE pg) {
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
    const int n = FPDFText_CountChars(tp);
    std::vector<unsigned short> buf(size_t(n) + 1);
    FPDFText_GetText(tp, 0, n, buf.data());
    FPDFText_ClosePage(tp);
    return utf16ToUtf8(buf.data());
}

// Pierwsze położenie fragmentu tekstu (lewy dół pierwszego znaku), w przestrzeni PDF.
static bool findText(FPDF_PAGE pg, const std::string& needle, double& x, double& y) {
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
    const int n = FPDFText_CountChars(tp);
    std::vector<unsigned short> buf(size_t(n) + 1);
    FPDFText_GetText(tp, 0, n, buf.data());
    const std::u32string hay = utf8To32(utf16ToUtf8(buf.data())), nd = utf8To32(needle);
    const size_t pos = hay.find(nd);
    bool ok = false;
    if (pos != std::u32string::npos) ok = FPDFText_GetCharOrigin(tp, int(pos), &x, &y);
    FPDFText_ClosePage(tp);
    return ok;
}

static std::string fontOfText(FPDF_PAGE pg, const std::string& needle) {
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
    const int n = FPDFText_CountChars(tp);
    std::vector<unsigned short> buf(size_t(n) + 1);
    FPDFText_GetText(tp, 0, n, buf.data());
    const size_t pos = utf8To32(utf16ToUtf8(buf.data())).find(utf8To32(needle));
    std::string name;
    if (pos != std::u32string::npos) {
        if (FPDF_PAGEOBJECT o = FPDFText_GetTextObject(tp, int(pos))) {
            char b[128] = {};
            FPDFFont_GetBaseFontName(FPDFTextObj_GetFont(o), b, sizeof b);
            name = b;
        }
    }
    FPDFText_ClosePage(tp);
    return name;
}

static void writePreview(const Document& d, const Page& p, const std::string& path) {
    std::vector<uint8_t> rgba;
    int w, h;
    if (d.render(p, 1.0f, rgba, w, h)) writePng(path, w, h, rgba.data());
}

static std::vector<uint8_t> reopenAndCheck(const std::string& path, const std::function<void(FPDF_DOCUMENT)>& fn) {
    std::vector<uint8_t> bytes;
    readFileBytes(path, bytes);
    FPDF_DOCUMENT out = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
    CHECK(out != nullptr, "ponowne otwarcie %s (%zu bajtów)", path.c_str(), bytes.size());
    if (out) { fn(out); FPDF_CloseDocument(out); }
    return bytes;
}

// =====================================================================================

static void testRotationsAndOverlays(const std::string& outDir) {
    std::printf("\n== obroty stron i nakładki\n");
    FPDF_DOCUMENT d = FPDF_CreateNewDocument();
    for (int r = 0; r < 4; ++r) {
        FPDF_PAGE p = FPDFPage_New(d, r, 595.28, 841.89);
        FPDF_PAGEOBJECT frame = FPDFPageObj_CreateNewRect(20, 20, 555.28f, 801.89f);
        FPDFPageObj_SetStrokeColor(frame, 120, 120, 120, 255);
        FPDFPageObj_SetStrokeWidth(frame, 1);
        FPDFPath_SetDrawMode(frame, FPDF_FILLMODE_NONE, 1);
        FPDFPage_InsertObject(p, frame);
        FPDFPage_GenerateContent(p);
        FPDFPage_SetRotation(p, r);
        FPDF_ClosePage(p);
    }
    std::vector<uint8_t> src = saveDoc(d);
    FPDF_CloseDocument(d);

    Document doc;
    std::string err;
    CHECK(doc.appendBytes(src, "źródło.pdf", "", err) == LoadResult::Ok, "wczytanie PDF z obróconymi stronami");
    CHECK(doc.pages.size() == 4, "4 strony po wczytaniu");
    for (int r = 0; r < 4; ++r) CHECK(doc.pages[r].baseRot == r, "strona %d ma /Rotate %d", r + 1, r * 90);
    const int blankId = doc.insertBlankPage(1);
    CHECK(doc.pages.size() == 5 && doc.pages[1].id == blankId, "wstawienie pustej strony na pozycji 2");

    std::vector<uint8_t> px(64 * 48 * 4);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x) {
            uint8_t* q = &px[(y * 64 + x) * 4];
            q[0] = uint8_t(x * 4); q[1] = 120; q[2] = uint8_t(255 - x * 4); q[3] = x < 32 ? 255 : 90;
        }
    ImagePtr img = makeImage(64, 48, std::move(px));
    const std::string phrase = "Zażółć gęślą jaźń";
    for (auto& p : doc.pages) {
        Obj t; t.type = ObjType::Text; t.x = 100; t.y = 140;
        TextStyle st; st.size = 18; st.color = {0.77f, 0.2f, 0.17f, 1};
        t.styles = {st};
        t.text = phrase + "\nlinia 2";
        t.normalizeStyles();
        p.objs.push_back(t);
        Obj hl; hl.type = ObjType::Highlight; hl.x = 96; hl.y = 118; hl.w = 200; hl.h = 28; hl.color = {1, 0.83f, 0, 1};
        p.objs.push_back(hl);
        Obj wh; wh.type = ObjType::Whiteout; wh.x = 15; wh.y = 300; wh.w = 120; wh.h = 40;
        p.objs.push_back(wh);
        Obj ink; ink.type = ObjType::Ink; ink.size = 2.5f; ink.color = {0.12f, 0.31f, 0.82f, 1};
        for (int k = 0; k <= 40; ++k) ink.pts.push_back({100.f + k * 5, 220.f + 12 * std::sin(k * 0.4f)});
        p.objs.push_back(ink);
        Obj im; im.type = ObjType::Image; im.x = 330; im.y = 60; im.w = 128; im.h = 96; im.image = img;
        p.objs.push_back(im);
    }
    doc.pages[0].extraRot = 1;
    std::swap(doc.pages[3], doc.pages[4]);
    std::vector<int> expectedRot;
    for (auto& p : doc.pages) expectedRot.push_back(p.totalRot());

    const std::string outPath = outDir + "/obroty.pdf";
    CHECK(doc.saveToFile(outPath, err), "zapis %s", err.c_str());
    reopenAndCheck(outPath, [&](FPDF_DOCUMENT out) {
        CHECK(FPDF_GetPageCount(out) == 5, "wynik ma 5 stron");
        for (int i = 0; i < FPDF_GetPageCount(out); ++i) {
            FPDF_PAGE pg = FPDF_LoadPage(out, i);
            CHECK(FPDFPage_GetRotation(pg) == expectedRot[i], "strona %d: obrót %d°", i + 1, expectedRot[i] * 90);
            CHECK(pageText(pg).find(phrase) != std::string::npos, "strona %d: tekst z polskimi znakami da się wyszukać", i + 1);
            // położenie litery Z w układzie bazowym strony
            FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
            double l, r, b, t;
            const Page& model = doc.pages[i];
            if (FPDFText_GetCharBox(tp, int(pageText(pg).find('Z')), &l, &r, &b, &t)) {
                int dx1, dy1, dx2, dy2;
                const int W = int(std::lround(model.shownWidth())), H = int(std::lround(model.shownHeight()));
                FPDF_PageToDevice(pg, 0, 0, W, H, 0, l, t, &dx1, &dy1);
                FPDF_PageToDevice(pg, 0, 0, W, H, 0, r, b, &dx2, &dy2);
                Vec2 a = shownToBase(model, {float(dx1), float(dy1)}), c = shownToBase(model, {float(dx2), float(dy2)});
                const float left = std::min(a.x, c.x), top = std::min(a.y, c.y), bottom = std::max(a.y, c.y);
                CHECK(std::fabs(left - 100) < 3 && top > 122 && bottom <= 141.5f,
                      "strona %d: litera Z na linii bazowej y=140 (x=%.1f, y=%.1f..%.1f)", i + 1, left, top, bottom);
            }
            FPDFText_ClosePage(tp);
            FPDF_ClosePage(pg);
        }
    });
    Document check;
    check.appendFile(outPath, "", err);
    for (size_t i = 0; i < check.pages.size(); ++i) writePreview(check, check.pages[i], outDir + "/obroty-" + std::to_string(i + 1) + ".png");
}

// Dokument testowy do poprawiania tekstu i zamazywania.
static std::vector<uint8_t> makeContentPdf(int rotate) {
    FPDF_DOCUMENT d = FPDF_CreateNewDocument();
    FPDF_PAGE p = FPDFPage_New(d, 0, 595.28, 841.89);
    FPDF_FONT helv = FPDFText_LoadStandardFont(d, "Helvetica");
    const FontFile& ser = fontFile(FamilySerif, false, false);
    FPDF_FONT serif = FPDFText_LoadFont(d, ser.data, uint32_t(ser.size), FPDF_FONT_TRUETYPE, 1);
    addText(d, p, helv, 12, "Hello world from Helvetica", 72, 760);
    addText(d, p, serif, 14, "Zażółć gęślą jaźń serif", 72, 720);
    addText(d, p, helv, 12, "Account number: 1234-5678 SECRET end", 72, 680);
    // obraz 200x100: niebieski
    FPDF_BITMAP bmp = FPDFBitmap_Create(200, 100, 0);
    FPDFBitmap_FillRect(bmp, 0, 0, 200, 100, 0xFF1E78DC);
    FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(d);
    FPDFImageObj_SetBitmap(&p, 1, img, bmp);
    FPDFBitmap_Destroy(bmp);
    FPDFImageObj_SetMatrix(img, 200, 0, 0, 100, 72, 500);
    FPDFPage_InsertObject(p, img);
    // mały kwadrat (do usunięcia w całości) i duży (zostaje)
    FPDF_PAGEOBJECT sq = FPDFPageObj_CreateNewRect(400, 520, 20, 20);
    FPDFPageObj_SetFillColor(sq, 200, 30, 30, 255);
    FPDFPath_SetDrawMode(sq, FPDF_FILLMODE_WINDING, 0);
    FPDFPage_InsertObject(p, sq);
    FPDF_PAGEOBJECT big = FPDFPageObj_CreateNewRect(380, 400, 150, 60);
    FPDFPageObj_SetFillColor(big, 30, 160, 60, 255);
    FPDFPath_SetDrawMode(big, FPDF_FILLMODE_WINDING, 0);
    FPDFPage_InsertObject(p, big);
    FPDFPage_GenerateContent(p);
    FPDFFont_Close(helv);
    FPDFFont_Close(serif);

    // tekst wewnątrz osadzonej grafiki (Form XObject) — z osobnego dokumentu
    FPDF_DOCUMENT d2 = FPDF_CreateNewDocument();
    FPDF_PAGE p2 = FPDFPage_New(d2, 0, 595.28, 841.89);
    FPDF_FONT h2 = FPDFText_LoadStandardFont(d2, "Helvetica");
    addText(d2, p2, h2, 12, "Nested PRIVATE note", 72, 300);
    FPDFPage_GenerateContent(p2);
    FPDF_ClosePage(p2);
    FPDFFont_Close(h2);
    FPDF_XOBJECT xo = FPDF_NewXObjectFromPage(d, d2, 0);
    FPDF_PAGEOBJECT form = FPDF_NewFormObjectFromXObject(xo);
    FPDFPage_InsertObject(p, form);
    FPDF_CloseXObject(xo);
    FPDFPage_GenerateContent(p);
    FPDFPage_SetRotation(p, rotate);
    FPDF_ClosePage(p);
    std::vector<uint8_t> bytes = saveDoc(d);
    FPDF_CloseDocument(d);
    FPDF_CloseDocument(d2);
    return bytes;
}

// Punkt w układzie bazowym odpowiadający punktowi PDF (przez renderowanie współrzędnych).
static Vec2 pdfToBase(const Page& p, double x, double y) {
    // dla stron bez CropBox przesuniętego: przekształcenie odwrotne do /Rotate
    const float w = p.baseRot % 2 ? p.height : p.width, h = p.baseRot % 2 ? p.width : p.height;
    const float X = float(x) - p.cropX, Y = float(y) - p.cropY;
    switch (p.baseRot) {
        case 1: return {Y, X};
        case 2: return {w - X, Y};
        case 3: return {h - Y, w - X};
        default: return {X, h - Y};
    }
}

static void testTextEditing(const std::string& outDir, int rotate) {
    std::printf("\n== poprawianie istniejącego tekstu (strona z /Rotate %d)\n", rotate * 90);
    Document doc;
    std::string err;
    doc.appendBytes(makeContentPdf(rotate), "tresc.pdf", "", err);
    Page& p = doc.pages[0];
    const std::string tag = "-r" + std::to_string(rotate * 90);
    writePreview(doc, p, outDir + "/edycja" + tag + "-przed.png");

    // A) linia Helvetica, tylko znaki ASCII -> czcionka z pliku
    Obj a;
    int caret = -1;
    std::string why;
    Pick r = doc.pickText(p, pdfToBase(p, 100, 764), a, caret, why);
    CHECK(r == Pick::Ok && a.text == "Hello world from Helvetica", "wybór linii Helvetica: \"%s\" (kursor %d)", a.text.c_str(), caret);
    CHECK(a.srcObjs.size() == 1, "linia to jeden obiekt PDF");
    a.text = "Hello edited world";
    a.normalizeStyles();
    p.objs.push_back(a);
    TextLayout la = doc.layoutText(p, p.objs.back());
    CHECK(la.styleOriginal.size() == 1 && la.styleOriginal[0], "tekst ASCII zostaje w oryginalnej czcionce (%s)", la.styleFont[0].c_str());

    // B) ta sama czcionka, ale polskie znaki -> czcionka zastępcza
    Obj b;
    r = doc.pickText(p, pdfToBase(p, 120, 684), b, caret, why);
    CHECK(r == Pick::Ok && b.text.find("SECRET") != std::string::npos, "wybór linii z numerem konta");
    b.text = "Numer konta: zmieniony — ąęłśżźćń";
    b.normalizeStyles();
    p.objs.push_back(b);
    TextLayout lb = doc.layoutText(p, p.objs.back());
    CHECK(!lb.styleOriginal[0], "polskie znaki -> czcionka zastępcza (%s)", lb.styleFont[0].c_str());

    // C) linia z czcionką osadzoną: znaki już obecne w pliku -> oryginalna czcionka
    Obj c;
    r = doc.pickText(p, pdfToBase(p, 90, 724), c, caret, why);
    CHECK(r == Pick::Ok && c.text.rfind("Zażółć", 0) == 0, "wybór linii z czcionką osadzoną: \"%s\"", c.text.c_str());
    c.text = "Zażółć jaźń";
    c.normalizeStyles();
    p.objs.push_back(c);
    CHECK(doc.layoutText(p, p.objs.back()).styleOriginal[0], "znaki obecne w osadzonej czcionce -> czcionka z pliku");

    // D) tekst w osadzonej grafice nie jest edytowalny
    Obj d;
    r = doc.pickText(p, pdfToBase(p, 100, 304), d, caret, why);
    CHECK(r == Pick::NotEditable, "tekst wewnątrz osadzonej grafiki: komunikat zamiast edycji");

    // E) po zmianie linia nie jest już wybierana ponownie (zastąpiona)
    Obj again;
    r = doc.pickText(p, pdfToBase(p, 100, 764), again, caret, why);
    CHECK(r != Pick::Ok || again.text != "Hello world from Helvetica", "zastąpiona linia nie wraca przy kolejnym kliknięciu");

    writePreview(doc, p, outDir + "/edycja" + tag + "-po.png");
    const std::string path = outDir + "/edycja" + tag + ".pdf";
    CHECK(doc.saveToFile(path, err), "zapis %s", err.c_str());
    reopenAndCheck(path, [&](FPDF_DOCUMENT out) {
        FPDF_PAGE pg = FPDF_LoadPage(out, 0);
        const std::string t = pageText(pg);
        CHECK(t.find("Hello edited world") != std::string::npos && t.find("Hello world from") == std::string::npos,
              "zapisany plik: nowy tekst zamiast starego");
        CHECK(t.find("ąęłśżźćń") != std::string::npos && t.find("SECRET") == std::string::npos, "polskie znaki zapisane, stary tekst usunięty");
        CHECK(t.find("Zażółć jaźń") != std::string::npos && t.find("gęślą") == std::string::npos, "poprawka w czcionce osadzonej");
        CHECK(fontOfText(pg, "Hello edited") == "Helvetica", "czcionka z pliku zachowana (%s)", fontOfText(pg, "Hello edited").c_str());
        double x, y;
        CHECK(findText(pg, "Hello edited", x, y) && std::fabs(x - 72) < 0.5 && std::fabs(y - 760) < 0.5,
              "nowy tekst dokładnie w miejscu starego (%.2f, %.2f)", x, y);
        CHECK(findText(pg, "Numer konta", x, y) && std::fabs(x - 72) < 0.5 && std::fabs(y - 680) < 0.5,
              "tekst w czcionce zastępczej w miejscu starego (%.2f, %.2f)", x, y);
        CHECK(t.find("Nested PRIVATE note") != std::string::npos, "pozostała treść nienaruszona");
        FPDF_ClosePage(pg);
    });
}

static void testRedaction(const std::string& outDir, int rotate) {
    std::printf("\n== zamazywanie z usunięciem treści (strona z /Rotate %d)\n", rotate * 90);
    Document doc;
    std::string err;
    doc.appendBytes(makeContentPdf(rotate), "tresc.pdf", "", err);
    Page& p = doc.pages[0];
    double sx = 0, sy = 0, ex = 0, ey = 0;
    auto addRedact = [&](double x0, double y0, double x1, double y1) {
        const Vec2 a = pdfToBase(p, x0, y0), b = pdfToBase(p, x1, y1);
        Obj w;
        w.type = ObjType::Whiteout;
        w.x = std::min(a.x, b.x); w.y = std::min(a.y, b.y); w.w = std::fabs(b.x - a.x); w.h = std::fabs(b.y - a.y);
        p.objs.push_back(w);
    };
    // "SECRET" w linii y=680: szerokość Helvetica 12 — od ok. x=230 do 280
    {
        std::vector<uint8_t> src = makeContentPdf(rotate);
        FPDF_DOCUMENT sd = FPDF_LoadMemDocument64(src.data(), src.size(), nullptr);
        FPDF_PAGE sp = FPDF_LoadPage(sd, 0);
        findText(sp, "SECRET", sx, sy);
        findText(sp, "end", ex, ey);
        FPDF_ClosePage(sp);
        FPDF_CloseDocument(sd);
    }
    addRedact(sx - 1, 676, ex - 3, 692);            // zakrywa "SECRET", nie zakrywa "end"
    addRedact(60, 490, 172, 610);                    // lewa połowa obrazu (72..272 x 500..600)
    addRedact(395, 515, 425, 545);                   // cały mały kwadrat
    addRedact(115, 296, 170, 312);                   // słowo "PRIVATE" w osadzonej grafice

    writePreview(doc, p, outDir + "/redakcja-r" + std::to_string(rotate * 90) + ".png");
    const std::string path = outDir + "/redakcja-r" + std::to_string(rotate * 90) + ".pdf";
    CHECK(doc.saveToFile(path, err), "zapis %s", err.c_str());
    reopenAndCheck(path, [&](FPDF_DOCUMENT out) {
        FPDF_PAGE pg = FPDF_LoadPage(out, 0);
        const std::string t = pageText(pg);
        CHECK(t.find("SECRET") == std::string::npos, "słowo pod zamazaniem usunięte z pliku");
        double x, y;
        CHECK(findText(pg, "end", x, y) && std::fabs(x - ex) < 0.3 && std::fabs(y - ey) < 0.3,
              "tekst obok zamazania został w tym samym miejscu (%.2f vs %.2f)", x, ex);
        CHECK(t.find("number: 1234-5678") != std::string::npos, "początek linii zachowany");
        CHECK(t.find("PRIVATE") == std::string::npos, "słowo w osadzonej grafice usunięte");
        CHECK(t.find("Nested") != std::string::npos && t.find("note") != std::string::npos, "reszta osadzonej grafiki zachowana");

        // obraz: lewa część zamalowana w danych obrazu, prawa niebieska
        int images = 0, whiteLeft = 0, blueRight = 0, paths = 0, redSquares = 0;
        std::function<void(FPDF_PAGEOBJECT)> visit = [&](FPDF_PAGEOBJECT o) {
            const int type = FPDFPageObj_GetType(o);
            if (type == FPDF_PAGEOBJ_FORM) {
                for (int k = 0; k < FPDFFormObj_CountObjects(o); ++k) visit(FPDFFormObj_GetObject(o, k));
            } else if (type == FPDF_PAGEOBJ_IMAGE) {
                ++images;
                FPDF_BITMAP bm = FPDFImageObj_GetBitmap(o);
                if (bm) {
                    const int w = FPDFBitmap_GetWidth(bm), st = FPDFBitmap_GetStride(bm);
                    const int bpp = FPDFBitmap_GetFormat(bm) == FPDFBitmap_BGR ? 3 : FPDFBitmap_GetFormat(bm) == FPDFBitmap_Gray ? 1 : 4;
                    const auto* px = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bm));
                    const uint8_t* l = px + 50 * st + 20 * bpp;
                    const uint8_t* r = px + 50 * st + (w - 20) * bpp;
                    if (bpp >= 3 && l[0] > 240 && l[1] > 240 && l[2] > 240) ++whiteLeft;
                    if (bpp >= 3 && r[0] > 180 && r[2] < 80) ++blueRight;
                    FPDFBitmap_Destroy(bm);
                }
            } else if (type == FPDF_PAGEOBJ_PATH) {
                ++paths;
                unsigned rr, gg, bb, aa;
                if (FPDFPageObj_GetFillColor(o, &rr, &gg, &bb, &aa) && rr == 200 && gg == 30) ++redSquares;
            }
        };
        for (int k = 0; k < FPDFPage_CountObjects(pg); ++k) visit(FPDFPage_GetObject(pg, k));
        CHECK(images == 1 && whiteLeft == 1 && blueRight == 1, "piksele obrazu pod zamazaniem zamalowane, reszta obrazu bez zmian");
        CHECK(redSquares == 0, "grafika w całości pod zamazaniem usunięta");
        {
            std::vector<uint8_t> raw;
            readFileBytes(path, raw);
            const std::string bs(raw.begin(), raw.end());
            size_t n = 0;
            for (size_t at = bs.find("/Subtype /Image"); at != std::string::npos; at = bs.find("/Subtype /Image", at + 1)) ++n;
            for (size_t at = bs.find("/Subtype/Image"); at != std::string::npos; at = bs.find("/Subtype/Image", at + 1)) ++n;
            size_t masks = 0;   // maska przezroczystości nowego obrazu też jest obiektem /Image
            for (size_t at = bs.find("/SMask"); at != std::string::npos; at = bs.find("/SMask", at + 1)) ++masks;
            CHECK(n - masks == 1, "oryginał zamazanego obrazu nie został w pliku (obrazów: %zu, masek: %zu)", n, masks);
        }
        FPDF_ClosePage(pg);
    });
}


// ===================================================================================== akapity

struct Word { std::string text; int style; };   // 0 zwykły, 1 pogrubiony, 2 czerwony

static float measure(FPDF_FONT f, float size, const std::string& s) {
    float w = 0;
    for (char32_t c : utf8To32(s)) { float g = 0; FPDFFont_GetGlyphWidth(f, uint32_t(c), size, &g); w += g; }
    return w;
}

// Strona z nagłówkiem, akapitem z formatowaniem (zawiniętym do szerokości), akapitem z wcięciem
// i przeniesionym wyrazem oraz akapitem justowanym. Każde słowo to osobny obiekt PDF.
static std::vector<uint8_t> makeParagraphPdf(float& p1Width, float& p3Width) {
    FPDF_DOCUMENT d = FPDF_CreateNewDocument();
    FPDF_PAGE p = FPDFPage_New(d, 0, 595.28, 841.89);
    FPDF_FONT reg = FPDFText_LoadStandardFont(d, "Helvetica");
    FPDF_FONT bold = FPDFText_LoadStandardFont(d, "Helvetica-Bold");
    const float size = 11, lead = 14, left = 72;
    auto put = [&](const Word& w, float x, float y) {
        FPDF_PAGEOBJECT t = addText(d, p, w.style == 1 ? bold : reg, size, w.text.c_str(), x, y);
        if (w.style == 2) FPDFPageObj_SetFillColor(t, 200, 20, 20, 255);
    };
    auto wordW = [&](const Word& w) { return measure(w.style == 1 ? bold : reg, size, w.text); };
    const float space = measure(reg, size, " ");

    addText(d, p, bold, 16, "Naglowek dokumentu", left, 760);

    // akapit 1: zawijanie zachłanne do szerokości 260 pt, wyrównanie do lewej
    std::vector<Word> p1;
    const char* t1 = "Korektor PDF pozwala poprawiac tekst calych akapitow zachowujac";
    for (const char* q = t1; *q;) { const char* e = std::strchr(q, ' '); std::string w = e ? std::string(q, e) : std::string(q); p1.push_back({w, 0}); q = e ? e + 1 : q + w.size(); }
    p1.push_back({"pogrubienia", 1});
    p1.push_back({"oraz", 0});
    p1.push_back({"kolorowe", 2});
    for (const char* w : {"slowa", "w", "tekscie,", "a", "wiersze", "same", "sie", "zawijaja", "po", "kazdej", "zmianie."}) p1.push_back({w, 0});
    const float width = 260;
    float x = left, y = 730, maxR = 0;
    for (const Word& w : p1) {
        const float ww = wordW(w);
        if (x > left && x + ww > left + width) { x = left; y -= lead; }
        put(w, x, y);
        maxR = std::max(maxR, x + ww);
        x += ww + space;
    }
    p1Width = maxR - left;

    // akapit 2: wcięcie 18 pt i wyraz przeniesiony do następnego wiersza
    y -= lead;
    x = left + 18;
    for (const char* w : {"Drugi", "akapit", "ma", "wyraz", "edyto-"}) { put({w, 0}, x, y); x += wordW({w, 0}) + space; }
    y -= lead;
    x = left;
    for (const char* w : {"wanie", "i", "konczy", "sie", "tutaj."}) { put({w, 0}, x, y); x += wordW({w, 0}) + space; }

    // akapit 3: justowany do szerokości 250 pt
    std::vector<std::string> p3;
    const char* t3 = "Trzeci akapit jest wyjustowany do obu krawedzi tak jak w gazecie a odstepy miedzy wyrazami sa rozciagniete aby kazdy wiersz konczyl sie w tym samym miejscu po prawej stronie strony";
    for (const char* q = t3; *q;) { const char* e = std::strchr(q, ' '); std::string w = e ? std::string(q, e) : std::string(q); p3.push_back(w); q = e ? e + 1 : q + w.size(); }
    const float w3 = 250;
    p3Width = w3;
    y -= 3 * lead;
    std::vector<std::vector<std::string>> rows(1);
    float acc = 0;
    for (auto& w : p3) {
        const float ww = measure(reg, size, w);
        if (!rows.back().empty() && acc + space + ww > w3) { rows.emplace_back(); acc = 0; }
        acc += (rows.back().empty() ? 0 : space) + ww;
        rows.back().push_back(w);
    }
    for (size_t r = 0; r < rows.size(); ++r) {
        float nat = 0;
        for (auto& w : rows[r]) nat += measure(reg, size, w);
        const bool last = r + 1 == rows.size();
        const float gap = (!last && rows[r].size() > 1) ? (w3 - nat) / float(rows[r].size() - 1) : space;
        x = left;
        for (auto& w : rows[r]) { put({w, 0}, x, y); x += measure(reg, size, w) + gap; }
        y -= lead;
    }
    FPDFPage_GenerateContent(p);
    FPDF_ClosePage(p);
    FPDFFont_Close(reg);
    FPDFFont_Close(bold);
    std::vector<uint8_t> bytes = saveDoc(d);
    FPDF_CloseDocument(d);
    return bytes;
}

// Linie bazowe i prawe krawędzie znaków fragmentu tekstu strony (od pierwszego wystąpienia `from` do `to`).
struct RowInfo { double y, left, right; };
static std::vector<RowInfo> rowsOf(FPDF_PAGE pg, const std::string& from, const std::string& to) {
    std::vector<RowInfo> rows;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
    const int n = FPDFText_CountChars(tp);
    std::vector<unsigned short> buf(size_t(n) + 1);
    FPDFText_GetText(tp, 0, n, buf.data());
    const std::u32string hay = utf8To32(utf16ToUtf8(buf.data()));
    const size_t a = hay.find(utf8To32(from)), b = hay.find(utf8To32(to), a);
    if (a != std::u32string::npos && b != std::u32string::npos)
        for (size_t k = a; k < b + utf8To32(to).size(); ++k) {
            if (FPDFText_IsGenerated(tp, int(k)) == 1) continue;
            double ox, oy, l, r, bo, t;
            FPDFText_GetCharOrigin(tp, int(k), &ox, &oy);
            FPDFText_GetCharBox(tp, int(k), &l, &r, &bo, &t);
            if (rows.empty() || std::fabs(rows.back().y - oy) > 0.5) rows.push_back({oy, ox, r});
            else rows.back().right = std::max(rows.back().right, r);
        }
    FPDFText_ClosePage(tp);
    return rows;
}

static std::string colorOf(FPDF_PAGE pg, const std::string& needle) {
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(pg);
    const int n = FPDFText_CountChars(tp);
    std::vector<unsigned short> buf(size_t(n) + 1);
    FPDFText_GetText(tp, 0, n, buf.data());
    const size_t pos = utf8To32(utf16ToUtf8(buf.data())).find(utf8To32(needle));
    char out[32] = "?";
    if (pos != std::u32string::npos)
        if (FPDF_PAGEOBJECT o = FPDFText_GetTextObject(tp, int(pos))) {
            unsigned r, g, b, a;
            FPDFPageObj_GetFillColor(o, &r, &g, &b, &a);
            std::snprintf(out, sizeof out, "%u,%u,%u", r, g, b);
        }
    FPDFText_ClosePage(tp);
    return out;
}

static void testParagraphs(const std::string& outDir) {
    std::printf("\n== akapity z zawijaniem i formatowaniem\n");
    float w1 = 0, w3 = 0;
    const std::vector<uint8_t> src = makeParagraphPdf(w1, w3);
    Document doc;
    std::string err;
    doc.appendBytes(src, "akapity.pdf", "", err);
    Page& p = doc.pages[0];
    writePreview(doc, p, outDir + "/akapity-przed.png");
    const std::string p1 = "Korektor PDF pozwala poprawiac tekst calych akapitow zachowujac pogrubienia oraz kolorowe "
                           "slowa w tekscie, a wiersze same sie zawijaja po kazdej zmianie.";

    // klik w drugi wiersz akapitu 1
    Obj a;
    int caret = 0;
    std::string why;
    Pick r = doc.pickText(p, pdfToBase(p, 120, 718), a, caret, why);
    CHECK(r == Pick::Ok && a.text == p1, "cały akapit 1 (bez nagłówka i akapitu 2): \"%s\"", a.text.c_str());
    CHECK(a.styles.size() == 3, "trzy style w akapicie: zwykły, pogrubiony, czerwony (%zu)", a.styles.size());
    CHECK(a.align == Align::Left && std::fabs(a.boxWidth - w1) < 2 && std::fabs(a.lineStep - 14) < 0.1f,
          "wyrównanie do lewej, szerokość %.1f (oczekiwano %.1f), interlinia %.1f", a.boxWidth, w1, a.lineStep);
    {
        const std::u32string t = utf8To32(a.text);
        const size_t bi = t.find(U"pogrubienia"), ci = t.find(U"kolorowe");
        const TextStyle& sb = a.styles[a.charStyle[bi]];
        const TextStyle& sc = a.styles[a.charStyle[ci]];
        CHECK(sb.font.bold && sc.color.r > 0.7f && sc.color.g < 0.2f, "styl słów: pogrubienie i kolor rozpoznane");
    }
    // wstawienie słów w środku akapitu (styl jak znak przed kursorem)
    {
        const std::u32string ins = U" NOWE SLOWA WSTAWIONE TUTAJ";
        std::u32string t = utf8To32(a.text);
        const size_t at = t.find(U" zachowujac");
        t.insert(at, ins);
        a.charStyle.insert(a.charStyle.begin() + long(at), ins.size(), a.charStyle[at - 1]);
        a.text = utf32To8(t);
        a.normalizeStyles();
    }
    p.objs.push_back(a);

    // akapit 2: wcięcie i przeniesiony wyraz
    Obj b;
    double wx = 0, wy = 0;
    {
        FPDF_DOCUMENT sd = FPDF_LoadMemDocument64(src.data(), src.size(), nullptr);
        FPDF_PAGE sp = FPDF_LoadPage(sd, 0);
        findText(sp, "wanie", wx, wy);   // drugi wiersz akapitu 2
        FPDF_ClosePage(sp);
        FPDF_CloseDocument(sd);
    }
    r = doc.pickText(p, pdfToBase(p, wx + 8, wy + 3), b, caret, why);
    CHECK(r == Pick::Ok && b.text == "Drugi akapit ma wyraz edytowanie i konczy sie tutaj.",
          "akapit z wcięciem, wyraz przeniesiony połączony: \"%s\"", b.text.c_str());
    CHECK(std::fabs(b.firstIndent - 18) < 0.5f, "wcięcie pierwszego wiersza %.1f pt (align %d, szer %.1f, x %.1f)", b.firstIndent, int(b.align), b.boxWidth, b.x);

    // akapit 3: justowanie
    Obj c;
    r = doc.pickText(p, pdfToBase(p, 100, 730 - 9 * 14 + 2), c, caret, why);
    bool found = r == Pick::Ok && c.text.rfind("Trzeci akapit", 0) == 0;
    if (!found) {   // w razie innego położenia: poszukaj wiersza akapitu 3
        for (float yy = 640; yy > 500 && !found; yy -= 3) {
            r = doc.pickText(p, pdfToBase(p, 100, yy), c, caret, why);
            found = r == Pick::Ok && c.text.rfind("Trzeci akapit", 0) == 0;
        }
    }
    CHECK(found && c.align == Align::Justify && std::fabs(c.boxWidth - w3) < 2, "akapit justowany rozpoznany (szerokość %.1f)", c.boxWidth);
    c.text += " i jeszcze kilka dopisanych slow na koniec akapitu";
    c.normalizeStyles();
    p.objs.push_back(c);

    // układ na ekranie: wszystkie pozycje kursora w ramce akapitu 1
    {
        const TextLayout l = doc.layoutText(p, p.objs[0]);
        bool inside = true;
        for (auto& ln : l.lines) for (auto& v : ln.carets) inside &= v.x >= 72 - 0.5f && v.x <= 72 + a.boxWidth + 0.5f;
        CHECK(l.lines.size() >= 5 && inside, "akapit 1 po wstawieniu zawija się do %zu wierszy w ramce", l.lines.size());
    }

    writePreview(doc, p, outDir + "/akapity-po.png");
    const std::string path = outDir + "/akapity.pdf";
    CHECK(doc.saveToFile(path, err), "zapis %s", err.c_str());
    reopenAndCheck(path, [&](FPDF_DOCUMENT out) {
        FPDF_PAGE pg = FPDF_LoadPage(out, 0);
        const std::string t = pageText(pg);
        CHECK(t.find("NOWE SLOWA WSTAWIONE TUTAJ") != std::string::npos, "wstawione słowa w pliku");
        CHECK(fontOfText(pg, "pogrubienia") == "Helvetica-Bold", "pogrubione słowo zachowało czcionkę (%s)", fontOfText(pg, "pogrubienia").c_str());
        CHECK(fontOfText(pg, "pozwala") == "Helvetica", "zwykły tekst w czcionce z pliku (%s)", fontOfText(pg, "pozwala").c_str());
        CHECK(colorOf(pg, "kolorowe") == "200,20,20", "kolorowe słowo zachowało kolor (%s)", colorOf(pg, "kolorowe").c_str());
        const auto rows = rowsOf(pg, "Korektor", "zmianie.");
        bool step = rows.size() >= 5, box = true;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (std::fabs(rows[i].y - (730 - 14.0 * double(i))) > 0.3) step = false;
            if (rows[i].left < 71.5 || rows[i].right > 72 + w1 + 1.5) box = false;
        }
        CHECK(step && box, "akapit 1: %zu wierszy co 14 pt, wszystkie w ramce szerokości %.0f pt", rows.size(), w1);
        double x, y;
        CHECK(findText(pg, "Naglowek", x, y) && std::fabs(y - 760) < 0.1, "nagłówek nietknięty");
        CHECK(findText(pg, "Drugi", x, y) && std::fabs(x - 90) < 0.3, "akapit 2 nietknięty (x=%.1f)", x);
        const auto rows3 = rowsOf(pg, "Trzeci", "akapitu");
        bool just = rows3.size() >= 3;
        for (size_t i = 0; i + 1 < rows3.size(); ++i)
            if (std::fabs(rows3[i].right - (72 + w3)) > 1.5) just = false;
        CHECK(just, "akapit 3 po dopisaniu: %zu wierszy, każdy (poza ostatnim) kończy się przy prawej krawędzi", rows3.size());
        FPDF_ClosePage(pg);
    });
}

// ===================================================================================== obrazy z pliku

static FPDF_PAGEOBJECT addImage(FPDF_DOCUMENT d, FPDF_PAGE p, uint32_t argb, int w, int h, float a, float b, float c, float dd, float e, float f) {
    FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0);
    FPDFBitmap_FillRect(bmp, 0, 0, w, h, argb);
    FPDFBitmap_FillRect(bmp, 0, 0, w / 4, h / 4, 0xFF000000);   // czarny róg (lewy górny piksel obrazu)
    FPDF_PAGEOBJECT img = FPDFPageObj_NewImageObj(d);
    FPDFImageObj_SetBitmap(&p, 1, img, bmp);
    FPDFBitmap_Destroy(bmp);
    FPDFImageObj_SetMatrix(img, a, b, c, dd, e, f);
    FPDFPage_InsertObject(p, img);
    return img;
}

// Granice obrazów strony (także w osadzonych grafikach), w przestrzeni PDF.
struct ImgBox { float l, b, r, t; uint32_t px; };
static std::vector<ImgBox> pageImages(FPDF_DOCUMENT doc, FPDF_PAGE pg) {
    std::vector<ImgBox> out;
    std::function<void(FPDF_PAGEOBJECT)> visit = [&](FPDF_PAGEOBJECT o) {
        const int t = FPDFPageObj_GetType(o);
        if (t == FPDF_PAGEOBJ_FORM) { for (int k = 0; k < FPDFFormObj_CountObjects(o); ++k) visit(FPDFFormObj_GetObject(o, k)); return; }
        if (t != FPDF_PAGEOBJ_IMAGE) return;
        ImgBox bx{};
        FPDFPageObj_GetBounds(o, &bx.l, &bx.b, &bx.r, &bx.t);
        if (FPDF_BITMAP bm = FPDFImageObj_GetBitmap(o)) {   // kolor środka obrazu
            const auto* q = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bm)) + (FPDFBitmap_GetHeight(bm) / 2) * FPDFBitmap_GetStride(bm) +
                            (FPDFBitmap_GetWidth(bm) / 2) * (FPDFBitmap_GetFormat(bm) == FPDFBitmap_BGR ? 3 : 4);
            bx.px = (uint32_t(q[2]) << 16) | (uint32_t(q[1]) << 8) | q[0];
            FPDFBitmap_Destroy(bm);
        }
        out.push_back(bx);
    };
    for (int k = 0; k < FPDFPage_CountObjects(pg); ++k) visit(FPDFPage_GetObject(pg, k));
    (void)doc;
    return out;
}

static void testPageImages(const std::string& outDir, int rotate) {
    std::printf("\n== obrazy z pliku (strona z /Rotate %d)\n", rotate * 90);
    // strona: obraz czerwony (100x50 pt), obraz zielony w osadzonej grafice, obraz niebieski obrócony o 90°, napis
    FPDF_DOCUMENT d = FPDF_CreateNewDocument();
    FPDF_PAGE p = FPDFPage_New(d, 0, 595.28, 841.89);
    addImage(d, p, 0xFFDC1E1E, 40, 20, 100, 0, 0, 50, 72, 700);
    addImage(d, p, 0xFF1E3CDC, 30, 30, 0, 80, -80, 0, 380, 600);   // obrót 90°: zajmuje x 300..380, y 600..680
    FPDF_FONT helv = FPDFText_LoadStandardFont(d, "Helvetica");
    addText(d, p, helv, 12, "Podpis pod obrazami", 72, 560);
    FPDFFont_Close(helv);
    FPDFPage_GenerateContent(p);
    {   // obraz zielony w Form XObject
        FPDF_DOCUMENT d2 = FPDF_CreateNewDocument();
        FPDF_PAGE p2 = FPDFPage_New(d2, 0, 595.28, 841.89);
        addImage(d2, p2, 0xFF1EB43C, 20, 20, 60, 0, 0, 60, 72, 450);
        FPDFPage_GenerateContent(p2);
        FPDF_ClosePage(p2);
        FPDF_XOBJECT xo = FPDF_NewXObjectFromPage(d, d2, 0);
        FPDFPage_InsertObject(p, FPDF_NewFormObjectFromXObject(xo));
        FPDF_CloseXObject(xo);
        FPDF_CloseDocument(d2);
    }
    FPDFPage_GenerateContent(p);
    FPDFPage_SetRotation(p, rotate);
    FPDF_ClosePage(p);
    const std::vector<uint8_t> src = saveDoc(d);
    FPDF_CloseDocument(d);

    Document doc;
    std::string err;
    doc.appendBytes(src, "obrazy.pdf", "", err);
    Page& pg = doc.pages[0];
    const std::string tag = "-r" + std::to_string(rotate * 90);
    writePreview(doc, pg, outDir + "/obrazy" + tag + "-przed.png");

    Obj red, blue, green, none;
    CHECK(doc.pickImage(pg, pdfToBase(pg, 120, 725), red) && red.childPath.empty() && red.pixelW == 40,
          "wybór czerwonego obrazu (%dx%d px, %.0fx%.0f pt)", red.pixelW, red.pixelH, red.w, red.h);
    CHECK(doc.pickImage(pg, pdfToBase(pg, 340, 640), blue) && std::fabs(std::max(blue.w, blue.h) - 80) < 0.5f, "wybór obróconego niebieskiego obrazu");
    CHECK(doc.pickImage(pg, pdfToBase(pg, 100, 480), green) && green.childPath.size() == 1, "wybór obrazu wewnątrz osadzonej grafiki");
    CHECK(!doc.pickImage(pg, pdfToBase(pg, 500, 300), none), "puste miejsce: brak obrazu");

    // czerwony: przesunięcie o 200 pt w prawo (we współrzędnych strony po /Rotate) i powiększenie 2x
    red.x += 200; red.w *= 2; red.h *= 2;
    pg.objs.push_back(red);
    CHECK(!doc.pickImage(pg, pdfToBase(pg, 120, 725), none), "zmieniony obraz nie jest wybierany drugi raz");
    // niebieski: obrót o 90° (ramka zamienia boki wokół środka)
    {
        const float cx = blue.x + blue.w / 2, cy = blue.y + blue.h / 2;
        std::swap(blue.w, blue.h);
        blue.x = cx - blue.w / 2; blue.y = cy - blue.h / 2;
        blue.imgRot = 1;
    }
    pg.objs.push_back(blue);
    // zielony: usunięty
    green.hidden = true;
    pg.objs.push_back(green);

    writePreview(doc, pg, outDir + "/obrazy" + tag + "-po.png");
    const std::string path = outDir + "/obrazy" + tag + ".pdf";
    CHECK(doc.saveToFile(path, err), "zapis %s", err.c_str());
    reopenAndCheck(path, [&](FPDF_DOCUMENT out) {
        FPDF_PAGE op = FPDF_LoadPage(out, 0);
        const auto imgs = pageImages(out, op);
        CHECK(imgs.size() == 2, "zostały 2 obrazy (zielony usunięty): %zu", imgs.size());
        bool redOk = false, blueOk = false;
        for (const auto& b : imgs) {
            // pozycje w przestrzeni PDF zależą od /Rotate — porównujemy w układzie bazowym strony
            const Vec2 p1 = pdfToBase(pg, b.l, b.b), p2 = pdfToBase(pg, b.r, b.t);
            const float x0 = std::min(p1.x, p2.x), y0 = std::min(p1.y, p2.y), w = std::fabs(p2.x - p1.x), h = std::fabs(p2.y - p1.y);
            if (std::fabs(x0 - red.x) < 0.6f && std::fabs(y0 - red.y) < 0.6f && std::fabs(w - red.w) < 0.6f && std::fabs(h - red.h) < 0.6f) redOk = true;
            if (std::fabs(x0 - blue.x) < 0.6f && std::fabs(y0 - blue.y) < 0.6f && std::fabs(w - blue.w) < 0.6f && std::fabs(h - blue.h) < 0.6f) blueOk = true;
        }
        CHECK(redOk, "czerwony obraz przesunięty i powiększony 2x");
        CHECK(blueOk, "niebieski obraz obrócony w miejscu");
        CHECK(pageText(op).find("Podpis pod obrazami") != std::string::npos, "tekst strony nietknięty");
        FPDF_ClosePage(op);
    });
    {   // usunięty obraz nie zostaje w pliku jako nieużywany obiekt
        std::vector<uint8_t> raw;
        readFileBytes(path, raw);
        const std::string b(raw.begin(), raw.end());
        size_t n = 0;
        for (size_t at = b.find("/Image"); at != std::string::npos; at = b.find("/Image", at + 1)) ++n;
        CHECK(n == 2, "w pliku są tylko 2 obrazy — dane usuniętego obrazu nie zostały (%zu)", n);
    }
    // obrót w prawo: czarny róg obrazu przesuwa się o ćwierć obrotu zgodnie z ruchem wskazówek zegara
    if (rotate == 0) {
        auto blackCenter = [&](const Document& dd, const Page& pp, float bx0, float by0, float bw, float bh, float& cx, float& cy) {
            std::vector<uint8_t> rgba;
            int w, h;
            dd.render(pp, 1.f, rgba, w, h);
            double sx = 0, sy = 0; int n = 0;
            for (int y = int(by0); y < int(by0 + bh); ++y)
                for (int x = int(bx0); x < int(bx0 + bw); ++x) {
                    const uint8_t* q = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
                    if (q[0] < 60 && q[1] < 60 && q[2] < 60) { sx += x + 0.5; sy += y + 0.5; ++n; }
                }
            cx = n ? float(sx / n) : -1; cy = n ? float(sy / n) : -1;
        };
        Document before, after;
        before.appendBytes(src, "a.pdf", "", err);
        after.appendFile(path, "", err);
        float bx, by, ax, ay;
        blackCenter(before, before.pages[0], blue.x, blue.y, blue.w, blue.h, bx, by);
        blackCenter(after, after.pages[0], blue.x, blue.y, blue.w, blue.h, ax, ay);
        const float cx = blue.x + blue.w / 2, cy = blue.y + blue.h / 2;
        const float ex = cx - (by - cy), ey = cy + (bx - cx);   // (dx, dy) -> (-dy, dx): obrót w prawo na ekranie
        CHECK(std::hypot(ax - ex, ay - ey) < 2.f, "obrót obrazu w prawo: róg z (%.0f, %.0f) przeszedł do (%.0f, %.0f), oczekiwano (%.0f, %.0f)", bx, by, ax, ay, ex, ey);
    }

    // podmiana i eksport
    {
        Document d3;
        d3.appendBytes(src, "obrazy.pdf", "", err);
        Page& p3 = d3.pages[0];
        Obj r2;
        d3.pickImage(p3, pdfToBase(p3, 120, 725), r2);
        const std::string png = outDir + "/eksport" + tag + ".png";
        CHECK(d3.exportImage(p3, r2, png, err), "eksport obrazu do PNG %s", err.c_str());
        std::string e2;
        ImagePtr back = loadImageFile(png, e2);
        CHECK(back && back->width == 40 && back->height == 20 && back->rgba[(10 * 40 + 30) * 4] > 200, "wyeksportowany PNG ma oryginalną rozdzielczość i kolor");
        std::vector<uint8_t> yellow(16 * 16 * 4);
        for (size_t k = 0; k < yellow.size(); k += 4) { yellow[k] = 240; yellow[k + 1] = 220; yellow[k + 2] = 20; yellow[k + 3] = 255; }
        r2.image = makeImage(16, 16, std::move(yellow));
        p3.objs.push_back(r2);
        const std::string path3 = outDir + "/podmiana" + tag + ".pdf";
        CHECK(d3.saveToFile(path3, err), "zapis po podmianie");
        reopenAndCheck(path3, [&](FPDF_DOCUMENT out) {
            FPDF_PAGE op = FPDF_LoadPage(out, 0);
            bool found = false;
            for (const auto& b : pageImages(out, op)) {
                const Vec2 p1 = pdfToBase(p3, b.l, b.b), p2 = pdfToBase(p3, b.r, b.t);
                if (std::fabs(std::min(p1.x, p2.x) - r2.x) < 0.6f && std::fabs(std::max(p1.x, p2.x) - (r2.x + r2.w)) < 0.6f && b.px == 0xF0DC14) found = true;
            }
            CHECK(found, "obraz podmieniony w tym samym miejscu");
            FPDF_ClosePage(op);
        });
    }
}

// ===================================================================================== zaznaczenie bez zmian

static void testUntouchedSelection(const std::string& outDir) {
    std::printf("\n== zaznaczony, niezmieniony akapit nie zmienia strony\n");
    float w1, w3;
    const std::vector<uint8_t> src = makeParagraphPdf(w1, w3);
    Document doc;
    std::string err;
    doc.appendBytes(src, "a.pdf", "", err);
    Page& p = doc.pages[0];
    std::vector<uint8_t> before, after;
    int w, h;
    doc.render(p, 1.5f, before, w, h);
    Obj a;
    int caret;
    std::string why;
    CHECK(doc.pickText(p, pdfToBase(p, 120, 718), a, caret, why) == Pick::Ok && a.untouched(), "wybrany akapit jest oznaczony jako niezmieniony");
    CHECK(doc.hasTextAt(p, pdfToBase(p, 120, 718)) && !doc.hasTextAt(p, pdfToBase(p, 500, 300)), "szybkie trafienie w tekst");
    p.objs.push_back(a);
    doc.render(p, 1.5f, after, w, h);
    CHECK(before == after, "render z zaznaczonym akapitem identyczny co do piksela");
    // przesunięcie akapitu o 100 pt w dół: zmieniony, więc przerysowany w nowym miejscu
    p.objs.back().y += 100;
    CHECK(!p.objs.back().untouched(), "po przesunięciu akapit jest zmieniony");
    const std::string path = outDir + "/przesuniety-akapit.pdf";
    CHECK(doc.saveToFile(path, err), "zapis");
    reopenAndCheck(path, [&](FPDF_DOCUMENT out) {
        FPDF_PAGE pg = FPDF_LoadPage(out, 0);
        double x, y;
        CHECK(findText(pg, "Korektor", x, y) && std::fabs(y - 630) < 0.3 && std::fabs(x - 72) < 0.3, "akapit przesunięty o 100 pt (y=%.1f)", y);
        CHECK(fontOfText(pg, "pogrubienia") == "Helvetica-Bold", "formatowanie przesuniętego akapitu zachowane");
        FPDF_ClosePage(pg);
    });
    // kopia obrazu z pliku: wygląd jak na stronie
    Obj img;
    Document d2;
    {
        FPDF_DOCUMENT dd = FPDF_CreateNewDocument();
        FPDF_PAGE pp = FPDFPage_New(dd, 0, 595.28, 841.89);
        addImage(dd, pp, 0xFFDC1E1E, 40, 20, 0, 100, -50, 0, 200, 600);   // obraz obrócony o 90° w pliku
        FPDFPage_GenerateContent(pp);
        FPDFPage_SetRotation(pp, 1);
        FPDF_ClosePage(pp);
        d2.appendBytes(saveDoc(dd), "i.pdf", "", err);
        FPDF_CloseDocument(dd);
    }
    Page& p2 = d2.pages[0];
    CHECK(d2.pickImage(p2, pdfToBase(p2, 175, 650), img), "wybór obrazu do skopiowania");
    int rot = 0;
    ImagePtr look = d2.pageImageAppearance(p2, img, rot);
    // kopia jako wstawiony obraz w tym samym miejscu musi wyglądać tak samo jak oryginał
    Obj copy;
    copy.type = ObjType::Image;
    copy.x = img.x; copy.y = img.y; copy.w = img.w; copy.h = img.h;
    copy.image = look;
    copy.imgRot = rot;
    std::vector<uint8_t> r1, r2;
    d2.render(p2, 1.f, r1, w, h);
    img.hidden = true;
    p2.objs.push_back(img);
    p2.objs.push_back(copy);
    const std::string path2 = outDir + "/kopia-obrazu.pdf";
    d2.saveToFile(path2, err);
    Document d3;
    d3.appendFile(path2, "", err);
    d3.render(d3.pages[0], 1.f, r2, w, h);
    // porównanie czarnego rogu (położenie charakterystycznego fragmentu)
    auto dark = [&](const std::vector<uint8_t>& px) {
        double sx = 0, sy = 0; int n = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { const uint8_t* q = &px[(size_t(y) * size_t(w) + size_t(x)) * 4]; if (q[0] < 60 && q[1] < 60 && q[2] < 60) { sx += x; sy += y; ++n; } }
        return std::make_pair(n ? sx / n : -1, n ? sy / n : -1);
    };
    const auto c1 = dark(r1), c2 = dark(r2);
    CHECK(look && std::hypot(c1.first - c2.first, c1.second - c2.second) < 2.0,
          "kopia obrazu wygląda jak oryginał (róg %.0f,%.0f vs %.0f,%.0f)", c1.first, c1.second, c2.first, c2.second);
}

static void testBlankPages(const std::string& outDir) {
    std::printf("\n== puste strony wybranego rozmiaru\n");
    Document doc;
    std::string err;
    doc.insertBlankPage(-1, mmToPt(215.9f), mmToPt(279.4f));      // Letter pionowo
    doc.insertBlankPage(-1, mmToPt(420), mmToPt(297));            // A3 poziomo
    doc.insertBlankPage(0, mmToPt(120), mmToPt(80));              // własny, na początku
    doc.insertBlankPage(-1, 1, 99999);                            // poza zakresem PDF: przycięty
    CHECK(paperName(doc.pages[1].width, doc.pages[1].height) == "Letter" && paperName(doc.pages[2].width, doc.pages[2].height) == "A3",
          "rozpoznanie formatów: %s, %s", paperName(doc.pages[1].width, doc.pages[1].height).c_str(), paperName(doc.pages[2].width, doc.pages[2].height).c_str());
    CHECK(paperName(doc.pages[0].width, doc.pages[0].height) == "120×80 mm", "własny rozmiar opisany w mm (%s)", paperName(doc.pages[0].width, doc.pages[0].height).c_str());
    const std::string path = outDir + "/puste-strony.pdf";
    CHECK(doc.saveToFile(path, err), "zapis");
    reopenAndCheck(path, [&](FPDF_DOCUMENT out) {
        CHECK(FPDF_GetPageCount(out) == 4, "4 strony");
        struct { float w, h; } want[] = {{mmToPt(120), mmToPt(80)}, {612, 792}, {mmToPt(420), mmToPt(297)}, {kMinPagePt, kMaxPagePt}};
        for (int i = 0; i < 4 && i < FPDF_GetPageCount(out); ++i) {
            FPDF_PAGE pg = FPDF_LoadPage(out, i);
            const float w = FPDF_GetPageWidthF(pg), h = FPDF_GetPageHeightF(pg);
            CHECK(std::fabs(w - want[i].w) < 0.05f && std::fabs(h - want[i].h) < 0.05f, "strona %d: %.2f × %.2f pt", i + 1, w, h);
            FPDF_ClosePage(pg);
        }
    });
}


// Odnośniki: odczyt, zmiana, nowe, wewnętrzne po zmianie kolejności stron, zamazanie, usunięta strona docelowa.
static void testLinks(const std::string& outDir) {
    std::printf("\n== odnośniki\n");
    CHECK(normalizeUri("www.example.com") == "https://www.example.com", "adres bez schematu -> https (%s)", normalizeUri("www.example.com").c_str());
    CHECK(normalizeUri(" jan@example.pl ") == "mailto:jan@example.pl", "e-mail -> mailto:");
    CHECK(normalizeUri("localhost:8080/a") == "https://localhost:8080/a", "host z portem to nie schemat");
    const std::string pl = normalizeUri("https://pl.wikipedia.org/wiki/Łódź Kaliska");
    CHECK(pl == "https://pl.wikipedia.org/wiki/%C5%81%C3%B3d%C5%BA%20Kaliska", "polskie znaki i spacje kodowane (%s)", pl.c_str());
    CHECK(displayUri(pl) == "https://pl.wikipedia.org/wiki/Łódź Kaliska", "czytelna postać adresu");

    // plik źródłowy: 4 strony, na pierwszej dwa odnośniki www (jeden pod przyszłym zamazaniem)
    FPDF_DOCUMENT src = FPDF_CreateNewDocument();
    for (int i = 0; i < 4; ++i) FPDF_ClosePage(FPDFPage_New(src, i, 595, 842));
    {
        FPDF_PAGE pg = FPDF_LoadPage(src, 0);
        auto add = [&](const char* uri, float l, float b, float r, float t) {
            FPDF_ANNOTATION an = FPDFPage_CreateAnnot(pg, FPDF_ANNOT_LINK);
            FS_RECTF rc{l, t, r, b};
            FPDFAnnot_SetRect(an, &rc);
            FPDFAnnot_SetURI(an, uri);
            FPDFPage_CloseAnnot(an);
        };
        add("https://www.example.com/", 100, 700, 250, 715);
        add("https://tajne.example.com/", 100, 600, 250, 615);
        FPDF_FONT helv = FPDFText_LoadStandardFont(src, "Helvetica");
        addText(src, pg, helv, 11, "Strona  firmy", 102, 704);
        FPDFFont_Close(helv);
        FPDFPage_GenerateContent(pg);
        FPDF_ClosePage(pg);
    }
    std::vector<uint8_t> bytes = saveDoc(src);
    FPDF_CloseDocument(src);

    Document doc;
    std::string err;
    doc.appendBytes(bytes, "linki.pdf", "", err);
    CHECK(doc.pages.size() == 4 && doc.pages[0].links.size() == 2, "odczyt: %zu odnośników na stronie 1", doc.pages.empty() ? 0 : doc.pages[0].links.size());
    if (doc.pages.size() != 4 || doc.pages[0].links.size() != 2) return;
    Page& p0 = doc.pages[0];
    const Link& l0 = p0.links[0];
    CHECK(std::fabs(l0.box.x - 100) < 0.1f && std::fabs(l0.box.y - (842 - 715)) < 0.1f && std::fabs(l0.box.w - 150) < 0.1f && std::fabs(l0.box.h - 15) < 0.1f,
          "obszar we współrzędnych strony (%.1f %.1f %.1f %.1f)", l0.box.x, l0.box.y, l0.box.w, l0.box.h);

    const std::string under = doc.textInRect(p0, l0.box);
    CHECK(under == "Strona firmy", "tekst pod odnośnikiem: „%s”", under.c_str());
    CHECK(doc.textInRect(p0, p0.links[1].box).empty(), "odnośnik bez tekstu");

    // zmiany: adres, nowy e-mail, odnośniki wewnętrzne, zamazanie drugiego odnośnika
    const int page3 = doc.pages[2].id, page4 = doc.pages[3].id;
    p0.links[0].uri = "www.zmieniony.pl/ścieżka";
    Link mail; mail.box = {100, 300, 120, 14}; mail.uri = "biuro@example.pl";
    Link toP4; toP4.box = {100, 400, 120, 14}; toP4.targetPage = page4; toP4.hasTargetY = true; toP4.targetY = 200;
    Link toP3; toP3.box = {100, 450, 120, 14}; toP3.targetPage = page3;
    p0.links.push_back(mail);
    p0.links.push_back(toP4);
    p0.links.push_back(toP3);
    Obj wo; wo.type = ObjType::Whiteout; wo.x = 90; wo.y = 842 - 620; wo.w = 200; wo.h = 30;
    p0.objs.push_back(wo);
    p0.extraRot = 1;                                     // obrót w edytorze nie przesuwa odnośników
    doc.pages.erase(doc.pages.begin() + 2);              // strona 3 usunięta -> odnośnik do niej znika
    std::swap(doc.pages[1], doc.pages[2]);               // kolejność: 1, 4, 2
    const std::string path = outDir + "/odnosniki.pdf";
    CHECK(doc.saveToFile(path, err), "zapis");
    auto verify = [&](FPDF_DOCUMENT out, const char* tag) {
        FPDF_PAGE pg = FPDF_LoadPage(out, 0);
        int pos = 0, n = 0, internal = 0;
        bool uriOk = false, mailOk = false, secret = false, destOk = false, rectOk = false;
        FPDF_LINK link = nullptr;
        while (FPDFLink_Enumerate(pg, &pos, &link)) {
            ++n;
            FPDF_DEST dest = FPDFLink_GetDest(out, link);
            FPDF_ACTION act = FPDFLink_GetAction(link);
            if (!dest && act && FPDFAction_GetType(act) == PDFACTION_GOTO) dest = FPDFAction_GetDest(out, act);
            if (dest) {
                ++internal;
                FPDF_BOOL hx, hy, hz;
                FS_FLOAT x, y, z;
                const bool loc = FPDFDest_GetLocationInPage(dest, &hx, &hy, &hz, &x, &y, &z);
                destOk = FPDFDest_GetDestPageIndex(out, dest) == 1 && loc && hy && std::fabs(y - (842 - 200)) < 0.1f;
                continue;
            }
            char buf[256] = {};
            if (act) FPDFAction_GetURIPath(out, act, buf, sizeof buf);
            const std::string u = buf;
            if (u == "https://www.zmieniony.pl/%C5%9Bcie%C5%BCka") {
                uriOk = true;
                FS_RECTF r;
                rectOk = FPDFLink_GetAnnotRect(link, &r) && std::fabs(r.left - 100) < 0.1f && std::fabs(r.top - 715) < 0.1f;
            }
            mailOk |= u == "mailto:biuro@example.pl";
            secret |= u.find("tajne") != std::string::npos;
        }
        CHECK(n == 3 && internal == 1, "%s: 3 odnośniki na stronie 1, w tym 1 wewnętrzny (jest %d/%d)", tag, n, internal);
        CHECK(uriOk && rectOk, "%s: zmieniony adres zakodowany, obszar bez zmian mimo obrotu", tag);
        CHECK(mailOk, "%s: nowy odnośnik e-mail", tag);
        CHECK(destOk, "%s: odnośnik wewnętrzny wskazuje przeniesioną stronę i wysokość", tag);
        CHECK(!secret, "%s: odnośnik pod zamazaniem usunięty", tag);
        FPDF_ClosePage(pg);
    };
    const std::vector<uint8_t> saved = reopenAndCheck(path, [&](FPDF_DOCUMENT out) { verify(out, "po zapisie"); });
    CHECK(std::string(saved.begin(), saved.end()).find("kpdf-goto") == std::string::npos, "brak adresów zastępczych w pliku");

    // ponowne otwarcie i zapis bez zmian: nic się nie mnoży ani nie gubi
    Document again;
    again.appendBytes(saved, "odnosniki.pdf", "", err);
    CHECK(!again.pages.empty() && again.pages[0].links.size() == 3, "odczyt zapisanego: %zu odnośników", again.pages.empty() ? 0 : again.pages[0].links.size());
    if (again.pages.size() == 3) {
        int internalTarget = -1;
        for (const Link& l : again.pages[0].links) if (l.internal()) internalTarget = l.targetPage;
        CHECK(internalTarget == again.pages[1].id, "cel wewnętrzny rozpoznany jako strona 2");
    }
    // odnośnik z celem /FitR (prostokąt — tak linkują np. czasopisma do bibliografii): znane miejsce na stronie
    {
        const std::string raw =
            "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
            "2 0 obj<</Type/Pages/Kids[3 0 R 4 0 R]/Count 2>>endobj\n"
            "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 600 800]/Annots[5 0 R]>>endobj\n"
            "4 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 600 800]>>endobj\n"
            "5 0 obj<</Type/Annot/Subtype/Link/Rect[100 700 160 712]/Dest[4 0 R/FitR 40 500 300 620]>>endobj\n"
            "trailer<</Root 1 0 R/Size 6>>\n%%EOF\n";
        Document fr;
        fr.appendBytes(std::vector<uint8_t>(raw.begin(), raw.end()), "fitr.pdf", "", err);
        const bool ok = fr.pages.size() == 2 && fr.pages[0].links.size() == 1;
        const Link l = ok ? fr.pages[0].links[0] : Link{};
        CHECK(ok && l.internal() && l.targetPage == fr.pages[1].id && l.hasTargetY && std::fabs(l.targetY - 180) < 0.1f,
              "cel /FitR: strona 2, górna krawędź prostokąta (y=%.1f)", l.targetY);
    }

    const std::string path2 = outDir + "/odnosniki-2.pdf";
    CHECK(again.saveToFile(path2, err), "drugi zapis");
    reopenAndCheck(path2, [&](FPDF_DOCUMENT out) { verify(out, "po drugim zapisie"); });
}


// ===================================================================================== kształty z pliku

static FPDF_PAGEOBJECT addRect(FPDF_PAGE p, float x, float y, float w, float h, int fill, int stroke, float width) {
    FPDF_PAGEOBJECT r = FPDFPageObj_CreateNewRect(x, y, w, h);
    if (fill >= 0) FPDFPageObj_SetFillColor(r, unsigned(fill >> 16) & 255, unsigned(fill >> 8) & 255, unsigned(fill) & 255, 255);
    if (stroke >= 0) {
        FPDFPageObj_SetStrokeColor(r, unsigned(stroke >> 16) & 255, unsigned(stroke >> 8) & 255, unsigned(stroke) & 255, 255);
        FPDFPageObj_SetStrokeWidth(r, width);
    }
    FPDFPath_SetDrawMode(r, fill >= 0 ? FPDF_FILLMODE_ALTERNATE : FPDF_FILLMODE_NONE, stroke >= 0);
    FPDFPage_InsertObject(p, r);
    return r;
}

struct PathInfo { unsigned r, g, b; int fill; bool stroke; float width; Rect box; };
// Ścieżki strony — także wewnątrz osadzonych grafik (zapis zachowuje grafiki z oryginału).
static void collectPaths(FPDF_PAGEOBJECT o, const FS_MATRIX& toPage, const Page& ref, std::vector<PathInfo>& out) {
    auto apply = [](const FS_MATRIX& m, float x, float y) { return std::pair<float, float>{m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f}; };
    if (FPDFPageObj_GetType(o) == FPDF_PAGEOBJ_FORM) {
        FS_MATRIX fm;
        if (!FPDFPageObj_GetMatrix(o, &fm)) return;
        const FS_MATRIX c{fm.a * toPage.a + fm.b * toPage.c, fm.a * toPage.b + fm.b * toPage.d, fm.c * toPage.a + fm.d * toPage.c,
                          fm.c * toPage.b + fm.d * toPage.d, fm.e * toPage.a + fm.f * toPage.c + toPage.e, fm.e * toPage.b + fm.f * toPage.d + toPage.f};
        for (int i = 0; i < FPDFFormObj_CountObjects(o); ++i) collectPaths(FPDFFormObj_GetObject(o, (unsigned long)i), c, ref, out);
        return;
    }
    if (FPDFPageObj_GetType(o) != FPDF_PAGEOBJ_PATH) return;
    PathInfo pi{};
    unsigned a;
    FPDFPageObj_GetFillColor(o, &pi.r, &pi.g, &pi.b, &a);
    FPDF_BOOL st = 0;
    FPDFPath_GetDrawMode(o, &pi.fill, &st);
    pi.stroke = st != 0;
    FPDFPageObj_GetStrokeWidth(o, &pi.width);
    FS_MATRIX m;
    FPDFPageObj_GetMatrix(o, &m);
    pi.width *= std::sqrt(std::fabs(m.a * m.d - m.b * m.c)) * std::sqrt(std::fabs(toPage.a * toPage.d - toPage.b * toPage.c));
    float l, b, r, t;
    FPDFPageObj_GetBounds(o, &l, &b, &r, &t);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (auto [x, y] : {apply(toPage, l, b), apply(toPage, r, b), apply(toPage, l, t), apply(toPage, r, t)}) {
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    const Vec2 c1 = pdfToBase(ref, x0, y0), c2 = pdfToBase(ref, x1, y1);
    pi.box = {std::min(c1.x, c2.x), std::min(c1.y, c2.y), std::fabs(c2.x - c1.x), std::fabs(c2.y - c1.y)};
    out.push_back(pi);
}

static std::vector<PathInfo> pagePaths(FPDF_PAGE pg, const Page& ref) {
    std::vector<PathInfo> out;
    const FS_MATRIX id{1, 0, 0, 1, 0, 0};
    for (int i = 0; i < FPDFPage_CountObjects(pg); ++i) collectPaths(FPDFPage_GetObject(pg, i), id, ref, out);
    return out;
}

static void testShapes(const std::string& outDir, int rotate) {
    std::printf("\n== kształty z pliku (strona z /Rotate %d)\n", rotate * 90);
    FPDF_DOCUMENT d = FPDF_CreateNewDocument();
    FPDF_PAGE p = FPDFPage_New(d, 0, 595.28, 841.89);
    addRect(p, 0, 0, 595.28f, 841.89f, 0xFFFFFF, -1, 0);       // białe tło całej strony (pomijane przy wyborze)
    addRect(p, 100, 600, 200, 80, 0xC8C8C8, -1, 0);            // szary prostokąt
    addRect(p, 72, 500, 400, 0.5f, 0x000000, -1, 0);           // „linia” jak w czasopismach: cienki wypełniony prostokąt
    addRect(p, 100, 300, 100, 100, -1, 0x1E3CDC, 4);           // niebieski kontur bez wypełnienia
    FPDFPage_GenerateContent(p);
    {   // zielony kwadrat wewnątrz osadzonej grafiki (Form XObject)
        FPDF_DOCUMENT d2 = FPDF_CreateNewDocument();
        FPDF_PAGE p2 = FPDFPage_New(d2, 0, 595.28, 841.89);
        addRect(p2, 400, 700, 50, 50, 0x1EB43C, -1, 0);
        FPDFPage_GenerateContent(p2);
        FPDF_ClosePage(p2);
        FPDF_XOBJECT xo = FPDF_NewXObjectFromPage(d, d2, 0);
        FPDFPage_InsertObject(p, FPDF_NewFormObjectFromXObject(xo));
        FPDF_CloseXObject(xo);
        FPDF_CloseDocument(d2);
    }
    FPDFPage_GenerateContent(p);
    FPDFPage_SetRotation(p, rotate);
    FPDF_ClosePage(p);
    FPDF_ClosePage(FPDFPage_New(d, 1, 595.28, 841.89));
    const std::vector<uint8_t> src = saveDoc(d);
    FPDF_CloseDocument(d);

    Document doc;
    std::string err;
    doc.appendBytes(src, "ksztalty.pdf", "", err);
    Page& pg = doc.pages[0];
    const std::string tag = "-r" + std::to_string(rotate * 90);
    Obj gray, line, frame, none;
    CHECK(doc.pickShape(pg, pdfToBase(pg, 200, 640), 2, gray) && gray.fillMode != 0 && !gray.strokeOn &&
              std::fabs(gray.color.r - 200 / 255.f) < 0.01f && std::fabs(std::max(gray.w, gray.h) - 200) < 0.1f,
          "wybór szarego prostokąta (%.0f×%.0f pt)", gray.w, gray.h);
    CHECK(doc.pickShape(pg, pdfToBase(pg, 300, 502), 2.5f, line) && std::min(line.w, line.h) < 1, "wybór cienkiej linii z tolerancją");
    CHECK(doc.pickShape(pg, pdfToBase(pg, 101, 350), 2, frame) && frame.strokeOn && frame.fillMode == 0 && std::fabs(frame.size - 4) < 0.01f,
          "wybór konturu (grubość %.1f pt)", frame.size);
    CHECK(!doc.pickShape(pg, pdfToBase(pg, 150, 350), 2, none), "środek konturu bez wypełnienia: nic");
    CHECK(!doc.pickShape(pg, pdfToBase(pg, 500, 100), 2, none), "puste miejsce: tło strony nie jest wybierane");
    std::vector<int> zi, zs;
    Obj tmp;
    CHECK(doc.pickShape(pg, pdfToBase(pg, 200, 640), 2, tmp, &zs) && zs.size() == 1 && zs[0] == 1, "kolejność rysowania kształtu");

    Obj green;
    CHECK(doc.pickShape(pg, pdfToBase(pg, 425, 725), 2, green) && green.childPath.size() == 1, "wybór kształtu wewnątrz osadzonej grafiki");
    const Rect greenBox{green.x, green.y, green.w, green.h};
    green.y += 300;   // w dół strony (we współrzędnych strony)

    // samo zaznaczenie nie zmienia strony
    std::vector<uint8_t> before, after;
    int w, h;
    doc.render(pg, 1.f, before, w, h);
    pg.objs.push_back(gray);
    CHECK(pg.objs.back().shapeIdle(), "zaznaczony kształt bez zmian");
    doc.render(pg, 1.f, after, w, h);
    CHECK(before == after, "render z zaznaczonym kształtem identyczny co do piksela");
    CHECK(!doc.pickShape(pg, pdfToBase(pg, 200, 640), 2, none), "zaznaczony kształt nie jest wybierany drugi raz");

    // zmiany: szary — przesunięcie o 50 pt w prawo (na stronie) i kolor czerwony; linia — usunięta;
    // kontur — dwa razy szerszy (grubość linii ma zostać)
    Obj& g = pg.objs.back();
    const Rect grayBox{g.x, g.y, g.w, g.h};
    g.x += 50;
    g.color = {0.86f, 0.12f, 0.12f, 1};
    g.shapeEdits |= kShapeFill;
    line.hidden = true;
    pg.objs.push_back(line);
    pg.objs.push_back(green);
    const Rect frameBox{frame.x, frame.y, frame.w, frame.h};
    frame.w *= 2;
    pg.objs.push_back(frame);
    // kopia czerwonego prostokąta na drugiej stronie
    Obj copy = pg.objs[pg.objs.size() - 4];
    copy.type = ObjType::Shape;
    copy.srcObjs.clear();
    copy.childPath.clear();
    copy.shapeEdits = 0;
    doc.pages[1].objs.push_back(copy);
    writePreview(doc, pg, outDir + "/ksztalty" + tag + ".png");

    const std::string path = outDir + "/ksztalty" + tag + ".pdf";
    CHECK(doc.saveToFile(path, err), "zapis");
    reopenAndCheck(path, [&](FPDF_DOCUMENT out) {
        FPDF_PAGE p0 = FPDF_LoadPage(out, 0);
        const auto paths = pagePaths(p0, pg);
        bool red = false, black = false, oldGray = false, blue = false, greenOk = false;
        for (const PathInfo& pi : paths) {
            if (pi.fill && pi.r > 200 && pi.g < 60) {
                red = std::fabs(pi.box.x - (grayBox.x + 50)) < 0.6f && std::fabs(pi.box.y - grayBox.y) < 0.6f &&
                      std::fabs(pi.box.w - grayBox.w) < 0.6f && std::fabs(pi.box.h - grayBox.h) < 0.6f;
            }
            if (pi.fill && pi.r == 0 && pi.g == 0 && pi.b == 0) black = true;
            if (pi.fill && pi.g == 0xB4)
                greenOk = std::fabs(pi.box.x - greenBox.x) < 0.6f && std::fabs(pi.box.y - (greenBox.y + 300)) < 0.6f;
            if (pi.fill && pi.r == 200) oldGray = true;
            if (pi.stroke) {
                const float pad = pi.box.h - frameBox.h;   // granice PDFium obejmują grubość linii
                blue = std::fabs(pi.width - 4) < 0.05f && pad > 0 && pad < 10 && std::fabs(pi.box.w - pad - 2 * frameBox.w) < 0.5f;
                std::printf("      kontur: %.1f×%.1f pt, grubość %.2f\n", pi.box.w, pi.box.h, pi.width);
            }
        }
        CHECK(red, "szary prostokąt przesunięty i czerwony");
        CHECK(!oldGray, "nie ma już szarego prostokąta");
        CHECK(!black, "linia usunięta");
        CHECK(greenOk, "kształt z osadzonej grafiki przesunięty");
        CHECK(blue, "kontur dwa razy szerszy, grubość linii bez zmian");
        FPDF_ClosePage(p0);
        FPDF_PAGE p1 = FPDF_LoadPage(out, 1);
        const auto paths1 = pagePaths(p1, doc.pages[1]);
        CHECK(paths1.size() == 1 && paths1[0].r > 200 && paths1[0].fill, "kopia kształtu na drugiej stronie");
        FPDF_ClosePage(p1);
    });
}

// ------------------------------------------------------------------ zachowanie oryginalnej treści
// Plik z elementami, które generator PDFium psuje przy przepisywaniu strony: kolory CMYK i dodatkowe,
// maska przezroczystości, nadruk, odstępy i kerning tekstu, obraz wbudowany, cieniowanie, warstwa
// ukryta, dwa strumienie treści. Po zmianach na stronie wszystko poza zmienionymi miejscami ma
// wyglądać jak w oryginale.

static std::vector<uint8_t> rawPdf(const std::vector<std::string>& objs) {
    std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offs;
    for (size_t i = 0; i < objs.size(); ++i) {
        offs.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : offs) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return std::vector<uint8_t>(out.begin(), out.end());
}

static std::string streamObj(const std::string& dict, const std::string& data) {
    return "<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream";
}

// Strona 1 pliku jako BGRA (1 piksel = 1 pt).
static std::vector<uint8_t> renderPdfPage(const std::vector<uint8_t>& bytes, int& w, int& h) {
    std::vector<uint8_t> px;
    FPDF_DOCUMENT d = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
    if (!d) return px;
    FPDF_PAGE pg = FPDF_LoadPage(d, 0);
    w = int(FPDF_GetPageWidthF(pg));
    h = int(FPDF_GetPageHeightF(pg));
    FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0);
    FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, pg, 0, 0, w, h, 0, FPDF_ANNOT);
    const auto* b = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bmp));
    const int stride = FPDFBitmap_GetStride(bmp);
    px.resize(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) std::memcpy(&px[size_t(y) * size_t(w) * 4], b + size_t(y) * size_t(stride), size_t(w) * 4);
    FPDFBitmap_Destroy(bmp);
    FPDF_ClosePage(pg);
    FPDF_CloseDocument(d);
    return px;
}

static void testPreservedContent(const std::string& outDir) {
    std::printf("\n== zachowanie oryginalnej treści (CMYK, maski, nadruk, tekst, obraz wbudowany, cieniowanie)\n");
    const std::string contentA =
        "q\n0.1 0 0.9 0 k 50 700 200 80 re f\n/CS0 cs 1 scn 300 700 200 80 re f\n"
        "/GS1 gs 1 0 0 0 k 50 560 200 100 re f\nQ\n"
        "q /GS2 gs 0 1 0 0 k 300 560 200 100 re f Q\n"
        "BT /F1 18 Tf 0 0 0 1 k 2 Tc 50 470 Td [(Kawa) -400 (Tekst)] TJ 0 -30 Td 150 Tz (Szeroki) Tj "
        "100 Tz 5 Tw 0 -30 Td (a b c) Tj 4 Ts (r) Tj ET\n"
        "q 40 0 0 40 300 440 cm BI /W 2 /H 2 /CS /G /BPC 8 ID \x10\x80\xC0\x40 EI Q\n"
        "q 300 300 200 100 re W n /Sh0 sh Q\n"
        "/OC /oc1 BDC 0.5 0 0 0 k 400 200 100 50 re f EMC\n"
        "q 100 0 0 100 50 100 cm /Im1 Do Q\n";   // usuwany — PDFium przepisałby cały ten strumień
    const std::string contentB =
        "q 0 0.5 1 0 k 250 100 60 60 re f Q\n"
        "q 0.2 0.2 0.2 0.2 K 6 w 1 0.9 0 0 k 350 100 60 60 re B Q\n";
    std::string cmyk;
    for (int i = 0; i < 4; ++i) cmyk += std::string("\xFF\x00\x00\x00", 4);
    const auto src = rawPdf({
        "<< /Type /Catalog /Pages 2 0 R /OCProperties << /OCGs [10 0 R] /D << /OFF [10 0 R] >> >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources 4 0 R /Contents [5 0 R 6 0 R] >>",
        "<< /Font << /F1 7 0 R >> /ExtGState << /GS1 8 0 R /GS2 11 0 R >> /ColorSpace << /CS0 12 0 R >> "
        "/Shading << /Sh0 13 0 R >> /XObject << /Im1 14 0 R >> /Properties << /oc1 10 0 R >> >>",
        streamObj("", contentA),
        streamObj("", contentB),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        "<< /Type /ExtGState /SMask << /Type /Mask /S /Luminosity /G 9 0 R >> >>",
        streamObj("/Type /XObject /Subtype /Form /BBox [0 0 595 842] /Group << /S /Transparency /CS /DeviceGray >>",
                  "0.5 g 0 0 595 842 re f"),
        "<< /Type /OCG /Name (Ukryta) >>",
        "<< /Type /ExtGState /OP true /op true /OPM 1 >>",
        "[/Separation /Spot /DeviceCMYK 15 0 R]",
        "<< /ShadingType 2 /ColorSpace /DeviceCMYK /Coords [300 0 500 0] /Function 16 0 R /Extend [true true] >>",
        streamObj("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceCMYK /BitsPerComponent 8", cmyk),
        "<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0.8 0.2 0] /N 1 >>",
        "<< /FunctionType 2 /Domain [0 1] /C0 [1 0 0 0] /C1 [0 0 1 0] /N 1 >>",
    });
    Document doc;
    std::string err;
    CHECK(doc.appendBytes(src, "zachowanie.pdf", "", err) == LoadResult::Ok, "wczytanie pliku testowego %s", err.c_str());
    if (doc.pages.empty()) return;
    Page& pg = doc.pages[0];
    Obj img, mv, rc;
    CHECK(doc.pickImage(pg, pdfToBase(pg, 100, 150), img), "wybór obrazu CMYK");
    CHECK(doc.pickShape(pg, pdfToBase(pg, 280, 130), 1, mv), "wybór kształtu CMYK do przesunięcia");
    CHECK(doc.pickShape(pg, pdfToBase(pg, 380, 130), 1, rc), "wybór kształtu z konturem CMYK");
    img.hidden = true;
    pg.objs.push_back(img);
    mv.x += 30;
    pg.objs.push_back(mv);
    rc.color = {1, 0, 0, 1};
    rc.shapeEdits |= kShapeFill;
    pg.objs.push_back(rc);
    Obj ink;
    ink.type = ObjType::Ink;
    ink.size = 3;
    ink.color = {0.1f, 0.6f, 0.2f, 1};
    for (int k = 0; k <= 20; ++k) ink.pts.push_back(pdfToBase(pg, 450 + k * 5, 400));
    pg.objs.push_back(ink);
    const std::string path = outDir + "/zachowanie.pdf";
    CHECK(doc.saveToFile(path, err), "zapis %s", err.c_str());
    std::vector<uint8_t> saved;
    readFileBytes(path, saved);

    int w = 0, h = 0, w2 = 0, h2 = 0;
    const auto a = renderPdfPage(src, w, h), b = renderPdfPage(saved, w2, h2);
    CHECK(!a.empty() && a.size() == b.size(), "renderowanie oryginału i zapisu (%dx%d)", w2, h2);
    if (a.empty() || a.size() != b.size()) return;
    struct R { float l, b, r, t; };
    // (warstwa ukryta: zapis kopiuje strony do nowego dokumentu bez ustawień warstw katalogu — /OCProperties —
    //  więc jej widoczność sprawdzamy po znaczniku w treści, nie po wyglądzie)
    const R changed[] = {{50, 100, 150, 200}, {250, 100, 340, 160}, {347, 97, 413, 163}, {440, 390, 560, 410}, {400, 200, 500, 250}};
    auto inChanged = [&](int x, int y) {
        const float px = float(x) + 0.5f, py = float(h - y) - 0.5f;
        for (const R& r : changed)
            if (px > r.l - 4 && px < r.r + 4 && py > r.b - 4 && py < r.t + 4) return true;
        return false;
    };
    // piksel w punkcie PDF (x, y); bufor BGRA
    auto at = [&](const std::vector<uint8_t>& v, float x, float y) { return &v[(size_t(h - 1 - int(y)) * size_t(w) + size_t(x)) * 4]; };
    auto same = [](const uint8_t* p, const uint8_t* q, int tol) {
        for (int i = 0; i < 3; ++i)
            if (std::abs(int(p[i]) - int(q[i])) > tol) return false;
        return true;
    };
    int diff = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (!inChanged(x, y) && !same(&a[(size_t(y) * size_t(w) + size_t(x)) * 4], &b[(size_t(y) * size_t(w) + size_t(x)) * 4], 8)) ++diff;
    CHECK(diff == 0, "poza zmienionymi miejscami strona wygląda jak oryginał (różnych pikseli: %d)", diff);
    CHECK(same(at(a, 120, 740), at(b, 120, 740), 2) && at(b, 120, 740)[1] > 150, "kolor CMYK zachowany");
    CHECK(same(at(a, 400, 740), at(b, 400, 740), 2), "kolor dodatkowy (Separation) zachowany");
    CHECK(same(at(a, 120, 610), at(b, 120, 610), 2), "maska przezroczystości zachowana");
    {
        bool ocMark = false;
        FPDF_DOCUMENT d = FPDF_LoadMemDocument64(saved.data(), saved.size(), nullptr);
        FPDF_PAGE sp = d ? FPDF_LoadPage(d, 0) : nullptr;
        for (int k = 0; sp && k < FPDFPage_CountObjects(sp); ++k) {
            FPDF_PAGEOBJECT o = FPDFPage_GetObject(sp, k);
            for (int m = 0; m < FPDFPageObj_CountMarks(o); ++m) {
                unsigned short name[16] = {};
                unsigned long len = 0;
                if (FPDFPageObjMark_GetName(FPDFPageObj_GetMark(o, (unsigned long)m), name, sizeof name, &len) && name[0] == 'O' && name[1] == 'C' && name[2] == 0)
                    ocMark = true;
            }
        }
        if (sp) FPDF_ClosePage(sp);
        if (d) FPDF_CloseDocument(d);
        CHECK(ocMark, "znacznik warstwy (/OC … BDC) zachowany w treści");
    }
    CHECK(same(at(a, 290, 130), at(b, 320, 130), 2) && at(b, 255, 130)[0] > 250, "przesunięty kształt zachował kolor CMYK");
    const uint8_t* fill = at(b, 380, 130);
    CHECK(fill[2] > 200 && fill[1] < 60 && fill[0] < 60, "nowe wypełnienie kształtu (BGR %d %d %d)", fill[0], fill[1], fill[2]);
    CHECK(same(at(a, 349, 130), at(b, 349, 130), 6), "kontur CMYK kształtu z nowym wypełnieniem zachowany");
    CHECK(at(b, 100, 150)[0] > 250 && at(b, 100, 150)[2] > 250, "usunięty obraz zniknął");
    const uint8_t* ink2 = at(b, 500, 400);
    CHECK(ink2[1] > ink2[2] + 40, "nowa adnotacja odręczna jest na stronie");
}

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? argv[1] : "selftest-out";
    std::filesystem::create_directories(outDir);
    Document::initLibrary();
    testRotationsAndOverlays(outDir);
    for (int r : {0, 1}) {
        testTextEditing(outDir, r);
        testRedaction(outDir, r);
    }
    testParagraphs(outDir);
    testPageImages(outDir, 0);
    testPageImages(outDir, 1);
    testUntouchedSelection(outDir);
    testBlankPages(outDir);
    testLinks(outDir);
    testShapes(outDir, 0);
    testShapes(outDir, 1);
    testPreservedContent(outDir);
    {
        Page p; p.width = 300; p.height = 500;
        bool inv = true;
        for (int r = 0; r < 4; ++r) {
            p.extraRot = r;
            Vec2 b{37, 81}, s = baseToShown(p, b), b2 = shownToBase(p, s);
            inv &= std::fabs(b2.x - b.x) < 1e-4f && std::fabs(b2.y - b.y) < 1e-4f;
        }
        CHECK(inv, "baseToShown/shownToBase są odwrotne dla 0/90/180/270°");
    }
    Document::destroyLibrary();
    std::printf(failures ? "\n%d błędów\n" : "\nWszystkie testy przeszły.\n", failures);
    return failures ? 1 : 0;
}
