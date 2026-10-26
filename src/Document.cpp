#include "Document.h"
#include "ContentSplice.h"
#include "PdfUtil.h"

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace kpdf {

using namespace detail;

// ============================================================================ UTF

std::u32string utf8To32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = uint8_t(s[i]);
        int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        if (extra) c &= (0x3F >> extra);
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) c = (c << 6) | (uint8_t(s[i]) & 0x3F);
        out.push_back(char32_t(c));
    }
    return out;
}

std::string utf32To8(const std::u32string& w) {
    std::string s;
    for (char32_t c : w) {
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | c >> 6); s += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += char(0xE0 | c >> 12); s += char(0x80 | (c >> 6 & 63)); s += char(0x80 | (c & 63)); }
        else { s += char(0xF0 | c >> 18); s += char(0x80 | (c >> 12 & 63)); s += char(0x80 | (c >> 6 & 63)); s += char(0x80 | (c & 63)); }
    }
    return s;
}

// ============================================================================ Obj

const TextStyle& Obj::styleAt(size_t ch) const {
    static const TextStyle def;
    if (styles.empty()) return def;
    size_t i = ch < charStyle.size() ? charStyle[ch] : (charStyle.empty() ? 0 : charStyle.back());
    return i < styles.size() ? styles[i] : styles[0];
}

void Obj::normalizeStyles() {
    if (type != ObjType::Text) return;
    if (styles.empty()) styles.push_back(TextStyle{});
    const size_t n = utf8To32(text).size();
    const uint16_t fill = charStyle.empty() ? 0 : charStyle.back();
    charStyle.resize(n, fill);
    for (auto& c : charStyle) if (c >= styles.size()) c = 0;
    if (n == 0) return;   // pusty tekst: style zostają (wpisywanie zacznie od pierwszego)
    std::vector<int> remap(styles.size(), -1);
    std::vector<TextStyle> used;
    for (auto& c : charStyle) {
        if (remap[c] < 0) {
            // ten sam wygląd pod innym indeksem łączymy w jeden styl
            int found = -1;
            for (size_t k = 0; k < used.size(); ++k) if (used[k] == styles[c]) found = int(k);
            if (found < 0) { found = int(used.size()); used.push_back(styles[c]); }
            remap[c] = found;
        }
        c = uint16_t(remap[c]);
    }
    styles = std::move(used);
}

uint64_t Obj::contentHash() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* d, size_t n) {
        const auto* p = static_cast<const uint8_t*>(d);
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    };
    auto mixf = [&](float v) { mix(&v, sizeof v); };
    mix(text.data(), text.size());
    mixf(x); mixf(y); mixf(boxWidth); mixf(lineStep); mixf(firstIndent);
    mixf(adv.x); mixf(adv.y); mixf(up.x); mixf(up.y);
    const int al = int(align);
    mix(&al, sizeof al);
    for (const TextStyle& st : styles) {
        mixf(st.size); mixf(st.color.r); mixf(st.color.g); mixf(st.color.b); mixf(st.color.a);
        mix(&st.renderMode, sizeof st.renderMode);
        mix(&st.font.family, sizeof st.font.family);
        const uint8_t bi = uint8_t((st.font.bold ? 1 : 0) | (st.font.italic ? 2 : 0));
        mix(&bi, 1);
        mix(&st.font.origObj, sizeof st.font.origObj);
        mixf(st.font.fit);
    }
    if (!charStyle.empty()) mix(charStyle.data(), charStyle.size() * sizeof(uint16_t));
    return h | 1;   // nigdy 0 (0 = „nie z pliku”)
}

