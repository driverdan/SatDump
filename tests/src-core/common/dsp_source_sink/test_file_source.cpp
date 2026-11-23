#include "cli_test_helpers.h"
#include "common/dsp_source_sink/file_source.h"
#include "helpers/satdump_guards.h"
#include "helpers/temp_files.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>

// FileSource::start() logs the size of the file it opens. Its format had no '%' before PRIu64,
// so it printed "filesize lu" (or "llu") and never the size
TEST_CASE("FileSource logs the size of the file it opens", "[common][dsp_source_sink][file_source]")
{
    // 512 cf32 samples
    test_helpers::TempFile file("file_source.cf32", std::string(4096, '\0'));

    // The constructor reads the default input directory from the config
    test_helpers::ConfigGuard cfg;
    satdump::satdump_cfg.main_cfg["satdump_directories"]["default_input_directory"]["value"] = ".";

    cli_test::LogCapture log;
    // Until #85 is merged, FileSource's worker thread reads is_started / should_run without
    // synchronisation (#71), as in the main_live tests. The line checked here is logged by
    // start() itself, on this thread.
    {
        FileSource src(dsp::SourceDescriptor{"file", "File", "file", false});
        src.set_settings({{"file_path", file.path.string()}, {"baseband_type", "cf32"}});
        src.set_samplerate(1000000);
        src.open();
        src.start();
        src.stop();
        src.close();
    }

    INFO(log.dump());
    CHECK(log.contains("filesize 4096", slog::LOG_DEBUG));
}

// With no samples in the file, every read gets nothing. The worker swapped empty buffers in a
// loop then, without its pacing sleep, and kept a CPU core busy.
TEST_CASE("FileSource does not spin on a file without samples", "[common][dsp_source_sink][file_source]")
{
    test_helpers::TempFile file("file_source_empty.cf32", std::string());

    test_helpers::ConfigGuard cfg;
    satdump::satdump_cfg.main_cfg["satdump_directories"]["default_input_directory"]["value"] = ".";

    FileSource src(dsp::SourceDescriptor{"file", "File", "file", false});
    src.set_settings({{"file_path", file.path.string()}, {"baseband_type", "cf32"}});
    src.set_samplerate(1000000);
    src.open();
    src.start();

    // Counts the buffers it sends, like the next block would read them
    auto stream = src.output_stream;
    std::atomic<int> buffers{0};
    std::thread reader(
        [&]()
        {
            while (stream->read() >= 0)
            {
                buffers++;
                stream->flush();
            }
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    src.stop();
    stream->stopReader();
    reader.join();
    src.close();

    CHECK(buffers < 100);
}
