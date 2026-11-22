#include "jpss_support/jpss/instruments/atms/atms_calibrator.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinRel;
using namespace jpss::atms;

namespace
{
    const int POS_X = 10;
    const uint32_t SCENE_COUNTS = 17000;

    // One scan of calibration data, built so that every input of channel 1 is a round number:
    // - all warm load PRTs and the shelf PRTs read 1077 ohm with R0 = 1000 ohm and
    //   alpha = 0.00385 (delta = beta = 0), so they are at 20 C,
    // - the warm and cold counts average to 30000 and 10000 (the average starts from -1,
    //   #96, hence the extra count on one of the 4 samples),
    // - no warm or cold bias, the band correction is the identity, no beam efficiency
    //   correction and no scan bias.
    // Only channels 1 to 5 are used: warmBias and coldBias are indexed by channel, and have 5
    // entries (#109).
    struct Options
    {
        int ich = 0; // 0-based channel
        double emissivity = 0;
        bool use_quadratic_term = true;
        int map_rc = 2;       // mapRc entry for the redundancy configuration of the scan
        bool shelf_ok = true; // false: no usable shelf PRT
    };

    nlohmann::json scanVars(const Options &o)
    {
        ATMSCalibPkt cal = {};
        cal.pamKav = 2000;
        cal.pamWg = 2000;
        for (auto &prt : cal.prtCoeffKav)
        {
            prt[0] = 1000;
            prt[1] = 0.00385;
        }
        for (auto &prt : cal.prtCoeffShelf)
        {
            prt[0] = 1000;
            prt[1] = 0.00385;
        }
        if (!o.shelf_ok)
            cal.prtCoeffShelf[1][0] = 0; // R0 = 0 marks a bad PRT
        cal.valid = true;

        // R = pam * counts / pam_counts = 2000 * 5385 / 10000 = 1077 ohm
        ATMSHotCalTempPkt hot = {};
        for (auto &prt : hot.kavPrt)
            prt = 5385;
        hot.kavPamCounts = 10000;
        hot.valid = true;

        ATMSHealtStatusPkt eng = {};
        eng.data[26] = 5385; // KKA shelf PRT
        eng.data[28] = 5385; // V shelf PRT
        eng.data[45] = 0;    // PRT offset counts
        eng.data[72] = 2;    // Redundancy configuration 2 through mapRc, index 1
        eng.valid = true;

        nlohmann::json v;
        v["calibration_pkt"] = cal;
        v["hotcal_pkt"] = hot;
        v["engineering_pkt"] = eng;
        v["beam_pos_cc"] = std::vector<double>(4, 4096);  // 22.5 deg
        v["beam_pos_wc"] = std::vector<double>(4, 32000); // 175.78125 deg
        std::vector<double> sc(96, 0);
        sc[POS_X] = 5000; // 27.46582 deg
        v["beam_pos_sc"] = sc;
        std::vector<std::vector<double>> warm(22, {30000, 30000, 30000, 30001});
        std::vector<std::vector<double>> cold(22, {10000, 10000, 10000, 10001});
        v["warm_counts"] = warm;
        v["cold_counts"] = cold;
        return v;
    }

    // The same coefficients for every channel, at 23.8 GHz
    nlohmann::json sdrCoeffs(const Options &o)
    {
        const int NCH = NUM_CHANNELS;
        nlohmann::json c;
        c["scanBias"] = std::vector<std::vector<double>>(NCH, std::vector<double>(NUM_BEAM_POSITIONS, 0));
        c["beamEfficiencyCorrection"] = std::vector<std::vector<double>>(NCH, std::vector<double>(NUM_BEAM_POSITIONS, 1));
        c["centralFrequency"] = std::vector<double>(NCH, 23.8e9);
        c["radianceBandCorrection"] = {std::vector<double>(NCH, 0), std::vector<double>(NCH, 1)};

        // mu = a0 T^2 + a1 T + a2 for T in C, given for radiances in W m-2 sr-1 Hz-1, like the
        // coefficients shipped in ATMS.cbor. Only redundancy configuration 2 (index 1) is set,
        // the others would give a very different value.
        std::vector<std::vector<std::vector<double>>> q(NUM_NL_COEFFS, std::vector<std::vector<double>>(NUM_REDUNDANCY_CONFIGS, std::vector<double>(NCH, 0)));
        for (int ch = 0; ch < NCH; ch++)
        {
            q[0][1][ch] = 1e10;
            q[1][1][ch] = 1e11;
            q[2][1][ch] = 1.5e14;
            for (int rc : {0, 2, 3})
                q[2][rc][ch] = 5e15;
        }
        c["quadraticRc"] = q;

        c["shelfTemp"] = {std::vector<double>(NUM_SHELF_TEMPS, -10), std::vector<double>(NUM_SHELF_TEMPS, 15), std::vector<double>(NUM_SHELF_TEMPS, 40)};
        c["prtConvergence"] = 0.0005;
        c["reflectorEmissivity"] = std::vector<double>(NCH, o.emissivity);
        c["reflectorTempOffset"] = 0;
        c["mapRc"] = {1, 4, o.map_rc, 3, 1, 4, 2, 3}; // As in ATMS.cbor (1-based), entry 2 from the options
        c["resolverOffset"] = 0;
        c["prtLoops"] = 50;
        c["useQuadraticTerm"] = o.use_quadratic_term ? 1 : 0;
        return c;
    }

