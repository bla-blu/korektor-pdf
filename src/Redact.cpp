// Zamazywanie z usunięciem treści (redakcja).
//
// Pod prostokątem:
//  * tekst — znaki zakryte przynajmniej w 40% są usuwane; pozostałe znaki tego samego obiektu
//    wracają na stronę jako osobne obiekty w dokładnie tych samych miejscach,
//  * obrazy — piksele pod prostokątem są zamalowywane na biało (powstaje nowy obraz),
//  * grafika wektorowa leżąca w całości pod prostokątem jest usuwana,
// a na wierzchu ląduje biały prostokąt.

#include "PdfUtil.h"

#include <fpdf_text.h>
#include <fpdf_transformpage.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace kpdf::detail {

namespace {

struct Node {
    FPDF_PAGEOBJECT obj;
    FPDF_PAGEOBJECT parent;   // obiekt formularza (XObject) albo nullptr dla najwyższego poziomu
    FPDF_PAGEOBJECT top;      // przodek na najwyższym poziomie strony
    Xf toPage;                // przestrzeń kontenera obiektu -> przestrzeń strony
};

void collect(FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT parent, FPDF_PAGEOBJECT top, const Xf& toPage, std::vector<Node>& out) {
    out.push_back({obj, parent, top, toPage});
    if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_FORM) return;
    FS_MATRIX m;
    if (!FPDFPageObj_GetMatrix(obj, &m)) return;
    const Xf child = Xf::from(m).then(toPage);
    const int n = FPDFFormObj_CountObjects(obj);
    for (int i = 0; i < n; ++i) collect(FPDFFormObj_GetObject(obj, (unsigned long)i), obj, top, child, out);
}

std::vector<Node> collectAll(FPDF_PAGE page) {
    std::vector<Node> out;
    const int n = FPDFPage_CountObjects(page);
    for (int i = 0; i < n; ++i) {
        FPDF_PAGEOBJECT o = FPDFPage_GetObject(page, i);
        collect(o, nullptr, o, Xf{}, out);
    }
    return out;
}

// Granice obiektu w przestrzeni strony.
bool pageBounds(const Node& nd, float& l, float& b, float& r, float& t) {
    float x0, y0, x1, y1;
    if (!FPDFPageObj_GetBounds(nd.obj, &x0, &y0, &x1, &y1)) return false;
    l = b = 1e30f;
    r = t = -1e30f;
    for (Vec2 c : {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x0, y1}, Vec2{x1, y1}}) {
        const Vec2 p = nd.toPage.apply(c);
        l = std::min(l, p.x); r = std::max(r, p.x); b = std::min(b, p.y); t = std::max(t, p.y);
    }
    return true;
}

struct Box {
    float l, b, r, t;
    bool contains(float x, float y) const { return x >= l && x <= r && y >= b && y <= t; }
    bool containsBox(float x0, float y0, float x1, float y1) const { return x0 >= l && x1 <= r && y0 >= b && y1 <= t; }
    bool intersects(float x0, float y0, float x1, float y1) const { return x0 < r && x1 > l && y0 < t && y1 > b; }
};

