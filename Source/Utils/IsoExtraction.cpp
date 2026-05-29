#include "IsoExtraction.h"
#include <iostream>
#include <stdio.h>
#include <chrono>
#include <thread>
#include <vector>
#include <filesystem>
#include <sstream>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#include <commdlg.h>
#else
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
extern char** environ;
#endif

// ── Platform-agnostic config helpers (Linux only) ────────────────────────────
#ifndef _WIN32
static std::filesystem::path ConfigPath() {
    const char* home = getenv("HOME");
    std::filesystem::path dir = home ? std::filesystem::path(home) / ".config" / "GoopieLauncher"
                                     : std::filesystem::current_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "config.ini";
}

static std::string ConfigRead(const std::string& key, const std::string& def) {
    std::ifstream f(ConfigPath());
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return def;
}

static void ConfigWrite(const std::string& key, const std::string& value) {
    auto path = ConfigPath();
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string line;
        bool found = false;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos && line.substr(0, eq) == key) {
                lines.push_back(key + "=" + value);
                found = true;
            } else {
                lines.push_back(line);
            }
        }
        if (!found) lines.push_back(key + "=" + value);
    }
    std::ofstream f(path);
    for (const auto& l : lines) f << l << "\n";
}
#endif

std::string GetGamesFolder() {
#ifdef _WIN32
    // Check if a custom games path is set in the registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\GoopieLauncher", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buffer[MAX_PATH];
        DWORD bufferSize = sizeof(buffer);
        DWORD type = 0;
        if (RegQueryValueExA(hKey, "GamesPath", NULL, &type, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS && type == REG_SZ) {
            RegCloseKey(hKey);
            std::string customPath(buffer);
            if (!customPath.empty()) return customPath;
        }
        RegCloseKey(hKey);
    }

    // Fallback to default path in LOCALAPPDATA
    char* localAppData = nullptr;
    size_t len = 0;
    if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData != nullptr) {
        std::string result = std::string(localAppData) + "\\Goopie\\Games";
        free(localAppData);
        return result;
    }
    return std::filesystem::current_path().string() + "\\Games";
#else
    std::string custom = ConfigRead("GamesPath", "");
    if (!custom.empty()) return custom;

    const char* home = getenv("HOME");
    return home ? std::string(home) + "/.local/share/Goopie/Games"
                : std::filesystem::current_path().string() + "/Games";
#endif
}

namespace IsoExtraction {

static bool s_consoleEnabled = false;
static bool s_consoleInitialized = false;
static void* s_mainWindowHandle = nullptr;

void EnableConsoleLogging() {
    s_consoleEnabled = true;
}

void SetMainWindowHandle(void* hwnd) {
    s_mainWindowHandle = hwnd;
}

static void EnsureConsole() {
    if (!s_consoleEnabled || s_consoleInitialized) return;
    s_consoleInitialized = true;
#ifdef _WIN32
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);
    std::cout.clear(); std::cerr.clear(); std::cin.clear();
#endif
    // On Linux stdout/stderr are already connected to the terminal.
}

std::string OpenIsoFileDialog() {
#ifdef _WIN32
    std::string originalCwd = std::filesystem::current_path().string();

    OPENFILENAME ofn;
    wchar_t szFile[260] = {0};

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(s_mainWindowHandle);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = L"ISO Files\0*.iso\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    std::string result;
    if (GetOpenFileNameW(&ofn) == TRUE) {
        int len = WideCharToMultiByte(CP_UTF8, 0, szFile, -1, NULL, 0, NULL, NULL);
        result.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, szFile, -1, &result[0], len, NULL, NULL);
    }

    try {
        std::filesystem::current_path(originalCwd);
    } catch (const std::exception&) {}

    return result;
#else
    FILE* fp = popen("zenity --file-selection --title='Select ISO file'"
                     " --file-filter='ISO files (*.iso) | *.iso' 2>/dev/null", "r");
    if (!fp) {
        std::cerr << "Enter ISO file path: ";
        std::string path;
        std::getline(std::cin, path);
        return path;
    }
    char buf[4096] = {};
    std::string result;
    if (fgets(buf, sizeof(buf), fp)) {
        result = std::string(buf);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
    }
    pclose(fp);
    return result;
#endif
}