    double calibrate(const Options &o)
    {
        nlohmann::json cfg;
        cfg["vars"] = {scanVars(o)};
        cfg["sdr_cc"] = sdrCoeffs(o);
        JpssATMSCalibrator calib(nullptr, cfg);
        return calib.compute(o.ich, POS_X, 0, SCENE_COUNTS);
    }
} // namespace

// Expected radiances (mW m-2 sr-1 (cm-1)-1) computed separately, in Python, from the JPSS
// ATMS SDR ATBD (D0001-M01-S01-001, rev. D): linear part, eq. 40 and 48 for the nonlinearity
// (mu in W m-2 sr-1 Hz-1, T in C, eq. 16 and 103), eq. 49-50 and 60-64 for the reflector
// emission corrections (sin^2 for the QV channels 1, 2 and 16, cos^2 for the others, Table 2).
// Scene counts 17000 between 10000 and 30000 give x = 0.35.
TEST_CASE("JpssATMSCalibrator applies the linear calibration", "[plugins][jpss_support][atms]")
{
    Options o;
    o.use_quadratic_term = false;
    // Tb 104.4019 K
    CHECK_THAT(calibrate(o), WithinRel(5.417231757951e-04, 1e-9));
}

TEST_CASE("JpssATMSCalibrator applies the nonlinearity correction", "[plugins][jpss_support][atms]")
{
    // Tb 103.8811 K: -0.52 K
    CHECK_THAT(calibrate(Options()), WithinRel(5.390060203803e-04, 1e-9));
}

TEST_CASE("JpssATMSCalibrator applies the reflector emission corrections", "[plugins][jpss_support][atms]")
{
    Options o;
    o.emissivity = 0.002;

    SECTION("QV channel 1: sin^2")
    {
        o.ich = 0;
        // Tb 103.8572 K
        CHECK_THAT(calibrate(o), WithinRel(5.388813013069e-04, 1e-9));
    }

    SECTION("QH channel 3: cos^2")
    {
        o.ich = 2;
        // Tb 103.9082 K
        CHECK_THAT(calibrate(o), WithinRel(5.391471817186e-04, 1e-9));
    }
}

TEST_CASE("JpssATMSCalibrator skips the nonlinearity for an unknown redundancy configuration", "[plugins][jpss_support][atms]")
{
    Options o;
    o.map_rc = GENERATE(0, 5);
    CAPTURE(o.map_rc);
    // The linear radiance
    CHECK_THAT(calibrate(o), WithinRel(5.417231757951e-04, 1e-9));
}

// Without a shelf temperature the nonlinearity and the scene reflector correction are skipped.
// The warm and cold target corrections still use the shelf value of -1 K, as before.
TEST_CASE("JpssATMSCalibrator skips the temperature dependent corrections without a shelf PRT", "[plugins][jpss_support][atms]")
{
    Options o;
    o.shelf_ok = false;

    SECTION("no reflector emission")
    {
        // The linear radiance
        CHECK_THAT(calibrate(o), WithinRel(5.417231757951e-04, 1e-9));
    }

    SECTION("with reflector emission")
    {
        o.emissivity = 0.002;
        // Tb 104.1890 K
        CHECK_THAT(calibrate(o), WithinRel(5.406125784286e-04, 1e-9));
    }
}
