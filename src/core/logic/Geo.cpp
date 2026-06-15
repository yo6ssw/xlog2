#include "Geo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace geo {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthKm = 6371.0;  // mean radius
double rad(double d) { return d * kPi / 180.0; }
double deg(double r) { return r * 180.0 / kPi; }

std::string envOr(const char* var, const char* home_rel) {
    if (const char* v = std::getenv(var); v && *v)
        return v;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/" + home_rel;
}
}  // namespace

std::optional<LatLon> maidenheadToLatLon(std::string g) {
    // Strip whitespace and upper-case; valid lengths are 2, 4, 6 or 8.
    g.erase(std::remove_if(g.begin(), g.end(),
                           [](unsigned char c) { return std::isspace(c); }),
            g.end());
    const std::size_t n = g.size();
    if (n != 2 && n != 4 && n != 6 && n != 8)
        return std::nullopt;
    for (auto& c : g) c = static_cast<char>(std::toupper((unsigned char)c));

    auto isField = [](char c) { return c >= 'A' && c <= 'R'; };  // A..R (18)
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    auto isSub   = [](char c) { return c >= 'A' && c <= 'X'; };  // A..X (24)

    if (!isField(g[0]) || !isField(g[1]))
        return std::nullopt;
    // Origin at the SW corner; we accumulate cell size as we descend, then add
    // half the finest cell to land in the centre.
    double lon = (g[0] - 'A') * 20.0 - 180.0;
    double lat = (g[1] - 'A') * 10.0 - 90.0;
    double lonCell = 20.0, latCell = 10.0;

    if (n >= 4) {
        if (!isDigit(g[2]) || !isDigit(g[3])) return std::nullopt;
        lonCell = 2.0;  latCell = 1.0;
        lon += (g[2] - '0') * lonCell;
        lat += (g[3] - '0') * latCell;
    }
    if (n >= 6) {
        if (!isSub(g[4]) || !isSub(g[5])) return std::nullopt;
        lonCell = 2.0 / 24.0;  latCell = 1.0 / 24.0;
        lon += (g[4] - 'A') * lonCell;
        lat += (g[5] - 'A') * latCell;
    }
    if (n >= 8) {
        if (!isDigit(g[6]) || !isDigit(g[7])) return std::nullopt;
        lonCell = (2.0 / 24.0) / 10.0;  latCell = (1.0 / 24.0) / 10.0;
        lon += (g[6] - '0') * lonCell;
        lat += (g[7] - '0') * latCell;
    }
    return LatLon{lat + latCell / 2.0, lon + lonCell / 2.0};
}

std::string latLonToMaidenhead(LatLon p) {
    double lon = p.lon + 180.0;
    double lat = p.lat + 90.0;
    lon = std::clamp(lon, 0.0, 359.999999);
    lat = std::clamp(lat, 0.0, 179.999999);
    std::string g(6, ' ');
    g[0] = static_cast<char>('A' + static_cast<int>(lon / 20.0));
    g[1] = static_cast<char>('A' + static_cast<int>(lat / 10.0));
    lon = std::fmod(lon, 20.0);
    lat = std::fmod(lat, 10.0);
    g[2] = static_cast<char>('0' + static_cast<int>(lon / 2.0));
    g[3] = static_cast<char>('0' + static_cast<int>(lat / 1.0));
    lon = std::fmod(lon, 2.0);
    lat = std::fmod(lat, 1.0);
    g[4] = static_cast<char>('A' + static_cast<int>(lon / (2.0 / 24.0)));
    g[5] = static_cast<char>('A' + static_cast<int>(lat / (1.0 / 24.0)));
    return g;
}

double distanceKm(LatLon a, LatLon b) {
    const double dLat = rad(b.lat - a.lat);
    const double dLon = rad(b.lon - a.lon);
    const double s = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(rad(a.lat)) * std::cos(rad(b.lat)) *
                         std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * kEarthKm * std::asin(std::min(1.0, std::sqrt(s)));
}

double bearingDeg(LatLon a, LatLon b) {
    const double dLon = rad(b.lon - a.lon);
    const double y = std::sin(dLon) * std::cos(rad(b.lat));
    const double x = std::cos(rad(a.lat)) * std::sin(rad(b.lat)) -
                     std::sin(rad(a.lat)) * std::cos(rad(b.lat)) * std::cos(dLon);
    double br = deg(std::atan2(y, x));
    return std::fmod(br + 360.0, 360.0);
}

std::vector<LatLon> greatCircle(LatLon a, LatLon b, int segments) {
    if (segments < 1) segments = 1;
    // Convert to unit vectors and slerp; angular distance via the dot product.
    auto toVec = [](LatLon p, double v[3]) {
        const double la = rad(p.lat), lo = rad(p.lon);
        v[0] = std::cos(la) * std::cos(lo);
        v[1] = std::cos(la) * std::sin(lo);
        v[2] = std::sin(la);
    };
    double va[3], vb[3];
    toVec(a, va);
    toVec(b, vb);
    double dot = va[0] * vb[0] + va[1] * vb[1] + va[2] * vb[2];
    dot = std::clamp(dot, -1.0, 1.0);
    const double omega = std::acos(dot);

    std::vector<LatLon> out;
    out.reserve(segments + 1);
    if (omega < 1e-9) {  // coincident (or nearly): just the endpoints
        out.push_back(a);
        out.push_back(b);
        return out;
    }
    const double sinOmega = std::sin(omega);
    for (int i = 0; i <= segments; ++i) {
        const double t = static_cast<double>(i) / segments;
        const double s0 = std::sin((1 - t) * omega) / sinOmega;
        const double s1 = std::sin(t * omega) / sinOmega;
        const double x = s0 * va[0] + s1 * vb[0];
        const double y = s0 * va[1] + s1 * vb[1];
        const double z = s0 * va[2] + s1 * vb[2];
        out.push_back({deg(std::atan2(z, std::sqrt(x * x + y * y))), deg(std::atan2(y, x))});
    }
    return out;
}

XY equirect(LatLon p) {
    return {(p.lon + 180.0) / 360.0, (90.0 - p.lat) / 180.0};
}

std::vector<std::vector<LatLon>> loadCoastline(const std::string& path) {
    std::vector<std::vector<LatLon>> polylines;
    if (path.empty())
        return polylines;
    std::ifstream in(path);
    if (!in)
        return polylines;
    // Format: per record, an integer point count N followed by N "lon lat"
    // pairs (whitespace-separated; '#' comment lines are skipped).
    auto skipComments = [&]() {
        while (in >> std::ws && in.peek() == '#')
            in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    };
    long n = 0;
    while (skipComments(), in >> n) {
        if (n <= 0) continue;
        std::vector<LatLon> poly;
        poly.reserve(static_cast<std::size_t>(n));
        for (long i = 0; i < n; ++i) {
            double lon = 0, lat = 0;
            if (!(in >> lon >> lat))
                return polylines;  // truncated; keep what parsed
            poly.push_back({lat, lon});
        }
        if (poly.size() >= 2)
            polylines.push_back(std::move(poly));
    }
    return polylines;
}

std::string defaultCoastlinePath() {
    const std::string candidates[] = {
        envOr("XDG_DATA_HOME", ".local/share") + "/xlog2/coastline.txt",
        "/usr/share/xlog2/coastline.txt",
        "data/coastline.txt",
    };
    for (const auto& c : candidates) {
        std::ifstream in(c);
        if (in)
            return c;
    }
    return "";
}

}  // namespace geo
