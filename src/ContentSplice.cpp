// Zapis strony z zachowaniem oryginalnej treści — opis w ContentSplice.h.
//
// Składniki:
//  1. czytnik plików PDF zapisanych przez PDFium (tabela xref, obiekty, strumienie z filtrami),
//  2. interpreter strumienia treści: dla każdego obiektu, który PDFium utworzy z treści strony,
//     zapamiętuje jego bajty i pełny stan graficzny (CTM, obcięcie, stany graficzne, kolory w oryginalnej
//     przestrzeni barw, styl linii, stan tekstu, znaczniki treści),
//  3. złożenie nowej treści: obiekty oryginalne z bajtów, nowe — z zapisu PDFium (wycinane po znacznikach),
//     zasoby strony z oryginału i z zapisu PDFium, dopisane jako aktualizacja przyrostowa pliku.
//
// Liczby są czytane i zapisywane bez funkcji zależnych od ustawień regionalnych (przecinek dziesiętny).

#include "ContentSplice.h"

#include "stb_image.h"         // stbi_zlib_decode_* (implementacja w ImageLoad.cpp)

// z stb_image_write (implementacja w ImageLoad.cpp; nagłówek nie deklaruje tej funkcji)
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>

namespace kpdf::detail {

namespace {

// ============================================================================ liczby

bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }

double numFrom(std::string_view s) {
    size_t i = 0;
    bool neg = false;
    while (i < s.size() && (s[i] == '+' || s[i] == '-')) { if (s[i] == '-') neg = !neg; ++i; }
    double v = 0;
    while (i < s.size() && isDigit(uint8_t(s[i]))) v = v * 10 + (s[i++] - '0');
    if (i < s.size() && s[i] == '.') {
        ++i;
        double f = 0.1;
        while (i < s.size() && isDigit(uint8_t(s[i]))) { v += (s[i++] - '0') * f; f *= 0.1; }
    }
    return neg ? -v : v;
}

void fmtNum(std::string& out, double v) {
    if (!std::isfinite(v)) v = 0;
    long long s = std::llround(v * 1e6);
    if (s == 0) { out += '0'; return; }
    if (s < 0) { out += '-'; s = -s; }
    out += std::to_string(s / 1000000);
    long long fr = s % 1000000;
    if (!fr) return;
    char d[7];
    for (int i = 5; i >= 0; --i) { d[i] = char('0' + fr % 10); fr /= 10; }
    int len = 6;
    while (len > 0 && d[len - 1] == '0') --len;
    out += '.';
    out.append(d, size_t(len));
}

// ============================================================================ macierze (konwencja PDF)

struct M {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// najpierw x, potem y
M mul(const M& x, const M& y) {
    return {x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d,
            x.e * y.a + x.f * y.c + y.e, x.e * y.b + x.f * y.d + y.f};
}

bool inv(const M& m, M& out) {
    const double det = m.a * m.d - m.b * m.c;
    if (std::fabs(det) < 1e-12) return false;
    out.a = m.d / det; out.b = -m.b / det; out.c = -m.c / det; out.d = m.a / det;
    out.e = -(m.e * out.a + m.f * out.c);
    out.f = -(m.e * out.b + m.f * out.d);
    return true;
}

bool approxEq(double x, double y, double tol) { return std::fabs(x - y) <= tol * std::max(1.0, std::max(std::fabs(x), std::fabs(y))); }

bool nearM(const M& x, const FS_MATRIX& y, double tol, bool withTranslation) {
    return approxEq(x.a, y.a, tol) && approxEq(x.b, y.b, tol) && approxEq(x.c, y.c, tol) && approxEq(x.d, y.d, tol) &&
           (!withTranslation || (approxEq(x.e, y.e, tol) && approxEq(x.f, y.f, tol)));
}

void fmtM(std::string& out, const M& m) {
    fmtNum(out, m.a); out += ' '; fmtNum(out, m.b); out += ' '; fmtNum(out, m.c); out += ' ';
    fmtNum(out, m.d); out += ' '; fmtNum(out, m.e); out += ' '; fmtNum(out, m.f);
}

// ============================================================================ leksyka PDF

enum class Tk { End, Num, Name, Str, Hex, ArrO, ArrC, DictO, DictC, Word };

struct Tok {
    Tk t = Tk::End;
    size_t b = 0, e = 0;
};

bool isWs(uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool isDelim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

bool looksNumeric(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == '+' || s[i] == '-')) ++i;
    bool digit = false;
    for (; i < n; ++i) {
        if (isDigit(s[i])) digit = true;
        else if (s[i] != '.') return false;
    }
    return digit;
}

struct Lex {
    const uint8_t* p;
    size_t n;
    size_t pos;
    Lex(const uint8_t* p_, size_t n_, size_t at = 0) : p(p_), n(n_), pos(at) {}

    void skipWs() {
        while (pos < n) {
            if (isWs(p[pos])) ++pos;
            else if (p[pos] == '%') { while (pos < n && p[pos] != '\n' && p[pos] != '\r') ++pos; }
            else break;
        }
    }
    Tok next() {
        skipWs();
        Tok t;
        t.b = pos;
        if (pos >= n) { t.e = pos; return t; }
        const uint8_t c = p[pos];
        switch (c) {
        case '/':
            ++pos;
            while (pos < n && !isWs(p[pos]) && !isDelim(p[pos])) ++pos;
            t.t = Tk::Name;
            break;
        case '(': {
            int depth = 0;
            while (pos < n) {
                const uint8_t ch = p[pos++];
                if (ch == '\\') { if (pos < n) ++pos; }
                else if (ch == '(') ++depth;
                else if (ch == ')' && --depth == 0) break;
            }
            t.t = Tk::Str;
            break;
        }
        case '<':
            if (pos + 1 < n && p[pos + 1] == '<') { pos += 2; t.t = Tk::DictO; }
            else {
                ++pos;
                while (pos < n && p[pos] != '>') ++pos;
                if (pos < n) ++pos;
                t.t = Tk::Hex;
            }
            break;
        case '>':
            if (pos + 1 < n && p[pos + 1] == '>') { pos += 2; t.t = Tk::DictC; }
            else { ++pos; t.t = Tk::Word; }
            break;
        case '[': ++pos; t.t = Tk::ArrO; break;
        case ']': ++pos; t.t = Tk::ArrC; break;
        case '{': case '}': case ')': ++pos; t.t = Tk::Word; break;
        default:
            while (pos < n && !isWs(p[pos]) && !isDelim(p[pos])) ++pos;
            t.t = looksNumeric(p + t.b, pos - t.b) ? Tk::Num : Tk::Word;
        }
        t.e = pos;
        return t;
    }
    std::string_view text(const Tok& t) const { return {reinterpret_cast<const char*>(p + t.b), t.e - t.b}; }
};

int hexVal(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "/Nazwa#20x" -> "Nazwa x"
std::string decodeName(std::string_view raw) {
    std::string out;
    size_t i = raw.empty() || raw[0] != '/' ? 0 : 1;
    for (; i < raw.size(); ++i) {
        if (raw[i] == '#' && i + 2 < raw.size()) {
            const int h = hexVal(uint8_t(raw[i + 1])), l = hexVal(uint8_t(raw[i + 2]));
            if (h >= 0 && l >= 0) { out += char(h * 16 + l); i += 2; continue; }
        }
        out += raw[i];
    }
    return out;
}

std::string encodeName(const std::string& name) {
    std::string out = "/";
    for (unsigned char c : name) {
        if (c <= 32 || c >= 127 || c == '#' || isDelim(c)) {
            static const char* hx = "0123456789ABCDEF";
            out += '#'; out += hx[c >> 4]; out += hx[c & 15];
        } else out += char(c);
    }
    return out;
}

// ============================================================================ obiekty PDF

struct PObj {
    enum K : uint8_t { Null, Bool, Num, Name, Str, Arr, Dict, Ref, Other } k = Null;
    std::string s;   // Num/Bool/Str/Other: tekst źródłowy; Name: nazwa zdekodowana (bez '/')
    double num = 0;
    int ref = 0, gen = 0;
    std::vector<PObj> a;
    std::vector<std::pair<std::string, PObj>> d;

    const PObj* get(std::string_view key) const {
        for (auto& kv : d) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    void set(const std::string& key, PObj v) {
        for (auto& kv : d) if (kv.first == key) { kv.second = std::move(v); return; }
        d.emplace_back(key, std::move(v));
    }
    bool isName(std::string_view v) const { return k == Name && s == v; }
};

PObj mkRef(int num, int gen = 0) { PObj o; o.k = PObj::Ref; o.ref = num; o.gen = gen; return o; }

bool parseFrom(Lex& lx, const Tok& t, PObj& o, bool allowRef, int depth);

bool parseObj(Lex& lx, PObj& o, bool allowRef, int depth = 0) {
    const Tok t = lx.next();
    return parseFrom(lx, t, o, allowRef, depth);
}

bool parseFrom(Lex& lx, const Tok& t, PObj& o, bool allowRef, int depth) {
    if (depth > 100) return false;
    switch (t.t) {
    case Tk::Num: {
        if (allowRef) {
            const size_t save = lx.pos;
            const Tok t2 = lx.next();
            if (t2.t == Tk::Num) {
                const Tok t3 = lx.next();
                if (t3.t == Tk::Word && lx.text(t3) == "R") {
                    o.k = PObj::Ref;
                    o.ref = int(numFrom(lx.text(t)));
                    o.gen = int(numFrom(lx.text(t2)));
                    return true;
                }
            }
            lx.pos = save;
        }
        o.k = PObj::Num;
        o.s = std::string(lx.text(t));
        o.num = numFrom(o.s);
        return true;
    }
    case Tk::Name: o.k = PObj::Name; o.s = decodeName(lx.text(t)); return true;
    case Tk::Str: case Tk::Hex: o.k = PObj::Str; o.s = std::string(lx.text(t)); return true;
    case Tk::ArrO:
        o.k = PObj::Arr;
        for (;;) {
            const Tok e = lx.next();
            if (e.t == Tk::ArrC) return true;
            if (e.t == Tk::End) return false;
            PObj v;
            if (!parseFrom(lx, e, v, allowRef, depth + 1)) return false;
            o.a.push_back(std::move(v));
        }
    case Tk::DictO:
        o.k = PObj::Dict;
        for (;;) {
            const Tok kt = lx.next();
            if (kt.t == Tk::DictC) return true;
            if (kt.t != Tk::Name) return false;
            PObj v;
            const Tok vt = lx.next();
            if (vt.t == Tk::DictC) { o.set(decodeName(lx.text(kt)), PObj{}); return true; }
            if (!parseFrom(lx, vt, v, allowRef, depth + 1)) return false;
            o.set(decodeName(lx.text(kt)), std::move(v));
        }
    case Tk::Word: {
        const std::string_view w = lx.text(t);
        if (w == "true" || w == "false") { o.k = PObj::Bool; o.s = std::string(w); return true; }
        if (w == "null") { o.k = PObj::Null; return true; }
        o.k = PObj::Other;
        o.s = std::string(w);
        return true;
    }
    default: return false;
    }
}

void ser(const PObj& o, std::string& out) {
    switch (o.k) {
    case PObj::Null: out += "null"; break;
    case PObj::Bool: case PObj::Num: case PObj::Str: case PObj::Other: out += o.s; break;
    case PObj::Name: out += encodeName(o.s); break;
    case PObj::Ref: out += std::to_string(o.ref) + ' ' + std::to_string(o.gen) + " R"; break;
    case PObj::Arr:
        out += '[';
        for (size_t i = 0; i < o.a.size(); ++i) { if (i) out += ' '; ser(o.a[i], out); }
        out += ']';
        break;
    case PObj::Dict:
        out += "<<";
        for (auto& kv : o.d) { out += encodeName(kv.first); out += ' '; ser(kv.second, out); out += ' '; }
        out += ">>";
        break;
    }
}

std::string serStr(const PObj& o) { std::string s; ser(o, s); return s; }

// Czy tekst jest pusty (dla Tj/TJ): "()" albo "<>".
bool emptyString(const PObj& o) {
    if (o.k != PObj::Str || o.s.size() < 2) return true;
    if (o.s[0] == '(') return o.s.size() == 2;
    for (size_t i = 1; i + 1 < o.s.size(); ++i) if (hexVal(uint8_t(o.s[i])) >= 0) return false;
    return true;
}

// ============================================================================ filtry strumieni

bool flate(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    if (n > size_t(INT32_MAX)) return false;
    int len = 0;
    char* r = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(p), int(n), int(n * 4 + 1024), &len, 1);
    if (!r) r = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(p), int(n), int(n * 4 + 1024), &len, 0);
    if (!r) return false;
    out.assign(reinterpret_cast<uint8_t*>(r), reinterpret_cast<uint8_t*>(r) + len);
    std::free(r);
    return true;
}

bool pngPredictor(std::vector<uint8_t>& data, int colors, int bpc, int columns) {
    const size_t bpp = size_t(std::max(1, (colors * bpc + 7) / 8));
    const size_t row = size_t((colors * bpc * columns + 7) / 8);
    if (row == 0) return false;
    std::vector<uint8_t> out;
    std::vector<uint8_t> prev(row, 0);
    size_t i = 0;
    while (i + 1 + row <= data.size()) {
        const uint8_t ft = data[i++];
        std::vector<uint8_t> cur(data.begin() + long(i), data.begin() + long(i + row));
        i += row;
        for (size_t x = 0; x < row; ++x) {
            const int left = x >= bpp ? cur[x - bpp] : 0, up = prev[x], ul = x >= bpp ? prev[x - bpp] : 0;
            int v = cur[x];
            switch (ft) {
            case 0: break;
            case 1: v += left; break;
            case 2: v += up; break;
            case 3: v += (left + up) / 2; break;
            case 4: {
                const int pp = left + up - ul, pa = std::abs(pp - left), pb = std::abs(pp - up), pc = std::abs(pp - ul);
                v += (pa <= pb && pa <= pc) ? left : (pb <= pc ? up : ul);
                break;
            }
            default: return false;
            }
            cur[x] = uint8_t(v);
        }
        out.insert(out.end(), cur.begin(), cur.end());
        prev.swap(cur);
    }
    data.swap(out);
    return true;
}

bool asciiHex(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    out.clear();
    int hi = -1;
    for (uint8_t c : in) {
        if (c == '>') break;
        const int v = hexVal(c);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { out.push_back(uint8_t(hi * 16 + v)); hi = -1; }
    }
    if (hi >= 0) out.push_back(uint8_t(hi * 16));
    return true;
}

bool ascii85(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t tuple = 0;
    int cnt = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const uint8_t c = in[i];
        if (c == '~') break;
        if (isWs(c)) continue;
        if (c == 'z' && cnt == 0) { out.insert(out.end(), 4, 0); continue; }
        if (c < '!' || c > 'u') return false;
        tuple = tuple * 85 + uint32_t(c - '!');
        if (++cnt == 5) {
            for (int k = 3; k >= 0; --k) out.push_back(uint8_t(tuple >> (k * 8)));
            tuple = 0; cnt = 0;
        }
    }
    if (cnt > 1) {
        for (int k = cnt; k < 5; ++k) tuple = tuple * 85 + 84;
        for (int k = 0; k < cnt - 1; ++k) out.push_back(uint8_t(tuple >> ((3 - k) * 8)));
    }
    return true;
}

