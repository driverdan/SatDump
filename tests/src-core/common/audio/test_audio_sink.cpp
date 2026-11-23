#include "common/audio/audio_sink.h"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <vector>

namespace
{
    // Records which factory produced it
    class TaggedSink : public audio::AudioSink
    {
    public:
        std::string tag;
        TaggedSink(std::string t) : tag(t) {}
        void set_samplerate(int) {}
        void start() {}
        void stop() {}
        void push_samples(int16_t *, int) {}
    };

    // Temporarily swaps the global sink registry
    struct RegistryGuard
    {
        std::map<std::string, std::function<std::shared_ptr<audio::AudioSink>()>> saved;
        RegistryGuard() : saved(audio::sink_registry) { audio::sink_registry.clear(); }
        ~RegistryGuard() { audio::sink_registry = saved; }
    };
} // namespace

TEST_CASE("audio::AudioSink::resample_s16 at equal rates copies the input", "[common][audio]")
{
    std::vector<int16_t> in = {1, -2, 300, -400, 5000, -6000, 32767, -32768};
    std::vector<int16_t> out(8, 0x5555);

    int n = audio::AudioSink::resample_s16(in.data(), out.data(), 48000, 48000, 8, 1);

    REQUIRE(n == 8);
    CHECK(out == in);
}

TEST_CASE("audio::AudioSink::resample_s16 decimates by two", "[common][audio]")
{
    std::vector<int16_t> in = {0, 10, 20, 30, 40, 50, 60, 70};
    std::vector<int16_t> out(4);

    int n = audio::AudioSink::resample_s16(in.data(), out.data(), 48000, 24000, 8, 1);

    REQUIRE(n == 4);
    CHECK(out == std::vector<int16_t>{0, 20, 40, 60});
}

TEST_CASE("audio::AudioSink::resample_s16 interpolates when upsampling", "[common][audio]")
{
    // The last output has no next input to interpolate towards and holds the last value
    std::vector<int16_t> in = {0, 100, 200, 300};
    std::vector<int16_t> out(8);

    int n = audio::AudioSink::resample_s16(in.data(), out.data(), 24000, 48000, 4, 1);

    REQUIRE(n == 8);
    CHECK(out == std::vector<int16_t>{0, 50, 100, 150, 200, 250, 300, 300});
}

TEST_CASE("audio::AudioSink::resample_s16 stays within the input and output buffers", "[common][audio]")
{
    // Heap buffers sized exactly, so ASan reports any access past either end
    const int rates[][2] = {{48000, 48000}, {48000, 24000}, {24000, 48000}, {48000, 44100}, {44100, 48000}, {8000, 48000}, {48000, 8000}};
    for (int channels = 1; channels <= 3; channels++)
    {
        for (auto &r : rates)
        {
            for (int inputSize = channels; inputSize <= 64 * channels; inputSize += channels)
            {
                std::unique_ptr<int16_t[]> in(new int16_t[inputSize]);
                for (int i = 0; i < inputSize; i++)
                    in[i] = 1000;
                int size = audio::AudioSink::resample_s16(in.get(), nullptr, r[0], r[1], inputSize, channels);
                std::unique_ptr<int16_t[]> out(new int16_t[size + 1]);
                out[size] = 0x5555;

                int n = audio::AudioSink::resample_s16(in.get(), out.get(), r[0], r[1], inputSize, channels);

                REQUIRE(n == size);
                REQUIRE(n % channels == 0);
                CHECK(out[size] == 0x5555);
                // Constant input interpolates to the same constant
                for (int i = 0; i < n; i++)
                    REQUIRE(out[i] == 1000);
            }
        }
    }
}

