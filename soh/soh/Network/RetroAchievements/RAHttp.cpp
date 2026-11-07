#ifdef ENABLE_RETROACHIEVEMENTS

#include "RAHttp.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <curl/curl.h>
#include <rc_api_request.h>
#include <spdlog/spdlog.h>

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

void Request(const std::string& url, const std::string& postData, const std::string& contentType,
             Callback callback) {
    std::lock_guard lock(sMutex);
    if (!sWorkerStarted) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
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