bool lzw(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, int early) {
    out.clear();
    std::vector<std::vector<uint8_t>> table;
    auto reset = [&] {
        table.assign(258, {});
        for (int i = 0; i < 256; ++i) table[size_t(i)] = {uint8_t(i)};
    };
    reset();
    int bits = 9;
    size_t bitPos = 0;
    std::vector<uint8_t> prev;
    const size_t totalBits = in.size() * 8;
    while (bitPos + size_t(bits) <= totalBits) {
        int code = 0;
        for (int k = 0; k < bits; ++k, ++bitPos) code = (code << 1) | ((in[bitPos / 8] >> (7 - bitPos % 8)) & 1);
        if (code == 256) { reset(); bits = 9; prev.clear(); continue; }
        if (code == 257) break;
        std::vector<uint8_t> entry;
        if (code < int(table.size())) entry = table[size_t(code)];
        else if (code == int(table.size()) && !prev.empty()) { entry = prev; entry.push_back(prev[0]); }
        else return false;
        out.insert(out.end(), entry.begin(), entry.end());
        if (!prev.empty()) { auto ne = prev; ne.push_back(entry[0]); table.push_back(std::move(ne)); }
        prev = std::move(entry);
        const int sz = int(table.size()) + early;
        if (sz >= 4096) bits = 12;
        else if (sz >= 2048) bits = 12;
        else if (sz >= 1024) bits = 11;
        else if (sz >= 512) bits = 10;
    }
    return true;
}

bool runLength(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    out.clear();
    for (size_t i = 0; i < in.size();) {
        const int len = in[i++];
        if (len == 128) break;
        if (len < 128) {
            const size_t n = size_t(len) + 1;
            if (i + n > in.size()) return false;
            out.insert(out.end(), in.begin() + long(i), in.begin() + long(i + n));
            i += n;
        } else {
            if (i >= in.size()) return false;
            out.insert(out.end(), size_t(257 - len), in[i++]);
        }
    }
    return true;
}

// ============================================================================ plik PDF (zapis PDFium)

struct PdfFile {
    std::vector<uint8_t> buf;
    std::map<int, std::pair<size_t, int>> xref;   // numer -> (pozycja, generacja)
    PObj trailer;
    size_t startxref = 0;
    struct Cached { PObj o; bool stream = false; size_t sb = 0; size_t hdrEnd = 0; };
    std::unordered_map<int, Cached> cache;

    bool load() {
        const size_t n = buf.size();
        if (n < 32) return false;
        const char* key = "startxref";
        size_t at = std::string::npos;
        for (size_t i = n >= 9 ? n - 9 : 0; i + 1 > 0 && n - i < 4096; --i) {
            if (std::memcmp(buf.data() + i, key, 9) == 0) { at = i; break; }
            if (i == 0) break;
        }
        if (at == std::string::npos) return false;
        Lex lx(buf.data(), n, at + 9);
        const Tok t = lx.next();
        if (t.t != Tk::Num) return false;
        startxref = size_t(numFrom(lx.text(t)));
        size_t pos = startxref;
        std::set<size_t> seen;
        bool first = true;
        while (pos < n && seen.insert(pos).second) {
            Lex x(buf.data(), n, pos);
            const Tok w = x.next();
            if (w.t != Tk::Word || x.text(w) != "xref") return false;   // strumień xref — nie obsługujemy
            for (;;) {
                const Tok a = x.next();
                if (a.t == Tk::Word && x.text(a) == "trailer") break;
                if (a.t != Tk::Num) return false;
                const Tok c = x.next();
                if (c.t != Tk::Num) return false;
                const int start = int(numFrom(x.text(a))), count = int(numFrom(x.text(c)));
                for (int i = 0; i < count; ++i) {
                    const Tok off = x.next(), gen = x.next(), ty = x.next();
                    if (off.t != Tk::Num || gen.t != Tk::Num || ty.t != Tk::Word) return false;
                    const int num = start + i;
                    if (x.text(ty) == "n" && !xref.count(num))
                        xref[num] = {size_t(numFrom(x.text(off))), int(numFrom(x.text(gen)))};
                    else if (!xref.count(num)) xref[num] = {0, -1};   // wolny — blokuje starsze wpisy
                }
            }
            PObj tr;
            if (!parseObj(x, tr, true) || tr.k != PObj::Dict) return false;
            if (first) { trailer = tr; first = false; }
            const PObj* prev = tr.get("Prev");
            if (!prev || prev->k != PObj::Num) break;
            pos = size_t(prev->num);
        }
        return !first;
    }

    const Cached* entry(int num) {
        auto it = cache.find(num);
        if (it != cache.end()) return &it->second;
        auto xr = xref.find(num);
        if (xr == xref.end() || xr->second.second < 0 || xr->second.first >= buf.size()) return nullptr;
        Lex lx(buf.data(), buf.size(), xr->second.first);
        const Tok a = lx.next(), b = lx.next(), c = lx.next();
        if (a.t != Tk::Num || b.t != Tk::Num || c.t != Tk::Word || lx.text(c) != "obj" || int(numFrom(lx.text(a))) != num) return nullptr;
        Cached ce;
        if (!parseObj(lx, ce.o, true)) return nullptr;
        ce.hdrEnd = lx.pos;
        const size_t save = lx.pos;
        const Tok s = lx.next();
        if (s.t == Tk::Word && lx.text(s) == "stream") {
            size_t p = s.e;
            if (p < buf.size() && buf[p] == '\r') ++p;
            if (p < buf.size() && buf[p] == '\n') ++p;
            ce.stream = true;
            ce.sb = p;
        } else lx.pos = save;
        return &cache.emplace(num, std::move(ce)).first->second;
    }

