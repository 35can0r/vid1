#include "preview_presenter.h"
#include "../include/renderer.h"
#include <stdexcept>
#include <iostream>
#include <d3dcompiler.h>
#include <unknwn.h>

// Forward declarations
struct IDXGISwapChain;

// WinUI 3 ISwapChainPanelNative interface definition
struct __declspec(uuid("63aad0b8-7c24-40ff-85a8-640d944cc325")) ISwapChainPanelNative : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetSwapChain(IDXGISwapChain* swapChain) = 0;
};

using Microsoft::WRL::ComPtr;

// Fullscreen procedural triangle and Reinhard tonemapper shader
const char* tonemapShaderSource = R"(
struct VS_OUTPUT {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT VSMain(uint vertexID : SV_VertexID) {
    VS_OUTPUT output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

Texture2D<float4> InputTexture : register(t0);
SamplerState PointSampler : register(s0);

float4 PSMain(VS_OUTPUT input) : SV_Target {
    float4 hdr = InputTexture.Sample(PointSampler, input.uv);
    hdr.rgb = hdr.rgb / (hdr.rgb + 1.0f); // Reinhard tonemapping
    return float4(hdr.rgb, 1.0f);
}
)";

PreviewPresenter::PreviewPresenter(IUnknown* panelNative,
                                   ID3D12Device* device,
                                   ID3D12CommandQueue* queue,
                                   uint32_t width, uint32_t height)
    : device_(device), queue_(queue), width_(width), height_(height) {

    create_swapchain(panelNative, width, height);
    create_tonemap_pso();

    // Create Direct Command Allocator
    HRESULT hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_alloc_));
    if (FAILED(hr)) throw std::runtime_error("Failed to create Command Allocator");

    // Create Direct Command List (closed initially)
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc_.Get(), tonemap_pso_.Get(), IID_PPV_ARGS(&cmd_list_));
    if (FAILED(hr)) throw std::runtime_error("Failed to create Command List");
    cmd_list_->Close();

    // Create Fence
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) throw std::runtime_error("Failed to create Fence");
    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Create RTV Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 2;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtv_heap_));
    if (FAILED(hr)) throw std::runtime_error("Failed to create RTV heap");

    UINT rtvDescriptorSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();

    // Initialize RTV views for backbuffers
    for (UINT i = 0; i < 2; ++i) {
        device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize;
    }

    // Create SRV Descriptor Heap (shader-visible)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device_->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&tonemap_srv_heap_));
    if (FAILED(hr)) throw std::runtime_error("Failed to create SRV heap");
}

PreviewPresenter::~PreviewPresenter() {
    wait_gpu();
    if (fence_event_) {
        CloseHandle(fence_event_);
    }
    for (UINT i = 0; i < 2; ++i) {
        back_buffers_[i].Reset();
    }
    swap_chain_.Reset();
}

void PreviewPresenter::create_swapchain(IUnknown* panel_native, uint32_t w, uint32_t h) {
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) throw std::runtime_error("CreateDXGIFactory2 failed");

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForComposition(queue_, &desc, nullptr, &sc1);
    if (FAILED(hr)) throw std::runtime_error("CreateSwapChainForComposition failed");

    hr = sc1.As(&swap_chain_);
    if (FAILED(hr)) throw std::runtime_error("QueryInterface for IDXGISwapChain3 failed");

    // Query native ISwapChainPanelNative
    ComPtr<ISwapChainPanelNative> nativePanel;
    hr = panel_native->QueryInterface(IID_PPV_ARGS(&nativePanel));
    if (FAILED(hr)) throw std::runtime_error("QueryInterface for ISwapChainPanelNative failed");

    hr = nativePanel->SetSwapChain(swap_chain_.Get());
    if (FAILED(hr)) throw std::runtime_error("SetSwapChain failed");

    // Retrieve backbuffers
    for (UINT i = 0; i < 2; ++i) {
        hr = swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
        if (FAILED(hr)) throw std::runtime_error("GetBuffer failed");
    }
}

void PreviewPresenter::create_tonemap_pso() {
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(
        tonemapShaderSource,
        strlen(tonemapShaderSource),
        "tonemap.hlsl",
        nullptr,
        nullptr,
        "VSMain",
        "vs_5_0",
        0, 0,
        &vsBlob,
        &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        }
        throw std::runtime_error("Failed to compile VSMain");
    }

    hr = D3DCompile(
        tonemapShaderSource,
        strlen(tonemapShaderSource),
        "tonemap.hlsl",
        nullptr,
        nullptr,
        "PSMain",
        "ps_5_0",
        0, 0,
        &psBlob,
        &errorBlob
    );
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        }
        throw std::runtime_error("Failed to compile PSMain");
    }

    // Create Root Signature
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static Sampler
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC sigDesc = {};
    sigDesc.NumParameters = 1;
    sigDesc.pParameters = &param;
    sigDesc.NumStaticSamplers = 1;
    sigDesc.pStaticSamplers = &sampler;
    sigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    hr = D3D12SerializeRootSignature(&sigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA((const char*)errorBlob->GetBufferPointer());
        }
        throw std::runtime_error("Failed to serialize root signature");
    }

    hr = device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&tonemap_root_sig_));
    if (FAILED(hr)) throw std::runtime_error("Failed to create root signature");

    // Create Pipeline State Object (PSO)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = tonemap_root_sig_.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&tonemap_pso_));
    if (FAILED(hr)) throw std::runtime_error("Failed to create graphics pipeline state");
}