namespace detail {

std::vector<unsigned short> toUtf16z(const std::u32string& s) {
    std::vector<unsigned short> out;
    for (char32_t c : s) {
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<unsigned short>(0xD800 + (c >> 10)));
            out.push_back(static_cast<unsigned short>(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(static_cast<unsigned short>(c));
        }
    }
    out.push_back(0);
    return out;
}

// ============================================================================ geometria

Xf Xf::inverse() const {
    const float det = a * d - b * c;
    if (std::fabs(det) < 1e-12f) return {};
    Xf r;
    r.a = d / det; r.b = -b / det; r.c = -c / det; r.d = a / det;
    r.e = -(r.a * e + r.c * f);
    r.f = -(r.b * e + r.d * f);
    return r;
}

Xf Xf::then(const Xf& n) const {
    return {n.a * a + n.c * b, n.b * a + n.d * b, n.a * c + n.c * d, n.b * c + n.d * d,
            n.a * e + n.c * f + n.e, n.b * e + n.d * f + n.f};
}

Xf baseToPdf(const Page& p) {
    const float w = p.baseRot % 2 ? p.height : p.width;   // wymiary w przestrzeni PDF
    const float h = p.baseRot % 2 ? p.width : p.height;
    Xf m;
    switch (p.baseRot) {
        case 1: m = {0, 1, -1, 0, w, 0}; break;
        case 2: m = {-1, 0, 0, -1, w, h}; break;
        case 3: m = {0, -1, 1, 0, 0, h}; break;
        default: break;
    }
    m.e += p.cropX;
    m.f += p.cropY;
    const Xf flip{1, 0, 0, -1, 0, p.height};   // oś Y w dół -> oś Y w górę
    return flip.then(m);
}

// ============================================================================ czcionki

FPDF_FONT FontCache::get(int family, bool bold, bool italic) {
    family = std::clamp(family, 0, 2);
    const int i = family * 4 + (bold ? 1 : 0) + (italic ? 2 : 0);
    if (!fonts[i] && doc) {
        const FontFile& f = fontFile(family, bold, italic);
        fonts[i] = FPDFText_LoadFont(doc, f.data, uint32_t(f.size), FPDF_FONT_TRUETYPE, 1);
    }
    return fonts[i];
}

void FontCache::close() {
    for (auto& f : fonts) if (f) { FPDFFont_Close(f); f = nullptr; }
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static std::string fontName(FPDF_FONT f) {
    if (!f) return {};
    char buf[256] = {};
    FPDFFont_GetBaseFontName(f, buf, sizeof buf);
    std::string n = buf;
    if (n.size() > 7 && n[6] == '+') n = n.substr(7);   // prefiks podzbioru "ABCDEF+"
    return n;
}

void guessStyle(FPDF_FONT font, int& family, bool& bold, bool& italic) {
    const std::string n = lower(fontName(font));
    const int flags = FPDFFont_GetFlags(font);
    auto has = [&](std::initializer_list<const char*> ws) {
        for (auto w : ws) if (n.find(w) != std::string::npos) return true;
        return false;
    };
    family = FamilySans;
    if (has({"courier", "mono", "consol", "code", "typewriter"}) || (flags & 1)) family = FamilyMono;
    else if (!has({"sans", "arial", "helvetica", "verdana", "calibri", "segoe", "tahoma"}) &&
             (has({"times", "serif", "roman", "georgia", "garamond", "cambria", "minion", "palatino",
                   "caslon", "baskerville", "book", "century"}) || (flags & 2)))
        family = FamilySerif;
    const int weight = FPDFFont_GetWeight(font);
    bold = has({"bold", "black", "heavy", "semibold", "demi"}) || weight >= 600 || (flags & (1 << 18));
    int angle = 0;
    FPDFFont_GetItalicAngle(font, &angle);
    italic = has({"italic", "oblique"}) || (flags & 64) || angle != 0;
}

std::u32string observedChars(FPDF_TEXTPAGE tp, FPDF_FONT font, bool embedded) {
    std::set<char32_t> s;
    const int n = FPDFText_CountChars(tp);
    for (int k = 0; k < n; ++k) {
        if (FPDFText_IsGenerated(tp, k) == 1 || FPDFText_HasUnicodeMapError(tp, k) == 1) continue;
        FPDF_PAGEOBJECT o = FPDFText_GetTextObject(tp, k);
        if (!o || FPDFTextObj_GetFont(o) != font) continue;
        const unsigned u = FPDFText_GetUnicode(tp, k);
        if (u >= 32) s.insert(char32_t(u));
    }
    auto hasGlyph = [&](char32_t c, bool blank) {
        FPDF_GLYPHPATH gp = FPDFFont_GetGlyphPath(font, uint32_t(c), 10.f);
        float w = 0;
        const bool okW = FPDFFont_GetGlyphWidth(font, uint32_t(c), 10.f, &w) && w > 0;
        if (!gp || !okW) return false;
        const int segs = FPDFGlyphPath_CountGlyphSegments(gp);
        return blank ? segs == 0 : segs > 0;
    };
    if (!s.count(U' ') && hasGlyph(U' ', true)) s.insert(U' ');
    // Czcionki nieosadzone (np. standardowa Helvetica) mają pełny zestaw znaków Latin-1.
    if (!embedded)
        for (char32_t c = 33; c < 256; ++c)
            if (c < 127 || c > 160) if (hasGlyph(c, false)) s.insert(c);
    return std::u32string(s.begin(), s.end());
}

bool charsOk(const std::u32string& text, const std::u32string& ok) {
    for (char32_t c : text)
        if (c != U'\n' && ok.find(c) == std::u32string::npos) return false;
    return true;
}

// ============================================================================ nakładanie zmian

void PageEdit::remove(FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT parentForm) {
    const bool ok = parentForm ? FPDFFormObj_RemoveObject(parentForm, obj) : FPDFPage_RemoveObject(page, obj);
    if (ok) removed.push_back(obj);
    if (ok && parentForm) touchedForms.insert(parentForm);
}

int PageEdit::indexOf(FPDF_PAGEOBJECT obj) const {
    const int n = FPDFPage_CountObjects(page);
    for (int i = 0; i < n; ++i) if (FPDFPage_GetObject(page, i) == obj) return i;
    return -1;
}

void PageEdit::insertAt(FPDF_PAGEOBJECT obj, FPDF_PAGEOBJECT before) {
    const int idx = before ? indexOf(before) : -1;
    if (idx >= 0) FPDFPage_InsertObjectAtIndex(page, obj, size_t(idx));
    else FPDFPage_InsertObject(page, obj);
    added.insert(obj);
}

void PageEdit::destroyRemoved() {
    for (auto o : removed) FPDFPageObj_Destroy(o);
    removed.clear();
}

static int styleIndex(const Obj& o, size_t i) {
    int s = 0;
    if (i < o.charStyle.size()) s = o.charStyle[i];
    else if (!o.charStyle.empty()) s = o.charStyle.back();
    return s < int(o.styles.size()) ? s : 0;
}

static StyleFont resolveStyle(const PageEdit& pe, const Obj& o, int si, const std::u32string& text) {
    StyleFont r;
    const TextStyle& st = o.styles[si];
    bool orig = st.font.origObj >= 0 && st.font.origObj < int(pe.pristine.size()) &&
                FPDFPageObj_GetType(pe.pristine[st.font.origObj]) == FPDF_PAGEOBJ_TEXT;
    if (orig) {
        // czcionka z pliku tylko wtedy, gdy ma każdy znak tego stylu (spacje nie są rysowane)
        const std::u32string ok = utf8To32(st.font.okChars);
        for (size_t i = 0; i < text.size() && orig; ++i)
            if (text[i] != U' ' && text[i] != U'\n' && styleIndex(o, i) == si && ok.find(text[i]) == std::u32string::npos) orig = false;
    }
    if (orig) {
        r.font = FPDFTextObj_GetFont(pe.pristine[st.font.origObj]);
        r.original = r.font != nullptr;
    }
    if (!r.font) {
        r.font = pe.fonts ? pe.fonts->get(st.font.family, st.font.bold, st.font.italic) : nullptr;
        r.hscale = st.font.fit;
    }
    r.asc = 0.8f * st.size;
    r.desc = -0.2f * st.size;
    float sp = 0;
    if (r.font) {
        FPDFFont_GetAscent(r.font, st.size, &r.asc);
        FPDFFont_GetDescent(r.font, st.size, &r.desc);
        if (!FPDFFont_GetGlyphWidth(r.font, uint32_t(U' '), st.size, &sp)) sp = 0;
    }
    if (sp <= 0.05f * st.size) sp = 0.28f * st.size;   // wiele plików nie ma znaku spacji w czcionce
    r.spaceW = sp * r.hscale;
    return r;
}

Laid layoutObj(const PageEdit& pe, const Obj& o) {
    Laid L;
    L.text = utf8To32(o.text);
    const size_t n = L.text.size();
    if (o.styles.empty()) return L;
    for (int si = 0; si < int(o.styles.size()); ++si) L.fonts.push_back(resolveStyle(pe, o, si, L.text));

    std::vector<float> w(n, 0.f);
    for (size_t i = 0; i < n; ++i) {
        const char32_t c = L.text[i];
        const int si = styleIndex(o, i);
        const StyleFont& f = L.fonts[si];
        const float size = o.styles[si].size;
        if (c == U'\n') continue;
        if (c == U' ') { w[i] = f.spaceW; continue; }
        float g = 0;
        if (!f.font || !FPDFFont_GetGlyphWidth(f.font, uint32_t(c), size, &g)) g = 0.5f * size;
        w[i] = g * f.hscale;
    }

    float y = 0;
    bool firstLine = true;
    size_t a = 0;
    while (true) {
        size_t b = L.text.find(U'\n', a);
        if (b == std::u32string::npos) b = n;
        bool firstOfPara = true;
        size_t i = a;
        do {
            const float indent = firstOfPara ? o.firstIndent : 0.f;
            const float avail = o.boxWidth - indent;
            const size_t start = i;
            size_t j = i, lastBreak = std::u32string::npos;
            float width = 0;
            while (j < b) {
                const char32_t c = L.text[j];
                if (o.boxWidth > 0 && c != U' ' && j > start && width + w[j] > avail + 0.01f) {
                    if (lastBreak != std::u32string::npos && lastBreak > start) j = lastBreak;   // przed słowem
                    break;                                                                      // albo w środku zbyt długiego słowa
                }
                width += w[j];
                if (c == U' ') lastBreak = j + 1;
                ++j;
            }
            LaidLine ln;
            ln.first = int(start);
            ln.count = int(j - start);
            size_t lastNon = j;
            while (lastNon > start && L.text[lastNon - 1] == U' ') --lastNon;
            float natural = 0;
            int inner = 0;
            for (size_t k = start; k < lastNon; ++k) {
                natural += w[k];
                if (L.text[k] == U' ') ++inner;
            }
            const bool lastOfPara = j >= b;
            float off = 0, extra = 0;
            if (o.boxWidth > 0) {
                switch (o.align) {
                    case Align::Center: off = (avail - natural) / 2; break;
                    case Align::Right: off = avail - natural; break;
                    case Align::Justify:
                        if (!lastOfPara && inner > 0 && avail > natural) extra = (avail - natural) / inner;
                        break;
                    default: break;
                }
            }
            float x = indent + off;
            ln.caret.push_back(x);
            for (size_t k = start; k < j; ++k) {
                x += w[k];
                if (L.text[k] == U' ' && k < lastNon) x += extra;
                ln.caret.push_back(x);
            }
            // wysokość wiersza
            float maxSize = 0;
            auto take = [&](int si) {
                ln.asc = std::max(ln.asc, L.fonts[si].asc);
                ln.desc = std::min(ln.desc, L.fonts[si].desc);
                maxSize = std::max(maxSize, o.styles[si].size);
            };
            for (size_t k = start; k < j; ++k) take(styleIndex(o, k));
            if (j == start) take(styleIndex(o, start > 0 ? start - 1 : 0));
            if (!firstLine) y += o.lineStep > 0 ? o.lineStep : 1.2f * maxSize;
            ln.y = y;
            firstLine = false;
            L.lines.push_back(std::move(ln));
            i = j;
            firstOfPara = false;
        } while (i < b);
        if (b >= n) break;
        a = b + 1;
    }
    return L;
}

static unsigned c8(float v) { return unsigned(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); }

void applyImageEdits(PageEdit& pe, const Page& p);

void applyContentEdits(PageEdit& pe, const Page& p) {
    applyImageEdits(pe, p);
    const Xf V = baseToPdf(p);
    std::unordered_set<FPDF_PAGEOBJECT> gone;

    for (const Obj& o : p.objs) {
        if (o.type != ObjType::Text || o.styles.empty() || o.untouched()) continue;   // tylko zaznaczony: bez zmian
        const Laid L = layoutObj(pe, o);

        // nowy tekst ląduje w miejscu zastępowanego (zachowuje kolejność rysowania)
        FPDF_PAGEOBJECT before = nullptr;
        for (int idx : o.srcObjs)
            if (idx >= 0 && idx < int(pe.pristine.size()) && !gone.count(pe.pristine[idx])) {
                FPDF_PAGEOBJECT cand = pe.pristine[idx];
                if (!before || pe.indexOf(cand) < pe.indexOf(before)) before = cand;
            }

        // Każde słowo w jednym stylu to osobny obiekt PDF: spacje (także rozciągnięte przy
        // justowaniu) są odstępami, więc nie wymagają znaku spacji w czcionce z pliku.
        for (const LaidLine& ln : L.lines) {
            int k = 0;
            while (k < ln.count) {
                const size_t gi = size_t(ln.first + k);
                if (L.text[gi] == U' ') { ++k; continue; }
                const int si = styleIndex(o, gi);
                int e = k + 1;
                while (e < ln.count && L.text[ln.first + e] != U' ' && styleIndex(o, size_t(ln.first + e)) == si) ++e;
                const StyleFont& f = L.fonts[si];
                const TextStyle& st = o.styles[si];
                if (f.font) {
                    FPDF_PAGEOBJECT t = FPDFPageObj_CreateTextObj(pe.doc, f.font, st.size);
                    if (t) {
                        const auto w16 = toUtf16z(L.text.substr(gi, size_t(e - k)));
                        FPDFText_SetText(t, reinterpret_cast<FPDF_WIDESTRING>(w16.data()));
                        FPDFPageObj_SetFillColor(t, c8(st.color.r), c8(st.color.g), c8(st.color.b), c8(st.color.a));
                        FPDFPageObj_SetStrokeColor(t, c8(st.color.r), c8(st.color.g), c8(st.color.b), c8(st.color.a));
                        if (st.renderMode > 0 && st.renderMode <= 7) FPDFTextObj_SetTextRenderMode(t, FPDF_TEXT_RENDERMODE(st.renderMode));
                        const float x = ln.caret[size_t(k)];
                        const Vec2 org{o.x + o.adv.x * x - o.up.x * ln.y, o.y + o.adv.y * x - o.up.y * ln.y};
                        const Vec2 P = V.apply(org);
                        const Vec2 A = V.lin({o.adv.x * f.hscale, o.adv.y * f.hscale});
                        const Vec2 C = V.lin(o.up);
                        FPDFPageObj_Transform(t, A.x, A.y, C.x, C.y, P.x, P.y);
                        pe.insertAt(t, before);
                    }
                }
                k = e;
            }
        }
        for (int idx : o.srcObjs) {
            if (idx < 0 || idx >= int(pe.pristine.size())) continue;
            FPDF_PAGEOBJECT src = pe.pristine[idx];
            if (gone.insert(src).second && pe.indexOf(src) >= 0) pe.remove(src);
        }
    }

    for (const Obj& o : p.objs) {
        if (o.type != ObjType::Whiteout) continue;
        const Vec2 a = V.apply({o.x, o.y}), b = V.apply({o.x + o.w, o.y + o.h});
        redactArea(pe, std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
    }
}

} // namespace detail

// ============================================================================ kształty

bool Obj::shapeIdle() const {
    auto eq = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    return type == ObjType::PageShape && !hidden && shapeEdits == 0 && eq(x, origBox.x) && eq(y, origBox.y) &&
           eq(w, origBox.w) && eq(h, origBox.h);
}

Vec2 shapePoint(const Obj& o, Vec2 p) {
    const Rect& ob = o.origBox;
    const float sx = ob.w > 0.01f ? o.w / ob.w : 1.f, sy = ob.h > 0.01f ? o.h / ob.h : 1.f;
    return {o.x + (p.x - ob.x) * sx, o.y + (p.y - ob.y) * sy};
}

namespace detail {

// Nowa ścieżka PDF z geometrii i wyglądu obiektu (współrzędne strony, macierz jednostkowa).
static FPDF_PAGEOBJECT buildShape(const Page& p, const Obj& o) {
    if (o.pts.empty() || o.segs.size() != o.pts.size()) return nullptr;
    const Xf V = baseToPdf(p);
    auto P = [&](size_t i) { return V.apply(shapePoint(o, o.pts[i])); };
    const Vec2 first = P(0);
    FPDF_PAGEOBJECT path = FPDFPageObj_CreateNewPath(first.x, first.y);
    if (!path) return nullptr;
    for (size_t i = 0; i < o.pts.size(); ++i) {
        const int t = o.segs[i] & 3;
        size_t last = i;
        if (i > 0) {
            if (t == 0) { const Vec2 q = P(i); FPDFPath_MoveTo(path, q.x, q.y); }
            else if (t == 1) { const Vec2 q = P(i); FPDFPath_LineTo(path, q.x, q.y); }
            else if (t == 2 && i + 2 < o.pts.size()) {
                const Vec2 a = P(i), b = P(i + 1), c = P(i + 2);
                FPDFPath_BezierTo(path, a.x, a.y, b.x, b.y, c.x, c.y);
                last = i + 2;
            }
        }
        if (o.segs[last] & 4) FPDFPath_Close(path);
        i = last;
    }
    FPDFPageObj_SetFillColor(path, c8(o.color.r), c8(o.color.g), c8(o.color.b), c8(o.color.a));
    FPDFPageObj_SetStrokeColor(path, c8(o.stroke.r), c8(o.stroke.g), c8(o.stroke.b), c8(o.stroke.a));
    FPDFPageObj_SetStrokeWidth(path, std::max(0.f, o.size));
    FPDFPageObj_SetLineCap(path, o.lineCap);
    FPDFPageObj_SetLineJoin(path, o.lineJoin);
    if (!o.dash.empty()) FPDFPageObj_SetDashArray(path, o.dash.data(), o.dash.size(), o.dashPhase);
    FPDFPath_SetDrawMode(path, o.fillMode, o.strokeOn ? 1 : 0);
    return path;
}

static float matrixScale(FPDF_PAGEOBJECT obj) {
    FS_MATRIX m;
    if (!FPDFPageObj_GetMatrix(obj, &m)) return 1.f;
    const float k = std::sqrt(std::fabs(m.a * m.d - m.b * m.c));
    return k > 1e-6f ? k : 1.f;
}

// Zmiany kształtu z pliku: usunięcie, przesunięcie i skala (kształt bez konturu albo skala
// jednakowa — przekształcenie samego obiektu, więc zostaje jego przestrzeń barw i obcięcie),
// kontur przy nierównej skali (nowa ścieżka, żeby linie nie zmieniły grubości), kolory i grubość.
static void applyShape(PageEdit& pe, const Page& p, const Obj& o, FPDF_PAGEOBJECT path) {
    if (o.hidden) { pe.remove(path); return; }
    const Rect& ob = o.origBox;
    const float sx = ob.w > 0.01f ? o.w / ob.w : 1.f, sy = ob.h > 0.01f ? o.h / ob.h : 1.f;
    const bool scaled = std::fabs(sx - 1) > 1e-4f || std::fabs(sy - 1) > 1e-4f;
    const bool moved = scaled || std::fabs(o.x - ob.x) > 1e-3f || std::fabs(o.y - ob.y) > 1e-3f;
    if (o.strokeOn && scaled && std::fabs(sx - sy) > 1e-3f) {
        if (FPDF_PAGEOBJECT neu = buildShape(p, o)) { pe.insertAt(neu, path); pe.remove(path); }
        return;
    }
    if (moved) {
        const Xf V = baseToPdf(p), Vi = V.inverse();
        Xf T;
        T.a = sx; T.d = sy;
        T.e = o.x - sx * ob.x;
        T.f = o.y - sy * ob.y;
        const Xf D = Vi.then(T).then(V);
        FPDFPageObj_Transform(path, D.a, D.b, D.c, D.d, D.e, D.f);
        if (FPDFPageObj_GetClipPath(path)) FPDFPageObj_TransformClipPath(path, D.a, D.b, D.c, D.d, D.e, D.f);
        auto mv = pe.moved.find(path);
        pe.moved[path] = mv != pe.moved.end() ? mv->second.then(D) : D;
    }
    const float k = matrixScale(path);   // jednostki ścieżki -> pt
    int styled = 0;
    if (o.strokeOn && (scaled || (o.shapeEdits & (kShapeWidth | kShapeStroke)))) {
        FPDFPageObj_SetStrokeWidth(path, o.size / k);
        styled |= kStyledWidth;
    }
    if ((o.shapeEdits & kShapeFill) && o.fillMode) {
        FPDFPageObj_SetFillColor(path, c8(o.color.r), c8(o.color.g), c8(o.color.b), c8(o.color.a));
        styled |= kStyledFill;
    }
    if ((o.shapeEdits & kShapeStroke) && o.strokeOn) {
        FPDFPageObj_SetStrokeColor(path, c8(o.stroke.r), c8(o.stroke.g), c8(o.stroke.b), c8(o.stroke.a));
        styled |= kStyledStroke;
    }
    if (o.shapeEdits & (kShapeFill | kShapeStroke)) {
        FPDFPath_SetDrawMode(path, o.fillMode, o.strokeOn ? 1 : 0);
        styled |= kStyledMode;
    }
    if (styled) pe.styled[path] |= styled;
}

} // namespace detail

// ============================================================================ obrazy z pliku

void imageCorners(const Obj& o, Vec2 out[4]) {
    const Vec2 box[4] = {{o.x, o.y}, {o.x + o.w, o.y}, {o.x + o.w, o.y + o.h}, {o.x, o.y + o.h}};
    const int r = ((o.imgRot % 4) + 4) % 4;
    for (int i = 0; i < 4; ++i) out[i] = box[(i + r) % 4];
}

namespace detail {

// Obiekt i łańcuch osadzonych grafik, w których leży (chain[0] = obiekt strony, back() = obraz).
static std::vector<FPDF_PAGEOBJECT> resolveChain(FPDF_PAGEOBJECT top, const std::vector<int>& path) {
    std::vector<FPDF_PAGEOBJECT> chain{top};
    for (int idx : path) {
        FPDF_PAGEOBJECT cur = chain.back();
        if (!cur || FPDFPageObj_GetType(cur) != FPDF_PAGEOBJ_FORM || idx < 0 || idx >= FPDFFormObj_CountObjects(cur)) return {};
        chain.push_back(FPDFFormObj_GetObject(cur, (unsigned long)idx));
    }
    return chain;
}

void applyImageEdits(PageEdit& pe, const Page& p) {
    struct Job { const Obj* o; std::vector<FPDF_PAGEOBJECT> chain; };
    std::vector<Job> jobs;
    // 1) najpierw wszystkie wskaźniki — rozkładanie grafik zmienia ich zawartość
    for (const Obj& o : p.objs) {
        const bool img = o.type == ObjType::PageImage, shape = o.type == ObjType::PageShape && !o.shapeIdle();
        if ((!img && !shape) || o.srcObjs.empty()) continue;
        const int top = o.srcObjs[0];
        if (top < 0 || top >= int(pe.pristine.size())) continue;
        auto chain = resolveChain(pe.pristine[size_t(top)], o.childPath);
        if (chain.empty() || FPDFPageObj_GetType(chain.back()) != (img ? FPDF_PAGEOBJ_IMAGE : FPDF_PAGEOBJ_PATH)) continue;
        jobs.push_back({&o, std::move(chain)});
    }
    // 2) obrazy wewnątrz osadzonych grafik: grafiki rozkładamy na obiekty strony (PDFium zapisuje
    //    tylko treść samej strony), dzięki czemu obraz staje się zwykłym obiektem strony
    std::unordered_set<FPDF_PAGEOBJECT> flattened;
    for (const Job& j : jobs)
        for (size_t k = 0; k + 1 < j.chain.size(); ++k) {
            FPDF_PAGEOBJECT form = j.chain[k];
            if (flattened.count(form)) continue;
            const int at = pe.indexOf(form);
            if (at < 0) break;
            flattenForm(pe, form, at);
            flattened.insert(form);
        }
    const Xf V = baseToPdf(p), Vi = V.inverse();
    for (const Job& j : jobs) {
        const Obj& o = *j.o;
        FPDF_PAGEOBJECT img = j.chain.back();
        if (pe.indexOf(img) < 0) continue;
        if (o.type == ObjType::PageShape) { applyShape(pe, p, o, img); continue; }
        if (o.hidden) { pe.remove(img); continue; }
        // przekształcenie pierwotnego prostokąta obrazu w nowy (we współrzędnych bazowych)
        const Rect& ob = o.origBox;
        if (ob.w <= 0 || ob.h <= 0) continue;
        Vec2 n[4];
        imageCorners(o, n);
        Xf T;
        T.a = (n[1].x - n[0].x) / ob.w; T.b = (n[1].y - n[0].y) / ob.w;
        T.c = (n[3].x - n[0].x) / ob.h; T.d = (n[3].y - n[0].y) / ob.h;
        T.e = n[0].x - T.a * ob.x - T.c * ob.y;
        T.f = n[0].y - T.b * ob.x - T.d * ob.y;
        const Xf D = Vi.then(T).then(V);
        if (o.image && o.image->width > 0) {   // podmiana: nowy obiekt obrazu w tym samym miejscu
            const ImageData& im = *o.image;
            FPDF_BITMAP bmp = FPDFBitmap_Create(im.width, im.height, 1);
            auto* dst = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bmp));
            const int stride = FPDFBitmap_GetStride(bmp);
            for (int y = 0; y < im.height; ++y) {
                const uint8_t* sp = im.rgba.data() + size_t(y) * size_t(im.width) * 4;
                uint8_t* d = dst + size_t(y) * size_t(stride);
                for (int x = 0; x < im.width; ++x, sp += 4, d += 4) { d[0] = sp[2]; d[1] = sp[1]; d[2] = sp[0]; d[3] = sp[3]; }
            }
            FPDF_PAGEOBJECT neu = FPDFPageObj_NewImageObj(pe.doc);
            FPDFImageObj_SetBitmap(&pe.page, 1, neu, bmp);
            FPDFBitmap_Destroy(bmp);
            FS_MATRIX m;
            FPDFPageObj_GetMatrix(img, &m);
            FPDFImageObj_SetMatrix(neu, m.a, m.b, m.c, m.d, m.e, m.f);
            pe.insertAt(neu, img);
            pe.replaced[neu] = img;
            pe.remove(img);
            img = neu;
        }
        const bool identity = std::fabs(D.a - 1) < 1e-5f && std::fabs(D.b) < 1e-5f && std::fabs(D.c) < 1e-5f &&
                              std::fabs(D.d - 1) < 1e-5f && std::fabs(D.e) < 1e-3f && std::fabs(D.f) < 1e-3f;
        if (identity) continue;
        FPDFPageObj_Transform(img, D.a, D.b, D.c, D.d, D.e, D.f);
        if (FPDFPageObj_GetClipPath(img)) FPDFPageObj_TransformClipPath(img, D.a, D.b, D.c, D.d, D.e, D.f);
        auto mv = pe.moved.find(img);
        pe.moved[img] = mv != pe.moved.end() ? mv->second.then(D) : D;
    }
    // 3) kształty dodane w edytorze (kopie) — nad treścią strony, w kolejności elementów
    for (const Obj& o : p.objs)
        if (o.type == ObjType::Shape)
            if (FPDF_PAGEOBJECT s = buildShape(p, o)) pe.insertAt(s, nullptr);
}

} // namespace detail