std::string EscapeForCmd(const std::string& path) {
    std::string escaped = "\"";
    for (char c : path) {
        if (c == '"') escaped += "\\\"";
        else escaped += c;
    }
    escaped += "\"";
    return escaped;
}

std::string GetSafePathName(const std::string& longPath) {
    EnsureConsole();
    std::cout << "[IsoExtraction] GetSafePathName input: " << longPath << std::endl;
#ifdef _WIN32
    // Convert to wide string
    int wideLength = MultiByteToWideChar(CP_UTF8, 0, longPath.c_str(), -1, nullptr, 0);
    if (wideLength == 0) {
        DWORD error = GetLastError();
        std::cout << "[IsoExtraction] GetSafePathName: MultiByteToWideChar failed, error: " << error << std::endl;
        MessageBoxA(nullptr, ("Failed to convert path to wide string (error code: " + std::to_string(error) + "). Using original path.").c_str(), "Goopie Launcher", MB_ICONWARNING | MB_OK);
        return longPath;
    }
    std::wstring widePath(wideLength, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, longPath.c_str(), -1, &widePath[0], wideLength);
    widePath.resize(wideLength - 1);

    DWORD shortLength = ::GetShortPathNameW(widePath.c_str(), nullptr, 0);
    if (shortLength == 0) {
        DWORD error = GetLastError();
        std::cout << "[IsoExtraction] GetSafePathName: GetShortPathNameW failed, error: " << error << " - using original path" << std::endl;
        MessageBoxA(nullptr, ("Failed to get short path name (error code: " + std::to_string(error) + "). Using original path.").c_str(), "Goopie Launcher", MB_ICONWARNING | MB_OK);
        return longPath;
    }

    std::wstring shortWidePath(shortLength, L'\0');
    ::GetShortPathNameW(widePath.c_str(), &shortWidePath[0], shortLength);
    shortWidePath.resize(shortLength - 1);

    int utf8Length = WideCharToMultiByte(CP_UTF8, 0, shortWidePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length == 0) {
        DWORD error = GetLastError();
        std::cout << "[IsoExtraction] GetSafePathName: WideCharToMultiByte failed, error: " << error << std::endl;
        MessageBoxA(nullptr, ("Failed to convert short path back to UTF-8 (error code: " + std::to_string(error) + "). Using original path.").c_str(), "Goopie Launcher", MB_ICONWARNING | MB_OK);
        return longPath;
    }
    std::string shortPath(utf8Length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, shortWidePath.c_str(), -1, &shortPath[0], utf8Length, nullptr, nullptr);
    shortPath.resize(utf8Length - 1);

    std::cout << "[IsoExtraction] GetSafePathName output: " << shortPath << std::endl;
    return shortPath;
#else
    // On Linux, long paths with spaces are passed safely via argv arrays — no conversion needed.
    return longPath;
#endif
}

