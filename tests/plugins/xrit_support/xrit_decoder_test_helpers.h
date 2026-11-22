#pragma once

// Shared helpers for the tests of the xRIT data decoders of the GK-2A, FengYun-4 and
// Elektro-L plugins: they are given a .cadu file made with xrit_test_helpers.h, and write
// the files they decode below an output directory.

#include "cli_test_helpers.h"
#include "helpers/file_io.h"
#include "pipeline/pipeline_test_helpers.h"
#include "xrit_support/xrit_test_helpers.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace xrit_decoder_test
{
    using namespace xrit_test;

    struct DecoderEnv
    {
        test_helpers::ConfigGuard cfg;
        cli_test::LogCapture log;
        test_helpers::TempDir tmp{"xrit_decoder"};
        std::string out = tmp.file("out");

        std::string path(const std::string &name) { return out + "/" + name; }
        std::string read(const std::string &name) { return test_helpers::readFile(path(name)); }

        // Every file below the output directory, relative to it
        std::vector<std::string> files()
        {
            std::vector<std::string> list;
            if (std::filesystem::exists(out))
                for (auto &e : std::filesystem::recursive_directory_iterator(out))
                    if (e.is_regular_file())
                        list.push_back(std::filesystem::relative(e.path(), out).generic_string());
            std::sort(list.begin(), list.end());
            return list;
        }

        // Runs the decoder made by getInstance on the .cadu file of what the mux holds. A fill
        // frame is added at the end, as there always is one on the air.
        template <typename Module>
        void decode(Mux &mux, nlohmann::json params = nlohmann::json::object())
        {
            mux.vcdus(); // Completes every virtual channel first
            mux.fill();
            auto vcdus = mux.vcdus();
            std::string input = tmp.file("in.cadu");
            test_helpers::writeBytes(input, caduFile(vcdus));

            auto m = Module::getInstance(input, out + "/decoder", params);
            m->setInputType(satdump::pipeline::DATA_FILE);
            m->setOutputType(satdump::pipeline::DATA_FILE);
            m->init();
            m->process();
        }

        // Checks that this error was logged, and only once
        void checkErrorOnce(const std::string &error)
        {
            INFO(log.dump());
            CHECK(log.contains(error, slog::LOG_ERROR));
            std::string all = log.dump();
            size_t pos = all.find(error);
            CHECK((pos != std::string::npos && all.find(error, pos + 1) == std::string::npos));
        }
    };

    // A text file (type 2) that no satellite claims, so the decoders save it as received
    inline Bytes unknownFile(const std::string &name, const std::string &text, const std::vector<Bytes> &extra_records = {})
    {
        std::vector<Bytes> records = {annotationRecord(name)};
        records.insert(records.end(), extra_records.begin(), extra_records.end());
        return lritFile(2, records, Bytes(text.begin(), text.end()));
    }

    // The key header (type 7) of GK-2A and FengYun-4: a key other than 0 means the file is
    // encrypted
    inline Bytes keyRecord(uint32_t key)
    {
        Bytes c;
        put32(c, key);
        return record(7, c);
    }
} // namespace xrit_decoder_test