// ============================================================================ odnośniki

std::string normalizeUri(const std::string& in) {
    std::string s = in;
    while (!s.empty() && std::isspace(uint8_t(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
    if (s.empty()) return s;
    std::string lower;
    for (char c : s) lower += char(std::tolower(uint8_t(c)));
    const bool hasScheme = [&] {
        const size_t colon = s.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        for (size_t i = 0; i < colon; ++i)
            if (!(std::isalnum(uint8_t(s[i])) || s[i] == '+' || s[i] == '-' || s[i] == '.')) return false;
        return !(colon + 1 < s.size() && std::isdigit(uint8_t(s[colon + 1])) && lower.find("://") == std::string::npos &&
                 lower.rfind("mailto:", 0) != 0 && lower.rfind("tel:", 0) != 0);   // „host:8080” to nie schemat
    }();
    if (!hasScheme) {
        if (s.find('@') != std::string::npos && s.find('/') == std::string::npos) s = "mailto:" + s;
        else s = "https://" + s;
    }
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (c < 0x21 || c >= 0x7F) { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out;
}

std::string displayUri(const std::string& uri) {
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(uint8_t(uri[i + 1])) && std::isxdigit(uint8_t(uri[i + 2]))) {
            const int v = std::stoi(uri.substr(i + 1, 2), nullptr, 16);
            if (v >= 0x80 || v == 0x20) { out += char(v); i += 2; continue; }
        }
        out += uri[i];
    }
    return out;
}

bool linkCovered(const Page& p, const Link& l) {
    for (const Obj& o : p.objs) {
        if (o.type != ObjType::Whiteout) continue;
        const float ix = std::max(0.f, std::min(o.x + o.w, l.box.x + l.box.w) - std::max(o.x, l.box.x));
        const float iy = std::max(0.f, std::min(o.y + o.h, l.box.y + l.box.h) - std::max(o.y, l.box.y));
        if (ix * iy >= 0.3f * l.box.w * l.box.h) return true;
    }
    return false;
}

namespace detail {

// Odnośnik wewnętrzny: strona docelowa (indeks w pliku) i opcjonalnie wysokość w przestrzeni PDF.
static bool readDest(FPDF_DOCUMENT doc, FPDF_DEST dest, int& pageIndex, bool& hasY, float& y) {
    if (!dest) return false;
    pageIndex = FPDFDest_GetDestPageIndex(doc, dest);
    if (pageIndex < 0) return false;
    FPDF_BOOL hx = 0, hy = 0, hz = 0;
    FS_FLOAT x = 0, yy = 0, z = 0;
    hasY = FPDFDest_GetLocationInPage(dest, &hx, &hy, &hz, &x, &yy, &z) && hy;
    y = yy;
    if (!hasY) {
        // inne widoki: FitH/FitBH (górna krawędź), FitR (prostokąt) — np. odnośniki do bibliografii
        unsigned long np = 4;
        FS_FLOAT pr[4] = {0, 0, 0, 0};
        const unsigned long view = FPDFDest_GetView(dest, &np, pr);
        if ((view == PDFDEST_VIEW_FITH || view == PDFDEST_VIEW_FITBH) && np >= 1) { hasY = true; y = pr[0]; }
        else if (view == PDFDEST_VIEW_FITR && np >= 4) { hasY = true; y = std::max(pr[1], pr[3]); }
    }
    return true;
}

// Rodzaj odnośnika: 1 = www/e-mail, 2 = wewnętrzny, 3 = bez akcji (np. utracony cel), 0 = inna akcja (zostaje w pliku).
static int linkKind(FPDF_DOCUMENT doc, FPDF_LINK link, std::string* uri, int* page, bool* hasY, float* y) {
    int pi = -1;
    bool hy = false;
    float yy = 0;
    if (readDest(doc, FPDFLink_GetDest(doc, link), pi, hy, yy)) {
        if (page) { *page = pi; *hasY = hy; *y = yy; }
        return 2;
    }
    FPDF_ACTION act = FPDFLink_GetAction(link);
    if (!act) return 3;
    const unsigned long t = FPDFAction_GetType(act);
    if (t == PDFACTION_URI) {
        if (uri) {
            const unsigned long n = FPDFAction_GetURIPath(doc, act, nullptr, 0);
            std::string b(n, '\0');
            if (n) FPDFAction_GetURIPath(doc, act, b.data(), n);
            while (!b.empty() && b.back() == '\0') b.pop_back();
            *uri = b;
        }
        return 1;
    }
    if (t == PDFACTION_GOTO) {
        // cel może być nieczytelny (np. po skopiowaniu stron wskazuje stronę spoza dokumentu) — wtedy -1
        if (!readDest(doc, FPDFAction_GetDest(doc, act), pi, hy, yy)) pi = -1;
        if (page) { *page = pi; *hasY = hy; *y = yy; }
        return 2;
    }
    return 0;
}

// Zamiana zastępczych adresów „kpdf-goto/…” na prawdziwe odnośniki wewnętrzne. PDFium nie ma funkcji
// do ustawiania celu wewnątrz dokumentu, więc w zapisanym pliku podmieniamy słownik akcji na
// /S/GoTo z odwołaniem do obiektu strony — w tej samej długości, żeby tablica xref została poprawna.
static void resolveGotoPlaceholders(std::vector<uint8_t>& bytes) {
    std::string b(bytes.begin(), bytes.end());
    if (b.find("(kpdf-goto/") == std::string::npos) return;
    auto objText = [&](int num) -> std::string {
        const std::string key = std::to_string(num) + " 0 obj";
        size_t at = 0;
        while ((at = b.find(key, at)) != std::string::npos) {
            if (at == 0 || b[at - 1] == '\n' || b[at - 1] == '\r') {
                const size_t end = b.find("endobj", at);
                return b.substr(at, end == std::string::npos ? std::string::npos : end - at);
            }
            at += key.size();
        }
        return {};
    };
    auto refAfter = [](const std::string& t, const char* name) -> int {
        const size_t k = t.find(name);
        if (k == std::string::npos) return -1;
        return std::atoi(t.c_str() + k + std::strlen(name));
    };
    // drzewo stron od katalogu
    const size_t root = b.rfind("/Root");
    if (root == std::string::npos) return;
    const int catalog = std::atoi(b.c_str() + root + 5);
    std::vector<int> pagesInOrder;
    std::function<void(int, int)> walk = [&](int num, int depth) {
        if (depth > 32) return;
        const std::string t = objText(num);
        const size_t kids = t.find("/Kids");
        if (t.find("/Type/Pages") != std::string::npos || t.find("/Type /Pages") != std::string::npos || kids != std::string::npos) {
            if (kids == std::string::npos) return;
            const size_t a = t.find('[', kids), e = t.find(']', kids);
            if (a == std::string::npos || e == std::string::npos) return;
            std::string arr = t.substr(a + 1, e - a - 1);
            size_t i = 0;
            while (i < arr.size()) {
                while (i < arr.size() && !std::isdigit(uint8_t(arr[i]))) ++i;
                if (i >= arr.size()) break;
                const int n = std::atoi(arr.c_str() + i);
                while (i < arr.size() && arr[i] != 'R') ++i;
                ++i;
                walk(n, depth + 1);
            }
        } else {
            pagesInOrder.push_back(num);
        }
    };
    walk(refAfter(objText(catalog), "/Pages"), 0);

    size_t at = 0;
    while ((at = b.find("(kpdf-goto/", at)) != std::string::npos) {
        const size_t open = b.rfind("<<", at), close = b.find(">>", b.find(')', at));
        if (open == std::string::npos || close == std::string::npos) break;
        int page = -1;
        float y = 0;
        int hasY = 0;
        std::sscanf(b.c_str() + at + 11, "%d/%d/%f", &page, &hasY, &y);
        std::string repl;
        if (page >= 0 && page < int(pagesInOrder.size())) {
            char d[160];
            if (hasY) std::snprintf(d, sizeof d, "<</S/GoTo/D[%d 0 R/XYZ null %.2f null]", pagesInOrder[size_t(page)], y);
            else std::snprintf(d, sizeof d, "<</S/GoTo/D[%d 0 R/Fit]", pagesInOrder[size_t(page)]);
            repl = d;
        } else {
            repl = "<<";   // cel nie istnieje: pusty słownik akcji
        }
        const size_t len = close - open;   // bez końcowego „>>”
        if (repl.size() > len) { at += 11; continue; }
        repl.append(len - repl.size(), ' ');
        b.replace(open, len, repl);
        at = open + len;
    }
    bytes.assign(b.begin(), b.end());
}

} // namespace detail

// ============================================================================ Document

struct Document::Source {
    std::vector<uint8_t> bytes; // PDFium czyta z tego bufora przez cały czas życia dokumentu
    FPDF_DOCUMENT doc = nullptr;
    std::string name;
    FontCache fonts;
    ~Source() {
        fonts.close();
        if (doc) FPDF_CloseDocument(doc);
    }
};

struct Document::Live {
    PageEdit pe;
    FPDF_PAGE ref = nullptr;   // nieedytowana kopia strony (wybór i eksport obrazów z pliku)
    FPDF_TEXTPAGE tp = nullptr; // tekst żywej strony (szybkie trafienia kursorem)
    uint64_t version = ~0ull;
    int src = -1, idx = -1;
    int lastUse = 0;
    void teardown() {
        if (tp) FPDFText_ClosePage(tp);   // strona tekstu przed samą stroną
        tp = nullptr;
        pe.destroyRemoved();
        if (pe.page) FPDF_ClosePage(pe.page);
        if (ref) FPDF_ClosePage(ref);
        ref = nullptr;
        pe.page = nullptr;
        pe.pristine.clear();
        pe.added.clear();
    }
    ~Live() { teardown(); }
};

void Document::initLibrary() {
    FPDF_LIBRARY_CONFIG cfg{};
    cfg.version = 2;
    FPDF_InitLibraryWithConfig(&cfg);
}
void Document::destroyLibrary() { FPDF_DestroyLibrary(); }

Document::Document() = default;
Document::~Document() { clear(); }

void Document::clear() {
    live_.clear();
    pages.clear();
    sources_.clear();
}

LoadResult Document::appendFile(const std::string& utf8Path, const std::string& password, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(utf8Path, bytes)) { err = "Nie można odczytać pliku."; return LoadResult::Error; }
    namespace fs = std::filesystem;
    std::string name = fs::u8path(utf8Path).filename().u8string();
    return appendBytes(std::move(bytes), name, password, err);
}

LoadResult Document::appendBytes(std::vector<uint8_t> bytes, const std::string& name,
                                 const std::string& password, std::string& err) {
    auto src = std::make_unique<Source>();
    src->bytes = std::move(bytes);
    src->name = name;
    src->doc = FPDF_LoadMemDocument64(src->bytes.data(), src->bytes.size(),
                                      password.empty() ? nullptr : password.c_str());
    if (!src->doc) {
        const unsigned long e = FPDF_GetLastError();
        if (e == FPDF_ERR_PASSWORD) {
            err = password.empty() ? "Plik jest chroniony hasłem." : "Nieprawidłowe hasło.";
            return password.empty() ? LoadResult::NeedPassword : LoadResult::WrongPassword;
        }
        err = e == FPDF_ERR_FORMAT ? "To nie jest poprawny plik PDF albo jest uszkodzony."
                                   : "Nie udało się otworzyć pliku PDF (kod " + std::to_string(e) + ").";
        return LoadResult::Error;
    }
    src->fonts.doc = src->doc;
    const int n = FPDF_GetPageCount(src->doc);
    if (n <= 0) { err = "Plik PDF nie zawiera stron."; return LoadResult::Error; }

    const int si = int(sources_.size());
    const size_t firstNew = pages.size();
    std::vector<int> idOfIndex(size_t(n), -1);
    struct RawLink { size_t page; Link link; int targetIndex; float pdfY; };
    std::vector<RawLink> raw;
    for (int i = 0; i < n; ++i) {
        FPDF_PAGE pg = FPDF_LoadPage(src->doc, i);
        if (!pg) continue;
        Page p;
        p.id = nextPageId_++;
        p.source = si;
        p.index = i;
        p.baseRot = std::max(0, FPDFPage_GetRotation(pg)) % 4;
        p.width = FPDF_GetPageWidthF(pg);   // uwzględnia /Rotate
        p.height = FPDF_GetPageHeightF(pg);
        float l, b, r, t;
        if (FPDFPage_GetCropBox(pg, &l, &b, &r, &t) || FPDFPage_GetMediaBox(pg, &l, &b, &r, &t)) {
            p.cropX = std::min(l, r);
            p.cropY = std::min(b, t);
        }
        // odnośniki www/e-mail i wewnętrzne
        {
            const Xf Vi = baseToPdf(p).inverse();
            int pos = 0;
            FPDF_LINK link = nullptr;
            while (FPDFLink_Enumerate(pg, &pos, &link)) {
                RawLink rl{pages.size(), {}, -1, 0};
                const int kind = linkKind(src->doc, link, &rl.link.uri, &rl.targetIndex, &rl.link.hasTargetY, &rl.pdfY);
                if (kind != 1 && kind != 2) continue;
                FS_RECTF r{};
                if (!FPDFLink_GetAnnotRect(link, &r)) continue;
                const Vec2 a = Vi.apply({r.left, r.bottom}), c = Vi.apply({r.right, r.top});
                rl.link.box = {std::min(a.x, c.x), std::min(a.y, c.y), std::fabs(c.x - a.x), std::fabs(c.y - a.y)};
                if (kind == 1 && rl.link.uri.empty()) continue;
                raw.push_back(std::move(rl));
            }
        }
        idOfIndex[size_t(i)] = p.id;
        FPDF_ClosePage(pg);
        pages.push_back(std::move(p));
    }
    // cele odnośników wewnętrznych: indeks strony w pliku -> id strony, wysokość -> współrzędne bazowe
    for (RawLink& rl : raw) {
        if (rl.link.internal()) {
            if (rl.targetIndex < 0 || rl.targetIndex >= n || idOfIndex[size_t(rl.targetIndex)] < 0) continue;
            rl.link.targetPage = idOfIndex[size_t(rl.targetIndex)];
            if (rl.link.hasTargetY) {
                const Page* tp = nullptr;
                for (size_t k = firstNew; k < pages.size(); ++k) if (pages[k].id == rl.link.targetPage) tp = &pages[k];
                if (tp) rl.link.targetY = baseToPdf(*tp).inverse().apply({0, rl.pdfY}).y;
                // strona obrócona: wysokość w PDF to inna oś — wtedy bez konkretnego miejsca
                if (tp && tp->baseRot % 2) rl.link.hasTargetY = false;
            }
        }
        pages[rl.page].links.push_back(std::move(rl.link));
    }
    sources_.push_back(std::move(src));
    return LoadResult::Ok;
}

int Document::insertBlankPage(int at, float w, float h) {
    w = std::clamp(w, kMinPagePt, kMaxPagePt);
    h = std::clamp(h, kMinPagePt, kMaxPagePt);
    auto src = std::make_unique<Source>();
    src->doc = FPDF_CreateNewDocument();
    src->fonts.doc = src->doc;
    src->name = "pusta strona";
    FPDF_PAGE pg = FPDFPage_New(src->doc, 0, w, h);
    FPDFPage_GenerateContent(pg);
    FPDF_ClosePage(pg);

    Page p;
    p.id = nextPageId_++;
    p.source = int(sources_.size());
    p.index = 0;
    p.width = w;
    p.height = h;
    sources_.push_back(std::move(src));
    if (at < 0 || at > int(pages.size())) at = int(pages.size());
    pages.insert(pages.begin() + at, std::move(p));
    return pages[at].id;
}

void Document::swap(Document& o) noexcept {
    pages.swap(o.pages);
    sources_.swap(o.sources_);
    live_.swap(o.live_);
    std::swap(liveClock_, o.liveClock_);
    std::swap(nextPageId_, o.nextPageId_);
}

Page* Document::pageById(int id) {
    for (auto& p : pages) if (p.id == id) return &p;
    return nullptr;
}
int Document::indexOfPage(int id) const {
    for (size_t i = 0; i < pages.size(); ++i) if (pages[i].id == id) return int(i);
    return -1;
}
const std::string& Document::sourceName(int s) const {
    static const std::string none;
    return s >= 0 && s < int(sources_.size()) ? sources_[s]->name : none;
}

// ---------------------------------------------------------------------------- żywe strony

static void mix(uint64_t& h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
}
template <class T> static void mixv(uint64_t& h, const T& v) { mix(h, &v, sizeof v); }

uint64_t Document::contentVersion(const Page& page) const {
    uint64_t h = 1469598103934665603ull;
    for (const Obj& o : page.objs) {
        if (o.type == ObjType::PageShape && o.shapeIdle()) continue;   // tylko zaznaczony: strona bez zmian
        if (o.type == ObjType::PageShape || o.type == ObjType::Shape) {
            mixv(h, o.type);
            mixv(h, o.x); mixv(h, o.y); mixv(h, o.w); mixv(h, o.h); mixv(h, o.hidden);
            mixv(h, o.color.r); mixv(h, o.color.g); mixv(h, o.color.b); mixv(h, o.color.a);
            mixv(h, o.stroke.r); mixv(h, o.stroke.g); mixv(h, o.stroke.b); mixv(h, o.stroke.a);
            mixv(h, o.fillMode); mixv(h, o.strokeOn); mixv(h, o.size); mixv(h, o.shapeEdits);
            for (int s : o.srcObjs) mixv(h, s);
            for (int s : o.childPath) mixv(h, s);
            mixv(h, o.pts.size());
            if (!o.pts.empty()) { mixv(h, o.pts[0].x); mixv(h, o.pts[0].y); }
            mixv(h, uint8_t(0xFC));
            continue;
        }
        if (o.type != ObjType::Text && o.type != ObjType::Whiteout && o.type != ObjType::PageImage) continue;
        mixv(h, o.type);
        mixv(h, o.x); mixv(h, o.y); mixv(h, o.w); mixv(h, o.h);
        if (o.type == ObjType::Whiteout) continue;
        if (o.type == ObjType::Text && o.untouched()) continue;
        if (o.type == ObjType::PageImage) {
            mixv(h, o.imgRot); mixv(h, o.hidden);
            const uint64_t uid = o.image ? o.image->uid : 0;
            mixv(h, uid);
            for (int s : o.srcObjs) mixv(h, s);
            for (int s : o.childPath) mixv(h, s);
            mixv(h, uint8_t(0xFD));
            continue;
        }
        mix(h, o.text.data(), o.text.size());
        mixv(h, o.adv.x); mixv(h, o.adv.y); mixv(h, o.up.x); mixv(h, o.up.y);
        mixv(h, o.boxWidth); mixv(h, o.align); mixv(h, o.lineStep); mixv(h, o.firstIndent);
        for (const TextStyle& st : o.styles) {
            mixv(h, st.size); mixv(h, st.color.r); mixv(h, st.color.g); mixv(h, st.color.b); mixv(h, st.color.a);
            mixv(h, st.renderMode);
            mixv(h, st.font.family); mixv(h, st.font.bold); mixv(h, st.font.italic); mixv(h, st.font.origObj); mixv(h, st.font.fit);
            mix(h, st.font.okChars.data(), st.font.okChars.size());
        }
        if (!o.charStyle.empty()) mix(h, o.charStyle.data(), o.charStyle.size() * sizeof(uint16_t));
        for (int s : o.srcObjs) mixv(h, s);
        mixv(h, uint8_t(0xFE));
    }
    return h;
}

Document::Live& Document::live(const Page& page) const {
    auto& slot = live_[page.id];
    if (!slot) slot = std::make_unique<Live>();
    Live& L = *slot;
    L.lastUse = ++liveClock_;
    const uint64_t v = contentVersion(page);
    if (L.pe.page && L.version == v && L.src == page.source && L.idx == page.index) return L;

    L.teardown();
    Source& s = *sources_[page.source];
    L.pe.doc = s.doc;
    L.pe.fonts = &s.fonts;
    L.pe.page = FPDF_LoadPage(s.doc, page.index);
    L.src = page.source;
    L.idx = page.index;
    L.version = v;
    if (L.pe.page) {
        const int n = FPDFPage_CountObjects(L.pe.page);
        for (int i = 0; i < n; ++i) L.pe.pristine.push_back(FPDFPage_GetObject(L.pe.page, i));
        applyContentEdits(L.pe, page);
    }
    // ograniczenie pamięci: najdawniej używane strony są zamykane
    if (live_.size() > 24) {
        int oldestId = -1, oldest = L.lastUse;
        for (auto& [id, lp] : live_)
            if (id != page.id && lp->lastUse < oldest) { oldest = lp->lastUse; oldestId = id; }
        if (oldestId >= 0) live_.erase(oldestId);
    }
    return L;
}

void Document::dropLive(int pageId) const { live_.erase(pageId); }

bool Document::render(const Page& page, float scale, std::vector<uint8_t>& rgba, int& w, int& h, const int* region) const {
    if (page.source < 0 || page.source >= int(sources_.size())) return false;
    Live& L = live(page);
    if (!L.pe.page) return false;
    const int fullW = std::max(1, int(std::lround(page.shownWidth() * scale)));
    const int fullH = std::max(1, int(std::lround(page.shownHeight() * scale)));
    int rx = 0, ry = 0;
    w = fullW;
    h = fullH;
    if (region) {
        rx = std::clamp(region[0], 0, fullW - 1);
        ry = std::clamp(region[1], 0, fullH - 1);
        w = std::clamp(region[2], 1, fullW - rx);
        h = std::clamp(region[3], 1, fullH - ry);
    }
    FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0);
    if (!bmp) return false;
    FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, L.pe.page, -rx, -ry, fullW, fullH, page.extraRot, FPDF_ANNOT | FPDF_LCD_TEXT);
    const auto* src = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bmp));
    const int stride = FPDFBitmap_GetStride(bmp);
    rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + size_t(y) * stride;
        uint8_t* d = rgba.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
    }
    FPDFBitmap_Destroy(bmp);
    return true;
}

