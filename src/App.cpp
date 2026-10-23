#include "App.h"
#include "Icons.h"

#include "imgui.h"
#include "imgui_stdlib.h"
#include "nfd.h"

#define GL_SILENCE_DEPRECATION
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <sys/wait.h>
#  include <unistd.h>
#endif
#include <GLFW/glfw3.h>
#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#  define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace kpdf;

namespace {

// ---------- paleta interfejsu: szary blat, białe arkusze, czerwień korektorska ----------
const ImVec4 kDesk{0.865f, 0.886f, 0.902f, 1};
const ImVec4 kBar{0.965f, 0.969f, 0.973f, 1};
const ImVec4 kInk{0.106f, 0.129f, 0.153f, 1};
const ImVec4 kMute{0.353f, 0.400f, 0.439f, 1};
const ImVec4 kAccent{0.769f, 0.208f, 0.169f, 1};
const ImVec4 kSel{0.165f, 0.435f, 0.859f, 1};

const Color kPalette[] = {
    {0.067f, 0.067f, 0.067f, 1}, {0.122f, 0.310f, 0.820f, 1}, {0.769f, 0.208f, 0.169f, 1},
    {0.102f, 0.541f, 0.243f, 1}, {1.000f, 0.831f, 0.000f, 1}, {1.000f, 0.561f, 0.784f, 1},
};

ImU32 col32(const ImVec4& c, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * a)); }
ImU32 col32(const Color& c, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a * a)); }
ImVec4 toV4(const Color& c) { return ImVec4(c.r, c.g, c.b, c.a); }
bool sameColor(const Color& a, const Color& b) {
    return std::fabs(a.r - b.r) < 0.004f && std::fabs(a.g - b.g) < 0.004f && std::fabs(a.b - b.b) < 0.004f;
}
bool isPdf(const std::string& p) {
    std::string e = std::filesystem::u8path(p).extension().u8string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == ".pdf";
}
bool isImage(const std::string& p) {
    std::string e = std::filesystem::u8path(p).extension().u8string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".gif" || e == ".tga";
}
bool isShape(const Obj& o) { return o.type == ObjType::PageShape || o.type == ObjType::Shape; }
bool resizable(const Obj& o) {
    return o.type == ObjType::Image || o.type == ObjType::PageImage || o.type == ObjType::Highlight || o.type == ObjType::Whiteout || isShape(o);
}
bool isImage(const Obj& o) { return o.type == ObjType::Image || o.type == ObjType::PageImage; }
bool allCorners(const Obj& o) { return isImage(o) || isShape(o); }   // uchwyty we wszystkich narożnikach
// Element rysowany przez PDFium w renderze strony — przeciągany z podglądem wyciętym z renderu.
bool liveObj(const Obj& o) { return o.type == ObjType::PageImage || isShape(o); }
// Oś, w której kształt jest cienki jak linia: 1 — linia pozioma (cienka wysokość), 2 — pionowa, 0 — nie linia.
int thinAxis(const Obj& o) {
    if (o.type != ObjType::PageShape && o.type != ObjType::Shape) return 0;
    if (o.h <= 6.f && o.w >= 8.f * o.h && o.w > 0) return 1;
    if (o.w <= 6.f && o.h >= 8.f * o.w && o.h > 0) return 2;
    return 0;
}
bool removedFromFile(const Obj& o) { return (o.type == ObjType::PageImage || o.type == ObjType::PageShape) && o.hidden; }
float segDist(Vec2 p, Vec2 a, Vec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0;
    t = std::clamp(t, 0.f, 1.f);
    const float qx = a.x + t * dx - p.x, qy = a.y + t * dy - p.y;
    return std::sqrt(qx * qx + qy * qy);
}

void applyUiStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsLight(&s);
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.GrabRounding = 5;
    s.ScrollbarRounding = 6;
    s.FramePadding = ImVec2(9, 6);
    s.ItemSpacing = ImVec2(6, 6);
    s.WindowBorderSize = 0;
    s.ScrollbarSize = 13;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kInk;
    c[ImGuiCol_TextDisabled] = kMute;
    c[ImGuiCol_WindowBg] = kBar;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_Border] = ImVec4(0.78f, 0.80f, 0.83f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.91f, 0.93f, 0.94f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.87f, 0.89f, 0.91f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.84f, 0.86f, 0.88f, 1);
    c[ImGuiCol_Button] = ImVec4(0.91f, 0.93f, 0.94f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.85f, 0.87f, 0.89f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.83f, 0.86f, 1);
    c[ImGuiCol_Header] = ImVec4(0.88f, 0.90f, 0.92f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.85f, 0.87f, 0.89f, 1);
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_TitleBg] = kBar;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.90f, 0.92f, 0.93f, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.1f, 0.12f, 0.15f, 0.35f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.925f, 0.933f, 0.941f, 1);
    c[ImGuiCol_DragDropTarget] = kSel;
    c[ImGuiCol_NavCursor] = kSel;
}

// Enter zatwierdził poprzednie pole (dla pól liczbowych zamiast ImGuiInputTextFlags_EnterReturnsTrue,
// którego ImGui nie obsługuje w InputInt/InputFloat i zgłasza asercję w kompilacji Debug).
bool enterConfirmed() {
    return ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
}

const float kGutter = 28.f;  // odstęp wokół stron na blacie
const float kGap = 26.f;     // odstęp między stronami

} // namespace

// =====================================================================================

App::App(GLFWwindow* window, ImFont* font, ImFont* bold, ImFont* italic)
    : window_(window), font_(font), fontBoldUi_(bold), fontItalicUi_(italic) {
    applyUiStyle();
    setWindowIcon();
    NFD_Init();
    settings_.load();
    tabs_.push_back(std::make_unique<TabStore>());
    tabs_.back()->uid = nextTabUid_++;
    updateTitle();
}

static std::vector<uint8_t> resizeArea(const ImageData& im, int W, int H);

// Ikona okna i paska zadań (Windows, Linux X11). macOS bierze ikonę z pakietu aplikacji, Wayland — z pliku .desktop.
void App::setWindowIcon() {
#if !defined(__APPLE__)
#  if defined(GLFW_PLATFORM_WAYLAND)
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) return;
#  endif
    const EmbeddedFile* f = iconFile("logo");
    std::string err;
    const ImagePtr img = f ? decodeImage(f->data, f->size, err) : nullptr;
    if (!img) return;
    std::vector<std::vector<uint8_t>> px;
    std::vector<GLFWimage> imgs;
    for (int s : {16, 24, 32, 48, 64, 128, 256}) px.push_back(s == img->width ? img->rgba : resizeArea(*img, s, s));
    int k = 0;
    for (int s : {16, 24, 32, 48, 64, 128, 256}) imgs.push_back({s, s, px[size_t(k++)].data()});
    glfwSetWindowIcon(window_, int(imgs.size()), imgs.data());
#endif
}

App::~App() {
    dropTextures();
    for (auto& [k, t] : imgTex_) glDeleteTextures(1, &t.id);
    for (auto& [k, ic] : icons_) if (ic.tex) glDeleteTextures(1, &ic.tex);
    for (auto& t : tabs_) { freeTextures(t->pageTex); freeTextures(t->thumbTex); }
    NFD_Quit();
}

bool App::busy() const {
    return pendingRender_ || drag_ != Drag::None || editPage_ >= 0 || !toast_.empty() || !queue_.empty() || linkPress_.page >= 0;
}

void App::toast(const std::string& msg, double seconds) {
    toast_ = msg;
    toastUntil_ = ImGui::GetTime() + seconds;
}

void App::updateTitle() {
    std::string t = docName_.empty() ? "Korektor PDF" : docName_ + (dirty_ ? " • " : " — ") + "Korektor PDF";
    if (tabs_.size() > 1) t += "  (" + std::to_string(tabs_.size()) + " karty)";
    glfwSetWindowTitle(window_, t.c_str());
}

void App::markDirty() {
    if (!dirty_) { dirty_ = true; updateTitle(); }
}

// ------------------------------------------------------------------------------- cofanie

void App::snapshot() { pushUndo(doc_.pages); }

void App::beginPropChange() {
    if (editPage_ < 0) snapshot();   // w trakcie pisania zmiana wchodzi do kroku cofania całej edycji
    else markDirty();
}

void App::pushUndo(std::vector<Page> before) {
    undo_.push_back(std::move(before));
    if (undo_.size() > 100) undo_.erase(undo_.begin());
    redo_.clear();
    markDirty();
}

void App::undo() {
    if (editPage_ >= 0) finishEdit();
    if (undo_.empty()) return;
    redo_.push_back(doc_.pages);
    doc_.pages = std::move(undo_.back());
    undo_.pop_back();
    selPage_ = selObj_ = -1;
    lk_.page = -1;
    markDirty();
}

void App::redo() {
    if (redo_.empty()) return;
    undo_.push_back(doc_.pages);
    doc_.pages = std::move(redo_.back());
    redo_.pop_back();
    selPage_ = selObj_ = -1;
    lk_.page = -1;
    markDirty();
}

// ------------------------------------------------------------------------------- tekstury

static void uploadTexture(unsigned& id, int w, int h, const uint8_t* rgba) {
    if (!id) glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

std::vector<kpdf::Rect> App::contentBoxes(const Page& p, float scale) const {
    std::vector<kpdf::Rect> out;
    for (const Obj& o : p.objs) {
        kpdf::Rect r;
        if (o.type == ObjType::Text) {
            r = objBounds(p, o);
            if (o.origBox.w > 0) {
                const float x0 = std::min(r.x, o.origBox.x), y0 = std::min(r.y, o.origBox.y);
                const float x1 = std::max(r.x + r.w, o.origBox.x + o.origBox.w), y1 = std::max(r.y + r.h, o.origBox.y + o.origBox.h);
                r = {x0, y0, x1 - x0, y1 - y0};
            }
            r = {r.x - 3, r.y - 3, r.w + 6, r.h + 6};
        } else if (o.type == ObjType::Whiteout) {
            r = {o.x - 30, o.y - 30, o.w + 60, o.h + 60};   // usunięte znaki mogą wystawać poza prostokąt
        } else if (isShape(o)) {
            if (o.shapeIdle()) continue;
            const float m = (o.strokeOn ? o.size : 0.f) + 3.f;   // grubość linii i jej zakończenia
            float x0 = o.x, y0 = o.y, x1 = o.x + o.w, y1 = o.y + o.h;
            if (o.type == ObjType::PageShape) {
                x0 = std::min(x0, o.origBox.x); y0 = std::min(y0, o.origBox.y);
                x1 = std::max(x1, o.origBox.x + o.origBox.w); y1 = std::max(y1, o.origBox.y + o.origBox.h);
            }
            r = {x0 - m, y0 - m, x1 - x0 + 2 * m, y1 - y0 + 2 * m};
        } else if (o.type == ObjType::PageImage) {
            // pierwotne i obecne miejsce obrazu
            const float x0 = std::min(o.x, o.origBox.x), y0 = std::min(o.y, o.origBox.y);
            const float x1 = std::max(o.x + o.w, o.origBox.x + o.origBox.w), y1 = std::max(o.y + o.h, o.origBox.y + o.origBox.h);
            r = {x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4};
        } else {
            continue;
        }
        const Vec2 a = baseToShown(p, {r.x, r.y}), c = baseToShown(p, {r.x + r.w, r.y + r.h});
        out.push_back({std::min(a.x, c.x) * scale, std::min(a.y, c.y) * scale, std::fabs(c.x - a.x) * scale, std::fabs(c.y - a.y) * scale});
    }
    return out;
}

void App::ensurePageTex(const Page& p, std::unordered_map<int, Tex>& cache, float scale, int& budget, bool partial) {
    const float maxDim = std::max(p.shownWidth(), p.shownHeight());
    scale = std::min(scale, 8192.f / std::max(1.f, maxDim));
    Tex& t = cache[p.id];
    t.lastUse = frameNo_;
    const uint64_t ver = doc_.contentVersion(p);
    const bool sameGeom = t.id && t.rot == p.extraRot && std::fabs(t.scale - scale) < 1e-3f;
    if (sameGeom && t.version == ver) return;

    if (sameGeom && partial) {
        // Zmieniła się tylko treść (np. pisanie): renderujemy na nowo wyłącznie zmieniony obszar.
        std::vector<kpdf::Rect> boxes = contentBoxes(p, scale);
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        auto add = [&](const kpdf::Rect& r) {
            x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.x + r.w); y1 = std::max(y1, r.y + r.h);
        };
        auto same = [](const kpdf::Rect& a, const kpdf::Rect& b) {
            return std::fabs(a.x - b.x) < 0.5f && std::fabs(a.y - b.y) < 0.5f && std::fabs(a.w - b.w) < 0.5f && std::fabs(a.h - b.h) < 0.5f;
        };
        if (boxes.size() != t.boxes.size()) {
            for (auto& r : boxes) add(r);
            for (auto& r : t.boxes) add(r);
        } else {
            for (size_t i = 0; i < boxes.size(); ++i)
                if (!same(boxes[i], t.boxes[i])) { add(boxes[i]); add(t.boxes[i]); }
            // te same obszary, inna treść (np. kolor albo litera tej samej szerokości) — odświeżamy wszystkie
            if (x1 < x0) for (auto& r : boxes) add(r);
        }
        if (x1 > x0) {
            const int region[4] = {int(std::floor(x0)) - 2, int(std::floor(y0)) - 2, int(std::ceil(x1 - x0)) + 5, int(std::ceil(y1 - y0)) + 5};
            int rr[4] = {std::clamp(region[0], 0, t.w - 1), std::clamp(region[1], 0, t.h - 1), 0, 0};
            rr[2] = std::clamp(region[0] + region[2], 1, t.w) - rr[0];
            rr[3] = std::clamp(region[1] + region[3], 1, t.h) - rr[1];
            std::vector<uint8_t> rgba;
            int w, h;
            if (rr[2] > 0 && rr[3] > 0 && doc_.render(p, scale, rgba, w, h, rr)) {
                glBindTexture(GL_TEXTURE_2D, t.id);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexSubImage2D(GL_TEXTURE_2D, 0, rr[0], rr[1], w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            }
        }
        t.version = ver;
        t.boxes = std::move(boxes);
        return;
    }

    // miniatury przy pisaniu odświeżamy najwyżej co pół sekundy
    if (sameGeom && !partial && ImGui::GetTime() - t.renderedAt < 0.5) { pendingRender_ = true; return; }
    if (budget <= 0) { pendingRender_ = true; return; }
    --budget;
    std::vector<uint8_t> rgba;
    int w, h;
    if (!doc_.render(p, scale, rgba, w, h)) return;
    uploadTexture(t.id, w, h, rgba.data());
    t.w = w; t.h = h; t.scale = scale; t.rot = p.extraRot;
    t.version = ver;
    t.boxes = contentBoxes(p, scale);
    t.renderedAt = ImGui::GetTime();
}

unsigned App::imageTex(const ImagePtr& img) {
    if (!img) return 0;
    Tex& t = imgTex_[img->uid];
    if (!t.id) uploadTexture(t.id, img->width, img->height, img->rgba.data());
    return t.id;
}

// Zmniejszenie obrazu metodą średniej z pól (na wstępnie przemnożonej alfie — bez ciemnych obwódek).
static std::vector<uint8_t> resizeArea(const ImageData& im, int W, int H) {
    const int w = im.width, h = im.height;
    std::vector<float> pm(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        const float a = im.rgba[i * 4 + 3] / 255.f;
        for (int c = 0; c < 3; ++c) pm[i * 4 + size_t(c)] = im.rgba[i * 4 + size_t(c)] / 255.f * a;
        pm[i * 4 + 3] = a;
    }
    struct Wt { int s; float k; };
    auto weights = [](int srcN, int dstN) {
        std::vector<std::vector<Wt>> out(static_cast<size_t>(dstN));
        const float sc = float(srcN) / float(dstN);
        for (int d = 0; d < dstN; ++d) {
            const float s0 = d * sc, s1 = (d + 1) * sc;
            for (int s = int(s0); s < int(std::ceil(s1)) && s < srcN; ++s) {
                const float cov = std::min(float(s + 1), s1) - std::max(float(s), s0);
                if (cov > 0) out[size_t(d)].push_back({s, cov / (s1 - s0)});
            }
        }
        return out;
    };
    const auto wx = weights(w, W), wy = weights(h, H);
    std::vector<float> tmp(size_t(W) * size_t(h) * 4, 0.f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < W; ++x)
            for (const Wt& t : wx[size_t(x)])
                for (int c = 0; c < 4; ++c) tmp[(size_t(y) * W + x) * 4 + c] += pm[(size_t(y) * w + t.s) * 4 + c] * t.k;
    std::vector<uint8_t> out(size_t(W) * size_t(H) * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float v[4] = {0, 0, 0, 0};
            for (const Wt& t : wy[size_t(y)])
                for (int c = 0; c < 4; ++c) v[c] += tmp[(size_t(t.s) * W + x) * 4 + c] * t.k;
            uint8_t* d = &out[(size_t(y) * W + x) * 4];
            const float a = v[3];
            for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp(a > 1e-4f ? v[c] / a : 0.f, 0.f, 1.f) * 255.f + 0.5f);
            d[3] = uint8_t(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f);
        }
    return out;
}

// Tekstura ikony w dokładnie potrzebnym rozmiarze (w pikselach ekranu), tworzona przy pierwszym użyciu
// i po zmianie skali ekranu.
unsigned App::iconTex(const char* name, int px) {
    IconTex& ic = icons_[name];
    if (ic.tex && ic.px == px) return ic.tex;
    if (ic.failed) return 0;
    if (!ic.img) {
        const EmbeddedFile* f = iconFile(name);
        std::string err;
        if (f) ic.img = decodeImage(f->data, f->size, err);
        if (!ic.img) { ic.failed = true; return 0; }
    }
    px = std::clamp(px, 8, ic.img->width);
    if (ic.tex && ic.px == px) return ic.tex;
    const std::vector<uint8_t> rgba = px == ic.img->width ? ic.img->rgba : resizeArea(*ic.img, px, px);
    uploadTexture(ic.tex, px, px, rgba.data());
    ic.px = px;
    return ic.tex;
}

void App::dropTextures() {
    for (auto* m : {&pageTex_, &thumbTex_})
        for (auto& [k, t] : *m) if (t.id) glDeleteTextures(1, &t.id);
    pageTex_.clear();
    thumbTex_.clear();
}

void App::evictTextures() {
    const size_t limit = 24;
    for (auto* m : {&pageTex_, &thumbTex_}) {
        if (m->size() <= (m == &pageTex_ ? limit : 400)) continue;
        std::vector<std::pair<int, int>> age;
        for (auto& [id, t] : *m) if (t.lastUse < frameNo_ - 30) age.push_back({t.lastUse, id});
        std::sort(age.begin(), age.end());
        size_t excess = m->size() - (m == &pageTex_ ? limit : 400);
        for (size_t i = 0; i < age.size() && i < excess; ++i) {
            auto it = m->find(age[i].second);
            if (it->second.id) glDeleteTextures(1, &it->second.id);
            m->erase(it);
        }
    }
    // tekstury stron, których już nie ma w dokumencie
    for (auto* m : {&pageTex_, &thumbTex_})
        for (auto it = m->begin(); it != m->end();)
            if (doc_.indexOfPage(it->first) < 0 && it->second.lastUse < frameNo_ - 600) {
                if (it->second.id) glDeleteTextures(1, &it->second.id);
                it = m->erase(it);
            } else ++it;
}

// ------------------------------------------------------------------------------- granice

kpdf::Rect App::objBounds(const Page& p, const Obj& o) const {
    switch (o.type) {
    case ObjType::Ink: {
        if (o.pts.empty()) return {0, 0, 0, 0};
        float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
        for (auto& q : o.pts) { x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y); }
        const float r = o.size / 2;
        return {x0 - r, y0 - r, x1 - x0 + 2 * r, y1 - y0 + 2 * r};
    }
    case ObjType::Text: {
        // usunięta linia (pusty tekst w miejscu oryginału) da się zaznaczyć w jej dawnym miejscu
        if (o.text.empty() && !o.srcObjs.empty()) return o.origBox;
        kpdf::Rect r = layoutBounds(doc_.layoutText(p, o));
        if (r.w < o.size * 0.4f) { r.x -= o.size * 0.2f; r.w = std::max(r.w, o.size * 0.4f); }
        return r;
    }
    default:
        return {o.x, o.y, o.w, o.h};
    }
}

// ------------------------------------------------------------------------------- zaznaczenie

Obj* App::selectedObj() {
    Page* p = doc_.pageById(selPage_);
    if (!p || selObj_ < 0 || selObj_ >= int(p->objs.size())) return nullptr;
    return &p->objs[selObj_];
}

App::Prop App::propTarget() const {
    if (editPage_ >= 0) return Prop::Text;
    if (const Page* p = const_cast<App*>(this)->doc_.pageById(selPage_)) {
        if (selObj_ >= 0 && selObj_ < int(p->objs.size())) {
            switch (p->objs[selObj_].type) {
                case ObjType::Text: return Prop::Text;
                case ObjType::Ink: return Prop::Pen;
                case ObjType::Highlight: return Prop::Highlight;
                default: return Prop::None;
            }
        }
    }
    switch (tool_) {
        case Tool::Text: return Prop::Text;
        case Tool::Pen: return Prop::Pen;
        case Tool::Highlight: return Prop::Highlight;
        default: return Prop::None;
    }
}

Color& App::propColor(Prop p) {
    return p == Prop::Pen ? colPen_ : p == Prop::Highlight ? colHl_ : textStyle_.color;
}

int App::hitTest(const Page& p, Vec2 b) const {
    const float tol = 4.f / zoom_;
    for (int i = int(p.objs.size()) - 1; i >= 0; --i) {
        const Obj& o = p.objs[i];
        if (removedFromFile(o)) continue;   // usunięty obraz albo kształt z pliku
        if (o.type == ObjType::Text && o.text.empty() && o.srcObjs.empty()) continue;
        if (o.type == ObjType::Ink) {
            const float lim = std::max(o.size / 2, tol + 1.f / zoom_);
            if (o.pts.size() == 1 && std::hypot(b.x - o.pts[0].x, b.y - o.pts[0].y) <= lim) return i;
            for (size_t k = 1; k < o.pts.size(); ++k)
                if (segDist(b, o.pts[k - 1], o.pts[k]) <= lim) return i;
            continue;
        }
        const kpdf::Rect r = objBounds(p, o);
        if (b.x >= r.x - tol && b.x <= r.x + r.w + tol && b.y >= r.y - tol && b.y <= r.y + r.h + tol) return i;
    }
    return -1;
}


int App::hitHandle(const Page& p, const ImVec2& origin, const ImVec2& mouse) const {
    if (selPage_ != p.id || selObj_ < 0 || selObj_ >= int(p.objs.size())) return -1;
    const Obj& o = p.objs[size_t(selObj_)];
    if (!resizable(o) || removedFromFile(o)) return -1;
    const Vec2 c[4] = {{o.x, o.y}, {o.x + o.w, o.y}, {o.x + o.w, o.y + o.h}, {o.x, o.y + o.h}};
    const float r = ImGui::GetFontSize() * 0.55f;
    for (int k = 0; k < 4; ++k) {
        if (!allCorners(o) && k != 2) continue;   // zakreślenie i zamazanie: tylko prawy dolny
        const Vec2 s = baseToShown(p, c[k]);
        if (std::fabs(mouse.x - (origin.x + s.x * zoom_)) <= r && std::fabs(mouse.y - (origin.y + s.y * zoom_)) <= r) return k;
    }
    return -1;
}

// ------------------------------------------------------------------------------- narzędzia

void App::setTool(Tool t) {
    if (editPage_ >= 0) finishEdit();
    tool_ = t;
    if (t != Tool::Select) selPage_ = selObj_ = -1;
}

void App::setZoom(float z, const ImVec2* anchor) {
    z = std::clamp(z, 0.1f, 6.f);
    if (std::fabs(z - zoom_) < 1e-4f) return;
    zoomRatio_ = z / zoom_;
    zoomAnchorPending_ = true;
    zoomAnchorScreen_ = anchor ? *anchor : ImVec2(-1, -1);
    zoom_ = z;
}

void App::beginPointer(Page& p, Vec2 b, bool dbl) {
    // kliknięcie w edytowany tekst przesuwa kursor albo zaczyna zaznaczanie
    if (editPage_ >= 0) {
        if (editPage_ == p.id && hitWidthHandle(p, ImVec2(pageOrigin_[p.id].first, pageOrigin_[p.id].second), ImGui::GetIO().MousePos, editObj_)) {
            dragOrig_ = p.objs[size_t(editObj_)];
            dragPage_ = p.id;
            dragStart_ = b;
            drag_ = Drag::TextWidth;
            return;
        }
        if (editPage_ == p.id && hitTest(p, b) == editObj_) {
            Obj& o = p.objs[editObj_];
            caret_ = caretFromPoint(p, o, b);
            if (!ImGui::GetIO().KeyShift) anchor_ = caret_;
            caretMoved();
            if (dbl) {   // dwuklik zaznacza słowo
                auto isWord = [](unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '-' || c == '_'; };
                size_t a = caret_, e = caret_;
                while (a > 0 && isWord(o.text[a - 1])) --a;
                while (e < o.text.size() && isWord(o.text[e])) ++e;
                anchor_ = a;
                caret_ = e;
                caretMoved();
            }
            caretTime_ = ImGui::GetTime();
            dragPage_ = p.id;
            drag_ = Drag::TextSelect;
            return;
        }
        finishEdit();
    }
    dragPage_ = p.id;
    dragStart_ = b;
    dragMoved_ = false;

    // poprawianie tekstu z pliku: narzędzie Tekst albo dwuklik w trybie zaznaczania
    auto pickExisting = [&]() -> bool {
        Obj o;
        int caret = 0;
        std::string why;
        const Pick r = doc_.pickText(p, b, o, caret, why);
        if (r == Pick::NotEditable) { toast(why, 6); return true; }
        if (r != Pick::Ok) return false;
        editBefore_ = doc_.pages;
        p.objs.push_back(std::move(o));
        startEdit(p.id, int(p.objs.size()) - 1, true, caret);
        return true;
    };

    switch (tool_) {
    case Tool::Select: {
        const ImVec2 orgS(pageOrigin_[p.id].first, pageOrigin_[p.id].second);
        if (selPage_ == p.id && hitWidthHandle(p, orgS, ImGui::GetIO().MousePos, selObj_)) {
            dragBefore_ = doc_.pages;
            dragOrig_ = p.objs[size_t(selObj_)];
            drag_ = Drag::TextWidth;
            return;
        }
        if (const int corner = hitHandle(p, orgS, ImGui::GetIO().MousePos); corner >= 0) {
            dragBefore_ = doc_.pages;
            dragOrig_ = p.objs[size_t(selObj_)];
            draft_ = dragOrig_;
            dragCorner_ = corner;
            drag_ = Drag::Resize;
            return;
        }
        // odnośnik: Ctrl+klik otwiera, zwykłe kliknięcie (bez przeciągania) pokaże okienko odnośnika
        linkPress_ = {};
        if (const int li = hitLink(p, b); li >= 0) {
            if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) { cmdOpenLink(p.links[size_t(li)]); return; }
            if (!dbl) linkPress_ = {p.id, li, ImGui::GetIO().MousePos, ImGui::GetTime(), false};
        }
        int i = hitTest(p, b);
        if (dbl) {
            // dwuklik: edycja tekstu w miejscu kliknięcia
            if (i >= 0 && p.objs[size_t(i)].type == ObjType::Text) {
                startEdit(p.id, i, false);
                caret_ = anchor_ = caretFromPoint(p, p.objs[size_t(i)], b);
                caretMoved();
                dragPage_ = p.id;
                drag_ = Drag::TextSelect;
                return;
            }
            // tekst z pliku leżący na obrazie
            if ((i < 0 || p.objs[size_t(i)].type == ObjType::PageImage) && pickExisting()) return;
        }
        // pojedyncze kliknięcie: akapit albo obraz z pliku staje się zaznaczonym elementem
        if (i < 0) i = pickAt(p, b);
        if (i < 0) {
            selPage_ = selObj_ = -1;
            return;
        }
        selPage_ = p.id;
        selObj_ = i;
        syncFromSelection();
        const Obj& o = p.objs[size_t(i)];
        dragBefore_ = doc_.pages;
        dragOrig_ = o;
        draft_ = o;
        drag_ = Drag::Move;
        return;
    }
    case Tool::Text: {
        const int i = hitTest(p, b);
        if (i >= 0 && p.objs[i].type == ObjType::Text) {
            startEdit(p.id, i, false);
            Obj& o = p.objs[i];
            caret_ = anchor_ = caretFromPoint(p, o, b);
            caretMoved();
            dragPage_ = p.id;
            drag_ = Drag::TextSelect;
            return;
        }
        if (pickExisting()) return;
        createTextAt(p, b);
        drag_ = Drag::TextBox;   // przeciągnięcie wyznacza szerokość akapitu
        dragPage_ = p.id;
        return;
    }
    case Tool::Pen:
        draft_ = Obj{};
        draft_.type = ObjType::Ink;
        draft_.size = sizePen_;
        draft_.color = colPen_;
        draft_.pts = {b};
        drag_ = Drag::Pen;
        selPage_ = selObj_ = -1;
        return;
    case Tool::Highlight:
    case Tool::Whiteout:
        draft_ = Obj{};
        draft_.type = tool_ == Tool::Highlight ? ObjType::Highlight : ObjType::Whiteout;
        draft_.color = colHl_;
        draft_.x = b.x;
        draft_.y = b.y;
        drag_ = Drag::Rect;
        selPage_ = selObj_ = -1;
        return;
    }
}

