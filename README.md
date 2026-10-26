# Korektor PDF

Szybki i prosty edytor PDF w C++17. Działa na Windows, macOS i Linuksie (x64 i ARM64).

- **Poprawianie całych akapitów jak w edytorze tekstu.** Narzędziem Tekst klikasz akapit w PDF-ie. Program rozpoznaje, które wiersze do niego należą, oraz jego szerokość, wyrównanie (do lewej, do środka, do prawej, justowanie), interlinię i wcięcie pierwszego wiersza. Wyrazy przeniesione z dywizem łączy z powrotem. Potem piszesz w miejscu, a wiersze same się zawijają. To, co widać na ekranie, renderuje ten sam silnik, który zapisuje plik.
- **Formatowanie zostaje.** Każdy znak pamięta swoją czcionkę, rozmiar i kolor, więc pogrubione, pochylone czy kolorowe słowa w środku akapitu zostają takie, jakie były. Nowe znaki dostają styl znaku przed kursorem.
- **Formatowanie zaznaczenia:** pogrubienie (Ctrl+B), kursywa (Ctrl+I), kolor, rozmiar i krój. Bez zaznaczenia zmiana dotyczy dalszego pisania. Pogrubienie najpierw szuka pogrubionej wersji czcionki w samym pliku, a dopiero gdy jej nie ma, sięga po czcionkę zastępczą.
- **Ramka akapitu:** uchwyt z prawej strony zmienia szerokość, przyciski — wyrównanie. Przeciągnięcie narzędziem Tekst po pustym miejscu tworzy nowy akapit o zadanej szerokości. Gdy akapit po zmianach nachodzi na tekst obok, ramka robi się czerwona, a pasek stanu to podpowiada.
- **Czcionka z pliku zostaje**, jeśli ma wszystkie potrzebne znaki. Gdy brakuje któregoś (np. „ą” w czcionce osadzonej tylko z użytymi literami), dany styl przechodzi na podobną czcionkę zastępczą (bezszeryfowa, szeryfowa albo o stałej szerokości, z pogrubieniem i kursywą), zwężoną tak, by pasowała szerokością do oryginału. Pasek stanu pokazuje czcionkę przy kursorze.
- **Nowy tekst** z polskimi znakami: wybór rodziny, pogrubienia, kursywy, rozmiaru i koloru. W pliku jest prawdziwym tekstem, który da się zaznaczyć i wyszukać.
- **Zamazywanie z usunięciem treści.** Spod prostokąta znikają z pliku litery (sąsiednie zostają dokładnie na swoim miejscu), piksele obrazów i grafika leżąca w całości pod spodem. Działa też na warstwę tekstu w skanach i na treść osadzonych grafik.
- **Zaznaczanie jednym kliknięciem, edycja dwuklikiem** (narzędzie Zaznacz): kliknięcie w akapit z pliku zaznacza go w całości — można go przesunąć, usunąć, sformatować z paska (dotyczy całego akapitu) albo zmienić szerokość. Samo zaznaczenie niczego nie zmienia: strona zostaje identyczna co do piksela, dopóki czegoś nie zmienisz. Dwuklik otwiera edycję z kursorem w miejscu kliknięcia.
- **Logo programu** na pasku narzędzi, w oknie „O programie”, jako ikona okna i paska zadań oraz ikona pliku .exe w Windows.
- **Pasek narzędzi z ikonami:** Otwórz, Dołącz, Zapisz (czerwona kropka = niezapisane zmiany), Zapisz jako, narzędzia (Zaznacz, Tekst, Pióro, Zakreśl, Zamaż), Obraz, Odnośniki, Cofnij, Ponów, Usuń. Najechanie pokazuje nazwę i skrót klawiszowy; Widok → Podpisy przy ikonach dodaje nazwy obok ikon. Ikony są osadzone w programie i skalowane do rozdzielczości ekranu, więc są ostre także przy powiększeniu systemowym (HiDPI). Przyciski powiększenia są w prawym dolnym rogu okna.
- **Karty — kilka dokumentów naraz.** Każdy otwarty plik ma własną kartę nad stroną: z własnym powiększeniem, miejscem przewinięcia, zaznaczeniem, historią cofania i stanem zapisu (kropka przy nazwie = niezapisane zmiany, najechanie pokazuje ścieżkę). Ctrl+O, „Otwórz”, przeciągnięcie plików na okno i lista ostatnich plików otwierają nowe karty (przeciągnięcie z Ctrl dołącza strony do bieżącego dokumentu); plik już otwarty nie otwiera się drugi raz, tylko program przechodzi do jego karty. Przełączanie: kliknięcie, Ctrl+Tab / Ctrl+Shift+Tab albo Ctrl+PgDn / Ctrl+PgUp; strzałka po lewej pokazuje listę wszystkich kart, a karty można przestawiać przeciąganiem. Zamykanie: ×, środkowy przycisk myszy albo Ctrl+W (przy niezapisanych zmianach program zapyta). Przy zamykaniu programu okno wymienia dokumenty z niezapisanymi zmianami i pozwala zapisać wszystkie naraz. Elementy skopiowane w jednej karcie można wkleić w innej.
- **Panel miniatur: przypięty, chowany albo zamknięty.** Nagłówek panelu ma pinezkę i ×. Odpięcie pinezki włącza automatyczne ukrywanie: panel znika, a strona zajmuje całą szerokość; najechanie myszą na lewą krawędź (jest tam wąski uchwyt) wysuwa miniatury nad stroną, a odsunięcie myszy chowa je z powrotem. × zamyka panel całkowicie; przywraca go F4 albo Widok → Miniatury stron. Tryb wybiera też Widok → Automatyczne ukrywanie miniatur; wybór jest zapamiętywany.
- **Menu główne:** Plik (nowy, otwórz, ostatnio otwierane, dołącz, zapisz, eksport bieżącej strony do PNG w 72/150/300 dpi, zamknij), Edycja, Widok (powiększenie, 100%, dopasowanie do szerokości i całej strony, nawigacja po stronach, przejdź do strony, miniatury, pasek właściwości), Wstaw, Narzędzia, Strona (obrót, kolejność, puste strony, usuwanie) i Pomoc (skróty klawiszowe, o programie). Lista ostatnich plików i ustawienia widoku są zapamiętywane między uruchomieniami.
- **Kształty z pliku: linie, prostokąty, ramki, krzywe.** Kliknięcie zaznacza kształt (także wewnątrz osadzonej grafiki, np. logo). Można go przesunąć, zmienić rozmiar za narożnik (Shift zachowuje proporcje), usunąć, skopiować — także na inną stronę — i zmienić wygląd z paska albo menu kontekstowego: wypełnienie (kolor, włącz/wyłącz), kontur (kolor, włącz/wyłącz) i grubość. Linie, które w czasopismach są cienkimi wypełnionymi paskami, program rozpoznaje jako linie: przeciąganie narożnika zmienia tylko ich długość, a „Grubość” — grubość. Kontur przy rozciąganiu zachowuje grubość. Kształt tylko przesunięty albo przeskalowany zachowuje w pliku swoją przestrzeń barw i obcięcie. Tła zajmujące większość strony nie są zaznaczane, żeby nie przeszkadzały w klikaniu.
- **Odnośniki (linki).** Odnośniki do stron www i adresów e-mail oraz do innych stron dokumentu mają cienką niebieską ramkę (można ją wyłączyć: Widok → Obramowanie odnośników). Najechanie pokazuje adres, kliknięcie otwiera okienko, w którym zmienisz adres albo stronę docelową, otworzysz odnośnik, skopiujesz adres lub usuniesz odnośnik. Ctrl+klik od razu otwiera odnośnik. Nowy odnośnik powstaje z zaznaczenia (Ctrl+K, Wstaw → Odnośnik… albo menu kontekstowe) — z akapitu, obrazu albo fragmentu edytowanego tekstu; zaznaczony adres www lub e-mail podpowiada się sam. Adres można wpisać bez `https://` i z polskimi znakami — program go uzupełni i zakoduje. Odnośnik złamany na kilka wierszy zmienia się w całości. Odnośniki idą za tekstem: przy przesuwaniu akapitu lub obrazu i przy pisaniu w akapicie (gdy wiersze się przeleją, ramka odnośnika przenosi się razem ze swoimi słowami). Odnośniki do stron dokumentu wskazują właściwą stronę także po zmianie kolejności stron, a „Przejdź” przewija do wskazanego miejsca na stronie (np. do pozycji w bibliografii); odnośnik do usuniętej strony jest zaznaczony na czerwono i nie trafia do pliku. Odnośnik pod zamazaniem znika z pliku razem z treścią.
- **Lista odnośników** (przycisk „Odnośniki”, Ctrl+L, Narzędzia → Odnośniki w dokumencie…): osobne okno, które nie blokuje pracy. Odnośniki są pogrupowane według adresu — ten sam adres w wielu miejscach to jeden wiersz z liczbą wystąpień i numerami stron, a zmiana adresu w polu (Enter albo kliknięcie obok) obejmuje wszystkie wystąpienia. Strzałka rozwija pojedyncze wystąpienia: „Pokaż” przewija stronę do odnośnika i na chwilę go wyróżnia, „Zmień…” zmienia tylko to jedno, „Usuń” usuwa (tekst zostaje). Kolumna „Tekst odnośnika” pokazuje napis pod odnośnikiem. Wyszukiwanie zawęża listę, a „Zamień” podmienia znaleziony fragment we wszystkich pokazanych adresach naraz (np. `http://` na `https://` albo starą domenę na nową) — jednym krokiem cofania. Odnośniki do stron dokumentu (np. do bibliografii) są domyślnie ukryte; pokazuje je pole „Odnośniki do stron dokumentu”.
  Okno nie spowalnia programu: zamknięte nic nie kosztuje, lista jest przebudowywana tylko po zmianie odnośników, rysowane są wyłącznie widoczne wiersze, a tekst spod odnośników jest odczytywany po kilka na klatkę i zapamiętywany.