// ---------------------------------------------------------------------------- tekst

TextLayout Document::layoutText(const Page& page, const Obj& o) const {
    TextLayout out;
    if (o.styles.empty()) return out;
    Live& L = live(page);
    if (!L.pe.page) return out;
    const Laid lay = layoutObj(L.pe, o);
    auto at = [&](float x, float y) { return Vec2{o.x + o.adv.x * x - o.up.x * y, o.y + o.adv.y * x - o.up.y * y}; };
    for (const LaidLine& ln : lay.lines) {
        TextLayout::Line tl;
        tl.first = ln.first;
        for (float x : ln.caret) tl.carets.push_back(at(x, ln.y));
        tl.ascent = {o.up.x * ln.asc, o.up.y * ln.asc};
        tl.descent = {o.up.x * ln.desc, o.up.y * ln.desc};
        if (o.boxWidth > 0) {
            const Vec2 r = at(o.boxWidth, ln.y);
            out.extent.push_back({r.x + tl.ascent.x, r.y + tl.ascent.y});
            out.extent.push_back({r.x + tl.descent.x, r.y + tl.descent.y});
            const Vec2 l = at(0, ln.y);
            out.extent.push_back({l.x + tl.ascent.x, l.y + tl.ascent.y});
            out.extent.push_back({l.x + tl.descent.x, l.y + tl.descent.y});
        }
        out.lines.push_back(std::move(tl));
    }
    for (const StyleFont& f : lay.fonts) {
        out.styleFont.push_back(fontName(f.font));
        out.styleOriginal.push_back(f.original);
    }
    return out;
}

static bool isLetter(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= 0xC0 && c < 0x250 && c != 0xD7 && c != 0xF7);
}
static bool isLower(char32_t c) {
    if (c >= U'a' && c <= U'z') return true;
    if (c >= 0xDF && c <= 0xFF && c != 0xF7) return true;
    // Latin Extended-A: wielkie i małe litery na zmianę, z przesunięciem parzystości w dwóch zakresach
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c % 2 == 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c % 2 == 0;
    return c == 0x138 || c == 0x17F;
}