void ExtractIsoAsync(const std::string& isoPath, std::shared_ptr<IsoExtractionProgress> progress,
                     const std::string& gameName) {
    std::thread([isoPath, progress, gameName]() {
        EnsureConsole();
        std::cout << "[IsoExtraction] === Starting ISO Extraction ===" << std::endl;
        std::cout << "[IsoExtraction] Input ISO path: " << isoPath << std::endl;
        std::cout << "[IsoExtraction] Game name: " << gameName << std::endl;

        progress->isExtracting = true;
        progress->isComplete = false;
        progress->hasError = false;

        if (isoPath.empty()) {
            std::cout << "[IsoExtraction] ERROR: ISO path is empty!" << std::endl;
            progress->hasError = true;
            progress->errorMessage = "No ISO file selected";
            progress->isExtracting = false;
            return;
        }

        if (!std::filesystem::exists(isoPath)) {
            std::string msg = "ISO file does not exist: " + isoPath;
            std::cout << "[IsoExtraction] ERROR: " << msg << std::endl;
            progress->hasError = true;
            progress->errorMessage = msg;
#ifdef _WIN32
            MessageBoxA(nullptr, ("The selected ISO file does not exist:\n" + isoPath).c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
#else
            std::cerr << "[Goopie Launcher] " << msg << std::endl;
#endif
            progress->isExtracting = false;
            return;
        }

        std::cout << "[IsoExtraction] ISO file exists, size: " << std::filesystem::file_size(isoPath) << " bytes" << std::endl;

        // Locate our own executable directory
#ifdef _WIN32
        char exePathBuf[MAX_PATH];
        GetModuleFileNameA(NULL, exePathBuf, MAX_PATH);
        std::string exeDir = std::filesystem::path(exePathBuf).parent_path().string();
#else
        char exePathBuf[4096] = {};
        ssize_t len = readlink("/proc/self/exe", exePathBuf, sizeof(exePathBuf) - 1);
        std::string exeDir = (len > 0)
            ? std::filesystem::path(exePathBuf).parent_path().string()
            : std::filesystem::current_path().string();
#endif
        std::cout << "[IsoExtraction] Executable directory: " << exeDir << std::endl;

#ifdef _WIN32
        std::vector<std::string> possiblePaths = {
            "Assets\\xdvdfs.exe",
            "Launcher\\Assets\\xdvdfs.exe",
            ".\\Assets\\xdvdfs.exe",
            ".\\Launcher\\Assets\\xdvdfs.exe",
            exeDir + "\\Assets\\xdvdfs.exe",
            exeDir + "\\..\\Launcher\\Assets\\xdvdfs.exe",
            exeDir + "\\..\\..\\..\\Assets\\xdvdfs.exe"
        };
        const std::string toolName = "xdvdfs.exe";
#else
        std::vector<std::string> possiblePaths = {
            "Assets/xdvdfs",
            "Launcher/Assets/xdvdfs",
            "./Assets/xdvdfs",
            exeDir + "/Assets/xdvdfs",
            exeDir + "/../Assets/xdvdfs",
            exeDir + "/../../../Assets/xdvdfs"
        };
        const std::string toolName = "xdvdfs";
#endif

        std::string exisoPath;
        bool found = false;

        std::cout << "[IsoExtraction] Searching for " << toolName << "..." << std::endl;
        for (const std::string& path : possiblePaths) {
            std::cout << "[IsoExtraction]   Checking: " << path << " -> " << (std::filesystem::exists(path) ? "FOUND" : "not found") << std::endl;
            if (std::filesystem::exists(path)) {
                exisoPath = path;
                found = true;
                break;
            }
        }

        if (!found) {
            std::string msg = toolName + " not found in any expected location";
            std::cout << "[IsoExtraction] ERROR: " << msg << std::endl;
            progress->hasError = true;
            progress->errorMessage = msg;
#ifdef _WIN32
            MessageBoxA(nullptr, ("Could not find " + toolName + ". Please make sure it is located in the Assets folder.").c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
#else
            std::cerr << "[Goopie Launcher] " << msg << std::endl;
#endif
            progress->isExtracting = false;
            return;
        }

        std::cout << "[IsoExtraction] Using " << toolName << " at: " << exisoPath << std::endl;

        // Determine destination path
        std::filesystem::path gameAssetsPath;
        if (!gameName.empty()) {
            gameAssetsPath = std::filesystem::path(GetGamesFolder()) / gameName / "assets";
        }
        std::string absoluteGameAssetsPath = gameAssetsPath.string();
        std::cout << "[IsoExtraction] Destination path: " << absoluteGameAssetsPath << std::endl;

        if (std::filesystem::exists(gameAssetsPath)) {
            std::cout << "[IsoExtraction] Removing existing assets directory..." << std::endl;
            try {
                std::filesystem::remove_all(gameAssetsPath);
            } catch (const std::exception& e) {
                std::string msg = "Failed to remove existing assets directory:\n" + absoluteGameAssetsPath + "\nError: " + e.what();
                std::cout << "[IsoExtraction] WARNING: " << msg << std::endl;
#ifdef _WIN32
                MessageBoxA(nullptr, (msg + "\nPlease try manually deleting this directory.").c_str(), "Goopie Launcher", MB_ICONWARNING | MB_OK);
#else
                std::cerr << "[Goopie Launcher] " << msg << std::endl;
#endif
            }
        }

        std::cout << "[IsoExtraction] Creating assets directory..." << std::endl;
        try {
            std::filesystem::create_directories(gameAssetsPath);
        } catch (const std::exception& e) {
            std::string msg = "Failed to create destination directory: " + std::string(e.what());
            std::cout << "[IsoExtraction] ERROR: " << msg << std::endl;
#ifdef _WIN32
            MessageBoxA(nullptr, ("Failed to create destination directory:\n" + absoluteGameAssetsPath + "\nError: " + e.what()).c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
#else
            std::cerr << "[Goopie Launcher] " << msg << std::endl;
#endif
            progress->hasError = true;
            progress->errorMessage = msg;
            progress->isExtracting = false;
            return;
        }

        std::string shortIsoPath = GetSafePathName(isoPath);
        std::string shortDestPath = GetSafePathName(absoluteGameAssetsPath);

        std::cout << "[IsoExtraction] Short ISO path: " << shortIsoPath << std::endl;
        std::cout << "[IsoExtraction] Short dest path: " << shortDestPath << std::endl;

        int exitCode = 1;

#ifdef _WIN32
        std::string arguments = "unpack \"" + shortIsoPath + "\" \"" + shortDestPath + "\"";
        std::string commandLine = "\"" + exisoPath + "\" " + arguments;
        std::cout << "[IsoExtraction] Command line: " << commandLine << std::endl;

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        ZeroMemory(&pi, sizeof(pi));

        std::cout << "[IsoExtraction] Starting " << toolName << " process..." << std::endl;

        BOOL success = CreateProcessA(NULL, const_cast<char*>(commandLine.c_str()),
            NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

        if (!success) {
            DWORD error = GetLastError();
            std::string msg = "Failed to start " + toolName + " (error code: " + std::to_string(error) + ")";
            std::cout << "[IsoExtraction] ERROR: " << msg << std::endl;
            progress->hasError = true;
            progress->errorMessage = msg;
            MessageBoxA(nullptr, (msg + "\nPlease make sure " + toolName + " is present and try again.").c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
            progress->isExtracting = false;
            return;
        }

        std::cout << "[IsoExtraction] Process started, PID: " << pi.dwProcessId << std::endl;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD dwExitCode;
        GetExitCodeProcess(pi.hProcess, &dwExitCode);
        exitCode = static_cast<int>(dwExitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
#else
        // Use posix_spawn to launch xdvdfs with arguments passed as argv (safe for paths with spaces)
        char* argv_tool[] = {
            const_cast<char*>(exisoPath.c_str()),
            (char*)"unpack",
            const_cast<char*>(shortIsoPath.c_str()),
            const_cast<char*>(shortDestPath.c_str()),
            nullptr
        };
        pid_t pid;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        int spawnErr = posix_spawn(&pid, exisoPath.c_str(), &fa, nullptr, argv_tool, environ);
        posix_spawn_file_actions_destroy(&fa);

        if (spawnErr != 0) {
            std::string msg = "Failed to start " + toolName + ": " + strerror(spawnErr);
            std::cout << "[IsoExtraction] ERROR: " << msg << std::endl;
            progress->hasError = true;
            progress->errorMessage = msg;
            std::cerr << "[Goopie Launcher] " << msg << std::endl;
            progress->isExtracting = false;
            return;
        }

        std::cout << "[IsoExtraction] Process started, PID: " << pid << std::endl;
        int status;
        waitpid(pid, &status, 0);
        exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif

        std::cout << "[IsoExtraction] Process finished with exit code: " << exitCode << std::endl;

        progress->isExtracting = false;
        if (exitCode == 0) {
            progress->isComplete = true;
            std::cout << "[IsoExtraction] === Extraction completed successfully ===" << std::endl;
            try {
                int fileCount = 0;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(absoluteGameAssetsPath))
                    if (entry.is_regular_file()) fileCount++;
                std::cout << "[IsoExtraction] Extracted " << fileCount << " files to " << absoluteGameAssetsPath << std::endl;
            } catch (const std::exception& e) {
                std::cout << "[IsoExtraction] WARNING: Could not count extracted files: " << e.what() << std::endl;
            }
        } else {
            std::string msg = toolName + " failed with exit code: " + std::to_string(exitCode);
            progress->hasError = true;
            progress->errorMessage = msg;
#ifdef _WIN32
            MessageBoxA(nullptr, ("Extraction failed with exit code: " + std::to_string(exitCode) + "\nPlease check the console output for more details.").c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
#else
            std::cerr << "[Goopie Launcher] " << msg << std::endl;
#endif
            std::cout << "[IsoExtraction] === Extraction FAILED with exit code: " << exitCode << " ===" << std::endl;
        }
    }).detach();
}

} // namespace IsoExtraction
