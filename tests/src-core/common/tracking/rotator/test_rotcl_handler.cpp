#include "common/tracking/rotator/rotcl_handler.h"
#include <catch2/catch_test_macros.hpp>

// Only the parts of RotctlHandler that work without a rotctld connection are tested here.

TEST_CASE("RotctlHandler identifies itself and defaults to localhost:4533", "[common][tracking][rotator]")
{
    rotator::RotctlHandler h;
    CHECK(h.get_id() == "rotctl");
    nlohmann::json s = h.get_settings();
    CHECK(s["address"] == "127.0.0.1");
    CHECK(s["port"] == 4533);
    CHECK_FALSE(h.is_connected());
}

TEST_CASE("RotctlHandler settings round-trip", "[common][tracking][rotator]")
{
    rotator::RotctlHandler h;
    nlohmann::json in;
    in["address"] = "192.168.100.200";
    in["port"] = 12345;
    h.set_settings(in);
    nlohmann::json out = h.get_settings();
    CHECK(out["address"] == "192.168.100.200");
    CHECK(out["port"] == 12345);
}

TEST_CASE("RotctlHandler keeps current settings for missing or invalid fields", "[common][tracking][rotator]")
{
    rotator::RotctlHandler h;
    h.set_settings(nlohmann::json::object());
    CHECK(h.get_settings()["address"] == "127.0.0.1");
    CHECK(h.get_settings()["port"] == 4533);

    nlohmann::json bad;
    bad["port"] = "not a number";
    h.set_settings(bad);
    CHECK(h.get_settings()["port"] == 4533);
}

TEST_CASE("RotctlHandler accepts an address shorter than the previous one", "[common][tracking][rotator]")
{
    // The old tail must not survive ("127.0.0.1" -> "10.0.0.1" must not become "10.0.0.11")
    rotator::RotctlHandler h;
    nlohmann::json in;
    in["address"] = "10.0.0.1";
    h.set_settings(in);
    CHECK(h.get_settings()["address"] == "10.0.0.1");
}

TEST_CASE("RotctlHandler truncates an address too long for its buffer", "[common][tracking][rotator]")
{
    rotator::RotctlHandler h;
    std::string long_address(500, 'a');
    nlohmann::json in;
    in["address"] = long_address;
    in["port"] = 1234;
    h.set_settings(in);
    nlohmann::json out = h.get_settings();
    // The buffer holds 100 bytes including the terminator
    CHECK(out["address"] == std::string(99, 'a'));
    CHECK(out["port"] == 1234);

    // An address of exactly 99 characters fits, and a shorter one afterwards replaces it fully
    in["address"] = std::string(99, 'b');
    h.set_settings(in);
    CHECK(h.get_settings()["address"] == std::string(99, 'b'));
    in["address"] = "localhost";
    h.set_settings(in);
    CHECK(h.get_settings()["address"] == "localhost");
}

TEST_CASE("RotctlHandler commands fail with a connection error when not connected", "[common][tracking][rotator]")
{
    rotator::RotctlHandler h;
    float az = -1, el = -1;
    CHECK(h.get_pos(&az, &el) == rotator::ROT_ERROR_CON);
    // Outputs are left untouched
    CHECK(az == -1);
    CHECK(el == -1);
    CHECK(h.set_pos(180, 45) == rotator::ROT_ERROR_CON);
    // Disconnecting while not connected is harmless
    h.disconnect();
    CHECK_FALSE(h.is_connected());
}
