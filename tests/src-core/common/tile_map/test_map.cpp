#include "common/tile_map/map.h"
#include "helpers/file_io.h"
#include "helpers/http_server.h"
#include "helpers/temp_files.h"
#include "image/io.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <utility>

// Internal helpers of tile_map/map.cpp: not in the header, but have external linkage
std::pair<double, double> deg2num(double lat, double lon, int zoom);
std::pair<double, double> from4326_to3857(double lat, double lon);

namespace
{
    const double WEBMERC_MAX_LAT = 85.0511287798066;
    const double WEBMERC_HALF_WIDTH = M_PI * 6378137.0; // 20037508.34 m
} // namespace

TEST_CASE("deg2num maps lat/lon to slippy map tile coordinates", "[common][tile_map]")
{
    auto c0 = deg2num(0, 0, 0);
    CHECK(c0.first == Catch::Approx(0.5));
    CHECK(c0.second == Catch::Approx(0.5));

    auto c1 = deg2num(0, 0, 1);
    CHECK(c1.first == Catch::Approx(1.0));
    CHECK(c1.second == Catch::Approx(1.0));

    // Edges of the world at zoom 3 (8x8 tiles)
    CHECK(deg2num(0, -180, 3).first == Catch::Approx(0.0).margin(1e-12));
    CHECK(deg2num(0, 180, 3).first == Catch::Approx(8.0));
    CHECK(deg2num(WEBMERC_MAX_LAT, 0, 3).second == Catch::Approx(0.0).margin(1e-9));
    CHECK(deg2num(-WEBMERC_MAX_LAT, 0, 3).second == Catch::Approx(8.0));

    // Northern latitudes have smaller y; east longitudes have larger x
    auto paris = deg2num(48.8566, 2.3522, 10);
    CHECK(std::floor(paris.first) == 518);
    CHECK(std::floor(paris.second) == 352);
}

TEST_CASE("from4326_to3857 projects to Web Mercator meters", "[common][tile_map]")
{
    auto o = from4326_to3857(0, 0);
    CHECK(o.first == Catch::Approx(0.0).margin(1e-6));
    CHECK(o.second == Catch::Approx(0.0).margin(1e-6));

    CHECK(from4326_to3857(0, 180).first == Catch::Approx(WEBMERC_HALF_WIDTH).epsilon(1e-12));
    CHECK(from4326_to3857(0, -180).first == Catch::Approx(-WEBMERC_HALF_WIDTH).epsilon(1e-12));
    CHECK(from4326_to3857(WEBMERC_MAX_LAT, 0).second == Catch::Approx(WEBMERC_HALF_WIDTH).epsilon(1e-9));
    CHECK(from4326_to3857(-WEBMERC_MAX_LAT, 0).second == Catch::Approx(-WEBMERC_HALF_WIDTH).epsilon(1e-9));

    // Symmetric about the equator
    CHECK(from4326_to3857(45, 10).second == Catch::Approx(-from4326_to3857(-45, 10).second).epsilon(1e-12));
}

// Tiles are only ever served by a loopback HTTP server started by the test itself (helpers/http_server.h),
// which uses POSIX sockets.
#ifndef _WIN32
namespace
{
    // A 256x256 white RGB tile, PNG encoded
    std::string whiteTilePng()
    {
        image::Image tile(8, 256, 256, 3);
        tile.fill(255);
        test_helpers::TempFile png("tile.png");
        image::save_png(tile, png.str());
        return test_helpers::readFile(png.path);
    }

    // Center pixel of a zoom 0 map of (nearly) the whole world, made of its single tile.
    // At exactly the Web Mercator latitude limit, rounding can land in the tile row above
    int downloadWorldMapCenter(test_helpers::LoopbackHttpServer &server)
    {
        image::Image map = downloadTileMap(server.url("/{z}/{x}/{y}.png"), 85, -180, -85, 180, 0);
        server.finish();
        REQUIRE(map.width() == 256);
        REQUIRE(map.height() > 250);
        REQUIRE(server.requests.size() == 1);
        CHECK(server.requests[0].rfind("GET /0/0/0.png HTTP/1.1\r\n", 0) == 0);
        return map.get(0, (map.height() / 2) * map.width() + map.width() / 2);
    }
} // namespace

TEST_CASE("downloadTileMap draws the downloaded tiles", "[common][tile_map]")
{
    test_helpers::NoProxy no_proxy;
    test_helpers::OneShotHttpServer server("200 OK", whiteTilePng());
    CHECK(downloadWorldMapCenter(server) == 255);
}

TEST_CASE("downloadTileMap leaves a tile blank when the server answers with an error", "[common][tile_map]")
{
    // The error body is a valid image, so only the HTTP status tells it is not the tile
    test_helpers::NoProxy no_proxy;
    test_helpers::OneShotHttpServer server("404 Not Found", whiteTilePng());
    CHECK(downloadWorldMapCenter(server) == 0);
}
#endif