- **Menu kontekstowe** (prawy przycisk) dopasowane do tego, co jest pod kursorem:
  - tekst: edycja, kopiowanie tekstu, pogrubienie, kursywa, kolor, rozmiar, krój, wyrównanie,
  - obraz: obrót, podmiana, zapis jako PNG, na wierzch / pod spód (wstawione obrazy),
  - kształt: kolor wypełnienia, kolor konturu (albo bez), grubość,
  - pióro i zakreślenie: kolor, grubość,
  - podczas pisania: wytnij, kopiuj, wklej, zaznacz wszystko i formatowanie zaznaczenia,
  - odnośnik: zmień, otwórz, kopiuj adres, usuń; przy tekście i obrazie także „Dodaj odnośnik…”,
  - pusta część strony: wklej tutaj, wstaw tekst albo obraz w tym miejscu, obróć, wstaw lub usuń stronę,
  - a przy każdym elemencie: wytnij, kopiuj, wklej, duplikuj, przywróć oryginał z pliku, usuń.
- **Schowek elementów:** Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D kopiują, wycinają, wklejają i duplikują zaznaczony element — także na inną stronę. Kopia obrazu z pliku wygląda dokładnie jak oryginał, kopia akapitu zachowuje formatowanie; tekst trafia też do schowka systemowego.
- **Obrazy z pliku:** kliknięcie zaznacza obraz (także leżący wewnątrz osadzonej grafiki). Można go przesunąć, zmienić rozmiar za dowolny narożnik (proporcje zostają, Shift je zwalnia), obrócić o 90°, usunąć, podmienić na inny plik albo zapisać jako PNG w oryginalnej rozdzielczości. Pasek pokazuje rozmiar w mm, w pikselach i rozdzielczość w dpi. Przesuwany obraz ma podgląd na żywo, a zapisany plik zachowuje oryginalne dane obrazu (bez ponownej kompresji).
- **Zapis bez psucia oryginału.** Strona ze zmianami nie jest przepisywana od nowa: wszystko, czego nie zmieniłeś, trafia do pliku w oryginalnej postaci — kolory CMYK i dodatkowe (np. Pantone), profile ICC, maski przezroczystości i płynne zanikanie, nadruk, odstępy i kerning tekstu, obrazy wbudowane, cieniowania, warstwy i znaczniki struktury. Elementy przesunięte albo ze zmienionym stylem zachowują resztę wyglądu (np. kontur CMYK przy nowym wypełnieniu). Przy zmianach wewnątrz osadzonych grafik (np. zdjęć z maską zanikania w plikach z InDesign) program odtwarza ich grupy i maski, a obraz z zamazanymi pikselami rysuje z maską i obcięciem oryginału. Bez tego PDFium, które przepisuje stronę po każdej zmianie, zapisywało kolory CMYK jako czarne, gubiło maski, obrazy wbudowane i odstępy tekstu.
- **Czysty zapis:** usunięte obrazy, zamazane fragmenty i zastąpione czcionki nie zostają w pliku jako niewidoczne obiekty — program zapisuje tylko to, czego strony faktycznie używają.
- **Pióro** do odręcznych adnotacji i podpisów, **zakreślacz**.
- **Obrazy** PNG, JPG, BMP i GIF, także przeciągnięte na okno.
- **Strony:** obracanie, usuwanie, zmiana kolejności (przyciski lub przeciąganie miniatur).
- **Nowe puste strony dowolnego rozmiaru** (przycisk „+ Pusta strona…”, menu kontekstowe strony, Ctrl+Shift+N): formaty A0–A6, B4–B6, Letter, Legal, Tabloid, Executive, koperty DL/C5/C6, pocztówka, wizytówka, slajdy 16:9 i 4:3, „jak bieżąca strona” albo własny rozmiar w mm, cm, calach lub punktach. Do tego orientacja, liczba stron i miejsce (za lub przed stroną, na początku, na końcu) oraz podgląd proporcji.
- **Scalanie:** „Dołącz…” dokleja strony z kolejnych plików.
- Cofanie i ponawianie, pliki chronione hasłem, ostrzeżenie przed zamknięciem z niezapisanymi zmianami.

