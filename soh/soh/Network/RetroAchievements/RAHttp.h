#pragma once

#ifdef __cplusplus

#include <functional>
#include <string>

// Minimal asynchronous HTTP client for rc_client's server_call callback.
//
// Requests run on a worker thread. Responses are queued and handed back on
// whichever thread calls DispatchCompleted(), which SoH does from the game
// thread so rc_client callbacks never run concurrently with game code.
namespace RAHttp {

struct Response {
    int statusCode = 0; // HTTP status, or a negative RC_API_SERVER_RESPONSE_* error
    std::string body;
};

using Callback = std::function<void(const Response&)>;

void SetUserAgent(const std::string& userAgent);

// POSTs `postData` to `url` (or GETs when postData is empty)
void Request(const std::string& url, const std::string& postData, const std::string& contentType, Callback callback);

// Runs callbacks for finished requests. Call from the game thread.
void DispatchCompleted();

} // namespace RAHttp

#endif
