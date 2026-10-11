#include "wintabmapping.h"
#include <stdexcept>
#include <cmath>
#include <iostream>

// Keep checks active for release test builds as well as debug builds.
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

int main()
{
    using namespace WinTabMapping;
    CHECK(pressure(10, 10, 110) == 0);
    CHECK(pressure(110, 10, 110) == 1);
    CHECK(pressure(60, 10, 110) == 0.5f);
    CHECK(pressure(0, 10, 110) == 0);
    CHECK(pressure(120, 10, 110) == 1);
    CHECK(pressure(10, 10, 10) == 0);
    auto vertical = orientation(0, 900, 3600u << 16, 3600u << 16);
    CHECK(vertical.tilt == 0 && vertical.rotation == 180);
    auto right = orientation(900, 450, 3600u << 16, 3600u << 16);
    CHECK(right.tilt == 45 && right.rotation == 270);
    auto eraser = orientation(900, -450, 3600u << 16, 3600u << 16);
    CHECK(eraser.tilt == right.tilt && eraser.rotation == right.rotation);
    CHECK(orientation(0, 0, 0, 0).tilt == 0xff);
    auto center = point(800, 450, 1600, 900, 1920, 1080);
    CHECK(center.inside && center.x == 0.5f && center.y == 0.5f);
    auto top = point(400, 275, 800, 1000, 1920, 1080);
    CHECK(top.inside && top.x == 0.5f && top.y == 0);
    CHECK(!point(400, 270, 800, 1000, 1920, 1080).inside);
    auto outside = point(-1, 1001, 800, 1000, 1920, 1080);
    CHECK(!outside.inside && outside.x == 0 && outside.y == 1);
    CHECK(!point(1, 1, 0, 900, 1920, 1080).inside);
    std::cout << "WinTab pressure, orientation and letterbox mapping tests passed\n";
}
