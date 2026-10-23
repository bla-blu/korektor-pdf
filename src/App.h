#pragma once
#include "Document.h"
#include "Settings.h"
#include "imgui.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

// Interfejs Korektora PDF (Dear ImGui). Cały stan edytora siedzi tutaj;
// operacje na PDF wykonuje kpdf::Document.
class App {
public:
    App(GLFWwindow* window, ImFont* font, ImFont* bold = nullptr, ImFont* italic = nullptr);
    ~App();

    void frame();                                       // buduje interfejs na bieżącą klatkę
    void openPaths(const std::vector<std::string>& paths, bool newTabs);   // newTabs: każdy plik w nowej karcie, inaczej dołączenie
    void onDrop(const std::vector<std::string>& paths);
    void onCloseRequested();                            // przycisk zamknięcia okna
    bool wantsQuit() const { return quit_; }
    bool busy() const;                                  // czy potrzebne są kolejne klatki bez czekania na zdarzenia

private:
    enum class Tool { Select, Text, Pen, Highlight, Whiteout };
    enum class Drag { None, Move, Resize, Pen, Rect, TextSelect, TextWidth, TextBox };
    enum class Prop { None, Text, Pen, Highlight };
    struct Tex {
        unsigned id = 0;
        int w = 0, h = 0;
        float scale = 0;
        int rot = -1;
        int lastUse = 0;
        uint64_t version = 0;                 // wersja zmian treści strony w chwili renderowania
        std::vector<kpdf::Rect> boxes;        // obszary zmian treści (piksele), do odświeżania fragmentów
        double renderedAt = 0;
    };
    struct QueuedFile { std::string path; bool append; bool newTab; };

    // --- części interfejsu
    void drawMenuBar();
    void drawToolbar();
    void drawHelpWindows();
    void drawThumbs(float width, bool overlay);
    void drawThumbsHeader(float width);
    void drawThumbsOverlay(float width);
    void drawTabBar();
    void drawCanvas();
    void drawPage(kpdf::Page& p, const ImVec2& origin, ImDrawList* dl);
    void drawObj(const kpdf::Page& p, const kpdf::Obj& o, const ImVec2& origin, ImDrawList* dl, bool ghost);
    void drawTextCaret(const kpdf::Page& p, const ImVec2& origin, ImDrawList* dl);
    void drawStatusBar();
    void drawModals();
    void handleShortcuts();
    void handleTextKeys();

    // --- interakcja na stronie
    void beginPointer(kpdf::Page& p, kpdf::Vec2 b, bool dbl);
    void updateDrag();
    void startEdit(int pageId, int obj, bool fresh, int caretChar = -1);
    void finishEdit();
    int hitTest(const kpdf::Page& p, kpdf::Vec2 b) const;
    int hitHandle(const kpdf::Page& p, const ImVec2& origin, const ImVec2& mouse) const;   // narożnik 0..3 albo -1
    kpdf::Rect objBounds(const kpdf::Page& p, const kpdf::Obj& o) const;
    kpdf::Obj* editedObj();
    size_t caretFromPoint(const kpdf::Page& p, const kpdf::Obj& o, kpdf::Vec2 b) const;
    bool hitWidthHandle(const kpdf::Page& p, const ImVec2& origin, const ImVec2& mouse, int obj) const;
    ImVec2 widthHandlePos(const kpdf::Page& p, const ImVec2& origin, const kpdf::Obj& o) const;
    void replaceText(size_t a, size_t e, const std::string& str);   // bajty [a, e) -> str, w stylu pisania
    void insertText(const std::string& s);
    void eraseSelection();
    void caretMoved();                                             // styl pisania = styl znaku przed kursorem
    void applyStyle(const std::function<void(kpdf::TextStyle&)>& fn, bool undoStep = true);
    void setAlign(kpdf::Align a);
    void toggleBold();
    void toggleItalic();

