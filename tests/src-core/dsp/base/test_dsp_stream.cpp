#include "dsp/base/dsp_buffer.h"
#include "dsp/base/stream.h"
#include "dsp/ndsp_test_helpers.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

using namespace satdump::ndsp;

TEST_CASE("ndsp DSPBuffer defaults to an invalid, empty buffer", "[dsp][base][dsp_buffer]")
{
    DSPBuffer b;
    CHECK(b.type == DSP_BUFFER_TYPE_INVALID);
    CHECK(b.size == 0);
    CHECK(b.max_size == 0);
    CHECK(b.typesize == 0);
    CHECK(b.getSamples<float>() == nullptr);
    CHECK_FALSE(b.isTerminator());
    CHECK_FALSE(b.terminatorShouldPropagate());
}

TEST_CASE("ndsp DSPBuffer terminator flags", "[dsp][base][dsp_buffer]")
{
    DSPBuffer b;
    b.type = DSP_BUFFER_TYPE_TERMINATOR_PROPAGATING;
    CHECK(b.isTerminator());
    CHECK(b.terminatorShouldPropagate());

    b.type = DSP_BUFFER_TYPE_TERMINATOR_NON_PROPAGATING;
    CHECK(b.isTerminator());
    CHECK_FALSE(b.terminatorShouldPropagate());

    b.type = DSP_BUFFER_TYPE_SAMPLES;
    CHECK_FALSE(b.isTerminator());
    CHECK_FALSE(b.terminatorShouldPropagate());
}

TEST_CASE("ndsp volk_alignment matches VOLK", "[dsp][base][dsp_buffer]")
{
    CHECK(volk_alignment == volk_get_alignment());
    CHECK(volk_alignment > 0);
}

TEST_CASE("ndsp DSPStream reports its capacity and starts empty", "[dsp][base][dsp_stream]")
{
    DSPStream s(5);
    CHECK(s.max_capacity() == 5);
    CHECK(s.size_approx() == 0);
    DSPBuffer b;
    CHECK_FALSE(s.try_dequeue(b));
}

TEST_CASE("ndsp DSPStream newBufferSamples allocates aligned sample buffers", "[dsp][base][dsp_stream]")
{
    DSPStream s(2);
    DSPBuffer b = s.newBufferSamples(100, sizeof(complex_t));
    CHECK(b.type == DSP_BUFFER_TYPE_SAMPLES);
    CHECK(b.max_size == 100);
    CHECK(b.size == 0);
    REQUIRE(b.getSamples<complex_t>() != nullptr);
    CHECK((uintptr_t)b.getSamples<void>() % volk_get_alignment() == 0);

    // The whole buffer is usable
    complex_t *p = b.getSamples<complex_t>();
    for (int i = 0; i < 100; i++)
        p[i] = complex_t(i, -i);
    CHECK(p[99].imag == -99.0f);
    s.free(b);
}

TEST_CASE("ndsp DSPStream passes buffers in FIFO order", "[dsp][base][dsp_stream]")
{
    DSPStream s(3);
    for (int i = 0; i < 3; i++)
    {
        DSPBuffer b = s.newBufferSamples(1, sizeof(int32_t));
        b.getSamples<int32_t>()[0] = i * 11;
        b.size = 1;
        s.wait_enqueue(b);
    }
    CHECK(s.size_approx() == 3);
    for (int i = 0; i < 3; i++)
    {
        DSPBuffer b = s.wait_dequeue();
        CHECK(b.type == DSP_BUFFER_TYPE_SAMPLES);
        CHECK(b.size == 1);
        CHECK(b.getSamples<int32_t>()[0] == i * 11);
        s.free(b);
    }
    CHECK(s.size_approx() == 0);
}

