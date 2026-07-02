// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

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

// Extract every flat <tag>value</tag> element from `block`, in order. QRZ's
// callsign fields carry no attributes and don't nest, so a simple scan works.
std::vector<std::pair<std::string, std::string>> parseElements(const std::string& block) {
    std::vector<std::pair<std::string, std::string>> out;
    const size_t n = block.size();
    size_t i = 0;
    while (i < n) {
        if (block[i] != '<') { ++i; continue; }
        const size_t gt = block.find('>', i);
        if (gt == std::string::npos)
            break;
        const std::string tag = block.substr(i + 1, gt - i - 1);
        i = gt + 1;
        if (tag.empty() || tag[0] == '/' || tag[0] == '!' || tag[0] == '?' ||
            tag.find(' ') != std::string::npos)  // skip closing/decl/attr tags
            continue;
        const std::string close = "</" + tag + ">";
        const size_t end = block.find(close, i);
        if (end == std::string::npos)
            continue;
        out.emplace_back(tag, xmlUnescape(block.substr(i, end - i)));
        i = end + close.size();
    }
    return out;
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

QrzClient::QrzClient(IUiDispatcher& ui) : ui_(ui) {
    ensureCurlGlobalInit();
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
    net_ = {user, password, callsign};

    // 1) Local cache — a fresh hit skips the mesh and the network entirely.
    if (auto cached = cache_.get(callsign, cacheDays_.load())) {
        deliverResolved(std::move(*cached), {});
        return true;
    }

    // 2) Peers — ask the mesh whether anyone has it cached. The resolver replies
    //    once (UI thread) with a record or nullopt; on nullopt we hit qrz.com.
    if (peerResolver_) {
        peerResolver_(callsign, [this, w = std::weak_ptr<bool>(alive_)](
                                    std::optional<QrzResult> r) {
            if (w.expired())
                return;
            if (r && !r->call.empty()) {
                cache_.put(*r);               // remember the peer's answer locally
                deliverResolved(std::move(*r), {});
            } else {
                startNetwork();
            }
        });
        return true;
    }

    // 3) No peer source — straight to qrz.com.
    startNetwork();
    return true;
}

void QrzClient::startNetwork() {
    if (thread_.joinable())
        thread_.join();
    thread_ = std::thread(&QrzClient::worker, this, net_.user, net_.password, net_.callsign);
}

void QrzClient::deliverResolved(QrzResult result, std::string error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_    = std::move(result);
        error_     = std::move(error);
        hasResult_ = true;
    }
    // Post (never call onResult synchronously): the enrich queue sets its
    // "active" flag after lookup() returns, and relies on the result arriving
    // afterwards.
    ui_.post([this, w = std::weak_ptr<bool>(alive_)]() {
        if (!w.expired())
            deliverResult();
    });
}

std::optional<QrzResult> QrzClient::cachedLookup(const std::string& callsign) {
    return cache_.get(callsign, cacheDays_.load());
}

void QrzClient::cachePut(const QrzResult& result) {
    if (!result.call.empty())
        cache_.put(result);
}

bool QrzClient::doLogin(void* curlV, const std::string& user,
                       const std::string& password, std::string& error) {
    CURL* curl = static_cast<CURL*>(curlV);
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
}

QrzResult QrzClient::fetchOne(void* curlV, const std::string& user,
                              const std::string& password,
                              const std::string& callsign, std::string& error) {
    CURL* curl = static_cast<CURL*>(curlV);
    QrzResult result;

    auto query = [&](std::string& e) -> std::string {
        const std::string url = std::string(kBase) + "?s=" +
            urlEscape(curl, sessionKey_) + "&callsign=" + urlEscape(curl, callsign);
        return httpGet(curl, url, e);
    };

    if (sessionKey_.empty() && !doLogin(curl, user, password, error))
        return result;  // login failed (error set; sessionKey_ stays empty)

    std::string e;
    std::string body = query(e);
    std::string err = e.empty() ? xmlTag(body, "Error") : e;

    // A stale key needs a fresh login and one retry.
    if (e.empty() && !err.empty() &&
        (err.find("ession") != std::string::npos ||  // "Session Timeout"
         err.find("nvalid") != std::string::npos)) {  // "Invalid session key"
        sessionKey_.clear();
        if (!doLogin(curl, user, password, error))
            return result;  // login failed (error set)
        body = query(e);
        err  = e.empty() ? xmlTag(body, "Error") : e;
    }

    if (!err.empty()) {
        error = err;
        return result;
    }
    result.call = xmlTag(body, "call");
    if (result.call.empty()) {
        error = "No QRZ record for " + callsign;
        return result;
    }
    const size_t cs = body.find("<Callsign>");
    const size_t ce = body.find("</Callsign>");
    if (cs != std::string::npos && ce != std::string::npos && ce > cs)
        result.fields = parseElements(body.substr(cs + 10, ce - (cs + 10)));

    const std::string fn = xmlTag(body, "fname");
    const std::string ln = xmlTag(body, "name");
    result.name = fn.empty() ? ln : ln.empty() ? fn : fn + " " + ln;
    const std::string city  = xmlTag(body, "addr2");
    const std::string state = xmlTag(body, "state");
    result.qth = city.empty() ? state : state.empty() ? city : city + ", " + state;
    result.locator = xmlTag(body, "grid");
    result.country = xmlTag(body, "country");
    return result;
}

