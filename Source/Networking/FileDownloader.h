#pragma once

#include <string>
#include <functional>
#include <memory>

inline int staticprogress = -1; //0 -> 100

namespace Networking {

class FileDownloader {
public:
    // Progress callback: (downloaded_bytes, total_bytes) -> void
    using ProgressCallback = std::function<void(long long, long long)>;

    enum class Result {
        SUCCESS,
        NETWORK_ERROR,
        FILE_ERROR,
        CANCELLED
    };

    // Completion callback: (Result) -> void
    using CompletionCallback = std::function<void(Result)>;

    FileDownloader();
    ~FileDownloader();

    // Download a file from the specified URL to the local path
    Result downloadFile(const std::string& url, const std::string& localPath, 
                       ProgressCallback progressCallback = nullptr);

    // Download from the game server: http://64.225.52.239/downloads/[FilePath]
    Result downloadFromGameServer(const std::string& filePath, const std::string& localPath,
                                 ProgressCallback progressCallback = nullptr);

    // Async version: launches download in a new thread
    void downloadFromGameServerAsync(const std::string& filePath, const std::string& localPath,
                                     ProgressCallback progressCallback = nullptr,
                                     CompletionCallback completionCallback = nullptr);

    // Cancel ongoing download
    void cancelDownload();

    // Get last error message
    const std::string& getLastError() const { return m_lastError; }

    // Check if download is cancelled (used by callback)
    bool isCancelled() const { return m_cancelled; }

    // Fetch content from URL to string (for API calls)
    Result fetchToString(const std::string& url, std::string& outContent);

    // Calculate SHA256 hash of a file
    static std::string calculateFileSHA256(const std::string& filePath);

    // Call progress callback if set (used by callback)
    void callProgressCallback(long long downloaded, long long total) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::string m_lastError;
    bool m_cancelled;
    ProgressCallback m_currentProgressCallback;
};

} // namespace Networking