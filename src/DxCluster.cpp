#include "DxCluster.h"

#include "Bands.h"

#include <giomm/inputstream.h>
#include <giomm/outputstream.h>
#include <glibmm/bytes.h>
#include <glibmm/main.h>

#include <cctype>
#include <cstdlib>
#include <vector>

namespace {

std::string toLower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// A trailing time stamp like "1432Z": digits followed by 'Z'.
bool isTimeToken(const std::string& t) {
    if (t.size() < 3 || t.size() > 5)
        return false;
    if (std::toupper(static_cast<unsigned char>(t.back())) != 'Z')
        return false;
    for (size_t i = 0; i + 1 < t.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(t[i])))
            return false;
    return true;
}

bool looksLikeLoginPrompt(const std::string& lower) {
    return lower.find("login:") != std::string::npos ||
           lower.find("your call") != std::string::npos ||
           lower.find("enter your call") != std::string::npos ||
           lower.find("call:") != std::string::npos ||
           lower.find("callsign:") != std::string::npos;
}

}  // namespace

namespace dxcluster {

std::optional<DxSpot> parseSpot(const std::string& line) {
    const std::string lower = toLower(line);
    const std::string pfx = "dx de ";
    if (lower.compare(0, pfx.size(), pfx) != 0)
        return std::nullopt;

    const size_t colon = line.find(':', pfx.size());
    if (colon == std::string::npos)
        return std::nullopt;

    DxSpot s;
    s.spotter = trim(line.substr(pfx.size(), colon - pfx.size()));

    const std::vector<std::string> tok = splitWs(line.substr(colon + 1));
    if (tok.size() < 2)
        return std::nullopt;

    char* endp = nullptr;
    s.freqKHz = std::strtod(tok[0].c_str(), &endp);
    if (endp == tok[0].c_str() || s.freqKHz <= 0.0)
        return std::nullopt;  // first token wasn't a frequency
    s.dxCall = tok[1];

    size_t commentEnd = tok.size();
    if (tok.size() >= 3 && isTimeToken(tok.back())) {
        s.timeUtc = tok.back();
        commentEnd = tok.size() - 1;
    }
    for (size_t i = 2; i < commentEnd; ++i) {
        if (!s.comment.empty()) s.comment += ' ';
        s.comment += tok[i];
    }

    s.band = bands::forFrequencyMHz(s.freqKHz / 1000.0);
    return s;
}

}  // namespace dxcluster

DxCluster::~DxCluster() {
    // Drop callbacks before tearing down: at shutdown the owning panel may
    // already be destroyed, and teardown() would otherwise invoke onStatus and
    // call into the freed widget (use-after-free).
    onSpot   = nullptr;
    onLine   = nullptr;
    onStatus = nullptr;
    disconnect();
}

void DxCluster::connectTo(const std::string& host, int port,
                          const std::string& loginCall) {
    disconnect();
    if (host.empty() || port <= 0 || port > 65535) {
        if (onStatus) onStatus("DX cluster: invalid host/port.");
        return;
    }
    loginCall_  = loginCall;
    loggedIn_   = false;
    cancellable_ = Gio::Cancellable::create();
    client_      = Gio::SocketClient::create();
    if (onStatus)
        onStatus("DX cluster: connecting to " + host + ":" + std::to_string(port) + "…");
    client_->connect_to_host_async(host, static_cast<guint16>(port), cancellable_,
                                   sigc::mem_fun(*this, &DxCluster::onConnected));
}

void DxCluster::onConnected(const Glib::RefPtr<Gio::AsyncResult>& result) {
    try {
        conn_ = client_->connect_to_host_finish(result);
    } catch (const Glib::Error& e) {
        teardown("DX cluster: connect failed — " + std::string(e.what()));
        return;
    }
    if (!conn_) {
        teardown("DX cluster: connect failed.");
        return;
    }
    connected_ = true;
    in_ = Gio::DataInputStream::create(conn_->get_input_stream());
    if (onStatus) onStatus("DX cluster: connected.");
    queueRead();

    // Auto-login: we normally send the call when the cluster's login prompt is
    // recognised (handleLine), but prompts vary between cluster software, so
    // fall back to sending it a short time after connecting if no prompt fired.
    if (!loginCall_.empty())
        loginTimer_ = Glib::signal_timeout().connect(
            [this]() { sendLogin(); return false; /* one-shot */ }, 2000);
}

void DxCluster::sendLogin() {
    if (!connected_ || loggedIn_ || loginCall_.empty())
        return;
    writeRaw(loginCall_ + "\r\n");
    loggedIn_ = true;
    if (onStatus) onStatus("DX cluster: sent login " + loginCall_ + ".");
}

void DxCluster::queueRead() {
    if (in_ && connected_)
        in_->read_line_async(sigc::mem_fun(*this, &DxCluster::onLineRead),
                             cancellable_);
}

void DxCluster::onLineRead(const Glib::RefPtr<Gio::AsyncResult>& result) {
    if (!connected_ || !in_)
        return;  // a read completing after we tore down
    std::string line;
    try {
        if (!in_->read_line_finish(result, line)) {
            teardown("DX cluster: disconnected.");
            return;
        }
    } catch (const Glib::Error& e) {
        teardown("DX cluster: " + std::string(e.what()));
        return;
    }
    if (line.empty()) {  // EOF — the server closed the connection
        teardown("DX cluster: connection closed by server.");
        return;
    }
    handleLine(std::move(line));
    queueRead();
}

void DxCluster::handleLine(std::string line) {
    if (!line.empty() && line.back() == '\r')  // telnet CRLF; '\n' already stripped
        line.pop_back();

    if (onLine) onLine(line);

    if (!loggedIn_ && !loginCall_.empty() && looksLikeLoginPrompt(toLower(line))) {
        loginTimer_.disconnect();  // beat the fallback timer to it
        sendLogin();
    }

    if (auto spot = dxcluster::parseSpot(line); spot && onSpot)
        onSpot(*spot);
}

void DxCluster::sendCommand(const std::string& line) {
    if (!connected_) {
        if (onStatus) onStatus("DX cluster: not connected.");
        return;
    }
    writeRaw(line + "\r\n");
}

void DxCluster::writeRaw(const std::string& bytes) {
    if (!conn_)
        return;
    auto os = conn_->get_output_stream();
    auto b  = Glib::Bytes::create(bytes.data(), bytes.size());
    os->write_bytes_async(
        b,
        [os](const Glib::RefPtr<Gio::AsyncResult>& r) {
            try { os->write_bytes_finish(r); } catch (const Glib::Error&) {}
        },
        cancellable_);
}

void DxCluster::teardown(const std::string& statusMsg) {
    const bool announce = connected_ || !statusMsg.empty();
    connected_ = false;
    loggedIn_  = false;
    loginTimer_.disconnect();
    if (cancellable_) cancellable_->cancel();
    if (conn_) { try { conn_->close(); } catch (const Glib::Error&) {} }
    in_.reset();
    conn_.reset();
    client_.reset();
    cancellable_.reset();
    if (announce && onStatus) onStatus(statusMsg);
}

void DxCluster::disconnect() {
    if (connected_ || conn_ || client_)
        teardown("DX cluster: disconnected.");
}