Edycje są przechowywane jako osobne obiekty: do zapisu można je przesuwać, zmieniać i cofać. Przy zapisie program buduje nowy plik PDF i wpisuje zmiany w treść stron. Oryginał zostaje nietknięty, chyba że zapiszesz pod tą samą nazwą.

## Budowanie

Potrzebne: CMake 3.20+, kompilator C++17 (MSVC 2019+, GCC 9+, Clang 10+) i dostęp do sieci przy pierwszej konfiguracji. CMake sam pobiera PDFium, Dear ImGui, GLFW, nativefiledialog-extended, stb i czcionki DejaVu (Sans, Serif, Mono), które osadza w programie.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release        # test rdzenia (bez okna)
```

Program pojawi się w `build/` (w Visual Studio: `build/Release/`), razem z biblioteką PDFium (`pdfium.dll`, `libpdfium.so` albo `libpdfium.dylib`). Do przeniesienia na inny komputer:

```sh
cmake --install build --config Release --prefix dist   # dist/bin: program + PDFium
```

### Wersja przenośna dla Windows (Visual Studio)

1. Otwórz folder projektu w Visual Studio (Plik → Otwórz → Folder).
2. Na liście konfiguracji u góry wybierz **x64 Release (wersja do przenoszenia)**.
3. Kompiluj → **Zainstaluj Korektor PDF**.
4. Gotowy folder: `out\install\x64-release\bin` — `korektor.exe`, `pdfium.dll` i `CZYTAJ.md`. Ten folder (albo jego ZIP) kopiujesz na dowolny komputer z Windows 10/11 64-bit; nic nie trzeba instalować.

Program jest budowany ze statyczną biblioteką uruchomieniową (opcja `KOREKTOR_STATIC_RUNTIME`, domyślnie włączona), więc nie potrzebuje „Microsoft Visual C++ Redistributable”. `pdfium.dll` musi leżeć obok `korektor.exe`. Wersja Debug służy tylko do pracy nad programem — wymaga bibliotek debugowania z Visual Studio i nie uruchomi się na innym komputerze. Ustawienia (ostatnie pliki, widok) są zapisywane w `%APPDATA%\KorektorPDF` na każdym komputerze osobno. Plik nie jest podpisany cyfrowo, więc przy pierwszym uruchomieniu Windows może pokazać ostrzeżenie SmartScreen („Więcej informacji” → „Uruchom mimo to”).

### Pakiety systemowe

| System | Polecenie |
|---|---|
| Windows | Visual Studio z „Programowanie aplikacji klasycznych w C++” (zawiera CMake). Otwórz folder w VS albo użyj poleceń wyżej w „Developer PowerShell”. |
| macOS | `xcode-select --install` oraz `brew install cmake` |
| Debian / Ubuntu | `sudo apt install build-essential cmake ninja-build git libgtk-3-dev libgl1-mesa-dev xorg-dev libwayland-dev libxkbcommon-dev wayland-protocols` |
| Fedora | `sudo dnf install gcc-c++ cmake ninja-build git gtk3-devel mesa-libGL-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel wayland-devel libxkbcommon-devel wayland-protocols-devel` |

GTK jest potrzebny na Linuksie tylko do systemowych okien „Otwórz” i „Zapisz”.

### macOS (Apple Silicon i Intel)

Potrzebne: macOS 13 lub nowszy, narzędzia Apple (`xcode-select --install`, pełny Xcode nie jest konieczny) i CMake (`brew install cmake ninja`).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
open "build/Korektor PDF.app"
cmake --install build --prefix dist      # gotowy pakiet: dist/bin/Korektor PDF.app
```

