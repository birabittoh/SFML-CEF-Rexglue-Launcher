#if defined(__linux__) || defined(__APPLE__)
#include <sys/utsname.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <tlhelp32.h>
#include <processthreadsapi.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#elif defined(__linux__)
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
extern char** environ;
#endif

#include <iostream>
#include <vector>
#include <cmath>
#include <fstream>
#include <algorithm>
#include <cctype>
#include "Window/Window.h"

#include "cef_client.h"
#include "cef_app.h"
#include <filesystem>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>
#include <thread>
#include <mutex>

#include "Utils/IsoExtraction.h"
#include "Networking/FileDownloader.h"
#ifdef _WIN32
#include "../resource.h"
#endif

#include "../VehicleParser.h"

#ifdef _WIN32
#include <iostream>
#include <cstdio>

bool gameIsRunning = false;
HANDLE gameProcessHandle = nullptr;
bool g_consoleEnabled = false;
bool g_consoleInitialized = false;

// Forward declaration for dialog parenting
extern HWND g_mainWindowHandle;
#else
bool gameIsRunning = false;
pid_t gameProcessHandle = -1;
#endif

// Non-throwing filesystem helpers (CEF renderer runs sandboxed; seccomp may
// block statx/mkdir with EPERM which would otherwise terminate the process).
static bool fs_exists(const std::filesystem::path& p) noexcept {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}
static bool fs_is_dir(const std::filesystem::path& p) noexcept {
    std::error_code ec;
    return std::filesystem::is_directory(p, ec);
}
static void fs_mkdirs(const std::filesystem::path& p) noexcept {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
}

