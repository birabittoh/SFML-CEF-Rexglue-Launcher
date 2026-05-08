#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <mutex>

// Returns the base games folder path (defaults to %LOCALAPPDATA%\Goopie\Games\)
std::string GetGamesFolder();

struct IsoExtractionProgress {
    std::atomic<bool> isExtracting{false};
    std::atomic<bool> isComplete{false};
    std::atomic<bool> hasError{false};
    std::string errorMessage;
    std::string lastConsoleOutput;
    std::mutex outputMutex;

    void Reset() {
        isExtracting = false;
        isComplete = false;
        hasError = false;
        errorMessage.clear();
        lastConsoleOutput.clear();
    }
};

namespace IsoExtraction {
    // Enable console logging (call when --console is passed)
    void EnableConsoleLogging();

    // Set the main window handle for dialog parenting
    void SetMainWindowHandle(void* hwnd);

    std::string OpenIsoFileDialog();

    std::string EscapeForCmd(const std::string& path);

    void ExtractIsoAsync(const std::string& isoPath, std::shared_ptr<IsoExtractionProgress> progress,
                         const std::string& gameName = "");
}