// Akapit albo obraz z pliku pod punktem staje się elementem modelu (niezmienionym — strona
// wygląda tak samo, dopóki go nie zmienisz). Zwraca indeks elementu albo -1.
int App::pickAt(Page& p, Vec2 b) {
    Obj o;
    int caret = 0;
    std::string why;
    if (doc_.hasTextAt(p, b) && doc_.pickText(p, b, o, caret, why) == Pick::Ok) {
        p.objs.push_back(std::move(o));
        return int(p.objs.size()) - 1;
    }
    // obraz albo kształt — ten, który leży wyżej (rysowany później)
    Obj img, shp;
    std::vector<int> zi, zs;
    const bool hasImg = doc_.pickImage(p, b, img, &zi);
    const bool hasShp = doc_.pickShape(p, b, 4.f / zoom_, shp, &zs);
    if (hasImg || hasShp) {
        const bool shapeOnTop = hasShp && (!hasImg || std::lexicographical_compare(zi.begin(), zi.end(), zs.begin(), zs.end()));
        p.objs.push_back(shapeOnTop ? std::move(shp) : std::move(img));
        return int(p.objs.size()) - 1;
    }
    return -1;
}

void App::syncFromSelection() {
    const Obj* o = selectedObj();
    if (!o) return;
    if (o->type == ObjType::Text) { textStyle_ = o->styleAt(0); align_ = o->align; textUnit_ = std::hypot(o->up.x, o->up.y); }
    if (o->type == ObjType::Ink) { colPen_ = o->color; sizePen_ = o->size; }
    if (o->type == ObjType::Highlight) colHl_ = o->color;
}

void App::createTextAt(Page& p, Vec2 b) {
    // nowy tekst, poziomo na ekranie niezależnie od obrotu strony
    editBefore_ = doc_.pages;
    Obj t;
    t.type = ObjType::Text;
    TextStyle st = textStyle_;
    st.size = std::clamp(textStyle_.size * textUnit_, 4.f, 200.f);   // rozmiar widoczny (tekst z pliku bywa skalowany macierzą)
    st.font.origObj = -1;           // nowy tekst: czcionka z pliku, jeśli strona ma taki krój
    st.font.okChars.clear();
    st.font.fit = 1.f;
    doc_.matchPageFont(p, st.font);
    st.renderMode = 0;
    t.styles = {st};
    t.align = Align::Left;   // nowy tekst zawsze od lewej; wyrównanie zmienia się przyciskami po wpisaniu
    align_ = Align::Left;
    const Vec2 o0 = shownToBase(p, {0, 0}), ox = shownToBase(p, {1, 0}), oy = shownToBase(p, {0, 1});
    t.adv = {ox.x - o0.x, ox.y - o0.y};
    t.up = {-(oy.x - o0.x), -(oy.y - o0.y)};
    const float down = st.size * 0.35f;   // kliknięcie wypada mniej więcej w środku wiersza
    t.x = b.x - t.up.x * down;
    t.y = b.y - t.up.y * down;
    p.objs.push_back(t);
    startEdit(p.id, int(p.objs.size()) - 1, true);
}

void App::updateDrag() {
    if (drag_ == Drag::None) return;
    Page* p = doc_.pageById(dragPage_);
    if (!p) { drag_ = Drag::None; return; }
    const auto org = pageOrigin_[p->id];
    const ImVec2 m = ImGui::GetIO().MousePos;
    Vec2 b = shownToBase(*p, {(m.x - org.first) / zoom_, (m.y - org.second) / zoom_});
    b.x = std::clamp(b.x, 0.f, p->width);
    b.y = std::clamp(b.y, 0.f, p->height);
    const float dx = b.x - dragStart_.x, dy = b.y - dragStart_.y;

    if (drag_ == Drag::TextSelect) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { drag_ = Drag::None; return; }
        if (Obj* o = editedObj()) {
            if (std::hypot(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y) > 0) {
                caret_ = caretFromPoint(*p, *o, b);
                caretMoved();
            }
        }
        return;
    }
    if (drag_ == Drag::TextWidth || drag_ == Drag::TextBox) {
        const int oi = drag_ == Drag::TextBox || editPage_ >= 0 ? editObj_ : selObj_;
        if (oi < 0 || oi >= int(p->objs.size())) { drag_ = Drag::None; return; }
        Obj& o = p->objs[size_t(oi)];
        const float al2 = o.adv.x * o.adv.x + o.adv.y * o.adv.y;
        const float w = ((b.x - o.x) * o.adv.x + (b.y - o.y) * o.adv.y) / al2;   // jednostki tekstu
        const float minW = 2.f * textStyle_.size;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (drag_ == Drag::TextBox && std::hypot(dx, dy) * zoom_ < 8.f) { o.boxWidth = 0; return; }   // zwykłe kliknięcie
            dragMoved_ = true;
            o.boxWidth = std::max(minW, w);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            return;
        }
        if (drag_ == Drag::TextWidth && dragMoved_ && editPage_ < 0) pushUndo(std::move(dragBefore_));
        if (drag_ == Drag::TextWidth && dragMoved_) markDirty();
        dragBefore_.clear();
        drag_ = Drag::None;
        return;
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        switch (drag_) {
        case Drag::Move:
        case Drag::Resize: {
            if (!dragMoved_ && std::hypot(dx, dy) * zoom_ < 2.f) break;
            dragMoved_ = true;
            if (selObj_ < 0 || selObj_ >= int(p->objs.size())) break;
            // obraz z pliku: podgląd przeciągania bez ponownego renderowania strony (model zmienia się po puszczeniu)
            Obj& o = liveObj(p->objs[size_t(selObj_)]) ? draft_ : p->objs[size_t(selObj_)];
            if (drag_ == Drag::Resize) {
                // przeciągany narożnik; przeciwległy stoi w miejscu
                const Obj& g = dragOrig_;
                const Vec2 c[4] = {{g.x, g.y}, {g.x + g.w, g.y}, {g.x + g.w, g.y + g.h}, {g.x, g.y + g.h}};
                const Vec2 fixed = c[(dragCorner_ + 2) % 4];
                Vec2 moving{c[dragCorner_].x + dx, c[dragCorner_].y + dy};
                float w = std::max(3.f, std::fabs(moving.x - fixed.x)), h = std::max(3.f, std::fabs(moving.y - fixed.y));
                // proporcje: obraz domyślnie je zachowuje (Shift = dowolne), kształt odwrotnie
                const bool keepAspect = isImage(g) ? !ImGui::GetIO().KeyShift : (isShape(g) && ImGui::GetIO().KeyShift);
                if (keepAspect && g.w > 0 && g.h > 0) {
                    const float k = std::max(w / g.w, h / g.h);
                    w = g.w * k;
                    h = g.h * k;
                }
                const bool left = moving.x < fixed.x, up = moving.y < fixed.y;
                o.x = left ? fixed.x - w : fixed.x;
                o.y = up ? fixed.y - h : fixed.y;
                o.w = w;
                o.h = h;
                // linia (bardzo cienki kształt): zmienia się tylko długość, grubość zostaje
                if (thinAxis(g) == 1) { o.y = g.y; o.h = g.h; }
                if (thinAxis(g) == 2) { o.x = g.x; o.w = g.w; }
            } else if (o.type == ObjType::Ink) {
                for (size_t k = 0; k < o.pts.size(); ++k)
                    o.pts[k] = {dragOrig_.pts[k].x + dx, dragOrig_.pts[k].y + dy};
            } else {
                o.x = dragOrig_.x + dx;
                o.y = dragOrig_.y + dy;
            }
            break;
        }
        case Drag::Pen: {
            const Vec2 l = draft_.pts.back();
            if (std::hypot(b.x - l.x, b.y - l.y) * zoom_ > 1.2f) draft_.pts.push_back(b);
            break;
        }
        case Drag::Rect:
            draft_.x = std::min(dragStart_.x, b.x);
            draft_.y = std::min(dragStart_.y, b.y);
            draft_.w = std::fabs(dx);
            draft_.h = std::fabs(dy);
            break;
        default: break;
        }
        return;
    }

    // puszczony przycisk — zatwierdzenie
    switch (drag_) {
    case Drag::Move:
    case Drag::Resize:
        if (dragMoved_) {
            if (selObj_ >= 0 && selObj_ < int(p->objs.size()) && liveObj(p->objs[size_t(selObj_)]))
                p->objs[size_t(selObj_)] = draft_;
            // odnośniki leżące na przesuniętym tekście albo obrazie wędrują razem z nim
            if (drag_ == Drag::Move && selObj_ >= 0 && selObj_ < int(p->objs.size())) {
                const Obj& now = p->objs[size_t(selObj_)];
                if (now.type == ObjType::Text || isImage(now))
                    moveLinksWith(*p, objBounds(*p, dragOrig_), now.x - dragOrig_.x, now.y - dragOrig_.y);
            }
            pushUndo(std::move(dragBefore_));
        }
        break;
    case Drag::Pen: {
        auto before = doc_.pages;
        p->objs.push_back(draft_);
        pushUndo(std::move(before));
        break;
    }
    case Drag::Rect:
        if (draft_.w * zoom_ >= 3 && draft_.h * zoom_ >= 3) {
            auto before = doc_.pages;
            p->objs.push_back(draft_);
            pushUndo(std::move(before));
        }
        break;
    default: break;
    }
    dragBefore_.clear();
    drag_ = Drag::None;
}

Obj* App::editedObj() {
    Page* p = doc_.pageById(editPage_);
    if (!p || editObj_ < 0 || editObj_ >= int(p->objs.size())) return nullptr;
    return &p->objs[editObj_];
}

static size_t charToByte(const std::string& s, int ci) {
    size_t i = 0;
    for (int k = 0; k < ci && i < s.size(); ++k) {
        ++i;
        while (i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80) ++i;
    }
    return i;
}
static int byteToChar(const std::string& s, size_t b) {
    int n = 0;
    for (size_t i = 0; i < b && i < s.size(); ++i)
        if ((uint8_t(s[i]) & 0xC0) != 0x80) ++n;
    return n;
}
static size_t prevCp(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (uint8_t(s[i]) & 0xC0) == 0x80) --i;
    return i;
}
static size_t nextCp(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (uint8_t(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

// Wiersz układu, w którym stoi kursor przed znakiem `ci`. Na granicy zawiniętych wierszy
// kursor należy do początku następnego wiersza.
static int lineOfChar(const TextLayout& l, int ci) {
    int best = 0;
    for (size_t k = 0; k < l.lines.size(); ++k)
        if (l.lines[k].first <= ci) best = int(k);
    return best;
}

size_t App::caretFromPoint(const Page& p, const Obj& o, Vec2 b) const {
    const TextLayout l = doc_.layoutText(p, o);
    if (l.lines.empty()) return 0;
    const float ul = std::hypot(o.up.x, o.up.y), al = std::hypot(o.adv.x, o.adv.y);
    const Vec2 u{o.up.x / ul, o.up.y / ul}, a{o.adv.x / al, o.adv.y / al};
    // wiersz: punkt pomiędzy górą a dołem wiersza, albo najbliższy
    int best = 0;
    float bestD = 1e30f;
    for (size_t k = 0; k < l.lines.size(); ++k) {
        const auto& ln = l.lines[k];
        const Vec2 c = ln.carets[0];
        const float h = (b.x - c.x) * u.x + (b.y - c.y) * u.y;   // wysokość nad linią bazową
        const float top = ln.ascent.x * u.x + ln.ascent.y * u.y, bot = ln.descent.x * u.x + ln.descent.y * u.y;
        const float d = h > top ? h - top : h < bot ? bot - h : 0.f;
        if (d < bestD) { bestD = d; best = int(k); }
    }
    const auto& cs = l.lines[size_t(best)].carets;
    int col = 0;
    float colD = 1e30f;
    for (size_t k = 0; k < cs.size(); ++k) {
        const float d = std::fabs((b.x - cs[k].x) * a.x + (b.y - cs[k].y) * a.y);
        if (d < colD) { colD = d; col = int(k); }
    }
    // spacja kończąca zawinięty wiersz: kursor przed nią, a nie na początku następnego wiersza
    int ci = l.lines[size_t(best)].first + col;
    const std::u32string t = utf8To32(o.text);
    if (best + 1 < int(l.lines.size()) && ci == l.lines[size_t(best) + 1].first && ci > 0 && t[size_t(ci) - 1] == U' ') --ci;
    return charToByte(o.text, ci);
}

ImVec2 App::widthHandlePos(const Page& p, const ImVec2& org, const Obj& o) const {
    const TextLayout l = doc_.layoutText(p, o);
    if (l.lines.empty()) return ImVec2(-1e9f, -1e9f);
    const auto& first = l.lines.front();
    const auto& last = l.lines.back();
    float w = o.boxWidth;
    if (w <= 0) {   // bez zawijania: prawa krawędź najdłuższego wiersza
        for (const auto& ln : l.lines) {
            const Vec2 c = ln.carets.back(), s = ln.carets.front();
            const float al2 = o.adv.x * o.adv.x + o.adv.y * o.adv.y;
            w = std::max(w, ((c.x - s.x) * o.adv.x + (c.y - s.y) * o.adv.y) / al2);
        }
    }
    // środek prawej krawędzi ramki: od góry pierwszego do dołu ostatniego wiersza
    const float up2 = o.up.x * o.up.x + o.up.y * o.up.y;
    const float lastDrop = ((o.x - last.carets[0].x) * o.up.x + (o.y - last.carets[0].y) * o.up.y) / up2;   // jednostki tekstu
    const Vec2 top{o.x + first.ascent.x, o.y + first.ascent.y};
    const Vec2 bot{o.x - o.up.x * lastDrop + last.descent.x, o.y - o.up.y * lastDrop + last.descent.y};
    const Vec2 mid{(top.x + bot.x) / 2 + o.adv.x * w, (top.y + bot.y) / 2 + o.adv.y * w};
    const Vec2 sh = baseToShown(p, mid);
    return ImVec2(org.x + sh.x * zoom_, org.y + sh.y * zoom_);
}

bool App::hitWidthHandle(const Page& p, const ImVec2& org, const ImVec2& mouse, int obj) const {
    if (obj < 0 || obj >= int(p.objs.size()) || p.objs[size_t(obj)].type != ObjType::Text) return false;
    const ImVec2 h = widthHandlePos(p, org, p.objs[size_t(obj)]);
    const float r = ImGui::GetFontSize() * 0.6f;
    return std::fabs(mouse.x - h.x) <= r && std::fabs(mouse.y - h.y) <= r * 1.6f;
}

void App::startEdit(int pageId, int obj, bool fresh, int caretChar) {
    Page* p = doc_.pageById(pageId);
    if (!p || obj < 0 || obj >= int(p->objs.size())) return;
    if (!fresh) editBefore_ = doc_.pages;
    editPage_ = pageId;
    editObj_ = obj;
    editFresh_ = fresh;
    Obj& o = p->objs[size_t(obj)];
    o.normalizeStyles();
    editOrig_ = o;
    trackEditLinks();
    caret_ = anchor_ = caretChar < 0 ? o.text.size() : charToByte(o.text, caretChar);
    caretTime_ = ImGui::GetTime();
    selPage_ = pageId;
    selObj_ = obj;
    align_ = o.align;
    pendingStyle_ = false;
    caretMoved();
}

void App::caretMoved() {
    caretTime_ = ImGui::GetTime();
    Obj* o = editedObj();
    if (!o || o->styles.empty()) return;
    pendingStyle_ = false;
    const size_t at = std::min(caret_, anchor_);
    const int ci = byteToChar(o->text, at);
    // styl znaku przed kursorem; na początku akapitu — znaku za nim
    textStyle_ = o->styleAt(size_t(ci > 0 ? ci - 1 : 0));
    if (caret_ != anchor_) textStyle_ = o->styleAt(size_t(ci));
    textUnit_ = std::hypot(o->up.x, o->up.y);
}

static bool sameText(const Obj& a, const Obj& b) {
    return a.text == b.text && a.styles == b.styles && a.charStyle == b.charStyle && a.x == b.x && a.y == b.y &&
           a.boxWidth == b.boxWidth && a.align == b.align && a.lineStep == b.lineStep && a.firstIndent == b.firstIndent;
}

void App::finishEdit() {
    Page* p = doc_.pageById(editPage_);
    if (p && editObj_ >= 0 && editObj_ < int(p->objs.size())) {
        Obj& o = p->objs[size_t(editObj_)];
        while (!o.text.empty() && (o.text.back() == '\n' || o.text.back() == '\r')) o.text.pop_back();
        o.normalizeStyles();
        const int n = int(utf8To32(o.text).size());
        for (EditLink& e : editLinks_) { e.c0 = std::min(e.c0, n); e.c1 = std::min(e.c1, n); }
        updateEditLinks();
        const bool blank = o.text.find_first_not_of(" \t\r\n") == std::string::npos;
        if (editFresh_ && sameText(o, editOrig_)) {
            // nic nie zmieniono — oryginał zostaje nietknięty
            p->objs.erase(p->objs.begin() + editObj_);
            selPage_ = selObj_ = -1;
        } else if (blank && o.srcObjs.empty()) {
            p->objs.erase(p->objs.begin() + editObj_);
            selPage_ = selObj_ = -1;
            if (!editFresh_) pushUndo(std::move(editBefore_));
        } else {
            if (blank) { o.text.clear(); o.normalizeStyles(); }
            if (editFresh_ || !sameText(o, editOrig_)) pushUndo(std::move(editBefore_));
        }
    }
    editBefore_.clear();
    editLinks_.clear();
    editLinks0_.clear();
    editLinksOrig_.clear();
    editPage_ = editObj_ = -1;
    pendingStyle_ = false;
    if (drag_ == Drag::TextSelect) drag_ = Drag::None;
}

void App::replaceText(size_t a, size_t e, const std::string& str) {
    Obj* o = editedObj();
    if (!o) return;
    o->normalizeStyles();
    const int ca = byteToChar(o->text, a), ce = byteToChar(o->text, e);
    // indeks stylu pisania (dodany, jeśli to nowy wygląd)
    int si = -1;
    for (size_t k = 0; k < o->styles.size(); ++k) if (o->styles[k] == textStyle_) si = int(k);
    if (si < 0) { si = int(o->styles.size()); o->styles.push_back(textStyle_); }
    shiftEditLinks(ca, ce, byteToChar(str, str.size()));
    o->text.replace(a, e - a, str);
    o->charStyle.erase(o->charStyle.begin() + ca, o->charStyle.begin() + ce);
    o->charStyle.insert(o->charStyle.begin() + ca, size_t(byteToChar(str, str.size())), uint16_t(si));
    o->normalizeStyles();
    caret_ = anchor_ = a + str.size();
    caretTime_ = ImGui::GetTime();
    markDirty();
}

void App::eraseSelection() {
    if (caret_ == anchor_) return;
    const size_t a = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
    const TextStyle keep = textStyle_;
    if (Obj* o = editedObj()) textStyle_ = o->styleAt(size_t(byteToChar(o->text, a)));
    replaceText(a, e, "");
    textStyle_ = keep;
}

void App::insertText(const std::string& str) {
    if (!editedObj()) return;
    const size_t a = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
    if (a != e && !pendingStyle_) {   // nadpisanie zaznaczenia — w stylu jego pierwszego znaku
        Obj* o = editedObj();
        textStyle_ = o->styleAt(size_t(byteToChar(o->text, a)));
    }
    replaceText(a, e, str);
}

void App::applyStyle(const std::function<void(TextStyle&)>& fn, bool undoStep) {
    fn(textStyle_);
    Page* p = nullptr;
    Obj* o = nullptr;
    if (editPage_ >= 0) { p = doc_.pageById(editPage_); o = editedObj(); }
    else if (Obj* s = selectedObj(); s && s->type == ObjType::Text) { p = doc_.pageById(selPage_); o = s; }
    if (!o) return;
    size_t a = 0, e = o->text.size();
    if (editPage_ >= 0) {
        if (caret_ == anchor_) { pendingStyle_ = true; return; }   // dotyczy następnych znaków
        a = std::min(caret_, anchor_);
        e = std::max(caret_, anchor_);
    }
    if (undoStep) beginPropChange();
    else markDirty();
    o = editPage_ >= 0 ? editedObj() : selectedObj();
    o->normalizeStyles();
    const int ca = byteToChar(o->text, a), ce = byteToChar(o->text, e);
    std::vector<TextStyle> styles = o->styles;
    for (int i = ca; i < ce; ++i) {
        TextStyle ns = styles[o->charStyle[size_t(i)]];
        fn(ns);
        int idx = -1;
        for (size_t k = 0; k < styles.size(); ++k) if (styles[k] == ns) idx = int(k);
        if (idx < 0) { idx = int(styles.size()); styles.push_back(ns); }
        o->charStyle[size_t(i)] = uint16_t(idx);
    }
    o->styles = std::move(styles);
    o->normalizeStyles();
    (void)p;
}

// Zmiana odmiany: najpierw szukamy takiej czcionki w pliku (np. pogrubionej wersji kroju
// akapitu), dopiero potem bierzemy czcionkę zastępczą.
static void setVariant(const Document& doc, const Page* p, TextStyle& st, int family, bool bold, bool italic) {
    st.font.family = family;
    st.font.bold = bold;
    st.font.italic = italic;
    st.font.origObj = -1;
    st.font.okChars.clear();
    st.font.fit = 1.f;
    if (p) doc.matchPageFont(*p, st.font);
}

void App::toggleBold() {
    const Page* p = doc_.pageById(editPage_ >= 0 ? editPage_ : selPage_);
    const bool on = !textStyle_.font.bold;
    applyStyle([&](TextStyle& st) { setVariant(doc_, p, st, st.font.family, on, st.font.italic); });
}

void App::toggleItalic() {
    const Page* p = doc_.pageById(editPage_ >= 0 ? editPage_ : selPage_);
    const bool on = !textStyle_.font.italic;
    applyStyle([&](TextStyle& st) { setVariant(doc_, p, st, st.font.family, st.font.bold, on); });
}

void App::setAlign(Align a) {
    align_ = a;
    Obj* o = editPage_ >= 0 ? editedObj() : selectedObj();
    if (!o || o->type != ObjType::Text || o->align == a) return;
    Page* p = doc_.pageById(editPage_ >= 0 ? editPage_ : selPage_);
    beginPropChange();
    o = editPage_ >= 0 ? editedObj() : selectedObj();
    if (o->boxWidth <= 0) {   // tekst bez ramki: ramka o szerokości obecnego tekstu
        const kpdf::Rect r = layoutBounds(doc_.layoutText(*p, *o));
        const float al = std::hypot(o->adv.x, o->adv.y);
        o->boxWidth = std::max(std::fabs(o->adv.x) > std::fabs(o->adv.y) ? r.w : r.h, 10.f) / al;
    }
    o->align = a;
}

void App::handleTextKeys() {
    Obj* o = editedObj();
    if (!o) { editPage_ = editObj_ = -1; return; }
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl || io.KeySuper, shift = io.KeyShift;
    caret_ = std::min(caret_, o->text.size());
    anchor_ = std::min(anchor_, o->text.size());

    // wpisywane znaki
    std::string typed;
    for (ImWchar c : io.InputQueueCharacters)
        if (c >= 32 && c != 127) typed += utf32To8(std::u32string(1, char32_t(c)));
    io.InputQueueCharacters.resize(0);
    if (!typed.empty() && !ctrl) insertText(typed);
    o = editedObj();
    const std::string& t = o->text;

    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    auto moveTo = [&](size_t pos) {
        caret_ = pos;
        if (!shift) anchor_ = caret_;
        caretMoved();
    };
    auto isWord = [](unsigned char c) { return c >= 0x80 || std::isalnum(c); };
    auto wordLeft = [&](size_t i) {
        while (i > 0 && !isWord(uint8_t(t[prevCp(t, i)]))) i = prevCp(t, i);
        while (i > 0 && isWord(uint8_t(t[prevCp(t, i)]))) i = prevCp(t, i);
        return i;
    };
    auto wordRight = [&](size_t i) {
        while (i < t.size() && !isWord(uint8_t(t[i]))) i = nextCp(t, i);
        while (i < t.size() && isWord(uint8_t(t[i]))) i = nextCp(t, i);
        return i;
    };
    Page* page = doc_.pageById(editPage_);
    auto layout = [&] { return doc_.layoutText(*page, *editedObj()); };

    if (pressed(ImGuiKey_Escape) || ((pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) && ctrl)) {
        finishEdit();
        return;
    }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) { insertText("\n"); return; }
    if (pressed(ImGuiKey_Backspace)) {
        if (caret_ != anchor_) eraseSelection();
        else if (caret_ > 0) { const size_t from = ctrl ? wordLeft(caret_) : prevCp(t, caret_); anchor_ = from; eraseSelection(); }
        caretMoved();
        return;
    }
    if (pressed(ImGuiKey_Delete)) {
        if (caret_ != anchor_) eraseSelection();
        else if (caret_ < t.size()) { anchor_ = ctrl ? wordRight(caret_) : nextCp(t, caret_); eraseSelection(); }
        caretMoved();
        return;
    }
    if (pressed(ImGuiKey_LeftArrow)) {
        if (!shift && caret_ != anchor_) moveTo(std::min(caret_, anchor_));
        else moveTo(ctrl ? wordLeft(caret_) : prevCp(t, caret_));
    }
    if (pressed(ImGuiKey_RightArrow)) {
        if (!shift && caret_ != anchor_) moveTo(std::max(caret_, anchor_));
        else moveTo(ctrl ? wordRight(caret_) : nextCp(t, caret_));
    }
    if (pressed(ImGuiKey_Home) || pressed(ImGuiKey_End)) {
        if (ctrl) moveTo(pressed(ImGuiKey_Home) ? 0 : t.size());
        else {
            const TextLayout l = layout();
            const int li = lineOfChar(l, byteToChar(t, caret_));
            const auto& ln = l.lines[size_t(li)];
            int ci = ln.first + (pressed(ImGuiKey_Home) ? 0 : int(ln.carets.size()) - 1);
            // koniec zawiniętego wiersza: przed spacją łamania
            const std::u32string u = utf8To32(t);
            if (!pressed(ImGuiKey_Home) && li + 1 < int(l.lines.size()) && ci > 0 && ci <= int(u.size()) && u[size_t(ci) - 1] == U' ') --ci;
            moveTo(charToByte(t, ci));
        }
    }
    if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow)) {
        const TextLayout l = layout();
        const int ci = byteToChar(t, caret_);
        const int li = lineOfChar(l, ci);
        const int nl = pressed(ImGuiKey_UpArrow) ? li - 1 : li + 1;
        if (nl >= 0 && nl < int(l.lines.size())) {
            const auto& cur = l.lines[size_t(li)];
            const Vec2 at = cur.carets[size_t(std::clamp(ci - cur.first, 0, int(cur.carets.size()) - 1))];
            const auto& nxt = l.lines[size_t(nl)];
            const Vec2 d{nxt.carets[0].x - cur.carets[0].x, nxt.carets[0].y - cur.carets[0].y};
            moveTo(caretFromPoint(*page, *editedObj(), {at.x + d.x, at.y + d.y}));
        } else {
            moveTo(nl < 0 ? 0 : t.size());
        }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) { anchor_ = 0; caret_ = t.size(); caretMoved(); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_B, false)) toggleBold();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_I, false)) toggleItalic();
    if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_X, false)) && caret_ != anchor_) {
        const size_t a = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
        ImGui::SetClipboardText(t.substr(a, e - a).c_str());
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) eraseSelection();
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        if (const char* clip = ImGui::GetClipboardText()) {
            std::string c = clip;
            c.erase(std::remove(c.begin(), c.end(), '\r'), c.end());
            insertText(c);
        }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {   // cofnięcie całej bieżącej edycji
        Obj& e = *editedObj();
        e = editOrig_;
        editLinks_ = editLinks0_;
        caret_ = anchor_ = std::min(caret_, e.text.size());
        caretMoved();
    }
}

static bool idleImage(const Obj& o) {
    if (o.type == ObjType::PageShape) return o.shapeIdle();   // kształt z pliku tylko zaznaczony
    auto eq = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    return o.type == ObjType::PageImage && !o.hidden && !o.image && o.imgRot == 0 && eq(o.x, o.origBox.x) &&
           eq(o.y, o.origBox.y) && eq(o.w, o.origBox.w) && eq(o.h, o.origBox.h);
}

// Obrazy z pliku, które tylko zaznaczono (bez zmian), znikają z modelu po odznaczeniu.
void App::pruneIdleImages() {
    if (drag_ != Drag::None) return;
    for (Page& p : doc_.pages)
        for (int i = int(p.objs.size()) - 1; i >= 0; --i) {
            const Obj& o = p.objs[size_t(i)];
            const bool idle = idleImage(o) || (o.type == ObjType::Text && o.untouched() && !(editPage_ == p.id && editObj_ == i));
            if (!idle || (selPage_ == p.id && selObj_ == i)) continue;
            p.objs.erase(p.objs.begin() + i);
            if (selPage_ == p.id && selObj_ > i) --selObj_;
            if (editPage_ == p.id && editObj_ > i) --editObj_;
        }
}

// Obrót o 90° wokół środka (dir = 1: w prawo, -1: w lewo).
void App::cmdRotateImage(int dir) {
    Obj* o = selectedObj();
    if (!o || !isImage(*o)) return;
    snapshot();
    o = selectedObj();
    const float cx = o->x + o->w / 2, cy = o->y + o->h / 2;
    std::swap(o->w, o->h);
    o->x = cx - o->w / 2;
    o->y = cy - o->h / 2;
    o->imgRot = ((o->imgRot + dir) % 4 + 4) % 4;
}

