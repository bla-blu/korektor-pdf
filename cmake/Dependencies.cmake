# Zależności pobierane automatycznie przy konfiguracji.
# Każdą można podmienić na lokalną kopię (tryb offline), np.:
#   -DPDFIUM_DIR=/ścieżka/do/pdfium   (katalog z PDFiumConfig.cmake)
#   -DFETCHCONTENT_SOURCE_DIR_IMGUI=/ścieżka/do/imgui   (analogicznie GLFW, NFD, STB)
#   -DKOREKTOR_FONT_DIR=/ścieżka/do/dejavu/ttf  (katalog z plikami DejaVu*.ttf)

set(KOREKTOR_PDFIUM_RELEASE "chromium/8044" CACHE STRING "Wydanie z github.com/bblanchon/pdfium-binaries")

# ---------- PDFium (gotowe binaria: Windows, macOS, Linux; x64 i arm64) ----------
if(NOT PDFIUM_DIR)
  if(WIN32)
    set(_os win)
  elseif(APPLE)
    set(_os mac)
  else()
    set(_os linux)
  endif()

  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _cpu)
  if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    set(_arch univ)
  elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
      set(_arch arm64)
    else()
      set(_arch x64)
    endif()
  elseif(_cpu MATCHES "^(aarch64|arm64)$")
    set(_arch arm64)
  elseif(_cpu MATCHES "^(arm|armv7.*)$")
    set(_arch arm)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(_arch x86)
  else()
    set(_arch x64)
  endif()

  set(_name "pdfium-${_os}-${_arch}")
  set(PDFIUM_DIR "${CMAKE_BINARY_DIR}/_deps/${_name}")
  if(NOT EXISTS "${PDFIUM_DIR}/PDFiumConfig.cmake")
    string(REPLACE "/" "%2F" _tag "${KOREKTOR_PDFIUM_RELEASE}")
    set(_url "https://github.com/bblanchon/pdfium-binaries/releases/download/${_tag}/${_name}.tgz")
    message(STATUS "Pobieranie PDFium: ${_url}")
    file(DOWNLOAD "${_url}" "${CMAKE_BINARY_DIR}/_deps/${_name}.tgz" STATUS _st SHOW_PROGRESS)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      message(FATAL_ERROR "Nie udało się pobrać PDFium (${_st}). Pobierz ręcznie ${_url} i podaj -DPDFIUM_DIR=...")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/_deps/${_name}.tgz" DESTINATION "${PDFIUM_DIR}")
  endif()
endif()
set(PDFium_DIR "${PDFIUM_DIR}")
find_package(PDFium REQUIRED)

# macOS: gotowa biblioteka ma nazwę instalacyjną „./libpdfium.dylib”, czyli szukaną w bieżącym katalogu
# (program uruchomiony z Findera by jej nie znalazł). Zmieniamy ją na @rpath/libpdfium.dylib — wtedy
# program szuka jej obok siebie albo w Contents/Frameworks pakietu — i podpisujemy bibliotekę na nowo
# (zmiana unieważnia podpis, a Mac z procesorem M nie wczyta biblioteki bez ważnego podpisu).
if(APPLE AND PDFium_LIBRARY)
  execute_process(COMMAND otool -D "${PDFium_LIBRARY}" OUTPUT_VARIABLE _pdfium_id OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _pdfium_id MATCHES "@rpath/libpdfium.dylib")
    message(STATUS "PDFium: nazwa instalacyjna -> @rpath/libpdfium.dylib")
    execute_process(COMMAND install_name_tool -id "@rpath/libpdfium.dylib" "${PDFium_LIBRARY}" RESULT_VARIABLE _r1)
    execute_process(COMMAND codesign --force --sign - "${PDFium_LIBRARY}" RESULT_VARIABLE _r2)
    if(NOT _r1 EQUAL 0 OR NOT _r2 EQUAL 0)
      message(FATAL_ERROR "Nie udało się poprawić biblioteki PDFium (${PDFium_LIBRARY}). Czy są narzędzia Xcode (xcode-select --install)?")
    endif()
  endif()
endif()

# ---------- czcionki DejaVu (interfejs, nowy tekst i czcionki zastępcze przy edycji) ----------
# Rodziny Sans, Serif i Mono w odmianach zwykłej, pogrubionej i pochyłej — z pełnym zestawem
# polskich znaków. Osadzane w programie podczas kompilacji.
if(NOT KOREKTOR_FONT_DIR)
  FetchContent_Declare(dejavu
    URL https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2
    URL_HASH SHA256=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(dejavu)
  set(KOREKTOR_FONT_DIR "${dejavu_SOURCE_DIR}/ttf")
endif()
set(KOREKTOR_FONT_FILES
  DejaVuSans DejaVuSans-Bold DejaVuSans-Oblique DejaVuSans-BoldOblique
  DejaVuSerif DejaVuSerif-Bold DejaVuSerif-Italic DejaVuSerif-BoldItalic
  DejaVuSansMono DejaVuSansMono-Bold DejaVuSansMono-Oblique DejaVuSansMono-BoldOblique)
foreach(_f IN LISTS KOREKTOR_FONT_FILES)
  if(NOT EXISTS "${KOREKTOR_FONT_DIR}/${_f}.ttf")
    message(FATAL_ERROR "Brak czcionki ${KOREKTOR_FONT_DIR}/${_f}.ttf")
  endif()
endforeach()

# ---------- stb_image (dekodowanie PNG/JPG/BMP/GIF) ----------
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(stb)   # brak CMakeLists — tylko pobranie źródeł

if(KOREKTOR_BUILD_GUI)
  # ---------- Dear ImGui ----------
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(imgui)

  # ---------- GLFW ----------
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw)

  # ---------- natywne okna wyboru plików ----------
  set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG v1.2.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nfd)
endif()
