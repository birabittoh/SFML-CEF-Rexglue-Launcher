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
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <thread>

#include "Utils/IsoExtraction.h"
#include "Networking/FileDownloader.h"
#include "../resource.h"

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
#endif

std::atomic<bool> is_cef_initialized(false);
std::atomic<bool> is_browser_closed(false);
CefRefPtr<CefBrowser> g_browser;
WNDPROC original_wnd_proc = nullptr;
HWND cef_window_handle = nullptr;
HWND g_mainWindowHandle = nullptr;
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
// Extracts a zip file to destPath using PowerShell's Expand-Archive cmdlet.
// Runs synchronously (blocks the calling thread until extraction completes).
// Returns true on success.
static bool ExtractZipWithPowerShell(const std::string& zipPath, const std::string& destPath) {
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

// Window procedure to handle resize messages
LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (uMsg == WM_SIZE && g_browser && g_browser->GetHost()) {
		RECT rect;
		GetClientRect(hwnd, &rect);
		InitConsole();
		std::cout << "Window resized: " << rect.right - rect.left << "x" << rect.bottom - rect.top << std::endl;
		// Get the CEF window handle and resize it
		if (cef_window_handle) {
			SetWindowPos(cef_window_handle, NULL, 0, 0, 
				rect.right - rect.left, rect.bottom - rect.top, 
				SWP_NOZORDER | SWP_NOACTIVATE);
		}

		g_browser->GetHost()->WasResized();
	}
	return CallWindowProc(original_wnd_proc, hwnd, uMsg, wParam, lParam);
}

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
		command_line->AppendSwitch("in-process-gpu");

		if (IsD3D11Supported()) {
			command_line->AppendSwitch("enable-gpu");
			command_line->AppendSwitchWithValue("use-angle", "d3d11");
			command_line->AppendSwitch("ignore-gpu-blocklist");
		} else {
			command_line->AppendSwitchWithValue("use-gl", "swiftshader");
		}
	}

	static bool IsD3D11Supported() {
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

		if (name == "SetLanguage") {
			if (arguments.size() < 1 || !arguments[0]->IsInt()) {
				std::cout << "SetLanguage: Invalid argument, expected integer" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}
			int language = arguments[0]->GetIntValue();
			// Save to registry
			HKEY hKey;
			if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\GoopieLauncher", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
				RegSetValueExA(hKey, "UserLanguage", 0, REG_DWORD, (const BYTE*)&language, sizeof(DWORD));
				RegCloseKey(hKey);
				std::cout << "Language set to: " << language << std::endl;
				retval = CefV8Value::CreateBool(true);
			} else {
				std::cout << "Failed to save language to registry" << std::endl;
				retval = CefV8Value::CreateBool(false);
			}
			return true;
		}

		if (name == "GetLanguage") {
			int language = 1; // Default to English
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
			retval = CefV8Value::CreateInt(language);
			std::cout << "GetLanguage: " << language << std::endl;
			return true;
		}

		if(name == "SetGamesPath") {
			//open a file dialog to set the folder where the games are stored, and save it to registry
			std::string folder = OpenGamesFolderDialog();
				if (!folder.empty()) {
					// Save to registry
					HKEY hKey;
					if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\GoopieLauncher", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
						RegSetValueExA(hKey, "GamesPath", 0, REG_SZ, (const BYTE*)folder.c_str(), static_cast<DWORD>(folder.size() + 1));
						RegCloseKey(hKey);
						std::cout << "Games path set to: " << folder << std::endl;
					} else {
						std::cout << "Failed to open registry key for writing" << std::endl;
						MessageBoxA(nullptr, "Failed to save games path to registry. Please try again.", "Goopie Launcher", MB_ICONERROR | MB_OK);
					}
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
			auto re = MessageBoxA(nullptr, arguments[0]->GetStringValue().ToString().c_str(),
				"Rexglue Launcher", MB_SYSTEMMODAL | MB_ICONQUESTION | MB_YESNOCANCEL);
			retval = CefV8Value::CreateString(re == IDYES ? "yes" : re == IDNO ? "no" : "cancel");
			std::cout << "testFunction called with argument: " << arguments[0]->GetStringValue().ToString() << std::endl;

			return true;
		}

		if (name == "OpenExternalLink") {
			std::string url = arguments[0]->GetStringValue().ToString();
			ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			std::cout << "Opening external link: " << url << std::endl;
			return true;
		}

		if (name == "isIsoInstalled") {
			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::filesystem::path xexPath = std::filesystem::path(GetGamesFolder()) / GameName / "assets" / "default.xex";
			bool installed = std::filesystem::exists(xexPath);
			retval = CefV8Value::CreateBool(installed);
			std::cout << "Checking if ISO is installed for game: " << GameName << " - " << (installed ? "Installed" : "Not Installed") << std::endl;
			return true;
		}

		if (name == "isExeUpdated") {
			std::string GameName = arguments[0]->GetStringValue().ToString();
			std::string exeFileName = GameName + "-windows-x64.exe";
			std::filesystem::path versionPath = std::filesystem::path(GetGamesFolder()) / GameName / exeFileName;
			bool installed = std::filesystem::exists(versionPath);
			retval = CefV8Value::CreateBool(installed); //Checks if the exe is the latest version
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
			if (!std::filesystem::exists(sidecarPath)) {
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
			if (std::filesystem::exists(GamePath)) {
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

			// Optional asset name (e.g. retip-windows-x64-release.exe). Defaults to
			// the legacy `<GameName>-windows-x64.exe` for backwards compatibility.
			std::string assetName;
			if (arguments.size() >= 3 && arguments[2]->IsString()) {
				assetName = arguments[2]->GetStringValue().ToString();
			}
			if (assetName.empty()) {
				assetName = GameName + "-windows-x64.exe";
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

			// Construct the download URL from the GitHub releases URL. We always save
			// to the canonical `<GameName>-windows-x64.exe` so `Play` keeps working
			// regardless of which build flavour was selected.
			std::string canonicalExeName = GameName + "-windows-x64.exe";
			std::string downloadUrl = GameGit + assetName;
			std::filesystem::path gameDir = std::filesystem::path(GetGamesFolder()) / GameName;
			std::string localPath = (gameDir / canonicalExeName).string();

			std::cout << "Download URL: " << downloadUrl << std::endl;
			std::cout << "Local path: " << localPath << std::endl;

			// Construct toml file paths
			std::string tomlFileName = GameName + ".toml";
			std::string tomlDownloadUrl = GameGit + tomlFileName;
			std::string tomlLocalPath = (gameDir / tomlFileName).string();

			// Sidecar describing what is installed; written after a successful download.
			std::string sidecarPath = (gameDir / ".installed.json").string();
			std::string packagesSidecarPath = (gameDir / ".installed_packages.json").string();

			// Create the directory if it doesn't exist
			if (!std::filesystem::exists(gameDir)) {
				std::filesystem::create_directories(gameDir);
			}

			// Download from GitHub releases
			std::thread([downloadUrl, localPath, tomlDownloadUrl, tomlLocalPath, sidecarPath, packagesSidecarPath,
			             assetName, versionTag, packages, gameDir, GameGit]() {
				try {
					staticprogress = 0;
					Networking::FileDownloader downloader;
					auto exeResult = downloader.downloadFile(downloadUrl, localPath, downloadProgressCallback);

					// Try to download the toml file if it exists in the release
					std::cout << "Checking for toml file: " << tomlDownloadUrl << std::endl;
					Networking::FileDownloader tomlDownloader;
					auto tomlResult = tomlDownloader.downloadFile(tomlDownloadUrl, tomlLocalPath, nullptr);
					if (tomlResult == Networking::FileDownloader::Result::SUCCESS) {
						std::cout << "Successfully downloaded toml file: " << tomlLocalPath << std::endl;
					} else {
						std::cout << "No toml file found in release (this is optional)" << std::endl;
					}

					// On a successful exe download, persist what we just installed so
					// the UI can show the version + build label without re-hashing.
					if (exeResult == Networking::FileDownloader::Result::SUCCESS) {
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
						std::ofstream sidecar(sidecarPath, std::ios::binary | std::ios::trunc);
						if (sidecar) {
							sidecar << "{\"version\":\"" << jsonEscape(versionTag)
								<< "\",\"asset\":\"" << jsonEscape(assetName) << "\"}";
							std::cout << "Wrote installed sidecar: " << sidecarPath << std::endl;
						} else {
							std::cout << "Warning: failed to write sidecar at " << sidecarPath << std::endl;
						}
					}

					// Download and extract each zip package
					for (const auto& pkg : packages) {
						std::cout << "Downloading package: " << pkg.assetName << std::endl;
						staticprogress = 0;
						std::string pkgDownloadUrl = GameGit + pkg.assetName;
						std::string pkgLocalZip = (gameDir / pkg.assetName).string();

						Networking::FileDownloader pkgDownloader;
						auto pkgResult = pkgDownloader.downloadFile(pkgDownloadUrl, pkgLocalZip, downloadProgressCallback);

						if (pkgResult == Networking::FileDownloader::Result::SUCCESS) {
							std::cout << "Extracting package: " << pkg.assetName << " -> " << gameDir.string() << std::endl;
							bool extracted = ExtractZipWithPowerShell(pkgLocalZip, gameDir.string());
							// Remove the zip regardless of extraction outcome
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
				assetName = GameName + "-windows-x64.exe";
			}
			std::cout << "NeedsUpdate: GameName: " << GameName
				<< ", GitHubApiUrl: " << GitHubApiUrl
				<< ", AssetName: " << assetName << std::endl;

			// The exe is always saved canonically as `<GameName>-windows-x64.exe`.
			std::string canonicalExeName = GameName + "-windows-x64.exe";
			std::filesystem::path localExePath = std::filesystem::path(GetGamesFolder()) / GameName / canonicalExeName;

			// If local exe doesn't exist, needs update
			if (!std::filesystem::exists(localExePath)) {
				std::cout << "NeedsUpdate: Local exe not found, needs update" << std::endl;
				retval = CefV8Value::CreateBool(true);
				return true;
			}

			// Calculate local file SHA256
			std::string localSha = Networking::FileDownloader::calculateFileSHA256(localExePath.string());
			if (localSha.empty()) {
				std::cout << "NeedsUpdate: Failed to calculate local SHA, assuming needs update" << std::endl;
				retval = CefV8Value::CreateBool(true);
				return true;
			}
			std::cout << "NeedsUpdate: Local SHA256: " << localSha << std::endl;

			// Fetch release info from GitHub API
			Networking::FileDownloader downloader;
			std::string responseBody;
			auto result = downloader.fetchToString(GitHubApiUrl, responseBody);

			if (result != Networking::FileDownloader::Result::SUCCESS) {
				std::cout << "NeedsUpdate: Failed to fetch GitHub API: " << downloader.getLastError() << std::endl;
				retval = CefV8Value::CreateBool(true); // Assume needs update if we can't check
				return true;
			}

			// Parse the response to find the sha for the exe asset
				// GitHub API returns JSON with assets array, each asset has a "name" and "digest" field
				// The digest field contains the SHA256 hash in format "sha256:HASH"

				// Find the asset with matching exe name and extract its digest
				std::string remoteSha;
				size_t assetsPos = responseBody.find("\"assets\"");
				if (assetsPos != std::string::npos) {
					// Look for the exe file name in assets
					std::string nameSearch = "\"name\":\"" + assetName + "\"";
					size_t exeAssetPos = responseBody.find(nameSearch);
					if (exeAssetPos == std::string::npos) {
						// Try with space after colon
						nameSearch = "\"name\": \"" + assetName + "\"";
						exeAssetPos = responseBody.find(nameSearch);
					}

					if (exeAssetPos != std::string::npos) {
						std::cout << "NeedsUpdate: Found exe asset in response" << std::endl;

						// Find the digest field for this asset (search forward from the name)
						std::string digestSearch = "\"digest\":";
						size_t digestPos = responseBody.find(digestSearch, exeAssetPos);
						if (digestPos == std::string::npos) {
							digestSearch = "\"digest\": ";
							digestPos = responseBody.find(digestSearch, exeAssetPos);
						}

						if (digestPos != std::string::npos) {
							// Extract the digest value (format: "sha256:HASH")
							size_t valueStart = responseBody.find("\"", digestPos + digestSearch.length()) + 1;
							size_t valueEnd = responseBody.find("\"", valueStart);
							if (valueStart != std::string::npos && valueEnd != std::string::npos) {
								std::string digestValue = responseBody.substr(valueStart, valueEnd - valueStart);
								std::cout << "NeedsUpdate: Found digest: " << digestValue << std::endl;

								// Extract hash from "sha256:HASH" format
								size_t colonPos = digestValue.find(':');
								if (colonPos != std::string::npos) {
									remoteSha = digestValue.substr(colonPos + 1);
								} else {
									remoteSha = digestValue; // Assume it's just the hash
								}
							}
						}
					}
				}

				if (!remoteSha.empty()) {
					// Convert to lowercase for comparison
					std::transform(localSha.begin(), localSha.end(), localSha.begin(), ::tolower);
					std::transform(remoteSha.begin(), remoteSha.end(), remoteSha.begin(), ::tolower);

					bool needsUpdate = (localSha != remoteSha);
					std::cout << "NeedsUpdate: Remote SHA256: " << remoteSha << std::endl;
					std::cout << "NeedsUpdate: " << (needsUpdate ? "Update needed" : "Up to date") << std::endl;
					retval = CefV8Value::CreateBool(needsUpdate);
					return true;
				}

				// If we couldn't find the digest, assume needs update
				std::cout << "NeedsUpdate: Could not find digest in response, assuming needs update" << std::endl;
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

			std::filesystem::path exePath;
			if (!customExePath.empty()) {
				// Normalize forward slashes to backslashes for Windows
				std::replace(customExePath.begin(), customExePath.end(), '/', '\\');
				exePath = std::filesystem::path(GetGamesFolder()) / GameName / customExePath;
			} else {
				std::string exeFileName = GameName + "-windows-x64.exe";
				exePath = std::filesystem::path(GetGamesFolder()) / GameName / exeFileName;
			}
			if (std::filesystem::exists(exePath)) {
				std::string patches = "";
				std::string launchDir = (std::filesystem::path(GetGamesFolder()) / GameName).string();
				std::string exeFileName = exePath.filename().string();

				int userLanguage = 1;
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
				std::string launchArgs = "--user_language=" + std::to_string(userLanguage);

				// NEW: append cvar args
				if (!cvarArgs.empty()) {
					launchArgs += " " + cvarArgs;
				}

				std::string fullExePath = exePath.string();
				std::thread([patches, fullExePath, exeFileName, launchDir, launchArgs]() {
					std::string command = "\"" + fullExePath + "\"" + patches;
					if (!launchArgs.empty()) {
						command += " " + launchArgs;
					}

					// CreateProcess requires a writable command-line buffer.
					std::vector<char> cmdBuffer(command.begin(), command.end());
					cmdBuffer.push_back('\0');

					STARTUPINFOA si = {};
					si.cb = sizeof(si);
					PROCESS_INFORMATION pi = {};

					BOOL ok = CreateProcessA(
						fullExePath.c_str(), // application name (absolute path)
						cmdBuffer.data(),    // mutable command line
						nullptr,             // process security
						nullptr,             // thread security
						FALSE,               // inherit handles
						0,                   // creation flags
						nullptr,             // environment
						launchDir.c_str(),   // working directory
						&si,
						&pi);

					if (!ok) {
						DWORD err = GetLastError();
						std::cerr << "Play: CreateProcess failed (" << err
							<< ") for: " << command
							<< " in dir: " << launchDir << std::endl;
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
				}).detach();
			}
			else {
				std::cerr << "Play: Executable not found: " << exePath.string() << std::endl;
				std::string msg = "Game executable not found:\n" + exePath.string();
				MessageBoxA(nullptr, msg.c_str(), "Goopie Launcher", MB_ICONERROR | MB_OK);
			}
			return true;
		}

		if (name == "getVersion") {
			retval = CefV8Value::CreateInt(9); // Returns the current version of the Launcher exe
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

			if (!std::filesystem::exists(gameDir)) {
				std::filesystem::create_directories(gameDir);
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
						bool extracted = ExtractZipWithPowerShell(zipLocalPath, gameDir.string());
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

			if (!std::filesystem::exists(sidecarPath)) {
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
			if (std::filesystem::exists(savesPath) && std::filesystem::is_directory(savesPath)) {
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
			if (std::filesystem::exists(savesPath) && std::filesystem::is_directory(savesPath)) {
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
			if (std::filesystem::exists(activeSavePath)) {
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

			// Get Documents folder path
			char documentsPath[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath))) {
				std::cout << "backupSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}

			std::filesystem::path sourcePath = std::filesystem::path(documentsPath) / recompName;
			std::filesystem::path destPath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / saveName;

			try {
				if (!std::filesystem::exists(sourcePath)) {
					std::cout << "backupSave: Source path does not exist: " << sourcePath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				// Create destination directory
				std::filesystem::create_directories(destPath);

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

			// Get Documents folder path
			char documentsPath[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath))) {
				std::cout << "restoreSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}

			std::filesystem::path sourcePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / saveName;
			std::filesystem::path destPath = std::filesystem::path(documentsPath) / recompName;

			try {
				if (!std::filesystem::exists(sourcePath)) {
					std::cout << "restoreSave: Source save does not exist: " << sourcePath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				// Remove existing destination and recreate
				if (std::filesystem::exists(destPath)) {
					std::filesystem::remove_all(destPath);
				}
				std::filesystem::create_directories(destPath);

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
				if (!std::filesystem::exists(savePath)) {
					std::cout << "deleteSave: Save does not exist: " << savePath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				std::filesystem::remove_all(savePath);

				// Clear active save marker if this was the active save
				std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";
				if (std::filesystem::exists(activeSavePath)) {
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
				if (!std::filesystem::exists(oldPath)) {
					std::cout << "renameSave: Source save does not exist: " << oldPath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				if (std::filesystem::exists(newPath)) {
					std::cout << "renameSave: Destination already exists: " << newPath << std::endl;
					retval = CefV8Value::CreateBool(false);
					return true;
				}

				std::filesystem::rename(oldPath, newPath);

				// Update active save marker if this was the active save
				std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";
				if (std::filesystem::exists(activeSavePath)) {
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


			// Create the folder if it doesn't exist
			if (!std::filesystem::exists(gamesFolder)) {
				std::filesystem::create_directories(gamesFolder);
			}

			// Open in Windows Explorer
			ShellExecuteA(nullptr, "open", gamesFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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

			// Get Documents folder path
			char documentsPath[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath))) {
				std::cout << "openSaveFolder: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}

			std::filesystem::path saveFolderPath = std::filesystem::path(documentsPath) / recompName;

			// Create the folder if it doesn't exist
			if (!std::filesystem::exists(saveFolderPath)) {
				std::filesystem::create_directories(saveFolderPath);
			}

			// Open in Windows Explorer
			ShellExecuteA(nullptr, "open", saveFolderPath.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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

			// Get Documents folder path
			char documentsPath[MAX_PATH];
			if (FAILED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, documentsPath))) {
				std::cout << "deleteCurrentSave: Failed to get Documents path" << std::endl;
				retval = CefV8Value::CreateBool(false);
				return true;
			}

			std::filesystem::path saveFolderPath = std::filesystem::path(documentsPath) / recompName;

			try {
				if (!std::filesystem::exists(saveFolderPath)) {
					std::cout << "deleteCurrentSave: Save folder does not exist: " << saveFolderPath << std::endl;
					// Return true since the goal (no save data) is achieved
					retval = CefV8Value::CreateBool(true);
					return true;
				}

				// Delete the entire save folder
				std::filesystem::remove_all(saveFolderPath);

				// Clear active save marker
				std::filesystem::path activeSavePath = std::filesystem::path(GetGamesFolder()) / recompName / "saves" / ".active";
				if (std::filesystem::exists(activeSavePath)) {
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

class RexClient : public CefClient, public CefLifeSpanHandler {
	IMPLEMENT_REFCOUNTING(RexClient);

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
		cef_window_handle = nullptr;
		is_browser_closed = true;
	}
};

CefRefPtr<RexClient> client;

// Check if localhost:5173 is available
bool IsLocalhostAvailable(int port, int timeoutMs = 500) {
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		return false;
	}

	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET) {
		WSACleanup();
		return false;
	}

	// Set socket to non-blocking mode
	u_long mode = 1;
	ioctlsocket(sock, FIONBIO, &mode);

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	connect(sock, (sockaddr*)&addr, sizeof(addr));

	// Use select to wait for connection with timeout
	fd_set writeSet;
	FD_ZERO(&writeSet);
	FD_SET(sock, &writeSet);

	timeval timeout;
	timeout.tv_sec = timeoutMs / 1000;
	timeout.tv_usec = (timeoutMs % 1000) * 1000;

	bool available = select(0, nullptr, &writeSet, nullptr, &timeout) > 0;

	closesocket(sock);
	WSACleanup();
	return available;
}

void window_resize_callback(GLFWwindow* window, int width, int height) {
	if (g_browser && g_browser->GetHost() && cef_window_handle) {
		// Resize the CEF window to match the new GLFW window size
		RECT rect{};
		GetClientRect(glfwGetWin32Window(window), &rect);

		SetWindowPos(cef_window_handle, NULL, 0, 0, 
			rect.right - rect.left, rect.bottom - rect.top, 
			SWP_NOZORDER | SWP_NOACTIVATE);

		// Notify CEF that a move or resize operation has started
		g_browser->GetHost()->NotifyMoveOrResizeStarted();

		// Notify CEF that the window was resized
		g_browser->GetHost()->WasResized();
	}
}


int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	// Check for --console launch argument
	std::string cmdLine(lpCmdLine);
	if (cmdLine.find("--console") != std::string::npos) {
		g_consoleEnabled = true;
		InitConsole();
		IsoExtraction::EnableConsoleLogging();
	}

	const CefMainArgs main_args(hInstance);
	CefRefPtr<RexApp> app(new RexApp);
	if (const auto code = CefExecuteProcess(main_args, app.get(), nullptr); code >= 0) {
		return code;
	}
	CefSettings settings;
	settings.multi_threaded_message_loop = true;
	//CefString(&settings.cache_path).FromString((std::filesystem::current_path() / "cef_cache").string());
	CefInitialize(main_args, settings, app.get(), nullptr);

	//sleep until the cef is ready
	while (!is_cef_initialized.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	VinceWindow window(1280, 720, "Vince Engine");

	//SendMessage to set icon
	SendMessage(glfwGetWin32Window(window.getWindow()), WM_SETICON, ICON_SMALL, (LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1)));
	SendMessage(glfwGetWin32Window(window.getWindow()), WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1)));

	// Set up custom window procedure to handle resize messages
	HWND hwnd = glfwGetWin32Window(window.getWindow());
	original_wnd_proc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc);

	// Store the main window handle for dialog parenting
	g_mainWindowHandle = hwnd;
	IsoExtraction::SetMainWindowHandle(hwnd);

	const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
	glfwWindowHint(GLFW_RED_BITS, mode->redBits);
	glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
	glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
	glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);

	// Set up the resize callback
	glfwSetWindowSizeCallback(window.getWindow(), window_resize_callback);
	window_resize_callback(window.getWindow(), mode->width / 1.5, mode->height / 1.5); // Initial resize to set CEF window size

	// Initialize the client
	client = new RexClient();

	RECT rect{};
	GetClientRect(glfwGetWin32Window(window.getWindow()), &rect);
	CefRect cef_rect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);

	CefWindowInfo window_info;
	window_info.SetAsChild(glfwGetWin32Window(window.getWindow()), cef_rect);

	// Try localhost first for development, fall back to production URL
	std::string browserUrl = "https://goopie.xyz";
	if (IsLocalhostAvailable(5173)) {
		browserUrl = "http://localhost:5173/";
		std::cout << "Using development server at localhost:5173" << std::endl;
	} else {
		std::cout << "Using production server at goopie.xyz" << std::endl;
	}
	CefBrowserHost::CreateBrowser(window_info, client.get(), "https://goopie.xyz", CefBrowserSettings(), nullptr, nullptr);
	//CefBrowserHost::CreateBrowser(window_info, client.get(), "http://localhost:5173/", CefBrowserSettings(), nullptr, nullptr);

	while (!glfwWindowShouldClose(window.getWindow()))
	{
		glfwPollEvents();
	}

	if (g_browser && g_browser->GetHost()) {
		g_browser->GetHost()->CloseBrowser(true);
	}

	CefShutdown();
	return 1;
}

