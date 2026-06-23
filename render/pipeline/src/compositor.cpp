#include "compositor.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

// Vertex format
struct Vertex {
    float pos[2];
    float uv[2];
};

static std::vector<char> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(std::string("Failed to open file: ") + path);
    }
    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();
    return buffer;
}

Compositor::Compositor(ID3D12Device* device, ID3D12CommandQueue* queue,
                       uint32_t canvas_w, uint32_t canvas_h)
    : device_(device), queue_(queue), canvas_w_(canvas_w), canvas_h_(canvas_h), cb_mapped_(nullptr)
{
    // Create command allocator & list
    device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_alloc_));
    device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc_.Get(), nullptr, IID_PPV_ARGS(&cmd_list_));
    cmd_list_->Close(); // Close it initially

    // Create fence
    device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Create descriptor heaps
    // RTV heap (1 descriptor)
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap_));

    // SRV heap (32 descriptors, shader visible)
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.NumDescriptors = 32; // max layers
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap_));

    create_root_signature();
    create_pso();
    create_vertex_buffer();

    // Create CBV upload buffer
    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbv_desc = {};
    cbv_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbv_desc.Width = sizeof(LayerCBData) * 32; // allocate for max 32 layers
    cbv_desc.Height = 1;
    cbv_desc.DepthOrArraySize = 1;
    cbv_desc.MipLevels = 1;
    cbv_desc.Format = DXGI_FORMAT_UNKNOWN;
    cbv_desc.SampleDesc.Count = 1;
    cbv_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbv_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &cbv_desc,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                     IID_PPV_ARGS(&cb_upload_));

    cb_upload_->Map(0, nullptr, reinterpret_cast<void**>(&cb_mapped_));
}

Compositor::~Compositor() {
    wait_gpu();
    if (cb_upload_ && cb_mapped_) {
        cb_upload_->Unmap(0, nullptr);
    }
    if (fence_event_) {
        CloseHandle(fence_event_);
    }
}

void Compositor::wait_gpu() {
    fence_val_++;
    queue_->Signal(fence_.Get(), fence_val_);
    if (fence_->GetCompletedValue() < fence_val_) {
        fence_->SetEventOnCompletion(fence_val_, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

void Compositor::create_root_signature() {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};

    // Slot 0: descriptor table (1 SRV)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Slot 1: root CBV (b0)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Static sampler
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 0;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC sig_desc = {};
    sig_desc.NumParameters = 2;
    sig_desc.pParameters = params;
    sig_desc.NumStaticSamplers = 1;
    sig_desc.pStaticSamplers = &sampler;
    sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        if (error) {
            std::cerr << "Root sig error: " << (char*)error->GetBufferPointer() << "\n";
        }
        throw std::runtime_error("Failed to serialize root signature");
    }

    device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_sig_));
}

void Compositor::create_pso() {
    std::vector<char> vs_code;
    std::vector<char> ps_code;

    try {
        vs_code = ReadFile("compositor_vs.cso");
        ps_code = ReadFile("compositor_ps.cso");
    } catch (...) {
        std::cerr << "Failed to load CSO files, ensure compositor_vs.cso and compositor_ps.cso are in the current directory.\n";
        throw;
    }

    D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.InputLayout = { input_layout, 2 };
    pso_desc.pRootSignature = root_sig_.Get();
    pso_desc.VS = { vs_code.data(), vs_code.size() };
    pso_desc.PS = { ps_code.data(), ps_code.size() };

    // Rasterizer
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthClipEnable = FALSE;

    // Blend (alpha over)
    pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pso_desc.SampleDesc.Count = 1;

    device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso_));
}

void Compositor::create_vertex_buffer() {
    Vertex vertices[] = {
        { {-0.5f, -0.5f}, {0.0f, 1.0f} }, // bottom-left
        { {-0.5f,  0.5f}, {0.0f, 0.0f} }, // top-left
        { { 0.5f,  0.5f}, {1.0f, 0.0f} }, // top-right
        { {-0.5f, -0.5f}, {0.0f, 1.0f} }, // bottom-left (repeat)
        { { 0.5f,  0.5f}, {1.0f, 0.0f} }, // top-right (repeat)
        { { 0.5f, -0.5f}, {1.0f, 1.0f} }  // bottom-right
    };

    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vb_desc = {};
    vb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vb_desc.Width = sizeof(vertices);
    vb_desc.Height = 1;
    vb_desc.DepthOrArraySize = 1;
    vb_desc.MipLevels = 1;
    vb_desc.Format = DXGI_FORMAT_UNKNOWN;
    vb_desc.SampleDesc.Count = 1;
    vb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                     IID_PPV_ARGS(&vb_));

    void* mapped_data;
    vb_->Map(0, nullptr, &mapped_data);
    memcpy(mapped_data, vertices, sizeof(vertices));
    vb_->Unmap(0, nullptr);

    vb_view_.BufferLocation = vb_->GetGPUVirtualAddress();
    vb_view_.StrideInBytes = sizeof(Vertex);
    vb_view_.SizeInBytes = sizeof(vertices);
}