    // --- polecenia
    void cmdOpen(bool append);
    void cmdSave(bool saveAs);
    void cmdInsertImage();
    void insertImage(const std::string& path, int pageId, const kpdf::Vec2* at = nullptr);
    void cmdDeleteSelection();
    void cmdRotateImage(int dir);
    void cmdReplaceImage();
    void cmdExportImage();
    void pruneIdleImages();
    int pickAt(kpdf::Page& p, kpdf::Vec2 b);                    // tekst albo obraz z pliku pod punktem -> nowy element modelu
    void syncFromSelection();
    void createTextAt(kpdf::Page& p, kpdf::Vec2 b);
    // menu kontekstowe i schowek elementów
    void openContextMenu(kpdf::Page& p, kpdf::Vec2 b);
    void drawContextMenu();
    void cmdCopy(bool cut);
    void cmdPaste(int pageId, const kpdf::Vec2* at);
    void cmdDuplicate();
    void cmdRestoreOriginal();
    void cmdZOrder(bool toFront);
    bool clipFromSelection(std::vector<kpdf::Obj>& out);
    void drawImagePanel(kpdf::Obj& o);
    // kształty z pliku (linie, prostokąty)
    void drawShapePanel(kpdf::Obj& o);
    void changeShape(const std::function<void(kpdf::Obj&)>& fn, bool step);
    void setShapeThickness(float t, bool step);
    bool shapeHasThickness(const kpdf::Obj& o) const;
    float shapeThickness(const kpdf::Obj& o) const;
    // odnośniki
    int hitLink(const kpdf::Page& p, kpdf::Vec2 b) const;               // indeks odnośnika pod punktem albo -1
    std::vector<int> linkGroup(const kpdf::Page& p, int idx) const;     // odnośnik i jego dalsze wiersze (ten sam cel)
    void openLinkPopup(int pageId, int idx);
    void cmdAddLink();                                                  // Ctrl+K: odnośnik dla zaznaczenia
    void cmdOpenLink(const kpdf::Link& l);
    void cmdDeleteLink(int pageId, int idx);
    void drawLinkPopup();
    void drawLinks(const kpdf::Page& p, const ImVec2& origin, ImDrawList* dl);
    std::vector<kpdf::Rect> selectionBoxes() const;                     // obszar zaznaczenia (wiersze tekstu albo element)
    void moveLinksWith(kpdf::Page& p, const kpdf::Rect& oldBounds, float dx, float dy);
    void dropLinksIn(kpdf::Page& p, const kpdf::Rect& bounds);
    // okno „Odnośniki w dokumencie”
    void drawLinkList();
    void rebuildLinkIndex();
    void showLink(int pageId, int idx);                       // przewija do odnośnika i na chwilę go wyróżnia
    void replaceUriEverywhere(const std::string& from, const std::string& to);
    void trackEditLinks();                    // odnośniki na edytowanym tekście: zakresy znaków
    void updateEditLinks();                   // obszary odnośników z bieżącego układu tekstu
    void shiftEditLinks(int ca, int ce, int n);
    void cmdBlankPage(int refPageId = -1, int where = 0);   // otwiera okno wyboru rozmiaru
    void cmdNewDocument();
    void cmdCloseDocument();
    void cmdExportPagePng(float dpi);
    void goToPage(int index);
    int currentPageId() const;
    void drawNewPageDialog();
    void insertNewPages();
    void pageOp(int pageId, int op);
    void setTool(Tool t);
    void setZoom(float z, const ImVec2* anchorScreen = nullptr);
    void processQueue();
    void confirmDiscard(std::function<void()> action);
    // karty
    struct TabStore;
    void swapTab(TabStore& t);
    void prepareTabSwitch();
    void switchTab(int i);
    void newTab();
    void closeTab(int i);              // bez pytania
    void requestCloseTab(int i);       // z pytaniem o niezapisane zmiany
    bool tabDirty(int i) const;
    int findOpenTab(const std::string& path) const;
    void freeTextures(std::unordered_map<int, Tex>& m);

    // --- cofanie
    void snapshot();
    void beginPropChange();   // krok cofania dla zmiany właściwości (w trakcie pisania — wspólny z edycją)
    void pushUndo(std::vector<kpdf::Page> before);
    void undo();
    void redo();
    void markDirty();

    // --- tekstury
    void ensurePageTex(const kpdf::Page& p, std::unordered_map<int, Tex>& cache, float scale, int& budget, bool partial);
    std::vector<kpdf::Rect> contentBoxes(const kpdf::Page& p, float scale) const;
    unsigned imageTex(const kpdf::ImagePtr& img);
    unsigned iconTex(const char* name, int px);
    void setWindowIcon();
    void dropTextures();
    void evictTextures();

    Prop propTarget() const;
    kpdf::Obj* selectedObj();
    kpdf::Color& propColor(Prop p);
    void toast(const std::string& msg, double seconds = 3.0);
    void updateTitle();

    GLFWwindow* window_;
    ImFont* font_;
    ImFont* fontBoldUi_ = nullptr;
    ImFont* fontItalicUi_ = nullptr;
    kpdf::Document doc_;
    std::string docName_ = "";
    std::string savePath_;
    std::string srcPath_;                // plik, z którego otwarto dokument (do wykrycia ponownego otwarcia)
    bool dirty_ = false;
    bool quit_ = false;