Na Macu z procesorem M (M1, M2, …) powstaje wersja natywna, a z `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` — jedna wersja dla Maców z procesorami Apple i Intel.

Wynikiem jest pakiet `Korektor PDF.app`: ikona z logo, biblioteka PDFium w `Contents/Frameworks`, obsługa ekranów Retina, „Otwórz za pomocą → Korektor PDF” w Finderze i upuszczanie plików PDF na ikonę w Docku. Pakiet jest podpisany ad hoc (bez konta Apple) — tego wymagają Maki z procesorem M. Przy konfiguracji program poprawia nazwę instalacyjną gotowej biblioteki PDFium (`./libpdfium.dylib` → `@rpath/libpdfium.dylib`), bez czego aplikacja uruchomiona z Findera nie znalazłaby biblioteki.

**Pierwsze uruchomienie pobranej aplikacji.** macOS blokuje programy z internetu bez podpisu Apple. Po pierwszej próbie otwarcia wejdź w Ustawienia systemowe → Prywatność i ochrona → „Otwórz mimo to” albo zdejmij znacznik pobrania: `xattr -dr com.apple.quarantine "/Applications/Korektor PDF.app"`. Do swobodnego rozpowszechniania potrzebny jest podpis Developer ID i notaryzacja (Apple Developer Program).

