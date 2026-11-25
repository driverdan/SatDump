#include "reshape_buffer.h"
#include "dsp/block_helpers.h"
#include <complex.h>
#include <cstring>

namespace satdump
{
    namespace ndsp
    {
        template <typename T>
        ReshapeBufferBlock<T>::ReshapeBufferBlock() : Block("reshape_buffer_" + getShortTypeName<T>() + getShortTypeName<T>(), {{"in", getTypeSampleType<T>()}}, {{"out", getTypeSampleType<T>()}})
        {
        }

        template <typename T>
        ReshapeBufferBlock<T>::~ReshapeBufferBlock()
        {
        }

        template <typename T>
        void ReshapeBufferBlock<T>::dropPending()
        {
            if (has_pending)
                outputs[0].fifo->free(pending);
            has_pending = false;
        }

        template <typename T>
        void ReshapeBufferBlock<T>::init()
        {
            this->apply_params([&]() { d_buffer_size = buffer_size; });
        }

        template <typename T>
        void ReshapeBufferBlock<T>::stop(bool stop_now, bool force)
        {
            Block::stop(stop_now, force);
            // The work thread has exited, samples left over are not carried over to the next run
            dropPending();
        }

        template <typename T>
        bool ReshapeBufferBlock<T>::work()
        {
            DSPBuffer iblk = inputs[0].fifo->wait_dequeue();

            if (iblk.isTerminator())
            {
                // An incomplete last buffer is never output
                dropPending();
                if (iblk.terminatorShouldPropagate())
                    outputs[0].fifo->wait_enqueue(outputs[0].fifo->newBufferTerminator());
                inputs[0].fifo->free(iblk);
                return true;
            }

            // On a new buffer size, the samples collected so far go out as they are
            const int old_size = d_buffer_size;
            if (reinit_if_needed() && d_buffer_size != old_size && has_pending)
            {
                outputs[0].fifo->wait_enqueue(pending);
                has_pending = false;
            }

            const T *in = iblk.getSamples<T>();
            uint32_t consumed = 0;
            while (consumed < iblk.size)
            {
                if (!has_pending)
                {
                    pending = outputs[0].fifo->newBufferSamples(d_buffer_size, sizeof(T));
                    pending.size = 0;
                    has_pending = true;
                }

                uint32_t n = std::min<uint32_t>(iblk.size - consumed, d_buffer_size - pending.size);
                memcpy(pending.getSamples<T>() + pending.size, in + consumed, n * sizeof(T));
                pending.size += n;
                consumed += n;

                if (pending.size == (uint32_t)d_buffer_size)
                {
                    outputs[0].fifo->wait_enqueue(pending);
                    has_pending = false;
                }
            }

            inputs[0].fifo->free(iblk);

            return false;
        }

        template class ReshapeBufferBlock<int8_t>;
        template class ReshapeBufferBlock<float>;
        template class ReshapeBufferBlock<complex_t>;
    } // namespace ndsp
} // namespace satdump