    Tool tool_ = Tool::Select;
    kpdf::Color colPen_{0.12f, 0.31f, 0.82f, 1}, colHl_{1.0f, 0.83f, 0.0f, 1};
    float sizePen_ = 2.f;
    kpdf::TextStyle textStyle_;          // styl nowego tekstu i pisania (przy kursorze)
    float textUnit_ = 1.f;               // skala tekstu, z którego wzięto styl (|up|): rozmiar widoczny = size × textUnit_
    bool pendingStyle_ = false;          // styl zmieniony przy pustym zaznaczeniu — dotyczy następnych znaków
    kpdf::Align align_ = kpdf::Align::Left;
    uint64_t overlapVer_ = 0;
    bool overlap_ = false;
    float zoom_ = 1.f;
    bool fitNext_ = true;
    float fbScale_ = 1.f;

    int selPage_ = -1, selObj_ = -1;
    std::vector<std::vector<kpdf::Page>> undo_, redo_;

    Drag drag_ = Drag::None;
    int dragPage_ = -1;
    kpdf::Vec2 dragStart_;
    kpdf::Obj dragOrig_, draft_;
    std::vector<kpdf::Page> dragBefore_;
    bool dragMoved_ = false;
    int dragCorner_ = 2;                 // przeciągany narożnik przy zmianie rozmiaru

    // edycja tekstu na stronie (kursor i zaznaczenie jako przesunięcia w bajtach UTF-8)
    int editPage_ = -1, editObj_ = -1;
    bool editFresh_ = false;
    size_t caret_ = 0, anchor_ = 0;
    double caretTime_ = 0;
    std::vector<kpdf::Page> editBefore_;
    kpdf::Obj editOrig_;

    // zmiana powiększenia z zachowaniem punktu pod kursorem
    float zoomRatio_ = 1.f;
    bool zoomAnchorPending_ = false, anchorWaitFrame_ = false;
    ImVec2 zoomAnchorScreen_{-1, -1}, anchorOff_{0, 0}, anchorContent_{0, 0};
    float prevScrollX_ = 0, prevScrollY_ = 0;

    bool colorEditActive_ = false;
    std::vector<kpdf::Page> sizeBefore_;
    bool sizeBeforeValid_ = false;

    std::unordered_map<int, std::pair<float, float>> pageOrigin_; // id -> pozycja na ekranie w tej klatce
    std::unordered_map<int, Tex> pageTex_, thumbTex_;
    std::unordered_map<uint64_t, Tex> imgTex_;
    struct IconTex { unsigned tex = 0; int px = 0; bool failed = false; kpdf::ImagePtr img; };
    std::unordered_map<std::string, IconTex> icons_;   // ikony paska narzędzi
    int frameNo_ = 0;
    bool pendingRender_ = false;

    int scrollToPage_ = -1;
    float scrollToX_ = 0, scrollToY_ = -1;   // miejsce na stronie docelowej (współrzędne bazowe), -1 = góra strony
    int currentPage_ = 0;

    std::string toast_;
    double toastUntil_ = 0;

    std::deque<QueuedFile> queue_;
    std::string pwPath_, pwBuf_, pwMsg_;
    bool pwAppend_ = false, pwOpen_ = false;
    std::function<void()> pendingAction_;
    bool discardOpen_ = false, closeOpen_ = false;
    std::vector<std::pair<int, int>> pageOps_;

    // okno „Nowa strona”
    struct NewPage {
        bool request = false;
        int preset = 1;            // indeks w kPaperSizes; -1 = jak bieżąca strona, -2 = własny
        bool landscape = false;
        float wMm = 210, hMm = 297;
        int unit = 0;              // 0 mm, 1 cm, 2 cale, 3 punkty
        int count = 1;
        int where = 0;             // 0 za stroną, 1 przed stroną, 2 na początku, 3 na końcu
        int refPage = -1;          // strona odniesienia (id)
    } np_;

