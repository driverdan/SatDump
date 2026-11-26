#pragma once

#include "dsp_buffer.h"
#include "readerwritercircularbuffer.h"
#include <mutex>

namespace satdump
{
    namespace ndsp
    {
        class DSPStream
        {
        private:
            moodycamel::BlockingReaderWriterCircularBuffer<DSPBuffer> fifo;
            moodycamel::BlockingReaderWriterCircularBuffer<DSPBuffer> ufifo;

            // ufifo is single-producer, but free() is called both by the consumer
            // (returning a used buffer) and by the producer (dropping a buffer it
            // allocated but never sent). This serializes those enqueues.
            std::mutex ufifo_free_mtx;

        public:
            DSPStream(int size) : fifo(size), ufifo(size)
            {
                for (int i = 0; i < size; i++)
                {
                    DSPBuffer b;
                    ufifo.wait_enqueue(b);
                }
            }

            ~DSPStream()
            {
                DSPBuffer b;
                while (fifo.try_dequeue(b))
                    if (b.ptr != nullptr)
                        volk_free(b.ptr);
                while (ufifo.try_dequeue(b))
                    if (b.ptr != nullptr)
                        volk_free(b.ptr);
            }

            inline void wait_enqueue(DSPBuffer b) { fifo.wait_enqueue(b); }

            inline DSPBuffer wait_dequeue()
            {
                DSPBuffer b;
                fifo.wait_dequeue(b);
                return b;
            }

            inline bool try_dequeue(DSPBuffer &b) { return fifo.try_dequeue(b); }

            DSPBuffer alloc(size_t size)
            {
                DSPBuffer b;
                ufifo.wait_dequeue(b);
                if (b.ptr == nullptr)
                {
                    b.ptr = volk_malloc(size, volk_get_alignment());
                    b.ptr_size = size;
                    // printf("Alloc\n");
                }
                else if (b.ptr_size < size)
                {
                    volk_free(b.ptr);
                    b.ptr = volk_malloc(size, volk_get_alignment());
                    b.ptr_size = size;
                    // printf("Realloc\n");
                }
                return b;
            }

            inline void free(DSPBuffer &b)
            {
                // Never blocks while holding the lock: there are only ever as many
                // buffers as ufifo has slots, so it has room for any buffer out of it
                std::lock_guard<std::mutex> lock(ufifo_free_mtx);
                ufifo.wait_enqueue(b);
            }

            size_t size_approx() { return fifo.size_approx(); }
            size_t max_capacity() { return fifo.max_capacity(); }

        public:
            DSPBuffer newBufferTerminator(bool prop = true)
            {
                DSPBuffer b = alloc(0);
                b.type = prop ? DSP_BUFFER_TYPE_TERMINATOR_PROPAGATING : DSP_BUFFER_TYPE_TERMINATOR_NON_PROPAGATING;
                return b;
            }

            DSPBuffer newBufferSamples(uint32_t size, int typesize)
            {
                DSPBuffer b = alloc(size * typesize);
                b.type = DSP_BUFFER_TYPE_SAMPLES;
                b.max_size = size;
                return b;
            }
        };
    } // namespace ndsp
} // namespace satdump