// ── Platform-agnostic config helpers (Linux only) ────────────────────────────
#ifndef _WIN32
static std::filesystem::path ConfigPath_() {
    const char* home = getenv("HOME");
    std::filesystem::path dir = home ? std::filesystem::path(home) / ".config" / "GoopieLauncher"
                                     : std::filesystem::current_path();
    fs_mkdirs(dir);
    return dir / "config.ini";
}
static std::string ConfigRead_(const std::string& key, const std::string& def) {
    std::ifstream f(ConfigPath_());
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return def;
}
static void ConfigWrite_(const std::string& key, const std::string& value) {
    auto path = ConfigPath_();
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
static std::string GetDocumentsPath_() {
    FILE* fp = popen("xdg-user-dir DOCUMENTS 2>/dev/null", "r");
    if (fp) {
        char buf[4096] = {};
        if (fgets(buf, sizeof(buf), fp)) {
            std::string path(buf);
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
                path.pop_back();
            pclose(fp);
            if (!path.empty()) return path;
        } else {
            pclose(fp);
        }
    }
    const char* home = getenv("HOME");
    return home ? std::string(home) + "/Documents" : ".";
}
#endif

#ifdef _WIN32

std::string OpenGamesFolderDialog() {
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	// Force the dialog to come to foreground using thread input attachment
	HWND hwndForeground = GetForegroundWindow();
	DWORD foregroundThreadId = GetWindowThreadProcessId(hwndForeground, NULL);
	DWORD currentThreadId = GetCurrentThreadId();

	// Attach to the foreground thread to gain focus rights
	if (foregroundThreadId != currentThreadId) {
		AttachThreadInput(currentThreadId, foregroundThreadId, TRUE);
	}

	// Bring main window to front if available
	if (g_mainWindowHandle) {
		SetForegroundWindow(g_mainWindowHandle);
		BringWindowToTop(g_mainWindowHandle);
	}

	wchar_t folderPath[MAX_PATH];
	BROWSEINFO bi = { 0 };
	bi.hwndOwner = g_mainWindowHandle ? g_mainWindowHandle : GetActiveWindow();
	bi.lpszTitle = L"Select Games Folder";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
	LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
	std::string result = "";
	if (pidl != nullptr) {
		if (SHGetPathFromIDList(pidl, folderPath)) {
			std::wstring ws(folderPath);
			result = std::string(ws.begin(), ws.end());
		}
		CoTaskMemFree(pidl);
	}

	// Detach from the foreground thread
	if (foregroundThreadId != currentThreadId) {
		AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
	}

	CoUninitialize();
	return result;
}

std::string GetGamesFolder_() {
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

void InitConsole()
{
	if (!g_consoleEnabled || g_consoleInitialized) {
		return;
	}
	g_consoleInitialized = true;
	AllocConsole();

	FILE* fp;
	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONOUT$", "w", stderr);
	freopen_s(&fp, "CONIN$", "r", stdin);

	std::cout.clear();
	std::cerr.clear();
	std::cin.clear();
}
#else
// Linux stubs / alternatives

std::string OpenGamesFolderDialog() {
    FILE* fp = popen("zenity --file-selection --directory --title='Select Games Folder' 2>/dev/null", "r");
    if (!fp) {
        std::cerr << "Enter games folder path: ";
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
}

std::string GetGamesFolder_() {
    std::string custom = ConfigRead_("GamesPath", "");
    if (!custom.empty()) return custom;
    const char* home = getenv("HOME");
    return home ? std::string(home) + "/.local/share/Goopie/Games"
                : std::filesystem::current_path().string() + "/Games";
}

static void InitConsole() { /* no-op: already have a terminal on Linux */ }

#endif

std::atomic<bool> is_cef_initialized(false);
std::atomic<bool> is_browser_closed(false);
CefRefPtr<CefBrowser> g_browser;
#ifdef _WIN32
WNDPROC original_wnd_proc = nullptr;
HWND cef_window_handle = nullptr;
HWND g_mainWindowHandle = nullptr;
#else
unsigned long cef_window_handle = 0;
void* g_mainWindowHandle = nullptr;
// ── Off-Screen Rendering (Linux) ──────────────────────────────────────────────
static std::mutex           g_osr_mutex;
static std::vector<uint8_t> g_osr_pixels;
static int                  g_osr_w = 1, g_osr_h = 1;
static bool                 g_osr_dirty = false;
static double               g_cursor_x = 0, g_cursor_y = 0;
#endif
//make a sharable pointer to IsoExtractionProgress
std::shared_ptr<IsoExtractionProgress> isoExtractionProgress = std::make_shared<IsoExtractionProgress>();
//FileDownloader
Networking::FileDownloader fileDownloader;
std::string downloadString = "";

//callback for progress updates
void downloadProgressCallback(long long downloaded, long long total) {
	staticprogress = static_cast<int>((downloaded * 100) / total);
	//download string needs to have the number of mb downloaded and the total mb
	std::ostringstream oss;
	oss << (downloaded / (1024 * 1024)) << " MB / " << (total / (1024 * 1024)) << " MB";
	std::cout << "Download progress: " << oss.str() << std::endl;
	downloadString = oss.str();

}

// ── Zip extraction helper ──────────────────────────────────────────────────
// Extracts a zip file to destPath. Runs synchronously. Returns true on success.
static bool ExtractZip(const std::string& zipPath, const std::string& destPath) {
#ifdef _WIN32
	// Escape single quotes for PowerShell single-quoted strings ('' is the escape)
	auto escPS = [](const std::string& s) -> std::string {
		std::string out;
		out.reserve(s.size());
		for (char c : s) {
			if (c == '\'') out += "''";
			else out += c;
		}
		return out;
	};

	// Write a temporary .ps1 script to avoid command-line quoting issues
	char tempDir[MAX_PATH];
	GetTempPathA(MAX_PATH, tempDir);
	std::string scriptPath = std::string(tempDir) +
		"goopie_extract_" + std::to_string(GetCurrentProcessId()) +
		"_" + std::to_string(GetCurrentThreadId()) + ".ps1";

	{
		std::ofstream sf(scriptPath);
		if (!sf) {
			std::cout << "ExtractZip: failed to create temp script at " << scriptPath << std::endl;
			return false;
		}
		sf << "Expand-Archive -LiteralPath '" << escPS(zipPath)
		   << "' -DestinationPath '" << escPS(destPath) << "' -Force\n";
	}

	std::string cmd = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""
		+ scriptPath + "\"";
	std::vector<char> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back('\0');

	STARTUPINFOA si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};

	BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

	if (!ok) {
		std::cout << "ExtractZip: CreateProcess failed (error " << GetLastError() << ")" << std::endl;
		std::filesystem::remove(scriptPath);
		return false;
	}

	// Wait up to 5 minutes for extraction
	WaitForSingleObject(pi.hProcess, 300000);
	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	std::filesystem::remove(scriptPath);

	if (exitCode != 0) {
		std::cout << "ExtractZip: PowerShell exited with code " << exitCode << std::endl;
		return false;
	}
	return true;
#else
	std::string cmd = "unzip -o \"" + zipPath + "\" -d \"" + destPath + "\" > /dev/null 2>&1";
	int ret = system(cmd.c_str());
	if (ret != 0)
		std::cout << "ExtractZip: unzip exited with code " << ret << std::endl;
	return ret == 0;
#endif
}

// ── Archive asset helpers ──────────────────────────────────────────────────

static bool isZipAsset(const std::string& name) {
	if (name.size() < 4) return false;
	std::string lower(name);
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
	return lower.size() >= 4 && lower.substr(lower.size() - 4) == ".zip";
}

static bool isTarGzAsset(const std::string& name) {
	std::string lower(name);
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
	return lower.size() >= 7 && lower.substr(lower.size() - 7) == ".tar.gz";
}

static bool isArchiveAsset(const std::string& name) {
	return isZipAsset(name) || isTarGzAsset(name);
}

// Dispatches to the appropriate extraction method based on archive type.
// Returns true on success. Linux extraction is not yet implemented.
static bool ExtractArchive(const std::string& archivePath, const std::string& destPath) {
	if (isZipAsset(archivePath)) {
		return ExtractZip(archivePath, destPath);
	}
	if (isTarGzAsset(archivePath)) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
		// Windows 10 1803+ ships tar.exe; Linux/macOS have it natively.
		std::string cmd = "tar xzf \"" + archivePath + "\" -C \"" + destPath + "\"";
		int rc = std::system(cmd.c_str());
		if (rc != 0) {
			std::cout << "ExtractArchive: tar exited with code " << rc << std::endl;
			return false;
		}
		return true;
#else
		std::cout << "ExtractArchive: tar.gz extraction not supported on this platform" << std::endl;
		return false;
#endif
	}
	std::cout << "ExtractArchive: unrecognized archive type: " << archivePath << std::endl;
	return false;
}

// Searches the root of gameDir for the main executable.
// On Windows prefers <gameName>.exe, then falls back to the first .exe found.
// Returns just the filename, or empty string if nothing found.
static std::string FindMainExecutable(const std::filesystem::path& gameDir, const std::string& gameName) {
	std::string preferred;
	std::string fallback;
#ifdef _WIN32
	preferred = gameName + ".exe";
	std::string preferredLower(preferred);
	std::transform(preferredLower.begin(), preferredLower.end(), preferredLower.begin(), ::tolower);

	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(gameDir, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		std::string fname = entry.path().filename().string();
		std::string fnameLower(fname);
		std::transform(fnameLower.begin(), fnameLower.end(), fnameLower.begin(), ::tolower);
		if (fnameLower == preferredLower) return fname;
		if (fallback.empty() && fnameLower.size() >= 4 &&
			fnameLower.substr(fnameLower.size() - 4) == ".exe") {
			fallback = fname;
		}
	}
#else
	// On Linux/macOS the main binary typically has no extension and is executable.
	// Prefer <gameName> exactly, then any executable file without an extension.
	preferred = gameName;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(gameDir, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		std::string fname = entry.path().filename().string();
		if (fname == preferred) return fname;
		// Executables without an extension
		if (fallback.empty() && fname.find('.') == std::string::npos) {
			std::filesystem::perms p = entry.status(ec).permissions();
			if ((p & std::filesystem::perms::owner_exec) != std::filesystem::perms::none)
				fallback = fname;
		}
	}
#endif
	return fallback;
}

// ── Packages sidecar helpers ───────────────────────────────────────────────
// .installed_packages.json format: {"assetname.zip":true, ...}

static std::mutex s_packagesSidecarMutex;

// Records that a package has been successfully installed.
static void UpdatePackageSidecar(const std::string& sidecarPath, const std::string& assetName) {
	std::lock_guard<std::mutex> lock(s_packagesSidecarMutex);

	auto escJson = [](const std::string& s) -> std::string {
		std::string out;
		for (char c : s) {
			if (c == '"') out += "\\\"";
			else if (c == '\\') out += "\\\\";
			else out += c;
		}
		return out;
	};

	std::string existing = "{}";
	{
		std::ifstream rf(sidecarPath, std::ios::binary);
		if (rf) {
			std::string tmp((std::istreambuf_iterator<char>(rf)), {});
			if (!tmp.empty()) existing = tmp;
		}
	}

	std::string key = "\"" + escJson(assetName) + "\"";
	// If already present, nothing to do
	if (existing.find(key) != std::string::npos) return;

	// Append the new key: strip trailing '}', add ',key:true}'
	if (!existing.empty() && existing.back() == '}') existing.pop_back();
	std::string newJson;
	if (existing == "{") {
		newJson = "{" + key + ":true}";
	} else {
		newJson = existing + "," + key + ":true}";
	}

	std::ofstream wf(sidecarPath, std::ios::binary | std::ios::trunc);
	if (wf) wf << newJson;
}

// Window procedure to handle resize messages (Windows only)
#ifdef _WIN32
LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (uMsg == WM_SIZE && g_browser && g_browser->GetHost()) {
		RECT rect;
		GetClientRect(hwnd, &rect);
		InitConsole();
		std::cout << "Window resized: " << rect.right - rect.left << "x" << rect.bottom - rect.top << std::endl;
		if (cef_window_handle) {
			SetWindowPos(cef_window_handle, NULL, 0, 0,
				rect.right - rect.left, rect.bottom - rect.top,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
		g_browser->GetHost()->WasResized();
	}
	return CallWindowProc(original_wnd_proc, hwnd, uMsg, wParam, lParam);
}
#endif

class RexBrowserProcessHandler : public CefBrowserProcessHandler {
	IMPLEMENT_REFCOUNTING(RexBrowserProcessHandler);

	void OnContextInitialized() override {
		is_cef_initialized = true;
	}
};

class RexApp : public CefApp, public CefRenderProcessHandler, public CefV8Handler 
{
	IMPLEMENT_REFCOUNTING(RexApp);

	VehicleSaveManager vman;

	void OnBeforeCommandLineProcessing(const CefString& process_type,
		CefRefPtr<CefCommandLine> command_line) override {
		command_line->AppendSwitch("enable-webgl");
		command_line->AppendSwitch("disable-gpu-sandbox");

#ifdef _WIN32
		// Avoids a separate GPU process on Windows where it causes issues.
		command_line->AppendSwitch("in-process-gpu");
		if (IsD3D11Supported()) {
			command_line->AppendSwitch("enable-gpu");
			command_line->AppendSwitchWithValue("use-angle", "d3d11");
			command_line->AppendSwitch("ignore-gpu-blocklist");
		} else {
			command_line->AppendSwitchWithValue("use-gl", "swiftshader");
		}
#else
		command_line->AppendSwitch("no-sandbox");
		// Mirror the GLFW platform hint in Window.cpp: if DISPLAY is set we use
		// X11 (natively or via XWayland), so CEF must also use x11. Only fall
		// through to Wayland when DISPLAY is absent (pure Wayland, no XWayland).
		if (getenv("DISPLAY")) {
			command_line->AppendSwitchWithValue("ozone-platform", "x11");
		}
		// OSR uses a PBuffer surface (not a window surface). SwiftShader is a
		// reliable software backend for the GPU process on Linux.
		command_line->AppendSwitchWithValue("use-angle", "swiftshader");
		command_line->AppendSwitch("disable-gpu-compositing");
		command_line->AppendSwitch("enable-unsafe-swiftshader");
#endif
	}

	static bool IsD3D11Supported() {
#ifdef _WIN32
		HMODULE d3d11 = LoadLibraryA("d3d11.dll");
		if (!d3d11) return false;

		auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
			GetProcAddress(d3d11, "D3D11CreateDevice"));
		if (!createDevice) {
			FreeLibrary(d3d11);
			return false;
		}

		ID3D11Device* device = nullptr;
		D3D_FEATURE_LEVEL featureLevel;
		HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, nullptr);

		if (device) device->Release();
		FreeLibrary(d3d11);
		return SUCCEEDED(hr);
#else
		return false;
#endif
	}

	CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
		return new RexBrowserProcessHandler();
	}

	CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
		return this;
	}

	void OnContextCreated(CefRefPtr<CefBrowser> browser,
		CefRefPtr<CefFrame> frame,
		CefRefPtr<CefV8Context> v8context) override {
		v8context->GetGlobal()->SetValue("testFunction",
			CefV8Value::CreateFunction("testFunction", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getVehicleCount",
			CefV8Value::CreateFunction("getVehicleCount", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getVehicle",
			CefV8Value::CreateFunction("getVehicle", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("reloadVehicles",
			CefV8Value::CreateFunction("reloadVehicles", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("isIsoInstalled",
			CefV8Value::CreateFunction("isIsoInstalled", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("isExeUpdated",
			CefV8Value::CreateFunction("isExeUpdated", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getInstalledVersion",
			CefV8Value::CreateFunction("getInstalledVersion", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("isUpdating",
			CefV8Value::CreateFunction("isUpdating", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("isExtracting",
			CefV8Value::CreateFunction("isExtracting", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getExtractProgress",
			CefV8Value::CreateFunction("getExtractProgress", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getDownloadProgress",
			CefV8Value::CreateFunction("getDownloadProgress", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getDownloadString",
			CefV8Value::CreateFunction("getDownloadString", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("Update",
			CefV8Value::CreateFunction("Update", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("NeedsUpdate",
			CefV8Value::CreateFunction("NeedsUpdate", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("Uninstall",
			CefV8Value::CreateFunction("Uninstall", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("Install",
			CefV8Value::CreateFunction("Install", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("Play",
			CefV8Value::CreateFunction("Play", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getVersion",
			CefV8Value::CreateFunction("getVersion", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getSaveSlots",
			CefV8Value::CreateFunction("getSaveSlots", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getSaveSlotCount",
			CefV8Value::CreateFunction("getSaveSlotCount", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("getActiveSave",
			CefV8Value::CreateFunction("getActiveSave", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("backupSave",
			CefV8Value::CreateFunction("backupSave", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("restoreSave",
			CefV8Value::CreateFunction("restoreSave", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("deleteSave",
			CefV8Value::CreateFunction("deleteSave", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("renameSave",
			CefV8Value::CreateFunction("renameSave", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("OpenExternalLink",
			CefV8Value::CreateFunction("OpenExternalLink", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("SetGamesPath",
			CefV8Value::CreateFunction("SetGamesPath", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("GetGamesPath",
			CefV8Value::CreateFunction("GetGamesPath", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("GetPlatform",
			CefV8Value::CreateFunction("GetPlatform", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("GetArch",
			CefV8Value::CreateFunction("GetArch", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("SetLanguage",
			CefV8Value::CreateFunction("SetLanguage", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("GetLanguage",
			CefV8Value::CreateFunction("GetLanguage", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("OpenGamesFolder",
			CefV8Value::CreateFunction("OpenGamesFolder", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("openSaveFolder",
			CefV8Value::CreateFunction("openSaveFolder", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("deleteCurrentSave",
			CefV8Value::CreateFunction("deleteCurrentSave", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("InstallPackage",
			CefV8Value::CreateFunction("InstallPackage", this), V8_PROPERTY_ATTRIBUTE_NONE);

		v8context->GetGlobal()->SetValue("IsPackageInstalled",
			CefV8Value::CreateFunction("IsPackageInstalled", this), V8_PROPERTY_ATTRIBUTE_NONE);
	}
	bool Execute(const CefString& name,
		CefRefPtr<CefV8Value> object,
		const CefV8ValueList& arguments,
		CefRefPtr<CefV8Value>& retval,
		CefString& exception) override {
		InitConsole();

		if (name == "reloadVehicles") {
			vman.ReloadVehicles();
			retval = CefV8Value::CreateBool(true);
			return true;
		}

		if (name == "getVehicleCount") {
			retval = CefV8Value::CreateInt((int)vman.vehicles.size());
			return true;
		}

		if (name == "getVehicle") {
			if (arguments.size() < 1 || !arguments[0]->IsInt()) {
				exception = "getVehicle: expected integer index argument";
				return true;
			}
			int idx = arguments[0]->GetIntValue();
			if (idx < 0 || idx >= (int)vman.vehicles.size()) {
				exception = "getVehicle: index out of range";
				return true;
			}
			CefString json = CefWriteJSON(SerializeVehicle(vman.vehicles[idx]), JSON_WRITER_DEFAULT);
			retval = CefV8Value::CreateString(json);
			return true;
		}

		if (name == "GetPlatform") {
			#ifdef _WIN32
				retval = CefV8Value::CreateString("Windows");
			#elif __APPLE__
				retval = CefV8Value::CreateString("macOS");
			#elif __linux__
				retval = CefV8Value::CreateString("Linux");
			#else
				retval = CefV8Value::CreateString("Unknown");
			#endif
			return true;
		}

		if (name == "GetArch") {
			#ifdef _WIN32
				SYSTEM_INFO si = {};
				GetNativeSystemInfo(&si);
				switch (si.wProcessorArchitecture) {
					case PROCESSOR_ARCHITECTURE_AMD64: retval = CefV8Value::CreateString("x86_64"); break;
					case PROCESSOR_ARCHITECTURE_ARM64: retval = CefV8Value::CreateString("arm64"); break;
					case PROCESSOR_ARCHITECTURE_INTEL: retval = CefV8Value::CreateString("x86"); break;
					default: retval = CefV8Value::CreateString("unknown"); break;
				}
			#elif defined(__linux__) || defined(__APPLE__)
				struct utsname buf = {};
				if (uname(&buf) == 0) {
					retval = CefV8Value::CreateString(buf.machine);
				} else {
					retval = CefV8Value::CreateString("unknown");
				}
			#else
				retval = CefV8Value::CreateString("unknown");
			#endif
			return true;
		}

		if (name == "SetLanguage") {
			if (arguments.size() < 1 || !arguments[0]->IsInt()) {
				std::cout << "SetLanguage: Invalid argument, expected integer" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
			int language = arguments[0]->GetIntValue();
#ifdef _WIN32
			HKEY hKey;
			if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\GoopieLauncher", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
				RegSetValueExA(hKey, "UserLanguage", 0, REG_DWORD, (const BYTE*)&language, sizeof(DWORD));
				RegCloseKey(hKey);
				retval = CefV8Value::CreateBool(true);
			} else {
				retval = CefV8Value::CreateBool(false);
			}
#else
			ConfigWrite_("UserLanguage", std::to_string(language));
			retval = CefV8Value::CreateBool(true);
#endif
			std::cout << "Language set to: " << language << std::endl;
			return true;
		}

		if (name == "GetLanguage") {
			int language = 1; // Default to English
#ifdef _WIN32
			HKEY hKey;
			if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\GoopieLauncher", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
				DWORD value = 0;
				DWORD size = sizeof(DWORD);
				DWORD type = 0;
				if (RegQueryValueExA(hKey, "UserLanguage", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD) {
					language = static_cast<int>(value);
				}
				RegCloseKey(hKey);
			}
#else
			try { language = std::stoi(ConfigRead_("UserLanguage", "1")); } catch (...) {}
#endif
			retval = CefV8Value::CreateInt(language);
			std::cout << "GetLanguage: " << language << std::endl;
			return true;
		}

		if(name == "SetGamesPath") {
			std::string folder = OpenGamesFolderDialog();
			if (!folder.empty()) {
#ifdef _WIN32
				HKEY hKey;
				if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\GoopieLauncher", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
					RegSetValueExA(hKey, "GamesPath", 0, REG_SZ, (const BYTE*)folder.c_str(), static_cast<DWORD>(folder.size() + 1));
					RegCloseKey(hKey);
					std::cout << "Games path set to: " << folder << std::endl;
				} else {
					std::cout << "Failed to open registry key for writing" << std::endl;
					MessageBoxA(nullptr, "Failed to save games path to registry. Please try again.", "Goopie Launcher", MB_ICONERROR | MB_OK);
				}
#else
				ConfigWrite_("GamesPath", folder);
				std::cout << "Games path set to: " << folder << std::endl;
#endif
			} else {
				std::cout << "No folder selected" << std::endl;
			}
			return true;
		}

		if (name == "GetGamesPath") {
			retval = CefV8Value::CreateString(GetGamesFolder_());
			return true;
		}

		if (name == "testFunction") {
			std::cout << "testFunction called with argument: " << arguments[0]->GetStringValue().ToString() << std::endl;
#ifdef _WIN32
			auto re = MessageBoxA(nullptr, arguments[0]->GetStringValue().ToString().c_str(),
				"Rexglue Launcher", MB_SYSTEMMODAL | MB_ICONQUESTION | MB_YESNOCANCEL);
			retval = CefV8Value::CreateString(re == IDYES ? "yes" : re == IDNO ? "no" : "cancel");
#else
			retval = CefV8Value::CreateString("yes");
#endif
			return true;
		}

		if (name == "OpenExternalLink") {
			std::string url = arguments[0]->GetStringValue().ToString();
#ifdef _WIN32
			ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			system(("xdg-open \"" + url + "\" &").c_str());
#endif
			std::cout << "Opening external link: " << url << std::endl;
			return true;
		}

		if (name == "isIsoInstalled") {
			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::filesystem::path xexPath = std::filesystem::path(GetGamesFolder()) / GameName / "assets" / "default.xex";
			bool installed = fs_exists(xexPath);
			retval = CefV8Value::CreateBool(installed);
			std::cout << "Checking if ISO is installed for game: " << GameName << " - " << (installed ? "Installed" : "Not Installed") << std::endl;
			return true;
		}

		if (name == "isExeUpdated") {
			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::filesystem::path gameDir = std::filesystem::path(GetGamesFolder()) / GameName;

			// Check legacy canonical exe first.
#ifdef _WIN32
			std::filesystem::path canonicalExe = gameDir / (GameName + "-windows-x64.exe");
#else
			std::filesystem::path canonicalExe = gameDir / (GameName + "-linux-x64");
#endif
			bool installed = std::filesystem::exists(canonicalExe);

			// For archive-format installs the canonical name doesn't exist — check the
			// exePath recorded in .installed.json instead.
			if (!installed) {
				std::filesystem::path sidecarPath = gameDir / ".installed.json";
				std::ifstream f(sidecarPath, std::ios::binary);
				if (f) {
					std::string contents(std::istreambuf_iterator<char>(f), {});
					// Extract "exePath" value.
					auto findField = [&](const std::string& key) -> std::string {
						std::string search = "\"" + key + "\":\"";
						size_t pos = contents.find(search);
						if (pos == std::string::npos) { search = "\"" + key + "\": \""; pos = contents.find(search); }
						if (pos == std::string::npos) return "";
						size_t start = pos + search.size();
						size_t end = contents.find("\"", start);
						return (end == std::string::npos) ? "" : contents.substr(start, end - start);
					};
					std::string exePath = findField("exePath");
					if (!exePath.empty()) {
						installed = std::filesystem::exists(gameDir / exePath);
					}
				}
			}

			retval = CefV8Value::CreateBool(installed);
			std::cout << "Checking if exe is updated for game: " << GameName << " - " << (installed ? "Updated" : "Not Updated") << std::endl;
			return true;
		}

		if (name == "getInstalledVersion") {
			// Returns the contents of the `.installed.json` sidecar written by
			// `Update`, as a JSON string. Returns "" when no metadata exists.
			if (arguments.empty() || !arguments[0]->IsString()) {
				retval = CefV8Value::CreateString("");
				return true;
			}
			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::filesystem::path sidecarPath = std::filesystem::path(GetGamesFolder()) / GameName / ".installed.json";
			if (!fs_exists(sidecarPath)) {
				retval = CefV8Value::CreateString("");
				return true;
			}
			std::ifstream f(sidecarPath, std::ios::binary);
			if (!f) {
				retval = CefV8Value::CreateString("");
				return true;
			}
			std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			retval = CefV8Value::CreateString(contents);
			return true;
		}

		if (name == "isExtracting") {
			retval = CefV8Value::CreateBool(isoExtractionProgress->isExtracting); //Checks to see if the iso is currently extracting
			return true;
		}

		if (name == "isUpdating") {
			retval = CefV8Value::CreateBool(staticprogress != -1); //Checks to see if the exe is currently downloading
			return true;
		}

		if (name == "getDownloadProgress") {
			//return random int between 0 and 100 to simulate download progress
			retval = CefV8Value::CreateInt(staticprogress); // The percentage that appears on the progress bar
			return true; 
		}

		if (name == "getExtractProgress") {
			//return random int between 0 and 100 to simulate download progress
			retval = CefV8Value::CreateInt(std::rand() % 101); // The percentage that appears on the progress bar
			return true;
		}

		if (name == "getDownloadString") {
			retval = CefV8Value::CreateString(downloadString); // The string that appears under the progress bar, showing mb downloaded and total mb
			return true;
		}

		if (name == "Install") {
			InitConsole();
			std::string GameName = arguments[0]->GetStringValue().ToString();
			//Open File dialog to select iso, then extract it using the IsoExtraction functions, and save it to the Games/[GameName]/assets/ folder
			std::string iso = IsoExtraction::OpenIsoFileDialog();

			IsoExtraction::ExtractIsoAsync(iso, isoExtractionProgress, GameName);
			std::cout << "Installing game: " << GameName << " from ISO: " << iso << std::endl;

			return true;
		}

		if (name == "Uninstall") {
			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::filesystem::path GamePath = std::filesystem::path(GetGamesFolder()) / GameName;
			if (fs_exists(GamePath)) {
				// Remove all contents except the saves folder
				for (const auto& entry : std::filesystem::directory_iterator(GamePath)) {
					if (entry.path().filename() != "saves") {
						std::filesystem::remove_all(entry.path());
					}
				}
				std::cout << "Uninstalled game: " << GameName << " (saves preserved)" << std::endl;
			}
			return true;
		}

		if (name == "Update") {
			std::cout << "Update called with " << arguments.size() << " arguments" << std::endl;

			// Validate arguments
			if (arguments.size() < 1) {
				std::cout << "Update: No game name provided" << std::endl;
				return true;
			}

			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::cout << "GameName: " << GameName << std::endl;

			// Get GitHubReleaseUrl, use default if not provided or empty
			std::string GameGit;
			if (arguments.size() >= 2 && arguments[1]->IsString()) {
				GameGit = arguments[1]->GetStringValue().ToString();
			}

			// If GameGit is empty, we can't download
			if (GameGit.empty()) {
				std::cout << "Update: No GitHub release URL provided" << std::endl;
				return true;
			}

			// Make sure the URL ends with a trailing slash so we can append asset names.
			if (!GameGit.empty() && GameGit.back() != '/') {
				GameGit += "/";
			}

			std::cout << "GameGit URL: " << GameGit << std::endl;

			// Optional asset name. Defaults to the platform-canonical binary name.
			std::string assetName;
			if (arguments.size() >= 3 && arguments[2]->IsString()) {
				assetName = arguments[2]->GetStringValue().ToString();
			}
			if (assetName.empty()) {
#ifdef _WIN32
				assetName = GameName + "-windows-x64.exe";
#else
				assetName = GameName + "-linux-x64";
#endif
			}

			// Optional version tag, recorded in the sidecar so the UI can show
			// which version is currently installed.
			std::string versionTag;
			if (arguments.size() >= 4 && arguments[3]->IsString()) {
				versionTag = arguments[3]->GetStringValue().ToString();
			}

			// Optional 5th argument: JSON array of zip packages to download and extract.
			// Each entry: {"assetName":"foo.zip","hasExecutable":false,"executablePath":""}
			struct PkgInfo {
				std::string assetName;
				bool hasExecutable;
				std::string executablePath;
			};
			std::vector<PkgInfo> packages;
			if (arguments.size() >= 5 && arguments[4]->IsString()) {
				std::string pkgsJson = arguments[4]->GetStringValue().ToString();
				CefRefPtr<CefValue> parsed = CefParseJSON(CefString(pkgsJson), JSON_PARSER_ALLOW_TRAILING_COMMAS);
				if (parsed && parsed->GetType() == VTYPE_LIST) {
					CefRefPtr<CefListValue> list = parsed->GetList();
					for (size_t pi = 0; pi < list->GetSize(); ++pi) {
						if (list->GetType(pi) != VTYPE_DICTIONARY) continue;
						auto d = list->GetDictionary(pi);
						PkgInfo p;
						p.assetName = d->GetString("assetName").ToString();
						p.hasExecutable = d->GetBool("hasExecutable");
						p.executablePath = d->GetString("executablePath").ToString();
						if (!p.assetName.empty()) packages.push_back(p);
					}
				}
			}

			std::string downloadUrl = GameGit + assetName;
			std::filesystem::path gameDir = std::filesystem::path(GetGamesFolder()) / GameName;

			std::cout << "Download URL: " << downloadUrl << std::endl;

			// Construct toml file paths
			std::string tomlFileName = GameName + ".toml";
			std::string tomlDownloadUrl = GameGit + tomlFileName;
			std::string tomlLocalPath = (gameDir / tomlFileName).string();

			// Sidecar describing what is installed; written after a successful download.
			std::string sidecarPath = (gameDir / ".installed.json").string();
			std::string packagesSidecarPath = (gameDir / ".installed_packages.json").string();

			// For legacy single-exe assets, download to the canonical name so Play()
			// keeps working without needing the sidecar.
			bool archiveAsset = isArchiveAsset(assetName);
			std::string localPath = archiveAsset
				? (gameDir / assetName).string()
#ifdef _WIN32
				: (gameDir / (GameName + "-windows-x64.exe")).string();
#else
				: (gameDir / (GameName + "-linux-x64")).string();
#endif

			std::cout << "Local path: " << localPath << std::endl;

			// Create the directory if it doesn't exist
			if (!fs_exists(gameDir)) {
				fs_mkdirs(gameDir);
			}

			// Shared JSON-escape lambda used by the sidecar writer inside the thread.
			auto jsonEscape = [](const std::string& s) {
				std::string out;
				out.reserve(s.size());
				for (char c : s) {
					switch (c) {
						case '"': out += "\\\""; break;
						case '\\': out += "\\\\"; break;
						case '\n': out += "\\n"; break;
						case '\r': out += "\\r"; break;
						case '\t': out += "\\t"; break;
						default: out += c; break;
					}
				}
				return out;
			};

			// Download from GitHub releases
			std::thread([downloadUrl, localPath, tomlDownloadUrl, tomlLocalPath, sidecarPath, packagesSidecarPath,
			             assetName, versionTag, packages, gameDir, GameGit, archiveAsset, jsonEscape]() {
				try {
					staticprogress = 0;
					Networking::FileDownloader downloader;
					auto mainResult = downloader.downloadFile(downloadUrl, localPath, downloadProgressCallback);

					if (mainResult != Networking::FileDownloader::Result::SUCCESS) {
						std::cout << "Update: failed to download " << downloadUrl << std::endl;
						staticprogress = -1;
						return;
					}

					if (archiveAsset) {
						// ── New multi-file archive format ─────────────────────────────
						std::cout << "Update: extracting archive " << localPath << std::endl;
						bool extracted = ExtractArchive(localPath, gameDir.string());
						std::error_code ec;
						std::filesystem::remove(localPath, ec); // delete archive after extraction

						std::string exeFileName;
						if (extracted) {
							exeFileName = FindMainExecutable(gameDir, gameDir.filename().string());
							if (exeFileName.empty()) {
								std::cout << "Update: warning — no executable found after extraction" << std::endl;
							} else {
								std::cout << "Update: found main executable: " << exeFileName << std::endl;
							}
						} else {
							std::cout << "Update: extraction failed" << std::endl;
						}

						// Write sidecar with exePath so Play() knows what to launch.
						std::ofstream sidecar(sidecarPath, std::ios::binary | std::ios::trunc);
						if (sidecar) {
							sidecar << "{\"version\":\"" << jsonEscape(versionTag)
								<< "\",\"asset\":\"" << jsonEscape(assetName)
								<< "\",\"exePath\":\"" << jsonEscape(exeFileName) << "\"}";
							std::cout << "Update: wrote sidecar: " << sidecarPath << std::endl;
						} else {
							std::cout << "Update: warning — failed to write sidecar at " << sidecarPath << std::endl;
						}
					} else {
						// ── Legacy single-executable format ───────────────────────────
						// Try to download the optional toml config file.
						std::cout << "Checking for toml file: " << tomlDownloadUrl << std::endl;
						Networking::FileDownloader tomlDownloader;
						auto tomlResult = tomlDownloader.downloadFile(tomlDownloadUrl, tomlLocalPath, nullptr);
						if (tomlResult == Networking::FileDownloader::Result::SUCCESS) {
							std::cout << "Successfully downloaded toml file: " << tomlLocalPath << std::endl;
						} else {
							std::cout << "No toml file found in release (this is optional)" << std::endl;
						}

						// Persist installed metadata (no exePath — Play uses canonical name).
						std::ofstream sidecar(sidecarPath, std::ios::binary | std::ios::trunc);
						if (sidecar) {
							sidecar << "{\"version\":\"" << jsonEscape(versionTag)
								<< "\",\"asset\":\"" << jsonEscape(assetName) << "\"}";
							std::cout << "Update: wrote sidecar: " << sidecarPath << std::endl;
						} else {
							std::cout << "Update: warning — failed to write sidecar at " << sidecarPath << std::endl;
						}

#ifndef _WIN32
						// If the downloaded file is a gzip archive (tar.gz), extract it.
						// The asset name may include ".tar.gz" but localPath uses the canonical
						// exe name (no extension), so the archive lands at localPath unextracted.
						{
							unsigned char magic[2] = {0, 0};
							{
								std::ifstream mf(localPath, std::ios::binary);
								if (mf) mf.read(reinterpret_cast<char*>(magic), 2);
							}
							if (magic[0] == 0x1f && magic[1] == 0x8b) {
								std::cout << "Detected tar.gz — extracting..." << std::endl;
								std::string archivePath = localPath + ".tar.gz";
								std::error_code ec;
								std::filesystem::rename(localPath, archivePath, ec);
								if (!ec) {
									std::string gameDirStr = gameDir.string();
									// argv must be non-const pointers; build stable storage
									std::string arg_xzf = "xzf";
									std::string arg_C   = "-C";
									char* tar_argv[] = {
										(char*)"tar",
										arg_xzf.data(),
										archivePath.data(),
										arg_C.data(),
										gameDirStr.data(),
										nullptr
									};
									pid_t tar_pid = -1;
									posix_spawn_file_actions_t fa;
									posix_spawn_file_actions_init(&fa);
									int spawnErr = posix_spawnp(&tar_pid, "tar", &fa, nullptr, tar_argv, environ);
									posix_spawn_file_actions_destroy(&fa);
									if (spawnErr == 0) {
										int wstatus = 0;
										waitpid(tar_pid, &wstatus, 0);
										if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
											std::cout << "tar.gz extracted successfully" << std::endl;
										} else {
											std::cout << "tar exited with error" << std::endl;
										}
									} else {
										std::cout << "posix_spawnp(tar) failed: " << strerror(spawnErr) << std::endl;
									}
									std::filesystem::remove(archivePath, ec);

									// Locate the extracted executable. Archives commonly either:
									//   (a) extract the exe directly to gameDir with the bare game name, or
									//   (b) extract into a top-level subdirectory.
									// In both cases, rename it to localPath so Play() finds it.
									if (!std::filesystem::exists(localPath)) {
										std::string canonicalName = std::filesystem::path(localPath).filename().string();
										std::string gameName      = gameDir.filename().string();
										bool found = false;

										// (a) bare game name at root (e.g. "kameorepowered")
										{
											auto p = gameDir / gameName;
											if (std::filesystem::exists(p)) {
												std::filesystem::rename(p, localPath, ec);
												if (!ec) std::cout << "Moved exe to: " << localPath << std::endl;
												found = true;
											}
										}

										// (b) one level deep in a subdirectory
										if (!found) {
											for (const auto& entry : std::filesystem::directory_iterator(gameDir, ec)) {
												if (!entry.is_directory()) continue;
												for (const std::string& candidate : {canonicalName, gameName}) {
													auto p = entry.path() / candidate;
													if (std::filesystem::exists(p)) {
														std::filesystem::rename(p, localPath, ec);
														if (!ec) std::cout << "Moved exe to: " << localPath << std::endl;
														found = true;
														goto exe_search_done;
													}
												}
											}
										}

										if (!found) {
											std::cout << "Warning: could not find executable after tar extraction" << std::endl;
										}
										exe_search_done:;
									}
								} else {
									std::cout << "Failed to rename archive for extraction: " << ec.message() << std::endl;
								}
							}
						}
#endif
					}

					// Download and extract each zip package (both formats support this).
					for (const auto& pkg : packages) {
						std::cout << "Downloading package: " << pkg.assetName << std::endl;
						staticprogress = 0;
						std::string pkgDownloadUrl = GameGit + pkg.assetName;
						std::string pkgLocalZip = (gameDir / pkg.assetName).string();

						Networking::FileDownloader pkgDownloader;
						auto pkgResult = pkgDownloader.downloadFile(pkgDownloadUrl, pkgLocalZip, downloadProgressCallback);

						if (pkgResult == Networking::FileDownloader::Result::SUCCESS) {
							std::cout << "Extracting package: " << pkg.assetName << " -> " << gameDir.string() << std::endl;
						bool extracted = ExtractZip(pkgLocalZip, gameDir.string());
							std::error_code ec;
							std::filesystem::remove(pkgLocalZip, ec);

							if (extracted) {
								UpdatePackageSidecar(packagesSidecarPath, pkg.assetName);
								std::cout << "Package installed: " << pkg.assetName << std::endl;
							} else {
								std::cout << "Failed to extract package: " << pkg.assetName << std::endl;
							}
						} else {
							std::cout << "Failed to download package: " << pkg.assetName << std::endl;
						}
					}

					staticprogress = -1;
				} catch (const std::exception& e) {
					std::cout << "Download error: " << e.what() << std::endl;
					staticprogress = -1;
				}
			}).detach();

			std::string msg = "Downloading " + downloadUrl + " from GitHub";
			std::cout << msg << std::endl;

			return true;
		}

		if (name == "NeedsUpdate") {
			// NeedsUpdate(GameName, GitHubApiUrl, AssetName?)
			// GitHubApiUrl should be the API endpoint for releases, e.g.:
			// https://api.github.com/repos/owner/repo/releases/latest
			// or .../releases/tags/<tag>. AssetName defaults to the canonical
			// `<GameName>-windows-x64.exe` for backwards compatibility.

			if (arguments.size() < 2) {
				std::cout << "NeedsUpdate: Requires GameName and GitHubApiUrl" << std::endl;
				retval = CefV8Value::CreateBool(true); // Default to needs update if we can't check
				return true;
			}

			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::string GitHubApiUrl = arguments[1]->GetStringValue().ToString();
			std::string assetName;
			if (arguments.size() >= 3 && arguments[2]->IsString()) {
				assetName = arguments[2]->GetStringValue().ToString();
			}
			if (assetName.empty()) {
#ifdef _WIN32
				assetName = GameName + "-windows-x64.exe";
#else
				assetName = GameName + "-linux-x64";
#endif
			}
			std::cout << "NeedsUpdate: GameName: " << GameName
				<< ", GitHubApiUrl: " << GitHubApiUrl
				<< ", AssetName: " << assetName << std::endl;

			std::filesystem::path gameDir = std::filesystem::path(GetGamesFolder()) / GameName;
			std::filesystem::path sidecarPath = gameDir / ".installed.json";

			if (isArchiveAsset(assetName)) {
				// ── Archive format: compare stored version tag against remote tag_name ──
				// We can't SHA-compare extracted files against the archive, so we rely
				// on the version tag written into .installed.json at install time.

				if (!std::filesystem::exists(sidecarPath)) {
					std::cout << "NeedsUpdate: no sidecar found, needs update" << std::endl;
					retval = CefV8Value::CreateBool(true);
					return true;
				}

				// Read installed version from sidecar.
				std::string sidecarContents;
				{
					std::ifstream f(sidecarPath, std::ios::binary);
					if (!f) {
						retval = CefV8Value::CreateBool(true);
						return true;
					}
					sidecarContents = std::string(std::istreambuf_iterator<char>(f), {});
				}

				// Extract "version" field value using simple search.
				auto extractJsonString = [](const std::string& json, const std::string& key) -> std::string {
					std::string search = "\"" + key + "\":\"";
					size_t pos = json.find(search);
					if (pos == std::string::npos) {
						search = "\"" + key + "\": \"";
						pos = json.find(search);
					}
					if (pos == std::string::npos) return "";
					size_t start = pos + search.size();
					size_t end = json.find("\"", start);
					if (end == std::string::npos) return "";
					return json.substr(start, end - start);
				};

				std::string installedTag = extractJsonString(sidecarContents, "version");
				std::cout << "NeedsUpdate: installed tag: " << installedTag << std::endl;

				// Fetch release info and extract tag_name.
				Networking::FileDownloader downloader;
				std::string responseBody;
				auto result = downloader.fetchToString(GitHubApiUrl, responseBody);
				if (result != Networking::FileDownloader::Result::SUCCESS) {
					std::cout << "NeedsUpdate: failed to fetch GitHub API, assuming needs update" << std::endl;
					retval = CefV8Value::CreateBool(true);
					return true;
				}

				std::string remoteTag = extractJsonString(responseBody, "tag_name");
				std::cout << "NeedsUpdate: remote tag: " << remoteTag << std::endl;

				if (remoteTag.empty()) {
					std::cout << "NeedsUpdate: could not parse tag_name, assuming needs update" << std::endl;
					retval = CefV8Value::CreateBool(true);
					return true;
				}

				// Case-insensitive comparison.
				std::string installedLower = installedTag, remoteLower = remoteTag;
				std::transform(installedLower.begin(), installedLower.end(), installedLower.begin(), ::tolower);
				std::transform(remoteLower.begin(), remoteLower.end(), remoteLower.begin(), ::tolower);
				bool needsUpdate = (installedLower != remoteLower);
				std::cout << "NeedsUpdate: " << (needsUpdate ? "update needed" : "up to date") << std::endl;
				retval = CefV8Value::CreateBool(needsUpdate);
				return true;
			}

			// ── Legacy exe format: SHA256 comparison ──────────────────────────────────
#ifdef _WIN32
			std::string canonicalExeName = GameName + "-windows-x64.exe";
#else
			std::string canonicalExeName = GameName + "-linux-x64";
#endif
			std::filesystem::path localExePath = gameDir / canonicalExeName;

			if (!std::filesystem::exists(localExePath)) {
				std::cout << "NeedsUpdate: local exe not found, needs update" << std::endl;
				retval = CefV8Value::CreateBool(true);
				return true;
			}

			std::string localSha = Networking::FileDownloader::calculateFileSHA256(localExePath.string());
			if (localSha.empty()) {
				std::cout << "NeedsUpdate: failed to calculate local SHA, assuming needs update" << std::endl;
				retval = CefV8Value::CreateBool(true);
				return true;
			}
			std::cout << "NeedsUpdate: local SHA256: " << localSha << std::endl;

			Networking::FileDownloader downloader;
			std::string responseBody;
			auto result = downloader.fetchToString(GitHubApiUrl, responseBody);
			if (result != Networking::FileDownloader::Result::SUCCESS) {
				std::cout << "NeedsUpdate: failed to fetch GitHub API: " << downloader.getLastError() << std::endl;
				retval = CefV8Value::CreateBool(true);
				return true;
			}

			std::string remoteSha;
			size_t assetsPos = responseBody.find("\"assets\"");
			if (assetsPos != std::string::npos) {
				std::string nameSearch = "\"name\":\"" + assetName + "\"";
				size_t exeAssetPos = responseBody.find(nameSearch);
				if (exeAssetPos == std::string::npos) {
					nameSearch = "\"name\": \"" + assetName + "\"";
					exeAssetPos = responseBody.find(nameSearch);
				}
				if (exeAssetPos != std::string::npos) {
					std::cout << "NeedsUpdate: found asset in response" << std::endl;
					std::string digestSearch = "\"digest\":";
					size_t digestPos = responseBody.find(digestSearch, exeAssetPos);
					if (digestPos == std::string::npos) {
						digestSearch = "\"digest\": ";
						digestPos = responseBody.find(digestSearch, exeAssetPos);
					}
					if (digestPos != std::string::npos) {
						size_t valueStart = responseBody.find("\"", digestPos + digestSearch.length()) + 1;
						size_t valueEnd = responseBody.find("\"", valueStart);
						if (valueStart != std::string::npos && valueEnd != std::string::npos) {
							std::string digestValue = responseBody.substr(valueStart, valueEnd - valueStart);
							size_t colonPos = digestValue.find(':');
							remoteSha = (colonPos != std::string::npos)
								? digestValue.substr(colonPos + 1)
								: digestValue;
						}
					}
				}
			}

			if (!remoteSha.empty()) {
				std::transform(localSha.begin(), localSha.end(), localSha.begin(), ::tolower);
				std::transform(remoteSha.begin(), remoteSha.end(), remoteSha.begin(), ::tolower);
				bool needsUpdate = (localSha != remoteSha);
				std::cout << "NeedsUpdate: remote SHA256: " << remoteSha << std::endl;
				std::cout << "NeedsUpdate: " << (needsUpdate ? "update needed" : "up to date") << std::endl;
				retval = CefV8Value::CreateBool(needsUpdate);
				return true;
			}

			std::cout << "NeedsUpdate: could not find digest in response, assuming needs update" << std::endl;
			retval = CefV8Value::CreateBool(true);
			return true;
		}

		if (name == "Play") {
			std::string GameName = arguments[0]->GetStringValue().ToString();

			std::cout << "Starting Game: " << GameName << std::endl;

			// NEW: optional second arg is the cvar string built by the launcher UI,
			// e.g. "-console true -numberofcoins 999 -health 1.0"
			std::string cvarArgs;
			if (arguments.size() > 1 && arguments[1] && arguments[1]->IsString()) {
				cvarArgs = arguments[1]->GetStringValue().ToString();
			}

			// Optional third arg: relative exe path from a zip package,
			// e.g. "launcher.exe" or "bin/launcher.exe".
			// Falls back to the canonical "<GameName>-windows-x64.exe".
			std::string customExePath;
			if (arguments.size() > 2 && arguments[2] && arguments[2]->IsString()) {
				customExePath = arguments[2]->GetStringValue().ToString();
			}

			// Optional fourth arg: whether to append --game_data_root=".../assets".
			// Defaults to false for backward compatibility with existing games.
			bool setGameDataRootToAssets = false;
			if (arguments.size() > 3 && arguments[3]) {
				if (arguments[3]->IsBool()) {
					setGameDataRootToAssets = arguments[3]->GetBoolValue();
				} else if (arguments[3]->IsInt()) {
					setGameDataRootToAssets = arguments[3]->GetIntValue() != 0;
				} else if (arguments[3]->IsString()) {
					std::string s = arguments[3]->GetStringValue().ToString();
					std::transform(s.begin(), s.end(), s.begin(), ::tolower);
					setGameDataRootToAssets = (s == "1" || s == "true" || s == "yes");
				}
			}

			std::filesystem::path gameDir2 = std::filesystem::path(GetGamesFolder()) / GameName;
			std::filesystem::path exePath;
			if (!customExePath.empty()) {
				// Explicit path from JS takes priority (e.g. from a package with hasExecutable).
#ifdef _WIN32
				std::replace(customExePath.begin(), customExePath.end(), '/', '\\');
#endif
				exePath = gameDir2 / customExePath;
			} else {
				// For archive-format installs the exe name is stored in .installed.json.
				std::string sidecarExePath;
				{
					std::filesystem::path sidecarPath = gameDir2 / ".installed.json";
					std::ifstream f(sidecarPath, std::ios::binary);
					if (f) {
						std::string contents(std::istreambuf_iterator<char>(f), {});
						std::string search = "\"exePath\":\"";
						size_t pos = contents.find(search);
						if (pos == std::string::npos) { search = "\"exePath\": \""; pos = contents.find(search); }
						if (pos != std::string::npos) {
							size_t start = pos + search.size();
							size_t end = contents.find("\"", start);
							if (end != std::string::npos) sidecarExePath = contents.substr(start, end - start);
						}
					}
				}
				if (!sidecarExePath.empty()) {
#ifdef _WIN32
					std::replace(sidecarExePath.begin(), sidecarExePath.end(), '/', '\\');
#endif
					exePath = gameDir2 / sidecarExePath;
				} else {
#ifdef _WIN32
					exePath = gameDir2 / (GameName + "-windows-x64.exe");
#else
					exePath = gameDir2 / (GameName + "-linux-x64");
#endif
				}
			}
			if (fs_exists(exePath)) {
				std::string patches = "";
				std::string launchDir = (std::filesystem::path(GetGamesFolder()) / GameName).string();
				std::string exeFileName = exePath.filename().string();

				int userLanguage = 1;
#ifdef _WIN32
				HKEY hKey;
				if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\GoopieLauncher", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
					DWORD value = 0;
					DWORD size = sizeof(DWORD);
					DWORD type = 0;
					if (RegQueryValueExA(hKey, "UserLanguage", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD) {
						userLanguage = static_cast<int>(value);
					}
					RegCloseKey(hKey);
				}
#else
				try { userLanguage = std::stoi(ConfigRead_("UserLanguage", "1")); } catch (...) {}
#endif
				std::string launchArgs = "--user_language=" + std::to_string(userLanguage);
				if (setGameDataRootToAssets) {
					launchArgs += " --game_data_root=\"" + (std::filesystem::path(launchDir) / "assets").string() + "\"";
				}

				if (!cvarArgs.empty()) {
					launchArgs += " " + cvarArgs;
				}

				std::string fullExePath = exePath.string();
				std::thread([patches, fullExePath, exeFileName, launchDir, launchArgs]() {
#ifdef _WIN32
					std::string command = "\"" + fullExePath + "\"" + patches;
					if (!launchArgs.empty()) command += " " + launchArgs;

					std::vector<char> cmdBuffer(command.begin(), command.end());
					cmdBuffer.push_back('\0');

					STARTUPINFOA si = {};
					si.cb = sizeof(si);
					PROCESS_INFORMATION pi = {};

					BOOL ok = CreateProcessA(
						fullExePath.c_str(),
						cmdBuffer.data(),
						nullptr, nullptr, FALSE, 0, nullptr,
						launchDir.c_str(), &si, &pi);

					if (!ok) {
						DWORD err = GetLastError();
						std::cerr << "Play: CreateProcess failed (" << err << ")" << std::endl;
						std::string msg = "Failed to launch game.\n\nCommand: " + command +
							"\nWorking dir: " + launchDir +
							"\nError code: " + std::to_string(err);
						MessageBoxA(nullptr, msg.c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
						return;
					}

					gameProcessHandle = pi.hProcess;
					gameIsRunning = true;
					CloseHandle(pi.hThread);
					WaitForSingleObject(pi.hProcess, INFINITE);
					CloseHandle(pi.hProcess);
					gameProcessHandle = nullptr;
					gameIsRunning = false;
#else
					// Ensure the binary is executable (downloads don't preserve +x)
					chmod(fullExePath.c_str(), 0755);

					// Build argv from space-separated launchArgs (simple split; avoids shell injection)
					std::string shellCmd = "\"" + fullExePath + "\"";
					if (!launchArgs.empty()) shellCmd += " " + launchArgs;

					char* argv_sh[] = {
						(char*)"/bin/sh", (char*)"-c", shellCmd.data(), nullptr
					};
					posix_spawn_file_actions_t fa;
					posix_spawn_file_actions_init(&fa);
					pid_t pid;
					int err = posix_spawn(&pid, "/bin/sh", &fa, nullptr, argv_sh, environ);
					posix_spawn_file_actions_destroy(&fa);

					if (err != 0) {
						std::cerr << "Play: posix_spawn failed: " << strerror(err) << std::endl;
						return;
					}
					gameProcessHandle = pid;
					gameIsRunning = true;
					int status;
					waitpid(pid, &status, 0);
					gameProcessHandle = -1;
					gameIsRunning = false;
#endif
				}).detach();
			}
			else {
				std::cerr << "Play: Executable not found: " << exePath.string() << std::endl;
#ifdef _WIN32
				MessageBoxA(nullptr, ("Game executable not found:\n" + exePath.string()).c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
#endif
			}
			return true;
		}

		if (name == "getVersion") {
			retval = CefV8Value::CreateInt(10); // Returns the current version of the Launcher exe
			return true;
		}

		// ── InstallPackage ─────────────────────────────────────────────────────
		// InstallPackage(GameName, DownloadPrefix, ZipAssetName, HasExecutable, ExecutablePath)
		// Downloads a ZIP from the GitHub release and extracts it into the game
		// folder, then records it in .installed_packages.json.
		if (name == "InstallPackage") {
			if (arguments.size() < 3 || !arguments[0]->IsString() || !arguments[2]->IsString()) {
				std::cout << "InstallPackage: invalid arguments" << std::endl;
				return true;
			}

			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::string DownloadPrefix;
			if (arguments.size() >= 2 && arguments[1]->IsString()) {
				DownloadPrefix = arguments[1]->GetStringValue().ToString();
			}
			if (!DownloadPrefix.empty() && DownloadPrefix.back() != '/') DownloadPrefix += '/';
			std::string ZipAssetName = arguments[2]->GetStringValue().ToString();

			std::filesystem::path gameDir = std::filesystem::path(GetGamesFolder()) / GameName;
			std::string zipLocalPath = (gameDir / ZipAssetName).string();
			std::string packagesSidecarPath = (gameDir / ".installed_packages.json").string();

			if (!fs_exists(gameDir)) {
				fs_mkdirs(gameDir);
			}

			std::thread([GameName, DownloadPrefix, ZipAssetName, zipLocalPath, gameDir, packagesSidecarPath]() {
				try {
					std::string downloadUrl = DownloadPrefix + ZipAssetName;
					std::cout << "InstallPackage: downloading " << downloadUrl << std::endl;
					staticprogress = 0;

					Networking::FileDownloader pkgDownloader;
					auto result = pkgDownloader.downloadFile(downloadUrl, zipLocalPath, downloadProgressCallback);

					if (result == Networking::FileDownloader::Result::SUCCESS) {
						std::cout << "InstallPackage: extracting " << ZipAssetName << std::endl;
						bool extracted = ExtractZip(zipLocalPath, gameDir.string());
						std::error_code ec;
						std::filesystem::remove(zipLocalPath, ec);

						if (extracted) {
							UpdatePackageSidecar(packagesSidecarPath, ZipAssetName);
							std::cout << "InstallPackage: done " << ZipAssetName << std::endl;
						} else {
							std::cout << "InstallPackage: extraction failed for " << ZipAssetName << std::endl;
						}
					} else {
						std::cout << "InstallPackage: download failed for " << ZipAssetName << std::endl;
					}

					staticprogress = -1;
				} catch (const std::exception& e) {
					std::cout << "InstallPackage error: " << e.what() << std::endl;
					staticprogress = -1;
				}
			}).detach();

			return true;
		}

		// ── IsPackageInstalled ─────────────────────────────────────────────────
		// IsPackageInstalled(GameName, ZipAssetName) -> bool
		// Returns true if the named package appears in .installed_packages.json.
		if (name == "IsPackageInstalled") {
			if (arguments.size() < 2 || !arguments[0]->IsString() || !arguments[1]->IsString()) {
				retval = CefV8Value::CreateBool(false);
				return true;
			}
			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::string ZipAssetName = arguments[1]->GetStringValue().ToString();

			std::filesystem::path sidecarPath =
				std::filesystem::path(GetGamesFolder()) / GameName / ".installed_packages.json";

			if (!fs_exists(sidecarPath)) {
				retval = CefV8Value::CreateBool(false);
				return true;
			}

			std::ifstream rf(sidecarPath, std::ios::binary);
			if (!rf) {
				retval = CefV8Value::CreateBool(false);
				return true;
			}
			std::string contents((std::istreambuf_iterator<char>(rf)), {});
			// The key in the JSON is the quoted asset name
			bool installed = contents.find("\"" + ZipAssetName + "\"") != std::string::npos;
			retval = CefV8Value::CreateBool(installed);
			return true;
		}

		if (name == "getSaveSlots") {
			std::string recompName = arguments[0]->GetStringValue().ToString();
			std::filesystem::path savesPath = std::filesystem::path(GetGamesFolder()) / recompName / "saves";

			std::vector<CefString> slots;
			if (fs_exists(savesPath) && fs_is_dir(savesPath)) {
				for (const auto& entry : std::filesystem::directory_iterator(savesPath)) {
					if (entry.is_directory()) {
						slots.push_back(entry.path().filename().string());
					}
				}
			}

			CefRefPtr<CefV8Value> arr = CefV8Value::CreateArray(static_cast<int>(slots.size()));
			for (size_t i = 0; i < slots.size(); ++i) {
				arr->SetValue(static_cast<int>(i), CefV8Value::CreateString(slots[i]));
			}
			retval = arr;
			std::cout << "getSaveSlots for " << recompName << ": found " << slots.size() << " slots" << std::endl;
			return true;
		}

		if (name == "getSaveSlotCount") {
			std::string recompName = arguments[0]->GetStringValue().ToString();
			std::filesystem::path savesPath = std::filesystem::path(GetGamesFolder()) / recompName / "saves";

			int count = 0;
			if (fs_exists(savesPath) && fs_is_dir(savesPath)) {
				for (const auto& entry : std::filesystem::directory_iterator(savesPath)) {
					if (entry.is_directory()) {
						count++;
					}
				}
			}

			retval = CefV8Value::CreateInt(count);
			std::cout << "getSaveSlotCount for " << recompName << ": " << count << std::endl;
			return true;
		}

		if (name == "getActiveSave") {
			std::string recompName = arguments[0]->GetStringValue().ToString();
			std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";

			std::string activeSave = "";
			if (fs_exists(activeSavePath)) {
				std::ifstream file(activeSavePath);
				if (file.is_open()) {
					std::getline(file, activeSave);
					file.close();
				}
			}

			retval = CefV8Value::CreateString(activeSave);
			std::cout << "getActiveSave for " << recompName << ": " << (activeSave.empty() ? "(default)" : activeSave) << std::endl;
			return true;
		}

		if (name == "backupSave") {
			std::string recompName = arguments[0]->GetStringValue().ToString();
			std::string saveName = arguments[1]->GetStringValue().ToString();

#ifdef _WIN32
			char documentsPath[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath))) {
				std::cout << "backupSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
			std::string docsDir(documentsPath);
#else
			std::string docsDir = GetDocumentsPath_();
			if (docsDir.empty()) {
				std::cout << "backupSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
#endif

			std::filesystem::path sourcePath = std::filesystem::path(docsDir) / recompName;
			std::filesystem::path destPath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / saveName;

			try {
				if (!fs_exists(sourcePath)) {
					std::cout << "backupSave: Source path does not exist: " << sourcePath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				// Create destination directory
				fs_mkdirs(destPath);

				// Copy contents recursively
				std::filesystem::copy(sourcePath, destPath, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);

				// Update active save marker
				std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";
				std::ofstream activeFile(activeSavePath);
				if (activeFile.is_open()) {
					activeFile << saveName;
					activeFile.close();
				}

				std::cout << "backupSave: Successfully backed up " << recompName << " to " << saveName << std::endl;
				retval = CefV8Value::CreateBool(true);
			} catch (const std::exception& e) {
				std::cout << "backupSave: Error - " << e.what() << std::endl;
				retval = CefV8Value::CreateBool(false);
			}
			return true;
		}

		if (name == "restoreSave") {
			std::string recompName = arguments[0]->GetStringValue().ToString();
			std::string saveName = arguments[1]->GetStringValue().ToString();

#ifdef _WIN32
			char documentsPath[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath))) {
				std::cout << "restoreSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
			std::string docsDir(documentsPath);
#else
			std::string docsDir = GetDocumentsPath_();
			if (docsDir.empty()) {
				std::cout << "restoreSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
#endif

			std::filesystem::path sourcePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / saveName;
			std::filesystem::path destPath = std::filesystem::path(docsDir) / recompName;

			try {
				if (!fs_exists(sourcePath)) {
					std::cout << "restoreSave: Source save does not exist: " << sourcePath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				// Remove existing destination and recreate
				if (fs_exists(destPath)) {
					std::filesystem::remove_all(destPath);
				}
				fs_mkdirs(destPath);

				// Copy contents recursively
				std::filesystem::copy(sourcePath, destPath, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);

				// Update active save marker
				std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";
				std::ofstream activeFile(activeSavePath);
				if (activeFile.is_open()) {
					activeFile << saveName;
					activeFile.close();
				}

				std::cout << "restoreSave: Successfully restored " << saveName << " to " << recompName << std::endl;
				retval = CefV8Value::CreateBool(true);
			} catch (const std::exception& e) {
				std::cout << "restoreSave: Error - " << e.what() << std::endl;
				retval = CefV8Value::CreateBool(false);
			}
			return true;
		}

		if (name == "deleteSave") {
			std::string recompName = arguments[0]->GetStringValue().ToString();
			std::string saveName = arguments[1]->GetStringValue().ToString();

			std::filesystem::path savePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / saveName;

			try {
				if (!fs_exists(savePath)) {
					std::cout << "deleteSave: Save does not exist: " << savePath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				std::filesystem::remove_all(savePath);

				// Clear active save marker if this was the active save
				std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";
				if (fs_exists(activeSavePath)) {
					std::ifstream activeFile(activeSavePath);
					std::string activeSave;
					if (activeFile.is_open()) {
						std::getline(activeFile, activeSave);
						activeFile.close();
						if (activeSave == saveName) {
							std::filesystem::remove(activeSavePath);
						}
					}
				}

				std::cout << "deleteSave: Successfully deleted " << saveName << " for " << recompName << std::endl;
				retval = CefV8Value::CreateBool(true);
			} catch (const std::exception& e) {
				std::cout << "deleteSave: Error - " << e.what() << std::endl;
				retval = CefV8Value::CreateBool(false);
			}
			return true;
		}

		if (name == "renameSave") {
			std::string recompName = arguments[0]->GetStringValue().ToString();
			std::string oldName = arguments[1]->GetStringValue().ToString();
			std::string newName = arguments[2]->GetStringValue().ToString();

			std::filesystem::path oldPath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / oldName;
			std::filesystem::path newPath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / newName;

			try {
				if (!fs_exists(oldPath)) {
					std::cout << "renameSave: Source save does not exist: " << oldPath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				if (fs_exists(newPath)) {
					std::cout << "renameSave: Destination already exists: " << newPath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				std::filesystem::rename(oldPath, newPath);

				// Update active save marker if this was the active save
				std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";
				if (fs_exists(activeSavePath)) {
					std::ifstream activeFile(activeSavePath);
					std::string activeSave;
					if (activeFile.is_open()) {
						std::getline(activeFile, activeSave);
						activeFile.close();
						if (activeSave == oldName) {
							std::ofstream outFile(activeSavePath);
							if (outFile.is_open()) {
								outFile << newName;
								outFile.close();
							}
						}
					}
				}

				std::cout << "renameSave: Successfully renamed " << oldName << " to " << newName << " for " << recompName << std::endl;
				retval = CefV8Value::CreateBool(true);
			} catch (const std::exception& e) {
				std::cout << "renameSave: Error - " << e.what() << std::endl;
				retval = CefV8Value::CreateBool(false);
			}
			return true;
		}

		if (name == "OpenGamesFolder") {
			std::string gamesFolder = GetGamesFolder_();
			if (!fs_exists(gamesFolder))
				fs_mkdirs(gamesFolder);
#ifdef _WIN32
			ShellExecuteA(nullptr, "open", gamesFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			system(("xdg-open \"" + gamesFolder + "\" &").c_str());
#endif
			std::cout << "Opening games folder: " << gamesFolder << std::endl;
			retval = CefV8Value::CreateBool(true);
			return true;
		}

		if (name == "openSaveFolder") {
			if (arguments.size() < 1 || !arguments[0]->IsString()) {
				std::cout << "openSaveFolder: Missing recompName argument" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}

			std::string recompName = arguments[0]->GetStringValue().ToString();

#ifdef _WIN32
			char documentsPath[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath))) {
				std::cout << "openSaveFolder: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
			std::string docsDir(documentsPath);
#else
			std::string docsDir = GetDocumentsPath_();
			if (docsDir.empty()) {
				std::cout << "openSaveFolder: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
#endif

			std::filesystem::path saveFolderPath = std::filesystem::path(docsDir) / recompName;
			if (!fs_exists(saveFolderPath))
				fs_mkdirs(saveFolderPath);

#ifdef _WIN32
			ShellExecuteA(nullptr, "open", saveFolderPath.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			system(("xdg-open \"" + saveFolderPath.string() + "\" &").c_str());
#endif
			std::cout << "Opening save folder for " << recompName << ": " << saveFolderPath.string() << std::endl;
			retval = CefV8Value::CreateBool(true);
			return true;
		}

		if (name == "deleteCurrentSave") {
			if (arguments.size() < 1 || !arguments[0]->IsString()) {
				std::cout << "deleteCurrentSave: Missing recompName argument" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}

			std::string recompName = arguments[0]->GetStringValue().ToString();

#ifdef _WIN32
			char documentsPath[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath))) {
				std::cout << "deleteCurrentSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
			std::string docsDir(documentsPath);
#else
			std::string docsDir = GetDocumentsPath_();
			if (docsDir.empty()) {
				std::cout << "deleteCurrentSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
#endif

			std::filesystem::path saveFolderPath = std::filesystem::path(docsDir) / recompName;

			try {
				if (!fs_exists(saveFolderPath)) {
					std::cout << "deleteCurrentSave: Save folder does not exist: " << saveFolderPath << std::endl;
					// Return true since the goal (no save data) is achieved
					retval = CefV8Value::CreateBool(true);
					return true;
				}

				// Delete the entire save folder
				std::filesystem::remove_all(saveFolderPath);

				// Clear active save marker
				std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";
				if (fs_exists(activeSavePath)) {
					std::filesystem::remove(activeSavePath);
				}

				std::cout << "deleteCurrentSave: Successfully deleted save data for " << recompName << std::endl;
				retval = CefV8Value::CreateBool(true);
			} catch (const std::exception& e) {
				std::cout << "deleteCurrentSave: Error - " << e.what() << std::endl;
				retval = CefV8Value::CreateBool(false);
			}
			return true;
		}


		return false;
	}
};

#ifndef _WIN32
class RexRenderHandler : public CefRenderHandler {
	IMPLEMENT_REFCOUNTING(RexRenderHandler);
public:
	void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
		rect.Set(0, 0, g_osr_w, g_osr_h);
	}
	void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
	             const RectList&, const void* buffer, int w, int h) override {
		if (type != PET_VIEW) return;
		std::lock_guard<std::mutex> lk(g_osr_mutex);
		g_osr_pixels.assign(static_cast<const uint8_t*>(buffer),
		                     static_cast<const uint8_t*>(buffer) + w * h * 4);
		g_osr_w = w; g_osr_h = h;
		g_osr_dirty = true;
	}
};
#endif

class RexClient : public CefClient, public CefLifeSpanHandler {
	IMPLEMENT_REFCOUNTING(RexClient);
#ifndef _WIN32
	CefRefPtr<RexRenderHandler> m_renderHandler{new RexRenderHandler()};
	CefRefPtr<CefRenderHandler> GetRenderHandler() override { return m_renderHandler; }
#endif

	CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
		return this;
	}

	// Handle popup windows - allow Firebase auth popups, navigate others in main frame
	bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
		CefRefPtr<CefFrame> frame,
		int popup_id,
		const CefString& target_url,
		const CefString& target_frame_name,
		CefLifeSpanHandler::WindowOpenDisposition target_disposition,
		bool user_gesture,
		const CefPopupFeatures& popupFeatures,
		CefWindowInfo& windowInfo,
		CefRefPtr<CefClient>& client,
		CefBrowserSettings& settings,
		CefRefPtr<CefDictionaryValue>& extra_info,
		bool* no_javascript_access) override {
		std::string url = target_url.ToString();

		// Check if this is a Firebase/OAuth authentication popup that needs to open as a real popup
		bool isAuthPopup = 
			url.find("accounts.google.com") != std::string::npos ||
			url.find("firebaseapp.com") != std::string::npos ||
			url.find("firebase.google.com") != std::string::npos ||
			url.find("googleapis.com") != std::string::npos ||
			url.find("github.com/login") != std::string::npos ||
			url.find("discord.com/oauth") != std::string::npos;

		if (isAuthPopup && !url.empty()) {
			// Allow auth popups to open as real CEF popup windows for proper OAuth flow
			std::cout << "Allowing auth popup: " << url << std::endl;
			return false; // Allow the popup to open
		}

		// For non-auth popups, navigate the current browser to the URL
		if (!target_url.empty() && browser && browser->GetMainFrame()) {
			browser->GetMainFrame()->LoadURL(target_url);
		}
		// Return true to cancel the popup (we're handling it ourselves)
		return true;
	}

	void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
		g_browser = browser;
		// Get the CEF window handle
		cef_window_handle = browser->GetHost()->GetWindowHandle();
	}

	void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
		g_browser = nullptr;
#ifdef _WIN32
		cef_window_handle = nullptr;
#else
		cef_window_handle = 0;
#endif
		is_browser_closed = true;
	}
};

CefRefPtr<RexClient> client;

// Check if localhost:port is available
// Probe a single loopback address (already filled sockaddr + length).
// Returns true if a TCP connection succeeds within timeoutMs.
static bool ProbeLoopbackAddress(int af, const sockaddr* addr, socklen_t addrLen, int timeoutMs) {
#ifdef _WIN32
	SOCKET sock = socket(af, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET) return false;

	u_long mode = 1;
	ioctlsocket(sock, FIONBIO, &mode);
	connect(sock, addr, addrLen);

	fd_set writeSet; FD_ZERO(&writeSet); FD_SET(sock, &writeSet);
	timeval timeout{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
	bool available = select(0, nullptr, &writeSet, nullptr, &timeout) > 0;

	closesocket(sock);
	return available;
#else
	int sock = socket(af, SOCK_STREAM, 0);
	if (sock < 0) return false;

	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);
	connect(sock, addr, addrLen);

	fd_set writeSet; FD_ZERO(&writeSet); FD_SET(sock, &writeSet);
	timeval timeout{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
	bool available = false;
	if (select(sock + 1, nullptr, &writeSet, nullptr, &timeout) > 0) {
		int err = 0; socklen_t len = sizeof(err);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
		available = (err == 0);
	}

	close(sock);
	return available;
#endif
}

// Check whether something is listening on the given loopback port.
// Tries IPv6 (::1) first, then IPv4 (127.0.0.1), so it works correctly
// when the server binds to only one family (e.g. Vite defaults to IPv6
// on systems where localhost resolves to ::1).
bool IsLocalhostAvailable(int port, int timeoutMs = 500) {
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
#endif

	// IPv6 probe: ::1
	sockaddr_in6 addr6{};
	addr6.sin6_family = AF_INET6;
	addr6.sin6_port = htons(port);
	inet_pton(AF_INET6, "::1", &addr6.sin6_addr);
	if (ProbeLoopbackAddress(AF_INET6, (sockaddr*)&addr6, sizeof(addr6), timeoutMs)) {
#ifdef _WIN32
		WSACleanup();
#endif
		return true;
	}

	// IPv4 probe: 127.0.0.1
	sockaddr_in addr4{};
	addr4.sin_family = AF_INET;
	addr4.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &addr4.sin_addr);
	bool available = ProbeLoopbackAddress(AF_INET, (sockaddr*)&addr4, sizeof(addr4), timeoutMs);

#ifdef _WIN32
	WSACleanup();
#endif
	return available;
}

void window_resize_callback(GLFWwindow* win, int width, int height) {
#ifdef _WIN32
	if (g_browser && g_browser->GetHost() && cef_window_handle) {
		RECT rect{};
		GetClientRect(glfwGetWin32Window(win), &rect);
		SetWindowPos(cef_window_handle, NULL, 0, 0,
			rect.right - rect.left, rect.bottom - rect.top,
			SWP_NOZORDER | SWP_NOACTIVATE);
		g_browser->GetHost()->NotifyMoveOrResizeStarted();
		g_browser->GetHost()->WasResized();
	}
#else
	int fw, fh;
	glfwGetFramebufferSize(win, &fw, &fh);
	glViewport(0, 0, fw, fh);
	{
		std::lock_guard<std::mutex> lk(g_osr_mutex);
		g_osr_w = fw; g_osr_h = fh;
	}
	if (g_browser && g_browser->GetHost())
		g_browser->GetHost()->WasResized();
	(void)width; (void)height;
#endif
}


#ifdef _WIN32
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	(void)hPrevInstance; (void)nCmdShow;
	std::string cmdLine(lpCmdLine);
	if (cmdLine.find("--console") != std::string::npos) {
		g_consoleEnabled = true;
		InitConsole();
		IsoExtraction::EnableConsoleLogging();
	}
	const CefMainArgs main_args(hInstance);
#else
int main(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) == "--console") {
			IsoExtraction::EnableConsoleLogging();
		}
	}
	const CefMainArgs main_args(argc, argv);
#endif

	CefRefPtr<RexApp> app(new RexApp);
	if (const auto code = CefExecuteProcess(main_args, app.get(), nullptr); code >= 0) {
		return code;
	}
	CefSettings settings;
	settings.multi_threaded_message_loop = true;
#ifndef _WIN32
	settings.windowless_rendering_enabled = true;
#endif
	CefInitialize(main_args, settings, app.get(), nullptr);

	while (!is_cef_initialized.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	VinceWindow window(1280, 720, "Vince Engine");

#ifdef _WIN32
	SendMessage(glfwGetWin32Window(window.getWindow()), WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1)));
	SendMessage(glfwGetWin32Window(window.getWindow()), WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1)));

	HWND hwnd = glfwGetWin32Window(window.getWindow());
	original_wnd_proc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc);
	g_mainWindowHandle = hwnd;
	IsoExtraction::SetMainWindowHandle(hwnd);
#else
	IsoExtraction::SetMainWindowHandle(nullptr);
#endif

	const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
	glfwWindowHint(GLFW_RED_BITS, mode->redBits);
	glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
	glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
	glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

	glfwSetWindowSizeCallback(window.getWindow(), window_resize_callback);
	window_resize_callback(window.getWindow(), mode->width / 1.5, mode->height / 1.5);

	client = new RexClient();

	int fw, fh;
	glfwGetFramebufferSize(window.getWindow(), &fw, &fh);
	CefRect cef_rect(0, 0, fw, fh);

	CefWindowInfo window_info;
#ifdef _WIN32
	window_info.SetAsChild(glfwGetWin32Window(window.getWindow()), cef_rect);
#else
	{
		std::lock_guard<std::mutex> lk(g_osr_mutex);
		g_osr_w = fw; g_osr_h = fh;
	}
	window_info.SetAsWindowless(0);
#endif

	std::string browserUrl = "https://goopie.xyz";
	if (IsLocalhostAvailable(5173)) {
		browserUrl = "http://localhost:5173/";
		std::cout << "Using development server at localhost:5173" << std::endl;
	} else {
		std::cout << "Using production server at goopie.xyz" << std::endl;
	}
	CefBrowserHost::CreateBrowser(window_info, client.get(), browserUrl, CefBrowserSettings(), nullptr, nullptr);

#ifndef _WIN32
	// ── Fullscreen-quad shader for OSR output ─────────────────────────────────
	const char* osr_vs_src = R"GLSL(
#version 130
in vec4 aPos;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    vUV = aPos.zw;
}
)GLSL";
	const char* osr_fs_src = R"GLSL(
#version 130
in vec2 vUV;
uniform sampler2D uTex;
void main() {
    gl_FragColor = texture(uTex, vUV);
}
)GLSL";
	auto compile_sh = [](GLenum t, const char* s) {
		GLuint sh = glCreateShader(t);
		glShaderSource(sh, 1, &s, nullptr);
		glCompileShader(sh);
		return sh;
	};
	GLuint osr_vs = compile_sh(GL_VERTEX_SHADER,   osr_vs_src);
	GLuint osr_fs = compile_sh(GL_FRAGMENT_SHADER, osr_fs_src);
	GLuint osr_prog = glCreateProgram();
	glAttachShader(osr_prog, osr_vs);
	glAttachShader(osr_prog, osr_fs);
	glBindAttribLocation(osr_prog, 0, "aPos");
	glLinkProgram(osr_prog);
	glDeleteShader(osr_vs);
	glDeleteShader(osr_fs);
	glUseProgram(osr_prog);
	glUniform1i(glGetUniformLocation(osr_prog, "uTex"), 0);

	// bottom-left(-1,-1) → UV(0,1) … top-right(1,1) → UV(1,0)  (flip Y for CEF top-down buffer)
	const float osr_verts[] = {
		-1.f, -1.f,  0.f, 1.f,
		 1.f, -1.f,  1.f, 1.f,
		-1.f,  1.f,  0.f, 0.f,
		 1.f,  1.f,  1.f, 0.f,
	};
	GLuint osr_vao, osr_vbo, osr_tex;
	glGenVertexArrays(1, &osr_vao);
	glGenBuffers(1, &osr_vbo);
	glBindVertexArray(osr_vao);
	glBindBuffer(GL_ARRAY_BUFFER, osr_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(osr_verts), osr_verts, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
	glBindVertexArray(0);

	glGenTextures(1, &osr_tex);
	glBindTexture(GL_TEXTURE_2D, osr_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	const uint8_t osr_blank[4] = {0, 0, 0, 255};
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, osr_blank);
	glBindTexture(GL_TEXTURE_2D, 0);

	// ── Input forwarding ──────────────────────────────────────────────────────
	glfwSetCursorPosCallback(window.getWindow(), [](GLFWwindow*, double x, double y) {
		g_cursor_x = x; g_cursor_y = y;
		if (!g_browser) return;
		CefMouseEvent e{}; e.x = (int)x; e.y = (int)y;
		g_browser->GetHost()->SendMouseMoveEvent(e, false);
	});
	glfwSetMouseButtonCallback(window.getWindow(), [](GLFWwindow*, int btn, int action, int mods) {
		if (!g_browser) return;
		CefMouseEvent e{}; e.x = (int)g_cursor_x; e.y = (int)g_cursor_y;
		if (mods & GLFW_MOD_SHIFT)   e.modifiers |= EVENTFLAG_SHIFT_DOWN;
		if (mods & GLFW_MOD_CONTROL) e.modifiers |= EVENTFLAG_CONTROL_DOWN;
		if (mods & GLFW_MOD_ALT)     e.modifiers |= EVENTFLAG_ALT_DOWN;
		auto cbt = btn == GLFW_MOUSE_BUTTON_RIGHT  ? MBT_RIGHT
		         : btn == GLFW_MOUSE_BUTTON_MIDDLE ? MBT_MIDDLE : MBT_LEFT;
		g_browser->GetHost()->SendMouseClickEvent(e, cbt, action == GLFW_RELEASE, 1);
	});
	glfwSetScrollCallback(window.getWindow(), [](GLFWwindow*, double dx, double dy) {
		if (!g_browser) return;
		CefMouseEvent e{}; e.x = (int)g_cursor_x; e.y = (int)g_cursor_y;
		g_browser->GetHost()->SendMouseWheelEvent(e, (int)(dx * 40), (int)(dy * 40));
	});
	glfwSetCharCallback(window.getWindow(), [](GLFWwindow*, unsigned int cp) {
		if (!g_browser) return;
		CefKeyEvent e{};
		e.type = KEYEVENT_CHAR;
		e.character = e.unmodified_character = (char16_t)cp;
		e.windows_key_code = (int)cp;
		g_browser->GetHost()->SendKeyEvent(e);
	});
	glfwSetKeyCallback(window.getWindow(), [](GLFWwindow*, int key, int scancode, int action, int mods) {
		if (!g_browser || action == GLFW_RELEASE) return;
		int vk = 0;
		switch (key) {
		case GLFW_KEY_BACKSPACE:  vk = 0x08; break;
		case GLFW_KEY_TAB:        vk = 0x09; break;
		case GLFW_KEY_ENTER:      vk = 0x0D; break;
		case GLFW_KEY_ESCAPE:     vk = 0x1B; break;
		case GLFW_KEY_DELETE:     vk = 0x2E; break;
		case GLFW_KEY_LEFT:       vk = 0x25; break;
		case GLFW_KEY_RIGHT:      vk = 0x27; break;
		case GLFW_KEY_UP:         vk = 0x26; break;
		case GLFW_KEY_DOWN:       vk = 0x28; break;
		case GLFW_KEY_HOME:       vk = 0x24; break;
		case GLFW_KEY_END:        vk = 0x23; break;
		case GLFW_KEY_PAGE_UP:    vk = 0x21; break;
		case GLFW_KEY_PAGE_DOWN:  vk = 0x22; break;
		default: return;
		}
		CefKeyEvent e{};
		e.windows_key_code = vk;
		e.native_key_code = scancode;
		if (mods & GLFW_MOD_SHIFT)   e.modifiers |= EVENTFLAG_SHIFT_DOWN;
		if (mods & GLFW_MOD_CONTROL) e.modifiers |= EVENTFLAG_CONTROL_DOWN;
		if (mods & GLFW_MOD_ALT)     e.modifiers |= EVENTFLAG_ALT_DOWN;
		e.type = KEYEVENT_RAWKEYDOWN;
		g_browser->GetHost()->SendKeyEvent(e);
		if (vk == 0x08 || vk == 0x0D) {
			e.type = KEYEVENT_CHAR;
			e.character = e.unmodified_character = (char16_t)vk;
			g_browser->GetHost()->SendKeyEvent(e);
		}
	});
#endif

	while (!glfwWindowShouldClose(window.getWindow()))
	{
#ifndef _WIN32
		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);

		if (g_osr_dirty) {
			std::vector<uint8_t> px;
			int tw, th;
			{
				std::lock_guard<std::mutex> lk(g_osr_mutex);
				px = g_osr_pixels;
				tw = g_osr_w; th = g_osr_h;
				g_osr_dirty = false;
			}
			if (!px.empty()) {
				glBindTexture(GL_TEXTURE_2D, osr_tex);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_BGRA,
				             GL_UNSIGNED_BYTE, px.data());
			}
		}

		glUseProgram(osr_prog);
		glBindVertexArray(osr_vao);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, osr_tex);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		window.EndFrame();
#endif
		glfwPollEvents();
	}

	if (g_browser && g_browser->GetHost()) {
		g_browser->GetHost()->CloseBrowser(true);
	}

	CefShutdown();
	return 0;
}