void QrzClient::worker(std::string user, std::string password, std::string callsign) {
    QrzResult result;
    std::string error;

    // The cache and peers were already consulted in lookup(); this is the
    // network fallback.
    if (CURL* curl = curl_easy_init()) {
        result = fetchOne(curl, user, password, callsign, error);
        curl_easy_cleanup(curl);
        if (error.empty() && !result.call.empty())
            cache_.put(result);
    } else {
        error = "failed to initialise libcurl";
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_    = std::move(result);
        error_     = std::move(error);
        hasResult_ = true;
    }
    ui_.post([this, w = std::weak_ptr<bool>(alive_)]() {
        if (!w.expired())
            deliverResult();
    });
}

void QrzClient::deliverResult() {
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

void QrzClient::setCache(const std::string& path, int lifetimeDays) {
    cacheDays_.store(lifetimeDays);
    cache_.open(path);
}

bool QrzClient::fillLocators(const std::string& user, const std::string& password,
                             const std::vector<std::string>& callsigns) {
    if (busy_.load())
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fillResults_.clear();
        fillFromCache_ = 0;
        fillFetched_   = 0;
        fillError_.clear();
        hasFill_ = false;
    }
    busy_.store(true);
    if (thread_.joinable())
        thread_.join();
    thread_ = std::thread(&QrzClient::fillWorker, this, user, password, callsigns);
    return true;
}

void QrzClient::fillWorker(std::string user, std::string password,
                           std::vector<std::string> callsigns) {
    std::vector<std::pair<std::string, std::string>> out;
    int fromCache = 0, fetched = 0;
    std::string error;

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "failed to initialise libcurl";
    } else {
        const int total = static_cast<int>(callsigns.size());
        int done = 0;
        for (const auto& call : callsigns) {
            QrzResult r;
            if (auto cached = cache_.get(call, cacheDays_.load())) {
                r = std::move(*cached);
                ++fromCache;
            } else {
                std::string e;
                r = fetchOne(curl, user, password, call, e);
                if (r.call.empty()) {
                    // A failed login (no valid session) is fatal for the whole
                    // run; a per-call "no record" just means skip this one.
                    if (sessionKey_.empty()) { error = e; break; }
                } else {
                    cache_.put(r);
                    ++fetched;
                }
            }
            if (!r.locator.empty())
                out.emplace_back(call, r.locator);
            ++done;
            if (done % 5 == 0 || done == total)
                ui_.post([this, w = std::weak_ptr<bool>(alive_), done, total]() {
                    if (!w.expired() && onFillProgress)
                        onFillProgress(done, total);
                });
        }
        curl_easy_cleanup(curl);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        fillResults_   = std::move(out);
        fillFromCache_ = fromCache;
        fillFetched_   = fetched;
        fillError_     = std::move(error);
        hasFill_       = true;
    }
    ui_.post([this, w = std::weak_ptr<bool>(alive_)]() {
        if (!w.expired())
            deliverFill();
    });
}

void QrzClient::deliverFill() {
    std::vector<std::pair<std::string, std::string>> results;
    int fromCache = 0, fetched = 0;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasFill_)
            return;
        results   = std::move(fillResults_);
        fromCache = fillFromCache_;
        fetched   = fillFetched_;
        error     = std::move(fillError_);
        hasFill_  = false;
    }
    if (thread_.joinable())
        thread_.join();
    busy_.store(false);
    if (onFillResult)
        onFillResult(results, fromCache, fetched, error);
}