TEST_CASE("audio::AudioSink::resample_s16 keeps stereo channels apart", "[common][audio]")
{
    // Left rises, right falls
    std::vector<int16_t> in = {0, 300, 100, 200, 200, 100, 300, 0};
    std::vector<int16_t> out(16, 0x5555);

    int n = audio::AudioSink::resample_s16(in.data(), out.data(), 24000, 48000, 8, 2);

    REQUIRE(n == 16);
    CHECK(out == std::vector<int16_t>{0, 300, 50, 250, 100, 200, 150, 150, 200, 100, 250, 50, 300, 0, 300, 0});

    // Decimating stereo by two keeps every other frame
    std::vector<int16_t> out2(4, 0x5555);
    n = audio::AudioSink::resample_s16(in.data(), out2.data(), 48000, 24000, 8, 2);
    REQUIRE(n == 4);
    CHECK(out2 == std::vector<int16_t>{0, 300, 200, 100});
}

TEST_CASE("audio::AudioSink::resample_s16 rejects unusable arguments", "[common][audio]")
{
    int16_t in[2] = {1, 2};
    int16_t out[8] = {};
    CHECK(audio::AudioSink::resample_s16(in, out, 48000, 48000, 2, 0) == 0);
    CHECK(audio::AudioSink::resample_s16(in, out, 48000, 48000, 2, -1) == 0);
    // Less than one whole frame of input
    CHECK(audio::AudioSink::resample_s16(in, out, 8000, 48000, 1, 2) == 0);
    CHECK(audio::AudioSink::resample_s16(in, nullptr, 8000, 48000, 1, 2) == 0);
    CHECK(audio::AudioSink::resample_s16(in, out, 0, 48000, 2, 1) == 0);
    CHECK(audio::AudioSink::resample_s16(in, out, 48000, 0, 2, 1) == 0);
}

TEST_CASE("audio::AudioSink::resample_s16 output size computation", "[common][audio]")
{
    int16_t dummy = 0;
    CHECK(audio::AudioSink::resample_s16(nullptr, nullptr, 48000, 24000, 100, 1) == 0);
    CHECK(audio::AudioSink::resample_s16(&dummy, nullptr, 48000, 44100, 480, 1) == 441);
    CHECK(audio::AudioSink::resample_s16(&dummy, nullptr, 8000, 48000, 100, 1) == 600);
    // Rounded down to a whole number of channel frames
    CHECK(audio::AudioSink::resample_s16(&dummy, nullptr, 48000, 24000, 102, 2) == 50);
    CHECK(audio::AudioSink::resample_s16(&dummy, nullptr, 48000, 48000, 0, 1) == 0);
}

TEST_CASE("audio::get_default_sink falls back to a dummy sink", "[common][audio]")
{
    RegistryGuard guard;

    CHECK_FALSE(audio::has_sink());
    auto sink = audio::get_default_sink();
    REQUIRE(sink != nullptr);
    CHECK(dynamic_cast<TaggedSink *>(sink.get()) == nullptr);

    // The dummy sink accepts everything without side effects
    int16_t samples[4] = {1, 2, 3, 4};
    sink->set_samplerate(48000);
    sink->start();
    sink->push_samples(samples, 4);
    sink->stop();
}

TEST_CASE("audio::get_default_sink prefers portaudio over rtaudio", "[common][audio]")
{
    RegistryGuard guard;

    audio::sink_registry["rtaudio"] = []() { return std::make_shared<TaggedSink>("rtaudio"); };
    CHECK(audio::has_sink());
    auto sink = std::dynamic_pointer_cast<TaggedSink>(audio::get_default_sink());
    REQUIRE(sink != nullptr);
    CHECK(sink->tag == "rtaudio");

    audio::sink_registry["portaudio"] = []() { return std::make_shared<TaggedSink>("portaudio"); };
    sink = std::dynamic_pointer_cast<TaggedSink>(audio::get_default_sink());
    REQUIRE(sink != nullptr);
    CHECK(sink->tag == "portaudio");

    // Unknown sink names are not picked as default
    audio::sink_registry.clear();
    audio::sink_registry["other"] = []() { return std::make_shared<TaggedSink>("other"); };
    CHECK(audio::has_sink());
    CHECK(std::dynamic_pointer_cast<TaggedSink>(audio::get_default_sink()) == nullptr);
}
