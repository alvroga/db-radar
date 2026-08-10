#pragma once

namespace geo {

constexpr double EARTH_RADIUS_M = 6371000.0;

/**
 * Great-circle distance between two lat/lon points, in meters.
 * Deliberately NOT the equirectangular approximation navigation.cpp uses for
 * radar rendering (ADR-0022) — that is only accurate at radar scale (a few km).
 * Candidates here (waypoint index selection) can be globally distributed.
 */
double haversineMeters(double lat1, double lon1, double lat2, double lon2);

} // namespace geo