// Zamalowuje piksele obrazu leżące w prostokącie; zwraca true, jeśli coś zmieniono.
bool redactImage(PageEdit& pe, const Node& nd, const Box& box) {
    FS_MATRIX fm;
    if (!FPDFPageObj_GetMatrix(nd.obj, &fm)) return false;
    const Xf img = Xf::from(fm).then(nd.toPage);   // kwadrat jednostkowy obrazu -> strona

    // Obraz z przezroczystością i prostą macierzą: wersja z nałożoną maską (zachowuje przezroczystość).
    const bool upright = std::fabs(img.b) < 1e-4f && std::fabs(img.c) < 1e-4f && img.a > 0 && img.d > 0;
    FPDF_BITMAP src = nullptr;
    if (upright && FPDFPageObj_HasTransparency(nd.obj)) src = FPDFImageObj_GetRenderedBitmap(pe.doc, pe.page, nd.obj);
    if (!src) src = FPDFImageObj_GetBitmap(nd.obj);
    if (!src) {   // nie da się zdekodować — dla bezpieczeństwa usuwamy cały obraz
        pe.remove(nd.obj, nd.parent);
        return true;
    }
    const int w = FPDFBitmap_GetWidth(src), h = FPDFBitmap_GetHeight(src);
    const int fmt = FPDFBitmap_GetFormat(src);
    const int bpp = fmt == FPDFBitmap_Gray ? 1 : fmt == FPDFBitmap_BGR ? 3 : 4;
    const int sstride = FPDFBitmap_GetStride(src);
    const auto* sp = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(src));

    // zakres pikseli do sprawdzenia
    const Xf inv = img.inverse();
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    for (Vec2 c : {Vec2{box.l, box.b}, Vec2{box.r, box.b}, Vec2{box.l, box.t}, Vec2{box.r, box.t}}) {
        const Vec2 q = inv.apply(c);
        u0 = std::min(u0, q.x); u1 = std::max(u1, q.x); v0 = std::min(v0, q.y); v1 = std::max(v1, q.y);
    }
    const int px0 = std::clamp(int(std::floor(u0 * w)) - 1, 0, w), px1 = std::clamp(int(std::ceil(u1 * w)) + 1, 0, w);
    const int py0 = std::clamp(int(std::floor((1 - v1) * h)) - 1, 0, h), py1 = std::clamp(int(std::ceil((1 - v0) * h)) + 1, 0, h);

    FPDF_BITMAP out = FPDFBitmap_Create(w, h, 1);
    auto* dp = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(out));
    const int dstride = FPDFBitmap_GetStride(out);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = sp + size_t(y) * sstride;
        uint8_t* d = dp + size_t(y) * dstride;
        for (int x = 0; x < w; ++x, d += 4) {
            const uint8_t* q = s + size_t(x) * bpp;
            if (bpp == 1) { d[0] = d[1] = d[2] = q[0]; d[3] = 255; }
            else { d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = fmt == FPDFBitmap_BGRA ? q[3] : 255; }
        }
    }
    bool changed = false;
    for (int y = py0; y < py1; ++y) {
        uint8_t* d = dp + size_t(y) * dstride;
        for (int x = px0; x < px1; ++x) {
            const Vec2 p = img.apply({(x + 0.5f) / w, 1.f - (y + 0.5f) / h});
            if (box.contains(p.x, p.y)) {
                uint8_t* q = d + size_t(x) * 4;
                q[0] = q[1] = q[2] = q[3] = 255;
                changed = true;
            }
        }
    }
    FPDFBitmap_Destroy(src);
    if (!changed) { FPDFBitmap_Destroy(out); return false; }

    FPDF_PAGEOBJECT neu = FPDFPageObj_NewImageObj(pe.doc);
    FPDFImageObj_SetBitmap(&pe.page, 1, neu, out);
    FPDFBitmap_Destroy(out);
    FPDFImageObj_SetMatrix(neu, img.a, img.b, img.c, img.d, img.e, img.f);
    pe.insertAt(neu, nd.top);
    pe.remove(nd.obj, nd.parent);
    return true;
}

} // namespace

// Przenosi zawartość osadzonej grafiki (Form XObject) na stronę, żeby dało się ją zmienić.
// PDFium zapisuje zmiany tylko w treści samej strony, nie wewnątrz takich grafik.
void flattenForm(PageEdit& pe, FPDF_PAGEOBJECT form, int index) {
    FS_MATRIX fm;
    if (!FPDFPageObj_GetMatrix(form, &fm)) return;
    int k = 0;
    while (FPDFFormObj_CountObjects(form) > 0) {
        FPDF_PAGEOBJECT child = FPDFFormObj_GetObject(form, 0);
        if (!FPDFFormObj_RemoveObject(form, child)) break;
        FPDFPageObj_Transform(child, fm.a, fm.b, fm.c, fm.d, fm.e, fm.f);
        FPDFPageObj_TransformClipPath(child, fm.a, fm.b, fm.c, fm.d, fm.e, fm.f);
        FPDFPage_InsertObjectAtIndex(pe.page, child, size_t(index + k));
        ++k;
    }
    pe.remove(form);
}

