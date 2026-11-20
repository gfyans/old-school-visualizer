#pragma once
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

// Small in-place radix-2 complex FFT with precomputed tables. Size must be a power of two.
class Fft {
public:
    explicit Fft(int size = 0) { Resize(size); }

    void Resize(int size) {
        if (size == m_size) return;
        m_size = size;
        m_twiddle.resize(size / 2);
        m_bitReverse.resize(size);
        const float pi = 3.14159265358979f;
        for (int k = 0; k < size / 2; ++k) m_twiddle[k] = std::polar(1.0f, -2.0f * pi * k / size);
        int bits = 0;
        while ((1 << bits) < size) ++bits;
        for (int i = 0; i < size; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            m_bitReverse[i] = r;
        }
    }

    int Size() const { return m_size; }

    void Transform(std::complex<float>* a) const {
        for (int i = 0; i < m_size; ++i)
            if (i < m_bitReverse[i]) std::swap(a[i], a[m_bitReverse[i]]);
        for (int len = 2; len <= m_size; len <<= 1) {
            const int half = len >> 1, step = m_size / len;
            for (int i = 0; i < m_size; i += len) {
                for (int j = 0; j < half; ++j) {
                    const std::complex<float> v = a[i + j + half] * m_twiddle[j * step];
                    a[i + j + half] = a[i + j] - v;
                    a[i + j] += v;
                }
            }
        }
    }

    // Hann window of the given length.
    static std::vector<float> Hann(int size) {
        std::vector<float> w(size);
        for (int i = 0; i < size; ++i) w[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * i / (size - 1));
        return w;
    }

private:
    int m_size = 0;
    std::vector<std::complex<float>> m_twiddle;
    std::vector<int> m_bitReverse;
};
