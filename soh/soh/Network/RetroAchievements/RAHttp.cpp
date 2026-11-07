#ifdef ENABLE_RETROACHIEVEMENTS

#include "RAHttp.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <rc_api_request.h>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace {

struct Job {
    std::string url;
    std::string postData;
    std::string contentType;
    RAHttp::Callback callback;
    RAHttp::Response response;
};

std::mutex sMutex;
std::condition_variable sWake;
std::deque<Job> sPending;
std::deque<Job> sCompleted;
std::string sUserAgent = "ShipOfHarkinian";
bool sWorkerStarted = false;

#ifdef _WIN32

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// Closes a WinHTTP handle on every return path
struct HInternet {
    HINTERNET handle = nullptr;
    explicit HInternet(HINTERNET h) : handle(h) {
    }
    ~HInternet() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    operator HINTERNET() const {
        return handle;
    }
};

// Only the worker thread touches the session, so it needs no lock
HINTERNET sSession = nullptr;

void Fail(Job& job) {
    DWORD error = GetLastError();
    SPDLOG_WARN("[RetroAchievements] HTTP request failed: WinHTTP error {}", error);
    job.response.body.clear();
    // rc_client retries these, which is what we want for a dropped connection
    job.response.statusCode = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
}

void Perform(Job& job, const std::string& userAgent) {
    std::wstring userAgentW = Widen(userAgent);

    if (sSession == nullptr) {
        sSession = WinHttpOpen(userAgentW.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (sSession == nullptr) {
            // AUTOMATIC_PROXY needs Windows 8.1
            sSession = WinHttpOpen(userAgentW.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (sSession == nullptr) {
            Fail(job);
            return;
        }
    }

    std::wstring urlW = Widen(job.url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(urlW.c_str(), static_cast<DWORD>(urlW.size()), 0, &parts)) {
        Fail(job);
        return;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    HInternet connect(WinHttpConnect(sSession, host.c_str(), parts.nPort, 0));
    if (connect == nullptr) {
        Fail(job);
        return;
    }

    bool isPost = !job.postData.empty();
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HInternet request(WinHttpOpenRequest(connect, isPost ? L"POST" : L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (request == nullptr) {
        Fail(job);
        return;
    }

    // Same limits as the curl backend: 10 s to connect, 30 s for the transfer
    if (!WinHttpSetTimeouts(request, 10000, 10000, 30000, 30000)) {
        Fail(job);
        return;
    }

    // The user agent can change between requests; the session keeps the first one
    std::wstring userAgentHeader = L"User-Agent: " + userAgentW + L"\r\n";
    if (!WinHttpAddRequestHeaders(request, userAgentHeader.c_str(), static_cast<DWORD>(-1L),
                                  WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD)) {
        Fail(job);
        return;
    }
    if (!job.contentType.empty()) {
        std::wstring contentTypeHeader = L"Content-Type: " + Widen(job.contentType) + L"\r\n";
        if (!WinHttpAddRequestHeaders(request, contentTypeHeader.c_str(), static_cast<DWORD>(-1L),
                                      WINHTTP_ADDREQ_FLAG_REPLACE | WINHTTP_ADDREQ_FLAG_ADD)) {
            Fail(job);
            return;
        }
    }

    // WinHTTP follows redirects by default, like CURLOPT_FOLLOWLOCATION
    LPVOID body = isPost ? const_cast<char*>(job.postData.data()) : WINHTTP_NO_REQUEST_DATA;
    DWORD bodyLength = static_cast<DWORD>(job.postData.size());
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, bodyLength, bodyLength, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        Fail(job);
        return;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        Fail(job);
        return;
    }

    // Badge PNGs are binary, so append by length rather than as C strings
    job.response.body.clear();
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            Fail(job);
            return;
        }
        if (available == 0) {
            break;
        }
        size_t offset = job.response.body.size();
        job.response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, job.response.body.data() + offset, available, &read)) {
            Fail(job);
            return;
        }
        job.response.body.resize(offset + read);
        if (read == 0) {
            break;
        }
    }

    job.response.statusCode = static_cast<int>(status);
}

#else

size_t WriteBody(char* data, size_t size, size_t count, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * count);
    return size * count;
}

void Perform(Job& job, const std::string& userAgent) {
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        job.response.statusCode = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
        return;
    }

    struct curl_slist* headers = nullptr;
    if (!job.contentType.empty()) {
        headers = curl_slist_append(headers, ("Content-Type: " + job.contentType).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &job.response.body);
    if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (!job.postData.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job.postData.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(job.postData.size()));
    }

    CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        job.response.statusCode = static_cast<int>(status);
    } else {
        SPDLOG_WARN("[RetroAchievements] HTTP request failed: {}", curl_easy_strerror(result));
        job.response.body.clear();
        // rc_client retries these, which is what we want for a dropped connection
        job.response.statusCode = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

#endif // _WIN32

void WorkerLoop() {
    for (;;) {
        Job job;
        std::string userAgent;
        {
            std::unique_lock lock(sMutex);
            sWake.wait(lock, [] { return !sPending.empty(); });
            job = std::move(sPending.front());
            sPending.pop_front();
            userAgent = sUserAgent;
        }

        Perform(job, userAgent);

        std::lock_guard lock(sMutex);
        sCompleted.push_back(std::move(job));
    }
}

} // namespace

namespace RAHttp {

void SetUserAgent(const std::string& userAgent) {
    std::lock_guard lock(sMutex);
    sUserAgent = userAgent;
}

void Request(const std::string& url, const std::string& postData, const std::string& contentType, Callback callback) {
    std::lock_guard lock(sMutex);
    if (!sWorkerStarted) {
#ifndef _WIN32
        curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
        // Detached: the process may exit with a request in flight, and joining
        // would hold shutdown for up to the request timeout
        std::thread(WorkerLoop).detach();
        sWorkerStarted = true;
    }
    sPending.push_back({ url, postData, contentType, std::move(callback), {} });
    sWake.notify_one();
}

void DispatchCompleted() {
    std::deque<Job> done;
    {
        std::lock_guard lock(sMutex);
        done.swap(sCompleted);
    }
    for (auto& job : done) {
        job.callback(job.response);
    }
}

} // namespace RAHttp

#endif // ENABLE_RETROACHIEVEMENTS