Ikona `assets/korektor.icns` powstaje z logo: `python3 tools/make_icns.py assets/icons/logo.png assets/korektor.icns` (wymaga Pillow).

### Kompilacja skrośna na Windows (z Linuksa, MinGW)

Narzędzie osadzające czcionki i ikony musi działać na komputerze budującym, więc najpierw zbuduj projekt natywnie (choćby sam rdzeń), a potem wskaż to narzędzie (po aktualizacji źródeł zbuduj je ponownie — wersja 0.10 dodała osadzanie ikon):

```sh
cmake -S . -B build-host -DKOREKTOR_BUILD_GUI=OFF && cmake --build build-host --target korektor_embed
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=<twój plik toolchain MinGW> \
      -DKOREKTOR_EMBED_TOOL=$PWD/build-host/korektor_embed
cmake --build build-win
```

### Bez internetu

Każdą zależność można wskazać lokalnie. `PDFIUM_DIR` to rozpakowane archiwum z github.com/bblanchon/pdfium-binaries:

```sh
cmake -S . -B build \
  -DPDFIUM_DIR=/ścieżka/pdfium \
  -DFETCHCONTENT_SOURCE_DIR_IMGUI=/ścieżka/imgui \
  -DFETCHCONTENT_SOURCE_DIR_GLFW=/ścieżka/glfw \
  -DFETCHCONTENT_SOURCE_DIR_NFD=/ścieżka/nativefiledialog-extended \
  -DFETCHCONTENT_SOURCE_DIR_STB=/ścieżka/stb \
  -DKOREKTOR_FONT_DIR=/ścieżka/dejavu/ttf
```

### Automatyczne kompilacje

