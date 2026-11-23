#ifdef USE_OPENCL
#include "core/opencl.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <stdexcept>

// The test binary never calls initOpenCL(), so this is the state before (or without) it
TEST_CASE("opencl::useCL() is false before OpenCL is initialised", "[core][opencl]")
{
    CHECK_FALSE(satdump::opencl::useCL());
}

TEST_CASE("opencl::setupOCLContext() refuses to run before OpenCL is initialised", "[core][opencl]")
{
    CHECK_THROWS_WITH(satdump::opencl::setupOCLContext(), Catch::Matchers::StartsWith("User specified CPU processing"));
}
#endif
