#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// WinTab axes use the physical device range and 16.16 counts per full circle.
// These helpers have no platform dependencies so conversion tests run without a tablet.
namespace WinTabMapping {
inline float pressure(unsigned int value, int minimum, int maximum)
{
    if (maximum <= minimum) {
        return 0.0f;
    }
    return static_cast<float>(std::clamp((static_cast<double>(value) - minimum) / (static_cast<double>(maximum) - minimum), 0.0, 1.0));
}

struct Orientation {
    uint16_t rotation = 0xffff;
    uint8_t tilt = 0xff;
};

inline Orientation orientation(int azimuth, int altitude, uint32_t azimuthResolution, uint32_t altitudeResolution)
{
    if (azimuthResolution == 0 || altitudeResolution == 0) {
        return {};
    }
    const double azimuthDegrees = azimuth * 360.0 * 65536.0 / azimuthResolution;
    const double altitudeDegrees = std::abs(static_cast<double>(altitude)) * 360.0 * 65536.0 / altitudeResolution;
    // WinTab's azimuth starts at north. Sunshine reconstructs tilt from the
    // screen-space vector using sin(-rotation), cos(-rotation).
    int rotation = static_cast<int>(std::lround(azimuthDegrees + 180.0)) % 360;
    if (rotation < 0) {
        rotation += 360;
    }
    return {static_cast<uint16_t>(rotation), static_cast<uint8_t>(std::lround(std::clamp(90.0 - altitudeDegrees, 0.0, 90.0)))};
}

struct Point {
    float x;
    float y;
    bool inside;
};

inline Point point(double x, double y, int clientWidth, int clientHeight, int streamWidth, int streamHeight)
{
    if (clientWidth <= 0 || clientHeight <= 0 || streamWidth <= 0 || streamHeight <= 0) {
        return {0, 0, false};
    }
    const double widthFromHeight = std::ceil(static_cast<double>(clientHeight) * streamWidth / streamHeight);
    const double heightFromWidth = std::ceil(static_cast<double>(clientWidth) * streamHeight / streamWidth);
    const double width = heightFromWidth > clientHeight ? widthFromHeight : clientWidth;
    const double height = heightFromWidth > clientHeight ? clientHeight : heightFromWidth;
    const double left = (clientWidth - width) / 2;
    const double top = (clientHeight - height) / 2;
    const double normalizedX = (x - left) / width;
    const double normalizedY = (y - top) / height;
    return {static_cast<float>(std::clamp(normalizedX, 0.0, 1.0)), static_cast<float>(std::clamp(normalizedY, 0.0, 1.0)),
            normalizedX >= 0 && normalizedX <= 1 && normalizedY >= 0 && normalizedY <= 1};
}
}
