#include "render/GpuTimer.h"

bool GpuTimer::Create(ID3D11Device* device) {
    Reset();
    D3D11_QUERY_DESC dj{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC ts{ D3D11_QUERY_TIMESTAMP, 0 };
    for (Set& s : m_sets) {
        if (FAILED(device->CreateQuery(&dj, &s.disjoint)) ||
            FAILED(device->CreateQuery(&ts, &s.begin)) ||
            FAILED(device->CreateQuery(&ts, &s.end))) {
            Reset();
            return false;
        }
    }
    return true;
}

void GpuTimer::Reset() {
    for (Set& s : m_sets) {
        s.disjoint.Reset();
        s.begin.Reset();
        s.end.Reset();
        s.pending = false;
    }
    m_write = 0;
    m_active = false;
    m_hasValue = false;
    m_ms = 0.0f;
}

void GpuTimer::Begin(ID3D11DeviceContext* ctx) {
    Set& s = m_sets[m_write];
    m_active = s.disjoint && !s.pending;   // all sets still in flight: skip measuring this frame
    if (!m_active) return;
    ctx->Begin(s.disjoint.Get());
    ctx->End(s.begin.Get());
}

void GpuTimer::End(ID3D11DeviceContext* ctx) {
    if (!m_active) return;
    Set& s = m_sets[m_write];
    ctx->End(s.end.Get());
    ctx->End(s.disjoint.Get());
    s.pending = true;
    m_write = (m_write + 1) % kInFlight;
    m_active = false;
}

void GpuTimer::Collect(ID3D11DeviceContext* ctx) {
    for (Set& s : m_sets) {
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t0, t1;
        constexpr UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
        if (ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), flags) != S_OK ||
            ctx->GetData(s.begin.Get(), &t0, sizeof(t0), flags) != S_OK ||
            ctx->GetData(s.end.Get(), &t1, sizeof(t1), flags) != S_OK)
            continue;
        s.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;
        const float ms = float(double(t1 - t0) * 1000.0 / double(dj.Frequency));
        m_ms = m_hasValue ? m_ms + (ms - m_ms) * 0.1f : ms;
        m_hasValue = true;
    }
}