Pick Document::pickText(const Page& page, Vec2 base, Obj& out, int& caretChar, std::string& why) const {
    if (page.source < 0 || page.source >= int(sources_.size())) return Pick::None;
    Live& L = live(page);
    if (!L.pe.page) return Pick::None;
    const Xf V = baseToPdf(page), Vi = V.inverse();
    std::unordered_map<FPDF_PAGEOBJECT, int> pidx;
    for (size_t i = 0; i < L.pe.pristine.size(); ++i) pidx[L.pe.pristine[i]] = int(i);

    FPDF_TEXTPAGE tp = FPDFText_LoadPage(L.pe.page);
    if (!tp) return Pick::None;
    struct Closer { FPDF_TEXTPAGE t; ~Closer() { FPDFText_ClosePage(t); } } closer{tp};

    const Vec2 P = V.apply(base);
    const int n = FPDFText_CountChars(tp);
    auto objOf = [&](int k) { return FPDFText_GetTextObject(tp, k); };
    auto gen = [&](int k) { return FPDFText_IsGenerated(tp, k) == 1; };
    auto invisible = [&](FPDF_PAGEOBJECT o) { return FPDFTextObj_GetTextRenderMode(o) == FPDF_TEXTRENDERMODE_INVISIBLE; };
    // trafienie w znak; spacje nie mają wysokości, więc przy chybieniu szukamy szerzej
    int hit = FPDFText_GetCharIndexAtPos(tp, P.x, P.y, 2.0, 2.0);
    if (hit < 0) hit = FPDFText_GetCharIndexAtPos(tp, P.x, P.y, 6.0, 3.0);
    if (hit >= 0 && (gen(hit) || !objOf(hit))) {
        if (hit > 0 && !gen(hit - 1) && objOf(hit - 1)) --hit;
        else if (hit + 1 < n && !gen(hit + 1) && objOf(hit + 1)) ++hit;
    }
    if (hit < 0 || gen(hit)) return Pick::None;
    FPDF_PAGEOBJECT hitObj = objOf(hit);
    if (!hitObj || L.pe.added.count(hitObj)) return Pick::None;
    if (!pidx.count(hitObj)) {
        why = "Ten tekst jest częścią osadzonej grafiki i nie da się go poprawić. Możesz go zamazać i wpisać nowy.";
        return Pick::NotEditable;
    }
    if (invisible(hitObj)) {
        why = "To niewidoczna warstwa tekstu (np. rozpoznany skan) — widoczny tekst jest obrazem. Możesz go zamazać i wpisać nowy.";
        return Pick::NotEditable;
    }

    // ---------- wiersze strony
    struct PLine {
        std::vector<int> ch;            // znaki (z wygenerowanymi spacjami)
        Vec2 dir, upU, origin;          // kierunek, "w górę" (jednostkowe), początek pierwszego znaku
        float base = 0, left = 0, right = 0, size = 0, firstWord = 0;
        FPDF_PAGEOBJECT first = nullptr;
    };
    std::vector<PLine> lines;
    std::vector<int> lineOf(size_t(n), -1);
    std::vector<float> cLeft(size_t(n), 0), cRight(size_t(n), 0);
    auto norm = [](float x, float y) { const float l = std::hypot(x, y); return l > 0 ? Vec2{x / l, y / l} : Vec2{1, 0}; };
    auto dot = [](Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; };
    PLine cur;
    float lastRight = 0;
    auto flush = [&] {
        while (!cur.ch.empty() && gen(cur.ch.back())) cur.ch.pop_back();
        if (!cur.ch.empty()) {
            const int li = int(lines.size());
            bool inWord = true;
            for (int k : cur.ch) {
                if (gen(k) || FPDFText_GetUnicode(tp, k) == ' ') inWord = false;
                if (!gen(k)) {
                    lineOf[size_t(k)] = li;
                    cur.right = std::max(cur.right, cRight[size_t(k)]);
                    if (inWord) cur.firstWord = cRight[size_t(k)] - cur.left;
                }
            }
            lines.push_back(cur);
        }
        cur = PLine{};
    };
    for (int k = 0; k < n; ++k) {
        if (gen(k)) {
            const unsigned u = FPDFText_GetUnicode(tp, k);
            if (u == 13 || u == 10) flush();
            else if (!cur.ch.empty()) cur.ch.push_back(k);
            continue;
        }
        FPDF_PAGEOBJECT o = objOf(k);
        if (!o || L.pe.added.count(o) || !pidx.count(o) || invisible(o)) { flush(); continue; }
        FS_MATRIX m;
        FPDFPageObj_GetMatrix(o, &m);
        const Vec2 dir = norm(m.a, m.b);
        double ox = 0, oy = 0, l = 0, r = 0, b = 0, t = 0;
        FPDFText_GetCharOrigin(tp, k, &ox, &oy);
        FPDFText_GetCharBox(tp, k, &l, &r, &b, &t);
        if (!cur.ch.empty()) {
            const Vec2 o2{float(ox), float(oy)};
            if (dot(dir, cur.dir) < 0.99f) flush();
            else if (std::fabs(dot(o2, cur.upU) - cur.base) > 0.4f * cur.size) flush();   // inny wiersz (bez łamania w tekście)
            else if (dot(o2, cur.dir) - lastRight > 2.5f * cur.size) flush();             // inna kolumna
        }
        if (cur.ch.empty()) {
            lastRight = -1e30f;
            cur.right = -1e30f;
            float fs = 12;
            FPDFTextObj_GetFontSize(o, &fs);
            cur.dir = dir;
            cur.upU = norm(m.c, m.d);
            cur.origin = {float(ox), float(oy)};
            cur.size = fs * std::hypot(m.c, m.d);
            cur.left = dot(cur.origin, dir);
            cur.base = dot(cur.origin, cur.upU);
            cur.first = o;
        }
        float rr = -1e30f;
        for (Vec2 c : {Vec2{float(l), float(b)}, Vec2{float(r), float(b)}, Vec2{float(l), float(t)}, Vec2{float(r), float(t)}})
            rr = std::max(rr, dot(c, cur.dir));
        cLeft[size_t(k)] = dot({float(ox), float(oy)}, cur.dir);
        cRight[size_t(k)] = rr;
        lastRight = std::max(lastRight, rr);
        cur.ch.push_back(k);
    }
    flush();
    const int hl = lineOf[size_t(hit)];
    if (hl < 0) return Pick::None;

    // ---------- akapit: sąsiednie wiersze o tym samym kroju, odstępie i wyrównaniu
    auto neighbor = [&](int li, bool below) {
        const PLine& A = lines[size_t(li)];
        int best = -1;
        float bestGap = 1e30f;
        for (size_t j = 0; j < lines.size(); ++j) {
            if (int(j) == li) continue;
            const PLine& B = lines[j];
            if (dot(A.dir, B.dir) < 0.99f) continue;
            const float g = below ? A.base - B.base : B.base - A.base;
            const float sz = std::max(A.size, B.size);
            if (g < 0.5f * sz || g > 2.6f * sz) continue;
            if (std::min(A.right, B.right) - std::max(A.left, B.left) <= 0) continue;
            if (g < bestGap) { bestGap = g; best = int(j); }
        }
        return best;
    };
    auto sameSize = [&](const PLine& a, const PLine& b) { return std::fabs(a.size - b.size) <= 0.25f * std::max(a.size, b.size); };
    std::vector<int> para{hl};
    float gap0 = -1, maxRight = lines[size_t(hl)].right;
    auto okGap = [&](float g) { return gap0 < 0 || std::fabs(g - gap0) <= 0.25f * gap0; };
    for (int c = hl;;) {   // w dół
        const int b = neighbor(c, true);
        if (b < 0) break;
        const PLine &C = lines[size_t(c)], &B = lines[size_t(b)];
        const float g = C.base - B.base;
        if (!sameSize(C, B) || !okGap(g)) break;
        const float pr = std::max(maxRight, B.right);
        if (pr - C.right > B.firstWord + 0.6f * C.size) break;      // poprzedni wiersz kończy akapit
        if (B.left > C.left + 1.5f) break;                          // wcięty wiersz zaczyna nowy akapit
        if (para.size() >= 2 && std::fabs(B.left - lines[size_t(para[1])].left) > 1.5f) break;
        para.push_back(b);
        if (gap0 < 0) gap0 = g;
        maxRight = pr;
        c = b;
    }
    for (;;) {             // w górę
        const int top = para.front();
        const int a = neighbor(top, false);
        if (a < 0) break;
        const PLine &A = lines[size_t(a)], &T = lines[size_t(top)];
        const float g = A.base - T.base;
        if (!sameSize(A, T) || !okGap(g)) break;
        if (T.left > A.left + 1.5f) break;                          // T to wcięty pierwszy wiersz
        const float pr = std::max(maxRight, A.right);
        if (pr - A.right > T.firstWord + 0.6f * T.size) break;      // A kończy poprzedni akapit
        para.insert(para.begin(), a);
        if (gap0 < 0) gap0 = g;
        maxRight = pr;
        if (A.left > T.left + 1.5f) break;                          // A jest wciętym pierwszym wierszem
    }

    // ---------- tekst i style
    const PLine& F = lines[size_t(para.front())];
    FPDF_PAGEOBJECT first = F.first;
    FS_MATRIX m0;
    FPDFPageObj_GetMatrix(first, &m0);
    const float advLen = std::hypot(m0.a, m0.b), upLen = std::hypot(m0.c, m0.d);

    out = Obj{};
    out.type = ObjType::Text;
    std::u32string text;
    std::vector<uint16_t> cs;
    std::map<std::tuple<FPDF_FONT, int, unsigned, int>, int> styleKey;
    std::map<FPDF_FONT, std::string> okCache;
    std::set<FPDF_PAGEOBJECT> objs;
    int caret = 0;
    auto styleOf = [&](FPDF_PAGEOBJECT o) {
        FPDF_FONT font = FPDFTextObj_GetFont(o);
        float fs = 12;
        FPDFTextObj_GetFontSize(o, &fs);
        FS_MATRIX m;
        FPDFPageObj_GetMatrix(o, &m);
        const float size = fs * std::hypot(m.c, m.d) / std::max(upLen, 1e-6f);   // w jednostkach tekstu akapitu
        unsigned r = 0, g = 0, b = 0, a = 255;
        FPDFPageObj_GetFillColor(o, &r, &g, &b, &a);
        const int rm = std::max(0, int(FPDFTextObj_GetTextRenderMode(o)));
        const auto key = std::make_tuple(font, int(std::lround(size * 100)), (r << 24) | (g << 16) | (b << 8) | a, rm);
        auto it = styleKey.find(key);
        if (it != styleKey.end()) return it->second;
        TextStyle st;
        st.size = size;
        st.color = {r / 255.f, g / 255.f, b / 255.f, a / 255.f};
        st.renderMode = rm;
        guessStyle(font, st.font.family, st.font.bold, st.font.italic);
        st.font.origObj = pidx[o];
        if (!okCache.count(font)) okCache[font] = utf32To8(observedChars(tp, font, FPDFFont_GetIsEmbedded(font) == 1));
        st.font.okChars = okCache[font];
        const int idx = int(out.styles.size());
        out.styles.push_back(st);
        styleKey[key] = idx;
        return idx;
    };
    for (size_t pi = 0; pi < para.size(); ++pi) {
        const PLine& ln = lines[size_t(para[pi])];
        if (pi > 0) {
            // łączenie wierszy: przeniesiony wyraz ("edyto-" + "wanie") albo spacja
            const PLine& next = ln;
            char32_t nextFirst = 0;
            for (int k : next.ch) if (!gen(k)) { nextFirst = char32_t(FPDFText_GetUnicode(tp, k)); break; }
            const char32_t last = text.empty() ? 0 : text.back();
            const char32_t prev = text.size() >= 2 ? text[text.size() - 2] : 0;
            if (last == 0xAD) { text.pop_back(); cs.pop_back(); }
            else if (last == U'-' && isLetter(prev) && isLower(nextFirst)) { text.pop_back(); cs.pop_back(); }
            else { text.push_back(U' '); cs.push_back(cs.empty() ? 0 : cs.back()); }
        }
        for (int k : ln.ch) {
            unsigned u = FPDFText_GetUnicode(tp, k);
            if (u == 2) u = '-';   // PDFium oznacza dywiz na końcu wiersza kodem 2
            if (k == hit) caret = int(text.size());
            if (gen(k)) {
                if (u >= 32) { text.push_back(char32_t(u)); cs.push_back(cs.empty() ? 0 : cs.back()); }
                continue;
            }
            if (u < 32) continue;
            FPDF_PAGEOBJECT o = objOf(k);
            objs.insert(o);
            text.push_back(char32_t(u));
            cs.push_back(uint16_t(styleOf(o)));
        }
    }
    if (!text.empty() && text.back() == 0xAD) { text.pop_back(); cs.pop_back(); }
    // klik w prawą połowę znaku: kursor za nim
    {
        double l, r, b, t;
        if (FPDFText_GetCharBox(tp, hit, &l, &r, &b, &t)) {
            FS_MATRIX m;
            FPDFPageObj_GetMatrix(hitObj, &m);
            const float cx = float(l + r) / 2, cy = float(b + t) / 2;
            if ((P.x - cx) * m.a + (P.y - cy) * m.b > 0) ++caret;
        }
    }
    out.text = utf32To8(text);
    out.charStyle = cs;

    // dopasowanie szerokości czcionek zastępczych do oryginałów (osobno dla każdego stylu)
    for (int si = 0; si < int(out.styles.size()); ++si) {
        TextStyle& st = out.styles[size_t(si)];
        FPDF_FONT orig = FPDFTextObj_GetFont(L.pe.pristine[size_t(st.font.origObj)]);
        FPDF_FONT fb = L.pe.fonts->get(st.font.family, st.font.bold, st.font.italic);
        float wo = 0, wf = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if (cs[i] != si || text[i] == U' ') continue;
            float x = 0;
            if (orig && FPDFFont_GetGlyphWidth(orig, uint32_t(text[i]), st.size, &x)) wo += x;
            x = 0;
            if (fb && FPDFFont_GetGlyphWidth(fb, uint32_t(text[i]), st.size, &x)) wf += x;
        }
        if (wo > 0 && wf > 0) st.font.fit = std::clamp(wo / wf, 0.7f, 1.2f);
    }

    // ---------- geometria akapitu
    float bodyLeft = para.size() >= 2 ? lines[size_t(para[1])].left : F.left;
    float minLeft = 1e30f;
    for (int li : para) minLeft = std::min(minLeft, lines[size_t(li)].left);
    Align align = Align::Left;
    if (para.size() >= 2) {
        const float tol = 1.5f;
        bool lefts = true, nonLastRights = true, allRights = true, centers = true;
        const float c0 = (F.left + F.right) / 2;
        for (size_t i = 0; i < para.size(); ++i) {
            const PLine& ln = lines[size_t(para[i])];
            if (i > 0 && std::fabs(ln.left - bodyLeft) > tol) lefts = false;
            if (i + 1 < para.size() && std::fabs(ln.right - maxRight) > tol) nonLastRights = false;
            if (std::fabs(ln.right - maxRight) > tol) allRights = false;
            if (std::fabs((ln.left + ln.right) / 2 - c0) > tol) centers = false;
        }
        // rozciągnięte odstępy między wyrazami w pierwszym wierszu = justowanie
        auto stretched = [&](const PLine& ln) {
            float sum = 0;
            int cnt = 0;
            int prevReal = -1;
            bool sawSpace = false;
            for (int k : ln.ch) {
                const bool space = gen(k) || FPDFText_GetUnicode(tp, k) == ' ';
                if (space) { sawSpace = true; continue; }
                if (prevReal >= 0 && sawSpace) { sum += cLeft[size_t(k)] - cRight[size_t(prevReal)]; ++cnt; }
                prevReal = k;
                sawSpace = false;
            }
            if (!cnt) return false;
            float sp = 0;
            FPDF_FONT f = FPDFTextObj_GetFont(ln.first);
            float fs = 12;
            FPDFTextObj_GetFontSize(ln.first, &fs);
            if (!f || !FPDFFont_GetGlyphWidth(f, uint32_t(U' '), fs, &sp) || sp <= 0) sp = 0.28f * fs;
            return sum / cnt > 1.3f * sp * advLen;
        };
        if (lefts && nonLastRights && (para.size() >= 3 || stretched(F))) align = Align::Justify;
        else if (lefts) align = Align::Left;
        else if (allRights) { align = Align::Right; bodyLeft = minLeft; }
        else if (centers) { align = Align::Center; bodyLeft = minLeft; }
        else bodyLeft = minLeft;
    }
    const float indentPage = (align == Align::Left || align == Align::Justify) ? F.left - bodyLeft : 0.f;
    const Vec2 originPage{F.origin.x - F.dir.x * (F.left - bodyLeft), F.origin.y - F.dir.y * (F.left - bodyLeft)};
    const Vec2 org = Vi.apply(originPage);
    out.x = org.x;
    out.y = org.y;
    out.adv = Vi.lin({m0.a, m0.b});
    out.up = Vi.lin({m0.c, m0.d});
    out.align = align;
    if (para.size() >= 2) {
        out.boxWidth = (maxRight - bodyLeft) / advLen * 1.002f + 0.5f / advLen;
        out.lineStep = (F.base - lines[size_t(para.back())].base) / float(para.size() - 1) / upLen;
        out.firstIndent = std::fabs(indentPage) > 0.5f ? indentPage / advLen : 0.f;
    }

    float bl = 1e9f, bb = 1e9f, br = -1e9f, bt = -1e9f;
    for (FPDF_PAGEOBJECT o : objs) {
        out.srcObjs.push_back(pidx[o]);
        float l, bo, rr, t;
        if (FPDFPageObj_GetBounds(o, &l, &bo, &rr, &t)) {
            bl = std::min(bl, l); bb = std::min(bb, bo); br = std::max(br, rr); bt = std::max(bt, t);
        }
    }
    std::sort(out.srcObjs.begin(), out.srcObjs.end());
    if (br > bl) {
        const Vec2 p1 = Vi.apply({bl, bb}), p2 = Vi.apply({br, bt});
        out.origBox = {std::min(p1.x, p2.x), std::min(p1.y, p2.y), std::fabs(p2.x - p1.x), std::fabs(p2.y - p1.y)};
    }
    caretChar = std::clamp(caret, 0, int(text.size()));
    out.pickHash = out.contentHash();
    return Pick::Ok;
}

