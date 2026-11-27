#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <string>

#include "libs/calceph/calceph.h"
#include "libs/supernovas/novas-calceph.h"
#include "libs/supernovas/novas.h"

using Catch::Matchers::WithinAbs;

namespace
{
    // Uses the DE440s ephemeris that SatDump ships and loads at startup (src-core/init.cpp).
    // It stays open for the rest of the run, as SuperNOVAS keeps a pointer to it.
    void useShippedEphemeris()
    {
        static t_calcephbin *eph = nullptr;
        if (eph == nullptr)
        {
            std::string path = std::string(SATDUMP_TEST_SOURCE_DIR) + "/resources/spice/de440s.bsp";
            const char *files[] = {path.c_str()};
            eph = calceph_open_array(1, files);
        }
        REQUIRE(eph != nullptr);
        REQUIRE(novas_use_calceph(eph) == 0);
    }

    // Geocentric frame at 2025-09-23 18:00:00 UTC, with the given EOP values (UT1-UTC in s, pole offsets in mas)
    novas_frame geocentricFrame(double dut1 = 0.05, double xp_mas = 0.2, double yp_mas = 0.4)
    {
        novas_timespec t;
        REQUIRE(novas_set_unix_time(1758650400, 0, 37, dut1, &t) == 0);
        observer geo;
        REQUIRE(make_observer_at_geocenter(&geo) == 0);
        novas_frame f;
        REQUIRE(novas_make_frame(NOVAS_FULL_ACCURACY, &geo, &t, xp_mas, yp_mas, &f) == 0);
        return f;
    }

    struct BodyRef
    {
        enum novas_planet body;
        double ra_h;
        double dec_deg;
        double dist_km;
    };

    double norm(const double *v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
} // namespace

// Reference values are from SuperNOVAS v1.8.0. The previously vendored 1.5.0 snapshot agrees to
// within 0.01 mas and 0.1 m, so the tolerances (1 mas, 1 m) only catch real regressions.
TEST_CASE("SuperNOVAS: apparent positions of Solar-system bodies from DE440s", "[supernovas]")
{
    useShippedEphemeris();
    novas_frame f = geocentricFrame();

    const double one_mas_deg = 1.0 / 3600000.0;
    const BodyRef refs[] = {
        {NOVAS_SUN, 12.059078012116, -0.384261001901, 150098901.700844},
        {NOVAS_MOON, 13.312019598524, -11.500171990988, 402310.555556},
        {NOVAS_MARS, 13.921598256623, -11.807141037449, 349718748.842588},
        {NOVAS_JUPITER, 7.550408505444, 21.725154521085, 814099202.197676},
    };

    for (const BodyRef &ref : refs)
    {
        INFO("body " << (int)ref.body);
        object o;
        REQUIRE(make_planet(ref.body, &o) == 0);

        sky_pos sp;
        REQUIRE(novas_sky_pos(&o, &f, NOVAS_TOD, &sp) == 0);
        CHECK_THAT(sp.ra * 15.0 * std::cos(ref.dec_deg * M_PI / 180.0), WithinAbs(ref.ra_h * 15.0 * std::cos(ref.dec_deg * M_PI / 180.0), one_mas_deg));
        CHECK_THAT(sp.dec, WithinAbs(ref.dec_deg, one_mas_deg));

        double pos[3], vel[3];
        REQUIRE(novas_geom_posvel(&o, &f, NOVAS_ICRS, pos, vel) == 0);
        CHECK_THAT(norm(pos) * NOVAS_AU_KM, WithinAbs(ref.dist_km, 0.001));
    }
}

// SuperNOVAS before 1.6.0 returned velocities relative to the Solar-system barycenter here
TEST_CASE("SuperNOVAS: geometric velocity is relative to the observer", "[supernovas]")
{
    useShippedEphemeris();
    novas_frame f = geocentricFrame();

    object moon;
    REQUIRE(make_planet(NOVAS_MOON, &moon) == 0);
    double pos[3], vel[3];
    REQUIRE(novas_geom_posvel(&moon, &f, NOVAS_ICRS, pos, vel) == 0);
    double speed_kms = norm(vel) * NOVAS_AU_KM / 86400.0;
    CHECK(speed_kms > 0.9);
    CHECK(speed_kms < 1.1);
}

// The conversion that the AWS NavAtt reader uses on its ephemeris, with realistic EOP values.
// The 1 mm tolerance (0.03 mas at 7000 km) catches the previous copy (6.5 mm off, as it lacked the
// diurnal UT1 correction), missing pole offsets (12 m with xp = yp = 0) and UT1 errors of a few us.
TEST_CASE("SuperNOVAS: J2000 to ITRS transform", "[supernovas]")
{
    // SatDump supplies its own EOP values: SuperNOVAS is built without cURL, so it never fetches them
    CHECK(novas_is_auto_fetch_eop() == 0);

    useShippedEphemeris();
    novas_frame f = geocentricFrame(0.0812, 153.0, 371.0);

    novas_transform T;
    REQUIRE(novas_make_transform(&f, NOVAS_J2000, NOVAS_ITRS, &T) == 0);
    double v[3] = {7000e3 / NOVAS_AU, 0, 0}, out[3];
    REQUIRE(novas_transform_vector(v, &T, out) == 0);

    CHECK_THAT(out[0] * NOVAS_AU, WithinAbs(306072.148844, 0.001));
    CHECK_THAT(out[1] * NOVAS_AU, WithinAbs(6993283.319571, 0.001));
    CHECK_THAT(out[2] * NOVAS_AU, WithinAbs(17557.104222, 0.001));
    CHECK_THAT(norm(out) * NOVAS_AU, WithinAbs(7000e3, 1e-3));
}
