#include "FileDownloader.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>

#include <curl/curl.h>

#include <thread>
#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "libcurl.lib")
#pragma comment(lib, "Wldap32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Ws2_32.lib")
#else
#include "../Utils/Crypto/sha256.h"
#endif

namespace Networking {

struct FileDownloader::Impl {
    CURL* curl;
    FILE* file;
    FileDownloader::ProgressCallback progressCallback;
    FileDownloader* downloader;

    Impl(FileDownloader* owner) : curl(nullptr), file(nullptr), downloader(owner) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
    }

    ~Impl() {
        if (curl) {
            curl_easy_cleanup(curl);
        }
        if (file) {
            fclose(file);
        }
        curl_global_cleanup();
    }
};

// Callback function to write data to file
static size_t writeData(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

// Callback function for progress updates
static int progressCallbackFunc(void* clientp, curl_off_t dltotal, curl_off_t dlnow, 
                               curl_off_t ultotal, curl_off_t ulnow) {
    auto* downloader = static_cast<FileDownloader*>(clientp);

    if (downloader->isCancelled()) {
		staticprogress = -1; // Indicate cancellation in progress
        return 1; // Return non-zero to abort the transfer
    }

    if (dltotal > 0) {
        downloader->callProgressCallback(static_cast<long long>(dlnow), static_cast<long long>(dltotal));
    }

    return 0;
}

FileDownloader::FileDownloader() : m_impl(std::make_unique<Impl>(this)), m_cancelled(false) {
}

FileDownloader::~FileDownloader() = default;

void FileDownloader::callProgressCallback(long long downloaded, long long total) const {
    if (m_currentProgressCallback) {
		staticprogress = static_cast<int>((downloaded * 100) / total);
        m_currentProgressCallback(downloaded, total);
    }
}

FileDownloader::Result FileDownloader::downloadFile(const std::string& url, 
                                                   const std::string& localPath,
                                                   ProgressCallback progressCallback) {
    if (!m_impl->curl) {
        m_lastError = "Failed to initialize curl";
        std::cout << m_lastError << std::endl;
        return Result::NETWORK_ERROR;
    }

    m_cancelled = false;
    m_currentProgressCallback = progressCallback;

    // Open the file for writing
    m_impl->file = fopen(localPath.c_str(), "wb");
    if (!m_impl->file) {
        m_lastError = "Failed to open file for writing: " + localPath;
        std::cout << m_lastError << std::endl;
        return Result::FILE_ERROR;
    }

    // Setup curl options
    curl_easy_setopt(m_impl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEFUNCTION, writeData);
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEDATA, m_impl->file);
    curl_easy_setopt(m_impl->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(m_impl->curl, CURLOPT_TIMEOUT, 300L); // 5 minute timeout
    curl_easy_setopt(m_impl->curl, CURLOPT_CONNECTTIMEOUT, 30L); // 30 second connect timeout
    
    // Setup progress callback
    if (progressCallback) {
        curl_easy_setopt(m_impl->curl, CURLOPT_XFERINFOFUNCTION, progressCallbackFunc);
        curl_easy_setopt(m_impl->curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(m_impl->curl, CURLOPT_NOPROGRESS, 0L);
    }

    // Perform the request
    CURLcode res = curl_easy_perform(m_impl->curl);
    
    // Close the file
    fclose(m_impl->file);
    m_impl->file = nullptr;

    if (m_cancelled) {
        // Delete partially downloaded file
        std::remove(localPath.c_str());
        m_lastError = "Download was cancelled";
        staticprogress = -1; // Indicate cancellation in progress
        std::cout << m_lastError << std::endl;
        return Result::CANCELLED;
    }

    if (res != CURLE_OK) {
        // Delete partially downloaded file
        std::remove(localPath.c_str());
        m_lastError = "Download failed: " + std::string(curl_easy_strerror(res));
        staticprogress = -1; // Indicate cancellation in progress
        std::cout << m_lastError << std::endl;
        return Result::NETWORK_ERROR;
    }

    // Check HTTP response code
    long response_code;
    curl_easy_getinfo(m_impl->curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        std::remove(localPath.c_str());
        std::ostringstream oss;
        oss << "HTTP error: " << response_code;
        staticprogress = -1; // Indicate cancellation in progress
        m_lastError = oss.str();
		std::cout << m_lastError << std::endl;
        return Result::NETWORK_ERROR;
    }
	staticprogress = -1; // Indicate download complete
    return Result::SUCCESS;
}

FileDownloader::Result FileDownloader::downloadFromGameServer(const std::string& filePath, 
                                                            const std::string& localPath,
                                                            ProgressCallback progressCallback) {
    std::string url = "http://64.225.52.239/downloads/" + filePath;
	std::cout << "Starting download from game server: " << url << std::endl;
    return downloadFile(url, localPath, progressCallback);
}

void FileDownloader::downloadFromGameServerAsync(const std::string& filePath, const std::string& localPath,
                                                ProgressCallback progressCallback,
                                                CompletionCallback completionCallback) {
	std::cout << "Starting async download from game server: " << filePath << std::endl;
    // Launch the download in a new thread
    std::thread([=, this]() {
        Result result = downloadFromGameServer(filePath, localPath, progressCallback);
        if (completionCallback) {
            completionCallback(result);
        }
    }).detach();
}

void FileDownloader::cancelDownload() {
    staticprogress = -1; // Indicate cancellation in progress
    m_cancelled = true;
}

// Callback function to write data to string
static size_t writeToString(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

FileDownloader::Result FileDownloader::fetchToString(const std::string& url, std::string& outContent) {
    if (!m_impl->curl) {
        m_lastError = "Failed to initialize curl";
        return Result::NETWORK_ERROR;
    }

    outContent.clear();

    curl_easy_setopt(m_impl->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(m_impl->curl, CURLOPT_WRITEDATA, &outContent);
    curl_easy_setopt(m_impl->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(m_impl->curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(m_impl->curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(m_impl->curl, CURLOPT_USERAGENT, "URGL-Launcher/1.0");
    curl_easy_setopt(m_impl->curl, CURLOPT_NOPROGRESS, 1L);

    CURLcode res = curl_easy_perform(m_impl->curl);

    if (res != CURLE_OK) {
        m_lastError = "Fetch failed: " + std::string(curl_easy_strerror(res));
        return Result::NETWORK_ERROR;
    }

    long response_code;
    curl_easy_getinfo(m_impl->curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        std::ostringstream oss;
        oss << "HTTP error: " << response_code;
        m_lastError = oss.str();
        return Result::NETWORK_ERROR;
    }

    return Result::SUCCESS;
}

std::string FileDownloader::calculateFileSHA256(const std::string& filePath) {
#ifdef _WIN32
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::string result;

    if (FAILED(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return "";
    }

    DWORD hashObjSize = 0, dataSize = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&hashObjSize, sizeof(DWORD), &dataSize, 0);

    std::vector<BYTE> hashObj(hashObjSize);
    if (FAILED(BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    const size_t bufferSize = 65536;
    std::vector<char> buffer(bufferSize);

    while (file.read(buffer.data(), bufferSize) || file.gcount() > 0) {
        BCryptHashData(hHash, (PBYTE)buffer.data(), static_cast<ULONG>(file.gcount()), 0);
    }

    DWORD hashSize = 32; // SHA256 is 32 bytes
    std::vector<BYTE> hash(hashSize);
    BCryptFinishHash(hHash, hash.data(), hashSize, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::ostringstream oss;
    for (BYTE b : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
#else
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return "";

    SHA256_CTX1 ctx;
    sha256_init(&ctx);

    const size_t bufferSize = 65536;
    std::vector<char> buffer(bufferSize);
    while (file.read(buffer.data(), bufferSize) || file.gcount() > 0) {
        sha256_update(&ctx,
            reinterpret_cast<const unsigned char*>(buffer.data()),
            static_cast<size_t>(file.gcount()));
    }

    unsigned char hash[SHA256_BLOCK_SIZE];
    sha256_final(&ctx, hash);

    std::ostringstream oss;
    for (unsigned char b : hash)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    return oss.str();
#endif
}

} // namespace Networking