`.github/workflows/build.yml` buduje i testuje program na Linuksie (x64, ARM64), Windows i macOS. Dla macOS powstaje wersja uniwersalna (natywna na Makach z procesorem M i z Intelem), budowana na maszynie z Intelem (`macos-15-intel`), bo te są łatwo dostępne; dodatkowe zadanie na Macu z procesorem M (`macos-15`) testuje część ARM, ale bywa odrzucane z braku wolnych maszyn GitHuba — nie oznacza to błędu. Gotowe paczki trafiają do artefaktów przebiegu (zakładka Actions → przebieg → Artifacts). Wystarczy wypchnąć repozytorium na GitHub; dla repozytorium publicznego kompilacja jest bezpłatna. Przebieg można też uruchomić ręcznie (Actions → build → Run workflow).

Dla macOS artefakt `korektor-pdf-macos-universal` zawiera obraz dysku `korektor-pdf-macos-universal.dmg` (aplikacja do przeciągnięcia do folderu Programy) oraz — jeśli maszyna GitHuba ma grafikę — zrzut okna `zrzut-….png` z próbnego uruchomienia programu. Przebieg sprawdza też pakiet: Info.plist, zależności i rpath, nazwę instalacyjną PDFium i podpis.

## Obsługa

| Klawisz | Działanie |
|---|---|
| V / T / P / H / W | zaznacz / tekst / pióro / zakreśl / zamaż |
| Narzędzie Tekst, klik w tekst pliku | poprawianie akapitu w miejscu |
| Narzędzie Tekst, klik / przeciągnięcie w wolnym miejscu | nowy tekst / nowy akapit o szerokości przeciągnięcia |
| Podczas pisania | strzałki, Home/End (wiersz), Ctrl+Home/End (akapit), Shift — zaznaczanie, Ctrl+strzałki — po słowach, dwuklik — słowo, Ctrl+A/C/X/V, Ctrl+B / Ctrl+I — pogrubienie / kursywa, Enter — nowy akapit, Ctrl+Z — cofnięcie bieżącej edycji, Esc lub Ctrl+Enter — koniec |
| Klik (Zaznacz) | zaznaczenie akapitu lub obrazu z pliku; przeciąganie przesuwa, narożniki obrazu zmieniają rozmiar |
| Dwuklik (Zaznacz) | edycja tekstu z kursorem w miejscu kliknięcia |
| Prawy przycisk | menu kontekstowe |
| Klik w odnośnik (Zaznacz) / Ctrl+klik | okienko odnośnika (zmiana, otwarcie, usunięcie) / otwarcie odnośnika |
| Ctrl+K | nowy odnośnik z zaznaczonego akapitu, obrazu lub fragmentu tekstu |
| Ctrl+L | lista odnośników w dokumencie |
| Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D | kopiuj / wytnij / wklej / duplikuj zaznaczony element |
| Strzałki (z Shift) | przesunięcie zaznaczenia o 1 pt (10 pt) |
| Delete | usunięcie zaznaczonego elementu (dla tekstu z pliku: usunięcie linii) |
| Ctrl+Z / Ctrl+Y | cofnij / ponów |
| Ctrl+O / Ctrl+Shift+O | otwórz w nowej karcie / dołącz pliki |
| Ctrl+Shift+N | nowa pusta strona (wybór rozmiaru) |
| Ctrl+N / Ctrl+W / Ctrl+Q | nowy dokument / zamknij kartę / zakończ |
| Ctrl+Tab / Ctrl+Shift+Tab (Ctrl+PgDn / Ctrl+PgUp) | następna / poprzednia karta |
| Ctrl+1 / Ctrl+2 | powiększenie 100% / cała strona |
| PgUp / PgDn, Home / End, Ctrl+G | poprzednia / następna, pierwsza / ostatnia strona, przejdź do strony |
| Ctrl+R / Ctrl+Shift+R | obróć bieżącą stronę w prawo / w lewo |
| F1 / F4 | lista skrótów / pokaż lub ukryj miniatury stron |
| Ctrl+S / Ctrl+Shift+S | zapisz / zapisz jako |
| Ctrl+I | wstaw obraz |
| Ctrl+kółko, Ctrl+= / Ctrl+− / Ctrl+0 | powiększenie / dopasowanie do szerokości (przyciski powiększenia są w prawym dolnym rogu) |

Na macOS zamiast Ctrl działa Cmd. Plik PDF można też podać przy uruchomieniu: `korektor plik.pdf`.