void redactArea(PageEdit& pe, float l, float b, float r, float t) {
    const Box box{l, b, r, t};

    // ---------- 0) osadzone grafiki zachodzące na prostokąt rozkładamy na zwykłe obiekty strony
    for (bool again = true; again;) {
        again = false;
        const int n = FPDFPage_CountObjects(pe.page);
        for (int i = 0; i < n; ++i) {
            FPDF_PAGEOBJECT o = FPDFPage_GetObject(pe.page, i);
            if (FPDFPageObj_GetType(o) != FPDF_PAGEOBJ_FORM) continue;
            float x0, y0, x1, y1;
            if (!FPDFPageObj_GetBounds(o, &x0, &y0, &x1, &y1) || !box.intersects(x0, y0, x1, y1)) continue;
            flattenForm(pe, o, i);
            again = true;
            break;
        }
    }

    // ---------- 1) tekst, znak po znaku
    struct Ch { char32_t u; bool covered; double ox, oy; };
    std::map<FPDF_PAGEOBJECT, std::vector<Ch>> byObj;
    std::map<FPDF_FONT, std::u32string> okByFont;
    if (FPDF_TEXTPAGE tp = FPDFText_LoadPage(pe.page)) {
        const int n = FPDFText_CountChars(tp);
        for (int k = 0; k < n; ++k) {
            if (FPDFText_IsGenerated(tp, k) == 1) continue;
            FPDF_PAGEOBJECT o = FPDFText_GetTextObject(tp, k);
            if (!o) continue;
            double cl, cr, cb, ct;
            if (!FPDFText_GetCharBox(tp, k, &cl, &cr, &cb, &ct)) continue;
            double ox = cl, oy = cb;
            FPDFText_GetCharOrigin(tp, k, &ox, &oy);
            const double area = (cr - cl) * (ct - cb);
            const double ix = std::max(0.0, std::min<double>(cr, r) - std::max<double>(cl, l));
            const double iy = std::max(0.0, std::min<double>(ct, t) - std::max<double>(cb, b));
            const bool covered = area > 1e-6 ? ix * iy >= 0.4 * area : box.contains(float(ox), float(oy));
            unsigned u = FPDFText_GetUnicode(tp, k);
            if (u == 2) u = '-';   // dywiz na końcu wiersza
            byObj[o].push_back({char32_t(u), covered, ox, oy});
        }
        for (auto& [o, chars] : byObj) {
            const bool any = std::any_of(chars.begin(), chars.end(), [](const Ch& c) { return c.covered; });
            FPDF_FONT f = FPDFTextObj_GetFont(o);
            if (any && f && !okByFont.count(f)) okByFont[f] = observedChars(tp, f, FPDFFont_GetIsEmbedded(f) == 1);
        }
        FPDFText_ClosePage(tp);
    }

    std::map<FPDF_PAGEOBJECT, Node> nodeOf;
    for (const Node& nd : collectAll(pe.page)) nodeOf[nd.obj] = nd;

    for (auto& [o, chars] : byObj) {
        if (!std::any_of(chars.begin(), chars.end(), [](const Ch& c) { return c.covered; })) continue;
        auto it = nodeOf.find(o);
        if (it == nodeOf.end()) continue;
        const Node nd = it->second;
        const bool all = std::all_of(chars.begin(), chars.end(), [](const Ch& c) { return c.covered; });
        if (!all) {
            // pozostałe znaki obiektu wracają jako osobne obiekty w tych samych miejscach
            FPDF_FONT f = FPDFTextObj_GetFont(o);
            float fs = 12;
            FPDFTextObj_GetFontSize(o, &fs);
            FS_MATRIX fm;
            FPDFPageObj_GetMatrix(o, &fm);
            const Xf m = Xf::from(fm).then(nd.toPage);
            unsigned fr = 0, fg = 0, fb = 0, fa = 255, sr = 0, sg = 0, sb = 0, sa = 255;
            FPDFPageObj_GetFillColor(o, &fr, &fg, &fb, &fa);
            FPDFPageObj_GetStrokeColor(o, &sr, &sg, &sb, &sa);
            const int rm = FPDFTextObj_GetTextRenderMode(o);
            const std::u32string& ok = okByFont[f];
            int fam = FamilySans;
            bool bold = false, italic = false;
            if (f) guessStyle(f, fam, bold, italic);
            // Pozostałe znaki składamy w ciągi: kolejny znak dołącza do ciągu, jeśli stoi dokładnie
            // tam, gdzie postawiłaby go czcionka; inaczej (kerning, odstępy) zaczyna nowy obiekt.
            FPDF_FONT fallback = pe.fonts->get(fam, bold, italic);
            std::u32string run;
            FPDF_FONT runFont = nullptr;
            double runX = 0, runY = 0, nextX = 0, nextY = 0;
            auto flush = [&] {
                if (run.empty() || !runFont) { run.clear(); return; }
                if (run.find_first_not_of(U' ') != std::u32string::npos) {
                    FPDF_PAGEOBJECT neu = FPDFPageObj_CreateTextObj(pe.doc, runFont, fs);
                    const auto w16 = toUtf16z(run);
                    FPDFText_SetText(neu, reinterpret_cast<FPDF_WIDESTRING>(w16.data()));
                    FPDFPageObj_SetFillColor(neu, fr, fg, fb, fa);
                    FPDFPageObj_SetStrokeColor(neu, sr, sg, sb, sa);
                    if (rm > 0) FPDFTextObj_SetTextRenderMode(neu, FPDF_TEXT_RENDERMODE(rm));
                    FPDFPageObj_Transform(neu, m.a, m.b, m.c, m.d, float(runX), float(runY));
                    pe.insertAt(neu, nd.top);
                }
                run.clear();
            };
            for (const Ch& c : chars) {
                if (c.covered || c.u < U' ') { flush(); continue; }
                FPDF_FONT use = f && ok.find(c.u) != std::u32string::npos ? f : fallback;
                if (!use || (c.u == U' ' && use == fallback && f)) { flush(); continue; }
                const bool contiguous = !run.empty() && use == runFont &&
                                        std::fabs(c.ox - nextX) < 0.3 && std::fabs(c.oy - nextY) < 0.3;
                if (!contiguous) {
                    flush();
                    runFont = use;
                    runX = c.ox;
                    runY = c.oy;
                    nextX = c.ox;
                    nextY = c.oy;
                }
                run.push_back(c.u);
                float w = 0;
                FPDFFont_GetGlyphWidth(use, uint32_t(c.u), fs, &w);
                nextX += m.a * w;
                nextY += m.b * w;
            }
            flush();
        }
        pe.remove(o, nd.parent);
    }

    // ---------- 2) obrazy i grafika
    const std::vector<Node> nodes = collectAll(pe.page);
    std::map<FPDF_PAGEOBJECT, FPDF_PAGEOBJECT> parentOf;
    for (const Node& nd : nodes) parentOf[nd.obj] = nd.parent;
    std::map<FPDF_PAGEOBJECT, bool> gone;
    auto underRemoved = [&](FPDF_PAGEOBJECT p) {
        for (; p; p = parentOf[p]) if (gone.count(p)) return true;
        return false;
    };
    for (const Node& nd : nodes) {
        if (pe.added.count(nd.obj) || underRemoved(nd.obj)) continue;
        const int type = FPDFPageObj_GetType(nd.obj);
        if (type == FPDF_PAGEOBJ_TEXT) continue;
        float x0, y0, x1, y1;
        if (!pageBounds(nd, x0, y0, x1, y1) || !box.intersects(x0, y0, x1, y1)) continue;
        if (box.containsBox(x0, y0, x1, y1)) {   // w całości pod prostokątem — usuwamy
            pe.remove(nd.obj, nd.parent);
            gone[nd.obj] = true;
            continue;
        }
        if (type == FPDF_PAGEOBJ_IMAGE && redactImage(pe, nd, box)) gone[nd.obj] = true;
    }

    // ---------- 3) biały prostokąt na wierzchu
    FPDF_PAGEOBJECT cover = FPDFPageObj_CreateNewRect(l, b, r - l, t - b);
    FPDFPageObj_SetFillColor(cover, 255, 255, 255, 255);
    FPDFPath_SetDrawMode(cover, FPDF_FILLMODE_WINDING, 0);
    pe.insertAt(cover, nullptr);
}

} // namespace kpdf::detail
