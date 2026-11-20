// Dev tool: prints what the loopback capture is receiving, twice a second.
// Usage: capture_probe.exe [seconds]   (play something while it runs)
#include "audio/AudioCapture.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 8;
    auto capture = std::make_unique<AudioCapture>();
    capture->Start();
    std::vector<float> buf(4096);
    uint64_t last = 0;
    for (int i = 0; i < seconds * 2; ++i) {
        Sleep(500);
        capture->GetLatest(buf.data(), buf.size());
        float peak = 0;
        for (float s : buf) peak = std::fmax(peak, std::fabs(s));
        const uint64_t total = capture->TotalSamples();
        std::wprintf(L"%4.1fs  device=\"%ls\"  rate=%u  +%llu samples  peak=%.4f\n", (i + 1) * 0.5f,
            capture->DeviceName().c_str(), capture->SampleRate(), (unsigned long long)(total - last), peak);
        last = total;
    }
    return 0;
}