## Budowa kodu

```
src/Document.*      model dokumentu i praca z PDF (PDFium): wczytywanie, żywy podgląd zmian,
                    rozpoznawanie akapitów ze stylami, zawijanie wierszy, obrazy z pliku, zapis
src/Redact.cpp      zamazywanie z usunięciem treści (litery, piksele obrazów, grafika)
src/ContentSplice.* zapis strony z oryginalnych bajtów: czytnik PDF, interpreter strumienia treści (stan
                    graficzny każdego obiektu), składanie treści i zasobów, otoczki osadzonych grafik
src/PdfUtil.h       wewnętrzne narzędzia rdzenia (przekształcenia, czcionki, nakładanie zmian)
src/Fonts.*         osadzone czcionki DejaVu
src/ImageLoad.*     wczytywanie obrazów (stb_image)
src/App.*           interfejs (Dear ImGui): karty, narzędzia, pisanie na stronie, miniatury, cofanie
src/Settings.*      ustawienia zapamiętywane między uruchomieniami (ostatnie pliki, widok)
src/Icons.*         ikony paska narzędzi (osadzone pliki z assets/icons)
assets/icons/       ikony PNG 128×128 (logo 256×256) — podmiana pliku i przebudowanie zmienia ikonę
assets/korektor.ico ikona pliku .exe w Windows (src/korektor.rc)
src/main.cpp        okno GLFW + OpenGL 3 i pętla programu
src/MacOpen.*       macOS: pliki otwierane z Findera i z ikony w Docku
cmake/Info.plist.in opis pakietu aplikacji macOS (wersja, ikona, dokumenty PDF)
tools/embed_fonts.cpp  narzędzie budowania: czcionki TTF i ikony PNG -> tablice C++
tests/selftest.cpp  test rdzenia: obroty stron, poprawianie tekstu (czcionka z pliku i zastępcza,
                    położenie), akapity (style słów, wcięcie, dywiz, justowanie, zawijanie),
                    zamazywanie (tekst częściowo i w całości, osadzona grafika, obraz),
                    obrazy z pliku (wybór, przesunięcie, skala, obrót, usunięcie, podmiana, eksport),
                    zaznaczenie bez zmian (render identyczny), przesunięcie akapitu, kopia obrazu,
                    puste strony, kształty (wybór, przesunięcie, kolor, usunięcie, skala konturu,
                    osadzona grafika, kopia na inną stronę), odnośniki (odczyt, zmiana, nowe, wewnętrzne po zmianie
                    kolejności stron, pod zamazaniem, do usuniętej strony, ponowny zapis), zachowanie
                    oryginalnej treści po zmianach (CMYK, kolor dodatkowy, maska, nadruk, tekst z odstępami,
                    obraz wbudowany, cieniowanie, warstwa; strona poza zmianami identyczna co do piksela)
```

Rdzeń (`korektor_core`) nie zależy od interfejsu. Można go użyć w innym programie albo w narzędziu wsadowym (`-DKOREKTOR_BUILD_GUI=OFF`).

**Jak działa podgląd WYSIWYG.** Dla każdej zmienionej strony program trzyma jej żywą kopię w PDFium i nakłada na nią zmiany treści (poprawiony i nowy tekst, zamazania). Ta sama funkcja nakłada je przy zapisie, więc ekran i plik się zgadzają. Podczas pisania odświeżany jest tylko zmieniony fragment strony. Zawijanie, kursor i zaznaczenie liczy jedna funkcja układu akapitu, z szerokości znaków w tych samych czcionkach, których użyje plik — dlatego podgląd, kursor i zapisany plik się zgadzają.

## Ograniczenia