    const PObj* obj(int num) { const Cached* c = entry(num); return c ? &c->o : nullptr; }

    // Rozwiązuje odwołanie (pośrednie) — wynik ważny, dopóki istnieje plik.
    const PObj* resolve(const PObj* o, int depth = 0) {
        while (o && o->k == PObj::Ref && depth++ < 32) o = obj(o->ref);
        return o;
    }

    // Tekst obiektu od "obj" do strumienia/końca (do porównania tożsamości obiektów).
    std::string_view rawHeader(int num) {
        const Cached* c = entry(num);
        if (!c) return {};
        const size_t b = xref[num].first;
        return {reinterpret_cast<const char*>(buf.data() + b), c->hdrEnd - b};
    }

    bool streamData(int num, std::vector<uint8_t>& out) {
        const Cached* c = entry(num);
        if (!c || !c->stream || c->o.k != PObj::Dict) return false;
        size_t len = 0;
        const PObj* L = resolve(c->o.get("Length"));
        if (L && L->k == PObj::Num && L->num >= 0) len = size_t(L->num);
        if (!L || c->sb + len > buf.size()) {
            const char* key = "endstream";
            const auto it = std::search(buf.begin() + long(c->sb), buf.end(), key, key + 9);
            if (it == buf.end()) return false;
            len = size_t(it - buf.begin()) - c->sb;
        }
        out.assign(buf.begin() + long(c->sb), buf.begin() + long(c->sb + len));
        // filtry
        const PObj* f = resolve(c->o.get("Filter"));
        const PObj* dp = resolve(c->o.get("DecodeParms"));
        std::vector<const PObj*> filters, parms;
        if (f && f->k == PObj::Name) { filters.push_back(f); parms.push_back(dp); }
        else if (f && f->k == PObj::Arr) {
            for (size_t i = 0; i < f->a.size(); ++i) {
                filters.push_back(resolve(&f->a[i]));
                parms.push_back(dp && dp->k == PObj::Arr && i < dp->a.size() ? resolve(&dp->a[i]) : nullptr);
            }
        }
        for (size_t i = 0; i < filters.size(); ++i) {
            const PObj* fi = filters[i];
            if (!fi || fi->k != PObj::Name) return false;
            const std::string& fn = fi->s;
            std::vector<uint8_t> tmp;
            const PObj* pr = parms[i] && parms[i]->k == PObj::Dict ? parms[i] : nullptr;
            auto parmNum = [&](const char* k, double def) {
                const PObj* v = pr ? resolve(pr->get(k)) : nullptr;
                return v && v->k == PObj::Num ? v->num : def;
            };
            if (fn == "FlateDecode" || fn == "Fl") {
                if (!flate(out.data(), out.size(), tmp)) return false;
                const int pred = int(parmNum("Predictor", 1));
                if (pred >= 10) {
                    if (!pngPredictor(tmp, int(parmNum("Colors", 1)), int(parmNum("BitsPerComponent", 8)), int(parmNum("Columns", 1))))
                        return false;
                } else if (pred != 1) return false;
            } else if (fn == "ASCIIHexDecode" || fn == "AHx") asciiHex(out, tmp);
            else if (fn == "ASCII85Decode" || fn == "A85") { if (!ascii85(out, tmp)) return false; }
            else if (fn == "LZWDecode" || fn == "LZW") { if (!lzw(out, tmp, int(parmNum("EarlyChange", 1)))) return false; }
            else if (fn == "RunLengthDecode" || fn == "RL") { if (!runLength(out, tmp)) return false; }
            else return false;
            out.swap(tmp);
        }
        return true;
    }

    // Strony w kolejności (numery obiektów).
    std::vector<int> pages() {
        std::vector<int> out;
        const PObj* root = resolve(trailer.get("Root"));
        if (!root || root->k != PObj::Dict) return out;
        const PObj* pagesRef = root->get("Pages");
        if (!pagesRef || pagesRef->k != PObj::Ref) return out;
        std::set<int> seen;
        walk(pagesRef->ref, out, seen, 0);
        return out;
    }
    void walk(int num, std::vector<int>& out, std::set<int>& seen, int depth) {
        if (depth > 64 || !seen.insert(num).second) return;
        const PObj* node = obj(num);
        if (!node || node->k != PObj::Dict) return;
        const PObj* kids = resolve(node->get("Kids"));
        const PObj* type = node->get("Type");
        if (kids && kids->k == PObj::Arr && !(type && type->isName("Page"))) {
            for (const PObj& k : kids->a) if (k.k == PObj::Ref) walk(k.ref, out, seen, depth + 1);
        } else out.push_back(num);
    }

    // Słownik zasobów strony (z dziedziczeniem po drzewie stron).
    const PObj* pageResources(int pageNum) {
        const PObj* node = obj(pageNum);
        for (int d = 0; node && node->k == PObj::Dict && d < 32; ++d) {
            const PObj* r = resolve(node->get("Resources"));
            if (r && r->k == PObj::Dict) return r;
            node = resolve(node->get("Parent"));
        }
        return nullptr;
    }

    bool pageContent(int pageNum, std::vector<uint8_t>& out) {
        out.clear();
        const PObj* pg = obj(pageNum);
        if (!pg || pg->k != PObj::Dict) return false;
        const PObj* c = pg->get("Contents");
        std::vector<int> streams;
        if (!c) return true;
        if (c->k == PObj::Ref) {
            const PObj* t = obj(c->ref);
            if (t && t->k == PObj::Arr) { for (const PObj& e : t->a) if (e.k == PObj::Ref) streams.push_back(e.ref); }
            else streams.push_back(c->ref);
        } else if (c->k == PObj::Arr) {
            for (const PObj& e : c->a) if (e.k == PObj::Ref) streams.push_back(e.ref);
        }
        for (int s : streams) {
            std::vector<uint8_t> part;
            if (!streamData(s, part)) return false;
            out.insert(out.end(), part.begin(), part.end());
            out.push_back('\n');
        }
        return true;
    }
};

// ============================================================================ interpreter treści

// Przechodzi przez operatory strumienia treści (z obrazami wbudowanymi BI … ID … EI).
// onOp(op, operandy, początek, koniec) — zakres bajtów całej operacji (operandy + operator).
template <class F>
bool walkOps(const uint8_t* p, size_t n, F&& onOp) {
    Lex lx(p, n);
    std::vector<PObj> ops;
    size_t opStart = 0;
    for (;;) {
        const Tok t = lx.next();
        if (t.t == Tk::End) break;
        if (t.t == Tk::ArrC || t.t == Tk::DictC) continue;
        const bool operand = t.t != Tk::Word || lx.text(t) == "true" || lx.text(t) == "false" || lx.text(t) == "null";
        if (operand) {
            if (ops.empty()) opStart = t.b;
            PObj o;
            if (!parseFrom(lx, t, o, false, 0)) return false;
            ops.push_back(std::move(o));
            if (ops.size() > 4096) return false;
            continue;
        }
        const std::string_view op = lx.text(t);
        if (ops.empty()) opStart = t.b;
        if (op == "BI") {
            const size_t start = t.b;
            PObj dict;
            dict.k = PObj::Dict;
            bool ok = false;
            for (;;) {
                const Tok k = lx.next();
                if (k.t == Tk::End) break;
                if (k.t == Tk::Word && lx.text(k) == "ID") { ok = true; break; }
                if (k.t != Tk::Name) continue;
                PObj v;
                if (!parseObj(lx, v, false)) break;
                dict.set(decodeName(lx.text(k)), std::move(v));
            }
            if (!ok) return false;
            size_t i = lx.pos + 1;   // jeden znak odstępu po ID
            size_t end = std::string::npos;
            for (; i + 1 < n; ++i)
                if (p[i] == 'E' && p[i + 1] == 'I' && i > 0 && isWs(p[i - 1]) && (i + 2 >= n || isWs(p[i + 2]) || isDelim(p[i + 2]))) {
                    end = i + 2;
                    break;
                }
            if (end == std::string::npos) return false;
            lx.pos = end;
            std::vector<PObj> one{std::move(dict)};
            if (!onOp(std::string_view("BI"), one, start, end)) return false;
            ops.clear();
            continue;
        }
        if (!onOp(op, ops, opStart, lx.pos)) return false;
        ops.clear();
    }
    return true;
}

// Wpis dziennika stanu: operacje odtwarzające stan graficzny przed obiektem.
enum Kind : uint8_t {
    kW = 1, kJ, kj, kM, kd, kri, ki, kFillCS, kFillC, kStrokeCS, kStrokeC, kTc, kTw, kTz, kTL, kTf, kTr, kTs, kKindCount
};

struct GState {
    std::vector<std::string> keep;                 // cm, obcięcia, gs — w kolejności (zależą od CTM)
    std::vector<std::pair<uint8_t, std::string>> params;   // parametry nadpisywane (styl, kolory, tekst)
    M ctm;
    bool fontOk = false;
    double th = 1, rise = 0;
    int tr = 0, cap = 0;
    M keepCtm;            // CTM odpowiadające ostatniemu wpisowi "cm" w keep (do łączenia kolejnych cm)
    bool lastIsCm = false;
};

struct Snapshot {
    std::vector<std::string> lines;
};

struct CItem {
    int type = 0;   // FPDF_PAGEOBJ_*
    std::shared_ptr<const Snapshot> state;
    std::shared_ptr<const std::vector<std::string>> marks;
    M ctm;
    std::string pathBytes, clipOp, paintOp;   // ścieżka
    M tm;                                     // tekst: macierz tekstu (Tm)
    double rise = 0, th = 1;
    std::string show;                         // tekst: operacja Tj/TJ
    double tx = 0, ty = 0;                    // tekst: pozycja w przestrzeni Tm (z PDFium)
    double lineX = 0, lineY = 0;              // tekst: początek wiersza (Td/TD/T*/Tm) — dokładnie
    bool lineStart = false;                   // tekst: pierwszy napis od początku wiersza
    std::string raw;                          // Do / sh / BI…EI
    std::string xname;                        // Do: nazwa XObject (zdekodowana)
};

struct Interp {
    PdfFile& file;
    const PObj* res;
    std::vector<CItem> items;
    std::string why;

