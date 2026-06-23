#include "color_grade_compute.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

ColorGradeCompute::ColorGradeCompute(ID3D12Device* device, uint32_t max_width, uint32_t max_height)
    : device_(device), cb_mapped_(nullptr) {

    // 1. Compile color_grade.hlsl
    Microsoft::WRL::ComPtr<ID3DBlob> shader_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob;

    // Compile targeting cs_5_0 since D3DCompileFromFile (FXC) supports max SM 5.1,
    // and SM 6.x requires DXC compiler which involves a different API.
    HRESULT hr = D3DCompileFromFile(L"color_grade.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &shader_blob, &error_blob);
    if (FAILED(hr)) {
        if (error_blob) {
            throw std::runtime_error("Shader compilation failed: " + std::string(static_cast<const char*>(error_blob->GetBufferPointer())));
        }
        throw std::runtime_error("Shader compilation failed with HRESULT: " + std::to_string(hr));
    }

    // 2. Create Root Signature
    D3D12_DESCRIPTOR_RANGE uav_range = {};
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors = 1;
    uav_range.BaseShaderRegister = 0;
    uav_range.RegisterSpace = 0;
    uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER root_params[2] = {};

    // UAV Table
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[0].DescriptorTable.NumDescriptorRanges = 1;
    root_params[0].DescriptorTable.pDescriptorRanges = &uav_range;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Constant Buffer View
    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[1].Descriptor.ShaderRegister = 0;
    root_params[1].Descriptor.RegisterSpace = 0;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.NumParameters = 2;
    root_sig_desc.pParameters = root_params;
    root_sig_desc.NumStaticSamplers = 0;
    root_sig_desc.pStaticSamplers = nullptr;
    root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> root_sig_blob;
    hr = D3D12SerializeRootSignature(&root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &root_sig_blob, &error_blob);
    if (FAILED(hr)) {
         throw std::runtime_error("Root signature serialization failed.");
    }

    hr = device_->CreateRootSignature(0, root_sig_blob->GetBufferPointer(), root_sig_blob->GetBufferSize(), IID_PPV_ARGS(&root_sig_));
    if (FAILED(hr)) {
        throw std::runtime_error("Root signature creation failed.");
    }

    // 3. Create Compute Pipeline State
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root_sig_.Get();
    pso_desc.CS.pShaderBytecode = shader_blob->GetBufferPointer();
    pso_desc.CS.BytecodeLength = shader_blob->GetBufferSize();

    hr = device_->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) {
         throw std::runtime_error("Compute PSO creation failed.");
    }

    // 4. Create UAV Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NumDescriptors = 1;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    hr = device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&uav_heap_));
    if (FAILED(hr)) {
         throw std::runtime_error("UAV descriptor heap creation failed.");
    }

    // 5. Create UPLOAD Heap Constant Buffer
    D3D12_HEAP_PROPERTIES upload_heap_props = {};
    upload_heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    upload_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    upload_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    upload_heap_props.CreationNodeMask = 1;
    upload_heap_props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC cb_desc = {};
    cb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cb_desc.Alignment = 0;
    cb_desc.Width = 256; // 256-byte aligned size
    cb_desc.Height = 1;
    cb_desc.DepthOrArraySize = 1;
    cb_desc.MipLevels = 1;
    cb_desc.Format = DXGI_FORMAT_UNKNOWN;
    cb_desc.SampleDesc.Count = 1;
    cb_desc.SampleDesc.Quality = 0;
    cb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = device_->CreateCommittedResource(
        &upload_heap_props,
        D3D12_HEAP_FLAG_NONE,
        &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&cb_upload_)
    );
    if (FAILED(hr)) {
         throw std::runtime_error("Constant buffer creation failed.");
    }

    // Map persistently
    D3D12_RANGE read_range = {0, 0};
    hr = cb_upload_->Map(0, &read_range, reinterpret_cast<void**>(&cb_mapped_));
    if (FAILED(hr)) {
        throw std::runtime_error("Mapping constant buffer failed.");
    }
}

ColorGradeCompute::~ColorGradeCompute() {
    if (cb_upload_) {
        cb_upload_->Unmap(0, nullptr);
    }
}

void ColorGradeCompute::apply(ID3D12GraphicsCommandList* cmd_list,
                              ID3D12Resource* texture,
                              uint32_t width, uint32_t height,
                              const Params& params) {

    // a. Write params to cb_mapped_
    std::memcpy(cb_mapped_, &params, sizeof(Params));

    // b. Create UAV for the texture. To avoid a race condition, we should really bind this properly per resource
    // or use root descriptors if SM6.6. Since we're using a single heap, we create the view.
    // As apply is called synchronously sequentially in our specific setup, we just update it here.
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = texture->GetDesc().Format;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;

    device_->CreateUnorderedAccessView(texture, nullptr, &uav_desc, uav_heap_->GetCPUDescriptorHandleForHeapStart());

    // c. Set root signature
    cmd_list->SetComputeRootSignature(root_sig_.Get());

    // d. Set Descriptor Table
    ID3D12DescriptorHeap* heaps[] = { uav_heap_.Get() };
    cmd_list->SetDescriptorHeaps(1, heaps);
    cmd_list->SetComputeRootDescriptorTable(0, uav_heap_->GetGPUDescriptorHandleForHeapStart());

    // e. Set Constant Buffer
    cmd_list->SetComputeRootConstantBufferView(1, cb_upload_->GetGPUVirtualAddress());

    // f. Set PSO
    cmd_list->SetPipelineState(pso_.Get());

    // g. Dispatch
    uint32_t dispatch_x = static_cast<uint32_t>(std::ceil(width / 8.0f));
    uint32_t dispatch_y = static_cast<uint32_t>(std::ceil(height / 8.0f));
    cmd_list->Dispatch(dispatch_x, dispatch_y, 1);
}