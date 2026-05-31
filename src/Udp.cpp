#include "Udp.h"

#include "Adif.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

// ADIF is ASCII, so dropping NUL bytes turns a UTF-16- or UTF-8-encoded
// WSJT-X string into plain ASCII while leaving raw ADIF text unchanged.
std::string stripNuls(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in)
        if (c != '\0')
            out += c;
    return out;
}

// Reads a QDataStream-encoded string: a big-endian uint32 byte count
// (0xffffffff means null), then that many bytes. Advances `pos`.
bool readQtString(const std::uint8_t* d, std::size_t len, std::size_t& pos,
                  std::string& out) {
    if (pos + 4 > len)
        return false;
    const std::uint32_t n = be32(d + pos);
    pos += 4;
    if (n == 0xffffffffu) {  // null string
        out.clear();
        return true;
    }
    if (pos + n > len)
        return false;
    out.assign(reinterpret_cast<const char*>(d + pos), n);
    pos += n;
    return true;
}

constexpr std::uint32_t kWsjtxMagic      = 0xadbccbdau;
constexpr std::uint32_t kWsjtxLoggedAdif = 12;

} // namespace

std::vector<Qso> decodeDatagram(const std::uint8_t* data, std::size_t len,
                                std::string& source) {
    // WSJT-X binary packet: magic(4) schema(4) type(4) then payload, whose
    // first field is always the instance id (a string).
    if (len >= 12 && be32(data) == kWsjtxMagic) {
        if (be32(data + 8) != kWsjtxLoggedAdif)
            return {};  // heartbeat/status/decode/… carry no logged QSO
        std::size_t pos = 12;
        std::string id, adifText;
        if (!readQtString(data, len, pos, id) ||
            !readQtString(data, len, pos, adifText))
            return {};
        source = "WSJT-X";
        return adif::parse(stripNuls(adifText));
    }

    // Otherwise treat the whole datagram as raw ADIF text.
    source = "ADIF/UDP";
    return adif::parse(
        stripNuls(std::string(reinterpret_cast<const char*>(data), len)));
}

UdpListener::~UdpListener() {
    stop();
}

bool UdpListener::start(int port, std::string& error) {
    stop();

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        error = std::strerror(errno);
        return false;
    }

    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<std::uint16_t>(port));
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    const int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    port_ = port;
    ioConn_ = Glib::signal_io().connect(
        sigc::mem_fun(*this, &UdpListener::onReadable), fd_,
        Glib::IOCondition::IO_IN);
    return true;
}

void UdpListener::stop() {
    if (ioConn_.connected())
        ioConn_.disconnect();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    port_ = 0;
}

bool UdpListener::onReadable(Glib::IOCondition) {
    std::uint8_t buf[65536];
    for (;;) {
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n < 0)
            break;       // EAGAIN: datagram queue drained
        if (n == 0)
            continue;    // empty datagram
        std::string source;
        const auto qsos =
            decodeDatagram(buf, static_cast<std::size_t>(n), source);
        if (!qsos.empty() && callback_)
            callback_(qsos, source);
    }
    return true;  // keep watching
}