bool Document::matchPageFont(const Page& page, FontChoice& fc) const {
    Live& L = live(page);
    if (!L.pe.page) return false;
    FPDF_TEXTPAGE tp = nullptr;
    bool ok = false;
    for (size_t i = 0; i < L.pe.pristine.size() && !ok; ++i) {
        FPDF_PAGEOBJECT o = L.pe.pristine[i];
        if (FPDFPageObj_GetType(o) != FPDF_PAGEOBJ_TEXT) continue;
        FPDF_FONT f = FPDFTextObj_GetFont(o);
        if (!f) continue;
        int fam;
        bool b, it;
        guessStyle(f, fam, b, it);
        if (fam != fc.family || b != fc.bold || it != fc.italic) continue;
        if (!tp) tp = FPDFText_LoadPage(L.pe.page);
        if (!tp) break;
        fc.origObj = int(i);
        fc.okChars = utf32To8(observedChars(tp, f, FPDFFont_GetIsEmbedded(f) == 1));
        fc.fit = 1.f;
        ok = true;
    }
    if (tp) FPDFText_ClosePage(tp);
    return ok;
}

bool Document::pickImage(const Page& page, Vec2 base, Obj& out, std::vector<int>* z) const {
    if (page.source < 0 || page.source >= int(sources_.size())) return false;
    Live& L = live(page);
    if (!L.ref) L.ref = FPDF_LoadPage(sources_[size_t(page.source)]->doc, page.index);
    if (!L.ref) return false;
    const Xf V = baseToPdf(page), Vi = V.inverse();
    const Vec2 P = V.apply(base);

    auto edited = [&](int top, const std::vector<int>& path) {
        for (const Obj& o : page.objs)
            if (o.type == ObjType::PageImage && !o.srcObjs.empty() && o.srcObjs[0] == top && o.childPath == path) return true;
        return false;
    };
    // przeszukiwanie od wierzchu (ostatnio rysowane najpierw), także wewnątrz osadzonych grafik
    std::function<bool(FPDF_PAGEOBJECT, const Xf&, int, std::vector<int>&)> visit =
        [&](FPDF_PAGEOBJECT o, const Xf& toPage, int top, std::vector<int>& path) -> bool {
        const int type = FPDFPageObj_GetType(o);
        FS_MATRIX fm;
        if (!FPDFPageObj_GetMatrix(o, &fm)) return false;
        const Xf M = Xf::from(fm).then(toPage);
        if (type == FPDF_PAGEOBJ_FORM) {
            const int n = FPDFFormObj_CountObjects(o);
            for (int i = n - 1; i >= 0; --i) {
                path.push_back(i);
                if (visit(FPDFFormObj_GetObject(o, (unsigned long)i), M, top, path)) return true;
                path.pop_back();
            }
            return false;
        }
        if (type != FPDF_PAGEOBJ_IMAGE) return false;
        const Vec2 uv = M.inverse().apply(P);
        if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return false;
        if (edited(top, path)) return false;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (Vec2 c : {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}}) {
            const Vec2 b = Vi.apply(M.apply(c));
            x0 = std::min(x0, b.x); y0 = std::min(y0, b.y); x1 = std::max(x1, b.x); y1 = std::max(y1, b.y);
        }
        out = Obj{};
        out.type = ObjType::PageImage;
        out.srcObjs = {top};
        out.childPath = path;
        out.origBox = {x0, y0, x1 - x0, y1 - y0};
        out.x = x0; out.y = y0; out.w = x1 - x0; out.h = y1 - y0;
        if (z) { *z = {top}; z->insert(z->end(), path.begin(), path.end()); }
        unsigned pw = 0, ph = 0;
        if (FPDFImageObj_GetImagePixelSize(o, &pw, &ph)) { out.pixelW = int(pw); out.pixelH = int(ph); }
        return true;
    };
    const int n = FPDFPage_CountObjects(L.ref);
    for (int i = n - 1; i >= 0; --i) {
        std::vector<int> path;
        if (visit(FPDFPage_GetObject(L.ref, i), Xf{}, i, path)) return true;
    }
    return false;
}

bool Document::pickShape(const Page& page, Vec2 base, float tol, Obj& out, std::vector<int>* z) const {
    if (page.source < 0 || page.source >= int(sources_.size())) return false;
    Live& L = live(page);
    if (!L.ref) L.ref = FPDF_LoadPage(sources_[size_t(page.source)]->doc, page.index);
    if (!L.ref) return false;
    const Xf V = baseToPdf(page), Vi = V.inverse();
    const Vec2 P = V.apply(base);
    const float pageArea = page.width * page.height;

    auto edited = [&](int top, const std::vector<int>& path) {
        for (const Obj& o : page.objs)
            if (o.type == ObjType::PageShape && !o.srcObjs.empty() && o.srcObjs[0] == top && o.childPath == path) return true;
        return false;
    };
    auto segDist = [](Vec2 p, Vec2 a, Vec2 b) {
        const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
        const float t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.f, 1.f) : 0.f;
        return std::hypot(a.x + t * dx - p.x, a.y + t * dy - p.y);
    };
    std::function<bool(FPDF_PAGEOBJECT, const Xf&, int, std::vector<int>&)> visit =
        [&](FPDF_PAGEOBJECT o, const Xf& toPage, int top, std::vector<int>& path) -> bool {
        const int type = FPDFPageObj_GetType(o);
        if (type == FPDF_PAGEOBJ_FORM) {
            FS_MATRIX fm;
            if (!FPDFPageObj_GetMatrix(o, &fm)) return false;
            const Xf M = Xf::from(fm).then(toPage);
            const int n = FPDFFormObj_CountObjects(o);
            for (int i = n - 1; i >= 0; --i) {
                path.push_back(i);
                if (visit(FPDFFormObj_GetObject(o, (unsigned long)i), M, top, path)) return true;
                path.pop_back();
            }
            return false;
        }
        if (type != FPDF_PAGEOBJ_PATH) return false;
        int fillMode = 0;
        FPDF_BOOL stroke = 0;
        if (!FPDFPath_GetDrawMode(o, &fillMode, &stroke) || (fillMode == 0 && !stroke)) return false;   // ścieżka obcinająca
        // szybkie odrzucenie po granicach obiektu
        float l, b, r, t;
        if (FPDFPageObj_GetBounds(o, &l, &b, &r, &t)) {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (Vec2 c : {Vec2{l, b}, Vec2{r, b}, Vec2{l, t}, Vec2{r, t}}) {
                const Vec2 q = toPage.apply(c);
                x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y);
            }
            if (P.x < x0 - tol || P.x > x1 + tol || P.y < y0 - tol || P.y > y1 + tol) return false;
        }
        FS_MATRIX om;
        if (!FPDFPageObj_GetMatrix(o, &om)) return false;
        const Xf M = Xf::from(om).then(toPage);
        const int n = FPDFPath_CountSegments(o);
        if (n < 2) return false;
        std::vector<Vec2> pts;
        std::vector<uint8_t> segs;
        for (int i = 0; i < n; ++i) {
            FPDF_PATHSEGMENT sg = FPDFPath_GetPathSegment(o, i);
            float x = 0, y = 0;
            if (!sg || !FPDFPathSegment_GetPoint(sg, &x, &y)) return false;
            const int st = FPDFPathSegment_GetType(sg);
            uint8_t kind = st == FPDF_SEGMENT_MOVETO ? 0 : st == FPDF_SEGMENT_BEZIERTO ? 2 : 1;
            if (i == 0) kind = 0;
            if (FPDFPathSegment_GetClose(sg)) kind |= 4;
            pts.push_back(Vi.apply(M.apply({x, y})));
            segs.push_back(kind);
        }
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const Vec2& q : pts) { x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y); }
        if (fillMode && (x1 - x0) * (y1 - y0) > 0.6f * pageArea) return false;   // tło strony
        // linie łamane (krzywe przybliżone odcinkami), z podziałem na figury
        std::vector<std::vector<Vec2>> polys;
        for (size_t i = 0; i < pts.size(); ++i) {
            const int k = segs[i] & 3;
            if (k == 0 || polys.empty()) polys.push_back({pts[i]});
            else if (k == 1) polys.back().push_back(pts[i]);
            else if (k == 2 && i + 2 < pts.size()) {
                const Vec2 a = polys.back().back(), c1 = pts[i], c2 = pts[i + 1], e = pts[i + 2];
                for (int s = 1; s <= 8; ++s) {
                    const float u = s / 8.f, v = 1 - u;
                    polys.back().push_back({v * v * v * a.x + 3 * v * v * u * c1.x + 3 * v * u * u * c2.x + u * u * u * e.x,
                                            v * v * v * a.y + 3 * v * v * u * c1.y + 3 * v * u * u * c2.y + u * u * u * e.y});
                }
                if (segs[i + 2] & 4) polys.back().push_back(polys.back().front());
                i += 2;
                continue;
            }
            if (segs[i] & 4) polys.back().push_back(polys.back().front());
        }
        float width = 0;
        FPDFPageObj_GetStrokeWidth(o, &width);
        const float kScale = std::sqrt(std::fabs(M.a * M.d - M.b * M.c));
        const float widthPt = width * (kScale > 1e-6f ? kScale : 1.f);
        bool hit = false;
        const float edge = tol + (stroke ? widthPt / 2 : 0.f);
        for (const auto& pl : polys) {
            for (size_t i = 1; i < pl.size() && !hit; ++i) hit = segDist(base, pl[i - 1], pl[i]) <= edge;
            if (pl.size() == 1) hit = hit || std::hypot(base.x - pl[0].x, base.y - pl[0].y) <= edge;
        }
        if (!hit && fillMode) {   // wnętrze (parzysto-nieparzyste; figury otwarte domykamy jak przy wypełnianiu)
            bool in = false;
            for (const auto& pl : polys)
                for (size_t i = 0, j = pl.size() - 1; i < pl.size(); j = i++) {
                    const Vec2 &a = pl[i], &c = pl[j];
                    if ((a.y > base.y) != (c.y > base.y) && base.x < (c.x - a.x) * (base.y - a.y) / (c.y - a.y) + a.x) in = !in;
                }
            hit = in;
        }
        if (!hit || edited(top, path)) return false;

        out = Obj{};
        out.type = ObjType::PageShape;
        out.srcObjs = {top};
        out.childPath = path;
        out.origBox = {x0, y0, x1 - x0, y1 - y0};
        out.x = x0; out.y = y0; out.w = x1 - x0; out.h = y1 - y0;
        out.pts = std::move(pts);
        out.segs = std::move(segs);
        unsigned R = 0, G = 0, B = 0, A = 255;
        FPDFPageObj_GetFillColor(o, &R, &G, &B, &A);
        out.color = {R / 255.f, G / 255.f, B / 255.f, A / 255.f};
        R = G = B = 0; A = 255;
        FPDFPageObj_GetStrokeColor(o, &R, &G, &B, &A);
        out.stroke = {R / 255.f, G / 255.f, B / 255.f, A / 255.f};
        out.fillMode = fillMode;
        out.strokeOn = stroke != 0;
        out.size = widthPt;
        out.lineCap = FPDFPageObj_GetLineCap(o);
        out.lineJoin = FPDFPageObj_GetLineJoin(o);
        if (const int dc = FPDFPageObj_GetDashCount(o); dc > 0) {
            out.dash.resize(size_t(dc));
            FPDFPageObj_GetDashArray(o, out.dash.data(), size_t(dc));
            for (float& d : out.dash) d *= kScale;
            FPDFPageObj_GetDashPhase(o, &out.dashPhase);
            out.dashPhase *= kScale;
        }
        if (z) { *z = {top}; z->insert(z->end(), path.begin(), path.end()); }
        return true;
    };
    const int n = FPDFPage_CountObjects(L.ref);
    for (int i = n - 1; i >= 0; --i) {
        std::vector<int> path;
        if (visit(FPDFPage_GetObject(L.ref, i), Xf{}, i, path)) return true;
    }
    return false;
}