- Akapit poprawiony w edytorze nie przesuwa treści poniżej: gdy urośnie, nachodzi na nią (program to pokazuje). Trzeba go skrócić, poszerzyć ramkę albo przesunąć.
- Rozpoznawanie akapitów opiera się na geometrii wierszy (ten sam rozmiar, równy odstęp, wspólna krawędź). W nietypowych układach — tabele, wiele kolumn blisko siebie, tekst wokół obrazów — może złapać za dużo albo za mało; wtedy pojedyncze wiersze nadal można poprawiać osobno.
- Zawijanie dzieli tekst tylko na spacjach (bez automatycznego dzielenia wyrazów). Ręczne odstępy między literami (kerning) z oryginału nie są zachowywane w poprawionym akapicie; odstępy między wyrazami są liczone na nowo.
- Tekstu wewnątrz osadzonych grafik (Form XObject) i niewidocznej warstwy tekstu skanów nie da się poprawiać — program to komunikuje. Taki tekst można zamazać i wpisać nowy.
- Tekst w czcionce zastępczej osadza w pliku pełną czcionkę DejaVu (300–750 KB na każdą użytą odmianę). Tekst w czcionce z pliku nie zwiększa rozmiaru.
- Zamazywanie usuwa grafikę wektorową tylko wtedy, gdy leży w całości pod prostokątem; częściowo zakryte linie i kształty zostają (zakryte bielą). Obraz, z którego usunięto piksele, jest zapisywany bez stratnej kompresji, więc zdjęcia JPEG mogą urosnąć. Adnotacje i pola formularzy nie są zamazywane.
- Osadzone grafiki, które zachodzą na zamazanie, są rozkładane na zwykłe obiekty strony; ich obrys przycinający przepada.
- Obraz przesunięty poza swoją maskę przycinającą przesuwa ją razem z sobą; obrazy wewnątrz osadzonej grafiki są przy zmianie przenoszone na stronę, więc tracą jej obrys przycinający.
- Obrót obrazów jest co 90°; dowolnego kąta i przycinania obrazu na razie nie ma.
- Kształty: zmiana koloru zapisuje go jako RGB (wzory i kolory specjalne z oryginału zastępuje zwykłym kolorem); kontur rozciągnięty nierówno jest zapisywany jako nowa ścieżka bez obcięcia z oryginału. Logo złożone z wielu kształtów zaznacza się kawałkami; nowych linii i prostokątów nie da się jeszcze rysować.
- Zapis tworzy nowy plik ze stron dokumentu: zakładki (spis treści w panelu przeglądarki), dane formularzy i ustawienia warstw (które warstwy są ukryte) z oryginału nie są przenoszone.
- Nowe i poprawione elementy (wpisany albo poprawiony tekst, wstawione obrazy, pióro, nowy kolor kształtu) są zapisywane w RGB. Zamazywanie składa pozostałe litery przeciętego wiersza na nowo, bez oryginalnych odstępów między literami, a obraz z zamazanymi pikselami zapisuje w RGB.
- Jeśli treści strony nie da się jednoznacznie dopasować do obiektów (uszkodzony strumień treści, tekst jako ścieżka obcinająca), strona ze zmianami zapisuje się po staremu — przez PDFium. Diagnostyka: zmienna środowiskowa `KOREKTOR_SPLICE_LOG=1` wypisuje, które strony złożono z oryginału i dlaczego inne nie.
- Plik otwarty hasłem zapisuje się bez hasła.
- Odnośniki: edytowane są odnośniki www/e-mail i do stron dokumentu. Inne rodzaje (uruchomienie programu, otwarcie innego pliku, skrypty) zostają w pliku bez zmian, ale nie da się ich zmienić w programie; z bezpieczeństwa program otwiera tylko adresy http(s) i mailto. Odnośnik do miejsca opisanego nazwą (named destination) zapisuje się jako odnośnik do strony. Zmiana formatowania zaznaczonego (nieedytowanego) akapitu nie przesuwa jego odnośników — przy pisaniu w akapicie przesuwają się same.
- Pola formularzy są zachowywane jako wygląd, ale po zapisie tracą interaktywność.
- Karty: każda trzyma w pamięci cały dokument i jego historię cofania, więc wiele dużych plików naraz zajmuje odpowiednio więcej pamięci. Otwarte karty nie są przywracane po ponownym uruchomieniu programu (lista ostatnich plików — tak).

## Licencje

Kod programu możesz udostępnić na dowolnej licencji. Zależności: PDFium (BSD-3), Dear ImGui (MIT), GLFW (zlib), nativefiledialog-extended (zlib), stb (domena publiczna / MIT), DejaVu (licencja Bitstream Vera, wolna). Pliki licencji PDFium są w pobranym archiwum, w katalogu `licenses/`.