void App::cmdReplaceImage() {
    Obj* o = selectedObj();
    if (!o || !isImage(*o)) return;
    const nfdu8filteritem_t filters[] = {{"Obrazy", "png,jpg,jpeg,bmp,gif,tga"}};
    nfdu8char_t* out = nullptr;
    if (NFD_OpenDialogU8(&out, filters, 1, nullptr) != NFD_OKAY) return;
    const std::string path = out;
    NFD_FreePathU8(out);
    std::string err;
    ImagePtr img = loadImageFile(path, err);
    if (!img) { toast(err, 5); return; }
    snapshot();
    o = selectedObj();
    o->image = img;
    // ta sama szerokość, wysokość z proporcji nowego obrazu (z uwzględnieniem obrotu), ten sam środek
    const float aspect = o->imgRot % 2 ? float(img->width) / float(img->height) : float(img->height) / float(img->width);
    const float cy = o->y + o->h / 2;
    o->h = o->w * aspect;
    o->y = cy - o->h / 2;
    toast("Obraz podmieniony.");
}

void App::cmdExportImage() {
    Obj* o = selectedObj();
    Page* p = doc_.pageById(selPage_);
    if (!o || !p || !isImage(*o)) return;
    const nfdu8filteritem_t filters[] = {{"Obraz PNG", "png"}};
    nfdu8char_t* out = nullptr;
    const std::string def = "obraz-strona-" + std::to_string(doc_.indexOfPage(p->id) + 1) + ".png";
    if (NFD_SaveDialogU8(&out, filters, 1, nullptr, def.c_str()) != NFD_OKAY) return;
    std::string path = out;
    NFD_FreePathU8(out);
    if (path.size() < 4 || (path.compare(path.size() - 4, 4, ".png") != 0 && path.compare(path.size() - 4, 4, ".PNG") != 0)) path += ".png";
    std::string err;
    bool ok;
    if (o->image) ok = writePng(path, o->image->width, o->image->height, o->image->rgba.data());   // wstawiony albo podmieniony
    else ok = doc_.exportImage(*p, *o, path, err);
    if (!ok) toast("Nie zapisano obrazu: " + (err.empty() ? std::string("błąd zapisu") : err), 5);
    else toast("Zapisano: " + std::filesystem::u8path(path).filename().u8string());
}

void App::drawImagePanel(Obj& o) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Obraz");
    ImGui::SameLine(0, 10);
    const float mmw = o.w * 25.4f / 72.f, mmh = o.h * 25.4f / 72.f;
    if (o.type == ObjType::PageImage && o.pixelW > 0 && !o.image) {
        const int pw = o.imgRot % 2 ? o.pixelH : o.pixelW;
        const float dpi = o.w > 0 ? pw / (o.w / 72.f) : 0.f;
        ImGui::Text("%.0f × %.0f mm · %d × %d px · %.0f dpi", mmw, mmh, o.pixelW, o.pixelH, dpi);
    } else {
        ImGui::Text("%.0f × %.0f mm", mmw, mmh);
    }
    auto tip = [](const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", t); };
    ImGui::SameLine(0, 14);
    if (ImGui::Button("↺ Obróć")) cmdRotateImage(-1);
    tip("Obróć o 90° w lewo");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("↻ Obróć")) cmdRotateImage(1);
    tip("Obróć o 90° w prawo");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("Podmień…")) cmdReplaceImage();
    tip("Wstaw inny obraz w to samo miejsce");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("Zapisz obraz…")) cmdExportImage();
    tip("Zapisz obraz jako plik PNG w pełnej rozdzielczości");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("Usuń")) cmdDeleteSelection();
    tip("Usuń obraz ze strony (Delete)");
}

// ------------------------------------------------------------------------------- kształty

// Kształt „liniowy”: bez konturu, wypełniony, najwyżej 6 pt grubości i co najmniej 8 razy dłuższy — jego grubość to ten wymiar.
static bool lineLike(const Obj& o) { return !o.strokeOn && o.fillMode != 0 && thinAxis(o) != 0; }

bool App::shapeHasThickness(const Obj& o) const { return o.strokeOn || lineLike(o); }

float App::shapeThickness(const Obj& o) const { return o.strokeOn ? o.size : std::min(o.w, o.h); }

// Zmiana wyglądu zaznaczonego kształtu; `step` — nowy krok cofania (przy suwaku tylko pierwszy).
void App::changeShape(const std::function<void(Obj&)>& fn, bool step) {
    Obj* o = selectedObj();
    if (!o || !isShape(*o)) return;
    if (step) snapshot();
    o = selectedObj();
    fn(*o);
    markDirty();
}

void App::setShapeThickness(float t, bool step) {
    changeShape([&](Obj& o) {
        t = std::clamp(t, 0.05f, 100.f);
        if (o.strokeOn) {
            o.size = t;
            o.shapeEdits |= kShapeWidth;
        } else if (lineLike(o)) {   // cienki wypełniony prostokąt: zmiana grubości wokół osi linii (do 6 pt — dalej to już pas)
            t = std::min({t, 6.f, std::max(o.w, o.h) / 8.f});
            if (thinAxis(o) == 1) { const float cy = o.y + o.h / 2; o.h = t; o.y = cy - t / 2; }
            else { const float cx = o.x + o.w / 2; o.w = t; o.x = cx - t / 2; }
        }
    }, step);
}

void App::drawShapePanel(Obj& o) {
    auto tip = [](const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", t); };
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled(lineLike(o) || (o.pts.size() == 2) ? "Linia" : "Kształt");
    ImGui::SameLine(0, 10);
    ImGui::Text("%.0f × %.0f mm", ptToMm(o.w), ptToMm(o.h));
    const float sw = ImGui::GetFrameHeight();

    // kolor z paletą i dowolnym wyborem; zwraca true przy zmianie
    auto colorChooser = [&](const char* id, const Color& cur, const std::function<void(const Color&, bool)>& set) {
        ImGui::PushID(id);
        if (ImGui::ColorButton("##c", toV4(cur), ImGuiColorEditFlags_NoTooltip, ImVec2(sw * 1.4f, sw))) ImGui::OpenPopup("##pal");
        if (ImGui::BeginPopup("##pal")) {
            for (int i = 0; i < int(IM_ARRAYSIZE(kPalette)); ++i) {
                if (i) ImGui::SameLine(0, 4);
                ImGui::PushID(i);
                if (ImGui::ColorButton("##sw", toV4(kPalette[i]), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(sw, sw))) {
                    set(kPalette[i], true);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            static const Color greys[] = {{1, 1, 1, 1}, {0.906f, 0.906f, 0.906f, 1}, {0.75f, 0.75f, 0.75f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.25f, 0.25f, 0.25f, 1}};
            for (int i = 0; i < 5; ++i) {
                if (i) ImGui::SameLine(0, 4);
                ImGui::PushID(100 + i);
                if (ImGui::ColorButton("##g", toV4(greys[i]), ImGuiColorEditFlags_NoTooltip, ImVec2(sw, sw))) {
                    set(greys[i], true);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            float c3[3] = {cur.r, cur.g, cur.b};
            if (ImGui::ColorPicker3("##pick", c3, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_DisplayHex)) {
                set({c3[0], c3[1], c3[2], 1}, !colorEditActive_);
                colorEditActive_ = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    };

    ImGui::SameLine(0, 16);
    bool fill = o.fillMode != 0;
    if (ImGui::Checkbox("Wypełnienie", &fill))
        changeShape([&](Obj& s) { s.fillMode = fill ? 1 : 0; s.shapeEdits |= kShapeFill; }, true);
    tip("Wypełnienie kształtu kolorem");
    ImGui::SameLine(0, 6);
    colorChooser("fill", o.color, [&](const Color& c, bool step) {
        changeShape([&](Obj& s) { s.color = c; if (!s.fillMode) s.fillMode = 1; s.shapeEdits |= kShapeFill; }, step);
    });
    tip("Kolor wypełnienia");

    ImGui::SameLine(0, 16);
    bool stroke = o.strokeOn;
    if (ImGui::Checkbox("Kontur", &stroke))
        changeShape([&](Obj& s) {
            s.strokeOn = stroke;
            if (stroke && s.size < 0.1f) s.size = 1.f;
            s.shapeEdits |= kShapeStroke | kShapeWidth;
        }, true);
    tip("Linia obrysu kształtu");
    ImGui::SameLine(0, 6);
    colorChooser("stroke", o.stroke, [&](const Color& c, bool step) {
        changeShape([&](Obj& s) {
            s.stroke = c;
            if (!s.strokeOn) { s.strokeOn = true; if (s.size < 0.1f) s.size = 1.f; s.shapeEdits |= kShapeWidth; }
            s.shapeEdits |= kShapeStroke;
        }, step);
    });
    tip("Kolor konturu");
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) colorEditActive_ = false;   // koniec wyboru koloru

    if (Obj* cur = selectedObj(); cur && shapeHasThickness(*cur)) {
        ImGui::SameLine(0, 16);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Grubość");
        ImGui::SameLine();
        float t = shapeThickness(*cur);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.5f);
        if (ImGui::SliderFloat("##thick", &t, 0.1f, cur->strokeOn ? 20.f : 6.f, "%.2f pt", ImGuiSliderFlags_Logarithmic)) {
            setShapeThickness(t, !sizeBeforeValid_);
            sizeBeforeValid_ = true;
        }
        if (ImGui::IsItemDeactivated()) sizeBeforeValid_ = false;
        tip(cur->strokeOn ? "Grubość konturu" : "Grubość linii");
    }
    ImGui::SameLine(0, 16);
    if (ImGui::Button("Usuń")) cmdDeleteSelection();
    tip("Usuń kształt ze strony (Delete)");
}

void App::cmdDeleteSelection() {
    Page* p = doc_.pageById(selPage_);
    if (!p || selObj_ < 0 || selObj_ >= int(p->objs.size())) return;
    if (editPage_ >= 0) { finishEdit(); return; }
    auto before = doc_.pages;
    Obj& o = p->objs[selObj_];
    if (o.type == ObjType::Text || isImage(o)) dropLinksIn(*p, objBounds(*p, o));   // odnośniki usuniętego elementu
    if (o.type == ObjType::Text && !o.srcObjs.empty()) o.text.clear();   // usuwa tekst z pliku
    else if (o.type == ObjType::PageImage || o.type == ObjType::PageShape) o.hidden = true;   // usuwa obraz albo kształt z pliku
    else p->objs.erase(p->objs.begin() + selObj_);
    selPage_ = selObj_ = -1;
    pushUndo(std::move(before));
}

// ------------------------------------------------------------------------------- pliki

void App::confirmDiscard(std::function<void()> action) {
    if (!dirty_) { action(); return; }
    pendingAction_ = std::move(action);
    discardOpen_ = true;
}

void App::cmdOpen(bool append) {
    const nfdu8filteritem_t filters[] = {{"Dokument PDF", "pdf"}};
    const nfdpathset_t* set = nullptr;
    if (NFD_OpenDialogMultipleU8(&set, filters, 1, nullptr) != NFD_OKAY) return;
    nfdpathsetsize_t n = 0;
    NFD_PathSet_GetCount(set, &n);
    std::vector<std::string> paths;
    for (nfdpathsetsize_t i = 0; i < n; ++i) {
        nfdu8char_t* path = nullptr;
        if (NFD_PathSet_GetPathU8(set, i, &path) == NFD_OKAY) { paths.emplace_back(path); NFD_PathSet_FreePathU8(path); }
    }
    NFD_PathSet_Free(set);
    if (paths.empty()) return;
    openPaths(paths, !append || doc_.empty());   // Otwórz: każdy plik w nowej karcie; Dołącz: do bieżącego dokumentu
}

void App::openPaths(const std::vector<std::string>& paths, bool newTabs) {
    for (const auto& p : paths) queue_.push_back({p, !newTabs, newTabs});
    processQueue();
}

// ------------------------------------------------------------------------------- karty

static bool samePath(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    if (std::filesystem::equivalent(std::filesystem::u8path(a), std::filesystem::u8path(b), ec)) return true;
    return !ec ? false : a == b;
}

bool App::tabDirty(int i) const { return i == activeTab_ ? dirty_ : tabs_[size_t(i)]->dirty; }

int App::findOpenTab(const std::string& path) const {
    for (int i = 0; i < int(tabs_.size()); ++i) {
        const bool active = i == activeTab_;
        const std::string& src = active ? srcPath_ : tabs_[size_t(i)]->srcPath;
        const std::string& sav = active ? savePath_ : tabs_[size_t(i)]->savePath;
        if (samePath(src, path) || samePath(sav, path)) return i;
    }
    return -1;
}

void App::freeTextures(std::unordered_map<int, Tex>& m) {
    for (auto& [k, t] : m) if (t.id) glDeleteTextures(1, &t.id);
    m.clear();
}

// Zamiana stanu aktywnego dokumentu ze schowkiem karty (wszystko, co dotyczy jednego dokumentu).
void App::swapTab(TabStore& t) {
    doc_.swap(t.doc);
    std::swap(docName_, t.name);
    std::swap(savePath_, t.savePath);
    std::swap(srcPath_, t.srcPath);
    std::swap(dirty_, t.dirty);
    std::swap(selPage_, t.selPage);
    std::swap(selObj_, t.selObj);
    undo_.swap(t.undo);
    redo_.swap(t.redo);
    std::swap(zoom_, t.zoom);
    std::swap(fitNext_, t.fitNext);
    std::swap(fitPageNext_, t.fitPageNext);
    pageTex_.swap(t.pageTex);
    thumbTex_.swap(t.thumbTex);
    std::swap(currentPage_, t.currentPage);
    std::swap(tabScrollX_, t.scrollX);
    std::swap(tabScrollY_, t.scrollY);
    std::swap(scrollToPage_, t.scrollToPage);
    std::swap(scrollToX_, t.scrollToX);
    std::swap(scrollToY_, t.scrollToY);
    const bool llOpen = ll_.open;   // okno listy odnośników zostaje otwarte (pokazuje nowy dokument)
    std::swap(ll_, t.ll);
    ll_.open = llOpen;
}

// Przed zmianą karty: koniec pisania i przeciągania, zamknięcie okienek związanych ze stronami.
void App::prepareTabSwitch() {
    if (editPage_ >= 0) finishEdit();
    drag_ = Drag::None;
    dragBefore_.clear();
    dragMoved_ = false;
    lk_ = LinkUi{};
    linkPress_ = {};
    hoverLinkPage_ = hoverLink_ = -1;
    flash_ = {};
    ctxPage_ = -1;
    pageOrigin_.clear();
    pageOps_.clear();
    overlapVer_ = 0;
    zoomAnchorPending_ = anchorWaitFrame_ = false;
    tabScrollX_ = prevScrollX_;
    tabScrollY_ = prevScrollY_;
}

void App::switchTab(int i) {
    if (i < 0 || i >= int(tabs_.size()) || i == activeTab_) return;
    prepareTabSwitch();
    swapTab(*tabs_[size_t(activeTab_)]);
    activeTab_ = i;
    swapTab(*tabs_[size_t(i)]);
    restoreScroll_ = true;
    tabSelectFrames_ = 2;
    updateTitle();
}

void App::newTab() {
    auto t = std::make_unique<TabStore>();
    t->uid = nextTabUid_++;
    tabs_.push_back(std::move(t));
    switchTab(int(tabs_.size()) - 1);
}

void App::closeTab(int i) {
    if (i < 0 || i >= int(tabs_.size())) return;
    if (tabs_.size() == 1) { cmdCloseDocument(); return; }   // ostatnia karta zostaje, pusta
    if (i == activeTab_) switchTab(i + 1 < int(tabs_.size()) ? i + 1 : i - 1);
    freeTextures(tabs_[size_t(i)]->pageTex);
    freeTextures(tabs_[size_t(i)]->thumbTex);
    tabs_.erase(tabs_.begin() + i);
    if (activeTab_ > i) --activeTab_;
    tabSelectFrames_ = 2;
    updateTitle();
}

void App::requestCloseTab(int i) {
    if (i < 0 || i >= int(tabs_.size())) return;
    if (!tabDirty(i)) { closeTab(i); return; }
    switchTab(i);   // pytanie dotyczy widocznego dokumentu
    confirmDiscard([this] { closeTab(activeTab_); });
}

void App::drawTabBar() {
    if (tabs_.size() < 2 && doc_.empty()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
    ImGui::BeginChild("##tabbar", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.90f, 0.92f, 0.93f, 1));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.97f, 0.97f, 0.98f, 1));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kAccent);
    const ImGuiTabBarFlags flags = ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll |
                                   ImGuiTabBarFlags_TabListPopupButton | ImGuiTabBarFlags_DrawSelectedOverline;
    if (ImGui::BeginTabBar("##docs", flags)) {
        for (int i = 0; i < int(tabs_.size()); ++i) {
            const bool active = i == activeTab_;
            const TabStore& t = *tabs_[size_t(i)];
            std::string name = active ? docName_ : t.name;
            const bool empty = active ? doc_.empty() : t.doc.empty();
            if (name.empty()) name = empty ? "Pusta karta" : "Nowy dokument";
            if (std::u32string u = utf8To32(name); u.size() > 30) name = utf32To8(u.substr(0, 28)) + "…";
            const std::string label = name + "###tab" + std::to_string(t.uid);
            ImGuiTabItemFlags f = 0;
            if (tabDirty(i)) f |= ImGuiTabItemFlags_UnsavedDocument;
            if (active && tabSelectFrames_ > 0) f |= ImGuiTabItemFlags_SetSelected;
            bool open = true;
            const bool sel = ImGui::BeginTabItem(label.c_str(), &open, f);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                const std::string& path = active ? (savePath_.empty() ? srcPath_ : savePath_) : (t.savePath.empty() ? t.srcPath : t.savePath);
                ImGui::SetTooltip("%s%s", path.empty() ? name.c_str() : path.c_str(), tabDirty(i) ? "\nniezapisane zmiany" : "");
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) closeTabReq_ = i;   // środkowy przycisk zamyka
            if (sel) {
                if (!active && tabSelectFrames_ == 0) wantTab_ = i;
                ImGui::EndTabItem();
            }
            if (!open) closeTabReq_ = i;
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) cmdOpen(false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Otwórz plik w nowej karcie (Ctrl+O)");
        ImGui::EndTabBar();
    }
    ImGui::PopStyleColor(4);
    ImGui::EndChild();
    if (tabSelectFrames_ > 0) --tabSelectFrames_;
}

void App::processQueue() {
    while (!queue_.empty() && !pwOpen_) {
        QueuedFile f = queue_.front();
        queue_.pop_front();
        if (f.newTab) {
            if (const int open = findOpenTab(f.path); open >= 0) {   // już otwarty: przejście do jego karty
                switchTab(open);
                toast(std::filesystem::u8path(f.path).filename().u8string() + " jest już otwarty.");
                continue;
            }
            if (!doc_.empty() || dirty_) newTab();   // pusta karta jest wykorzystywana
            if (editPage_ >= 0) finishEdit();
            undo_.clear();
            redo_.clear();
            selPage_ = selObj_ = -1;
            savePath_.clear();
            srcPath_ = f.path;
            docName_ = std::filesystem::u8path(f.path).filename().u8string();
            dirty_ = false;
            fitNext_ = true;
        }
        const auto before = doc_.pages;
        const bool wasEmpty = doc_.empty();
        std::string err;
        const LoadResult r = doc_.appendFile(f.path, "", err);
        if (r == LoadResult::NeedPassword) {
            pwPath_ = f.path;
            pwAppend_ = !wasEmpty;
            pwBuf_.clear();
            pwMsg_.clear();
            pwOpen_ = true;
            break;
        }
        const std::string name = std::filesystem::u8path(f.path).filename().u8string();
        if (r != LoadResult::Ok) {
            toast(name + ": " + err, 5);
            settings_.removeRecent(f.path);
            settings_.save();
            if (f.newTab && doc_.empty()) {   // nieudane otwarcie: pusta karta znika
                if (tabs_.size() > 1) closeTab(activeTab_);
                else { docName_.clear(); srcPath_.clear(); }
            }
            continue;
        }
        settings_.addRecent(f.path);
        settings_.save();
        if (wasEmpty) {
            if (docName_.empty()) docName_ = name;
            fitNext_ = true;
        } else {
            pushUndo(before);
            toast("Dołączono: " + name);
        }
    }
    updateTitle();
}

void App::cmdSave(bool saveAs) {
    if (doc_.empty()) return;
    if (editPage_ >= 0) finishEdit();
    std::string path = savePath_;
    if (saveAs || path.empty()) {
        std::string base = docName_.empty() ? std::string("dokument") : docName_;
        if (base.size() > 4 && isPdf(base)) base.resize(base.size() - 4);
        const std::string def = base + (savePath_.empty() ? "-edytowany.pdf" : ".pdf");
        const nfdu8filteritem_t filters[] = {{"Dokument PDF", "pdf"}};
        nfdu8char_t* out = nullptr;
        if (NFD_SaveDialogU8(&out, filters, 1, nullptr, def.c_str()) != NFD_OKAY) return;
        path = out;
        NFD_FreePathU8(out);
        if (!isPdf(path)) path += ".pdf";
    }
    std::string err;
    if (!doc_.saveToFile(path, err)) { toast("Nie zapisano: " + err, 6); return; }
    savePath_ = path;
    docName_ = std::filesystem::u8path(path).filename().u8string();
    dirty_ = false;
    updateTitle();
    settings_.addRecent(path);
    settings_.save();
    toast("Zapisano: " + docName_);
}

void App::cmdInsertImage() {
    if (doc_.empty()) return;
    const nfdu8filteritem_t filters[] = {{"Obrazy", "png,jpg,jpeg,bmp,gif,tga"}};
    nfdu8char_t* out = nullptr;
    if (NFD_OpenDialogU8(&out, filters, 1, nullptr) != NFD_OKAY) return;
    std::string path = out;
    NFD_FreePathU8(out);
    const int idx = std::clamp(currentPage_, 0, int(doc_.pages.size()) - 1);
    insertImage(path, doc_.pages[idx].id);
}

void App::insertImage(const std::string& path, int pageId, const Vec2* at) {
    std::string err;
    ImagePtr img = loadImageFile(path, err);
    if (!img) { toast(err, 5); return; }
    Page* p = doc_.pageById(pageId);
    if (!p) return;
    auto before = doc_.pages;
    Obj o;
    o.type = ObjType::Image;
    o.image = img;
    o.w = std::min(p->width * 0.4f, img->width * 0.75f);   // 96 dpi -> pt
    o.h = o.w * img->height / std::max(1, img->width);
    o.x = (p->width - o.w) / 2;
    o.y = std::min((p->height - o.h) / 2, p->height * 0.3f);
    if (at) { o.x = at->x - o.w / 2; o.y = at->y - o.h / 2; }
    p->objs.push_back(o);
    pushUndo(std::move(before));
    setTool(Tool::Select);
    selPage_ = pageId;
    selObj_ = int(p->objs.size()) - 1;
    toast("Obraz wstawiony. Przeciągnij, aby przesunąć; róg zmienia rozmiar.");
}

void App::onDrop(const std::vector<std::string>& paths) {
    std::vector<std::string> pdfs;
    for (const auto& p : paths) {
        if (isPdf(p)) pdfs.push_back(p);
        else if (isImage(p) && !doc_.empty()) insertImage(p, doc_.pages[std::clamp(currentPage_, 0, int(doc_.pages.size()) - 1)].id);
    }
    // PDF upuszczony na okno: nowa karta; z wciśniętym Ctrl — dołączenie stron do bieżącego dokumentu
    const bool ctrl = glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    if (!pdfs.empty()) openPaths(pdfs, !(ctrl && !doc_.empty()));
}

void App::onCloseRequested() {
    bool any = false;
    for (int i = 0; i < int(tabs_.size()); ++i) any |= tabDirty(i);
    if (any) { closeOpen_ = true; glfwSetWindowShouldClose(window_, GLFW_FALSE); }
    else quit_ = true;
}

void App::cmdBlankPage(int refPageId, int where) {
    if (editPage_ >= 0) finishEdit();
    if (refPageId < 0 && !doc_.empty()) refPageId = doc_.pages[size_t(std::clamp(currentPage_, 0, int(doc_.pages.size()) - 1))].id;
    np_.refPage = refPageId;
    np_.where = doc_.empty() ? 3 : where;
    np_.request = true;
}

void App::insertNewPages() {
    const float wPt = mmToPt(np_.landscape ? std::max(np_.wMm, np_.hMm) : std::min(np_.wMm, np_.hMm));
    const float hPt = mmToPt(np_.landscape ? std::min(np_.wMm, np_.hMm) : std::max(np_.wMm, np_.hMm));
    const bool wasEmpty = doc_.empty();
    auto before = doc_.pages;
    int at = int(doc_.pages.size());
    const int ref = doc_.indexOfPage(np_.refPage);
    switch (np_.where) {
        case 0: at = ref >= 0 ? ref + 1 : at; break;
        case 1: at = ref >= 0 ? ref : 0; break;
        case 2: at = 0; break;
        default: break;
    }
    int first = -1;
    for (int k = 0; k < std::clamp(np_.count, 1, 500); ++k) {
        const int id = doc_.insertBlankPage(at + k, wPt, hPt);
        if (first < 0) first = id;
    }
    if (wasEmpty) {
        docName_ = "nowy.pdf";
        fitNext_ = true;
        dirty_ = true;
        updateTitle();
    } else {
        pushUndo(std::move(before));
    }
    scrollToPage_ = first;
    const int n = std::clamp(np_.count, 1, 500);
    toast(n == 1 ? "Dodano pustą stronę " + paperName(wPt, hPt) + "."
                 : "Dodano " + std::to_string(n) + " pustych stron " + paperName(wPt, hPt) + ".");
}

void App::drawNewPageDialog() {
    if (np_.request) {
        // rozmiar „jak bieżąca strona”, gdy jest strona odniesienia
        if (np_.preset == -1) {
            if (const Page* rp = doc_.pageById(np_.refPage)) {
                np_.wMm = ptToMm(std::min(rp->shownWidth(), rp->shownHeight()));
                np_.hMm = ptToMm(std::max(rp->shownWidth(), rp->shownHeight()));
                np_.landscape = rp->shownWidth() > rp->shownHeight();
            } else {
                np_.preset = 1;
            }
        }
        ImGui::OpenPopup("Nowa pusta strona");
        np_.request = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Nowa pusta strona", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;

    const float fs = ImGui::GetFontSize();
    const Page* refP = doc_.pageById(np_.refPage);
    auto pick = [&](int i) {
        np_.preset = i;
        if (i >= 0) {
            np_.wMm = kPaperSizes[i].wMm;
            np_.hMm = kPaperSizes[i].hMm;
            np_.landscape = kPaperSizes[i].landscape;
        } else if (i == -1 && refP) {
            np_.wMm = ptToMm(std::min(refP->shownWidth(), refP->shownHeight()));
            np_.hMm = ptToMm(std::max(refP->shownWidth(), refP->shownHeight()));
            np_.landscape = refP->shownWidth() > refP->shownHeight();
        }
    };

    // ---- lewa kolumna: formaty
    ImGui::BeginChild("##sizes", ImVec2(fs * 17.5f, fs * 24), ImGuiChildFlags_Borders);
    if (refP) {
        std::string lbl = "Jak bieżąca strona (" + paperName(refP->shownWidth(), refP->shownHeight()) + ")";
        if (ImGui::Selectable(lbl.c_str(), np_.preset == -1)) pick(-1);
    }
    const char* group = "";
    for (int i = 0; i < kPaperSizeCount; ++i) {
        const PaperSize& ps = kPaperSizes[i];
        if (std::string(group) != ps.group) {
            group = ps.group;
            ImGui::Spacing();
            ImGui::TextDisabled("%s", group);
        }
        char lbl[96];
        std::snprintf(lbl, sizeof lbl, "%-12s##%d", ps.name, i);
        ImGui::PushID(i);
        if (ImGui::Selectable(lbl, np_.preset == i)) pick(i);
        ImGui::SameLine(fs * 7.2f);
        ImGui::TextDisabled("%g × %g mm", std::round(ps.wMm * 10) / 10, std::round(ps.hMm * 10) / 10);
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Selectable("Własny rozmiar…", np_.preset == -2)) np_.preset = -2;
    ImGui::EndChild();

    ImGui::SameLine(0, fs);
    ImGui::BeginGroup();
    // ---- orientacja
    ImGui::TextDisabled("Orientacja");
    auto toggle = [&](const char* label, bool on) {
        if (on) { ImGui::PushStyleColor(ImGuiCol_Button, kInk); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kInk); ImGui::PushStyleColor(ImGuiCol_Text, kBar); }
        const bool c = ImGui::Button(label, ImVec2(fs * 6.5f, 0));
        if (on) ImGui::PopStyleColor(3);
        return c;
    };
    if (toggle("Pionowa", !np_.landscape)) np_.landscape = false;
    ImGui::SameLine(0, 4);
    if (toggle("Pozioma", np_.landscape)) np_.landscape = true;

    // ---- wymiary (edycja przełącza na własny rozmiar)
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("Wymiary");
    static const char* units[] = {"mm", "cm", "cale", "pt"};
    static const float perMm[] = {1.f, 0.1f, 1.f / 25.4f, 72.f / 25.4f};
    const float k = perMm[np_.unit];
    float w = (np_.landscape ? std::max(np_.wMm, np_.hMm) : std::min(np_.wMm, np_.hMm)) * k;
    float h = (np_.landscape ? std::min(np_.wMm, np_.hMm) : std::max(np_.wMm, np_.hMm)) * k;
    const char* fmt = np_.unit == 0 || np_.unit == 3 ? "%.1f" : "%.2f";
    const float lo = ptToMm(kMinPagePt) * k, hi = ptToMm(kMaxPagePt) * k;
    ImGui::SetNextItemWidth(fs * 5.5f);
    bool changed = ImGui::InputFloat("##w", &w, 0, 0, fmt);
    ImGui::SameLine(0, 4);
    ImGui::TextUnformatted("×");
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(fs * 5.5f);
    changed |= ImGui::InputFloat("##h", &h, 0, 0, fmt);
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(fs * 4.5f);
    ImGui::Combo("##unit", &np_.unit, units, IM_ARRAYSIZE(units));
    if (changed) {
        w = std::clamp(w, lo, hi);
        h = std::clamp(h, lo, hi);
        np_.preset = -2;
        np_.landscape = w > h;
        np_.wMm = std::min(w, h) / k;
        np_.hMm = std::max(w, h) / k;
    }
    const float wPt = mmToPt(np_.landscape ? std::max(np_.wMm, np_.hMm) : std::min(np_.wMm, np_.hMm));
    const float hPt = mmToPt(np_.landscape ? std::min(np_.wMm, np_.hMm) : std::max(np_.wMm, np_.hMm));
    ImGui::TextDisabled("%s · %.0f × %.0f pt", paperName(wPt, hPt).c_str(), wPt, hPt);

    // ---- liczba i miejsce
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("Liczba stron");
    ImGui::SetNextItemWidth(fs * 7);
    if (ImGui::InputInt("##count", &np_.count)) np_.count = std::clamp(np_.count, 1, 500);
    if (!doc_.empty()) {
        ImGui::Dummy(ImVec2(0, 4));
        const int refIdx = doc_.indexOfPage(np_.refPage);
        ImGui::TextDisabled("Miejsce");
        char a[48], b[48];
        std::snprintf(a, sizeof a, "Za stroną %d", refIdx + 1);
        std::snprintf(b, sizeof b, "Przed stroną %d", refIdx + 1);
        if (refIdx >= 0) {
            ImGui::RadioButton(a, &np_.where, 0);
            ImGui::RadioButton(b, &np_.where, 1);
        } else if (np_.where < 2) {
            np_.where = 3;
        }
        ImGui::RadioButton("Na początku dokumentu", &np_.where, 2);
        ImGui::RadioButton("Na końcu dokumentu", &np_.where, 3);
    }
    ImGui::EndGroup();

    // ---- podgląd proporcji
    ImGui::SameLine(0, fs);
    ImGui::BeginGroup();
    {
        const float box = fs * 9;
        const float sc = box / std::max(wPt, hPt);
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(box, box));
        const float pw = wPt * sc, ph = hPt * sc;
        const ImVec2 a(c0.x + (box - pw) / 2, c0.y + (box - ph) / 2), b(a.x + pw, a.y + ph);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(a.x + 2, a.y + 3), ImVec2(b.x + 2, b.y + 3), IM_COL32(20, 30, 40, 40));
        dl->AddRectFilled(a, b, IM_COL32_WHITE);
        dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border));
        // porównanie z A4 w tej samej skali
        const float a4w = mmToPt(np_.landscape ? 297.f : 210.f) * sc, a4h = mmToPt(np_.landscape ? 210.f : 297.f) * sc;
        if (a4w < box * 1.6f && a4h < box * 1.6f) {
            const ImVec2 q(c0.x + (box - a4w) / 2, c0.y + (box - a4h) / 2);
            dl->PushClipRect(c0, ImVec2(c0.x + box, c0.y + box), true);
            for (float t = 0; t < 1.f; t += 0.04f) {   // przerywany obrys A4
                auto L = [&](ImVec2 p0, ImVec2 p1) { dl->AddLine(ImVec2(p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t),
                                                                 ImVec2(p0.x + (p1.x - p0.x) * (t + 0.02f), p0.y + (p1.y - p0.y) * (t + 0.02f)), col32(kAccent, 0.6f)); };
                L(q, ImVec2(q.x + a4w, q.y)); L(ImVec2(q.x + a4w, q.y), ImVec2(q.x + a4w, q.y + a4h));
                L(ImVec2(q.x + a4w, q.y + a4h), ImVec2(q.x, q.y + a4h)); L(ImVec2(q.x, q.y + a4h), q);
            }
            dl->PopClipRect();
        }
        const char* note = "linia przerywana: A4";
        ImGui::SetCursorScreenPos(ImVec2(c0.x, c0.y + box + 4));
        ImGui::PushTextWrapPos(c0.x + box);
        ImGui::TextDisabled("%s", note);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.83f, 0.27f, 0.22f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool go = ImGui::Button(np_.count > 1 ? "Wstaw strony" : "Wstaw stronę") || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Anuluj") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (go) { insertNewPages(); ImGui::CloseCurrentPopup(); }
    else if (cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::pageOp(int pageId, int op) {
    const int i = doc_.indexOfPage(pageId);
    if (i < 0) return;
    auto before = doc_.pages;
    auto& v = doc_.pages;
    if (op == 0) v[i].extraRot = (v[i].extraRot + 1) % 4;
    else if (op == 4) v[i].extraRot = (v[i].extraRot + 3) % 4;
    else if (op == 5 && i > 0) { Page m = std::move(v[i]); v.erase(v.begin() + i); v.insert(v.begin(), std::move(m)); }
    else if (op == 6 && i + 1 < int(v.size())) { Page m = std::move(v[i]); v.erase(v.begin() + i); v.push_back(std::move(m)); }
    else if (op == 1 && i > 0) std::swap(v[i], v[i - 1]);
    else if (op == 2 && i + 1 < int(v.size())) std::swap(v[i], v[i + 1]);
    else if (op == 3) {
        if (v.size() == 1) { toast("To jedyna strona — dokument musi mieć co najmniej jedną."); return; }
        if (selPage_ == pageId) selPage_ = selObj_ = -1;
        if (editPage_ == pageId) { editPage_ = editObj_ = -1; editBefore_.clear(); }
        v.erase(v.begin() + i);
        toast("Usunięto stronę " + std::to_string(i + 1) + ". Ctrl+Z przywraca.");
    } else if (op >= 100) {           // przeniesienie przed stronę o indeksie op-100
        int to = op - 100;
        if (to == i || to == i + 1) return;
        Page moved = std::move(v[i]);
        v.erase(v.begin() + i);
        if (to > i) --to;
        v.insert(v.begin() + to, std::move(moved));
    } else return;
    pushUndo(std::move(before));
}

// ------------------------------------------------------------------------------- klatka

void App::frame() {
    ++frameNo_;
    pendingRender_ = false;
    {
        int ww, wh, fw, fh;
        glfwGetWindowSize(window_, &ww, &wh);
        glfwGetFramebufferSize(window_, &fw, &fh);
        fbScale_ = ww > 0 ? float(fw) / ww : 1.f;
    }
    handleShortcuts();
    pruneIdleImages();
    if (editPage_ >= 0) updateEditLinks();
    if (Obj* eo = editedObj()) {
        const Page& ep = *doc_.pageById(editPage_);
        const uint64_t v = doc_.contentVersion(ep) ^ (uint64_t(editObj_) << 48);
        if (v != overlapVer_) { overlapVer_ = v; overlap_ = doc_.overlapsOtherText(ep, *eo); }
    } else {
        overlap_ = false;
        overlapVer_ = 0;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9, 5));   // wysokość paska menu
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    drawMenuBar();
    ImGui::PopStyleVar(2);
    drawToolbar();
    drawTabBar();
    const float statusH = ImGui::GetFrameHeight() + 6;
    const float thumbsW = ImGui::GetFontSize() * 11.5f;
    if (!doc_.empty() && settings_.showThumbs && !settings_.thumbsAutoHide) {
        drawThumbs(thumbsW, false);
        ImGui::SameLine(0, 0);
    }
    drawCanvas();
    (void)statusH;
    drawStatusBar();
    ImGui::End();
    if (!doc_.empty() && settings_.showThumbs && settings_.thumbsAutoHide) drawThumbsOverlay(thumbsW);
    else { thumbsOpen_ = false; thumbsAnim_ = 0; }

    drawModals();
    drawNewPageDialog();
    drawHelpWindows();
    drawLinkList();
    if (ctxRequest_) { ImGui::OpenPopup("##ctx"); ctxRequest_ = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 5));
    if (ImGui::BeginPopup("##ctx")) { drawContextMenu(); ImGui::EndPopup(); }
    if (lk_.request) { ImGui::OpenPopup("##link"); lk_.request = false; }
    ImGui::SetNextWindowPos(lk_.anchor, ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    linkPopupOpen_ = ImGui::BeginPopup("##link", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    if (linkPopupOpen_) { drawLinkPopup(); ImGui::EndPopup(); }
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);

    // komunikat
    if (!toast_.empty()) {
        if (ImGui::GetTime() > toastUntil_) toast_.clear();
        else {
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - statusH - 16),
                                    ImGuiCond_Always, ImVec2(0.5f, 1.f));
            ImGui::SetNextWindowBgAlpha(0.95f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, kInk);
            ImGui::PushStyleColor(ImGuiCol_Text, kBar);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 9));
            ImGui::Begin("##toast", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextUnformatted(toast_.c_str());
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }
    }

    for (auto [id, op] : pageOps_) pageOp(id, op);
    pageOps_.clear();
    // zmiany kart na końcu klatki (nic nie jest wtedy w trakcie rysowania)
    if (wantTab_ >= 0) { const int t = wantTab_; wantTab_ = -1; switchTab(t); }
    if (closeTabReq_ >= 0) { const int t = closeTabReq_; closeTabReq_ = -1; requestCloseTab(t); }
    evictTextures();
}

