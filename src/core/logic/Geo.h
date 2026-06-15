#pragma once

#include <optional>
#include <string>
#include <vector>

// Toolkit-neutral geography helpers for the world-map panel: Maidenhead grid
// <-> latitude/longitude, great-circle distance/bearing/interpolation, an
// equirectangular projection, and a loader for the bundled coastline data.
namespace geo {

struct LatLon {
    double lat = 0.0;  // degrees, +N
    double lon = 0.0;  // degrees, +E
};

// Normalised equirectangular coordinates in [0,1]x[0,1] (x: 0=180W..1=180E,
// y: 0=90N..1=90S). The views map this into a centred 2:1 rectangle.
struct XY {
    double x = 0.0;
    double y = 0.0;
};

// Parse a 2/4/6/8-character Maidenhead locator (case-insensitive) and return the
// centre of the addressed square. Returns nullopt for structurally invalid input
// (so a half-typed field simply draws nothing).
std::optional<LatLon> maidenheadToLatLon(std::string grid);

// Maidenhead grid (6 characters) for a coordinate — handy for tests/round-trips.
std::string latLonToMaidenhead(LatLon p);

// Great-circle distance in kilometres (haversine, mean Earth radius).
double distanceKm(LatLon a, LatLon b);

// Initial great-circle bearing from a to b, degrees clockwise from true north.
double bearingDeg(LatLon a, LatLon b);

// Points along the great circle from a to b (inclusive of both ends), sampled at
// `segments`+1 positions via spherical interpolation — a smooth curve when drawn
// in the equirectangular projection.
std::vector<LatLon> greatCircle(LatLon a, LatLon b, int segments = 128);

// Equirectangular projection to normalised [0,1] coordinates.
XY equirect(LatLon p);

// Load the bundled coastline: a list of polylines (each a run of LatLon). Reads
// the compact text format (see data/coastline.txt). Returns empty on a missing
// or unreadable file, so the map degrades gracefully to a bare graticule.
std::vector<std::vector<LatLon>> loadCoastline(const std::string& path);

// First existing coastline path among the standard locations ($XDG_DATA_HOME,
// /usr/share/xlog2, ./data), or "" if none is present.
std::string defaultCoastlinePath();

}  // namespace geo
