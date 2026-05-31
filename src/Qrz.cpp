#include "Qrz.h"

#include <curl/curl.h>

#include <mutex>

namespace {

std::once_flag g_curlInit;

void ensureCurlGlobalInit() {
    std::call_once(g_curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string urlEscape(CURL* curl, const std::string& s) {
    char* e = curl_easy_escape(curl, s.c_str(), static_cast<int>(s.size()));
    std::string out = e ? e : std::string{};
    curl_free(e);
    return out;
}

// Replace the handful of XML entities QRZ may emit in text fields.
std::string xmlUnescape(std::string s) {
    struct { const char* from; char to; } ents[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
        {"&#39;", '\''},
    };
    for (const auto& e : ents) {
        const std::string from = e.from;
        for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;)
            s.replace(pos, from.size(), 1, e.to);
    }
    // &amp; last so it doesn't re-trigger the others.
    for (size_t pos = 0; (pos = s.find("&amp;", pos)) != std::string::npos; ++pos)
        s.replace(pos, 5, 1, '&');
    return s;
}

// Value of the first <tag>…</tag> in xml (QRZ's elements carry no attributes),
// or "" if absent. Tag names are matched case-sensitively, as in the XML.
std::string xmlTag(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const auto a = xml.find(open);
    if (a == std::string::npos)
        return {};
    const auto start = a + open.size();
    const auto b = xml.find(close, start);
    if (b == std::string::npos)
        return {};
    return xmlUnescape(xml.substr(start, b - start));
}

std::string httpGet(CURL* curl, const std::string& url, std::string& error) {
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "xlog2/0.1");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
        error = curl_easy_strerror(rc);
    return body;
}

constexpr const char* kBase = "https://xmldata.qrz.com/xml/current/";

} // namespace

QrzClient::QrzClient() {
    ensureCurlGlobalInit();
    dispatcher_.connect(sigc::mem_fun(*this, &QrzClient::onDispatch));
}

QrzClient::~QrzClient() {
    if (thread_.joinable())
        thread_.join();
}

bool QrzClient::lookup(const std::string& user, const std::string& password,
                       const std::string& callsign) {
    if (busy_.load())
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_    = QrzResult{};
        error_.clear();
        hasResult_ = false;
    }
    busy_.store(true);
    if (thread_.joinable())
        thread_.join();
    thread_ = std::thread(&QrzClient::worker, this, user, password, callsign);
    return true;
}

void QrzClient::worker(std::string user, std::string password, std::string callsign) {
    QrzResult result;
    std::string error;

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "failed to initialise libcurl";
    } else {
        // Log in (refresh the session key), returning false (and setting
        // `error`) on failure.
        auto login = [&]() -> bool {
            const std::string url = std::string(kBase) + "?username=" +
                urlEscape(curl, user) + "&password=" + urlEscape(curl, password) +
                "&agent=xlog2-0.1";
            std::string e;
            const std::string body = httpGet(curl, url, e);
            if (!e.empty()) { error = e; return false; }
            const std::string key = xmlTag(body, "Key");
            if (key.empty()) {
                const std::string err = xmlTag(body, "Error");
                error = err.empty() ? "QRZ login failed" : err;
                return false;
            }
            sessionKey_ = key;
            return true;
        };

        auto query = [&](std::string& e) -> std::string {
            const std::string url = std::string(kBase) + "?s=" +
                urlEscape(curl, sessionKey_) + "&callsign=" +
                urlEscape(curl, callsign);
            return httpGet(curl, url, e);
        };

        bool ok = true;
        if (sessionKey_.empty())
            ok = login();

        if (ok) {
            std::string e;
            std::string body = query(e);
            std::string err = e.empty() ? xmlTag(body, "Error") : e;

            // A stale key needs a fresh login and one retry.
            if (e.empty() && !err.empty() &&
                (err.find("ession") != std::string::npos ||  // "Session Timeout"
                 err.find("nvalid") != std::string::npos)) {  // "Invalid session key"
                sessionKey_.clear();
                if (login()) {
                    body = query(e);
                    err  = e.empty() ? xmlTag(body, "Error") : e;
                }
            }

            if (!err.empty()) {
                error = err;
            } else {
                result.call = xmlTag(body, "call");
                if (result.call.empty()) {
                    error = "No QRZ record for " + callsign;
                } else {
                    const std::string fn = xmlTag(body, "fname");
                    const std::string ln = xmlTag(body, "name");
                    result.name = fn.empty() ? ln
                                 : ln.empty() ? fn
                                              : fn + " " + ln;
                    const std::string city  = xmlTag(body, "addr2");
                    const std::string state = xmlTag(body, "state");
                    result.qth = city.empty() ? state
                                : state.empty() ? city
                                                : city + ", " + state;
                    result.locator = xmlTag(body, "grid");
                    result.country = xmlTag(body, "country");
                }
            }
        }
        curl_easy_cleanup(curl);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_    = std::move(result);
        error_     = std::move(error);
        hasResult_ = true;
    }
    dispatcher_.emit();
}

void QrzClient::onDispatch() {
    QrzResult result;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasResult_)
            return;
        result     = std::move(result_);
        error      = std::move(error_);
        hasResult_ = false;
    }
    if (thread_.joinable())
        thread_.join();
    busy_.store(false);
    if (onResult)
        onResult(result, error);
}
