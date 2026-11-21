#pragma once

/*
Comparison of large buffers, shared by the whole suite. Everything is inline
and lives in the test_helpers namespace, as every test file is linked into
the same executable.

CHECK(a == b) on two vectors prints both in full when it fails, which is
megabytes for a decoded file. bytesDiff() describes the difference instead,
and is empty when there is none:
    CHECK(test_helpers::bytesDiff(readBytes(path), expected) == "");
*/

#include <algorithm>
#include <string>
#include <vector>

namespace test_helpers
{
    // "" if both hold the same elements, otherwise the sizes and the first index that differs
    template <typename T>
    inline std::string bytesDiff(const std::vector<T> &got, const std::vector<T> &expected)
    {
        size_t common = std::min(got.size(), expected.size());
        size_t first = std::mismatch(got.begin(), got.begin() + common, expected.begin()).first - got.begin();
        if (first == common && got.size() == expected.size())
            return "";

        std::string out = "got " + std::to_string(got.size()) + " elements, expected " + std::to_string(expected.size());
        if (first < common)
            out += ", first difference at " + std::to_string(first) + " (" + std::to_string(got[first]) + " instead of " + std::to_string(expected[first]) + ")";
        else
            out += ", the common part is identical";
        return out;
    }
} // namespace test_helpers