    GState cur;
    std::vector<GState> stack;
    std::vector<std::string> marks;
    std::shared_ptr<const Snapshot> snap;
    std::shared_ptr<const std::vector<std::string>> marksSnap;
    bool dirty = true, marksDirty = true;

    // ścieżka (reguły PDFium: AddPathPoint / AddPathObject)
    struct Pt { double x, y; int type; bool close; };   // type: 0 move, 1 line, 2 bezier
    std::vector<Pt> pts;
    double curX = 0, curY = 0, startX = 0, startY = 0;
    std::string pathBytes;
    std::string clipOp;
    M tm;
    bool inText = false;
    double lineX = 0, lineY = 0, leading = 0;
    bool atLineStart = true;   // od ostatniego Td/TD/T*/Tm nie było przesunięcia tekstu

    Interp(PdfFile& f, const PObj* r) : file(f), res(r) {}

    const PObj* resource(const char* cat, const std::string& name) {
        if (!res) return nullptr;
        const PObj* c = file.resolve(res->get(cat));
        if (!c || c->k != PObj::Dict) return nullptr;
        return c->get(name);
    }

    void setParam(uint8_t kind, std::string text, std::initializer_list<uint8_t> alsoErase = {}) {
        auto& p = cur.params;
        p.erase(std::remove_if(p.begin(), p.end(), [&](const auto& e) {
                    if (e.first == kind) return true;
                    for (uint8_t k : alsoErase) if (e.first == k) return true;
                    return false;
                }), p.end());
        p.emplace_back(kind, std::move(text));
        dirty = true;
    }
    void eraseParams(std::initializer_list<uint8_t> kinds) {
        auto& p = cur.params;
        p.erase(std::remove_if(p.begin(), p.end(), [&](const auto& e) {
                    for (uint8_t k : kinds) if (e.first == k) return true;
                    return false;
                }), p.end());
        dirty = true;
    }
    void addKeep(std::string text) {
        if (!cur.keep.empty() && cur.keep.back() == text && text.find(" gs") != std::string::npos) return;
        cur.keep.push_back(std::move(text));
        cur.lastIsCm = false;
        dirty = true;
    }
    void addCm(const M& m) {
        cur.ctm = mul(m, cur.ctm);
        if (cur.lastIsCm && !cur.keep.empty()) {
            cur.keepCtm = mul(m, cur.keepCtm);
            std::string s;
            fmtM(s, cur.keepCtm);
            cur.keep.back() = s + " cm";
        } else {
            cur.keepCtm = m;
            std::string s;
            fmtM(s, m);
            cur.keep.push_back(s + " cm");
            cur.lastIsCm = true;
        }
        dirty = true;
    }

    std::shared_ptr<const Snapshot> snapshot() {
        if (dirty || !snap) {
            auto s = std::make_shared<Snapshot>();
            s->lines = cur.keep;
            for (auto& pr : cur.params) s->lines.push_back(pr.second);
            snap = std::move(s);
            dirty = false;
        }
        return snap;
    }
    std::shared_ptr<const std::vector<std::string>> marksSnapshot() {
        if (marksDirty || !marksSnap) {
            marksSnap = std::make_shared<std::vector<std::string>>(marks);
            marksDirty = false;
        }
        return marksSnap;
    }

    CItem& newItem(int type) {
        items.emplace_back();
        CItem& it = items.back();
        it.type = type;
        it.state = snapshot();
        it.marks = marksSnapshot();
        it.ctm = cur.ctm;
        return it;
    }

    void addPoint(double x, double y, int type) {
        if (type == 0 && !pts.empty() && !pts.back().close && pts.back().type == 0 && curX == x && curY == y) return;
        curX = x; curY = y;
        if (type == 0) {
            startX = x; startY = y;
            if (!pts.empty() && pts.back().type == 0 && !pts.back().close) { pts.back().x = x; pts.back().y = y; return; }
        } else if (pts.empty()) return;
        pts.push_back({x, y, type, false});
    }
    void addPointClose(double x, double y, int type) {
        curX = x; curY = y;
        if (pts.empty()) return;
        pts.push_back({x, y, type, true});
    }
    void closePath() {
        if (pts.empty()) return;
        if (startX != curX || startY != curY) addPointClose(startX, startY, 1);
        else pts.back().close = true;
    }

    // Zakończenie ścieżki operatorem malowania (albo "n").
    void paintPath(std::string_view op, bool fill, bool stroke) {
        std::vector<Pt> p;
        p.swap(pts);
        const std::string clip = clipOp;
        clipOp.clear();
        std::string bytes;
        bytes.swap(pathBytes);
        bool object = false;
        if (!p.empty()) {
            bool usable = true;
            if (p.size() == 1) {
                usable = false;
                if (clip.empty() && p[0].type == 0 && p[0].close && cur.cap == 1) { p.push_back({p[0].x, p[0].y, 1, true}); usable = true; }
            }
            if (usable) {
                if (p.back().type == 0 && !p.back().close) p.pop_back();
                object = stroke || fill;
            }
        }
        if (object) {
            CItem& it = newItem(FPDF_PAGEOBJ_PATH);
            it.pathBytes = bytes;
            it.clipOp = clip;
            it.paintOp = std::string(op);
        }
        if (!clip.empty() && !p.empty()) addKeep(bytes + " " + clip + " n");
    }

    bool fail(std::string w) { why = std::move(w); return false; }

    bool run(const uint8_t* p, size_t n) {
        return walkOps(p, n, [&](std::string_view op, std::vector<PObj>& o, size_t b, size_t e) {
            const std::string raw(reinterpret_cast<const char*>(p + b), e - b);
            auto num = [&](size_t i) { return i < o.size() && o[i].k == PObj::Num ? o[i].num : 0.0; };
            const size_t k = o.size();
            // ---------- stan graficzny
            if (op == "q") {
                if (stack.size() > 256) return fail("za głęboko zagnieżdżone q");
                stack.push_back(cur);
                cur.lastIsCm = false;
                return true;
            }
            if (op == "Q") {
                if (!stack.empty()) { cur = std::move(stack.back()); stack.pop_back(); dirty = true; }
                return true;
            }
            if (op == "cm") {
                if (k < 6) return true;
                addCm({num(k - 6), num(k - 5), num(k - 4), num(k - 3), num(k - 2), num(k - 1)});
                return true;
            }
            if (op == "w") { setParam(kW, raw); return true; }
            if (op == "J") { setParam(kJ, raw); cur.cap = int(num(0)); return true; }
            if (op == "j") { setParam(kj, raw); return true; }
            if (op == "M") { setParam(kM, raw); return true; }
            if (op == "d") { setParam(kd, raw); return true; }
            if (op == "ri") { setParam(kri, raw); return true; }
            if (op == "i") { setParam(ki, raw); return true; }
            if (op == "gs") {
                if (k < 1 || o[0].k != PObj::Name) return true;
                const PObj* gs = file.resolve(resource("ExtGState", o[0].s));
                if (gs && gs->k == PObj::Dict) {
                    // parametry ustawiane przez gs zastępują wcześniejsze operatory
                    if (gs->get("LW")) eraseParams({kW});
                    if (const PObj* lc = file.resolve(gs->get("LC"))) { eraseParams({kJ}); if (lc->k == PObj::Num) cur.cap = int(lc->num); }
                    if (gs->get("LJ")) eraseParams({kj});
                    if (gs->get("ML")) eraseParams({kM});
                    if (gs->get("D")) eraseParams({kd});
                    if (gs->get("RI")) eraseParams({kri});
                    if (gs->get("FL")) eraseParams({ki});
                    if (gs->get("Font")) { eraseParams({kTf}); cur.fontOk = true; }
                }
                addKeep(raw);
                return true;
            }
            // ---------- kolory
            if (op == "cs") { setParam(kFillCS, raw, {kFillC}); return true; }
            if (op == "CS") { setParam(kStrokeCS, raw, {kStrokeC}); return true; }
            if (op == "sc" || op == "scn") { setParam(kFillC, raw); return true; }
            if (op == "SC" || op == "SCN") { setParam(kStrokeC, raw); return true; }
            if (op == "g" || op == "rg" || op == "k") { setParam(kFillCS, raw, {kFillC}); return true; }
            if (op == "G" || op == "RG" || op == "K") { setParam(kStrokeCS, raw, {kStrokeC}); return true; }
            // ---------- ścieżki
            if (op == "m") { if (k >= 2) addPoint(num(k - 2), num(k - 1), 0); pathBytes += raw; pathBytes += ' '; return true; }
            if (op == "l") { if (k >= 2) addPoint(num(k - 2), num(k - 1), 1); pathBytes += raw; pathBytes += ' '; return true; }
            if (op == "c") {
                if (k >= 6) { addPoint(num(k - 6), num(k - 5), 2); addPoint(num(k - 4), num(k - 3), 2); addPoint(num(k - 2), num(k - 1), 2); }
                pathBytes += raw; pathBytes += ' ';
                return true;
            }
            if (op == "v") {
                if (k >= 4) { addPoint(curX, curY, 2); addPoint(num(k - 4), num(k - 3), 2); addPoint(num(k - 2), num(k - 1), 2); }
                pathBytes += raw; pathBytes += ' ';
                return true;
            }
            if (op == "y") {
                if (k >= 4) { addPoint(num(k - 4), num(k - 3), 2); addPoint(num(k - 2), num(k - 1), 2); addPoint(num(k - 2), num(k - 1), 2); }
                pathBytes += raw; pathBytes += ' ';
                return true;
            }
            if (op == "h") { closePath(); pathBytes += raw; pathBytes += ' '; return true; }
            if (op == "re") {
                if (k >= 4) {
                    const double x = num(k - 4), y = num(k - 3), w = num(k - 2), h = num(k - 1);
                    addPoint(x, y, 0); addPoint(x + w, y, 1); addPoint(x + w, y + h, 1); addPoint(x, y + h, 1);
                    addPointClose(x, y, 1);
                }
                pathBytes += raw; pathBytes += ' ';
                return true;
            }
            if (op == "W" || op == "W*") { clipOp = std::string(op); return true; }
            if (op == "n") { paintPath(op, false, false); return true; }
            if (op == "f" || op == "F" || op == "f*") { paintPath(op, true, false); return true; }
            if (op == "S") { paintPath(op, false, true); return true; }
            if (op == "s") { closePath(); paintPath(op, false, true); return true; }
            if (op == "B" || op == "B*") { paintPath(op, true, true); return true; }
            if (op == "b") { closePath(); paintPath(op, true, true); return true; }
            if (op == "b*") { addPointClose(startX, startY, 1); paintPath(op, true, true); return true; }
            // ---------- tekst
            if (op == "BT") { inText = true; tm = M{}; lineX = lineY = 0; atLineStart = true; return true; }
            if (op == "ET") { inText = false; return true; }
            if (op == "Tc") { setParam(kTc, raw); return true; }
            if (op == "Tw") { setParam(kTw, raw); return true; }
            if (op == "Tz") { setParam(kTz, raw); cur.th = num(0) / 100; return true; }
            if (op == "TL") { setParam(kTL, raw); leading = num(0); return true; }
            if (op == "Tr") { setParam(kTr, raw); cur.tr = int(num(0)); return true; }
            if (op == "Ts") { setParam(kTs, raw); cur.rise = num(0); return true; }
            if (op == "Tf") {
                setParam(kTf, raw);
                if (k >= 2 && o[k - 2].k == PObj::Name && resource("Font", o[k - 2].s)) cur.fontOk = true;
                return true;
            }
            if (op == "Tm") {
                if (k >= 6) tm = {num(k - 6), num(k - 5), num(k - 4), num(k - 3), num(k - 2), num(k - 1)};
                lineX = lineY = 0;
                atLineStart = true;
                return true;
            }
            // Pozycja początku wiersza (jak w PDFium: przesunięcia w przestrzeni Tm). Pozycję po wcześniejszych
            // napisach w wierszu podaje PDFium.
            if (op == "Td" || op == "TD") {
                if (k >= 2) { lineX += num(k - 2); lineY += num(k - 1); if (op == "TD") leading = -num(k - 1); }
                atLineStart = true;
                return true;
            }
            if (op == "T*") { lineY -= leading; atLineStart = true; return true; }
            if (op == "Tj" || op == "'" || op == "\"" || op == "TJ") {
                std::string show;
                bool object = false;
                if (op == "TJ") {
                    if (k < 1 || o[0].k != PObj::Arr) return true;
                    for (const PObj& el : o[0].a) if (el.k == PObj::Str) object = true;
                    show = raw;
                } else {
                    const PObj& s = o.empty() ? PObj{} : o.back();
                    if (op == "\"" && k >= 3) {
                        setParam(kTw, serStr(o[0]) + " Tw");
                        setParam(kTc, serStr(o[1]) + " Tc");
                    }
                    object = s.k == PObj::Str && !emptyString(s);
                    show = (s.k == PObj::Str ? s.s : std::string("()")) + " Tj";
                    if (op != "Tj") { lineY -= leading; atLineStart = true; }   // ' i " przechodzą do nowego wiersza
                }
                const bool lineStart = atLineStart;
                if (op == "TJ" || (o.size() && o.back().k == PObj::Str)) atLineStart = false;
                if (op == "TJ" && !object) {
                    // same liczby w TJ przesuwają pozycję (PDFium) — dalej znamy ją tylko z PDFium
                    for (const PObj& el : o[0].a) if (el.k == PObj::Num && el.num != 0) atLineStart = false;
                }
                if (!object || !cur.fontOk) return true;
                if (cur.tr >= 4) return fail("tekst jako obcięcie (Tr 4–7)");
                CItem& it = newItem(FPDF_PAGEOBJ_TEXT);
                it.tm = tm;
                it.rise = cur.rise;
                it.th = cur.th;
                it.show = std::move(show);
                it.lineX = lineX;
                it.lineY = lineY;
                it.lineStart = lineStart;
                return true;
            }
            // ---------- obiekty
            if (op == "Do") {
                if (k < 1 || o[0].k != PObj::Name) return true;
                const PObj* x = file.resolve(resource("XObject", o[0].s));
                if (!x) return true;
                const PObj* st = x->k == PObj::Dict ? x->get("Subtype") : nullptr;
                if (!st) return true;
                if (st->isName("Form") || st->isName("Image")) {
                    CItem& it = newItem(st->isName("Form") ? FPDF_PAGEOBJ_FORM : FPDF_PAGEOBJ_IMAGE);
                    it.raw = raw;
                    it.xname = o[0].s;
                }
                return true;
            }
            if (op == "sh") {
                if (k < 1 || o[0].k != PObj::Name || !resource("Shading", o[0].s)) return true;
                newItem(FPDF_PAGEOBJ_SHADING).raw = raw;
                return true;
            }
            if (op == "BI") { newItem(FPDF_PAGEOBJ_IMAGE).raw = raw; return true; }
            // ---------- znaczniki treści
            if (op == "BMC" || op == "BDC") { marks.push_back(raw); marksDirty = true; return true; }
            if (op == "EMC") { if (!marks.empty()) { marks.pop_back(); marksDirty = true; } return true; }
            return true;   // pozostałe (BX, EX, MP, DP, d0, d1, nieznane) — bez wpływu
        });
    }
};

