#include "effects_pipeline.h"
#include <stdexcept>
#include <iostream>

namespace EffectsPipeline {

    void upload_frame(
        DirectMLContext& context,
        const float* cpuInputRGBA,
        ID3D12Resource* gpuTexture,
        uint64_t sizeInBytes
    ) {
        ID3D12Device* device = context.GetD3D12Device();
        
        static ComPtr<ID3D12Resource> uploadBuffer;
        static uint64_t uploadBufferSize = 0;
        
        if (!uploadBuffer || uploadBufferSize < sizeInBytes) {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProps.CreationNodeMask = 1;
            heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Alignment = 0;
            desc.Width = sizeInBytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            HRESULT hr = device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&uploadBuffer)
            );
            if (FAILED(hr)) {
                throw std::runtime_error("upload_frame failed to create upload buffer. hr = " + std::to_string(hr));
            }
            uploadBufferSize = sizeInBytes;
        }

        // Map and copy CPU data to upload buffer
        void* mappedData = nullptr;
        HRESULT hr = uploadBuffer->Map(0, nullptr, &mappedData);
        if (FAILED(hr)) {
            throw std::runtime_error("upload_frame failed to map upload buffer");
        }
        memcpy(mappedData, cpuInputRGBA, sizeInBytes);
        uploadBuffer->Unmap(0, nullptr);

        // Copy command list
        static ComPtr<ID3D12CommandAllocator> copyAllocator;
        static ComPtr<ID3D12GraphicsCommandList> copyCommandList;
        
        if (!copyAllocator) {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAllocator));
            if (FAILED(hr)) throw std::runtime_error("Failed to create upload copy allocator");

            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAllocator.Get(), nullptr, IID_PPV_ARGS(&copyCommandList));
            if (FAILED(hr)) throw std::runtime_error("Failed to create upload copy command list");
            copyCommandList->Close();
        }

        copyAllocator->Reset();
        copyCommandList->Reset(copyAllocator.Get(), nullptr);

        // Transition gpuTexture from COMMON to COPY_DEST
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpuTexture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyCommandList->ResourceBarrier(1, &barrier);

        copyCommandList->CopyBufferRegion(gpuTexture, 0, uploadBuffer.Get(), 0, sizeInBytes);

        // Transition gpuTexture back from COPY_DEST to COMMON
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        copyCommandList->ResourceBarrier(1, &barrier);

        copyCommandList->Close();

        context.ExecuteCommandList(copyCommandList.Get());
        context.FlushGPU(); // Ensure upload is complete before proceeding
    }

    void apply_effects(
        DirectMLContext& context,
        ColorGradeEffect& effect,
        ID3D12Resource* inputTexture,
        ID3D12Resource* outputTexture,
        const ColorGradeParameters& params
    ) {
        effect.ProcessFrameGPUTexture(inputTexture, outputTexture, params);
    }

    void readback_frame(
        DirectMLContext& context,
        ID3D12Resource* gpuTexture,
        float* cpuOutputRGBA,
        uint64_t sizeInBytes
    ) {
        ID3D12Device* device = context.GetD3D12Device();
        
        static ComPtr<ID3D12Resource> readbackBuffer;
        static uint64_t readbackBufferSize = 0;
        
        if (!readbackBuffer || readbackBufferSize < sizeInBytes) {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProps.CreationNodeMask = 1;
            heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Alignment = 0;
            desc.Width = sizeInBytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            HRESULT hr = device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&readbackBuffer)
            );
            if (FAILED(hr)) {
                throw std::runtime_error("readback_frame failed to create readback buffer. hr = " + std::to_string(hr));
            }
            readbackBufferSize = sizeInBytes;
        }

        static ComPtr<ID3D12CommandAllocator> copyAllocator;
        static ComPtr<ID3D12GraphicsCommandList> copyCommandList;
        
        if (!copyAllocator) {
            HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAllocator));
            if (FAILED(hr)) throw std::runtime_error("Failed to create readback copy allocator");

            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAllocator.Get(), nullptr, IID_PPV_ARGS(&copyCommandList));
            if (FAILED(hr)) throw std::runtime_error("Failed to create readback copy command list");
            copyCommandList->Close();
        }

        copyAllocator->Reset();
        copyCommandList->Reset(copyAllocator.Get(), nullptr);

        // Transition gpuTexture from COMMON to COPY_SOURCE
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpuTexture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyCommandList->ResourceBarrier(1, &barrier);

        copyCommandList->CopyBufferRegion(readbackBuffer.Get(), 0, gpuTexture, 0, sizeInBytes);

        // Transition gpuTexture back from COPY_SOURCE to COMMON
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        copyCommandList->ResourceBarrier(1, &barrier);

        copyCommandList->Close();

        context.ExecuteCommandList(copyCommandList.Get());
        context.FlushGPU(); // Synchronously wait for copy to complete

        // Map and retrieve data
        void* mappedData = nullptr;
        HRESULT hr = readbackBuffer->Map(0, nullptr, &mappedData);
        if (FAILED(hr)) {
            throw std::runtime_error("readback_frame failed to map readback buffer");
        }
        memcpy(cpuOutputRGBA, mappedData, sizeInBytes);
        readbackBuffer->Unmap(0, nullptr);
    }
}
