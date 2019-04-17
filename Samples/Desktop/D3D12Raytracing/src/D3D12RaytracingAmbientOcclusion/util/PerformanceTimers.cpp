//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "../stdafx.h"
#include "PerformanceTimers.h"

#ifndef IID_GRAPHICS_PPV_ARGS
#define IID_GRAPHICS_PPV_ARGS(x) IID_PPV_ARGS(x)
#endif

#include <exception>
#include <stdexcept>

using namespace DirectX;
using namespace DX;

using Microsoft::WRL::ComPtr;

namespace
{
    inline float lerp(float a, float b, float f)
    {
        return (1.f - f) * a + f * b;
    }

    inline float UpdateRunningAverage(float avg, float value)
    {
        return avg >= 0.0001f ? lerp(avg, value, 0.05f) : value;
    }

    inline void DebugWarnings(uint32_t timerid, uint64_t start, uint64_t end)
    {
#if defined(_DEBUG)
        if (!start && end > 0)
        {
            char buff[128] = {};
            sprintf_s(buff, "ERROR: Timer %u stopped but not started\n", timerid);
            OutputDebugStringA(buff);
        }
        else if (start > 0 && !end)
        {
            char buff[128] = {};
            sprintf_s(buff, "ERROR: Timer %u started but not stopped\n", timerid);
            OutputDebugStringA(buff);
        }
#else
        UNREFERENCED_PARAMETER(timerid);
        UNREFERENCED_PARAMETER(start);
        UNREFERENCED_PARAMETER(end);
#endif
    }
};

//======================================================================================
// CPUTimer
//======================================================================================

CPUTimer::CPUTimer() :
    m_cpuFreqInv(1.f),
    m_start{},
    m_end{},
    m_avg{}
{
    LARGE_INTEGER cpuFreq;
    if (!QueryPerformanceFrequency(&cpuFreq))
    {
        throw std::exception("QueryPerformanceFrequency");
    }

    m_cpuFreqInv = 1000.0 / double(cpuFreq.QuadPart);
}

void CPUTimer::Start(uint32_t timerid)
{
    if (timerid >= c_maxTimers)
        throw std::out_of_range("Timer ID out of range");

    if (!QueryPerformanceCounter(&m_start[timerid]))
    {
        throw std::exception("QueryPerformanceCounter");
    }
}

void CPUTimer::Stop(uint32_t timerid)
{
    if (timerid >= c_maxTimers)
        throw std::out_of_range("Timer ID out of range");

    if (!QueryPerformanceCounter(&m_end[timerid]))
    {
        throw std::exception("QueryPerformanceCounter");
    }
}

void CPUTimer::Update()
{
    for (uint32_t j = 0; j < c_maxTimers; ++j)
    {
        uint64_t start = m_start[j].QuadPart;
        uint64_t end = m_end[j].QuadPart;

        DebugWarnings(j, start, end);

        float value = float(double(end - start) * m_cpuFreqInv);
        m_avg[j] = UpdateRunningAverage(m_avg[j], value);
    }
}

void CPUTimer::Reset()
{
    memset(m_avg, 0, sizeof(m_avg));
}

float CPUTimer::GetElapsedMS(uint32_t timerid) const
{
    if (timerid >= c_maxTimers)
        return 0.0;

    uint64_t start = m_start[timerid].QuadPart;
    uint64_t end = m_end[timerid].QuadPart;

    return static_cast<float>(double(end - start) * m_cpuFreqInv);
}


//======================================================================================
// GPUTimer (DirectX 12)
//======================================================================================

void GPUTimer::BeginFrame(_In_ ID3D12GraphicsCommandList5* commandList)
{
    UNREFERENCED_PARAMETER(commandList);
}

void GPUTimer::EndFrame(_In_ ID3D12GraphicsCommandList5* commandList)
{
    // Resolve query for the current frame.
    static UINT resolveToFrameID = 0;
    UINT64 resolveToBaseAddress = resolveToFrameID * c_timerSlots * sizeof(UINT64);
    commandList->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, c_timerSlots, m_buffer.Get(), resolveToBaseAddress);

    // Grab read-back data for the queries from a finished frame m_maxframeCount ago.                                                           
    UINT readBackFrameID = (resolveToFrameID + 1) % (m_maxframeCount + 1);
    SIZE_T readBackBaseOffset = readBackFrameID * c_timerSlots * sizeof(UINT64);
    D3D12_RANGE dataRange =
    {
        readBackBaseOffset,
        readBackBaseOffset + c_timerSlots * sizeof(UINT64),
    };

    UINT64* timingData;
    ThrowIfFailed(m_buffer->Map(0, &dataRange, reinterpret_cast<void**>(&timingData)));
    memcpy(m_timing, timingData, sizeof(UINT64) * c_timerSlots);
    m_buffer->Unmap(0, &CD3DX12_RANGE(0, 0));

    for (uint32_t j = 0; j < c_maxTimers; ++j)
    {
        UINT64 start = m_timing[j * 2];
        UINT64 end = m_timing[j * 2 + 1];

        DebugWarnings(j, start, end);

        float value = float(double(end - start) * m_gpuFreqInv);
		m_avgPeriodTotal[j] += value;
    }
	m_avgTimestampsTotal++;

	// Update averages if the period duration has passed.
	m_avgPeriodTimer.Stop();
	float elapsedMs = m_avgPeriodTimer.GetElapsedMS();
	if (m_avgPeriodTimer.GetElapsedMS() >= m_avgRefreshPeriodMs)
	{
		for (uint32_t j = 0; j < c_maxTimers; ++j)
		{
			m_avg[j] = m_avgPeriodTotal[j] / m_avgTimestampsTotal;
			m_avgPeriodTotal[j] = 0;
		}
		m_avgTimestampsTotal = 0;

		m_avgPeriodTimer.Start();
	}
	else
	{
		elapsedMs;
	}

    resolveToFrameID = readBackFrameID;
}

