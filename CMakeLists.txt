cmake_minimum_required(VERSION 3.20)
# macOS: najstarsza obsługiwana wersja systemu (gotowe PDFium wymaga macOS 13). Na innych systemach bez znaczenia.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Najstarsza obsługiwana wersja macOS")
project(KorektorPDF VERSION 0.11.1 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)   # obsługa macOS (src/MacOpen.mm); przed dołączeniem bibliotek
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(KOREKTOR_BUILD_GUI   "Buduj aplikację z interfejsem"      ON)
option(KOREKTOR_BUILD_TESTS "Buduj test rdzenia (bez interfejsu)" ON)

# Windows (Visual Studio): biblioteka uruchomieniowa C++ wbudowana w program (/MT), więc na innym
# komputerze nie trzeba instalować „Microsoft Visual C++ Redistributable”. Obejmuje też GLFW i NFD.
option(KOREKTOR_STATIC_RUNTIME "MSVC: statyczna biblioteka uruchomieniowa (program przenośny)" ON)
if(MSVC AND KOREKTOR_STATIC_RUNTIME)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)   # GLFW
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
include(cmake/Dependencies.cmake)   # PDFium, czcionka, stb (+ ImGui, GLFW, NFD gdy GUI)

if(WIN32)
  add_compile_definitions(NOMINMAX UNICODE _UNICODE)
endif()
if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------- osadzenie czcionek: małe narzędzie zamienia pliki TTF na tablice C++ ----------
# Przy kompilacji skrośnej (np. na Windows z Linuksa) narzędzie musi działać na komputerze
# budującym: podaj gotowe przez -DKOREKTOR_EMBED_TOOL=/ścieżka/korektor_embed.
set(KOREKTOR_EMBED_TOOL "" CACHE FILEPATH "Gotowe narzędzie korektor_embed (tylko kompilacja skrośna)")
if(KOREKTOR_EMBED_TOOL)
  set(_embed "${KOREKTOR_EMBED_TOOL}")
  set(_embed_dep "${KOREKTOR_EMBED_TOOL}")
else()
  add_executable(korektor_embed tools/embed_fonts.cpp)
  set(_embed korektor_embed)
  set(_embed_dep korektor_embed)
endif()
set(_font_args)
set(_font_deps)
foreach(_f IN LISTS KOREKTOR_FONT_FILES)
  list(APPEND _font_args "${_f}" "${KOREKTOR_FONT_DIR}/${_f}.ttf")
  list(APPEND _font_deps "${KOREKTOR_FONT_DIR}/${_f}.ttf")
endforeach()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")   # na pliki tworzone przez korektor_embed
set(KOREKTOR_FONT_CPP "${CMAKE_BINARY_DIR}/generated/FontData.cpp")
add_custom_command(
  OUTPUT "${KOREKTOR_FONT_CPP}"
  COMMAND ${_embed} "${KOREKTOR_FONT_CPP}" ${_font_args}
  DEPENDS ${_embed_dep} ${_font_deps}
  COMMENT "Osadzanie czcionek"
  VERBATIM)

# ---------- rdzeń: model dokumentu, renderowanie i zapis (bez GUI) ----------
add_library(korektor_core STATIC
  src/Document.cpp
  src/Document.h
  src/Redact.cpp
  src/ContentSplice.cpp
  src/ContentSplice.h
  src/PdfUtil.h
  src/Fonts.cpp
  src/Fonts.h
  src/ImageLoad.cpp
  src/ImageLoad.h
  ${KOREKTOR_FONT_CPP}
)
target_include_directories(korektor_core PUBLIC src PRIVATE ${stb_SOURCE_DIR})
# stb to kod zewnętrzny — jego ostrzeżenia wyciszamy w samym pliku ImageLoad.cpp (#pragma),
# żeby nie nadpisywać poziomu ostrzeżeń kompilatora (MSVC zgłasza wtedy D9025).
target_link_libraries(korektor_core PUBLIC pdfium)

# Biblioteka PDFium jest współdzielona — kopiujemy ją obok pliku wykonywalnego.
# W pakiecie aplikacji macOS (.app) biblioteka trafia do Contents/Frameworks.
function(korektor_runtime target)
  get_target_property(_bundle ${target} MACOSX_BUNDLE)
  if(APPLE AND _bundle)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pdfium>" "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks/"
      VERBATIM)
    set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "@executable_path/../Frameworks")
    return()
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:pdfium> $<TARGET_FILE_DIR:${target}>)
  if(APPLE)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "@executable_path" INSTALL_RPATH "@executable_path")
  elseif(UNIX)
    set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
  endif()
endfunction()

if(KOREKTOR_BUILD_TESTS)
  add_executable(korektor_selftest tests/selftest.cpp)
  target_include_directories(korektor_selftest PRIVATE ${stb_SOURCE_DIR})
  target_link_libraries(korektor_selftest PRIVATE korektor_core)
  korektor_runtime(korektor_selftest)
  enable_testing()
  add_test(NAME selftest COMMAND korektor_selftest ${CMAKE_BINARY_DIR}/selftest-out)
