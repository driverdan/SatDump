#include "hilbert.h"
#include <complex.h>

namespace satdump
{
    namespace ndsp
    {

        HilbertBlock::HilbertBlock() : Block("hilbert_fc", {{"in", DSP_SAMPLE_TYPE_F32}}, {{"out", DSP_SAMPLE_TYPE_CF32}}) {}

        HilbertBlock::~HilbertBlock()
        {
            if (taps != nullptr)
                volk_free(taps);

            if (buffer != nullptr)
                volk_free(buffer);
        }

        bool HilbertBlock::work()
        {
            DSPBuffer iblk = inputs[0].fifo->wait_dequeue();

            if (iblk.isTerminator())
            {
                if (iblk.terminatorShouldPropagate())
                    outputs[0].fifo->wait_enqueue(outputs[0].fifo->newBufferTerminator());
                inputs[0].fifo->free(iblk);
                return true;
            }

            reinit_if_needed();

            float *ibuf = iblk.getSamples<float>();

            lbuf_offset = 0;

            while (lbuf_offset < iblk.size)
            {
                // At most buffer_size samples at a time, and no more than are left
                lbuf_size = std::min<size_t>(buffer_size, iblk.size - lbuf_offset);

                DSPBuffer oblk = outputs[0].fifo->newBufferSamples(lbuf_size, sizeof(complex_t));
                complex_t *obuf = oblk.getSamples<complex_t>();

                memcpy(&buffer[d_ntaps], ibuf + lbuf_offset, lbuf_size * sizeof(float));

                for (int i = 0; i < lbuf_size; i++)
                {
                    volk_32f_x2_dot_prod_32f(&obuf[i].imag, &buffer[i], taps, d_ntaps);
                    obuf[i].real = buffer[i + d_ntaps / 2];
                }

                oblk.size = lbuf_size;
                outputs[0].fifo->wait_enqueue(oblk);

                memmove(&buffer[0], &buffer[lbuf_size], d_ntaps * sizeof(float));
                lbuf_offset += lbuf_size;
            }

            inputs[0].fifo->free(iblk);

            return false;
        }
    } // namespace ndsp
} // namespace satdump