void GPUTimer::Start(_In_ ID3D12GraphicsCommandList5* commandList, uint32_t timerid)
{
    if (timerid >= c_maxTimers)
        throw std::out_of_range("Timer ID out of range");

    commandList->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timerid * 2);
}

void GPUTimer::Stop(_In_ ID3D12GraphicsCommandList5* commandList, uint32_t timerid)
{
    if (timerid >= c_maxTimers)
        throw std::out_of_range("Timer ID out of range");

    commandList->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timerid * 2 + 1);
}

void GPUTimer::Reset()
{
    memset(m_avg, 0, sizeof(m_avg));
	memset(m_avgPeriodTotal, 0, sizeof(m_avg));
	m_avgTimestampsTotal = 0;
	m_avgPeriodTimer.Reset();
	m_avgPeriodTimer.Start();
}

float GPUTimer::GetElapsedMS(uint32_t timerid) const
{
    if (timerid >= c_maxTimers)
        return 0.0;
 
    UINT64 start = m_timing[timerid * 2];
    UINT64 end = m_timing[timerid * 2 + 1];

    if (end < start)
        return 0.0;

    return static_cast<float>(double(end - start) * m_gpuFreqInv);
}

void GPUTimer::ReleaseDevice()
{
    m_heap.Reset();
    m_buffer.Reset();
}

void GPUTimer::RestoreDevice(_In_ ID3D12Device5* device, _In_ ID3D12CommandQueue* commandQueue, UINT maxFrameCount)
{
    assert(device != 0 && commandQueue != 0);
    m_maxframeCount = maxFrameCount;

    // Filter out a debug warning coming when accessing a readback resource for the timing queries.
    // The readback resource handles multiple frames data via per-frame offsets within the same resource and CPU
    // maps an offset written "frame_count" frames ago and the data is guaranteed to had been written to by GPU by this time. 
    // Therefore the race condition doesn't apply in this case.
    ComPtr<ID3D12InfoQueue> d3dInfoQueue;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3dInfoQueue))))
    {
        // Suppress individual messages by their ID.
        D3D12_MESSAGE_ID denyIds[] =
        {
            D3D12_MESSAGE_ID_EXECUTECOMMANDLISTS_GPU_WRITTEN_READBACK_RESOURCE_MAPPED,	// ToDo this still spews sometimes...
        };

        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = _countof(denyIds);
        filter.DenyList.pIDList = denyIds;
        d3dInfoQueue->AddStorageFilterEntries(&filter);
		// ToDo this spews 3 times. remove duplicate debug spews
        OutputDebugString(L"Warning: GPUTimer is disabling an unwanted D3D12 debug layer warning: D3D12_MESSAGE_ID_EXECUTECOMMANDLISTS_GPU_WRITTEN_READBACK_RESOURCE_MAPPED.\n");
    }


    UINT64 gpuFreq;
    ThrowIfFailed(commandQueue->GetTimestampFrequency(&gpuFreq));
    m_gpuFreqInv = 1000.0 / double(gpuFreq);

    D3D12_QUERY_HEAP_DESC desc = {};
    desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count = c_timerSlots;
    ThrowIfFailed(device->CreateQueryHeap(&desc, IID_GRAPHICS_PPV_ARGS(&m_heap)));
    m_heap->SetName(L"GPUTimerHeap");

    // We allocate m_maxframeCount + 1 instances as an instance is guaranteed to be written to if maxPresentFrameCount frames
    // have been submitted since. This is due to a fact that Present stalls when none of the m_maxframeCount frames are done/available.
    size_t nPerFrameInstances = m_maxframeCount + 1;

    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(nPerFrameInstances * c_timerSlots * sizeof(UINT64));
    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_GRAPHICS_PPV_ARGS(&m_buffer))
    );
    m_buffer->SetName(L"GPUTimerBuffer");
}