void PreviewPresenter::present(ID3D12Resource* compositor_output) {
    if (!compositor_output) return;

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
    ID3D12Resource* backbuffer = back_buffers_[frame_index_].Get();

    cmd_alloc_->Reset();
    cmd_list_->Reset(cmd_alloc_.Get(), tonemap_pso_.Get());

    // 1. Transition compositor_output to PIXEL_SHADER_RESOURCE state
    D3D12_RESOURCE_BARRIER src_barrier = {};
    src_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    src_barrier.Transition.pResource = compositor_output;
    src_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd_list_->ResourceBarrier(1, &src_barrier);

    // 2. Create SRV for compositor output (which is RGBA32F) in slot 0 of tonemap_srv_heap_
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = tonemap_srv_heap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateShaderResourceView(compositor_output, &srvDesc, srvHandle);

    // 3. Transition swapchain backbuffer to RENDER_TARGET state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd_list_->ResourceBarrier(1, &barrier);

    // Set RTV
    UINT rtvDescriptorSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += frame_index_ * rtvDescriptorSize;
    cmd_list_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Clear render target
    float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmd_list_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // Bind Root Signature & Descriptor Heaps
    cmd_list_->SetGraphicsRootSignature(tonemap_root_sig_.Get());
    ID3D12DescriptorHeap* heaps[] = { tonemap_srv_heap_.Get() };
    cmd_list_->SetDescriptorHeaps(1, heaps);
    cmd_list_->SetGraphicsRootDescriptorTable(0, tonemap_srv_heap_->GetGPUDescriptorHandleForHeapStart());

    // Set viewport and scissor rects immediately before drawing
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width_, (float)height_, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG)width_, (LONG)height_ };
    cmd_list_->RSSetViewports(1, &vp);
    cmd_list_->RSSetScissorRects(1, &scissor);

    // Draw fullscreen triangle
    cmd_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_list_->DrawInstanced(3, 1, 0, 0);

    // 4. Transition compositor_output back to COMMON state
    src_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    src_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmd_list_->ResourceBarrier(1, &src_barrier);

    // 5. Transition swapchain backbuffer back to PRESENT state
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd_list_->ResourceBarrier(1, &barrier);

    // Close and Execute
    cmd_list_->Close();
    ID3D12CommandList* ppCommandLists[] = { cmd_list_.Get() };
    queue_->ExecuteCommandLists(1, ppCommandLists);

    // Present & Wait GPU
    swap_chain_->Present(1, 0);
    wait_gpu();
}

void PreviewPresenter::resize(uint32_t new_width, uint32_t new_height) {
    if (new_width == width_ && new_height == height_) return;
    if (new_width == 0 || new_height == 0) return;

    wait_gpu();

    // Release swapchain backbuffer references
    for (UINT i = 0; i < 2; ++i) {
        back_buffers_[i].Reset();
    }

    HRESULT hr = swap_chain_->ResizeBuffers(2, new_width, new_height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) throw std::runtime_error("ResizeBuffers failed");

    width_ = new_width;
    height_ = new_height;

    // Reacquire backbuffer references
    for (UINT i = 0; i < 2; ++i) {
        hr = swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
        if (FAILED(hr)) throw std::runtime_error("GetBuffer failed after resize");
    }

    // Recreate RTV views
    UINT rtvDescriptorSize = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; ++i) {
        device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize;
    }
}

void PreviewPresenter::wait_gpu() {
    fence_val_++;
    queue_->Signal(fence_.Get(), fence_val_);
    if (fence_->GetCompletedValue() < fence_val_) {
        fence_->SetEventOnCompletion(fence_val_, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

extern "C" {
    PresenterHandle* presenter_create(void* swap_chain_panel_native, RendererHandle* engine, uint32_t width, uint32_t height) {
        if (!engine) {
            std::cerr << "[presenter_create] Error: engine is null" << std::endl;
            return nullptr;
        }
        ID3D12Device* device = render_engine_get_device(engine);
        ID3D12CommandQueue* queue = render_engine_get_queue(engine);
        if (!device || !queue) {
            std::cerr << "[presenter_create] Error: device=" << device << ", queue=" << queue << std::endl;
            return nullptr;
        }

        try {
            std::cout << "[presenter_create] Creating PreviewPresenter, panel=" << swap_chain_panel_native << std::endl;
            auto* presenter = new PreviewPresenter(
                static_cast<IUnknown*>(swap_chain_panel_native),
                device,
                queue,
                width,
                height
            );
            std::cout << "[presenter_create] PreviewPresenter created successfully" << std::endl;
            return reinterpret_cast<PresenterHandle*>(presenter);
        } catch (const std::exception& e) {
            std::cerr << "[presenter_create] std::exception caught: " << e.what() << std::endl;
            return nullptr;
        } catch (...) {
            std::cerr << "[presenter_create] Unknown exception caught" << std::endl;
            return nullptr;
        }
    }

    void presenter_present(PresenterHandle* presenter, TextureHandle* compositor_output) {
        if (presenter && compositor_output) {
            auto* p = reinterpret_cast<PreviewPresenter*>(presenter);
            p->present(compositor_output);
        }
    }

    void presenter_resize(PresenterHandle* presenter, uint32_t width, uint32_t height) {
        if (presenter) {
            auto* p = reinterpret_cast<PreviewPresenter*>(presenter);
            p->resize(width, height);
        }
    }

    void presenter_destroy(PresenterHandle* presenter) {
        if (presenter) {
            auto* p = reinterpret_cast<PreviewPresenter*>(presenter);
            delete p;
        }
    }
}