// Nazwy zasobów użyte w treści: kategoria -> nazwy.
using Names = std::map<std::string, std::set<std::string>>;

void collectNames(const std::string& content, Names& out) {
    const auto* p = reinterpret_cast<const uint8_t*>(content.data());
    walkOps(p, content.size(), [&](std::string_view op, std::vector<PObj>& o, size_t, size_t) {
        auto name = [&](size_t i) -> const std::string* { return i < o.size() && o[i].k == PObj::Name ? &o[i].s : nullptr; };
        if (op == "gs") { if (auto n = name(0)) out["ExtGState"].insert(*n); }
        else if (op == "Tf") { if (o.size() >= 2) if (auto n = name(o.size() - 2)) out["Font"].insert(*n); }
        else if (op == "Do") { if (auto n = name(0)) out["XObject"].insert(*n); }
        else if (op == "sh") { if (auto n = name(0)) out["Shading"].insert(*n); }
        else if (op == "cs" || op == "CS") { if (auto n = name(0)) out["ColorSpace"].insert(*n); }
        else if (op == "scn" || op == "SCN") { if (!o.empty()) if (auto n = name(o.size() - 1)) out["Pattern"].insert(*n); }
        else if (op == "BDC") { if (o.size() >= 2) if (auto n = name(1)) out["Properties"].insert(*n); }
        else if (op == "BI") {
            if (!o.empty() && o[0].k == PObj::Dict)
                for (const char* key : {"CS", "ColorSpace"})
                    if (const PObj* cs = o[0].get(key)) if (cs->k == PObj::Name) out["ColorSpace"].insert(cs->s);
        }
        return true;
    });
}

// Wycina z zapisu PDFium fragmenty oznaczone znacznikami "/KPDFs… BMC … EMC".
std::map<std::string, std::string> extractChunks(const std::vector<uint8_t>& g) {
    std::map<std::string, std::string> out;
    struct Open { std::string name; size_t start; int depth; };
    std::vector<Open> open;
    int depth = 0;
    walkOps(g.data(), g.size(), [&](std::string_view op, std::vector<PObj>& o, size_t b, size_t e) {
        if (op == "BMC" || op == "BDC") {
            ++depth;
            if (op == "BMC" && o.size() == 1 && o[0].k == PObj::Name && o[0].s.rfind("KPDFs", 0) == 0)
                open.push_back({o[0].s, e, depth});
        } else if (op == "EMC") {
            if (!open.empty() && open.back().depth == depth) {
                out[open.back().name] = std::string(reinterpret_cast<const char*>(g.data() + open.back().start), b - open.back().start);
                open.pop_back();
            }
            if (depth > 0) --depth;
        }
        return true;
    });
    return out;
}

// Domyka q/BT w fragmencie (PDFium potrafi zostawić niezamknięte przy czcionkach Type3).
std::string balanced(const std::string& chunk) {
    int q = 0, bt = 0, deficit = 0;
    walkOps(reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size(), [&](std::string_view op, std::vector<PObj>&, size_t, size_t) {
        if (op == "q") ++q;
        else if (op == "Q") { if (q > 0) --q; else ++deficit; }
        else if (op == "BT") ++bt;
        else if (op == "ET" && bt > 0) --bt;
        return true;
    });
    std::string s;
    for (int i = 0; i < deficit; ++i) s += "q\n";
    s += chunk;
    s += '\n';
    for (int i = 0; i < bt; ++i) s += "ET\n";
    for (int i = 0; i < q; ++i) s += "Q\n";
    return s;
}

// Wszystkie odwołania w obiekcie (rekurencyjnie, bez rozwiązywania).
void refsIn(const PObj& o, std::vector<int>& out) {
    if (o.k == PObj::Ref) out.push_back(o.ref);
    for (const PObj& e : o.a) refsIn(e, out);
    for (const auto& kv : o.d) refsIn(kv.second, out);
}


// Nazwy zasobów osadzonej grafiki -> unikalne nazwy strony (prefiks). `used`: kategoria -> nowa nazwa ->
// (słownik zasobów grafiki, dawna nazwa).
using Renamed = std::map<std::string, std::map<std::string, std::pair<const PObj*, std::string>>>;

