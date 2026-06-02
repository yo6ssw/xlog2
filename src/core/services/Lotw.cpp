#include "Lotw.h"

#include <curl/curl.h>

#include <mutex>
#include <vector>

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

} // namespace

LotwClient::LotwClient(IUiDispatcher& ui) : ui_(ui), uploader_(ui) {
    ensureCurlGlobalInit();
}

LotwClient::~LotwClient() {
    if (thread_.joinable())
        thread_.join();
}

bool LotwClient::downloadConfirmations(const std::string& user,
                                       const std::string& password,
                                       const std::string& since) {
    if (busy_.load())
        return false;

    CURL* curl = curl_easy_init();
    std::string url =
        "https://lotw.arrl.org/lotwuser/lotwreport.adi?login=" +
        urlEscape(curl, user) + "&password=" + urlEscape(curl, password) +
        "&qso_query=1&qso_qsl=yes&qso_qsldetail=yes";
    if (!since.empty())
        url += "&qso_qslsince=" + urlEscape(curl, since);
    if (curl)
        curl_easy_cleanup(curl);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        body_.clear();
        error_.clear();
        hasResult_ = false;
    }
    busy_.store(true);
    if (thread_.joinable())
        thread_.join();
    thread_ = std::thread(&LotwClient::worker, this, std::move(url));
    return true;
}

void LotwClient::worker(std::string url) {
    std::string body;
    std::string error;

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "failed to initialise libcurl";
    } else {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "xlog2/0.1");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        const CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK)
            error = curl_easy_strerror(rc);
        curl_easy_cleanup(curl);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        body_      = std::move(body);
        error_     = std::move(error);
        hasResult_ = true;
    }
    ui_.post([this, w = std::weak_ptr<bool>(alive_)]() {
        if (!w.expired())
            deliverDownload();
    });
}

void LotwClient::deliverDownload() {
    std::string body;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasResult_)
            return;
        body       = std::move(body_);
        error      = std::move(error_);
        hasResult_ = false;
    }
    if (thread_.joinable())
        thread_.join();
    busy_.store(false);
    if (onDownloadDone)
        onDownloadDone(body, error);
}

void LotwClient::uploadAdifFile(const std::string& tqslPath,
                                const std::string& stationLocation,
                                const std::string& adifPath) {
    std::vector<std::string> argv = {
        tqslPath.empty() ? "tqsl" : tqslPath,
        "-x",          // batch mode, no GUI dialogs
        "-d",          // suppress duplicate-date warnings
        "-a", "all",   // process all QSOs (don't stop on duplicates)
        "-u",          // upload to LoTW after signing
    };
    if (!stationLocation.empty()) {
        argv.push_back("-l");
        argv.push_back(stationLocation);
    }
    argv.push_back(adifPath);

    uploader_.run(argv, [this, w = std::weak_ptr<bool>(alive_)](
                            const ProcessRunner::Result& r) {
        if (w.expired() || !onUploadDone)
            return;
        if (!r.error.empty())
            onUploadDone(false, "Could not run tqsl: " + r.error +
                                    " — is tqsl installed?");
        else if (r.ok)
            onUploadDone(true, "Upload complete.");
        else
            onUploadDone(false, "tqsl failed (exit " + std::to_string(r.exitCode) +
                                    "): " + r.output);
    });
}
