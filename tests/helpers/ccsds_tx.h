#pragma once

/*
Transmit side of a CCSDS concatenated-coded BPSK link, shared by the whole
suite. Everything is inline and lives in the test_helpers namespace, as every
test file is linked into the same executable.

SatDump only has the receive side of such links, so the tests build their
input here, step by step in the order a spacecraft applies them:

- makeCadu(vcdu) adds the Reed-Solomon (255,223) I=4 dual basis check symbols
  to 892 bytes of VCDU, randomizes the result and prepends the ASM: a 1024
  byte CADU, as on the GOES-R HRIT downlink.
- unpackBits() / nrzmEncode() / convEncode() give the channel symbols of a
  byte stream: NRZ-M, then the rate 1/2 k=7 convolutional code (two symbols
  per bit, in the order and polarity viterbi::Viterbi1_2 expects).
- softSymbols() turns channel symbols into the int8 soft symbols a
  demodulator module writes (1 is positive).
- bpskModulate() shapes channel symbols with a root raised cosine pulse at any
  (also fractional) number of samples per symbol, with an optional carrier
  phase and frequency offset.
- toU8IQ() quantizes a baseband to the unsigned 8-bit I/Q of an RTL-SDR.
*/

#include "common/codings/randomization.h"
#include "common/codings/reedsolomon/reedsolomon.h"
#include "common/codings/viterbi/cc_encoder.h"
#include "common/dsp/complex.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace test_helpers
{
    constexpr int CADU_BYTES = 1024;     // ASM + VCDU + Reed-Solomon check symbols
    constexpr int CADU_VCDU_BYTES = 892; // 4 interleaved RS(255,223) messages

    // ASM + randomized (VCDU + Reed-Solomon check symbols)
    inline std::vector<uint8_t> makeCadu(const std::vector<uint8_t> &vcdu)
    {
        if (vcdu.size() != (size_t)CADU_VCDU_BYTES)
            throw std::runtime_error("makeCadu needs 892 bytes of VCDU");

        std::vector<uint8_t> cadu(CADU_BYTES, 0);
        cadu[0] = 0x1A, cadu[1] = 0xCF, cadu[2] = 0xFC, cadu[3] = 0x1D;
        std::copy(vcdu.begin(), vcdu.end(), cadu.begin() + 4);

        reedsolomon::ReedSolomon rs(reedsolomon::RS223);
        rs.encode_interlaved(&cadu[4], true, 4);

        derand_ccsds(&cadu[4], CADU_BYTES - 4); // An XOR, so it randomizes as well
        return cadu;
    }

    // One bit per byte, MSB first
    inline std::vector<uint8_t> unpackBits(const std::vector<uint8_t> &bytes)
    {
        std::vector<uint8_t> bits;
        bits.reserve(bytes.size() * 8);
        for (uint8_t b : bytes)
            for (int i = 7; i >= 0; i--)
                bits.push_back((b >> i) & 1);
        return bits;
    }

    // NRZ-M: a 1 is sent as a level change
    inline std::vector<uint8_t> nrzmEncode(const std::vector<uint8_t> &bits)
    {
        std::vector<uint8_t> out(bits.size());
        uint8_t level = 0;
        for (size_t i = 0; i < bits.size(); i++)
            out[i] = level = level ^ bits[i];
        return out;
    }

    // Rate 1/2, k=7 convolutional code: two channel symbols per bit
    inline std::vector<uint8_t> convEncode(const std::vector<uint8_t> &bits)
    {
        viterbi::CCEncoder enc(bits.size(), 7, 2, {79, 109});
        std::vector<uint8_t> symbols(bits.size() * 2);
        enc.work((uint8_t *)bits.data(), symbols.data());
        return symbols;
    }

    // Channel symbols of a byte stream (CADUs back to back), NRZ-M then convolutional coding
    inline std::vector<uint8_t> channelSymbols(const std::vector<uint8_t> &bytes) { return convEncode(nrzmEncode(unpackBits(bytes))); }

    // Soft symbols as written by a demodulator: +amplitude for a 1, -amplitude for a 0
    inline std::vector<int8_t> softSymbols(const std::vector<uint8_t> &symbols, int amplitude = 100)
    {
        std::vector<int8_t> soft(symbols.size());
        for (size_t i = 0; i < symbols.size(); i++)
            soft[i] = symbols[i] ? amplitude : -amplitude;
        return soft;
    }

    // Root raised cosine impulse response, t in symbol periods, h(0) normalized to 1
    inline double rrcPulse(double t, double alpha)
    {
        const double peak = 1.0 - alpha + 4.0 * alpha / M_PI;
        if (std::abs(t) < 1e-9)
            return 1.0;
        if (std::abs(std::abs(4.0 * alpha * t) - 1.0) < 1e-9)
            return (alpha / std::sqrt(2.0)) * ((1.0 + 2.0 / M_PI) * std::sin(M_PI / (4.0 * alpha)) + (1.0 - 2.0 / M_PI) * std::cos(M_PI / (4.0 * alpha))) / peak;
        double num = std::sin(M_PI * t * (1.0 - alpha)) + 4.0 * alpha * t * std::cos(M_PI * t * (1.0 + alpha));
        double den = M_PI * t * (1.0 - std::pow(4.0 * alpha * t, 2.0));
        return num / den / peak;
    }

    // BPSK baseband of these channel symbols (1 is +amplitude on I), root raised cosine shaped.
    // sps is samples per symbol, freq the carrier offset in cycles per sample.
    inline std::vector<complex_t> bpskModulate(const std::vector<uint8_t> &symbols, double sps, double alpha, float amplitude = 1.0f, double phase = 0, double freq = 0)
    {
        const int span = 8; // Symbols of pulse kept on each side
        size_t count = (size_t)std::floor(symbols.size() * sps);
        std::vector<complex_t> out(count);
        for (size_t n = 0; n < count; n++)
        {
            double t = n / sps;
            long center = std::lround(t);
            double v = 0;
            for (long k = std::max<long>(0, center - span); k <= std::min<long>((long)symbols.size() - 1, center + span); k++)
                v += (symbols[k] ? 1.0 : -1.0) * rrcPulse(t - (double)k, alpha);
            double rot = phase + 2.0 * M_PI * freq * (double)n;
            out[n] = complex_t(amplitude * v * std::cos(rot), amplitude * v * std::sin(rot));
        }
        return out;
    }

    // Unsigned 8-bit I/Q, as an RTL-SDR (and rtl_tcp) outputs it
    inline std::vector<uint8_t> toU8IQ(const std::vector<complex_t> &samples)
    {
        auto quantize = [](float v) { return (uint8_t)std::clamp(std::lround(v * 128.0f + 127.4f), 0L, 255L); };
        std::vector<uint8_t> out(samples.size() * 2);
        for (size_t i = 0; i < samples.size(); i++)
        {
            out[i * 2 + 0] = quantize(samples[i].real);
            out[i * 2 + 1] = quantize(samples[i].imag);
        }
        return out;
    }
} // namespace test_helpers