std::string Document::textInRect(const Page& page, const Rect& base, size_t maxChars) const {
    if (page.source < 0 || page.source >= int(sources_.size())) return {};
    Live& L = live(page);
    if (!L.pe.page) return {};
    if (!L.tp) L.tp = FPDFText_LoadPage(L.pe.page);
    if (!L.tp) return {};
    const Xf V = baseToPdf(page);
    const Vec2 a = V.apply({base.x, base.y}), b = V.apply({base.x + base.w, base.y + base.h});
    const double l = std::min(a.x, b.x), r = std::max(a.x, b.x), bo = std::min(a.y, b.y), t = std::max(a.y, b.y);
    const int n = FPDFText_GetBoundedText(L.tp, l, t, r, bo, nullptr, 0);
    if (n <= 0) return {};
    std::vector<unsigned short> buf(size_t(n) + 1, 0);
    FPDFText_GetBoundedText(L.tp, l, t, r, bo, buf.data(), n);
    std::u32string u;
    bool space = false;
    for (int i = 0; i < n && buf[size_t(i)]; ++i) {
        char32_t c = buf[size_t(i)];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) { c = 0x10000 + ((c - 0xD800) << 10) + (buf[size_t(i) + 1] - 0xDC00); ++i; }
        if (c == 2) c = U'-';   // łącznik na końcu wiersza
        if (c < 32 || c == U' ' || c == 0xA0) { space = !u.empty(); continue; }
        if (space) { u += U' '; space = false; }
        u += c;
    }
    if (u.size() > maxChars) { u.resize(maxChars); u += U'…'; }
    return utf32To8(u);
}

bool Document::hasTextAt(const Page& page, Vec2 base) const {
    if (page.source < 0 || page.source >= int(sources_.size())) return false;
    Live& L = live(page);
    if (!L.pe.page) return false;
    if (!L.tp) L.tp = FPDFText_LoadPage(L.pe.page);
    if (!L.tp) return false;
    const Vec2 P = baseToPdf(page).apply(base);
    const int k = FPDFText_GetCharIndexAtPos(L.tp, P.x, P.y, 2.0, 2.0);
    if (k < 0 || FPDFText_IsGenerated(L.tp, k) == 1) return false;
    FPDF_PAGEOBJECT o = FPDFText_GetTextObject(L.tp, k);
    return o && !L.pe.added.count(o) && FPDFTextObj_GetTextRenderMode(o) != FPDF_TEXTRENDERMODE_INVISIBLE;
}

ImagePtr Document::pageImageAppearance(const Page& page, const Obj& obj, int& rot) const {
    rot = 0;
    if (obj.type != ObjType::PageImage || obj.srcObjs.empty()) return nullptr;
    if (obj.image) { rot = obj.imgRot; return obj.image; }   // podmieniony: nowy obraz, obrót z edytora
    Live& L = live(page);
    if (!L.ref) L.ref = FPDF_LoadPage(sources_[size_t(page.source)]->doc, page.index);
    if (!L.ref || obj.srcObjs[0] >= FPDFPage_CountObjects(L.ref)) return nullptr;
    auto chain = resolveChain(FPDFPage_GetObject(L.ref, obj.srcObjs[0]), obj.childPath);
    if (chain.empty() || FPDFPageObj_GetType(chain.back()) != FPDF_PAGEOBJ_IMAGE) return nullptr;
    // render z macierzą obrazu: wygląd jak na stronie bez /Rotate
    FPDF_BITMAP bmp = FPDFImageObj_GetRenderedBitmap(sources_[size_t(page.source)]->doc, L.ref, chain.back());
    if (!bmp) return nullptr;
    const int w = FPDFBitmap_GetWidth(bmp), h = FPDFBitmap_GetHeight(bmp), fmt = FPDFBitmap_GetFormat(bmp);
    const int bpp = fmt == FPDFBitmap_Gray ? 1 : fmt == FPDFBitmap_BGR ? 3 : 4;
    const int stride = FPDFBitmap_GetStride(bmp);
    const auto* src = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bmp));
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* q = src + size_t(y) * size_t(stride) + size_t(x) * size_t(bpp);
            uint8_t* d = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            if (bpp == 1) { d[0] = d[1] = d[2] = q[0]; d[3] = 255; }
            else { d[0] = q[2]; d[1] = q[1]; d[2] = q[0]; d[3] = fmt == FPDFBitmap_BGRA ? q[3] : 255; }
        }
    FPDFBitmap_Destroy(bmp);
    rot = (obj.imgRot + page.baseRot) % 4;   // strona z /Rotate pokazuje obraz obrócony
    return makeImage(w, h, std::move(rgba));
}

bool Document::exportImage(const Page& page, const Obj& obj, const std::string& utf8Path, std::string& err) const {
    if (obj.type != ObjType::PageImage || obj.srcObjs.empty()) { err = "To nie jest obraz z pliku."; return false; }
    Live& L = live(page);
    if (!L.ref) L.ref = FPDF_LoadPage(sources_[size_t(page.source)]->doc, page.index);
    if (!L.ref || obj.srcObjs[0] >= FPDFPage_CountObjects(L.ref)) { err = "Nie znaleziono obrazu."; return false; }
    auto chain = resolveChain(FPDFPage_GetObject(L.ref, obj.srcObjs[0]), obj.childPath);
    if (chain.empty() || FPDFPageObj_GetType(chain.back()) != FPDF_PAGEOBJ_IMAGE) { err = "Nie znaleziono obrazu."; return false; }
    FPDF_PAGEOBJECT img = chain.back();
    // obraz z maską przezroczystości: wersja z nałożoną maską; inaczej oryginalne piksele
    FPDF_BITMAP bmp = nullptr;
    if (FPDFPageObj_HasTransparency(img)) bmp = FPDFImageObj_GetRenderedBitmap(sources_[size_t(page.source)]->doc, L.ref, img);
    if (!bmp) bmp = FPDFImageObj_GetBitmap(img);
    if (!bmp) { err = "Nie udało się odczytać obrazu (nieobsługiwany format danych)."; return false; }
    const int w = FPDFBitmap_GetWidth(bmp), h = FPDFBitmap_GetHeight(bmp), fmt = FPDFBitmap_GetFormat(bmp);
    const int bpp = fmt == FPDFBitmap_Gray ? 1 : fmt == FPDFBitmap_BGR ? 3 : 4;
    const int stride = FPDFBitmap_GetStride(bmp);
    const auto* src = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bmp));
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* q = src + size_t(y) * size_t(stride) + size_t(x) * size_t(bpp);
            uint8_t* d = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            if (bpp == 1) { d[0] = d[1] = d[2] = q[0]; d[3] = 255; }
            else { d[0] = q[2]; d[1] = q[1]; d[2] = q[0]; d[3] = fmt == FPDFBitmap_BGRA ? q[3] : 255; }
        }
    FPDFBitmap_Destroy(bmp);
    if (!writePng(utf8Path, w, h, rgba.data())) { err = "Nie można zapisać pliku w tym miejscu."; return false; }
    return true;
}

bool Document::overlapsOtherText(const Page& page, const Obj& obj) const {
    if (obj.type != ObjType::Text || obj.text.empty()) return false;
    Live& L = live(page);
    if (!L.pe.page) return false;
    const Rect r = layoutBounds(layoutText(page, obj));
    const Xf V = baseToPdf(page);
    const Vec2 a = V.apply({r.x, r.y}), b = V.apply({r.x + r.w, r.y + r.h});
    const float x0 = std::min(a.x, b.x) + 1, x1 = std::max(a.x, b.x) - 1, y0 = std::min(a.y, b.y) + 1, y1 = std::max(a.y, b.y) - 1;
    std::unordered_set<int> own(obj.srcObjs.begin(), obj.srcObjs.end());
    for (size_t i = 0; i < L.pe.pristine.size(); ++i) {
        if (own.count(int(i))) continue;
        FPDF_PAGEOBJECT o = L.pe.pristine[i];
        if (FPDFPageObj_GetType(o) != FPDF_PAGEOBJ_TEXT || L.pe.indexOf(o) < 0) continue;
        float l, bo, rr, t;
        if (!FPDFPageObj_GetBounds(o, &l, &bo, &rr, &t)) continue;
        if (l < x1 && rr > x0 && bo < y1 && t > y0) return true;
    }
    return false;
}

Rect layoutBounds(const TextLayout& l) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    auto add = [&](Vec2 p) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); };
    for (const auto& ln : l.lines)
        for (const Vec2& c : ln.carets) {
            add({c.x + ln.ascent.x, c.y + ln.ascent.y});
            add({c.x + ln.descent.x, c.y + ln.descent.y});
        }
    for (const Vec2& p : l.extent) add(p);
    if (x1 < x0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

// ---------------------------------------------------------------------------- zapis

namespace {
struct Writer : FPDF_FILEWRITE {
    std::vector<uint8_t>* out;
};
int writeBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
    auto* w = static_cast<Writer*>(self);
    const auto* p = static_cast<const uint8_t*>(data);
    w->out->insert(w->out->end(), p, p + size);
    return 1;
}
} // namespace

namespace {

// Plan zapisu strony po zmianach: które obiekty odtworzyć z oryginalnych bajtów (z ewentualnym
// przesunięciem albo nowym stylem), a które oznaczyć znacznikiem i wziąć z zapisu PDFium.
SplicePlan makeSplicePlan(Splicer& splicer, int page, PageEdit& pe) {
    SplicePlan plan;
    plan.page = page;
    std::unordered_map<FPDF_PAGEOBJECT, int> idx;
    for (size_t j = 0; j < pe.pristine.size(); ++j) idx[pe.pristine[j]] = int(j);
    // osadzone grafiki, z których (także głębiej) usunięto obiekt — ich treść przepisuje PDFium
    std::unordered_set<FPDF_PAGEOBJECT> dirtyForms;
    if (!pe.touchedForms.empty()) {
        std::function<bool(FPDF_PAGEOBJECT, int)> touched = [&](FPDF_PAGEOBJECT f, int depth) {
            if (pe.touchedForms.count(f)) return true;
            if (depth > 32) return false;
            const int n = FPDFFormObj_CountObjects(f);
            for (int i = 0; i < n; ++i) {
                FPDF_PAGEOBJECT c = FPDFFormObj_GetObject(f, (unsigned long)i);
                if (c && FPDFPageObj_GetType(c) == FPDF_PAGEOBJ_FORM && touched(c, depth + 1)) return true;
            }
            return false;
        };
        const int n = FPDFPage_CountObjects(pe.page);
        for (int k = 0; k < n; ++k) {
            FPDF_PAGEOBJECT o = FPDFPage_GetObject(pe.page, k);
            if (FPDFPageObj_GetType(o) == FPDF_PAGEOBJ_FORM && touched(o, 0)) dirtyForms.insert(o);
        }
    }
    const int n = FPDFPage_CountObjects(pe.page);
    for (int k = 0; k < n; ++k) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(pe.page, k);
        SpliceItem si;
        const auto it = idx.find(obj);
        si.orig = it != idx.end() ? it->second : -1;
        if (!dirtyForms.count(obj)) {
            if (si.orig >= 0) si.replay = true;
            // obiekt z rozłożonej osadzonej grafiki (np. po edycji obrazu w jej wnętrzu albo zamazaniu)
            else if (!pe.added.count(obj) && splicer.childPath(page, obj, si.path)) si.replay = true;
        }
        if (si.replay) {
            if (auto mv = pe.moved.find(obj); mv != pe.moved.end()) { si.moved = true; si.move = mv->second; }
            if (auto st = pe.styled.find(obj); st != pe.styled.end()) {
                const int f = st->second;
                unsigned r = 0, g = 0, b = 0, a = 255;
                if (f & kStyledWidth) si.ovWidth = FPDFPageObj_GetStrokeWidth(obj, &si.width);
                if ((f & kStyledFill) && FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a)) {
                    if (a != 255) si.replay = false;   // przezroczystość — stan graficzny zapisze PDFium
                    si.ovFill = true; si.fill[0] = uint8_t(r); si.fill[1] = uint8_t(g); si.fill[2] = uint8_t(b);
                }
                if ((f & kStyledStroke) && FPDFPageObj_GetStrokeColor(obj, &r, &g, &b, &a)) {
                    if (a != 255) si.replay = false;
                    si.ovStroke = true; si.stroke[0] = uint8_t(r); si.stroke[1] = uint8_t(g); si.stroke[2] = uint8_t(b);
                }
                if (f & kStyledMode) {
                    int fm = 0;
                    FPDF_BOOL stroke = 0;
                    if (FPDFPath_GetDrawMode(obj, &fm, &stroke)) { si.ovMode = true; si.fillMode = fm; si.strokeOn = stroke != 0; }
                }
            }
        }
        // nowy obraz w miejscu oryginału (zamazane piksele, podmiana): rysowany w kontekście zastąpionego
        // obrazu — z jego stanem graficznym, maskami i grupami osadzonych grafik
        if (!si.replay && si.orig < 0 && FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_IMAGE)
            if (auto r = pe.replaced.find(obj); r != pe.replaced.end()) {
                if (auto oi = idx.find(r->second); oi != idx.end()) { si.orig = oi->second; si.ctx = true; }
                else if (splicer.childPath(page, r->second, si.path)) si.ctx = true;
            }
        if (!si.replay) {
            si.mark = "KPDFs" + std::to_string(k);
            FPDFPageObj_AddMark(obj, si.mark.c_str());
        }
        plan.items.push_back(std::move(si));
    }
    return plan;
}

bool spliceLogEnabled() {
    const char* v = std::getenv("KOREKTOR_SPLICE_LOG");
    return v && *v && *v != '0';
}

} // namespace