void App::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (pwOpen_ || discardOpen_ || closeOpen_) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) cmdOpen(false);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O)) cmdOpen(true);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) cmdSave(false);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N)) cmdBlankPage();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) cmdSave(true);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) cmdNewDocument();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W) && (!doc_.empty() || tabs_.size() > 1)) requestCloseTab(activeTab_);
    // karty: Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+PgDn / Ctrl+PgUp
    if (tabs_.size() > 1) {
        const int n = int(tabs_.size());
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Tab) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageDown))
            wantTab_ = (activeTab_ + 1) % n;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageUp))
            wantTab_ = (activeTab_ + n - 1) % n;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) onCloseRequested();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) showShortcuts_ = !showShortcuts_;
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) {
        settings_.showThumbs = !settings_.showThumbs;
        settings_.save();
        if (settings_.showThumbs && settings_.thumbsAutoHide) { thumbsOpen_ = true; thumbsLeaveT_ = ImGui::GetTime() + 1.0; }   // pokaż na chwilę
    }
    if (io.WantTextInput) return;   // pisanie w polu tekstowym ma pierwszeństwo
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K)) { cmdAddLink(); return; }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_L) && !doc_.empty()) { ll_.open = !ll_.open; return; }
    if (editPage_ >= 0) { handleTextKeys(); return; }   // pisanie na stronie
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) redo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) cmdCopy(false);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) cmdCopy(true);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) cmdDuplicate();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V) && !doc_.empty())
        cmdPaste(selPage_ >= 0 ? selPage_ : doc_.pages[size_t(std::clamp(currentPage_, 0, int(doc_.pages.size()) - 1))].id, nullptr);
    if (Obj* so = selectedObj(); so && so->type == ObjType::Text) {   // formatowanie całego zaznaczonego tekstu
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) toggleBold();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_I)) toggleItalic();
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_I)) cmdInsertImage();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Equal) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd)) setZoom(zoom_ * 1.2f);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Minus) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract)) setZoom(zoom_ / 1.2f);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_0)) fitNext_ = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_1)) setZoom(1.f);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_2)) fitPageNext_ = true;
    if (!doc_.empty()) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) { goToValue_ = currentPage_ + 1; goToOpen_ = true; }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) pageOps_.push_back({currentPageId(), 0});
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_R)) pageOps_.push_back({currentPageId(), 4});
    }
    if (io.KeyCtrl || io.KeySuper || io.KeyAlt) return;
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) setTool(Tool::Select);
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) setTool(Tool::Text);
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) setTool(Tool::Pen);
    if (ImGui::IsKeyPressed(ImGuiKey_H, false)) setTool(Tool::Highlight);
    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) setTool(Tool::Whiteout);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) selPage_ = selObj_ = -1;
    if (!doc_.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) goToPage(currentPage_ + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) goToPage(currentPage_ - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) goToPage(0);
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) goToPage(int(doc_.pages.size()) - 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) cmdDeleteSelection();
    if (Obj* o = selectedObj()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && o->type == ObjType::Text) { startEdit(selPage_, selObj_, false); return; }
        const float step = io.KeyShift ? 10.f : 1.f;
        float dx = 0, dy = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx = step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy = -step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy = step;
        if (dx != 0 || dy != 0) {
            // strzałki działają w układzie ekranu — przelicz na układ strony
            const Page& p = *doc_.pageById(selPage_);
            const Vec2 a = shownToBase(p, {0, 0}), c = shownToBase(p, {dx, dy});
            const float bx = c.x - a.x, by = c.y - a.y;
            snapshot();
            o = selectedObj();
            const kpdf::Rect old = objBounds(p, *o);
            if (o->type == ObjType::Ink) for (auto& q : o->pts) { q.x += bx; q.y += by; }
            else { o->x += bx; o->y += by; }
            if (o->type == ObjType::Text || isImage(*o)) moveLinksWith(*doc_.pageById(selPage_), old, bx, by);
        }
    }
}

// ------------------------------------------------------------------------------- menu główne

int App::currentPageId() const {
    if (doc_.empty()) return -1;
    return doc_.pages[size_t(std::clamp(currentPage_, 0, int(doc_.pages.size()) - 1))].id;
}

void App::goToPage(int index) {
    if (doc_.empty()) return;
    index = std::clamp(index, 0, int(doc_.pages.size()) - 1);
    scrollToPage_ = doc_.pages[size_t(index)].id;
    scrollToY_ = -1;
}

void App::cmdNewDocument() {
    if (!doc_.empty() || dirty_) newTab();   // nowy dokument w nowej karcie
    cmdBlankPage();
}

void App::cmdCloseDocument() {
    if (editPage_ >= 0) { editPage_ = editObj_ = -1; editBefore_.clear(); }
    doc_.clear();
    dropTextures();
    undo_.clear();
    redo_.clear();
    selPage_ = selObj_ = -1;
    lk_ = LinkUi{};
    linkPress_ = {};
    ll_.sig = 0;
    ll_.text.clear();
    ll_.expanded.clear();
    savePath_.clear();
    srcPath_.clear();
    docName_.clear();
    dirty_ = false;
    fitNext_ = true;
    updateTitle();
}