endif()

if(KOREKTOR_BUILD_GUI)
  find_package(OpenGL REQUIRED)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

  # ikony paska narzędzi (PNG 128×128) osadzane tym samym narzędziem co czcionki
  set(KOREKTOR_ICONS logo otworz dolacz zapisz zapisz-jako zaznacz tekst pioro zakresl zamaz obraz odnosniki cofnij ponow usun)
  set(_icon_args)
  set(_icon_deps)
  foreach(_i IN LISTS KOREKTOR_ICONS)
    list(APPEND _icon_args "${_i}" "${CMAKE_SOURCE_DIR}/assets/icons/${_i}.png")
    list(APPEND _icon_deps "${CMAKE_SOURCE_DIR}/assets/icons/${_i}.png")
  endforeach()
  set(KOREKTOR_ICON_CPP "${CMAKE_BINARY_DIR}/generated/IconData.cpp")
  add_custom_command(
    OUTPUT "${KOREKTOR_ICON_CPP}"
    COMMAND ${_embed} "${KOREKTOR_ICON_CPP}" --decl Icons.h kIconFiles kIconFileCount ${_icon_args}
    DEPENDS ${_embed_dep} ${_icon_deps}
    COMMENT "Osadzanie ikon"
    VERBATIM)

  add_executable(korektor src/main.cpp src/App.cpp src/App.h src/Settings.cpp src/Settings.h src/Icons.cpp src/Icons.h ${KOREKTOR_ICON_CPP})
  target_include_directories(korektor PRIVATE ${stb_SOURCE_DIR})
  target_link_libraries(korektor PRIVATE korektor_core imgui nfd)
  target_compile_definitions(korektor PRIVATE KOREKTOR_VERSION="${PROJECT_VERSION}")
  if(APPLE)
    # pakiet „Korektor PDF.app”: ikona, Info.plist (dokumenty PDF, Retina), obsługa plików z Findera
    set(_icns "${CMAKE_SOURCE_DIR}/assets/korektor.icns")
    target_sources(korektor PRIVATE src/MacOpen.mm src/MacOpen.h "${_icns}")
    set_source_files_properties("${_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_link_libraries(korektor PRIVATE "-framework Cocoa")
    set_target_properties(korektor PROPERTIES
      MACOSX_BUNDLE ON
      OUTPUT_NAME "Korektor PDF"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
      MACOSX_BUNDLE_GUI_IDENTIFIER "pl.korektorpdf.korektor"
      MACOSX_BUNDLE_BUNDLE_NAME "Korektor PDF"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_ICON_FILE "korektor.icns"
      MACOSX_BUNDLE_COPYRIGHT "Korektor PDF ${PROJECT_VERSION}")
  endif()
  korektor_runtime(korektor)
  if(APPLE)
    # Podpis ad hoc (bez konta Apple): wymagany, żeby program działał na Macach z procesorem M,
    # i obejmujący bibliotekę w pakiecie. Do rozpowszechniania potrzebny jest podpis Developer ID.
    add_custom_command(TARGET korektor POST_BUILD
      COMMAND codesign --force --sign - "$<TARGET_BUNDLE_CONTENT_DIR:korektor>/Frameworks/$<TARGET_FILE_NAME:pdfium>"
      COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:korektor>"
      COMMENT "Podpis ad hoc pakietu aplikacji"
      VERBATIM)
  endif()
  if(WIN32)
    target_sources(korektor PRIVATE src/korektor.rc)   # ikona pliku .exe
    set_property(SOURCE src/korektor.rc APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/assets")
    set_property(SOURCE src/korektor.rc APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/korektor.ico")
    set_target_properties(korektor PROPERTIES WIN32_EXECUTABLE ON)
    target_link_libraries(korektor PRIVATE shell32)   # ShellExecuteW: otwieranie odnośników
    if(MSVC)
      target_link_options(korektor PRIVATE /ENTRY:mainCRTStartup)
    endif()
  endif()
endif()

# cmake --install build --prefix <katalog>  ->  <katalog>/bin/korektor + biblioteka PDFium obok
# (w Visual Studio: Kompiluj → Zainstaluj Korektor PDF — gotowy folder w out/install/<konfiguracja>/bin)
if(KOREKTOR_BUILD_GUI)
  install(TARGETS korektor RUNTIME DESTINATION bin BUNDLE DESTINATION bin)
  if(APPLE)
    # biblioteka jest już w pakiecie; podpis odnawiamy na wypadek zmian przy kopiowaniu
    install(CODE "execute_process(COMMAND codesign --force --sign - \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/Korektor PDF.app\")")
  else()
    install(FILES $<TARGET_FILE:pdfium> DESTINATION bin)
  endif()
  install(FILES README.md DESTINATION bin RENAME CZYTAJ.md)
endif()