std::string renameRes(const std::string& text, const std::string& prefix, PdfFile& file, const PObj* res, Renamed& used) {
    if (prefix.empty() || !res) return text;
    const auto* p = reinterpret_cast<const uint8_t*>(text.data());
    const size_t n = text.size();
    std::string out;
    size_t copied = 0;
    auto has = [&](const char* cat, const std::string& name) {
        const PObj* c = file.resolve(res->get(cat));
        return c && c->k == PObj::Dict && c->get(name);
    };
    auto replaceTok = [&](const Tok& t, const char* cat, const std::string& name) {
        if (!has(cat, name) || t.b < copied) return;
        out.append(text, copied, t.b - copied);
        const std::string nn = prefix + name;
        out += encodeName(nn);
        copied = t.e;
        used[cat][nn] = {res, name};
    };
    Lex lx(p, n);
    struct Op { Tok t; PObj o; };
    std::vector<Op> ops;
    for (;;) {
        const Tok t = lx.next();
        if (t.t == Tk::End) break;
        if (t.t == Tk::ArrC || t.t == Tk::DictC) continue;
        const std::string_view w = lx.text(t);
        const bool operand = t.t != Tk::Word || w == "true" || w == "false" || w == "null";
        if (operand) {
            Op op;
            op.t = t;
            if (!parseFrom(lx, t, op.o, false, 0)) break;
            ops.push_back(std::move(op));
            continue;
        }
        if (w == "BI") {
            for (;;) {
                const Tok k = lx.next();
                if (k.t == Tk::End || (k.t == Tk::Word && lx.text(k) == "ID")) break;
                if (k.t != Tk::Name) continue;
                const std::string key = decodeName(lx.text(k));
                const Tok v = lx.next();
                if ((key == "CS" || key == "ColorSpace") && v.t == Tk::Name) replaceTok(v, "ColorSpace", decodeName(lx.text(v)));
                else if (v.t == Tk::ArrO || v.t == Tk::DictO) { PObj tmp; parseFrom(lx, v, tmp, false, 0); }
            }
            size_t i = lx.pos + 1;
            for (; i + 1 < n; ++i)
                if (p[i] == 'E' && p[i + 1] == 'I' && isWs(p[i - 1]) && (i + 2 >= n || isWs(p[i + 2]) || isDelim(p[i + 2]))) break;
            lx.pos = std::min(n, i + 2);
            ops.clear();
            continue;
        }
        auto nameAt = [&](size_t i) -> const Op* { return i < ops.size() && ops[i].o.k == PObj::Name ? &ops[i] : nullptr; };
        if (w == "gs") { if (auto o = nameAt(0)) replaceTok(o->t, "ExtGState", o->o.s); }
        else if (w == "Tf") { if (ops.size() >= 2) if (auto o = nameAt(ops.size() - 2)) replaceTok(o->t, "Font", o->o.s); }
        else if (w == "Do") { if (auto o = nameAt(0)) replaceTok(o->t, "XObject", o->o.s); }
        else if (w == "sh") { if (auto o = nameAt(0)) replaceTok(o->t, "Shading", o->o.s); }
        else if (w == "cs" || w == "CS") { if (auto o = nameAt(0)) replaceTok(o->t, "ColorSpace", o->o.s); }
        else if (w == "scn" || w == "SCN") { if (!ops.empty()) if (auto o = nameAt(ops.size() - 1)) replaceTok(o->t, "Pattern", o->o.s); }
        else if (w == "BDC") { if (auto o = nameAt(1)) replaceTok(o->t, "Properties", o->o.s); }
        ops.clear();
    }
    out.append(text, copied, std::string::npos);
    return out;
}

// Pozycja napisu (przestrzeń Tm) z macierzy obiektu PDFium; false — macierz się nie zgadza.
bool textPosition(CItem& it, const FS_MATRIX& m) {
    const M tmc = mul(it.tm, it.ctm);
    const M lin = mul(M{it.th, 0, 0, 1, 0, 0}, tmc);
    if (!nearM(lin, m, 2e-3, false)) return false;
    M ti;
    if (!inv(tmc, ti)) return false;
    it.tx = m.e * ti.a + m.f * ti.c + ti.e;
    it.ty = m.e * ti.b + m.f * ti.d + ti.f - it.rise;
    // na początku wiersza pozycja jest znana dokładnie (PDFium liczy na float) — jeśli się zgadza, bierzemy ją
    if (it.lineStart && approxEq(it.tx, it.lineX, 1e-3) && approxEq(it.ty, it.lineY, 1e-3)) { it.tx = it.lineX; it.ty = it.lineY; }
    return true;
}

// Zgodność elementów treści z obiektami PDFium (rodzaj, położenie; dla tekstu wylicza pozycję).
struct PdfiumObj { int type = 0; bool hasM = false; FS_MATRIX m{}; };

bool matchObjects(std::vector<CItem>& items, const std::vector<PdfiumObj>& objs, std::string& why) {
    if (items.size() != objs.size()) {
        why = "liczba obiektów: " + std::to_string(items.size()) + " zamiast " + std::to_string(objs.size());
        return false;
    }
    for (size_t k = 0; k < objs.size(); ++k) {
        CItem& it = items[k];
        const PdfiumObj& o = objs[k];
        if (o.type != it.type) { why = "rodzaj obiektu " + std::to_string(k); return false; }
        if (o.type == FPDF_PAGEOBJ_PATH || o.type == FPDF_PAGEOBJ_IMAGE) {
            if (!o.hasM || !nearM(it.ctm, o.m, 1e-3, true)) { why = "położenie obiektu " + std::to_string(k); return false; }
        } else if (o.type == FPDF_PAGEOBJ_TEXT) {
            if (!o.hasM || !textPosition(it, o.m)) { why = "macierz tekstu " + std::to_string(k); return false; }
        }
    }
    return true;
}

} // namespace

// ============================================================================ Splicer

struct Splicer::Impl {
    PdfFile orig;
    std::vector<int> origPages;
    // osadzona grafika (egzemplarz na stronie) — klucz: ścieżka [indeks na stronie, indeks w grafice, …]
    struct FormInfo {
        bool tried = false, ok = false;
        std::vector<CItem> items;
        const PObj* res = nullptr;         // zasoby, w których szukać nazw z treści grafiki
        std::string prefix;                // prefiks nazw zasobów na stronie ("" = zasoby strony)
        const PObj* renameRes = nullptr;   // słownik zasobów, z którego pochodzą nazwy z prefiksem
        M matrix;
        std::vector<std::pair<std::string, PObj>> extra;   // /Group, /OC grafiki (przenoszone do otoczki)
        bool hasBBox = false;
        double bbox[4] = {0, 0, 0, 0};
    };
    struct PageData {
        std::vector<CItem> items;
        bool ready = false;
        const PObj* res = nullptr;
        std::unordered_map<FPDF_PAGEOBJECT, std::vector<int>> childPaths;     // obiekt z grafiki -> ścieżka
        std::map<std::vector<int>, std::vector<PdfiumObj>> formChildren;      // grafika -> jej obiekty (PDFium)
        std::map<std::vector<int>, FormInfo> forms;
    };
    std::map<int, PageData> pages;
    std::vector<SplicePlan> plans;

    bool ensureForm(PageData& pd, const std::vector<int>& fpath);
};

bool Splicer::Impl::ensureForm(PageData& pd, const std::vector<int>& fpath) {
    FormInfo& F = pd.forms[fpath];
    if (F.tried) return F.ok;
    F.tried = true;
    const CItem* doItem = nullptr;
    const PObj* parentRes = nullptr;
    std::string parentPrefix;
    const PObj* parentRename = nullptr;
    if (fpath.size() == 1) {
        if (fpath[0] < 0 || fpath[0] >= int(pd.items.size())) return false;
        doItem = &pd.items[size_t(fpath[0])];
        parentRes = pd.res;
    } else {
        const std::vector<int> parent(fpath.begin(), fpath.end() - 1);
        if (!ensureForm(pd, parent)) return false;
        const FormInfo& P = pd.forms[parent];
        if (fpath.back() < 0 || fpath.back() >= int(P.items.size())) return false;
        doItem = &P.items[size_t(fpath.back())];
        parentRes = P.res;
        parentPrefix = P.prefix;
        parentRename = P.renameRes;
    }
    if (doItem->type != FPDF_PAGEOBJ_FORM || doItem->xname.empty() || !parentRes) return false;
    const PObj* xd = orig.resolve(parentRes->get("XObject"));
    const PObj* ref = xd && xd->k == PObj::Dict ? xd->get(doItem->xname) : nullptr;
    if (!ref || ref->k != PObj::Ref) return false;
    const PObj* fd = orig.obj(ref->ref);
    if (!fd || fd->k != PObj::Dict) return false;
    const PObj* r = orig.resolve(fd->get("Resources"));
    if (r && r->k == PObj::Dict) {
        F.res = r;
        F.renameRes = r;
        F.prefix = "kF" + std::to_string(ref->ref) + "_";
    } else {   // bez własnych zasobów — jak grafika nadrzędna (albo strona)
        F.res = parentRes;
        F.renameRes = parentRename;
        F.prefix = parentPrefix;
    }
    for (const char* key : {"Group", "OC"})
        if (const PObj* v = fd->get(key)) F.extra.emplace_back(key, *v);
    if (const PObj* mx = orig.resolve(fd->get("Matrix")); mx && mx->k == PObj::Arr && mx->a.size() == 6) {
        double v[6];
        for (int i = 0; i < 6; ++i) { const PObj* e = orig.resolve(&mx->a[size_t(i)]); v[i] = e && e->k == PObj::Num ? e->num : 0; }
        F.matrix = {v[0], v[1], v[2], v[3], v[4], v[5]};
    }
    if (const PObj* bb = orig.resolve(fd->get("BBox")); bb && bb->k == PObj::Arr && bb->a.size() == 4) {
        double v[4];
        for (int i = 0; i < 4; ++i) { const PObj* e = orig.resolve(&bb->a[size_t(i)]); v[i] = e && e->k == PObj::Num ? e->num : 0; }
        F.bbox[0] = std::min(v[0], v[2]); F.bbox[1] = std::min(v[1], v[3]);
        F.bbox[2] = std::max(v[0], v[2]); F.bbox[3] = std::max(v[1], v[3]);
        F.hasBBox = true;
    }
    std::vector<uint8_t> content;
    if (!orig.streamData(ref->ref, content)) return false;
    Interp in(orig, F.res);
    if (!in.run(content.data(), content.size())) return false;
    auto kids = pd.formChildren.find(fpath);
    if (kids == pd.formChildren.end()) return false;
    std::string why;
    if (!matchObjects(in.items, kids->second, why)) return false;
    F.items = std::move(in.items);
    F.ok = true;
    return true;
}

Splicer::Splicer() : d_(std::make_unique<Impl>()) {}
Splicer::~Splicer() = default;

bool Splicer::load(std::vector<uint8_t> origBytes) {
    d_->orig.buf = std::move(origBytes);
    if (!d_->orig.load()) return false;
    d_->origPages = d_->orig.pages();
    return !d_->origPages.empty();
}