void App::cmdExportPagePng(float dpi) {
    const int id = currentPageId();
    Page* p = doc_.pageById(id);
    if (!p) return;
    if (editPage_ >= 0) finishEdit();
    std::string base = docName_.empty() ? std::string("strona") : docName_;
    if (base.size() > 4 && isPdf(base)) base.resize(base.size() - 4);
    const std::string def = base + "-strona-" + std::to_string(doc_.indexOfPage(id) + 1) + ".png";
    const nfdu8filteritem_t filters[] = {{"Obraz PNG", "png"}};
    nfdu8char_t* out = nullptr;
    if (NFD_SaveDialogU8(&out, filters, 1, nullptr, def.c_str()) != NFD_OKAY) return;
    std::string path = out;
    NFD_FreePathU8(out);
    if (path.size() < 4 || (path.compare(path.size() - 4, 4, ".png") != 0 && path.compare(path.size() - 4, 4, ".PNG") != 0)) path += ".png";
    // rozmiar ograniczony do 12000 px na dłuższym boku
    float scale = dpi / 72.f;
    scale = std::min(scale, 12000.f / std::max(p->shownWidth(), p->shownHeight()));
    std::vector<uint8_t> rgba;
    int w, h;
    if (!doc_.render(*p, scale, rgba, w, h)) { toast("Nie udało się wyrenderować strony."); return; }
    // nakładki (pióro, zakreślacz, wstawione obrazy) są w pliku dopiero po zapisie — eksport pokazuje treść strony
    if (!writePng(path, w, h, rgba.data())) { toast("Nie można zapisać pliku w tym miejscu.", 5); return; }
    toast("Zapisano " + std::to_string(w) + " × " + std::to_string(h) + " px: " + std::filesystem::u8path(path).filename().u8string());
}

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    const bool hasDoc = !doc_.empty();
    const bool editing = editPage_ >= 0;
    Obj* sel = selectedObj();

    if (ImGui::BeginMenu("Plik")) {
        if (ImGui::MenuItem("Nowy dokument…", "Ctrl+N")) cmdNewDocument();
        if (ImGui::MenuItem("Otwórz…", "Ctrl+O")) cmdOpen(false);
        if (ImGui::BeginMenu("Otwórz ostatni", !settings_.recent.empty())) {
            std::string toOpen;
            for (const auto& r : settings_.recent) {
                const std::string name = std::filesystem::u8path(r).filename().u8string();
                if (ImGui::MenuItem(name.c_str())) toOpen = r;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", r.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Wyczyść listę")) { settings_.recent.clear(); settings_.save(); }
            ImGui::EndMenu();
            if (!toOpen.empty()) openPaths({toOpen}, true);
        }
        if (ImGui::MenuItem("Dołącz strony z pliku…", "Ctrl+Shift+O", false, hasDoc)) cmdOpen(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Zapisz", "Ctrl+S", false, hasDoc)) cmdSave(false);
        if (ImGui::MenuItem("Zapisz jako…", "Ctrl+Shift+S", false, hasDoc)) cmdSave(true);
        if (ImGui::BeginMenu("Eksportuj bieżącą stronę jako PNG", hasDoc)) {
            if (ImGui::MenuItem("72 dpi (ekran)")) cmdExportPagePng(72);
            if (ImGui::MenuItem("150 dpi")) cmdExportPagePng(150);
            if (ImGui::MenuItem("300 dpi (druk)")) cmdExportPagePng(300);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Zamknij kartę", "Ctrl+W", false, hasDoc || tabs_.size() > 1)) requestCloseTab(activeTab_);
        if (tabs_.size() > 1 && ImGui::MenuItem("Zamknij pozostałe karty")) {
            for (int i = int(tabs_.size()) - 1; i >= 0; --i)
                if (i != activeTab_ && !tabDirty(i)) closeTab(i);
            if (tabs_.size() > 1) toast("Karty z niezapisanymi zmianami zostały otwarte.", 4);
        }
        if (ImGui::MenuItem("Zakończ", "Ctrl+Q")) onCloseRequested();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edycja")) {
        if (ImGui::MenuItem("Cofnij", "Ctrl+Z", false, !undo_.empty() || editing)) undo();
        if (ImGui::MenuItem("Ponów", "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        if (editing) {
            Obj* o = editedObj();
            const bool hasSel = caret_ != anchor_;
            if (ImGui::MenuItem("Wytnij", "Ctrl+X", false, hasSel)) {
                const size_t a = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
                ImGui::SetClipboardText(o->text.substr(a, e - a).c_str());
                eraseSelection();
            }
            if (ImGui::MenuItem("Kopiuj", "Ctrl+C", false, hasSel)) {
                const size_t a = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
                ImGui::SetClipboardText(o->text.substr(a, e - a).c_str());
            }
            if (ImGui::MenuItem("Wklej", "Ctrl+V")) {
                if (const char* c = ImGui::GetClipboardText()) {
                    std::string t = c;
                    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
                    insertText(t);
                }
            }
            if (ImGui::MenuItem("Zaznacz wszystko", "Ctrl+A")) { anchor_ = 0; caret_ = o->text.size(); caretMoved(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Zakończ edycję tekstu", "Esc")) finishEdit();
        } else {
            if (ImGui::MenuItem("Wytnij", "Ctrl+X", false, sel != nullptr)) cmdCopy(true);
            if (ImGui::MenuItem("Kopiuj", "Ctrl+C", false, sel != nullptr)) cmdCopy(false);
            const bool haveClip = !clip_.empty() || (ImGui::GetClipboardText() && *ImGui::GetClipboardText());
            if (ImGui::MenuItem("Wklej", "Ctrl+V", false, hasDoc && haveClip)) cmdPaste(selPage_ >= 0 ? selPage_ : currentPageId(), nullptr);
            if (ImGui::MenuItem("Duplikuj", "Ctrl+D", false, sel != nullptr)) cmdDuplicate();
            if (ImGui::MenuItem("Usuń", "Delete", false, sel != nullptr)) cmdDeleteSelection();
            ImGui::Separator();
            if (ImGui::MenuItem("Edytuj tekst", "Enter", false, sel && sel->type == ObjType::Text)) startEdit(selPage_, selObj_, false);
            const bool fromFile = sel && !sel->srcObjs.empty() && !(sel->type == ObjType::Text && sel->untouched()) && !idleImage(*sel);
            if (ImGui::MenuItem("Przywróć oryginał zaznaczenia", nullptr, false, fromFile)) cmdRestoreOriginal();
            if (ImGui::MenuItem("Odznacz", "Esc", false, sel != nullptr)) selPage_ = selObj_ = -1;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Widok")) {
        if (ImGui::MenuItem("Powiększ", "Ctrl+=", false, hasDoc)) setZoom(zoom_ * 1.2f);
        if (ImGui::MenuItem("Pomniejsz", "Ctrl+−", false, hasDoc)) setZoom(zoom_ / 1.2f);
        if (ImGui::MenuItem("100%", "Ctrl+1", false, hasDoc)) setZoom(1.f);
        if (ImGui::MenuItem("Dopasuj do szerokości", "Ctrl+0", false, hasDoc)) fitNext_ = true;
        if (ImGui::MenuItem("Cała strona", "Ctrl+2", false, hasDoc)) fitPageNext_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Pierwsza strona", "Home", false, hasDoc)) goToPage(0);
        if (ImGui::MenuItem("Poprzednia strona", "PgUp", false, hasDoc && currentPage_ > 0)) goToPage(currentPage_ - 1);
        if (ImGui::MenuItem("Następna strona", "PgDn", false, hasDoc && currentPage_ + 1 < int(doc_.pages.size()))) goToPage(currentPage_ + 1);
        if (ImGui::MenuItem("Ostatnia strona", "End", false, hasDoc)) goToPage(int(doc_.pages.size()) - 1);
        if (ImGui::MenuItem("Przejdź do strony…", "Ctrl+G", false, hasDoc)) { goToValue_ = currentPage_ + 1; goToOpen_ = true; }
        ImGui::Separator();
        if (ImGui::MenuItem("Miniatury stron", "F4", settings_.showThumbs)) { settings_.showThumbs = !settings_.showThumbs; settings_.save(); }
        if (ImGui::MenuItem("Automatyczne ukrywanie miniatur", nullptr, settings_.thumbsAutoHide, settings_.showThumbs)) {
            settings_.thumbsAutoHide = !settings_.thumbsAutoHide;
            settings_.save();
        }
        if (ImGui::MenuItem("Pasek właściwości", nullptr, settings_.showProps)) { settings_.showProps = !settings_.showProps; settings_.save(); }
        if (ImGui::MenuItem("Podpisy przy ikonach", nullptr, settings_.iconLabels)) { settings_.iconLabels = !settings_.iconLabels; settings_.save(); }
        if (ImGui::MenuItem("Obramowanie odnośników", nullptr, settings_.showLinks)) { settings_.showLinks = !settings_.showLinks; settings_.save(); }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Wstaw", hasDoc)) {
        if (ImGui::MenuItem("Tekst", "T")) setTool(Tool::Text);
        if (ImGui::MenuItem("Obraz…", "Ctrl+I")) cmdInsertImage();
        if (ImGui::MenuItem("Odnośnik…", "Ctrl+K", false, editing ? caret_ != anchor_ : sel != nullptr)) cmdAddLink();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Zaznacz tekst (albo fragment edytowanego tekstu) lub obraz,\nz którego ma prowadzić odnośnik");
        ImGui::Separator();
        if (ImGui::MenuItem("Pusta strona…", "Ctrl+Shift+N")) cmdBlankPage();
        if (ImGui::MenuItem("Strony z innego pliku PDF…")) cmdOpen(true);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Narzędzia", hasDoc)) {
        struct T { Tool t; const char* label; const char* key; };
        const T tools[] = {{Tool::Select, "Zaznacz", "V"}, {Tool::Text, "Tekst", "T"}, {Tool::Pen, "Pióro", "P"},
                           {Tool::Highlight, "Zakreślacz", "H"}, {Tool::Whiteout, "Zamaż (usuwa treść)", "W"}};
        for (const T& t : tools)
            if (ImGui::MenuItem(t.label, t.key, tool_ == t.t)) setTool(t.t);
        ImGui::Separator();
        if (ImGui::MenuItem("Odnośniki w dokumencie…", "Ctrl+L", ll_.open)) ll_.open = !ll_.open;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Strona", hasDoc)) {
        const int id = currentPageId(), idx = currentPage_;
        const int n = int(doc_.pages.size());
        ImGui::TextDisabled("Strona %d z %d", idx + 1, n);
        ImGui::Separator();
        if (ImGui::MenuItem("Obróć w prawo", "Ctrl+R")) pageOps_.push_back({id, 0});
        if (ImGui::MenuItem("Obróć w lewo", "Ctrl+Shift+R")) pageOps_.push_back({id, 4});
        if (ImGui::MenuItem("Obróć wszystkie strony w prawo")) {
            auto before = doc_.pages;
            for (Page& p : doc_.pages) p.extraRot = (p.extraRot + 1) % 4;
            pushUndo(std::move(before));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Przesuń w górę", nullptr, false, idx > 0)) pageOps_.push_back({id, 1});
        if (ImGui::MenuItem("Przesuń w dół", nullptr, false, idx + 1 < n)) pageOps_.push_back({id, 2});
        if (ImGui::MenuItem("Przenieś na początek", nullptr, false, idx > 0)) pageOps_.push_back({id, 5});
        if (ImGui::MenuItem("Przenieś na koniec", nullptr, false, idx + 1 < n)) pageOps_.push_back({id, 6});
        ImGui::Separator();
        if (ImGui::MenuItem("Wstaw pustą stronę za bieżącą…")) cmdBlankPage(id, 0);
        if (ImGui::MenuItem("Wstaw pustą stronę przed bieżącą…")) cmdBlankPage(id, 1);
        ImGui::Separator();
        if (ImGui::MenuItem("Usuń bieżącą stronę", nullptr, false, n > 1)) pageOps_.push_back({id, 3});
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Pomoc")) {
        if (ImGui::MenuItem("Skróty klawiszowe", "F1")) showShortcuts_ = true;
        if (ImGui::MenuItem("O programie")) showAbout_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void App::drawHelpWindows() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    // przejdź do strony
    if (goToOpen_) { ImGui::OpenPopup("Przejdź do strony"); goToOpen_ = false; }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Przejdź do strony", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Numer strony (1–%d):", int(doc_.pages.size()));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputInt("##page", &goToValue_, 1, 10);   // EnterReturnsTrue jest niedozwolone dla pól liczbowych (asercja ImGui)
        const bool enter = enterConfirmed();
        goToValue_ = std::clamp(goToValue_, 1, std::max(1, int(doc_.pages.size())));
        if (ImGui::Button("Przejdź") || enter) { goToPage(goToValue_ - 1); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Anuluj") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showShortcuts_) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36, ImGui::GetFontSize() * 30), ImGuiCond_Appearing);
        if (ImGui::Begin("Skróty klawiszowe", &showShortcuts_, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
            struct S { const char* keys; const char* what; };
            const S groups[][14] = {
                {{"Ctrl+N", "nowy dokument"}, {"Ctrl+O", "otwórz (w nowej karcie)"}, {"Ctrl+Shift+O", "dołącz strony z pliku"}, {"Ctrl+S", "zapisz"},
                 {"Ctrl+Shift+S", "zapisz jako"}, {"Ctrl+W / środkowy klik", "zamknij kartę"}, {"Ctrl+Tab / Ctrl+PgDn", "następna karta"},
                 {"Ctrl+Shift+Tab / Ctrl+PgUp", "poprzednia karta"}, {"Ctrl+Q", "zakończ"}},
                {{"Ctrl+Z / Ctrl+Y", "cofnij / ponów"}, {"Ctrl+C / Ctrl+X / Ctrl+V", "kopiuj / wytnij / wklej"}, {"Ctrl+D", "duplikuj"},
                 {"Delete", "usuń zaznaczenie"}, {"Strzałki (Shift)", "przesuń o 1 pt (10 pt)"}, {"Enter / dwuklik", "edytuj tekst"},
                 {"Ctrl+B / Ctrl+I", "pogrubienie / kursywa"}, {"Esc", "zakończ edycję, odznacz"}, {"Prawy przycisk", "menu kontekstowe"}},
                {{"V T P H W", "zaznacz, tekst, pióro, zakreślacz, zamaż"}, {"Ctrl+I", "wstaw obraz"}, {"Ctrl+K", "odnośnik dla zaznaczenia"}, {"Ctrl+L", "lista odnośników w dokumencie"},
                 {"Klik w odnośnik", "pokaż i zmień adres"}, {"Ctrl+klik w odnośnik", "otwórz odnośnik"}, {"Ctrl+Shift+N", "nowa pusta strona"}},
                {{"Ctrl+= / Ctrl+−", "powiększ / pomniejsz"}, {"Ctrl+kółko myszy", "powiększenie"}, {"Ctrl+0 / Ctrl+1 / Ctrl+2", "szerokość / 100% / cała strona"},
                 {"PgUp / PgDn", "poprzednia / następna strona"}, {"Home / End", "pierwsza / ostatnia strona"}, {"Ctrl+G", "przejdź do strony"},
                 {"Ctrl+R / Ctrl+Shift+R", "obróć stronę w prawo / w lewo"}, {"F4", "miniatury stron (pokaż / ukryj)"}, {"F1", "ta lista"}},
            };
            const char* titles[] = {"Plik", "Edycja", "Narzędzia i wstawianie", "Widok i strony"};
            for (int g = 0; g < 4; ++g) {
                ImGui::SeparatorText(titles[g]);
                if (ImGui::BeginTable(titles[g], 2, ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("klawisze", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 13);
                    ImGui::TableSetupColumn("działanie", ImGuiTableColumnFlags_WidthStretch);
                    for (const S& s : groups[g]) {
                        if (!s.keys) break;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(s.keys);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", s.what);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Na macOS zamiast Ctrl działa Cmd.");
        }
        ImGui::End();
    }

    if (showAbout_) { ImGui::OpenPopup("O programie"); showAbout_ = false; }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("O programie", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const float ls = ImGui::GetFontSize() * 4.5f;
        if (const unsigned tex = iconTex("logo", int(std::lround(ls * fbScale_)))) {
            ImGui::Image(ImTextureRef(ImTextureID(tex)), ImVec2(ls, ls));
            ImGui::SameLine(0, 14);
        }
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, ls * 0.18f));
        ImGui::PushFont(fontBoldUi_ ? fontBoldUi_ : nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
        ImGui::TextUnformatted("Korektor PDF");
        ImGui::PopFont();
        ImGui::TextDisabled("wersja " KOREKTOR_VERSION);
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::TextUnformatted("Szybki edytor PDF: tekst i akapity, obrazy, pióro,\nzakreślanie, zamazywanie z usunięciem treści, strony.");
        ImGui::Spacing();
        ImGui::SeparatorText("Biblioteki");
        ImGui::TextUnformatted("PDFium (BSD-3) — renderowanie i zapis PDF\nDear ImGui (MIT) — interfejs\nGLFW (zlib) — okno i OpenGL\n"
                               "nativefiledialog-extended (zlib) — okna plików\nstb_image (domena publiczna / MIT) — obrazy\n"
                               "DejaVu (licencja Bitstream Vera) — czcionki");
        ImGui::Spacing();
        const std::string cfg = Settings::path();
        if (!cfg.empty()) ImGui::TextDisabled("Ustawienia: %s", cfg.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Zamknij") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------------------------- pasek narzędzi

void App::drawToolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));
    ImGui::BeginChild("##toolbar", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    const bool hasDoc = !doc_.empty();
    const ImGuiStyle& st = ImGui::GetStyle();

    // Przenosi kolejny element do nowej linii, gdy brakuje miejsca.
    auto next = [&](float w) {
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
    };
    auto bw = [&](const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + st.FramePadding.x * 2; };
    auto tip = [](const char* t) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", t);
    };

    // przyciski z ikonami (Widok → Podpisy przy ikonach dodaje nazwy)
    const float btn = std::round(ImGui::GetFrameHeight() * 1.45f);
    const float ico = btn - 8;
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    // zawijanie: przycisk, który się nie mieści, przechodzi do nowego wiersza; separator na końcu
    // wiersza znika, a cała grupa zaczyna się od nowej linii
    bool lineStart = true, afterSep = false;
    auto sepI = [&] {
        if (lineStart) return;
        ImGui::SameLine(0, 9);
        if (ImGui::GetContentRegionAvail().x < btn + 20) { ImGui::NewLine(); lineStart = true; return; }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        tdl->AddLine(ImVec2(p.x, p.y + 6), ImVec2(p.x, p.y + btn - 6), ImGui::GetColorU32(ImGuiCol_Border));
        ImGui::Dummy(ImVec2(1, btn));
        afterSep = true;
    };
    auto iconButton = [&](const char* icon, const char* label, const char* hint, bool enabled, bool on, bool badge = false) {
        const bool showLabel = settings_.iconLabels;
        const float w = showLabel ? btn + ImGui::CalcTextSize(label).x + 6 : btn;
        if (!lineStart) {
            ImGui::SameLine(0, afterSep ? 9.f : 3.f);
            if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
        }
        lineStart = afterSep = false;
        ImGui::PushID(icon);
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::InvisibleButton("##b", ImVec2(w, btn));
        const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s — %s", label, hint);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (on) {
            tdl->AddRectFilled(a, b, col32(kSel, 0.16f), 6);
            tdl->AddRect(a, b, col32(kSel, 0.6f), 6, 0, 1.3f);
        } else if (enabled && act) {
            tdl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_ButtonActive), 6);
        } else if (enabled && hov) {
            tdl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 6);
        }
        if (const unsigned tex = iconTex(icon, int(std::lround(ico * fbScale_))))
            tdl->AddImage(ImTextureRef(ImTextureID(tex)), ImVec2(a.x + 4, a.y + 4), ImVec2(a.x + 4 + ico, a.y + 4 + ico), ImVec2(0, 0), ImVec2(1, 1),
                          enabled ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 80));
        else   // ikona niedostępna: nazwa zamiast obrazka
            tdl->AddText(ImVec2(a.x + 4, a.y + (btn - ImGui::GetFontSize()) / 2), col32(kInk), label);
        if (badge) {   // niezapisane zmiany: czerwona kropka na dyskietce
            const float r = std::max(3.f, btn * 0.11f);
            tdl->AddCircleFilled(ImVec2(a.x + btn - r - 2, a.y + r + 2), r + 1.5f, col32(kBar), 16);
            tdl->AddCircleFilled(ImVec2(a.x + btn - r - 2, a.y + r + 2), r, col32(kAccent), 16);
        }
        if (showLabel)
            tdl->AddText(ImVec2(a.x + btn, a.y + (btn - ImGui::GetFontSize()) / 2), col32(enabled ? kInk : kMute), label);
        ImGui::PopID();
        return clicked && enabled;
    };

    // logo programu
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = btn - 2;
        if (const unsigned tex = iconTex("logo", int(std::lround(w * fbScale_))))
            tdl->AddImage(ImTextureRef(ImTextureID(tex)), ImVec2(p.x, p.y + 1), ImVec2(p.x + w, p.y + 1 + w));
        const char* name = "Korektor PDF";
        ImGui::PushFont(fontBoldUi_, 0.f);
        tdl->AddText(ImVec2(p.x + w + 8, p.y + (btn - ImGui::GetFontSize()) / 2), col32(kInk), name);
        const float tw = ImGui::CalcTextSize(name).x;
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(w + 8 + tw, btn));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Korektor PDF " KOREKTOR_VERSION " — Pomoc → O programie");
        lineStart = false;
    }
    sepI();

    if (iconButton("otworz", "Otwórz", "otwórz plik PDF (Ctrl+O)", true, false)) cmdOpen(false);
    if (iconButton("dolacz", "Dołącz", "dołącz strony z innych plików PDF na końcu (Ctrl+Shift+O)", hasDoc, false)) cmdOpen(true);
    if (iconButton("zapisz", "Zapisz", dirty_ ? "zapisz zmiany (Ctrl+S) — są niezapisane zmiany" : "zapisz PDF (Ctrl+S)", hasDoc, false, dirty_)) cmdSave(false);
    if (iconButton("zapisz-jako", "Zapisz jako", "zapisz pod inną nazwą (Ctrl+Shift+S)", hasDoc, false)) cmdSave(true);
    sepI();
    struct T { Tool t; const char* icon; const char* label; const char* hint; };
    const T tools[] = {
        {Tool::Select, "zaznacz", "Zaznacz", "zaznacz, przesuń, zmień rozmiar (V)"},
        {Tool::Text, "tekst", "Tekst", "popraw tekst z pliku albo dodaj nowy (T)"},
        {Tool::Pen, "pioro", "Pióro", "rysowanie odręczne i podpis (P)"},
        {Tool::Highlight, "zakresl", "Zakreśl", "zakreślacz — przeciągnij prostokąt (H)"},
        {Tool::Whiteout, "zamaz", "Zamaż", "usuń treść spod prostokąta i zakryj go na biało (W)"},
    };
    for (const T& t : tools)
        if (iconButton(t.icon, t.label, t.hint, hasDoc, hasDoc && tool_ == t.t)) setTool(t.t);
    if (iconButton("obraz", "Obraz", "wstaw obraz PNG/JPG na bieżącą stronę (Ctrl+I) — można też upuścić plik na okno", hasDoc, false)) cmdInsertImage();
    if (iconButton("odnosniki", "Odnośniki", "lista wszystkich odnośników w dokumencie — przegląd i zmiana adresów (Ctrl+L)", hasDoc, ll_.open))
        ll_.open = !ll_.open;
    sepI();
    if (iconButton("cofnij", "Cofnij", "cofnij ostatnią zmianę (Ctrl+Z)", !undo_.empty() || editPage_ >= 0, false)) undo();
    if (iconButton("ponow", "Ponów", "ponów cofniętą zmianę (Ctrl+Y)", !redo_.empty(), false)) redo();
    if (iconButton("usun", "Usuń", "usuń zaznaczony element (Delete)", selectedObj() != nullptr, false)) cmdDeleteSelection();


    if (settings_.showProps) {   // drugi wiersz paska (Widok → Pasek właściwości)
    ImGui::NewLine();
    // właściwości: kolor i rozmiar dla narzędzia albo zaznaczonego elementu
    // drugi wiersz: zawsze obecny, żeby strona nie przeskakiwała przy zmianie narzędzia
    const Prop prop = propTarget();
    {
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x - 12, sp.y - 3), ImVec2(sp.x + ImGui::GetWindowWidth(), sp.y - 3),
                                            ImGui::GetColorU32(ImGuiCol_Border, 0.6f));
    }
    if (Obj* so = selectedObj(); prop == Prop::None && so && isImage(*so) && !removedFromFile(*so)) {
        drawImagePanel(*so);
    } else if (Obj* sh = selectedObj(); prop == Prop::None && sh && isShape(*sh) && !removedFromFile(*sh)) {
        drawShapePanel(*sh);
    } else if (prop == Prop::None) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", selectedObj() ? "Ten element nie ma ustawień koloru ani rozmiaru."
                                                : "Wybierz narzędzie albo zaznacz element, aby zmienić kolor, rozmiar i czcionkę.");
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
    }
    if (prop != Prop::None) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(prop == Prop::Text ? "Tekst" : prop == Prop::Pen ? "Pióro" : "Zakreślacz");
        ImGui::SameLine(0, 10);
        const bool isText = prop == Prop::Text;
        Color& cur = propColor(prop);
        // nakładki (pióro, zakreślacz): kolor całego elementu
        auto setOverlayColor = [&](const Color& c, bool undoStep) {
            if (Obj* o = selectedObj(); o && (o->type == ObjType::Ink || o->type == ObjType::Highlight)) {
                if (undoStep) beginPropChange();
                selectedObj()->color = c;
            }
        };
        const float sw = ImGui::GetFrameHeight();
        bool firstSw = true;
        for (int i = 0; i < int(IM_ARRAYSIZE(kPalette)); ++i) {
            if (!firstSw) { ImGui::SameLine(0, 4); if (ImGui::GetContentRegionAvail().x < sw) ImGui::NewLine(); }
            firstSw = false;
            ImGui::PushID(i);
            const bool on = sameColor(cur, kPalette[i]);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::ColorButton("##sw", toV4(kPalette[i]), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(sw, sw))) {
                const Color c = kPalette[i];
                if (isText) applyStyle([&](TextStyle& st) { st.color = c; });
                else { setOverlayColor(c, true); cur = c; }
            }
            if (on) ImGui::GetWindowDrawList()->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + sw + 2, p.y + sw + 2), col32(kInk), 4, 0, 2);
            ImGui::PopID();
        }
        ImGui::SameLine(0, 4);
        if (ImGui::Button("…", ImVec2(ImGui::GetFrameHeight(), 0))) ImGui::OpenPopup("##picker");
        tip("Inny kolor");
        if (ImGui::BeginPopup("##picker")) {
            float c3[3] = {cur.r, cur.g, cur.b};
            if (ImGui::ColorPicker3("##pick", c3, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_DisplayHex)) {
                const Color c{c3[0], c3[1], c3[2], 1};
                const bool step = !colorEditActive_;   // jeden krok cofania na całą zmianę koloru
                colorEditActive_ = true;
                if (isText) applyStyle([&](TextStyle& st) { st.color = c; }, step);
                else { setOverlayColor(c, step); cur = c; }
            }
            ImGui::EndPopup();
        } else {
            colorEditActive_ = false;
        }

        if (prop == Prop::Text || prop == Prop::Pen) {
            const float unit = isText ? textUnit_ : 1.f;   // rozmiar widoczny na stronie (tekst z pliku bywa skalowany macierzą)
            float sz = isText ? textStyle_.size * unit : sizePen_;
            const float lo = isText ? 6.f : 0.5f, hi = isText ? 72.f : 16.f;
            const float sliderW = ImGui::GetFontSize() * 7.5f;
            next(sliderW + bw("Rozmiar"));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Rozmiar");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderW);
            const bool changed = ImGui::SliderFloat("##size", &sz, lo, hi, "%.1f pt");
            const bool activated = ImGui::IsItemActivated();
            if (activated) sizeBeforeValid_ = false;
            if (changed) {
                if (isText) {
                    applyStyle([&](TextStyle& st) { st.size = sz / unit; }, !sizeBeforeValid_);
                    sizeBeforeValid_ = true;   // dalsze przesuwanie suwaka w tym samym kroku cofania
                } else {
                    sizePen_ = sz;
                    if (Obj* o = selectedObj(); o && o->type == ObjType::Ink) {
                        if (!sizeBeforeValid_) beginPropChange();
                        sizeBeforeValid_ = true;
                        selectedObj()->size = sz;
                    }
                }
            }
            if (ImGui::IsItemDeactivated()) sizeBeforeValid_ = false;
        }

        // czcionka i akapit
        if (isText) {
            const char* families[] = {"Bezszeryfowa", "Szeryfowa", "Stała szerokość"};
            const float comboW = ImGui::GetFontSize() * 8.5f;
            next(comboW + ImGui::GetFrameHeight() * 2 + 10);
            const Page* fp = doc_.pageById(editPage_ >= 0 ? editPage_ : selPage_);
            std::string preview = families[std::clamp(textStyle_.font.family, 0, 2)];
            if (textStyle_.font.origObj >= 0) preview += " (z pliku)";
            ImGui::SetNextItemWidth(comboW);
            if (ImGui::BeginCombo("##family", preview.c_str())) {
                for (int i = 0; i < 3; ++i)
                    if (ImGui::Selectable(families[i], textStyle_.font.family == i))
                        applyStyle([&](TextStyle& st) { setVariant(doc_, fp, st, i, st.font.bold, st.font.italic); });
                ImGui::EndCombo();
            }
            tip("Krój tekstu. Jeśli w pliku jest czcionka tego rodzaju, zostanie użyta; inaczej czcionka zastępcza.");
            auto toggle = [&](const char* label, bool on) {
                if (on) { ImGui::PushStyleColor(ImGuiCol_Button, kInk); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kInk); ImGui::PushStyleColor(ImGuiCol_Text, kBar); }
                const bool clicked = ImGui::Button(label, ImVec2(std::max(ImGui::GetFrameHeight(), bw(label)), 0));
                if (on) ImGui::PopStyleColor(3);
                return clicked;
            };
            ImGui::SameLine(0, 4);
            ImGui::PushFont(fontBoldUi_ ? fontBoldUi_ : nullptr, 0.f);
            const bool bClick = toggle("B", textStyle_.font.bold);
            ImGui::PopFont();
            tip("Pogrubienie (Ctrl+B)");
            ImGui::SameLine(0, 4);
            ImGui::PushFont(fontItalicUi_ ? fontItalicUi_ : nullptr, 0.f);
            const bool iClick = toggle("I", textStyle_.font.italic);
            ImGui::PopFont();
            tip("Kursywa (Ctrl+I)");
            if (bClick) toggleBold();
            if (iClick) toggleItalic();

            // wyrównanie akapitu
            struct A { Align a; const char* label; const char* hint; };
            const A al[] = {{Align::Left, "Lewo", "Do lewej"}, {Align::Center, "Środek", "Do środka"},
                            {Align::Right, "Prawo", "Do prawej"}, {Align::Justify, "Justuj", "Do obu krawędzi"}};
            float need = 0;
            for (const A& x : al) need += bw(x.label) + 2;
            next(need + 12);
            ImGui::SameLine(0, 12);
            for (int k = 0; k < 4; ++k) {
                if (k) ImGui::SameLine(0, 2);
                ImGui::PushID(k);
                if (toggle(al[k].label, align_ == al[k].a) ) setAlign(al[k].a);
                ImGui::PopID();
                tip(al[k].hint);
            }
        }
    }
    }

    ImGui::EndChild();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 1), ImVec2(p.x + ImGui::GetWindowWidth(), p.y - 1),
                                        ImGui::GetColorU32(ImGuiCol_Border));
}

// ------------------------------------------------------------------------------- miniatury

// Nagłówek panelu miniatur: liczba stron, przełącznik automatycznego ukrywania i zamknięcie.
void App::drawThumbsHeader(float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight();
    ImGui::SetCursorPos(ImVec2(12, 4));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Strony · %d", int(doc_.pages.size()));
    auto iconBtn = [&](const char* id, float x, const char* tip, auto&& draw) {
        ImGui::SameLine(x);
        const bool click = ImGui::InvisibleButton(id, ImVec2(h, h));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered()) dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 4);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
        draw(ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), h * 0.5f);
        return click;
    };
    const bool autoHide = settings_.thumbsAutoHide;
    // pinezka: pionowa = panel przypięty, przechylona = wysuwany automatycznie
    const bool pinClick = iconBtn("##pin", width - 2 * h - 10,
        autoHide ? "Przypnij panel (stale widoczny)" : "Automatyczne ukrywanie — panel wysuwa się po najechaniu na lewą krawędź okna",
        [&](ImVec2 c, float r) {
            const float ang = autoHide ? 0.785f : 0.f, ca = std::cos(ang), sa = std::sin(ang);
            auto P = [&](float x, float y) { return ImVec2(c.x + (x * ca - y * sa) * r, c.y + (x * sa + y * ca) * r); };
            const ImU32 col = col32(autoHide ? kMute : kInk);
            dl->AddQuadFilled(P(-0.32f, -0.78f), P(0.32f, -0.78f), P(0.32f, -0.12f), P(-0.32f, -0.12f), col);
            dl->AddLine(P(-0.6f, -0.1f), P(0.6f, -0.1f), col, 2.f);
            dl->AddLine(P(0, -0.1f), P(0, 0.8f), col, 1.6f);
        });
    const bool closeClick = iconBtn("##close", width - h - 6, "Zamknij panel miniatur (F4 otwiera ponownie)", [&](ImVec2 c, float r) {
        const float k = r * 0.42f;
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col32(kInk), 1.6f);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col32(kInk), 1.6f);
    });
    const ImVec2 wp = ImGui::GetWindowPos();
    const float ly = ImGui::GetCursorScreenPos().y + 2;
    dl->AddLine(ImVec2(wp.x, ly), ImVec2(wp.x + width, ly), ImGui::GetColorU32(ImGuiCol_Border, 0.7f));
    ImGui::Dummy(ImVec2(0, 3));
    if (pinClick) {
        settings_.thumbsAutoHide = !settings_.thumbsAutoHide;
        settings_.save();
        thumbsOpen_ = settings_.thumbsAutoHide;   // po odpięciu panel zostaje wysunięty, dopóki jest nad nim mysz
        thumbsAnim_ = settings_.thumbsAutoHide ? 1.f : 0.f;
        thumbsLeaveT_ = ImGui::GetTime();
    }
    if (closeClick) {
        settings_.showThumbs = false;
        settings_.save();
        thumbsOpen_ = false;
        thumbsAnim_ = 0;
    }
}

// Panel wysuwany nad stronę (tryb automatycznego ukrywania): otwiera się po chwili przy lewej krawędzi
// obszaru stron, chowa się, gdy mysz go opuści (nie w trakcie przeciągania miniatur).
void App::drawThumbsOverlay(float width) {
    const ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    const ImVec2 mn = canvasMin_, mx = canvasMax_, m = io.MousePos;
    const bool inside = m.y >= mn.y && m.y <= mx.y && m.x >= mn.x - 2;
    const bool inEdge = inside && m.x <= mn.x + 10;
    const float shown = width * (1.f - (1.f - thumbsAnim_) * (1.f - thumbsAnim_));   // łagodne wysuwanie
    const bool inPanel = inside && thumbsAnim_ > 0.01f && m.x <= mn.x + shown + 14;
    const bool blocked = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || drag_ != Drag::None;
    if (!thumbsOpen_) {
        if (inEdge && !blocked && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (thumbsHoverT_ == 0) thumbsHoverT_ = now;
            if (now - thumbsHoverT_ > 0.15) { thumbsOpen_ = true; thumbsLeaveT_ = now; thumbsHoverT_ = 0; }
            else pendingRender_ = true;
        } else {
            thumbsHoverT_ = 0;
        }
    } else {
        if (inPanel || ImGui::GetDragDropPayload()) thumbsLeaveT_ = now;
        else if (now - thumbsLeaveT_ > 0.35) thumbsOpen_ = false;
        else pendingRender_ = true;
    }
    const float target = thumbsOpen_ ? 1.f : 0.f;
    if (thumbsAnim_ != target) {
        thumbsAnim_ += (target - thumbsAnim_) * std::min(1.f, io.DeltaTime * 16.f);
        if (std::fabs(thumbsAnim_ - target) < 0.01f) thumbsAnim_ = target;
        pendingRender_ = true;
    }
    if (thumbsAnim_ <= 0.f) return;
    ImGui::SetNextWindowPos(mn);
    ImGui::SetNextWindowSize(ImVec2(std::max(1.f, shown), mx.y - mn.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin("##thumbsOverlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    drawThumbs(width, true);
    ImGui::End();
    // cień przy prawej krawędzi wysuniętego panelu
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float x = mn.x + shown;
    fg->AddRectFilledMultiColor(ImVec2(x, mn.y), ImVec2(x + 10, mx.y), IM_COL32(20, 30, 40, 50), IM_COL32(20, 30, 40, 0),
                                IM_COL32(20, 30, 40, 0), IM_COL32(20, 30, 40, 50));
}

void App::drawThumbs(float width, bool overlay) {
    const float statusH = ImGui::GetFrameHeight() + 6;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBar);
    ImGui::BeginChild("##thumbcol", ImVec2(width, overlay ? 0.f : -statusH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    drawThumbsHeader(width);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##thumbs", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int budget = 2;
    const float maxPx = width - ImGui::GetFontSize() * 4.2f;
    const float availW = ImGui::GetContentRegionAvail().x;

    for (int i = 0; i < int(doc_.pages.size()); ++i) {
        Page& p = doc_.pages[i];
        ImGui::PushID(p.id);
        const float k = maxPx / std::max(p.shownWidth(), p.shownHeight());
        const float tw = p.shownWidth() * k, th = p.shownHeight() * k;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - ImGui::GetFontSize() * 2.2f - tw) / 2);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + tw, a.y + th);
        if (ImGui::InvisibleButton("##thumb", ImVec2(tw, th))) scrollToPage_ = p.id;
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("KPDF_PAGE", &p.id, sizeof(int));
            ImGui::Text("Strona %d", i + 1);
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            const bool after = ImGui::GetIO().MousePos.y > (a.y + b.y) / 2;
            dl->AddLine(ImVec2(a.x - 6, after ? b.y + 7 : a.y - 7), ImVec2(b.x + 6, after ? b.y + 7 : a.y - 7), col32(kSel), 3);
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("KPDF_PAGE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                const int from = *static_cast<const int*>(pl->Data);
                pageOps_.push_back({from, 100 + i + (after ? 1 : 0)});
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsRectVisible(a, b)) ensurePageTex(p, thumbTex_, k * fbScale_, budget, false);
        dl->AddRectFilled(ImVec2(a.x + 1, a.y + 2), ImVec2(b.x + 1, b.y + 2), IM_COL32(20, 30, 40, 40));
        dl->AddRectFilled(a, b, IM_COL32_WHITE);
        auto it = thumbTex_.find(p.id);
        if (it != thumbTex_.end() && it->second.id && it->second.rot == p.extraRot)
            dl->AddImage(ImTextureRef(ImTextureID(it->second.id)), a, b);
        if (i == currentPage_) dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), col32(kAccent), 2, 0, 2);
        else if (hovered) dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), ImGui::GetColorU32(ImGuiCol_Border), 2, 0, 1);

        // przyciski stron w kolumnie obok miniatury
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 2));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 3));
        const float bs = ImGui::GetFontSize() * 1.55f;
        if (ImGui::Button("↻", ImVec2(bs, 0))) pageOps_.push_back({p.id, 0});
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Obróć o 90°");
        ImGui::BeginDisabled(i == 0);
        if (ImGui::Button("↑", ImVec2(bs, 0))) pageOps_.push_back({p.id, 1});
        ImGui::EndDisabled();
        ImGui::BeginDisabled(i + 1 == int(doc_.pages.size()));
        if (ImGui::Button("↓", ImVec2(bs, 0))) pageOps_.push_back({p.id, 2});
        ImGui::EndDisabled();
        if (ImGui::Button("✕", ImVec2(bs, 0))) pageOps_.push_back({p.id, 3});
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Usuń stronę");
        ImGui::PopStyleVar(2);
        ImGui::EndGroup();

        // podpis: numer, format, liczba edycji
        char label[96];
        std::snprintf(label, sizeof label, "%d  ·  %s%s", i + 1, paperName(p.shownWidth(), p.shownHeight()).c_str(),
                      p.objs.empty() ? "" : ("  ·  ✎" + std::to_string(p.objs.size())).c_str());
        const float lw = ImGui::CalcTextSize(label).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (availW - ImGui::GetFontSize() * 2.2f - lw) / 2));
        ImGui::PushStyleColor(ImGuiCol_Text, i == currentPage_ ? kInk : kMute);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::PopID();
    }
    if (ImGui::Button("+ Pusta strona…", ImVec2(-FLT_MIN, 0))) cmdBlankPage();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wstaw pustą stronę wybranego rozmiaru (Ctrl+Shift+N)");
    ImGui::EndChild();
    ImGui::EndChild();
}

// ------------------------------------------------------------------------------- strony

