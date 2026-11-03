// OouraFFT: Ooura split-radix FFT/IFFT implementation wrapper.
// Realtime-safety: init() allocates; fft/ifft use pre-allocated buffers only

#pragma once
#include <complex>
#include <vector>
#include <cmath>

// Wrapper around Takuya Ooura's split-radix FFT, used under its permissive licence.
// http://www.kurims.kyoto-u.ac.jp/~ooura/fft.html
//
// NORMALISATION CONTRACT (verified against Ooura's own usage notes):
//   fft()  is unnormalised: out[k] = sum_n in[n] * exp(-2*pi*i*n*k/N).
//   ifft() IS normalised:   ifft(fft(x)) == x exactly.
// Ooura's raw rdft(n,-1) returns (n/2)*x and requires a 2/n scaling; that
// scaling lives inside ifft(). Do NOT apply an extra 1/N (or any other)
// factor at the call site.
class OouraFFT
{
public:
    void init(size_t nfft);
    void fft(float* in, std::complex<float>* out);
    void ifft(std::complex<float>* in, float* out);
    size_t getNfft() const { return nfft; }

private:
    void rdft(int n, int isgn, double* a, int* ip, double* w);
    void makewt(int nw, int* ip, double* w);
    void makect(int nc, int* ip, double* c);
    void bitrv2(int n, int* ip, double* a);
    void cftfsub(int n, double* a, double* w);
    void cftbsub(int n, double* a, double* w);
    void rftfsub(int n, double* a, int nc, double* c);
    void rftbsub(int n, double* a, int nc, double* c);
    void cft1st(int n, double* a, double* w);
    void cftmdl(int n, int l, double* a, double* w);

    std::vector<int> ip_;
    std::vector<double> sineTable_;
    std::vector<double> buffer_;
    size_t nfft = 0u;
};

inline bool isPowerOf2(size_t n)
{
    return n && !(n & (n - 1));
}