bool Splicer::prepare(int page, FPDF_PAGE pg, std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    if (page < 0 || page >= int(d_->origPages.size())) return fail("brak strony w kopii pliku");
    const int pnum = d_->origPages[size_t(page)];
    std::vector<uint8_t> content;
    if (!d_->orig.pageContent(pnum, content)) return fail("nieobsługiwany filtr strumienia treści");
    const PObj* res = d_->orig.pageResources(pnum);
    Interp in(d_->orig, res);
    if (!in.run(content.data(), content.size())) return fail(in.why.empty() ? "błąd składni treści" : in.why);
    auto describe = [](FPDF_PAGEOBJECT o) {
        PdfiumObj d;
        d.type = FPDFPageObj_GetType(o);
        d.hasM = FPDFPageObj_GetMatrix(o, &d.m);
        return d;
    };
    const int n = FPDFPage_CountObjects(pg);
    std::vector<PdfiumObj> objs;
    for (int k = 0; k < n; ++k) objs.push_back(describe(FPDFPage_GetObject(pg, k)));
    std::string w;
    if (!matchObjects(in.items, objs, w)) return fail(w);
    Impl::PageData& pd = d_->pages[page];
    pd.items = std::move(in.items);
    pd.res = res;
    pd.ready = true;
    // obiekty osadzonych grafik (przed zmianami — po rozłożeniu grafiki trafiają na stronę jako te same obiekty)
    size_t total = 0;
    std::vector<int> path;
    std::function<void(FPDF_PAGEOBJECT, int)> capture = [&](FPDF_PAGEOBJECT form, int depth) {
        if (depth > 8 || total > 200000) return;
        std::vector<PdfiumObj>& kids = pd.formChildren[path];
        const int m = FPDFFormObj_CountObjects(form);
        for (int i = 0; i < m; ++i, ++total) {
            FPDF_PAGEOBJECT c = FPDFFormObj_GetObject(form, (unsigned long)i);
            kids.push_back(describe(c));
            path.push_back(i);
            pd.childPaths[c] = path;
            if (kids.back().type == FPDF_PAGEOBJ_FORM) capture(c, depth + 1);
            path.pop_back();
        }
    };
    for (int k = 0; k < n; ++k)
        if (objs[size_t(k)].type == FPDF_PAGEOBJ_FORM) {
            path = {k};
            capture(FPDFPage_GetObject(pg, k), 0);
        }
    return true;
}

bool Splicer::childPath(int page, FPDF_PAGEOBJECT obj, std::vector<int>& out) {
    auto pit = d_->pages.find(page);
    if (pit == d_->pages.end() || !pit->second.ready) return false;
    Impl::PageData& pd = pit->second;
    auto c = pd.childPaths.find(obj);
    if (c == pd.childPaths.end()) return false;
    const std::vector<int>& path = c->second;
    for (size_t L = 1; L < path.size(); ++L)
        if (!d_->ensureForm(pd, std::vector<int>(path.begin(), path.begin() + long(L)))) return false;
    out = path;
    return true;
}

void Splicer::setPlan(SplicePlan plan) { d_->plans.push_back(std::move(plan)); }

namespace {

using Renamer = std::function<std::string(const std::string&)>;

void emitState(const CItem& it, const Renamer& ren, std::string& out, size_t& marks) {
    for (const std::string& l : it.state->lines) { out += ren ? ren(l) : l; out += '\n'; }
    for (const std::string& m : *it.marks) { out += ren ? ren(m) : m; out += '\n'; ++marks; }
}

void emitPaint(const CItem& it, const SpliceItem& si, const Renamer& ren, std::string& out) {
    switch (it.type) {
    case FPDF_PAGEOBJ_TEXT:
        out += "BT ";
        fmtM(out, it.tm);
        out += " Tm ";
        fmtNum(out, it.tx);
        out += ' ';
        fmtNum(out, it.ty);
        out += " Td\n";
        out += it.show;
        out += "\nET\n";
        break;
    case FPDF_PAGEOBJ_PATH: {
        if (si.ovWidth) { fmtNum(out, si.width); out += " w\n"; }
        auto rgb = [&](const uint8_t* c, const char* op) {
            for (int i = 0; i < 3; ++i) { fmtNum(out, c[i] / 255.0); out += ' '; }
            out += op;
            out += '\n';
        };
        if (si.ovFill) rgb(si.fill, "rg");
        if (si.ovStroke) rgb(si.stroke, "RG");
        out += it.pathBytes;
        if (!it.clipOp.empty()) { out += it.clipOp; out += ' '; }
        std::string paint = it.paintOp;
        if (si.ovMode) {
            const bool closing = paint == "b" || paint == "b*" || paint == "s";
            if (si.fillMode == FPDF_FILLMODE_NONE) paint = si.strokeOn ? "S" : "n";
            else if (si.fillMode == FPDF_FILLMODE_ALTERNATE) paint = si.strokeOn ? "B*" : "f*";
            else paint = si.strokeOn ? "B" : "f";
            if (closing) out += "h ";
        }
        out += paint;
        out += '\n';
        break;
    }
    default:
        out += ren ? ren(it.raw) : it.raw;
        out += '\n';
    }
}

void beginReplay(const SpliceItem& si, std::string& out) {
    out += "q\n";
    if (si.moved) {
        fmtM(out, M{si.move.a, si.move.b, si.move.c, si.move.d, si.move.e, si.move.f});
        out += " cm\n";
    }
}

void endReplay(size_t marks, std::string& out) {
    for (size_t i = 0; i < marks; ++i) out += "EMC\n";
    out += "Q\n";
}

void replay(const CItem& it, const SpliceItem& si, std::string& out) {
    size_t marks = 0;
    beginReplay(si, out);
    emitState(it, nullptr, out, marks);
    emitPaint(it, si, nullptr, out);
    endReplay(marks, out);
}

bool sameObject(PdfFile& a, PdfFile& b, int num) {
    const std::string_view x = a.rawHeader(num), y = b.rawHeader(num);
    return !x.empty() && x == y;
}

} // namespace