void App::drawObj(const Page& p, const Obj& o, const ImVec2& org, ImDrawList* dl, bool ghost) {
    auto S = [&](float x, float y) {
        const Vec2 s = baseToShown(p, {x, y});
        return ImVec2(org.x + s.x * zoom_, org.y + s.y * zoom_);
    };
    switch (o.type) {
    case ObjType::Highlight:
    case ObjType::Whiteout: {
        const ImVec2 a = S(o.x, o.y), c = S(o.x + o.w, o.y + o.h);
        const ImVec2 mn(std::min(a.x, c.x), std::min(a.y, c.y)), mx(std::max(a.x, c.x), std::max(a.y, c.y));
        if (o.type == ObjType::Highlight) dl->AddRectFilled(mn, mx, col32(o.color, 0.45f));
        else if (ghost) {   // zamazanie w trakcie rysowania; gotowe pochodzi z renderu strony
            dl->AddRectFilled(mn, mx, IM_COL32(255, 255, 255, 235));
            dl->AddRect(mn, mx, col32(kAccent), 0, 0, 1.5f);
        }
        break;
    }
    case ObjType::Image: {
        const unsigned tex = imageTex(o.image);
        Vec2 c[4];
        imageCorners(o, c);   // narożniki obrazu po obrocie
        dl->AddImageQuad(ImTextureRef(ImTextureID(tex)), S(c[0].x, c[0].y), S(c[1].x, c[1].y), S(c[2].x, c[2].y), S(c[3].x, c[3].y),
                         ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
        break;
    }
    case ObjType::PageImage:
    case ObjType::PageShape:
    case ObjType::Shape:
        break;   // obraz i kształt są częścią renderu strony
    case ObjType::Ink: {
        if (o.pts.empty()) break;
        std::vector<ImVec2> pts;
        pts.reserve(o.pts.size());
        for (auto& q : o.pts) pts.push_back(S(q.x, q.y));
        const float t = std::max(1.f, o.size * zoom_);
        const ImU32 c = col32(o.color);
        if (pts.size() > 1) dl->AddPolyline(pts.data(), int(pts.size()), c, ImDrawFlags_None, t);
        if (t >= 2.5f || pts.size() == 1)
            for (auto& q : pts) dl->AddCircleFilled(q, t / 2, c, 12);   // zaokrąglone łączenia i końce
        break;
    }
    case ObjType::Text:
        break;   // tekst jest częścią renderu strony (PDFium), więc wygląda dokładnie jak w pliku
    }
}

void App::drawPage(Page& p, const ImVec2& org, ImDrawList* dl) {
    const ImVec2 end(org.x + p.shownWidth() * zoom_, org.y + p.shownHeight() * zoom_);
    dl->AddRectFilled(ImVec2(org.x + 1, org.y + 2), ImVec2(end.x + 2, end.y + 4), IM_COL32(20, 30, 40, 28), 2);
    dl->AddRectFilled(ImVec2(org.x, org.y + 1), ImVec2(end.x + 1, end.y + 1), IM_COL32(20, 30, 40, 40));
    dl->AddRectFilled(org, end, IM_COL32_WHITE);
    auto it = pageTex_.find(p.id);
    if (it != pageTex_.end() && it->second.id && it->second.rot == p.extraRot) {
        it->second.lastUse = frameNo_;
        dl->AddImage(ImTextureRef(ImTextureID(it->second.id)), org, end);
    }

    for (const Obj& o : p.objs) drawObj(p, o, org, dl, false);
    drawLinks(p, org, dl);
    if (dragPage_ == p.id && (drag_ == Drag::Pen || drag_ == Drag::Rect)) drawObj(p, draft_, org, dl, true);

    // przeciąganie obrazu z pliku: wycinek bieżącego renderu strony w nowym miejscu
    const bool imgDrag = selPage_ == p.id && selObj_ >= 0 && selObj_ < int(p.objs.size()) &&
                         liveObj(p.objs[size_t(selObj_)]) && dragMoved_ &&
                         (drag_ == Drag::Move || drag_ == Drag::Resize);
    if (imgDrag && it != pageTex_.end() && it->second.id) {
        const Obj& cur = p.objs[size_t(selObj_)];
        const float W = p.shownWidth(), H = p.shownHeight();
        auto S = [&](float x, float y) { const Vec2 s = baseToShown(p, {x, y}); return ImVec2(org.x + s.x * zoom_, org.y + s.y * zoom_); };
        auto UV = [&](float x, float y) { const Vec2 s = baseToShown(p, {x, y}); return ImVec2(s.x / W, s.y / H); };
        // kształt: margines na grubość linii i wygładzanie krawędzi (cienka linia ma prawie zerową wysokość)
        const float m = isShape(cur) ? (cur.strokeOn ? cur.size / 2 : 0.f) + 1.5f : 0.f;
        const Vec2 sc[4] = {{cur.x - m, cur.y - m}, {cur.x + cur.w + m, cur.y - m}, {cur.x + cur.w + m, cur.y + cur.h + m}, {cur.x - m, cur.y + cur.h + m}};
        const Vec2 dc[4] = {{draft_.x - m, draft_.y - m}, {draft_.x + draft_.w + m, draft_.y - m}, {draft_.x + draft_.w + m, draft_.y + draft_.h + m},
                            {draft_.x - m, draft_.y + draft_.h + m}};
        dl->AddQuadFilled(S(sc[0].x, sc[0].y), S(sc[1].x, sc[1].y), S(sc[2].x, sc[2].y), S(sc[3].x, sc[3].y), IM_COL32(255, 255, 255, 190));
        dl->AddImageQuad(ImTextureRef(ImTextureID(it->second.id)), S(dc[0].x, dc[0].y), S(dc[1].x, dc[1].y), S(dc[2].x, dc[2].y), S(dc[3].x, dc[3].y),
                         UV(sc[0].x, sc[0].y), UV(sc[1].x, sc[1].y), UV(sc[2].x, sc[2].y), UV(sc[3].x, sc[3].y));
    }

    if (selPage_ == p.id && selObj_ >= 0 && selObj_ < int(p.objs.size()) &&
        !removedFromFile(p.objs[size_t(selObj_)])) {
        const Obj& o = imgDrag ? draft_ : p.objs[size_t(selObj_)];
        const kpdf::Rect r = objBounds(p, o);
        auto S = [&](float x, float y) {
            const Vec2 s = baseToShown(p, {x, y});
            return ImVec2(org.x + s.x * zoom_, org.y + s.y * zoom_);
        };
        const ImVec2 a = S(r.x, r.y), c = S(r.x + r.w, r.y + r.h);
        const ImVec2 mn(std::min(a.x, c.x) - 2, std::min(a.y, c.y) - 2), mx(std::max(a.x, c.x) + 2, std::max(a.y, c.y) + 2);
        if (editPage_ == p.id && editObj_ == selObj_) {
            // ramka edytowanego tekstu: przerywana, żeby nie zasłaniała liter
            const ImU32 c = overlap_ ? col32(kAccent, 0.9f) : col32(kSel, 0.7f);
            for (float x = mn.x; x < mx.x; x += 8) { dl->AddLine(ImVec2(x, mn.y), ImVec2(std::min(x + 4, mx.x), mn.y), c); dl->AddLine(ImVec2(x, mx.y), ImVec2(std::min(x + 4, mx.x), mx.y), c); }
            for (float y = mn.y; y < mx.y; y += 8) { dl->AddLine(ImVec2(mn.x, y), ImVec2(mn.x, std::min(y + 4, mx.y)), c); dl->AddLine(ImVec2(mx.x, y), ImVec2(mx.x, std::min(y + 4, mx.y)), c); }
        } else {
            dl->AddRect(mn, mx, col32(kSel), 0, 0, 1.5f);
        }
        if (resizable(o)) {
            const Vec2 cc[4] = {{o.x, o.y}, {o.x + o.w, o.y}, {o.x + o.w, o.y + o.h}, {o.x, o.y + o.h}};
            const float s = ImGui::GetFontSize() * 0.36f;
            for (int k = 0; k < 4; ++k) {
                if (!allCorners(o) && k != 2) continue;
                const ImVec2 h = S(cc[k].x, cc[k].y);
                dl->AddRectFilled(ImVec2(h.x - s - 1, h.y - s - 1), ImVec2(h.x + s + 1, h.y + s + 1), IM_COL32_WHITE, 2);
                dl->AddRectFilled(ImVec2(h.x - s + 1, h.y - s + 1), ImVec2(h.x + s - 1, h.y + s - 1), col32(kSel), 2);
            }
        }
    }
    // uchwyt szerokości akapitu (tekst zaznaczony albo edytowany)
    const int handleObj = editPage_ == p.id ? editObj_ : (selPage_ == p.id ? selObj_ : -1);
    if (handleObj >= 0 && handleObj < int(p.objs.size()) && p.objs[size_t(handleObj)].type == ObjType::Text) {
        const ImVec2 h = widthHandlePos(p, org, p.objs[size_t(handleObj)]);
        const float r = ImGui::GetFontSize() * 0.32f;
        const bool vertical = p.extraRot % 2 == 0;   // uchwyt prostopadły do kierunku tekstu na ekranie
        const ImVec2 ext = vertical ? ImVec2(r * 0.55f, r * 1.6f) : ImVec2(r * 1.6f, r * 0.55f);
        dl->AddRectFilled(ImVec2(h.x - ext.x - 1.5f, h.y - ext.y - 1.5f), ImVec2(h.x + ext.x + 1.5f, h.y + ext.y + 1.5f), IM_COL32_WHITE, 3);
        dl->AddRectFilled(ImVec2(h.x - ext.x, h.y - ext.y), ImVec2(h.x + ext.x, h.y + ext.y), col32(overlap_ && editPage_ == p.id ? kAccent : kSel), 2);
    }
    if (editPage_ == p.id) drawTextCaret(p, org, dl);
}

void App::drawTextCaret(const Page& p, const ImVec2& org, ImDrawList* dl) {
    if (editObj_ < 0 || editObj_ >= int(p.objs.size())) return;
    const Obj& o = p.objs[size_t(editObj_)];
    const TextLayout l = doc_.layoutText(p, o);
    if (l.lines.empty()) return;
    auto S = [&](Vec2 b) {
        const Vec2 s = baseToShown(p, b);
        return ImVec2(org.x + s.x * zoom_, org.y + s.y * zoom_);
    };
    auto add = [](Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; };
    // zaznaczenie: prostokąt w każdym wierszu, który obejmuje
    if (caret_ != anchor_) {
        const int c0 = byteToChar(o.text, std::min(caret_, anchor_)), c1 = byteToChar(o.text, std::max(caret_, anchor_));
        for (const auto& ln : l.lines) {
            const int lo = std::max(c0, ln.first), hi = std::min(c1, ln.first + int(ln.carets.size()) - 1);
            if (lo > hi || (lo == hi && c1 != hi)) continue;
            const Vec2 pa = ln.carets[size_t(lo - ln.first)], pb = ln.carets[size_t(hi - ln.first)];
            dl->AddQuadFilled(S(add(pa, ln.ascent)), S(add(pb, ln.ascent)), S(add(pb, ln.descent)), S(add(pa, ln.descent)),
                              col32(kSel, 0.28f));
        }
    }
    // migający kursor; jego wysokość odpowiada rozmiarowi stylu pisania
    if (std::fmod(ImGui::GetTime() - caretTime_, 1.06) < 0.62) {
        const int ci = byteToChar(o.text, caret_);
        const auto& ln = l.lines[size_t(lineOfChar(l, ci))];
        const Vec2 c = ln.carets[size_t(std::clamp(ci - ln.first, 0, int(ln.carets.size()) - 1))];
        const float ul = std::hypot(o.up.x, o.up.y);
        const Vec2 u{o.up.x / ul, o.up.y / ul};
        const float h = textStyle_.size * std::hypot(o.up.x, o.up.y);
        const Vec2 top{c.x + u.x * h * 0.82f, c.y + u.y * h * 0.82f}, bot{c.x - u.x * h * 0.22f, c.y - u.y * h * 0.22f};
        dl->AddLine(S(top), S(bot), col32(kInk), std::max(1.5f, zoom_ * 0.9f));
    }
}

void App::drawCanvas() {
    const float statusH = ImGui::GetFrameHeight() + 6;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kDesk);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGuiWindowFlags flags = ImGuiWindowFlags_HorizontalScrollbar;
    if (io.KeyCtrl) flags |= ImGuiWindowFlags_NoScrollWithMouse;   // Ctrl+kółko = powiększenie
    ImGui::BeginChild("##canvas", ImVec2(0, -statusH), ImGuiChildFlags_None, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 winPos = ImGui::GetWindowPos();
    canvasMin_ = winPos;
    canvasMax_ = ImVec2(winPos.x + ImGui::GetWindowWidth(), winPos.y + ImGui::GetWindowHeight());
    if (restoreScroll_ && !fitNext_) {   // powrót do karty: to samo miejsce dokumentu
        ImGui::SetScrollX(tabScrollX_);
        ImGui::SetScrollY(tabScrollY_);
        restoreScroll_ = false;
    }
    const int prevHoverPage = hoverLinkPage_, prevHover = hoverLink_;
    hoverLinkPage_ = hoverLink_ = -1;

    if (doc_.empty()) {
        const char* l1 = "Otwórz plik PDF albo przeciągnij go do okna.";
        const char* l2 = "Możesz też zacząć od pustej strony.";
        const float w = ImGui::GetFontSize() * 22;
        ImGui::SetCursorPos(ImVec2(std::max(16.f, (avail.x - w) / 2), std::max(16.f, avail.y * 0.36f)));
        ImGui::BeginGroup();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextUnformatted(l1);
        ImGui::PopFont();
        ImGui::TextDisabled("%s", l2);
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.83f, 0.27f, 0.22f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Button("Otwórz PDF…")) cmdOpen(false);
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Nowa pusta strona…")) cmdBlankPage();
        ImGui::Dummy(ImVec2(0, 14));
        ImGui::TextDisabled("Skróty: V zaznacz · T tekst · P pióro · H zakreśl · W zamaż · Ctrl+Z cofnij · Ctrl+S zapisz");
        ImGui::EndGroup();
        ImGui::EndChild();
        return;
    }

    float maxW = 0;
    for (auto& p : doc_.pages) maxW = std::max(maxW, p.shownWidth());
    if (fitNext_) {
        const float target = (avail.x - 2 * kGutter - ImGui::GetStyle().ScrollbarSize) / std::max(1.f, maxW);
        zoom_ = std::clamp(target, 0.1f, 3.f);
        fitNext_ = false;
    }
    if (fitPageNext_ && !doc_.empty()) {   // cała bieżąca strona w oknie
        const Page& cp = doc_.pages[size_t(std::clamp(currentPage_, 0, int(doc_.pages.size()) - 1))];
        const float zw = (avail.x - 2 * kGutter - ImGui::GetStyle().ScrollbarSize) / std::max(1.f, cp.shownWidth());
        const float zh = (avail.y - kGutter - 8) / std::max(1.f, cp.shownHeight());
        zoom_ = std::clamp(std::min(zw, zh), 0.1f, 6.f);
        scrollToPage_ = cp.id;
        fitPageNext_ = false;
    }
    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0)
        setZoom(zoom_ * std::pow(1.15f, io.MouseWheel), &io.MousePos);

    const float contentW = std::max(avail.x, maxW * zoom_ + 2 * kGutter);
    const float scrollY = ImGui::GetScrollY();
    const float probe = scrollY + avail.y * 0.33f;
    int budget = 1;
    float y = kGutter;
    bool anyHovered = false;

    for (int i = 0; i < int(doc_.pages.size()); ++i) {
        Page& p = doc_.pages[i];
        const float pw = p.shownWidth() * zoom_, ph = p.shownHeight() * zoom_;
        const float x = std::floor((contentW - pw) / 2);
        if (scrollToPage_ == p.id) {
            float off = 0;   // miejsce na stronie (odnośnik wewnętrzny), z odrobiną zapasu nad nim
            if (scrollToY_ >= 0) off = std::max(0.f, baseToShown(p, {scrollToX_, scrollToY_}).y * zoom_ - ImGui::GetFontSize() * 2.5f);
            ImGui::SetScrollY(y - kGutter * 0.5f + off);
            scrollToPage_ = -1;
            scrollToY_ = -1;
        }
        if (y <= probe) currentPage_ = i;
        ImGui::SetCursorPos(ImVec2(x, y));
        const ImVec2 org = ImGui::GetCursorScreenPos();
        pageOrigin_[p.id] = {org.x, org.y};
        ImGui::PushID(p.id);
        ImGui::InvisibleButton("##page", ImVec2(pw, ph));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const ImVec2 end(org.x + pw, org.y + ph);
        if (ImGui::IsRectVisible(org, end)) ensurePageTex(p, pageTex_, zoom_ * fbScale_, budget, true);

        if (hovered) anyHovered = true;
        if (hovered && drag_ == Drag::None) {
            const Vec2 b = shownToBase(p, {(io.MousePos.x - org.x) / zoom_, (io.MousePos.y - org.y) / zoom_});
            const int hObj = editPage_ == p.id ? editObj_ : (selPage_ == p.id ? selObj_ : -1);
            if (hObj >= 0 && hitWidthHandle(p, org, io.MousePos, hObj)) ImGui::SetMouseCursor(p.extraRot % 2 ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
            else if (tool_ == Tool::Text || (editPage_ == p.id && hitTest(p, b) == editObj_)) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
            else if (tool_ == Tool::Select) {
                const int corner = hitHandle(p, org, io.MousePos);
                if (corner >= 0) {
                    // ukośna strzałka zgodna z narożnikiem na ekranie
                    const Obj& so = p.objs[size_t(selObj_)];
                    const Vec2 cc[4] = {{so.x, so.y}, {so.x + so.w, so.y}, {so.x + so.w, so.y + so.h}, {so.x, so.y + so.h}};
                    const Vec2 a = baseToShown(p, cc[corner]), o2 = baseToShown(p, cc[(corner + 2) % 4]);
                    ImGui::SetMouseCursor((a.x - o2.x) * (a.y - o2.y) > 0 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
                } else if (const int li = hitLink(p, b); li >= 0) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (prevHoverPage != p.id || prevHover != li) hoverLinkSince_ = ImGui::GetTime();
                    hoverLinkPage_ = p.id;
                    hoverLink_ = li;
                    if (ImGui::GetTime() - hoverLinkSince_ > 0.45 && !linkPopupOpen_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const Link& l = p.links[size_t(li)];
                        std::string what;
                        if (!l.internal()) what = displayUri(l.uri);
                        else if (const int ti = doc_.indexOfPage(l.targetPage); ti >= 0) what = "Strona " + std::to_string(ti + 1);
                        else what = "Strona usunięta";
                        ImGui::SetTooltip("%s\nKliknij, aby zmienić · Ctrl+klik otwiera", what.c_str());
                    }
                } else if (hitTest(p, b) >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                else {
                    Obj probe;
                    if (doc_.hasTextAt(p, b) || doc_.pickImage(p, b, probe) || doc_.pickShape(p, b, 4.f / zoom_, probe))
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                }
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                beginPointer(p, b, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                openContextMenu(p, b);
        }

        drawPage(p, org, dl);
        char num[16];
        std::snprintf(num, sizeof num, "%d", i + 1);
        dl->AddText(ImVec2(end.x - ImGui::CalcTextSize(num).x, org.y - ImGui::GetFontSize() - 4), col32(kMute), num);
        y += ph + kGap;
    }
    ImGui::SetCursorPos(ImVec2(0, y));
    ImGui::Dummy(ImVec2(contentW, kGutter - kGap + 8));
    if (!anyHovered && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && drag_ == Drag::None) {
        // klik na blacie poza stronami
        if (editPage_ >= 0) finishEdit();
        if (tool_ == Tool::Select) selPage_ = selObj_ = -1;
    }

    if (zoomAnchorPending_ && !anchorWaitFrame_) {
        // Poprzednia klatka zmieniła powiększenie: utrzymaj punkt pod kursorem (albo środek widoku).
        anchorWaitFrame_ = true;
        const ImVec2 a = zoomAnchorScreen_.x < 0 ? ImVec2(winPos.x + avail.x / 2, winPos.y + avail.y / 2) : zoomAnchorScreen_;
        anchorOff_ = ImVec2(a.x - winPos.x, a.y - winPos.y);
        anchorContent_ = ImVec2(prevScrollX_ + anchorOff_.x, prevScrollY_ + anchorOff_.y);
    } else if (zoomAnchorPending_) {
        ImGui::SetScrollX(std::max(0.f, (anchorContent_.x - kGutter) * zoomRatio_ + kGutter - anchorOff_.x));
        ImGui::SetScrollY(std::max(0.f, (anchorContent_.y - kGutter) * zoomRatio_ + kGutter - anchorOff_.y));
        zoomAnchorPending_ = false;
        anchorWaitFrame_ = false;
    }
    prevScrollX_ = ImGui::GetScrollX();
    prevScrollY_ = ImGui::GetScrollY();

    updateDrag();
    // uchwyt wysuwanego panelu miniatur przy lewej krawędzi
    if (!doc_.empty() && settings_.showThumbs && settings_.thumbsAutoHide && thumbsAnim_ < 0.01f) {
        const bool hot = io.MousePos.x >= winPos.x - 2 && io.MousePos.x <= winPos.x + 10 && io.MousePos.y >= canvasMin_.y && io.MousePos.y <= canvasMax_.y;
        const float w = hot ? 7.f : 4.f, cy = (canvasMin_.y + canvasMax_.y) / 2;
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        fdl->AddRectFilled(ImVec2(winPos.x, canvasMin_.y), ImVec2(winPos.x + w, canvasMax_.y), col32(kSel, hot ? 0.35f : 0.12f));
        // zakładka ze strzałką w połowie wysokości
        const float th = ImGui::GetFontSize() * 2.2f, tw = ImGui::GetFontSize() * 0.8f;
        fdl->AddRectFilled(ImVec2(winPos.x, cy - th / 2), ImVec2(winPos.x + tw, cy + th / 2), col32(kSel, hot ? 0.75f : 0.35f), 4, ImDrawFlags_RoundCornersRight);
        const float ax = winPos.x + tw * 0.38f, k = tw * 0.22f;
        fdl->AddTriangleFilled(ImVec2(ax, cy - k * 1.4f), ImVec2(ax, cy + k * 1.4f), ImVec2(ax + k * 1.2f, cy), IM_COL32_WHITE);
    }
    // kliknięcie w odnośnik: okienko po puszczeniu przycisku (chwilę później, żeby dwuklik nadal edytował tekst)
    if (linkPress_.page >= 0) {
        if (!linkPress_.released && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            linkPress_.released = true;
            const ImVec2 m = io.MousePos;
            if (dragMoved_ || std::hypot(m.x - linkPress_.pos.x, m.y - linkPress_.pos.y) > 4.f) linkPress_ = {};
        }
        if (linkPress_.released && ImGui::GetTime() - linkPress_.time > io.MouseDoubleClickTime) {
            const int pg = linkPress_.page, li = linkPress_.idx;
            linkPress_ = {};
            if (editPage_ < 0) openLinkPopup(pg, li);
        }
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------------------- dolny pasek

void App::drawStatusBar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 3));
    ImGui::BeginChild("##status", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    const ImVec2 p = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + ImGui::GetWindowWidth(), p.y), ImGui::GetColorU32(ImGuiCol_Border));
    ImGui::AlignTextToFramePadding();
    if (doc_.empty()) {
        ImGui::TextDisabled("Brak dokumentu");
    } else {
        const int i = std::clamp(currentPage_, 0, int(doc_.pages.size()) - 1);
        const Page& pg = doc_.pages[i];
        ImGui::Text("Strona %d z %d", i + 1, int(doc_.pages.size()));
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("%s · %.0f × %.0f mm", paperName(pg.shownWidth(), pg.shownHeight()).c_str(),
                            pg.shownWidth() * 25.4f / 72.f, pg.shownHeight() * 25.4f / 72.f);
        ImGui::SameLine(0, 16);
        const char* hint = "";
        switch (tool_) {
            case Tool::Select:
                if (Obj* so = selectedObj(); so && isImage(*so))
                    hint = "Przeciągnij, aby przesunąć · narożniki zmieniają rozmiar (Shift: bez proporcji) · strzałki: 1 pt (Shift: 10 pt) · Delete usuwa";
                else if (Obj* sh = selectedObj(); sh && isShape(*sh))
                    hint = "Przeciągnij, aby przesunąć · narożniki zmieniają rozmiar (Shift: z proporcjami) · kolor i grubość na pasku · Delete usuwa";
                else if (Obj* so2 = selectedObj(); so2 && so2->type == ObjType::Text)
                    hint = "Dwuklik edytuje tekst · przeciągnij, aby przesunąć · formatowanie z paska dotyczy całości · prawy przycisk: menu";
                else hint = selectedObj() ? "Przeciągnij, aby przesunąć · strzałki przesuwają o 1 pt (Shift: 10 pt) · Delete usuwa · prawy przycisk: menu"
                                          : "Kliknięcie zaznacza tekst, obraz lub kształt (linię, prostokąt) · dwuklik edytuje tekst · prawy przycisk: menu";
                break;
            case Tool::Text: hint = "Kliknij tekst w pliku, aby go poprawić, albo wolne miejsce, aby dodać nowy"; break;
            case Tool::Pen: hint = "Rysuj z wciśniętym przyciskiem myszy"; break;
            case Tool::Highlight: hint = "Przeciągnij prostokąt nad tekstem"; break;
            case Tool::Whiteout: hint = "Przeciągnij prostokąt — treść pod nim zostanie usunięta z pliku"; break;
        }
        std::string editInfo;
        if (Obj* eo = editedObj()) {
            const Page& ep = *doc_.pageById(editPage_);
            const TextLayout l = doc_.layoutText(ep, *eo);
            // czcionka przy kursorze
            const int ci = std::max(0, byteToChar(eo->text, caret_) - 1);
            const size_t si = eo->charStyle.empty() ? 0 : eo->charStyle[size_t(std::min(ci, int(eo->charStyle.size()) - 1))];
            std::string font = si < l.styleFont.size() ? l.styleFont[si] : "";
            const bool orig = si < l.styleOriginal.size() && l.styleOriginal[si];
            const bool wanted = si < eo->styles.size() && eo->styles[si].font.origObj >= 0;
            editInfo = "Piszesz · " + font + (orig ? " (z pliku)" : wanted ? " (zastępcza: czcionka pliku nie ma któregoś znaku)" : "");
            if (overlap_) editInfo += " · akapit nachodzi na tekst obok — poszerz ramkę uchwytem albo skróć tekst";
            else editInfo += " · uchwyt z prawej zmienia szerokość · Esc kończy";
            hint = editInfo.c_str();
        }
        // prawa strona: niezapisane zmiany i powiększenie (podpowiedź jest przycinana przed nimi)
        const char* d = "● niezapisane zmiany";
        const ImGuiStyle& st = ImGui::GetStyle();
        const float zoomW = ImGui::CalcTextSize("−").x + ImGui::CalcTextSize("1000%").x + ImGui::CalcTextSize("+").x + st.FramePadding.x * 6 + 8;
        const float dirtyW = dirty_ ? ImGui::CalcTextSize(d).x + 18 : 0.f;
        const float right = std::max(ImGui::GetCursorPosX() + 40, ImGui::GetWindowWidth() - 12 - zoomW - dirtyW);
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(hp, ImVec2(ImGui::GetWindowPos().x + right - 12, hp.y + ImGui::GetFrameHeight()), true);
        ImGui::TextDisabled("%s", hint);
        ImGui::PopClipRect();
        ImGui::SameLine(right);
        if (dirty_) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted(d);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 18);
        }
        auto tip = [](const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", t); };
        char z[16];
        std::snprintf(z, sizeof z, "%d%%", int(std::lround(zoom_ * 100)));
        if (ImGui::Button("−")) setZoom(zoom_ / 1.2f);
        tip("Pomniejsz (Ctrl+−)");
        ImGui::SameLine(0, 4);
        if (ImGui::Button(z, ImVec2(ImGui::CalcTextSize("1000%").x + st.FramePadding.x * 2, 0))) fitNext_ = true;
        tip("Dopasuj do szerokości (Ctrl+0)");
        ImGui::SameLine(0, 4);
        if (ImGui::Button("+")) setZoom(zoom_ * 1.2f);
        tip("Powiększ (Ctrl+=, Ctrl+kółko myszy)");
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------------------- menu kontekstowe

void App::openContextMenu(Page& p, Vec2 b) {
    if (drag_ != Drag::None) return;
    ctxPage_ = p.id;
    ctxBase_ = b;
    ctxLink_ = hitLink(p, b);
    ctxRequest_ = true;
    if (editPage_ >= 0) {
        if (editPage_ == p.id && hitTest(p, b) == editObj_) {   // w edytowanym tekście
            ctxKind_ = Ctx::Editing;
            const size_t at = caretFromPoint(p, p.objs[size_t(editObj_)], b);
            const size_t lo = std::min(caret_, anchor_), hi = std::max(caret_, anchor_);
            if (at < lo || at > hi) { caret_ = anchor_ = at; caretMoved(); }   // klik poza zaznaczeniem przenosi kursor
            return;
        }
        finishEdit();
    }
    int i = hitTest(p, b);
    if (i < 0) i = pickAt(p, b);
    if (i >= 0) {
        selPage_ = p.id;
        selObj_ = i;
        syncFromSelection();
        ctxKind_ = Ctx::Object;
    } else {
        selPage_ = selObj_ = -1;
        ctxKind_ = Ctx::Page;
    }
}

// Kopia elementu, która żyje niezależnie od oryginału (np. obraz z pliku staje się wstawionym obrazem).
bool App::clipFromSelection(std::vector<Obj>& out) {
    Obj* o = selectedObj();
    Page* p = doc_.pageById(selPage_);
    if (!o || !p) return false;
    Obj c = *o;
    if (c.type == ObjType::PageImage) {
        if (c.hidden) return false;
        int rot = 0;
        ImagePtr img = doc_.pageImageAppearance(*p, c, rot);
        if (!img) { toast("Nie udało się skopiować obrazu."); return false; }
        c.type = ObjType::Image;
        c.image = img;
        c.imgRot = rot;
    } else if (c.type == ObjType::Text) {
        if (c.text.empty()) return false;
        c.normalizeStyles();
    } else if (c.type == ObjType::PageShape) {
        if (c.hidden) return false;
        c.type = ObjType::Shape;   // kopia: samodzielny kształt z tą samą geometrią i wyglądem
        c.shapeEdits = 0;
    }
    c.srcObjs.clear();
    c.childPath.clear();
    if (!isShape(c)) c.origBox = {};   // kształt: origBox opisuje geometrię w `pts`
    c.pickHash = 0;
    c.hidden = false;
    out = {c};
    return true;
}

void App::cmdCopy(bool cut) {
    std::vector<Obj> c;
    if (!clipFromSelection(c)) return;
    clip_ = std::move(c);
    clipPage_ = selPage_;
    clipTabUid_ = tabs_[size_t(activeTab_)]->uid;
    pasteCount_ = 0;
    if (clip_[0].type == ObjType::Text) ImGui::SetClipboardText(clip_[0].text.c_str());   // tekst także dla innych programów
    if (cut) cmdDeleteSelection();
    else toast("Skopiowano.");
}

static void translateObj(Obj& o, float dx, float dy) {
    if (o.type == ObjType::Ink) for (auto& q : o.pts) { q.x += dx; q.y += dy; }
    else { o.x += dx; o.y += dy; }
}

void App::cmdPaste(int pageId, const Vec2* at) {
    Page* p = doc_.pageById(pageId);
    if (!p) return;
    std::vector<Obj> items = clip_;
    if (items.empty()) {
        // tekst ze schowka systemowego jako nowy tekst
        const char* sys = ImGui::GetClipboardText();
        if (!sys || !*sys) return;
        Obj t;
        t.type = ObjType::Text;
        TextStyle st = textStyle_;
        st.size = std::clamp(textStyle_.size * textUnit_, 4.f, 200.f);
        st.font.origObj = -1;
        st.font.okChars.clear();
        doc_.matchPageFont(*p, st.font);
        t.styles = {st};
        t.text = sys;
        t.text.erase(std::remove(t.text.begin(), t.text.end(), '\r'), t.text.end());
        const Vec2 o0 = shownToBase(*p, {0, 0}), ox = shownToBase(*p, {1, 0}), oy = shownToBase(*p, {0, 1});
        t.adv = {ox.x - o0.x, ox.y - o0.y};
        t.up = {-(oy.x - o0.x), -(oy.y - o0.y)};
        const Vec2 c = at ? *at : shownToBase(*p, {p->shownWidth() * 0.15f, p->shownHeight() * 0.2f});
        t.x = c.x; t.y = c.y;
        t.normalizeStyles();
        items = {t};
    }
    auto before = doc_.pages;
    const bool samePage = pageId == clipPage_ && clipTabUid_ == tabs_[size_t(activeTab_)]->uid;   // ta sama strona tej samej karty
    for (Obj& o : items) {
        if (o.type == ObjType::Text && !samePage) {
            // czcionki z pliku są związane ze stroną — na innej stronie szukamy podobnej
            for (TextStyle& st : o.styles) {
                st.font.origObj = -1;
                st.font.okChars.clear();
                doc_.matchPageFont(*p, st.font);
            }
        }
        const kpdf::Rect r = objBounds(*p, o);
        if (at) {
            translateObj(o, at->x - r.x, at->y - r.y);
        } else if (samePage) {
            // to samo miejsce przesunięte w dół i w prawo (na ekranie), kolejne wklejenia dalej
            ++pasteCount_;
            const Vec2 a = shownToBase(*p, {0, 0}), d = shownToBase(*p, {12.f * pasteCount_, 12.f * pasteCount_});
            translateObj(o, d.x - a.x, d.y - a.y);
        }
        p->objs.push_back(o);
    }
    pushUndo(std::move(before));
    setTool(Tool::Select);
    selPage_ = pageId;
    selObj_ = int(p->objs.size()) - 1;
    syncFromSelection();
}

void App::cmdDuplicate() {
    std::vector<Obj> c;
    if (!clipFromSelection(c)) return;
    std::swap(c, clip_);
    const int keepPage = clipPage_, keepCount = pasteCount_;
    const uint64_t keepTab = clipTabUid_;
    clipPage_ = selPage_;
    clipTabUid_ = tabs_[size_t(activeTab_)]->uid;
    pasteCount_ = 0;
    cmdPaste(selPage_, nullptr);
    std::swap(c, clip_);
    clipPage_ = keepPage;
    clipTabUid_ = keepTab;
    pasteCount_ = keepCount;
}

void App::cmdRestoreOriginal() {
    Page* p = doc_.pageById(selPage_);
    Obj* o = selectedObj();
    if (!p || !o || o->srcObjs.empty()) return;
    auto before = doc_.pages;
    p->objs.erase(p->objs.begin() + selObj_);
    selPage_ = selObj_ = -1;
    pushUndo(std::move(before));
    toast("Przywrócono oryginał z pliku.");
}

void App::cmdZOrder(bool toFront) {
    Page* p = doc_.pageById(selPage_);
    Obj* o = selectedObj();
    if (!p || !o) return;
    auto before = doc_.pages;
    Obj moved = *o;
    p->objs.erase(p->objs.begin() + selObj_);
    if (toFront) { p->objs.push_back(moved); selObj_ = int(p->objs.size()) - 1; }
    else { p->objs.insert(p->objs.begin(), moved); selObj_ = 0; }
    pushUndo(std::move(before));
}

void App::drawContextMenu() {
    Page* p = doc_.pageById(ctxPage_);
    if (!p) { ImGui::CloseCurrentPopup(); return; }
    auto colorMenu = [&](bool text) {
        if (!ImGui::BeginMenu("Kolor")) return;
        static const char* names[] = {"Czarny", "Niebieski", "Czerwony", "Zielony", "Żółty", "Różowy"};
        for (int i = 0; i < int(IM_ARRAYSIZE(kPalette)); ++i) {
            ImGui::ColorButton("##c", toV4(kPalette[i]), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                               ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
            ImGui::SameLine();
            ImGui::PushID(i);
            if (ImGui::MenuItem(names[i])) {
                const Color c = kPalette[i];
                if (text) applyStyle([&](TextStyle& st) { st.color = c; });
                else if (Obj* o = selectedObj()) { beginPropChange(); selectedObj()->color = c; (void)o; }
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    };
    auto textFormat = [&]() {
        if (ImGui::MenuItem("Pogrubienie", "Ctrl+B", textStyle_.font.bold)) toggleBold();
        if (ImGui::MenuItem("Kursywa", "Ctrl+I", textStyle_.font.italic)) toggleItalic();
        colorMenu(true);
        if (ImGui::BeginMenu("Rozmiar")) {
            for (float sz : {8.f, 9.f, 10.f, 10.5f, 11.f, 12.f, 14.f, 16.f, 18.f, 20.f, 24.f, 28.f, 36.f, 48.f}) {
                char lbl[16];
                std::snprintf(lbl, sizeof lbl, sz == std::floor(sz) ? "%.0f pt" : "%.1f pt", sz);
                if (ImGui::MenuItem(lbl, nullptr, std::fabs(textStyle_.size * textUnit_ - sz) < 0.05f))
                    applyStyle([&](TextStyle& st) { st.size = sz / textUnit_; });
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Krój")) {
            const Page* fp = doc_.pageById(editPage_ >= 0 ? editPage_ : selPage_);
            const char* fam[] = {"Bezszeryfowy", "Szeryfowy", "Stałej szerokości"};
            for (int k = 0; k < 3; ++k)
                if (ImGui::MenuItem(fam[k], nullptr, textStyle_.font.family == k))
                    applyStyle([&](TextStyle& st) { setVariant(doc_, fp, st, k, st.font.bold, st.font.italic); });
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Wyrównanie")) {
            const char* al[] = {"Do lewej", "Do środka", "Do prawej", "Do obu krawędzi"};
            for (int k = 0; k < 4; ++k)
                if (ImGui::MenuItem(al[k], nullptr, int(align_) == k)) setAlign(Align(k));
            ImGui::EndMenu();
        }
    };
    const bool haveClip = !clip_.empty() || (ImGui::GetClipboardText() && *ImGui::GetClipboardText());
    // odnośnik pod kursorem
    const bool onLink = ctxLink_ >= 0 && ctxLink_ < int(p->links.size());
    auto linkItems = [&]() {
        if (!onLink) return;
        const Link l = p->links[size_t(ctxLink_)];
        if (ImGui::MenuItem("Zmień odnośnik…")) openLinkPopup(ctxPage_, ctxLink_);
        if (ImGui::MenuItem(l.internal() ? "Przejdź do strony odnośnika" : "Otwórz odnośnik", "Ctrl+klik")) cmdOpenLink(l);
        if (!l.internal() && ImGui::MenuItem("Kopiuj adres odnośnika")) {
            ImGui::SetClipboardText(displayUri(l.uri).c_str());
            toast("Adres skopiowany do schowka.");
        }
        if (ImGui::MenuItem("Usuń odnośnik")) cmdDeleteLink(ctxPage_, ctxLink_);
        ImGui::Separator();
    };

    if (ctxKind_ == Ctx::Editing) {
        Obj* o = editedObj();
        if (!o) { ImGui::CloseCurrentPopup(); return; }
        const bool sel = caret_ != anchor_;
        if (ImGui::MenuItem("Wytnij", "Ctrl+X", false, sel)) {
            const size_t a = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
            ImGui::SetClipboardText(o->text.substr(a, e - a).c_str());
            eraseSelection();
        }
        if (ImGui::MenuItem("Kopiuj", "Ctrl+C", false, sel)) {
            const size_t a = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
            ImGui::SetClipboardText(o->text.substr(a, e - a).c_str());
        }
        if (ImGui::MenuItem("Wklej", "Ctrl+V", false, ImGui::GetClipboardText() && *ImGui::GetClipboardText())) {
            std::string c = ImGui::GetClipboardText();
            c.erase(std::remove(c.begin(), c.end(), '\r'), c.end());
            insertText(c);
        }
        if (ImGui::MenuItem("Zaznacz wszystko", "Ctrl+A")) { anchor_ = 0; caret_ = o->text.size(); caretMoved(); }
        ImGui::Separator();
        if (!onLink && ImGui::MenuItem("Dodaj odnośnik…", "Ctrl+K", false, sel)) cmdAddLink();
        linkItems();
        if (!onLink) ImGui::Separator();
        textFormat();
        ImGui::Separator();
        if (ImGui::MenuItem("Zakończ edycję", "Esc")) finishEdit();
        return;
    }

    if (ctxKind_ == Ctx::Object) {
        Obj* o = selectedObj();
        if (!o) { ImGui::CloseCurrentPopup(); return; }
        const ObjType t = o->type;
        linkItems();
        if (t == ObjType::Text) {
            if (ImGui::MenuItem("Edytuj tekst", "dwuklik")) {
                startEdit(selPage_, selObj_, false);
                caret_ = anchor_ = caretFromPoint(*p, *editedObj(), ctxBase_);
                caretMoved();
                return;
            }
            if (ImGui::MenuItem("Kopiuj tekst")) { ImGui::SetClipboardText(o->text.c_str()); toast("Tekst skopiowany do schowka."); }
            if (!onLink && ImGui::MenuItem("Dodaj odnośnik…", "Ctrl+K")) cmdAddLink();
            ImGui::Separator();
            textFormat();
            ImGui::Separator();
        } else if (isImage(*o)) {
            if (ImGui::MenuItem("Obróć w prawo")) cmdRotateImage(1);
            if (ImGui::MenuItem("Obróć w lewo")) cmdRotateImage(-1);
            if (ImGui::MenuItem("Podmień obraz…")) cmdReplaceImage();
            if (ImGui::MenuItem("Zapisz obraz jako PNG…")) cmdExportImage();
            if (!onLink && ImGui::MenuItem("Dodaj odnośnik…", "Ctrl+K")) cmdAddLink();
            ImGui::Separator();
        } else if (isShape(*o)) {
            static const char* names[] = {"Czarny", "Niebieski", "Czerwony", "Zielony", "Żółty", "Różowy"};
            auto shapeColors = [&](bool fillPart) {
                for (int i = 0; i < int(IM_ARRAYSIZE(kPalette)); ++i) {
                    ImGui::ColorButton("##c", toV4(kPalette[i]), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                                       ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
                    ImGui::SameLine();
                    ImGui::PushID(i);
                    if (ImGui::MenuItem(names[i])) {
                        const Color c = kPalette[i];
                        changeShape([&](Obj& sh) {
                            if (fillPart) { sh.color = c; if (!sh.fillMode) sh.fillMode = 1; sh.shapeEdits |= kShapeFill; }
                            else {
                                sh.stroke = c;
                                if (!sh.strokeOn) { sh.strokeOn = true; if (sh.size < 0.1f) sh.size = 1.f; sh.shapeEdits |= kShapeWidth; }
                                sh.shapeEdits |= kShapeStroke;
                            }
                        }, true);
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (fillPart && ImGui::MenuItem("Bez wypełnienia", nullptr, false, o->fillMode != 0))
                    changeShape([](Obj& sh) { sh.fillMode = 0; sh.shapeEdits |= kShapeFill; }, true);
                if (!fillPart && ImGui::MenuItem("Bez konturu", nullptr, false, o->strokeOn))
                    changeShape([](Obj& sh) { sh.strokeOn = false; sh.shapeEdits |= kShapeStroke; }, true);
            };
            if (ImGui::BeginMenu("Kolor wypełnienia")) { shapeColors(true); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Kolor konturu")) { shapeColors(false); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Grubość", shapeHasThickness(*o))) {
                const float cur = shapeThickness(*o);
                for (float w : {0.25f, 0.5f, 0.75f, 1.f, 1.5f, 2.f, 3.f, 4.f, 6.f, 8.f}) {
                    if (!o->strokeOn && w > 6.f) continue;   // linia z wypełnionego paska: do 6 pt
                    char lbl[16];
                    std::snprintf(lbl, sizeof lbl, "%.2g pt", w);
                    if (ImGui::MenuItem(lbl, nullptr, std::fabs(cur - w) < 0.01f)) setShapeThickness(w, true);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
        } else if (t == ObjType::Ink || t == ObjType::Highlight) {
            colorMenu(false);
            if (t == ObjType::Ink && ImGui::BeginMenu("Grubość")) {
                for (float w : {0.5f, 1.f, 1.5f, 2.f, 3.f, 4.f, 6.f, 8.f}) {
                    char lbl[16];
                    std::snprintf(lbl, sizeof lbl, "%.1f pt", w);
                    if (ImGui::MenuItem(lbl, nullptr, std::fabs(o->size - w) < 0.01f)) { snapshot(); selectedObj()->size = w; sizePen_ = w; }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Wytnij", "Ctrl+X")) cmdCopy(true);
        if (ImGui::MenuItem("Kopiuj", "Ctrl+C")) cmdCopy(false);
        if (ImGui::MenuItem("Wklej", "Ctrl+V", false, haveClip)) cmdPaste(ctxPage_, &ctxBase_);
        if (ImGui::MenuItem("Duplikuj", "Ctrl+D")) cmdDuplicate();
        if (t == ObjType::Ink || t == ObjType::Highlight || t == ObjType::Image) {
            ImGui::Separator();
            if (ImGui::MenuItem("Na wierzch")) cmdZOrder(true);
            if (ImGui::MenuItem("Pod spód")) cmdZOrder(false);
        }
        ImGui::Separator();
        const bool fromFile = !o->srcObjs.empty() && !(t == ObjType::Text && o->untouched()) && !idleImage(*o);
        if (fromFile && ImGui::MenuItem("Przywróć oryginał")) cmdRestoreOriginal();
        if (ImGui::MenuItem("Usuń", "Delete")) cmdDeleteSelection();
        return;
    }

    // pusta część strony
    linkItems();
    if (ImGui::MenuItem("Wklej tutaj", "Ctrl+V", false, haveClip)) cmdPaste(ctxPage_, &ctxBase_);
    if (ImGui::MenuItem("Wstaw tekst tutaj")) { setTool(Tool::Text); createTextAt(*p, ctxBase_); }
    if (ImGui::MenuItem("Wstaw obraz tutaj…")) {
        const nfdu8filteritem_t filters[] = {{"Obrazy", "png,jpg,jpeg,bmp,gif,tga"}};
        nfdu8char_t* out = nullptr;
        if (NFD_OpenDialogU8(&out, filters, 1, nullptr) == NFD_OKAY) {
            const std::string path = out;
            NFD_FreePathU8(out);
            insertImage(path, ctxPage_, &ctxBase_);
        }
    }
    ImGui::Separator();
    const int idx = doc_.indexOfPage(ctxPage_);
    if (ImGui::MenuItem("Obróć stronę w prawo")) pageOps_.push_back({ctxPage_, 0});
    if (ImGui::MenuItem("Wstaw pustą stronę za tą…")) cmdBlankPage(ctxPage_, 0);
    if (ImGui::MenuItem("Wstaw pustą stronę przed tą…")) cmdBlankPage(ctxPage_, 1);
    (void)idx;
    if (ImGui::MenuItem("Usuń stronę", nullptr, false, doc_.pages.size() > 1)) pageOps_.push_back({ctxPage_, 3});
}

// ------------------------------------------------------------------------------- odnośniki

namespace {

// Otwiera adres w przeglądarce albo programie pocztowym. Tylko http(s) i mailto — inne rodzaje
// (pliki, programy, skrypty) mogą być niebezpieczne, więc ich nie uruchamiamy.
bool openExternal(const std::string& uri) {
    std::string lower;
    for (char c : uri) lower += char(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0 && lower.rfind("mailto:", 0) != 0) return false;
    for (char c : uri)
        if (static_cast<unsigned char>(c) < 0x21 || c == '"') return false;
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, 0, uri.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, uri.c_str(), -1, w.data(), n);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
#  if defined(__APPLE__)
    const char* tool = "open";
#  else
    const char* tool = "xdg-open";
#  endif
    char* argv[] = {const_cast<char*>(tool), const_cast<char*>(uri.c_str()), nullptr};
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {   // podwójny fork: program otwierający nie zostaje procesem-zombie
        if (fork() == 0) { execvp(tool, argv); _exit(127); }
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return true;
#endif
}

bool looksLikeAddress(const std::string& s) {
    if (s.empty() || s.size() > 2000 || s.find_first_of(" \t\n") != std::string::npos) return false;
    std::string l;
    for (char c : s) l += char(std::tolower(static_cast<unsigned char>(c)));
    if (l.rfind("http://", 0) == 0 || l.rfind("https://", 0) == 0 || l.rfind("www.", 0) == 0 || l.rfind("mailto:", 0) == 0) return true;
    const size_t at = l.find('@');
    return at != std::string::npos && at > 0 && l.find('.', at) != std::string::npos;
}

bool contains(const kpdf::Rect& r, Vec2 b, float tol = 0) {
    return b.x >= r.x - tol && b.x <= r.x + r.w + tol && b.y >= r.y - tol && b.y <= r.y + r.h + tol;
}

} // namespace

int App::hitLink(const Page& p, Vec2 b) const {
    int best = -1;
    float bestArea = FLT_MAX;
    for (int i = 0; i < int(p.links.size()); ++i) {
        const Link& l = p.links[size_t(i)];
        if (!contains(l.box, b, 1.f / zoom_) || linkCovered(p, l)) continue;
        const float a = l.box.w * l.box.h;
        if (a < bestArea) { bestArea = a; best = i; }
    }
    return best;
}

// Odnośnik złamany na kilka wierszy to w PDF kilka prostokątów z tym samym celem — edytujemy je razem.
std::vector<int> App::linkGroup(const Page& p, int idx) const {
    std::vector<int> g;
    if (idx < 0 || idx >= int(p.links.size())) return g;
    g.push_back(idx);
    const Link& base = p.links[size_t(idx)];
    auto sameTarget = [&](const Link& l) {
        return l.uri == base.uri && (!base.internal() || (l.targetPage == base.targetPage && l.hasTargetY == base.hasTargetY && l.targetY == base.targetY));
    };
    auto touching = [](const kpdf::Rect& a, const kpdf::Rect& b) {
        const float tol = std::max(1.f, 0.8f * std::min(std::min(a.w, a.h), std::min(b.w, b.h)));
        return a.x - tol <= b.x + b.w && b.x - tol <= a.x + a.w && a.y - tol <= b.y + b.h && b.y - tol <= a.y + a.h;
    };
    for (bool grew = true; grew;) {
        grew = false;
        for (int i = 0; i < int(p.links.size()); ++i) {
            if (std::find(g.begin(), g.end(), i) != g.end() || !sameTarget(p.links[size_t(i)])) continue;
            for (int k : g)
                if (touching(p.links[size_t(k)].box, p.links[size_t(i)].box)) { g.push_back(i); grew = true; break; }
        }
    }
    std::sort(g.begin(), g.end());
    return g;
}

// Punkt na ekranie tuż pod obszarami (lewy dolny róg ich obrysu).
static ImVec2 belowBoxes(const Page& p, const std::vector<kpdf::Rect>& boxes, ImVec2 org, float zoom) {
    float x0 = FLT_MAX, y1 = -FLT_MAX;
    for (const kpdf::Rect& r : boxes) {
        const Vec2 c[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
        for (const Vec2& v : c) {
            const Vec2 s = baseToShown(p, v);
            x0 = std::min(x0, org.x + s.x * zoom);
            y1 = std::max(y1, org.y + s.y * zoom);
        }
    }
    if (boxes.empty()) return ImGui::GetIO().MousePos;
    return ImVec2(x0, y1 + 6);
}

void App::openLinkPopup(int pageId, int idx) {
    Page* p = doc_.pageById(pageId);
    if (!p || idx < 0 || idx >= int(p->links.size())) return;
    const Link& l = p->links[size_t(idx)];
    lk_ = LinkUi{};
    lk_.page = pageId;
    lk_.idx = linkGroup(*p, idx);
    lk_.kind = l.internal() ? 1 : 0;
    lk_.uri = displayUri(l.uri);
    const int ti = doc_.indexOfPage(l.targetPage);
    lk_.pageNo = ti >= 0 ? ti + 1 : 1;
    std::vector<kpdf::Rect> boxes;
    for (int i : lk_.idx) boxes.push_back(p->links[size_t(i)].box);
    const auto o = pageOrigin_[pageId];
    lk_.anchor = belowBoxes(*p, boxes, ImVec2(o.first, o.second), zoom_);
    lk_.request = true;
}

// Prostokąty (po jednym na wiersz) obejmujące znaki [c0, c1) ułożonego tekstu.
static std::vector<kpdf::Rect> rangeBoxes(const TextLayout& l, int c0, int c1) {
    std::vector<kpdf::Rect> out;
    for (const auto& ln : l.lines) {
        const int lo = std::max(c0, ln.first), hi = std::min(c1, ln.first + int(ln.carets.size()) - 1);
        if (lo >= hi) continue;
        const Vec2 pa = ln.carets[size_t(lo - ln.first)], pb = ln.carets[size_t(hi - ln.first)];
        const Vec2 q[4] = {{pa.x + ln.ascent.x, pa.y + ln.ascent.y}, {pb.x + ln.ascent.x, pb.y + ln.ascent.y},
                           {pb.x + ln.descent.x, pb.y + ln.descent.y}, {pa.x + ln.descent.x, pa.y + ln.descent.y}};
        float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
        for (const Vec2& v : q) { x0 = std::min(x0, v.x); y0 = std::min(y0, v.y); x1 = std::max(x1, v.x); y1 = std::max(y1, v.y); }
        if (x1 - x0 > 0.5f && y1 - y0 > 0.5f) out.push_back({x0, y0, x1 - x0, y1 - y0});
    }
    return out;
}

void App::trackEditLinks() {
    editLinks_.clear();
    editLinks0_.clear();
    editLinksOrig_.clear();
    Page* p = doc_.pageById(editPage_);
    const Obj* o = editedObj();
    if (!p || !o || p->links.empty()) return;
    const TextLayout l = doc_.layoutText(*p, *o);
    std::vector<bool> used(p->links.size(), false);
    for (int i = 0; i < int(p->links.size()); ++i) {
        if (used[size_t(i)] || linkCovered(*p, p->links[size_t(i)])) continue;
        const std::vector<int> g = linkGroup(*p, i);
        int c0 = 1 << 30, c1 = -1;
        for (const auto& ln : l.lines) {
            const Vec2 mid{(ln.ascent.x + ln.descent.x) / 2, (ln.ascent.y + ln.descent.y) / 2};
            for (int k = 0; k + 1 < int(ln.carets.size()); ++k) {
                const Vec2 a = ln.carets[size_t(k)], b = ln.carets[size_t(k) + 1];
                const Vec2 c{(a.x + b.x) / 2 + mid.x, (a.y + b.y) / 2 + mid.y};
                for (int m : g)
                    if (contains(p->links[size_t(m)].box, c, 0.5f)) { c0 = std::min(c0, ln.first + k); c1 = std::max(c1, ln.first + k + 1); }
            }
        }
        if (c1 < 0) continue;
        for (int m : g) used[size_t(m)] = true;
        editLinks_.push_back({p->links[size_t(i)], g, c0, c1});
    }
    if (editLinks_.empty()) return;
    editLinks0_ = editLinks_;
    editLinksOrig_ = p->links;
}

void App::shiftEditLinks(int ca, int ce, int n) {
    const int delta = n - (ce - ca);
    for (EditLink& e : editLinks_) {
        auto start = [&](int x) { return x <= ca ? x : x >= ce ? x + delta : ca; };
        auto stop = [&](int x) { return x <= ca ? x : x >= ce ? x + delta : ca + n; };
        e.c0 = start(e.c0);
        e.c1 = stop(e.c1);
    }
}

void App::updateEditLinks() {
    if (editLinksOrig_.empty()) return;
    Page* p = doc_.pageById(editPage_);
    const Obj* o = editedObj();
    if (!p || !o) return;
    if (sameText(*o, editOrig_)) { p->links = editLinksOrig_; return; }   // bez zmian: odnośniki jak w pliku
    // nowe obszary odnośników tekstu w miejsce starych
    std::vector<Link> links;
    std::vector<bool> drop(editLinksOrig_.size(), false);
    for (const EditLink& e : editLinks0_)
        for (int m : e.members) drop[size_t(m)] = true;
    for (size_t i = 0; i < editLinksOrig_.size(); ++i)
        if (!drop[i]) links.push_back(editLinksOrig_[i]);
    const TextLayout l = doc_.layoutText(*p, *o);
    for (const EditLink& e : editLinks_) {
        if (e.c1 <= e.c0) continue;   // cały tekst odnośnika usunięty
        for (const kpdf::Rect& r : rangeBoxes(l, e.c0, e.c1)) {
            Link nl = e.target;
            nl.box = r;
            links.push_back(nl);
        }
    }
    p->links = std::move(links);
}

std::vector<kpdf::Rect> App::selectionBoxes() const {
    std::vector<kpdf::Rect> out;
    if (editPage_ >= 0) {
        const Page* p = const_cast<App*>(this)->doc_.pageById(editPage_);
        if (!p || editObj_ < 0 || editObj_ >= int(p->objs.size()) || caret_ == anchor_) return out;
        const Obj& o = p->objs[size_t(editObj_)];
        const int c0 = byteToChar(o.text, std::min(caret_, anchor_)), c1 = byteToChar(o.text, std::max(caret_, anchor_));
        return rangeBoxes(doc_.layoutText(*p, o), c0, c1);
    }
    const Page* p = const_cast<App*>(this)->doc_.pageById(selPage_);
    if (!p || selObj_ < 0 || selObj_ >= int(p->objs.size())) return out;
    const Obj& o = p->objs[size_t(selObj_)];
    if (o.type == ObjType::PageImage && o.hidden) return out;
    if (o.type == ObjType::Text && o.text.empty()) return out;
    const kpdf::Rect r = objBounds(*p, o);
    if (r.w > 0.5f && r.h > 0.5f) out.push_back(r);
    return out;
}

void App::cmdAddLink() {
    if (doc_.empty()) return;
    const std::vector<kpdf::Rect> boxes = selectionBoxes();
    const int pid = editPage_ >= 0 ? editPage_ : selPage_;
    Page* p = doc_.pageById(pid);
    if (boxes.empty() || !p) {
        toast(editPage_ >= 0 ? "Zaznacz w tekście fragment, który ma być odnośnikiem." : "Zaznacz tekst albo obraz, z którego ma prowadzić odnośnik.", 4);
        return;
    }
    // zaznaczenie leży już na odnośniku: edycja zamiast nowego
    int existing = -1;
    for (int i = 0; i < int(p->links.size()) && existing < 0; ++i) {
        const kpdf::Rect& a = p->links[size_t(i)].box;
        const kpdf::Rect& b = boxes[0];
        const float ix = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
        const float iy = std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
        if (ix * iy >= 0.6f * b.w * b.h && !linkCovered(*p, p->links[size_t(i)])) existing = i;
    }
    // podpowiedź adresu: zaznaczony tekst, jeśli wygląda jak adres
    std::string guess;
    if (editPage_ >= 0) {
        if (const Obj* o = editedObj()) guess = o->text.substr(std::min(caret_, anchor_), std::max(caret_, anchor_) - std::min(caret_, anchor_));
    } else if (const Obj* o = selectedObj(); o && o->type == ObjType::Text) {
        guess = o->text;
    }
    while (!guess.empty() && std::isspace(static_cast<unsigned char>(guess.back()))) guess.pop_back();
    while (!guess.empty() && std::isspace(static_cast<unsigned char>(guess.front()))) guess.erase(guess.begin());
    if (editPage_ >= 0) finishEdit();
    if (existing >= 0) { openLinkPopup(pid, existing); return; }
    lk_ = LinkUi{};
    lk_.page = pid;
    lk_.newBoxes = boxes;
    lk_.uri = looksLikeAddress(guess) ? guess : std::string();
    lk_.pageNo = std::min(int(doc_.pages.size()), doc_.indexOfPage(pid) + 2);
    const auto o = pageOrigin_[pid];
    lk_.anchor = belowBoxes(*p, boxes, ImVec2(o.first, o.second), zoom_);
    lk_.request = true;
}

void App::cmdOpenLink(const Link& l) {
    if (l.internal()) {
        const int idx = doc_.indexOfPage(l.targetPage);
        if (idx < 0) { toast("Strona, do której prowadzi odnośnik, została usunięta.", 4); return; }
        goToPage(idx);
        if (l.hasTargetY) { scrollToX_ = 0; scrollToY_ = std::max(0.f, l.targetY); }
        return;
    }
    const std::string uri = normalizeUri(l.uri);
    if (uri.empty()) return;
    if (!openExternal(uri)) toast("Tego odnośnika nie otwieram (to nie jest adres www ani e-mail): " + displayUri(uri), 6);
}

void App::cmdDeleteLink(int pageId, int idx) {
    Page* p = doc_.pageById(pageId);
    if (!p || idx < 0 || idx >= int(p->links.size())) return;
    const std::vector<int> g = linkGroup(*p, idx);
    snapshot();
    for (auto it = g.rbegin(); it != g.rend(); ++it) p->links.erase(p->links.begin() + *it);
    toast("Usunięto odnośnik. Tekst zostaje bez zmian.");
}

void App::moveLinksWith(Page& p, const kpdf::Rect& old, float dx, float dy) {
    if (dx == 0 && dy == 0) return;
    for (Link& l : p.links)
        if (contains(old, {l.box.x + l.box.w / 2, l.box.y + l.box.h / 2}, 1.f)) { l.box.x += dx; l.box.y += dy; }
}

void App::dropLinksIn(Page& p, const kpdf::Rect& r) {
    p.links.erase(std::remove_if(p.links.begin(), p.links.end(),
                                 [&](const Link& l) { return contains(r, {l.box.x + l.box.w / 2, l.box.y + l.box.h / 2}, 1.f); }),
                  p.links.end());
}

void App::drawLinks(const Page& p, const ImVec2& org, ImDrawList* dl) {
    if (p.links.empty()) return;
    auto S = [&](float x, float y) {
        const Vec2 s = baseToShown(p, {x, y});
        return ImVec2(org.x + s.x * zoom_, org.y + s.y * zoom_);
    };
    std::vector<int> hot;
    if (hoverLinkPage_ == p.id && hoverLink_ >= 0) hot = linkGroup(p, hoverLink_);
    // odnośnik wskazany z listy: na chwilę wyraźnie wyróżniony
    if (flash_.page == p.id && ImGui::GetTime() < flash_.until && flash_.idx < int(p.links.size())) {
        const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 9.f);
        for (int i : linkGroup(p, flash_.idx)) {
            const Link& l = p.links[size_t(i)];
            const ImVec2 a = S(l.box.x, l.box.y), c = S(l.box.x + l.box.w, l.box.y + l.box.h);
            const ImVec2 mn(std::min(a.x, c.x) - 3, std::min(a.y, c.y) - 3), mx(std::max(a.x, c.x) + 3, std::max(a.y, c.y) + 3);
            dl->AddRectFilled(mn, mx, col32(kAccent, 0.10f + 0.15f * pulse), 3);
            dl->AddRect(mn, mx, col32(kAccent, 0.9f), 3, 0, 2.f);
        }
        pendingRender_ = true;
    }
    const bool popupHere = lk_.page == p.id && linkPopupOpen_;
    if (popupHere) hot.insert(hot.end(), lk_.idx.begin(), lk_.idx.end());
    for (int i = 0; i < int(p.links.size()); ++i) {
        const Link& l = p.links[size_t(i)];
        if (linkCovered(p, l)) continue;
        const bool h = std::find(hot.begin(), hot.end(), i) != hot.end();
        if (!h && !settings_.showLinks) continue;
        const ImVec2 a = S(l.box.x, l.box.y), c = S(l.box.x + l.box.w, l.box.y + l.box.h);
        const ImVec2 mn(std::min(a.x, c.x), std::min(a.y, c.y)), mx(std::max(a.x, c.x), std::max(a.y, c.y));
        const bool broken = l.internal() && doc_.indexOfPage(l.targetPage) < 0;
        const ImVec4 col = broken ? kAccent : kSel;
        if (h) dl->AddRectFilled(mn, mx, col32(col, 0.12f), 2);
        dl->AddRect(mn, mx, col32(col, h ? 0.85f : 0.35f), 2, 0, h ? 1.5f : 1.f);
    }
    if (popupHere)
        for (const kpdf::Rect& r : lk_.newBoxes) {
            const ImVec2 a = S(r.x, r.y), c = S(r.x + r.w, r.y + r.h);
            const ImVec2 mn(std::min(a.x, c.x), std::min(a.y, c.y)), mx(std::max(a.x, c.x), std::max(a.y, c.y));
            dl->AddRectFilled(mn, mx, col32(kSel, 0.12f), 2);
            dl->AddRect(mn, mx, col32(kSel, 0.85f), 2, 0, 1.5f);
        }
}

void App::drawLinkPopup() {
    Page* p = doc_.pageById(lk_.page);
    const bool creating = !lk_.newBoxes.empty();
    bool valid = p != nullptr;
    if (p && !creating) {
        valid = !lk_.idx.empty();
        for (int i : lk_.idx) valid &= i >= 0 && i < int(p->links.size());
    }
    if (!valid) { ImGui::CloseCurrentPopup(); return; }
    const float fs = ImGui::GetFontSize();
    const int n = int(doc_.pages.size());

    ImGui::PushFont(fontBoldUi_, ImGui::GetStyle().FontSizeBase);
    ImGui::TextUnformatted(creating ? "Nowy odnośnik" : "Odnośnik");
    ImGui::PopFont();
    if (ImGui::RadioButton("Adres www lub e-mail", &lk_.kind, 0)) lk_.focus = true;
    ImGui::SameLine(0, 14);
    if (ImGui::RadioButton("Strona w tym dokumencie", &lk_.kind, 1)) lk_.focus = true;
    bool enter = false;
    if (lk_.kind == 0) {
        if (lk_.focus || ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); lk_.focus = false; }
        ImGui::SetNextItemWidth(fs * 26);
        enter = ImGui::InputTextWithHint("##uri", "np. www.firma.pl albo biuro@firma.pl", &lk_.uri,
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const std::string shown = displayUri(normalizeUri(lk_.uri));
        std::string typed = lk_.uri;
        while (!typed.empty() && std::isspace(static_cast<unsigned char>(typed.back()))) typed.pop_back();
        while (!typed.empty() && std::isspace(static_cast<unsigned char>(typed.front()))) typed.erase(typed.begin());
        if (!shown.empty() && shown != typed) ImGui::TextDisabled("Zostanie zapisany jako: %s", shown.c_str());
    } else {
        if (lk_.focus || ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); lk_.focus = false; }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Strona");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 8);
        ImGui::InputInt("##page", &lk_.pageNo, 1, 10);
        enter = enterConfirmed();
        lk_.pageNo = std::clamp(lk_.pageNo, 1, std::max(1, n));
        ImGui::SameLine();
        ImGui::TextDisabled("z %d", n);
        if (!creating) {
            const Link& cur = p->links[size_t(lk_.idx[0])];
            if (cur.internal() && doc_.indexOfPage(cur.targetPage) < 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
                ImGui::TextUnformatted("Strona docelowa została usunięta — wybierz inną.");
                ImGui::PopStyleColor();
            }
        }
    }
    const bool ok = lk_.kind == 1 || !normalizeUri(lk_.uri).empty();

    // odnośnik o wartościach z okienka (do zapisania albo otwarcia)
    auto target = [&](Link l) {
        if (lk_.kind == 0) {
            l.uri = normalizeUri(lk_.uri);
            l.targetPage = -1;
            l.hasTargetY = false;
        } else {
            const int id = doc_.pages[size_t(lk_.pageNo - 1)].id;
            if (!l.internal() || l.targetPage != id) { l.hasTargetY = false; l.targetY = 0; }
            l.uri.clear();
            l.targetPage = id;
        }
        return l;
    };

    ImGui::Dummy(ImVec2(0, 2));
    if (creating) {
        ImGui::BeginDisabled(!ok);
        bool go = ImGui::Button("Wstaw odnośnik");
        ImGui::EndDisabled();
        go |= enter && ok;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Anuluj");
        if (go) {
            snapshot();
            for (const kpdf::Rect& r : lk_.newBoxes) {
                Link l;
                l.box = r;
                p->links.push_back(target(l));
            }
            toast(settings_.showLinks ? "Dodano odnośnik." : "Dodano odnośnik (obramowania odnośników są wyłączone w menu Widok).");
            lk_ = LinkUi{};
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            lk_ = LinkUi{};
            ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::BeginDisabled(!ok);
        bool save = ImGui::Button("Zapisz");
        ImGui::EndDisabled();
        save |= enter && ok;
        ImGui::SameLine();
        ImGui::BeginDisabled(!ok);
        const bool open = ImGui::Button(lk_.kind == 0 ? "Otwórz" : "Przejdź");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(lk_.kind == 0 ? "Otwórz adres w przeglądarce albo programie pocztowym" : "Przewiń do strony docelowej");
        bool copy = false;
        if (lk_.kind == 0) {
            ImGui::SameLine();
            ImGui::BeginDisabled(!ok);
            copy = ImGui::Button("Kopiuj");
            ImGui::EndDisabled();
        }
        ImGui::SameLine(0, 18);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        const bool del = ImGui::Button("Usuń odnośnik");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Enter zapisuje · Esc zamyka · Ctrl+klik na odnośniku otwiera go od razu");
        if (save) {
            bool changed = false;
            for (int i : lk_.idx) {
                Link& l = p->links[size_t(i)];
                const Link nl = target(l);
                changed |= nl.uri != l.uri || nl.targetPage != l.targetPage || nl.hasTargetY != l.hasTargetY;
            }
            if (changed) {
                snapshot();
                for (int i : lk_.idx) p->links[size_t(i)] = target(p->links[size_t(i)]);
                toast("Odnośnik zmieniony.");
            }
            lk_ = LinkUi{};
            ImGui::CloseCurrentPopup();
        } else if (open) {
            const Link l = target(p->links[size_t(lk_.idx[0])]);
            if (l.internal()) { lk_ = LinkUi{}; ImGui::CloseCurrentPopup(); }
            cmdOpenLink(l);
        } else if (copy) {
            ImGui::SetClipboardText(displayUri(normalizeUri(lk_.uri)).c_str());
            toast("Adres skopiowany do schowka.");
        } else if (del) {
            const int pid = lk_.page, i0 = lk_.idx[0];
            lk_ = LinkUi{};
            ImGui::CloseCurrentPopup();
            cmdDeleteLink(pid, i0);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { lk_ = LinkUi{}; ImGui::CloseCurrentPopup(); }
}

// ------------------------------------------------------------------------------- okno „Odnośniki w dokumencie”

static const char* plural(int n, const char* one, const char* few, const char* many) {
    if (n == 1) return one;
    const int d = n % 10, t = n % 100;
    return d >= 2 && d <= 4 && !(t >= 12 && t <= 14) ? few : many;
}

static std::string lowerUtf8(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Indeks odnośników: przebudowywany tylko wtedy, gdy zmienił się skrót odnośników dokumentu
// (adresy, cele, położenie, kolejność stron, zamazania) — liczony wyłącznie przy otwartym oknie.
void App::rebuildLinkIndex() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* d, size_t n) {
        const auto* q = static_cast<const uint8_t*>(d);
        for (size_t i = 0; i < n; ++i) { h ^= q[i]; h *= 1099511628211ull; }
    };
    for (const Page& p : doc_.pages) {
        mix(&p.id, sizeof p.id);
        const size_t n = p.links.size();
        mix(&n, sizeof n);
        for (const Link& l : p.links) {
            mix(l.uri.data(), l.uri.size());
            mix(&l.targetPage, sizeof l.targetPage);
            mix(&l.box, sizeof l.box);
        }
        for (const Obj& o : p.objs)
            if (o.type == ObjType::Whiteout) { mix(&o.x, sizeof o.x); mix(&o.y, sizeof o.y); mix(&o.w, sizeof o.w); mix(&o.h, sizeof o.h); }
    }
    if (h == ll_.sig) return;
    ll_.sig = h;
    ll_.entries.clear();
    ll_.total = 0;
    std::unordered_map<std::string, size_t> at;
    std::vector<int> lastPage;
    for (int pi = 0; pi < int(doc_.pages.size()); ++pi) {
        const Page& p = doc_.pages[size_t(pi)];
        std::vector<bool> used(p.links.size(), false);
        for (int i = 0; i < int(p.links.size()); ++i) {
            if (used[size_t(i)]) continue;
            const Link& l = p.links[size_t(i)];
            if (linkCovered(p, l)) { used[size_t(i)] = true; continue; }
            const std::vector<int> g = linkGroup(p, i);
            for (int k : g) used[size_t(k)] = true;
            const std::string key = l.internal() ? "#" + std::to_string(l.targetPage) : l.uri;
            auto it = at.find(key);
            if (it == at.end()) {
                it = at.emplace(key, ll_.entries.size()).first;
                LinkEntry e;
                e.key = key;
                e.uri = l.uri;
                e.internal = l.internal();
                e.target = l.targetPage;
                if (e.internal) {
                    const int ti = doc_.indexOfPage(l.targetPage);
                    e.shown = ti >= 0 ? "Strona " + std::to_string(ti + 1) : std::string("Strona usunięta");
                } else {
                    e.shown = displayUri(l.uri);
                }
                ll_.entries.push_back(std::move(e));
                lastPage.push_back(-1);
            }
            LinkEntry& e = ll_.entries[it->second];
            e.occ.push_back({p.id, i});
            ++ll_.total;
            if (lastPage[it->second] != pi) {
                lastPage[it->second] = pi;
                if (e.pages.size() < 40) e.pages += (e.pages.empty() ? "" : ", ") + std::to_string(pi + 1);
                else if (e.pages.back() != '.') e.pages += "…";
            }
        }
    }
}

void App::showLink(int pageId, int idx) {
    Page* p = doc_.pageById(pageId);
    if (!p || idx < 0 || idx >= int(p->links.size())) return;
    goToPage(doc_.indexOfPage(pageId));
    scrollToX_ = p->links[size_t(idx)].box.x;
    scrollToY_ = std::max(0.f, p->links[size_t(idx)].box.y - 40.f);
    flash_ = {pageId, idx, ImGui::GetTime() + 1.8};
}

void App::replaceUriEverywhere(const std::string& from, const std::string& to) {
    if (from == to || to.empty()) return;
    snapshot();
    for (Page& p : doc_.pages)
        for (Link& l : p.links)
            if (!l.internal() && l.uri == from) l.uri = to;
    int n = 0;   // wystąpienia jak na liście (odnośnik w kilku wierszach liczy się raz)
    for (const LinkEntry& e : ll_.entries)
        if (!e.internal && e.uri == from) n = int(e.occ.size());
    toast(n > 1 ? "Adres zmieniony we wszystkich wystąpieniach (" + std::to_string(n) + ")." : std::string("Adres zmieniony."));
}

void App::drawLinkList() {
    if (!ll_.open) return;
    const float fs = ImGui::GetFontSize();
    // domyślnie w prawym dolnym rogu — góra strony, do której przewija „Pokaż”, zostaje widoczna
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(fs * 52, vp->WorkSize.x - 40), std::min(fs * 21, vp->WorkSize.y * 0.55f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 24, vp->WorkPos.y + vp->WorkSize.y - ImGui::GetFrameHeight() - 24),
                            ImGuiCond_FirstUseEver, ImVec2(1, 1));
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 30, fs * 12), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    const bool vis = ImGui::Begin("Odnośniki w dokumencie", &ll_.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (!vis) { ImGui::End(); return; }
    if (doc_.empty()) { ImGui::TextDisabled("Brak otwartego dokumentu."); ImGui::End(); return; }
    rebuildLinkIndex();
    auto tip = [](const char* t) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", t); };

    // --- filtr
    ImGui::SetNextItemWidth(fs * 17);
    ImGui::InputTextWithHint("##filter", "Szukaj w adresach…", &ll_.filter);
    ImGui::SameLine();
    ImGui::Checkbox("Odnośniki do stron dokumentu", &ll_.showInternal);
    tip("Pokaż też odnośniki prowadzące do stron tego dokumentu (np. do bibliografii)");
    const std::string needle = lowerUtf8(ll_.filter);
    std::vector<int> shown;
    int shownLinks = 0, shownExternal = 0;
    for (int i = 0; i < int(ll_.entries.size()); ++i) {
        const LinkEntry& e = ll_.entries[size_t(i)];
        if (e.internal && !ll_.showInternal) continue;
        if (!needle.empty() && lowerUtf8(e.shown).find(needle) == std::string::npos) continue;
        shown.push_back(i);
        shownLinks += int(e.occ.size());
        if (!e.internal) ++shownExternal;
    }
    ImGui::SameLine(0, 14);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%d %s · %d %s", int(shown.size()), plural(int(shown.size()), "adres", "adresy", "adresów"), shownLinks,
                        plural(shownLinks, "odnośnik", "odnośniki", "odnośników"));

    // --- zamiana fragmentu adresu (np. http:// -> https://, stara domena -> nowa)
    if (!ll_.filter.empty() && shownExternal > 0) {
        ImGui::SetNextItemWidth(fs * 17);
        ImGui::InputTextWithHint("##replace", "Zamień znaleziony fragment na…", &ll_.replace);
        ImGui::SameLine();
        char lbl[64];
        std::snprintf(lbl, sizeof lbl, "Zamień w %d %s", shownExternal, plural(shownExternal, "adresie", "adresach", "adresach"));
        if (ImGui::Button(lbl)) {
            // każdy pasujący adres: wszystkie wystąpienia fragmentu (bez względu na wielkość liter)
            std::vector<std::pair<std::string, std::string>> changes;
            for (int i : shown) {
                const LinkEntry& e = ll_.entries[size_t(i)];
                if (e.internal) continue;
                std::string s = e.shown, low = lowerUtf8(s), out;
                size_t pos = 0;
                for (size_t f; (f = low.find(needle, pos)) != std::string::npos; pos = f + needle.size()) out += s.substr(pos, f - pos) + ll_.replace;
                out += s.substr(pos);
                const std::string nu = normalizeUri(out);
                if (!nu.empty() && nu != e.uri) changes.push_back({e.uri, nu});
            }
            if (!changes.empty()) {
                snapshot();
                std::unordered_map<std::string, std::string> map(changes.begin(), changes.end());
                for (Page& p : doc_.pages)
                    for (Link& l : p.links)
                        if (!l.internal())
                            if (auto it = map.find(l.uri); it != map.end()) l.uri = it->second;
                const int n = int(changes.size());
                toast("Zmieniono " + std::to_string(n) + " " + plural(n, "adres", "adresy", "adresów") + " (Ctrl+Z cofa).");
                ll_.filter = ll_.replace;
                ll_.replace.clear();
            }
        }
        tip("Zamienia znaleziony fragment we wszystkich pokazanych adresach (jeden krok cofania)");
    }

    if (shown.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", ll_.entries.empty() ? "W dokumencie nie ma odnośników."
                                  : !ll_.filter.empty() ? "Żaden adres nie pasuje do wyszukiwania."
                                                        : "Są tylko odnośniki do stron dokumentu — zaznacz pole powyżej, aby je pokazać.");
        ImGui::End();
        return;
    }

    // --- tabela: tylko widoczne wiersze są rysowane (ImGuiListClipper)
    struct Row { int entry, occ; };
    std::vector<Row> rows;
    rows.reserve(shown.size());
    for (int i : shown) {
        rows.push_back({i, -1});
        const LinkEntry& e = ll_.entries[size_t(i)];
        if (e.occ.size() > 1 && ll_.expanded[e.key])
            for (int k = 0; k < int(e.occ.size()); ++k) rows.push_back({i, k});
    }
    const float rowH = ImGui::GetFrameHeight() + 2;
    const float actW = ImGui::CalcTextSize("Pokaż").x + ImGui::CalcTextSize("Otwórz").x + ImGui::CalcTextSize("Usuń").x +
                       ImGui::GetStyle().FramePadding.x * 6 + ImGui::GetStyle().ItemSpacing.x * 3;
    int budget = 3;   // nowych odczytów tekstu na klatkę — reszta w kolejnych klatkach, bez przycinania okna

    // tekst pod odnośnikiem (wszystkie wiersze odnośnika), z pamięci podręcznej
    auto anchorText = [&](const LinkOcc& oc) -> const std::string* {
        const Page* p = doc_.pageById(oc.page);
        if (!p || oc.idx < 0 || oc.idx >= int(p->links.size())) return nullptr;
        const kpdf::Rect& b = p->links[size_t(oc.idx)].box;
        char key[96];
        std::snprintf(key, sizeof key, "%d:%.2f:%.2f:%llx", oc.page, b.x, b.y, (unsigned long long)doc_.contentVersion(*p));
        auto it = ll_.text.find(key);
        if (it != ll_.text.end()) return &it->second;
        if (budget <= 0) { pendingRender_ = true; return nullptr; }
        --budget;
        std::string t;
        for (int m : linkGroup(*p, oc.idx)) {
            const std::string part = doc_.textInRect(*p, p->links[size_t(m)].box);
            if (!part.empty()) t += (t.empty() ? "" : " ") + part;
        }
        if (ll_.text.size() > 20000) ll_.text.clear();
        return &ll_.text.emplace(key, std::move(t)).first->second;
    };

    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    std::function<void()> after;   // zmiana modelu po narysowaniu tabeli (indeksy wierszy zostają ważne do końca)
    if (ImGui::BeginTable("##links", 4, tf, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Adres", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("Tekst odnośnika", ImGuiTableColumnFlags_WidthStretch, 2.f);
        ImGui::TableSetupColumn("Strony", ImGuiTableColumnFlags_WidthFixed, fs * 6.5f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, actW);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(int(rows.size()), rowH);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const Row row = rows[size_t(r)];
                const LinkEntry& e = ll_.entries[size_t(row.entry)];
                ImGui::TableNextRow(0, rowH);
                ImGui::PushID(e.key.c_str());
                ImGui::PushID(row.occ);
                if (row.occ < 0) {
                    // ---- adres (wszystkie wystąpienia)
                    ImGui::TableNextColumn();
                    if (e.occ.size() > 1) {
                        bool& ex = ll_.expanded[e.key];
                        if (ImGui::ArrowButton("##ex", ex ? ImGuiDir_Down : ImGuiDir_Right)) ex = !ex;
                        tip(ex ? "Zwiń wystąpienia" : "Pokaż każde wystąpienie osobno");
                    } else {
                        ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
                    }
                    ImGui::SameLine(0, 4);
                    if (e.internal) {
                        ImGui::AlignTextToFramePadding();
                        const bool broken = doc_.indexOfPage(e.target) < 0;
                        if (broken) ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
                        ImGui::Text("→ %s", e.shown.c_str());
                        if (broken) ImGui::PopStyleColor();
                    } else {
                        const bool editing = ll_.editKey == e.key;
                        std::string tmp = e.shown;
                        std::string& buf = editing ? ll_.editBuf : tmp;
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                        ImGui::InputText("##uri", &buf);
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemActivated()) { ll_.editKey = e.key; ll_.editBuf = e.shown; }
                        if (ImGui::IsItemDeactivatedAfterEdit() && editing) {
                            const std::string nu = normalizeUri(ll_.editBuf), from = e.uri;
                            if (nu.empty()) toast("Adres nie może być pusty — aby usunąć odnośnik, użyj „Usuń”.", 4);
                            else if (nu != from) after = [this, from, nu] { replaceUriEverywhere(from, nu); };
                            ll_.editKey.clear();
                        } else if (ImGui::IsItemDeactivated()) {
                            ll_.editKey.clear();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
                            ImGui::SetTooltip("%s\nKliknij, aby zmienić — zmiana obejmie %d %s", e.shown.c_str(), int(e.occ.size()),
                                              plural(int(e.occ.size()), "odnośnik", "odnośniki", "odnośników"));
                    }
                    // ---- tekst
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if (const std::string* t = anchorText(e.occ[0])) {
                        if (t->empty()) ImGui::TextDisabled("(bez tekstu)");
                        else ImGui::TextUnformatted(t->c_str());
                        if (!t->empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", t->c_str());
                    } else {
                        ImGui::TextDisabled("…");
                    }
                    // ---- strony
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if (e.occ.size() > 1) ImGui::Text("%d× · %s", int(e.occ.size()), e.pages.c_str());
                    else ImGui::TextUnformatted(e.pages.c_str());
                    // ---- działania
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Pokaż")) showLink(e.occ[0].page, e.occ[0].idx);
                    tip("Przewiń do odnośnika na stronie");
                    ImGui::SameLine();
                    if (ImGui::SmallButton(e.internal ? "Przejdź" : "Otwórz")) {
                        const Page* p = doc_.pageById(e.occ[0].page);
                        if (p) { const Link l = p->links[size_t(e.occ[0].idx)]; cmdOpenLink(l); }
                    }
                    tip(e.internal ? "Przewiń do strony docelowej" : "Otwórz adres w przeglądarce albo programie pocztowym");
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
                    if (ImGui::SmallButton("Usuń")) {
                        const bool internal = e.internal;
                        const int target = e.target;
                        const std::string uri = e.uri;
                        const int n = int(e.occ.size());
                        after = [this, internal, target, uri, n] {
                            snapshot();
                            for (Page& p : doc_.pages)
                                p.links.erase(std::remove_if(p.links.begin(), p.links.end(), [&](const Link& l) {
                                                  return internal ? (l.internal() && l.targetPage == target) : (!l.internal() && l.uri == uri);
                                              }), p.links.end());
                            toast("Usunięto " + std::to_string(n) + " " + plural(n, "odnośnik", "odnośniki", "odnośników") +
                                  " (tekst zostaje). Ctrl+Z cofa.");
                        };
                    }
                    ImGui::PopStyleColor();
                    tip(e.occ.size() > 1 ? "Usuń wszystkie odnośniki z tym adresem (tekst zostaje)" : "Usuń odnośnik (tekst zostaje)");
                } else {
                    // ---- jedno wystąpienie
                    const LinkOcc oc = e.occ[size_t(row.occ)];
                    ImGui::TableNextColumn();
                    ImGui::Indent(ImGui::GetFrameHeight() + 4);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("strona %d", doc_.indexOfPage(oc.page) + 1);
                    ImGui::Unindent(ImGui::GetFrameHeight() + 4);
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if (const std::string* t = anchorText(oc)) {
                        if (t->empty()) ImGui::TextDisabled("(bez tekstu)");
                        else ImGui::TextUnformatted(t->c_str());
                    } else {
                        ImGui::TextDisabled("…");
                    }
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Pokaż")) showLink(oc.page, oc.idx);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Zmień…")) {
                        after = [this, oc] {
                            showLink(oc.page, oc.idx);
                            openLinkPopup(oc.page, oc.idx);
                            lk_.anchor = ImGui::GetIO().MousePos;
                        };
                    }
                    tip("Zmień tylko ten odnośnik");
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
                    if (ImGui::SmallButton("Usuń")) after = [this, oc] { cmdDeleteLink(oc.page, oc.idx); };
                    ImGui::PopStyleColor();
                    tip("Usuń ten odnośnik (tekst zostaje)");
                }
                ImGui::PopID();
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (after) after();
}

// ------------------------------------------------------------------------------- okna dialogowe

void App::drawModals() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    const ImGuiWindowFlags mf = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

    if (pwOpen_ && !ImGui::IsPopupOpen("Plik chroniony hasłem")) ImGui::OpenPopup("Plik chroniony hasłem");
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Plik chroniony hasłem", nullptr, mf)) {
        ImGui::TextUnformatted(std::filesystem::u8path(pwPath_).filename().u8string().c_str());
        if (!pwMsg_.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, kAccent); ImGui::TextUnformatted(pwMsg_.c_str()); ImGui::PopStyleColor(); }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        bool go = ImGui::InputText("Hasło", &pwBuf_, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
        go |= ImGui::Button("Otwórz");
        ImGui::SameLine();
        const bool skip = ImGui::Button("Pomiń plik");
        if (go) {
            const auto before = doc_.pages;
            const bool wasEmpty = doc_.empty();
            std::string err;
            const LoadResult r = doc_.appendFile(pwPath_, pwBuf_, err);
            if (r == LoadResult::Ok) {
                if (!wasEmpty) pushUndo(before);
                else fitNext_ = true;
                pwOpen_ = false;
                ImGui::CloseCurrentPopup();
                toast("Otworzono. Zapisany plik nie będzie chroniony hasłem.", 5);
            } else {
                pwMsg_ = err;
                pwBuf_.clear();
            }
        }
        if (skip) {
            pwOpen_ = false;
            ImGui::CloseCurrentPopup();
            if (doc_.empty()) {   // pominięty plik w nowej karcie: karta znika
                if (tabs_.size() > 1) closeTab(activeTab_);
                else { docName_.clear(); srcPath_.clear(); updateTitle(); }
            }
        }
        ImGui::EndPopup();
        if (!pwOpen_) processQueue();
    }

    if (discardOpen_ && !ImGui::IsPopupOpen("Niezapisane zmiany##discard")) ImGui::OpenPopup("Niezapisane zmiany##discard");
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Niezapisane zmiany##discard", nullptr, mf)) {
        ImGui::TextUnformatted("Bieżący dokument ma niezapisane zmiany.");
        ImGui::TextDisabled("Jeśli go zamkniesz, zmiany przepadną.");
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Zapisz najpierw")) { cmdSave(false); if (!dirty_) { discardOpen_ = false; ImGui::CloseCurrentPopup(); auto a = std::move(pendingAction_); if (a) a(); } }
        ImGui::SameLine();
        if (ImGui::Button("Odrzuć zmiany")) { discardOpen_ = false; dirty_ = false; ImGui::CloseCurrentPopup(); auto a = std::move(pendingAction_); if (a) a(); }
        ImGui::SameLine();
        if (ImGui::Button("Anuluj") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { discardOpen_ = false; pendingAction_ = nullptr; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (closeOpen_ && !ImGui::IsPopupOpen("Niezapisane zmiany##close")) ImGui::OpenPopup("Niezapisane zmiany##close");
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Niezapisane zmiany##close", nullptr, mf)) {
        std::vector<int> dirtyTabs;
        for (int i = 0; i < int(tabs_.size()); ++i) if (tabDirty(i)) dirtyTabs.push_back(i);
        ImGui::TextUnformatted("Zapisać zmiany przed zamknięciem?");
        if (tabs_.size() > 1) {
            if (dirtyTabs.size() > 1) ImGui::TextDisabled("Niezapisane zmiany są w %d dokumentach:", int(dirtyTabs.size()));
            else ImGui::TextDisabled("Niezapisane zmiany są w dokumencie:");
            for (int i : dirtyTabs) {
                const std::string& n = i == activeTab_ ? docName_ : tabs_[size_t(i)]->name;
                ImGui::BulletText("%s", n.empty() ? "Nowy dokument" : n.c_str());
            }
        }
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button(dirtyTabs.size() > 1 ? "Zapisz wszystkie" : "Zapisz")) {
            bool ok = true;
            for (int i : dirtyTabs) {
                switchTab(i);
                cmdSave(false);
                if (dirty_) { ok = false; break; }   // zapis przerwany (np. anulowane okno wyboru pliku)
            }
            if (ok) { quit_ = true; closeOpen_ = false; ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Zamknij bez zapisu")) { quit_ = true; closeOpen_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Anuluj") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { closeOpen_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

