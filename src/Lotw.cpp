#include "Lotw.h"

#include <giomm/subprocess.h>

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

LotwClient::LotwClient() {
    ensureCurlGlobalInit();
    dispatcher_.connect(sigc::mem_fun(*this, &LotwClient::onDispatch));
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
    dispatcher_.emit();
}

void LotwClient::onDispatch() {
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

    try {
        auto proc = Gio::Subprocess::create(argv, Gio::Subprocess::Flags::NONE);
        proc->wait_check_async(
            [this, proc](const Glib::RefPtr<Gio::AsyncResult>& result) {
                try {
                    proc->wait_check_finish(result);
                    if (onUploadDone)
                        onUploadDone(true, "Upload complete.");
                } catch (const Glib::Error& e) {
                    if (onUploadDone)
                        onUploadDone(false, std::string("tqsl failed: ") + e.what());
                }
            });
    } catch (const Glib::Error& e) {
        if (onUploadDone)
            onUploadDone(false,
                         std::string("Could not run tqsl: ") + e.what() +
                             " — is tqsl installed?");
    }
}