TEST_CASE("ndsp DSPStream recycles freed buffers", "[dsp][base][dsp_stream]")
{
    DSPStream s(1);
    DSPBuffer a = s.newBufferSamples(64, sizeof(float));
    void *pa = a.getSamples<void>();
    s.free(a);

    // Same or smaller size reuses the allocation
    DSPBuffer b = s.newBufferSamples(16, sizeof(float));
    CHECK(b.getSamples<void>() == pa);
    CHECK(b.max_size == 16);
    s.free(b);

    // Larger size reallocates
    DSPBuffer c = s.newBufferSamples(4096, sizeof(float));
    REQUIRE(c.getSamples<float>() != nullptr);
    c.getSamples<float>()[4095] = 1.0f;
    CHECK((uintptr_t)c.getSamples<void>() % volk_get_alignment() == 0);
    s.free(c);

    // And it is kept at the larger size afterwards
    DSPBuffer d = s.newBufferSamples(4096, sizeof(float));
    CHECK(d.getSamples<void>() == c.getSamples<void>());
    s.free(d);
}

TEST_CASE("ndsp DSPStream newBufferTerminator sets the terminator type", "[dsp][base][dsp_stream]")
{
    DSPStream s(2);
    DSPBuffer t1 = s.newBufferTerminator();
    CHECK(t1.type == DSP_BUFFER_TYPE_TERMINATOR_PROPAGATING);
    CHECK(t1.isTerminator());
    s.wait_enqueue(t1);

    DSPBuffer t2 = s.newBufferTerminator(false);
    CHECK(t2.type == DSP_BUFFER_TYPE_TERMINATOR_NON_PROPAGATING);
    s.wait_enqueue(t2);

    DSPBuffer r1 = s.wait_dequeue();
    DSPBuffer r2 = s.wait_dequeue();
    CHECK(r1.terminatorShouldPropagate());
    CHECK_FALSE(r2.terminatorShouldPropagate());
    CHECK(r2.isTerminator());
    s.free(r1);
    s.free(r2);
}