    // menu kontekstowe
    enum class Ctx { Page, Object, Editing };
    bool ctxRequest_ = false;
    Ctx ctxKind_ = Ctx::Page;
    int ctxPage_ = -1;
    kpdf::Vec2 ctxBase_;
    // schowek elementów (wewnętrzny; tekst trafia też do schowka systemowego)
    std::vector<kpdf::Obj> clip_;
    // menu główne, ustawienia, okna pomocy
    kpdf::Settings settings_;
    bool fitPageNext_ = false;
    bool showShortcuts_ = false, showAbout_ = false, goToOpen_ = false;
    int goToValue_ = 1;
    int clipPage_ = -1;
    // okienko odnośnika (pojawia się po kliknięciu w odnośnik albo Ctrl+K)
    struct LinkUi {
        bool request = false;
        int page = -1;                       // id strony
        std::vector<int> idx;                // edytowane odnośniki (dalsze wiersze tego samego odnośnika)
        std::vector<kpdf::Rect> newBoxes;    // nowy odnośnik: obszary do utworzenia
        int kind = 0;                        // 0 adres www / e-mail, 1 strona dokumentu
        std::string uri;
        int pageNo = 1;                      // numer strony docelowej (od 1)
        ImVec2 anchor{0, 0};                 // miejsce na ekranie (pod odnośnikiem)
        bool focus = false;
    } lk_;
    struct LinkPress { int page = -1, idx = -1; ImVec2 pos{0, 0}; double time = 0; bool released = false; } linkPress_;
    int hoverLinkPage_ = -1, hoverLink_ = -1;
    double hoverLinkSince_ = 0;
    bool linkPopupOpen_ = false;
    // odnośniki leżące na edytowanym tekście idą za swoimi znakami
    struct EditLink { kpdf::Link target; std::vector<int> members; int c0 = 0, c1 = 0; };   // members: indeksy w editLinksOrig_
    std::vector<EditLink> editLinks_, editLinks0_;
    std::vector<kpdf::Link> editLinksOrig_;
    int ctxLink_ = -1;                   // odnośnik pod prawym kliknięciem
    // lista odnośników: wystąpienia pogrupowane według adresu (albo strony docelowej)
    struct LinkOcc { int page = -1, idx = -1; };                 // wystąpienie = odnośnik z dalszymi wierszami
    struct LinkEntry {
        std::string key;                                       // adres albo "#<id strony docelowej>"
        std::string uri, shown;                                 // adres w pliku i do pokazania
        bool internal = false;
        int target = -1;                                       // odnośnik wewnętrzny: id strony docelowej
        std::vector<LinkOcc> occ;
        std::string pages;                                      // numery stron, na których występuje
    };
    struct LinkList {
        bool open = false;
        uint64_t sig = 0;                                       // skrót odnośników dokumentu (przebudowa przy zmianie)
        std::vector<LinkEntry> entries;
        int total = 0;
        std::string filter, replace;
        bool showInternal = false;
        std::unordered_map<std::string, bool> expanded;
        std::string editKey, editBuf;                           // pole adresu w trakcie edycji
        std::unordered_map<std::string, std::string> text;      // tekst pod odnośnikiem (pamięć podręczna)
    } ll_;
    struct Flash { int page = -1, idx = -1; double until = 0; } flash_;
    int pasteCount_ = 0;

    // --- karty: stan aktywnego dokumentu jest w polach powyżej, nieaktywne czekają w TabStore
    struct TabStore {
        uint64_t uid = 0;
        kpdf::Document doc;
        std::string name, savePath, srcPath;
        bool dirty = false;
        int selPage = -1, selObj = -1;
        std::vector<std::vector<kpdf::Page>> undo, redo;
        float zoom = 1.f;
        bool fitNext = true, fitPageNext = false;
        std::unordered_map<int, Tex> pageTex, thumbTex;
        int currentPage = 0;
        float scrollX = 0, scrollY = 0;
        int scrollToPage = -1;
        float scrollToX = 0, scrollToY = -1;
        LinkList ll;
    };
    std::vector<std::unique_ptr<TabStore>> tabs_;   // tabs_[activeTab_] to pusty schowek aktywnej karty
    int activeTab_ = 0;
    uint64_t nextTabUid_ = 1;
    int tabSelectFrames_ = 0;            // ImGui zaznacza kartę programowo z opóźnieniem jednej klatki
    int wantTab_ = -1, closeTabReq_ = -1;
    float tabScrollX_ = 0, tabScrollY_ = 0;
    bool restoreScroll_ = false;
    uint64_t clipTabUid_ = 0;            // karta, z której skopiowano elementy

    // --- panel miniatur: przypięty, automatycznie ukrywany (wysuwa się nad stronę) albo zamknięty
    float thumbsAnim_ = 0;               // 0 schowany, 1 wysunięty
    bool thumbsOpen_ = false;
    double thumbsHoverT_ = 0, thumbsLeaveT_ = 0;
    ImVec2 canvasMin_{0, 0}, canvasMax_{0, 0};
};
