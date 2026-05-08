#include "IsoExtraction.h"
#include <iostream>
#include <stdio.h>
#include <chrono>
#include <thread>
#include <vector>
#include <direct.h>
#include <filesystem>
#include <windows.h>
#include <commdlg.h>
#include <sstream>
#include <cstdlib>

std::string GetGamesFolder() {
    // First, check if a custom games path is set in the registry
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\GoopieLauncher", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buffer[MAX_PATH];
        DWORD bufferSize = sizeof(buffer);
        DWORD type = 0;
        if (RegQueryValueExA(hKey, "GamesPath", NULL, &type, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS && type == REG_SZ) {
            RegCloseKey(hKey);
            std::string customPath(buffer);
            if (!customPath.empty()) {
                return customPath;
            }
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
    // Fallback to current directory if LOCALAPPDATA is not available
    return std::filesystem::current_path().string() + "\\Games";
}

namespace IsoExtraction {

// Console logging state
static bool s_consoleEnabled = false;
static bool s_consoleInitialized = false;
static HWND s_mainWindowHandle = nullptr;

void EnableConsoleLogging() {
    s_consoleEnabled = true;
}

void SetMainWindowHandle(void* hwnd) {
    s_mainWindowHandle = static_cast<HWND>(hwnd);
}

static void EnsureConsole() {
    if (!s_consoleEnabled || s_consoleInitialized) return;
    s_consoleInitialized = true;

    // Try to attach to parent console first, allocate new one if that fails
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }

    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();
}

std::string OpenIsoFileDialog() {
    std::string originalCwd = std::filesystem::current_path().string();

    OPENFILENAME ofn;
    wchar_t szFile[260] = {0};

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s_mainWindowHandle;
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
    } catch (const std::exception& e) {
      // eh
    }

    return result;
}

std::string GetSafePathName(const std::string& longPath) {
    EnsureConsole(); // Make sure we can log
    std::cout << "[IsoExtraction] GetSafePathName input: " << longPath << std::endl;

    // Convert to wide string
    int wideLength = MultiByteToWideChar(CP_UTF8, 0, longPath.c_str(), -1, nullptr, 0);
    if (wideLength == 0) {
        DWORD error = GetLastError();
        std::cout << "[IsoExtraction] GetSafePathName: MultiByteToWideChar failed, error: " << error << std::endl;
		MessageBoxA(nullptr, ("Failed to convert path to wide string (error code: " + std::to_string(error) + "). The path may contain characters that cannot be processed. Using original path.").c_str(), "Goopie Launcher", MB_ICONWARNING | MB_OK);
        return longPath;
    }
    std::wstring widePath(wideLength, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, longPath.c_str(), -1, &widePath[0], wideLength);
    widePath.resize(wideLength - 1); // Remove null terminator from string length

    // Get short path name
    DWORD shortLength = ::GetShortPathNameW(widePath.c_str(), nullptr, 0);
    if (shortLength == 0) {
        DWORD error = GetLastError();
        std::cout << "[IsoExtraction] GetSafePathName: GetShortPathNameW failed, error: " << error << " - using original path" << std::endl;
		MessageBoxA(nullptr, ("Failed to get short path name (error code: " + std::to_string(error) + "). The path may be too long or contain invalid characters. Using original path.").c_str(), "Goopie Launcher", MB_ICONWARNING | MB_OK);
        return longPath; // Fall back to original path if short path fails
    }

    std::wstring shortWidePath(shortLength, L'\0');
    ::GetShortPathNameW(widePath.c_str(), &shortWidePath[0], shortLength);
    shortWidePath.resize(shortLength - 1); // Remove null terminator from string length

    // Convert back to UTF-8
    int utf8Length = WideCharToMultiByte(CP_UTF8, 0, shortWidePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length == 0) {
        DWORD error = GetLastError();
        std::cout << "[IsoExtraction] GetSafePathName: WideCharToMultiByte failed, error: " << error << std::endl;
		MessageBoxA(nullptr, ("Failed to convert short path back to UTF-8 (error code: " + std::to_string(error) + "). Using original path.").c_str(), "Goopie Launcher", MB_ICONWARNING | MB_OK);
        return longPath;
    }
    std::string shortPath(utf8Length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, shortWidePath.c_str(), -1, &shortPath[0], utf8Length, nullptr, nullptr);
    shortPath.resize(utf8Length - 1); // Remove null terminator from string length

    std::cout << "[IsoExtraction] GetSafePathName output: " << shortPath << std::endl;
    return shortPath;
}

void ExtractIsoAsync(const std::string& isoPath, std::shared_ptr<IsoExtractionProgress> progress,
                     const std::string& gameName) {
    std::thread([isoPath, progress, gameName]() {
        EnsureConsole(); // Initialize console for this subprocess
        std::cout << "[IsoExtraction] === Starting ISO Extraction ===" << std::endl;
        std::cout << "[IsoExtraction] Input ISO path: " << isoPath << std::endl;
        std::cout << "[IsoExtraction] Game name: " << gameName << std::endl;

        progress->isExtracting = true;
        progress->isComplete = false;
        progress->hasError = false;

        // Validate ISO path
        if (isoPath.empty()) {
            std::cout << "[IsoExtraction] ERROR: ISO path is empty!" << std::endl;
            progress->hasError = true;
            progress->errorMessage = "No ISO file selected";
            progress->isExtracting = false;
            return;
        }

        if (!std::filesystem::exists(isoPath)) {
            std::cout << "[IsoExtraction] ERROR: ISO file does not exist: " << isoPath << std::endl;
            progress->hasError = true;
            progress->errorMessage = "ISO file does not exist: " + isoPath;
			MessageBoxA(nullptr, ("The selected ISO file does not exist:\n" + isoPath).c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
            progress->isExtracting = false;
            return;
        }

        std::cout << "[IsoExtraction] ISO file exists, size: " << std::filesystem::file_size(isoPath) << " bytes" << std::endl;

        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir = std::filesystem::path(exePath).parent_path().string();
        std::cout << "[IsoExtraction] Executable directory: " << exeDir << std::endl;

        std::vector<std::string> possiblePaths = {
            "Assets\\xdvdfs.exe",
            "Launcher\\Assets\\xdvdfs.exe",
            ".\\Assets\\xdvdfs.exe",
            ".\\Launcher\\Assets\\xdvdfs.exe",
            exeDir + "\\Assets\\xdvdfs.exe",
            exeDir + "\\..\\Launcher\\Assets\\xdvdfs.exe",
            exeDir + "\\..\\..\\..\\Assets\\xdvdfs.exe"
        };

        std::string exisoPath;
        bool found = false;

        std::cout << "[IsoExtraction] Searching for xdvdfs.exe..." << std::endl;
        for (const std::string& path : possiblePaths) {
            std::cout << "[IsoExtraction]   Checking: " << path << " -> " << (std::filesystem::exists(path) ? "FOUND" : "not found") << std::endl;
            if (std::filesystem::exists(path)) {
                exisoPath = path;
                found = true;
                break;
            }
        }

        if (!found) {
            std::cout << "[IsoExtraction] ERROR: xdvdfs.exe not found in any expected location!" << std::endl;
            progress->hasError = true;
            progress->errorMessage = "xdvdfs.exe not found in any expected location";
			MessageBoxA(nullptr, "Could not find xdvdfs.exe. Please make sure it is located in the Assets folder of the launcher.", "Goopie Launcher", MB_ICONERROR | MB_OK);
            progress->isExtracting = false;
            return;
        }

        std::cout << "[IsoExtraction] Using xdvdfs.exe at: " << exisoPath << std::endl;

        std::filesystem::path exisoDir = std::filesystem::path(exisoPath).parent_path();

        // Use the global games folder path
        std::filesystem::path gameAssetsPath;
        if (!gameName.empty()) {
            gameAssetsPath = std::filesystem::path(GetGamesFolder()) / gameName / "assets";
        }
        std::string absoluteGameAssetsPath = gameAssetsPath.string();
        std::cout << "[IsoExtraction] Destination path: " << absoluteGameAssetsPath << std::endl;

        std::filesystem::path gameAssetsDir = absoluteGameAssetsPath;
        if (std::filesystem::exists(gameAssetsDir)) {
            std::cout << "[IsoExtraction] Removing existing assets directory..." << std::endl;
            try {
                std::filesystem::remove_all(gameAssetsDir);
            } catch (const std::exception& e) {
                std::cout << "[IsoExtraction] WARNING: Failed to remove existing directory: " << e.what() << std::endl;
				MessageBoxA(nullptr, ("Failed to remove existing assets directory:\n" + absoluteGameAssetsPath + "\nError: " + e.what() + "\nPlease try manually deleting this directory and running the extraction again.").c_str(), "Goopie Launcher", MB_ICONWARNING | MB_OK);
            }
        }

        std::cout << "[IsoExtraction] Creating assets directory..." << std::endl;
        try {
            std::filesystem::create_directories(gameAssetsDir);
        } catch (const std::exception& e) {
            std::cout << "[IsoExtraction] ERROR: Failed to create directory: " << e.what() << std::endl;
			MessageBoxA(nullptr, ("Failed to create destination directory:\n" + absoluteGameAssetsPath + "\nError: " + e.what() + "\nPlease try manually creating this directory and running the extraction again.").c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
            progress->hasError = true;
            progress->errorMessage = "Failed to create destination directory: " + std::string(e.what());
            progress->isExtracting = false;
            return;
        }

        std::string shortIsoPath = GetSafePathName(isoPath);
        std::string shortDestPath = GetSafePathName(absoluteGameAssetsPath);

        std::cout << "[IsoExtraction] Original ISO path: " << isoPath << std::endl;
        std::cout << "[IsoExtraction] Short ISO path: " << shortIsoPath << std::endl;
        std::cout << "[IsoExtraction] Original dest path: " << absoluteGameAssetsPath << std::endl;
        std::cout << "[IsoExtraction] Short dest path: " << shortDestPath << std::endl;

        std::string arguments = "unpack \"" + shortIsoPath + "\" \"" + shortDestPath + "\"";
        std::string commandLine = "\"" + exisoPath + "\" " + arguments;

        std::cout << "[IsoExtraction] Command line: " << commandLine << std::endl;

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        // Show the command line window: do not set SW_HIDE, and do not set CREATE_NO_WINDOW
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        ZeroMemory(&pi, sizeof(pi));

        std::cout << "[IsoExtraction] Starting xdvdfs.exe process..." << std::endl;

        BOOL success = CreateProcessA(
            NULL,
            const_cast<char*>(commandLine.c_str()),
            NULL,
            NULL,
            FALSE,
            0, // No CREATE_NO_WINDOW, so the window will be shown
            NULL,
            NULL,
            &si,
            &pi
        );

        if (!success) {
            DWORD error = GetLastError();
            std::cout << "[IsoExtraction] ERROR: Failed to start xdvdfs.exe, Win32 error code: " << error << std::endl;
            progress->hasError = true;
            progress->errorMessage = "Failed to start xdvdfs.exe (error code: " + std::to_string(error) + ")";
			MessageBoxA(nullptr, ("Failed to start extraction process:\n" + commandLine + "\nError code: " + std::to_string(error) + "\nPlease make sure xdvdfs.exe is present and try again.").c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
            progress->isExtracting = false;
            return;
        }

        std::cout << "[IsoExtraction] Process started, PID: " << pi.dwProcessId << std::endl;
        std::cout << "[IsoExtraction] Waiting for extraction to complete..." << std::endl;

        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        std::cout << "[IsoExtraction] Process finished with exit code: " << exitCode << std::endl;

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        progress->isExtracting = false;
        if (exitCode == 0) {
            progress->isComplete = true;
            std::cout << "[IsoExtraction] === Extraction completed successfully ===" << std::endl;

            // Verify extraction by checking for files
            try {
                int fileCount = 0;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(absoluteGameAssetsPath)) {
                    if (entry.is_regular_file()) {
                        fileCount++;
                    }
                }
                std::cout << "[IsoExtraction] Extracted " << fileCount << " files to " << absoluteGameAssetsPath << std::endl;
            } catch (const std::exception& e) {
                std::cout << "[IsoExtraction] WARNING: Could not count extracted files: " << e.what() << std::endl;
            }
        } else {
            progress->hasError = true;
            progress->errorMessage = "xdvdfs.exe failed with exit code: " + std::to_string(exitCode);
			MessageBoxA(nullptr, ("Extraction failed with exit code: " + std::to_string(exitCode) + "\nPlease check the console output for more details.").c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
            std::cout << "[IsoExtraction] === Extraction FAILED with exit code: " << exitCode << " ===" << std::endl;
        }
    }).detach();
}

}