TEST_CASE("ndsp DSPStream allocation waits for a free buffer", "[dsp][base][dsp_stream]")
{
    auto s = std::make_shared<DSPStream>(1);
    DSPBuffer held = s->newBufferSamples(8, sizeof(float));

    // The pool is empty, so this has to wait until 'held' is freed
    auto fut = std::async(std::launch::async, [s]() { return s->newBufferSamples(8, sizeof(float)); });
    CHECK(fut.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

    s->free(held);
    REQUIRE(fut.wait_for(ndsp_test::kTimeout) == std::future_status::ready);
    DSPBuffer got = fut.get();
    CHECK(got.getSamples<void>() == held.getSamples<void>());
    s->free(got);
}

TEST_CASE("ndsp DSPStream blocking dequeue wakes up on enqueue", "[dsp][base][dsp_stream]")
{
    auto s = std::make_shared<DSPStream>(2);
    auto fut = std::async(std::launch::async,
                          [s]()
                          {
                              DSPBuffer b = s->wait_dequeue();
                              float v = b.getSamples<float>()[0];
                              s->free(b);
                              return v;
                          });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ndsp_test::pushSamples(*s, std::vector<float>{42.5f});
    REQUIRE(fut.wait_for(ndsp_test::kTimeout) == std::future_status::ready);
    CHECK(fut.get() == 42.5f);
}

TEST_CASE("ndsp DSPStream moves many buffers between two threads intact", "[dsp][base][dsp_stream]")
{
    auto s = std::make_shared<DSPStream>(3);
    const int nbufs = 500;
    std::vector<uint32_t> in;
    for (int i = 0; i < nbufs * 7; i++)
        in.push_back(i * 2654435761u);

    ndsp_test::Collector<uint32_t> col(s);
    for (int i = 0; i < nbufs; i++)
        ndsp_test::pushSamples<uint32_t>(*s, &in[i * 7], 7);
    ndsp_test::pushTerminator(*s);
    REQUIRE(col.waitForTerminator());
    col.finish();
    CHECK(col.data() == in);
    CHECK(col.sizes().size() == (size_t)nbufs);
}

TEST_CASE("ndsp DSPStream destructor frees queued buffers", "[dsp][base][dsp_stream]")
{
    // Nothing to assert directly, but this must neither crash nor leak under sanitizers
    auto s = std::make_unique<DSPStream>(4);
    ndsp_test::pushSamples(*s, std::vector<float>(1000, 1.0f));
    ndsp_test::pushSamples(*s, std::vector<float>(10, 2.0f));
    ndsp_test::pushTerminator(*s);
    CHECK(s->size_approx() == 3);
    s.reset();
    SUCCEED();
}

namespace
{
    // Runs fn on a detached thread, so that a thread stuck on a lost buffer
    // makes the test fail on a timeout instead of hanging in a future's destructor
    template <typename F>
    auto runDetached(F fn) -> std::future<decltype(fn())>
    {
        auto pr = std::make_shared<std::promise<decltype(fn())>>();
        auto fut = pr->get_future();
        std::thread([pr, fn]() { pr->set_value(fn()); }).detach();
        return fut;
    }
} // namespace

TEST_CASE("ndsp DSPStream free from producer and consumer at once loses no buffer", "[dsp][base][dsp_stream]")
{
    // Blocks drop buffers they allocated but never sent with free() on their
    // output stream, while the downstream block frees the buffers it used: two
    // threads enqueue into the free pool at once (#439)
    constexpr int nbuf = 2;
    constexpr int nloops = 20000;
    constexpr int nsamples = 16;
    auto s = std::make_shared<DSPStream>(nbuf);

    auto consumer = runDetached(
        [s]()
        {
            int bad = 0;
            int received = 0;
            uint32_t last = 0;
            while (true)
            {
                DSPBuffer b = s->wait_dequeue();
                if (b.isTerminator())
                {
                    s->free(b);
                    break;
                }
                uint32_t *p = b.getSamples<uint32_t>();
                // Each buffer holds its own sequence number, in increasing order
                if (b.size != nsamples || p[0] <= last)
                    bad++;
                for (int i = 1; i < nsamples; i++)
                    if (p[i] != p[0])
                        bad++;
                last = p[0];
                received++;
                s->free(b);
            }
            return std::make_pair(received, bad);
        });

    auto producer = runDetached(
        [s]()
        {
            int sent = 0;
            for (int i = 1; i <= nloops; i++)
            {
                DSPBuffer b = s->newBufferSamples(nsamples, sizeof(uint32_t));
                if (i % 2 == 0)
                {
                    // Dropped on the producer side, like ReshapeBufferBlock or BlockSimple do
                    s->free(b);
                    continue;
                }
                uint32_t *p = b.getSamples<uint32_t>();
                for (int j = 0; j < nsamples; j++)
                    p[j] = i;
                b.size = nsamples;
                s->wait_enqueue(b);
                sent++;
            }
            s->wait_enqueue(s->newBufferTerminator());
            return sent;
        });

    REQUIRE(producer.wait_for(std::chrono::seconds(60)) == std::future_status::ready);
    REQUIRE(consumer.wait_for(std::chrono::seconds(60)) == std::future_status::ready);
    int sent = producer.get();
    auto [received, bad] = consumer.get();
    CHECK(sent == nloops / 2);
    CHECK(received == sent);
    CHECK(bad == 0);

    // Every buffer is back in the pool exactly once: all nbuf of them can be
    // allocated again, and they are all distinct
    auto drain = runDetached(
        [s]()
        {
            std::vector<DSPBuffer> bufs;
            for (int i = 0; i < nbuf; i++)
                bufs.push_back(s->newBufferSamples(nsamples, sizeof(uint32_t)));
            return bufs;
        });
    REQUIRE(drain.wait_for(ndsp_test::kTimeout) == std::future_status::ready);
    std::vector<DSPBuffer> bufs = drain.get();
    for (int i = 0; i < nbuf; i++)
    {
        REQUIRE(bufs[i].getSamples<void>() != nullptr);
        for (int j = 0; j < i; j++)
            CHECK(bufs[i].getSamples<void>() != bufs[j].getSamples<void>());
    }
    for (auto &b : bufs)
        s->free(b);
}