int Splicer::apply(std::vector<uint8_t>& saved, std::string* diag) {
    if (d_->plans.empty()) return 0;
    PdfFile out;
    out.buf = saved;   // kopia do czytania; dopisujemy do `saved`
    if (!out.load()) { if (diag) *diag += "zapis PDFium: nieobsługiwana tabela xref\n"; return 0; }
    const std::vector<int> outPages = out.pages();
    const PObj* sizeObj = out.trailer.get("Size");
    int nextNum = sizeObj && sizeObj->k == PObj::Num ? int(sizeObj->num) : 0;
    for (auto& kv : out.xref) nextNum = std::max(nextNum, kv.first + 1);

    std::string update;
    std::vector<std::pair<int, size_t>> written;   // numer obiektu -> pozycja w aktualizacji
    int done = 0;
    if (saved.empty() || saved.back() != '\n') update += '\n';
    const size_t base = saved.size();

    for (const SplicePlan& plan : d_->plans) {
        auto fail = [&](const std::string& w) { if (diag) *diag += "strona " + std::to_string(plan.page + 1) + ": " + w + "\n"; };
        auto pit = d_->pages.find(plan.page);
        if (pit == d_->pages.end() || !pit->second.ready) continue;
        if (plan.page >= int(outPages.size()) || plan.page >= int(d_->origPages.size())) { fail("brak strony"); continue; }
        Impl::PageData& pd = pit->second;
        const std::vector<CItem>& items = pd.items;
        const int outNum = outPages[size_t(plan.page)], origNum = d_->origPages[size_t(plan.page)];
        Renamed renamed;
        std::vector<std::pair<int, std::string>> objs;   // nowe obiekty strony: numer -> tekst (zapisywane po powodzeniu)
        std::vector<int> needRefs;
        std::map<std::string, int> wrappers;               // otoczki grafik: nazwa zasobu -> numer obiektu
        int resNum = 0;                                    // zasoby strony jako obiekt (wspólne dla strony i otoczek)
        auto streamText = [](const std::string& dict, const std::string& data) {
            int zlen = 0;
            unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())), int(data.size()), &zlen, 8);
            std::string t = dict.substr(0, dict.size() - 2) + "/Length " + std::to_string(z ? zlen : int(data.size())) +
                            (z ? " /Filter /FlateDecode" : "") + ">>\nstream\n";
            if (z) { t.append(reinterpret_cast<const char*>(z), size_t(zlen)); std::free(z); }
            else t += data;
            t += "\nendstream";
            return t;
        };
        std::string replayed, fromPdfium, content;
        std::vector<uint8_t> g;
        if (!out.pageContent(outNum, g)) { fail("treść PDFium"); continue; }
        const auto chunks = extractChunks(g);
        // Zapis PDFium nowego obrazu w kontekście zastąpionego: jego stan graficzny (obcięcie, maski,
        // przezroczystość), a przekształcenie cofnięte do przestrzeni strony (w niej zapisał go PDFium).
        auto chunkInContext = [&](const SpliceItem& si, const CItem& it, const Renamer& ren, const M& ctmOnPage, std::string& part) {
            auto c = chunks.find(si.mark);
            if (c == chunks.end()) return true;   // obiekt, którego PDFium nie zapisał
            M ci;
            if (!inv(ctmOnPage, ci)) return false;
            size_t marks = 0;
            part += "q\n";
            emitState(it, ren, part, marks);
            fmtM(part, ci);
            part += " cm\n";
            const std::string ch = balanced(c->second);
            part += ch;
            fromPdfium += ch;
            endReplay(marks, part);
            return true;
        };

        // Otoczka rozłożonej osadzonej grafiki F (ścieżka fp): nowa grafika z tą samą grupą przezroczystości,
        // obcięciem i macierzą, z obiektami plan.items[a..b) (bez usuniętych). Zachowuje składanie masek
        // przezroczystości i grup tak jak oryginał. `cAtDo` — CTM w chwili wywołania grafiki (przestrzeń strony).
        std::function<bool(size_t, size_t, const std::vector<int>&, const M&, std::string&)> wrapForm =
            [&](size_t a, size_t b, const std::vector<int>& fp, const M& cAtDo, std::string& name) -> bool {
            auto f = pd.forms.find(fp);
            if (f == pd.forms.end() || !f->second.ok) return false;
            const Impl::FormInfo& F = f->second;
            const M cF = mul(F.matrix, cAtDo);   // CTM na początku treści grafiki
            Renamer ren;
            if (!F.prefix.empty() && F.renameRes) {
                const std::string prefix = F.prefix;
                const PObj* rr = F.renameRes;
                ren = [this, prefix, rr, &renamed](const std::string& t) { return renameRes(t, prefix, d_->orig, rr, renamed); };
            }
            const size_t L = fp.size();
            std::string body;
            for (size_t k = a; k < b;) {
                const SpliceItem& si = plan.items[k];
                const std::vector<int>& path = si.path;
                if (path.size() < L + 1 || path[L] < 0 || path[L] >= int(F.items.size())) return false;
                const CItem& it = F.items[size_t(path[L])];
                size_t marks = 0;
                if (path.size() == L + 1 && !si.replay) {   // nowy obraz w miejscu obiektu tej grafiki
                    if (!chunkInContext(si, it, ren, mul(it.ctm, cF), body)) return false;
                    ++k;
                    continue;
                }
                if (path.size() == L + 1) {   // obiekt tej grafiki
                    body += "q\n";
                    if (si.moved) {   // przesunięcie w przestrzeni strony -> w przestrzeni grafiki
                        M ci;
                        if (!inv(cF, ci)) return false;
                        fmtM(body, mul(mul(cF, M{si.move.a, si.move.b, si.move.c, si.move.d, si.move.e, si.move.f}), ci));
                        body += " cm\n";
                    }
                    emitState(it, ren, body, marks);
                    emitPaint(it, si, ren, body);
                    endReplay(marks, body);
                    ++k;
                    continue;
                }
                // grafika zagnieżdżona: kolejne obiekty z tej samej grafiki
                size_t m = k + 1;
                while (m < b && plan.items[m].path.size() > L + 1 && plan.items[m].path[L] == path[L]) ++m;
                std::vector<int> sub(fp);
                sub.push_back(path[L]);
                std::string inner;
                if (!wrapForm(k, m, sub, mul(it.ctm, cF), inner)) return false;
                body += "q\n";
                emitState(it, ren, body, marks);
                body += encodeName(inner) + " Do\n";
                endReplay(marks, body);
                k = m;
            }
            if (!resNum) resNum = nextNum++;
            const int num = nextNum++;
            PObj dict;
            dict.k = PObj::Dict;
            PObj v;
            v.k = PObj::Name; v.s = "XObject"; dict.set("Type", v);
            v.s = "Form"; dict.set("Subtype", v);
            auto nums = [](std::initializer_list<double> xs) {
                PObj arr;
                arr.k = PObj::Arr;
                for (double x : xs) { PObj e; e.k = PObj::Num; e.num = x; fmtNum(e.s, x); arr.a.push_back(e); }
                return arr;
            };
            if (F.hasBBox) dict.set("BBox", nums({F.bbox[0], F.bbox[1], F.bbox[2], F.bbox[3]}));
            else dict.set("BBox", nums({-1e5, -1e5, 1e5, 1e5}));
            dict.set("Matrix", nums({F.matrix.a, F.matrix.b, F.matrix.c, F.matrix.d, F.matrix.e, F.matrix.f}));
            for (const auto& [key, val] : F.extra) { refsIn(val, needRefs); dict.set(key, val); }
            dict.set("Resources", mkRef(resNum));
            name = "kG" + std::to_string(num);
            objs.emplace_back(num, streamText(serStr(dict), body));
            wrappers[name] = num;
            replayed += body;   // nazwy zasobów strony użyte w grafice (grafiki bez własnych zasobów)
            return true;
        };

        // 1) treść
        bool ok = true;
        for (size_t k = 0; k < plan.items.size();) {
            const SpliceItem& si = plan.items[k];
            auto inForm = [](const SpliceItem& x) { return (x.replay || x.ctx) && x.path.size() >= 2; };
            if (inForm(si)) {
                const int root = si.path[0];
                if (root < 0 || root >= int(items.size())) { ok = false; break; }
                size_t m = k + 1;
                while (m < plan.items.size() && inForm(plan.items[m]) && plan.items[m].path[0] == root) ++m;
                std::string name;
                if (!wrapForm(k, m, {root}, items[size_t(root)].ctm, name)) { ok = false; break; }
                std::string part = "q\n";
                size_t marks = 0;
                emitState(items[size_t(root)], nullptr, part, marks);
                part += encodeName(name) + " Do\n";
                endReplay(marks, part);
                replayed += part;
                content += part;
                k = m;
                continue;
            }
            if (si.replay) {
                if (si.orig < 0 || si.orig >= int(items.size())) { ok = false; break; }
                std::string part;
                replay(items[size_t(si.orig)], si, part);
                replayed += part;
                content += part;
            } else if (si.ctx && si.path.empty() && si.orig >= 0 && si.orig < int(items.size())) {
                std::string part;
                const CItem& it = items[size_t(si.orig)];
                if (!chunkInContext(si, it, nullptr, it.ctm, part)) { ok = false; break; }
                replayed += part;   // stan oryginału — nazwy z zasobów oryginału (fragment PDFium jest też w fromPdfium)
                content += part;
            } else {
                auto c = chunks.find(si.mark);
                if (c != chunks.end()) {   // (brak — obiekt, którego PDFium nie zapisał)
                    std::string part = "q\n";
                    const std::vector<std::string>* mk = si.orig >= 0 && si.orig < int(items.size()) ? items[size_t(si.orig)].marks.get() : nullptr;
                    if (mk) for (const std::string& mm : *mk) { part += mm; part += '\n'; }
                    part += balanced(c->second);
                    if (mk) for (size_t i = 0; i < mk->size(); ++i) part += "EMC\n";
                    part += "Q\n";
                    fromPdfium += part;
                    content += part;
                }
            }
            ++k;
        }
        if (!ok) { fail("plan"); continue; }

        // 2) zasoby: nazwy z odtworzonej treści — z oryginału, nazwy z fragmentów PDFium — z zapisu PDFium
        Names usedOrig, usedNew;
        collectNames(replayed, usedOrig);
        collectNames(fromPdfium, usedNew);
        // znaczniki oryginału przy fragmentach PDFium też odwołują się do zasobów oryginału
        for (const SpliceItem& si : plan.items)
            if (!si.replay && si.orig >= 0 && si.orig < int(items.size()))
                for (const std::string& m : *items[size_t(si.orig)].marks) collectNames(m, usedOrig);
        const PObj* origRes = d_->orig.pageResources(origNum);
        const PObj* outRes = out.pageResources(outNum);
        PObj res;
        res.k = PObj::Dict;
        bool conflict = false;
        auto category = [&](PdfFile& f, const PObj* r, const std::string& cat) -> const PObj* {
            const PObj* c = r ? f.resolve(r->get(cat)) : nullptr;
            return c && c->k == PObj::Dict ? c : nullptr;
        };
        std::set<std::string> cats;
        for (auto& kv : usedOrig) cats.insert(kv.first);
        for (auto& kv : usedNew) cats.insert(kv.first);
        for (auto& kv : renamed) cats.insert(kv.first);
        if (!wrappers.empty()) cats.insert("XObject");
        for (const std::string& cat : cats) {
            PObj dict;
            dict.k = PObj::Dict;
            const PObj* oc = category(d_->orig, origRes, cat);
            const PObj* nc = category(out, outRes, cat);
            for (const std::string& name : usedOrig[cat]) {
                const PObj* v = oc ? oc->get(name) : nullptr;
                if (!v) continue;   // np. przestrzeń urządzenia (DeviceRGB) albo brak w oryginale
                refsIn(*v, needRefs);
                dict.set(name, *v);
            }
            // zasoby osadzonych grafik pod nowymi nazwami
            for (auto& [newName, from] : renamed[cat]) {
                const PObj* fc = category(d_->orig, from.first, cat);
                const PObj* v = fc ? fc->get(from.second) : nullptr;
                if (!v) continue;
                if (dict.get(newName)) { conflict = true; continue; }
                refsIn(*v, needRefs);
                dict.set(newName, *v);
            }
            if (cat == "XObject")
                for (auto& [name, num] : wrappers) {
                    if (dict.get(name)) { conflict = true; continue; }
                    dict.set(name, mkRef(num));
                }
            for (const std::string& name : usedNew[cat]) {
                const PObj* v = nc ? nc->get(name) : nullptr;
                if (!v) continue;
                if (const PObj* have = dict.get(name)) {
                    if (serStr(*have) != serStr(*v)) conflict = true;
                    continue;
                }
                dict.set(name, *v);
            }
            if (!dict.d.empty()) res.set(cat, std::move(dict));
        }
        if (conflict) { fail("konflikt nazw zasobów"); continue; }
        if (outRes) if (const PObj* ps = outRes->get("ProcSet")) res.set("ProcSet", *ps);
        // obiekty oryginału muszą istnieć w zapisie PDFium bez zmian (te same numery)
        bool refsOk = true;
        for (int r : needRefs)
            if (!sameObject(d_->orig, out, r)) { refsOk = false; fail("zasób " + std::to_string(r) + " różni się w zapisie"); break; }
        if (!refsOk) continue;

        // 3) nowy strumień treści, otoczki grafik i nowa wersja słownika strony
        const PObj* pageDict = out.obj(outNum);
        if (!pageDict || pageDict->k != PObj::Dict) { fail("słownik strony"); continue; }
        const int contentNum = nextNum++;
        objs.emplace_back(contentNum, streamText("<<>>", content));
        PObj newPage = *pageDict;
        newPage.set("Contents", mkRef(contentNum));
        if (resNum) {
            objs.emplace_back(resNum, serStr(res));
            newPage.set("Resources", mkRef(resNum));
        } else newPage.set("Resources", std::move(res));
        objs.emplace_back(outNum, serStr(newPage));
        for (auto& [num, text] : objs) {
            const int gen = num == outNum ? std::max(0, out.xref[outNum].second) : 0;
            written.emplace_back(num, base + update.size());
            update += std::to_string(num) + ' ' + std::to_string(gen) + " obj\n" + text + "\nendobj\n";
        }
        ++done;
    }
    if (!done) return 0;

    // 4) tabela xref aktualizacji i trailer
    std::sort(written.begin(), written.end());
    const size_t xrefPos = base + update.size();
    update += "xref\n";
    for (auto& w : written) {
        char line[32];
        const int gen = out.xref.count(w.first) ? std::max(0, out.xref[w.first].second) : 0;
        std::string off = std::to_string(w.second);
        while (off.size() < 10) off.insert(off.begin(), '0');
        std::string gs = std::to_string(gen);
        while (gs.size() < 5) gs.insert(gs.begin(), '0');
        std::snprintf(line, sizeof line, "%d 1\n", w.first);
        update += line;
        update += off + ' ' + gs + " n\r\n";
    }
    PObj tr;
    tr.k = PObj::Dict;
    PObj sz;
    sz.k = PObj::Num;
    sz.s = std::to_string(nextNum);
    sz.num = nextNum;
    tr.set("Size", sz);
    for (const char* key : {"Root", "Info", "ID"})
        if (const PObj* v = out.trailer.get(key)) tr.set(key, *v);
    PObj prev;
    prev.k = PObj::Num;
    prev.s = std::to_string(out.startxref);
    tr.set("Prev", prev);
    update += "trailer\n";
    ser(tr, update);
    update += "\nstartxref\n" + std::to_string(xrefPos) + "\n%%EOF\n";
    saved.insert(saved.end(), update.begin(), update.end());
    return done;
}

} // namespace kpdf::detail
