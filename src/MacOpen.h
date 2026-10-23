#pragma once
// macOS: pliki otwierane z Findera („Otwórz za pomocą”, dwuklik, upuszczenie na ikonę w Docku).
// System przekazuje je zdarzeniem aplikacji, a nie w argumentach programu, a GLFW go nie obsługuje.

#include <string>
#include <vector>

// Wywołać przed glfwInit(): pliki podane przy samym uruchomieniu przychodzą już podczas inicjalizacji.
void macInstallOpenHandler();

// Pliki, które przyszły od ostatniego wywołania (ścieżki UTF-8).
std::vector<std::string> macTakeOpenedFiles();
