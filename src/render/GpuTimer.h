#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// Measures GPU time for a span of commands using timestamp queries. Results are
// read a few frames later without ever stalling the CPU.
class GpuTimer {
public:
    bool Create(ID3D11Device* device);
    void Reset();

    void Begin(ID3D11DeviceContext* ctx);
    void End(ID3D11DeviceContext* ctx);
    void Collect(ID3D11DeviceContext* ctx);   // call once per frame

    float Milliseconds() const { return m_ms; }   // smoothed; 0 until the first result
    bool HasValue() const { return m_hasValue; }

private:
    static constexpr int kInFlight = 4;
    struct Set {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    Set m_sets[kInFlight];
    int m_write = 0;
    bool m_active = false;
    bool m_hasValue = false;
    float m_ms = 0.0f;
};
