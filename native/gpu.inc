// Read display-driver counters without creating a graphics device or loading a vendor runtime.
#include <d3dkmthk.h>
#include <chrono>

class GpuCounters
{
    D3DKMT_ADAPTERINFO adapter{};
    unsigned segments = 0;
    std::vector<long long> lastRunning;
    ULONGLONG capacity = 0;
    std::chrono::steady_clock::time_point lastSample{};
    static void close(D3DKMT_HANDLE h)
    {
        if (h)
        {
            D3DKMT_CLOSEADAPTER args{h};
            auto status = D3DKMTCloseAdapter(&args);
            (void)status;
        }
    }

  public:
    GpuCounters()
    {
        D3DKMT_ENUMADAPTERS all{};
        if (D3DKMTEnumAdapters(&all) < 0)
            return;
        for (UINT i = 0; i < all.NumAdapters; i++)
        {
            auto item = all.Adapters[i];
            D3DKMT_SEGMENTSIZEINFO size{};
            D3DKMT_QUERYADAPTERINFO info{item.hAdapter, KMTQAITYPE_GETSEGMENTSIZE, &size, sizeof(size)};
            D3DKMT_QUERYSTATISTICS q{};
            q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
            q.AdapterLuid = item.AdapterLuid;
            if (D3DKMTQueryAdapterInfo(&info) >= 0 && size.DedicatedVideoMemorySize > capacity &&
                D3DKMTQueryStatistics(&q) >= 0)
            {
                close(adapter.hAdapter);
                adapter = item;
                capacity = size.DedicatedVideoMemorySize;
                segments = q.QueryResult.AdapterInformation.NbSegments;
                lastRunning.assign(q.QueryResult.AdapterInformation.NodeCount, 0);
            }
            else
                close(item.hAdapter);
        }
    }
    ~GpuCounters()
    {
        close(adapter.hAdapter);
    }
    bool read(double *v)
    {
        if (!adapter.hAdapter)
            return false;
        D3DKMT_QUERYSTATISTICS q{};
        q.AdapterLuid = adapter.AdapterLuid;
        q.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
        ULONGLONG resident = 0;
        bool available = false;
        for (unsigned i = 0; i < segments; i++)
        {
            q.QuerySegment.SegmentId = i;
            if (D3DKMTQueryStatistics(&q) >= 0 && !q.QueryResult.SegmentInformation.Aperture)
            {
                resident += q.QueryResult.SegmentInformation.BytesResident;
                available = true;
            }
        }
        if (!available)
            return false;
        v[3] = double(resident);
        v[4] = double(capacity);
        auto now = std::chrono::steady_clock::now();
        double interval = std::chrono::duration<double>(now - lastSample).count() * 10000000.;
        double busy = 0;
        q.Type = D3DKMT_QUERYSTATISTICS_NODE;
        for (unsigned i = 0; i < lastRunning.size(); i++)
        {
            q.QueryNode.NodeId = i;
            if (D3DKMTQueryStatistics(&q) < 0)
                continue;
            auto current = q.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;
            if (lastSample.time_since_epoch().count() && interval > 0)
                busy = std::max(busy, 100. * (current - lastRunning[i]) / interval);
            lastRunning[i] = current;
        }
        lastSample = now;
        v[5] = std::clamp(busy, 0., 100.);
        D3DKMT_ADAPTER_PERFDATA perf{};
        D3DKMT_QUERYADAPTERINFO info{adapter.hAdapter, KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof(perf)};
        v[6] = D3DKMTQueryAdapterInfo(&info) >= 0 && perf.Temperature > 0 ? perf.Temperature / 10. : -1.;
        return true;
    }
};