bool Document::saveToBytes(std::vector<uint8_t>& out, std::string& err) const {
    if (pages.empty()) { err = "Dokument nie ma stron."; return false; }
    FPDF_DOCUMENT dst = FPDF_CreateNewDocument();
    if (!dst) { err = "Nie udało się utworzyć dokumentu."; return false; }
    FontCache fonts;
    fonts.doc = dst;
    auto fail = [&](const std::string& msg) {
        err = msg;
        fonts.close();
        FPDF_CloseDocument(dst);
        return false;
    };

    // 1) Kopiowanie stron: wszystkie strony z danego pliku w jednym wywołaniu, żeby wspólne
    //    zasoby (czcionki, obrazy) trafiły do wyniku raz, a nie osobno dla każdej strony.
    std::vector<int> importedOrder;
    std::vector<bool> done(sources_.size(), false);
    for (size_t i = 0; i < pages.size(); ++i) {
        const int s = pages[i].source;
        if (done[s]) continue;
        done[s] = true;
        std::vector<int> idx, pos;
        for (size_t j = i; j < pages.size(); ++j)
            if (pages[j].source == s) { idx.push_back(pages[j].index); pos.push_back(int(j)); }
        if (!FPDF_ImportPagesByIndex(dst, sources_[s]->doc, idx.data(), (unsigned long)idx.size(), int(importedOrder.size())))
            return fail("Nie udało się skopiować stron z pliku „" + sources_[s]->name + "”.");
        importedOrder.insert(importedOrder.end(), pos.begin(), pos.end());
    }
    // 2) Ułożenie stron w kolejności z edytora.
    std::vector<int> order(pages.size());
    bool identity = true;
    for (size_t k = 0; k < importedOrder.size(); ++k) {
        order[importedOrder[k]] = int(k);
        identity &= importedOrder[k] == int(k);
    }
    if (!identity && !FPDF_MovePages(dst, order.data(), (unsigned long)order.size(), 0))
        return fail("Nie udało się ułożyć stron.");

    // 3) Zmiany treści, nakładki i obroty.
    //    Najpierw kopia stron przed zmianami: z niej bierzemy oryginalne bajty treści stron, żeby
    //    PDFium nie przepisywał niezmienionych obiektów (gubi kolory CMYK, maski, odstępy tekstu…).
    std::unique_ptr<Splicer> splicer;
    const bool log = spliceLogEnabled();
    const char* noSplice = std::getenv("KOREKTOR_NO_SPLICE");   // diagnostyka: dawny zapis przez PDFium
    if (!(noSplice && *noSplice && *noSplice != '0') &&
        std::any_of(pages.begin(), pages.end(), [](const Page& p) { return !p.objs.empty(); })) {
        std::vector<uint8_t> origBytes;
        Writer w0{};
        w0.version = 1;
        w0.WriteBlock = writeBlock;
        w0.out = &origBytes;
        if (FPDF_SaveAsCopy(dst, &w0, FPDF_NO_INCREMENTAL)) {
            splicer = std::make_unique<Splicer>();
            if (!splicer->load(std::move(origBytes))) {
                if (log) std::fprintf(stderr, "[splice] nie udało się odczytać kopii pliku\n");
                splicer.reset();
            }
        }
    }
    for (size_t i = 0; i < pages.size(); ++i) {
        const Page& p = pages[i];
        FPDF_PAGE pg = FPDF_LoadPage(dst, int(i));
        if (!pg) return fail("Błąd strony " + std::to_string(i + 1) + ".");
        const int base = std::max(0, FPDFPage_GetRotation(pg)) % 4;

        if (!p.objs.empty()) {
            PageEdit pe;
            pe.doc = dst;
            pe.page = pg;
            pe.fonts = &fonts;
            const int n = FPDFPage_CountObjects(pg);
            for (int k = 0; k < n; ++k) pe.pristine.push_back(FPDFPage_GetObject(pg, k));
            std::string why;
            const bool splice = splicer && splicer->prepare(int(i), pg, &why);
            if (log && splicer && !splice) std::fprintf(stderr, "[splice] strona %d: zapis przez PDFium (%s)\n", int(i) + 1, why.c_str());
            applyContentEdits(pe, p);

            const Xf V = baseToPdf(p);
            auto rectOf = [&](const Obj& o, float& l, float& b, float& w, float& h) {
                const Vec2 a = V.apply({o.x, o.y}), c = V.apply({o.x + o.w, o.y + o.h});
                l = std::min(a.x, c.x); b = std::min(a.y, c.y); w = std::fabs(c.x - a.x); h = std::fabs(c.y - a.y);
            };
            for (const Obj& o : p.objs) {
                FPDF_PAGEOBJECT obj = nullptr;
                switch (o.type) {
                case ObjType::Highlight: {
                    float l, b, w, h;
                    rectOf(o, l, b, w, h);
                    obj = FPDFPageObj_CreateNewRect(l, b, w, h);
                    FPDFPageObj_SetFillColor(obj, c8(o.color.r), c8(o.color.g), c8(o.color.b), 115);
                    FPDFPageObj_SetBlendMode(obj, "Multiply");
                    FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_WINDING, 0);
                    break;
                }
                case ObjType::Ink: {
                    if (o.pts.empty()) break;
                    const Vec2 s = V.apply(o.pts[0]);
                    obj = FPDFPageObj_CreateNewPath(s.x, s.y);
                    if (o.pts.size() == 1) FPDFPath_LineTo(obj, s.x + 0.01f, s.y);
                    for (size_t k = 1; k < o.pts.size(); ++k) { const Vec2 q = V.apply(o.pts[k]); FPDFPath_LineTo(obj, q.x, q.y); }
                    FPDFPageObj_SetStrokeColor(obj, c8(o.color.r), c8(o.color.g), c8(o.color.b), c8(o.color.a));
                    FPDFPageObj_SetStrokeWidth(obj, o.size);
                    FPDFPageObj_SetLineCap(obj, FPDF_LINECAP_ROUND);
                    FPDFPageObj_SetLineJoin(obj, FPDF_LINEJOIN_ROUND);
                    FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_NONE, 1);
                    break;
                }
                case ObjType::Image: {
                    if (!o.image || o.image->width <= 0) break;
                    const ImageData& im = *o.image;
                    FPDF_BITMAP bmp = FPDFBitmap_Create(im.width, im.height, 1);
                    auto* dstpx = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bmp));
                    const int stride = FPDFBitmap_GetStride(bmp);
                    for (int y = 0; y < im.height; ++y) {
                        const uint8_t* sp = im.rgba.data() + size_t(y) * im.width * 4;
                        uint8_t* d = dstpx + size_t(y) * stride;
                        for (int x = 0; x < im.width; ++x, sp += 4, d += 4) { d[0] = sp[2]; d[1] = sp[1]; d[2] = sp[0]; d[3] = sp[3]; }
                    }
                    obj = FPDFPageObj_NewImageObj(dst);
                    FPDFImageObj_SetBitmap(&pg, 1, obj, bmp);
                    FPDFBitmap_Destroy(bmp);
                    // kwadrat jednostkowy obrazu (0,0 = lewy dolny róg) -> narożniki na stronie
                    Vec2 c[4];
                    imageCorners(o, c);
                    const Vec2 bl = V.apply(c[3]), br = V.apply(c[2]), tl = V.apply(c[0]);
                    FPDFImageObj_SetMatrix(obj, br.x - bl.x, br.y - bl.y, tl.x - bl.x, tl.y - bl.y, bl.x, bl.y);
                    break;
                }
                default: break;   // tekst i zamazania już naniesione
                }
                if (obj) FPDFPage_InsertObject(pg, obj);
            }
            if (splice) splicer->setPlan(makeSplicePlan(*splicer, int(i), pe));
            FPDFPage_GenerateContent(pg);
            pe.destroyRemoved();
        }
        if (p.extraRot) FPDFPage_SetRotation(pg, (base + p.extraRot) % 4);
        FPDF_ClosePage(pg);
    }

    std::vector<uint8_t> first;
    Writer wr{};
    wr.version = 1;
    wr.WriteBlock = writeBlock;
    wr.out = &first;
    const bool ok = FPDF_SaveAsCopy(dst, &wr, FPDF_NO_INCREMENTAL);
    fonts.close();
    FPDF_CloseDocument(dst);
    if (!ok) { err = "Zapis PDF nie powiódł się."; return false; }
    if (splicer) {
        std::string diag;
        const int n = splicer->apply(first, &diag);
        if (log) std::fprintf(stderr, "[splice] stron złożonych z oryginału: %d\n%s", n, diag.c_str());
    }

    // Drugi przebieg: kopiujemy strony do czystego dokumentu. Przenosi się tylko to, czego strony
    // faktycznie używają — usunięte obrazy, wymazane fragmenty i zastąpione czcionki nie zostają
    // w pliku jako niewidoczne, ale możliwe do odzyskania dane.
    FPDF_DOCUMENT mid = FPDF_LoadMemDocument64(first.data(), first.size(), nullptr);
    FPDF_DOCUMENT clean = FPDF_CreateNewDocument();
    bool ok2 = mid && clean && FPDF_ImportPages(clean, mid, nullptr, 0);
    if (ok2) {
        FPDF_CopyViewerPreferences(clean, mid);
        // Odnośniki: kopiowanie stron gubi cele wewnętrzne, więc odnośniki www/e-mail i wewnętrzne
        // zapisujemy na nowo z modelu (z ewentualnymi zmianami). Inne rodzaje zostają nietknięte.
        for (size_t i = 0; i < pages.size(); ++i) {
            const Page& p = pages[i];
            FPDF_PAGE pg = FPDF_LoadPage(clean, int(i));
            if (!pg) continue;
            for (int k = FPDFPage_GetAnnotCount(pg) - 1; k >= 0; --k) {
                FPDF_ANNOTATION an = FPDFPage_GetAnnot(pg, k);
                bool drop = false;
                if (an && FPDFAnnot_GetSubtype(an) == FPDF_ANNOT_LINK) {
                    FPDF_LINK lk = FPDFAnnot_GetLink(an);
                    drop = lk && linkKind(clean, lk, nullptr, nullptr, nullptr, nullptr) != 0;
                }
                if (an) FPDFPage_CloseAnnot(an);
                if (drop) FPDFPage_RemoveAnnot(pg, k);
            }
            const Xf V = baseToPdf(p);
            for (const Link& ln : p.links) {
                if (linkCovered(p, ln) || ln.box.w <= 0 || ln.box.h <= 0) continue;   // pod zamazaniem znika z treścią
                std::string uri;
                if (!ln.internal()) {
                    uri = normalizeUri(ln.uri);
                } else {
                    const int target = indexOfPage(ln.targetPage);
                    if (target < 0) continue;   // strona docelowa usunięta
                    float y = 0;
                    if (ln.hasTargetY) {
                        const Page& tp = pages[size_t(target)];
                        y = baseToPdf(tp).apply({0, ln.targetY}).y;
                    }
                    char ph[200];
                    std::snprintf(ph, sizeof ph, "kpdf-goto/%d/%d/%.2f/%s", target, ln.hasTargetY ? 1 : 0, y,
                                  "................................................................");
                    uri = ph;
                }
                if (uri.empty()) continue;
                FPDF_ANNOTATION an = FPDFPage_CreateAnnot(pg, FPDF_ANNOT_LINK);
                if (!an) continue;
                const Vec2 a = V.apply({ln.box.x, ln.box.y}), c = V.apply({ln.box.x + ln.box.w, ln.box.y + ln.box.h});
                FS_RECTF r{std::min(a.x, c.x), std::max(a.y, c.y), std::max(a.x, c.x), std::min(a.y, c.y)};
                FPDFAnnot_SetRect(an, &r);
                FPDFAnnot_SetBorder(an, 0, 0, 0);
                FPDFAnnot_SetURI(an, uri.c_str());
                FPDFPage_CloseAnnot(an);
            }
            FPDF_ClosePage(pg);
        }
        out.clear();
        wr.out = &out;
        ok2 = FPDF_SaveAsCopy(clean, &wr, FPDF_NO_INCREMENTAL);
        if (ok2) resolveGotoPlaceholders(out);
    }
    if (clean) FPDF_CloseDocument(clean);
    if (mid) FPDF_CloseDocument(mid);
    if (!ok2) { err = "Zapis PDF nie powiódł się (porządkowanie pliku)."; return false; }
    return true;
}

bool Document::saveToFile(const std::string& utf8Path, std::string& err) const {
    std::vector<uint8_t> bytes;
    if (!saveToBytes(bytes, err)) return false;
    namespace fs = std::filesystem;
    const fs::path path = fs::u8path(utf8Path);
    // Zapis przez plik tymczasowy, żeby nie uszkodzić oryginału, gdy coś pójdzie nie tak.
    fs::path tmp = path;
    tmp += ".zapis";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) { err = "Nie można zapisać pliku w tym miejscu."; return false; }
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) { err = "Błąd zapisu na dysk."; return false; }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) { err = "Nie można zastąpić pliku: " + ec.message(); return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------- pomocnicze

Vec2 baseToShown(const Page& p, Vec2 b) {
    switch (p.extraRot) {
        case 1: return {p.height - b.y, b.x};
        case 2: return {p.width - b.x, p.height - b.y};
        case 3: return {b.y, p.width - b.x};
        default: return b;
    }
}

Vec2 shownToBase(const Page& p, Vec2 s) {
    switch (p.extraRot) {
        case 1: return {s.y, p.height - s.x};
        case 2: return {p.width - s.x, p.height - s.y};
        case 3: return {p.width - s.y, s.x};
        default: return s;
    }
}

const PaperSize kPaperSizes[] = {
    {"A3", "Seria A", 297, 420, false},
    {"A4", "Seria A", 210, 297, false},
    {"A5", "Seria A", 148, 210, false},
    {"A6", "Seria A", 105, 148, false},
    {"A2", "Seria A", 420, 594, false},
    {"A1", "Seria A", 594, 841, false},
    {"A0", "Seria A", 841, 1189, false},
    {"B4", "Seria B", 250, 353, false},
    {"B5", "Seria B", 176, 250, false},
    {"B6", "Seria B", 125, 176, false},
    {"Letter", "Ameryka Północna", 215.9f, 279.4f, false},
    {"Legal", "Ameryka Północna", 215.9f, 355.6f, false},
    {"Tabloid", "Ameryka Północna", 279.4f, 431.8f, false},
    {"Executive", "Ameryka Północna", 184.15f, 266.7f, false},
    {"Koperta DL", "Koperty i inne", 110, 220, true},
    {"Koperta C5", "Koperty i inne", 162, 229, true},
    {"Koperta C6", "Koperty i inne", 114, 162, true},
    {"Pocztówka", "Koperty i inne", 105, 148, true},
    {"Wizytówka", "Koperty i inne", 55, 85, true},
    {"Slajd 16:9", "Koperty i inne", 190.5f, 338.67f, true},
    {"Slajd 4:3", "Koperty i inne", 190.5f, 254, true},
};
const int kPaperSizeCount = int(sizeof(kPaperSizes) / sizeof(kPaperSizes[0]));

std::string paperName(float wPt, float hPt) {
    const float a = std::min(wPt, hPt), b = std::max(wPt, hPt);
    for (int i = 0; i < kPaperSizeCount; ++i) {
        const PaperSize& ps = kPaperSizes[i];
        if (std::string(ps.name) == "Pocztówka") continue;   // ten sam rozmiar co A6
        if (std::fabs(a - mmToPt(ps.wMm)) < 3.f && std::fabs(b - mmToPt(ps.hMm)) < 3.f) return ps.name;
    }
    return std::to_string(int(std::lround(ptToMm(wPt)))) + "×" + std::to_string(int(std::lround(ptToMm(hPt)))) + " mm";
}

} // namespace kpdf