TextureHandle* Compositor::composite(const LayerDesc* layers, uint32_t count, TextureHandle* output_texture) {
    if (count > 32) count = 32;

    cmd_alloc_->Reset();
    cmd_list_->Reset(cmd_alloc_.Get(), pso_.Get());

    // 1. Transition output_texture to RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output_texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd_list_->ResourceBarrier(1, &barrier);

    // Get RTV handle
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();

    // Create RTV
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device_->CreateRenderTargetView(output_texture, &rtv_desc, rtv_handle);

    // 2. Clear render target
    float clear_color[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmd_list_->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);

    cmd_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    // 3. Set viewport (MUST be after OMSetRenderTargets)
    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)canvas_w_, (float)canvas_h_, 0.0f, 1.0f };
    cmd_list_->RSSetViewports(1, &viewport);

    // 4. Set scissor
    D3D12_RECT scissor = { 0, 0, (LONG)canvas_w_, (LONG)canvas_h_ };
    cmd_list_->RSSetScissorRects(1, &scissor);

    // Set root signature and heaps
    ID3D12DescriptorHeap* heaps[] = { srv_heap_.Get() };
    cmd_list_->SetDescriptorHeaps(1, heaps);
    cmd_list_->SetGraphicsRootSignature(root_sig_.Get());
    cmd_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_list_->IASetVertexBuffers(0, 1, &vb_view_);

    UINT srv_descriptor_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 5. For each layer
    for (uint32_t i = 0; i < count; i++) {
        const LayerDesc& layer = layers[i];

        // Ensure texture is in SRV state (assume it already is, based on comments)

        // Update CBV
        LayerCBData* cb = &cb_mapped_[i];
        cb->quad_center[0] = layer.transform.center_x * 2.0f - 1.0f;
        cb->quad_center[1] = -(layer.transform.center_y * 2.0f - 1.0f);
        cb->quad_half_size[0] = layer.transform.width;
        cb->quad_half_size[1] = layer.transform.height;
        cb->rotation = layer.transform.rotation;
        cb->opacity = layer.opacity;
        cb->uv_min[0] = layer.crop.left;
        cb->uv_min[1] = layer.crop.top;
        cb->uv_max[0] = 1.0f - layer.crop.right;
        cb->uv_max[1] = 1.0f - layer.crop.bottom;
        cb->exposure = layer.grade.exposure;
        cb->contrast = layer.grade.contrast;
        cb->temperature = layer.grade.temperature;
        cb->tint = layer.grade.tint;
        cb->saturation = layer.grade.saturation;

        // Create SRV in the heap
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        cpu_handle.ptr += i * srv_descriptor_size;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        device_->CreateShaderResourceView(layer.texture, &srv_desc, cpu_handle);

        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        gpu_handle.ptr += i * srv_descriptor_size;

        cmd_list_->SetGraphicsRootDescriptorTable(0, gpu_handle);

        D3D12_GPU_VIRTUAL_ADDRESS cb_gpu_addr = cb_upload_->GetGPUVirtualAddress() + i * sizeof(LayerCBData);
        cmd_list_->SetGraphicsRootConstantBufferView(1, cb_gpu_addr);

        // Draw
        cmd_list_->DrawInstanced(6, 1, 0, 0);
    }

    // 6. Transition output_texture back to PIXEL_SHADER_RESOURCE
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd_list_->ResourceBarrier(1, &barrier);

    // 7. Close, execute, wait
    cmd_list_->Close();
    ID3D12CommandList* ppCommandLists[] = { cmd_list_.Get() };
    queue_->ExecuteCommandLists(1, ppCommandLists);

    wait_gpu();

    return output_texture;
}

// C ABI Implementation
extern "C" {
    CompositorHandle compositor_create(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t canvas_width, uint32_t canvas_height) {
        return new Compositor(device, queue, canvas_width, canvas_height);
    }

    TextureHandle* compositor_composite(CompositorHandle handle, const LayerDesc* layers, uint32_t layer_count, TextureHandle* output_texture) {
        Compositor* comp = static_cast<Compositor*>(handle);
        return comp->composite(layers, layer_count, output_texture);
    }

    void compositor_destroy(CompositorHandle handle) {
        delete static_cast<Compositor*>(handle);
    }
}
