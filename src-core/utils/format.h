#pragma once

/**
 * @file format.h
 * @brief String formatting utils
 */

#include <sstream>
#include <vector>

namespace satdump
{
    /**
     * @brief Format a number to string with a specific
     * float precision
     *
     * @param a_value the number to format
     * @param n precision to use
     * @return formatted string
     */
    template <typename T>
    std::string to_string_with_precision(const T a_value, const int n = 6)
    {
        std::ostringstream out;
        out.precision(n);
        out << std::fixed << a_value;
        return out.str();
    }

    /**
     * @brief Format a std::string with a standard printf
     * formatting pattern.
     *
     * @param fmt formatting string
     * @param args arguments
     * @return formatted string
     */
    template <typename... T>
    std::string svformat(const char *fmt, T &&...args)
    {
        // Start with a buffer that's big enough for us almost all the time.
        size_t size = 1024;
        std::vector<char> buf;
        buf.resize(size);

        // Try to snprintf into our buffer. It returns the length it would
        // have needed (excluding the NUL), or a negative value on error.
        // This is the C99 behaviour of snprintf on every toolchain we build
        // with, including MSVC since VS2015 (UCRT). Only the Microsoft-specific
        // _snprintf returns -1 when the output does not fit.
        int needed = snprintf(buf.data(), size, fmt, args...);

        if (needed < 0)
            return std::string();

        // snprintf also needs room for the terminating NUL
        if ((size_t)needed < size)
        {
            // It fit fine the first time, we're done.
            return std::string(buf.data(), needed);
        }
        else
        {
            // snprintf reported that it wanted to write more characters
            // than we allotted.  So grow to the right size and try again.
            // This doesn't happen very often if we chose our initial size
            // well.
            buf.resize((size_t)needed + 1);
            snprintf(buf.data(), buf.size(), fmt, args...);
            return std::string(buf.data(), needed);
        }
    }
} // namespace satdump