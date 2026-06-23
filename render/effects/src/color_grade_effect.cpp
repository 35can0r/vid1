#include "color_grade_effect.h"
#include <iostream>
#include <cmath>
#include <algorithm>

// DirectML helper macro to check HRESULTs
#define THROW_IF_FAILED(hr, msg) \
    if (FAILED(hr)) { \
        throw std::runtime_error(std::string(msg) + ". hr = " + std::to_string(hr)); \
    }

static ComPtr<ID3D12Resource> CreateD3D12Buffer(
    ID3D12Device* device,
    UINT64 size,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE
) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = heapType;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource)
    );
    THROW_IF_FAILED(hr, "Failed to create committed resource of size " + std::to_string(size));
    return resource;
}

ColorGradeEffect::ColorGradeEffect(DirectMLContext& context, uint32_t width, uint32_t height)
    : m_context(context), m_width(width), m_height(height) 
{
    m_pixelCount = m_width * m_height;
    m_tensorSizeInBytes = m_pixelCount * 4 * sizeof(float); // RGBA floats

    // Create D3D12 Command allocator and list
    HRESULT hr = m_context.GetD3D12Device()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator)
    );
    THROW_IF_FAILED(hr, "Failed to create D3D12 Command Allocator");

    hr = m_context.GetD3D12Device()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList)
    );
    THROW_IF_FAILED(hr, "Failed to create D3D12 Command List");
    m_commandList->Close(); // Close initially, will open on use
}

ColorGradeEffect::~ColorGradeEffect() {
    // Release resources
}

void ColorGradeEffect::CompileOperatorGraph(const ColorGradeParameters& params) {
    if (m_graphCompiled) return;

    std::cout << "[DirectML] Compiling ColorGradeEffect operator graph..." << std::endl;

    ComPtr<IDMLDevice1> dmlDevice1;
    HRESULT hr = m_context.GetDMLDevice()->QueryInterface(IID_PPV_ARGS(&dmlDevice1));
    THROW_IF_FAILED(hr, "Failed to query IDMLDevice1");

    // 1. Describe Tensors
    // Input Image Tensor: NHWC layout (interleaved RGBA)
    // Shape: [1, 4, H, W]
    // Strides: [H*W*4, 1, W*4, 4]
    UINT sizes[4] = { 1, 4, m_height, m_width };
    UINT strides[4] = { m_pixelCount * 4, 1, m_width * 4, 4 };

    DML_BUFFER_TENSOR_DESC inputBufferDesc = {};
    DML_TENSOR_DESC inputDesc = {};
    inputBufferDesc.DataType = DML_TENSOR_DATA_TYPE_FLOAT32;
    inputBufferDesc.Flags = DML_TENSOR_FLAG_NONE;
    inputBufferDesc.DimensionCount = 4;
    inputBufferDesc.Sizes = sizes;
    inputBufferDesc.Strides = strides;
    inputBufferDesc.TotalTensorSizeInBytes = m_tensorSizeInBytes;
    inputDesc.Type = DML_TENSOR_TYPE_BUFFER;
    inputDesc.Desc = &inputBufferDesc;

    // Scale Tensor: logically shape [1, 4, H, W], physically containing 4 elements broadcasted using strides [4, 1, 0, 0]
    UINT scaleSizes[4] = { 1, 4, m_height, m_width };
    UINT scaleStrides[4] = { 4, 1, 0, 0 };
    
    DML_BUFFER_TENSOR_DESC scaleBufferDesc = {};
    DML_TENSOR_DESC scaleDesc = {};
    scaleBufferDesc.DataType = DML_TENSOR_DATA_TYPE_FLOAT32;
    scaleBufferDesc.Flags = DML_TENSOR_FLAG_NONE;
    scaleBufferDesc.DimensionCount = 4;
    scaleBufferDesc.Sizes = scaleSizes;
    scaleBufferDesc.Strides = scaleStrides;
    scaleBufferDesc.TotalTensorSizeInBytes = 4 * sizeof(float);
    scaleDesc.Type = DML_TENSOR_TYPE_BUFFER;
    scaleDesc.Desc = &scaleBufferDesc;

    // Intermediate Image Tensors (same layout as Input/Output)
    DML_BUFFER_TENSOR_DESC intermediateBufferDesc = inputBufferDesc;
    DML_TENSOR_DESC intermediateDesc = {};
    intermediateDesc.Type = DML_TENSOR_TYPE_BUFFER;
    intermediateDesc.Desc = &intermediateBufferDesc;

    // Output Image Tensor (same layout as Input)
    DML_TENSOR_DESC outputDesc = inputDesc;

    // Bias Tensor: shape [1, 4, 1, 1], strides [4, 1, 1, 1]
    DML_BUFFER_TENSOR_DESC biasBufferDesc = scaleBufferDesc;
    DML_TENSOR_DESC biasDesc = {};
    biasDesc.Type = DML_TENSOR_TYPE_BUFFER;
    biasDesc.Desc = &biasBufferDesc;

    // 2. Create Operators
    // Node 0: Element-wise Multiply (Scale)
    DML_ELEMENT_WISE_MULTIPLY_OPERATOR_DESC multiplyDesc = {};
    multiplyDesc.ATensor = &inputDesc;
    multiplyDesc.BTensor = &scaleDesc;
    multiplyDesc.OutputTensor = &intermediateDesc;
    
    DML_OPERATOR_DESC opMultiplyDesc = {};
    opMultiplyDesc.Type = DML_OPERATOR_ELEMENT_WISE_MULTIPLY;
    opMultiplyDesc.Desc = &multiplyDesc;
    
    ComPtr<IDMLOperator> multiplyOp;
    hr = m_context.GetDMLDevice()->CreateOperator(&opMultiplyDesc, IID_PPV_ARGS(&multiplyOp));
    THROW_IF_FAILED(hr, "Failed to create ElementWiseMultiply Operator");

    // Node 1: Element-wise Add (Bias) - Bias uses same shape [1, 4, 1, 1] and strides
    DML_ELEMENT_WISE_ADD_OPERATOR_DESC addDesc = {};
    addDesc.ATensor = &intermediateDesc;
    addDesc.BTensor = &biasDesc; // Bias shape matches Scale shape
    addDesc.OutputTensor = &intermediateDesc;

    DML_OPERATOR_DESC opAddDesc = {};
    opAddDesc.Type = DML_OPERATOR_ELEMENT_WISE_ADD;
    opAddDesc.Desc = &addDesc;

    ComPtr<IDMLOperator> addOp;
    hr = m_context.GetDMLDevice()->CreateOperator(&opAddDesc, IID_PPV_ARGS(&addOp));
    THROW_IF_FAILED(hr, "Failed to create ElementWiseAdd Operator");

    // Node 2: Element-wise Clip (Clamp to [0.0, 1.0])
    DML_ELEMENT_WISE_CLIP_OPERATOR_DESC clipDesc = {};
    clipDesc.InputTensor = &intermediateDesc;
    clipDesc.OutputTensor = &outputDesc;
    clipDesc.Min = 0.0f;
    clipDesc.Max = 1.0f;

    DML_OPERATOR_DESC opClipDesc = {};
    opClipDesc.Type = DML_OPERATOR_ELEMENT_WISE_CLIP;
    opClipDesc.Desc = &clipDesc;

    ComPtr<IDMLOperator> clipOp;
    hr = m_context.GetDMLDevice()->CreateOperator(&opClipDesc, IID_PPV_ARGS(&clipOp));
    THROW_IF_FAILED(hr, "Failed to create ElementWiseClip Operator");

    // 3. Define Graph Nodes & Edges
    DML_GRAPH_NODE_DESC nodes[3] = {};

    DML_OPERATOR_GRAPH_NODE_DESC multiplyNode = { multiplyOp.Get(), "MultiplyNode" };
    nodes[0].Type = DML_GRAPH_NODE_TYPE_OPERATOR;
    nodes[0].Desc = &multiplyNode;

    DML_OPERATOR_GRAPH_NODE_DESC addNode = { addOp.Get(), "AddNode" };
    nodes[1].Type = DML_GRAPH_NODE_TYPE_OPERATOR;
    nodes[1].Desc = &addNode;

    DML_OPERATOR_GRAPH_NODE_DESC clipNode = { clipOp.Get(), "ClipNode" };
    nodes[2].Type = DML_GRAPH_NODE_TYPE_OPERATOR;
    nodes[2].Desc = &clipNode;

    // Connect Graph Inputs to Nodes
    DML_INPUT_GRAPH_EDGE_DESC inputEdgeDescs[3] = {};
    // Input 0 (Image) -> Node 0 Input A
    inputEdgeDescs[0].GraphInputIndex = 0;
    inputEdgeDescs[0].ToNodeIndex = 0;
    inputEdgeDescs[0].ToNodeInputIndex = 0;
    
    // Input 1 (Scale) -> Node 0 Input B
    inputEdgeDescs[1].GraphInputIndex = 1;
    inputEdgeDescs[1].ToNodeIndex = 0;
    inputEdgeDescs[1].ToNodeInputIndex = 1;

    // Input 2 (Bias) -> Node 1 Input B
    inputEdgeDescs[2].GraphInputIndex = 2;
    inputEdgeDescs[2].ToNodeIndex = 1;
    inputEdgeDescs[2].ToNodeInputIndex = 1;

    // Connect Nodes Internally
    DML_INTERMEDIATE_GRAPH_EDGE_DESC intermediateEdgeDescs[2] = {};
    // Node 0 Output -> Node 1 Input A
    intermediateEdgeDescs[0].FromNodeIndex = 0;
    intermediateEdgeDescs[0].FromNodeOutputIndex = 0;
    intermediateEdgeDescs[0].ToNodeIndex = 1;
    intermediateEdgeDescs[0].ToNodeInputIndex = 0;

    // Node 1 Output -> Node 2 Input
    intermediateEdgeDescs[1].FromNodeIndex = 1;
    intermediateEdgeDescs[1].FromNodeOutputIndex = 0;
    intermediateEdgeDescs[1].ToNodeIndex = 2;
    intermediateEdgeDescs[1].ToNodeInputIndex = 0;

    // Connect Node Output to Graph Output
    DML_OUTPUT_GRAPH_EDGE_DESC outputEdgeDescs[1] = {};
    outputEdgeDescs[0].FromNodeIndex = 2;
    outputEdgeDescs[0].FromNodeOutputIndex = 0;
    outputEdgeDescs[0].GraphOutputIndex = 0;

    // Wrap in DML_GRAPH_EDGE_DESC structures
    DML_GRAPH_EDGE_DESC inputEdges[3] = {};
    for (int i = 0; i < 3; ++i) {
        inputEdges[i].Type = DML_GRAPH_EDGE_TYPE_INPUT;
        inputEdges[i].Desc = &inputEdgeDescs[i];
    }

    DML_GRAPH_EDGE_DESC intermediateEdges[2] = {};
    for (int i = 0; i < 2; ++i) {
        intermediateEdges[i].Type = DML_GRAPH_EDGE_TYPE_INTERMEDIATE;
        intermediateEdges[i].Desc = &intermediateEdgeDescs[i];
    }

    DML_GRAPH_EDGE_DESC outputEdges[1] = {};
    for (int i = 0; i < 1; ++i) {
        outputEdges[i].Type = DML_GRAPH_EDGE_TYPE_OUTPUT;
        outputEdges[i].Desc = &outputEdgeDescs[i];
    }

    // 4. Compile Graph
    DML_GRAPH_DESC graphDesc = {};
    graphDesc.InputCount = 3;
    graphDesc.OutputCount = 1;
    graphDesc.NodeCount = 3;
    graphDesc.Nodes = nodes;
    graphDesc.InputEdgeCount = 3;
    graphDesc.InputEdges = inputEdges;
    graphDesc.OutputEdgeCount = 1;
    graphDesc.OutputEdges = outputEdges;
    graphDesc.IntermediateEdgeCount = 2;
    graphDesc.IntermediateEdges = intermediateEdges;

    hr = dmlDevice1->CompileGraph(
        &graphDesc,
        DML_EXECUTION_FLAG_NONE,
        IID_PPV_ARGS(&m_compiledOperator)
    );
    THROW_IF_FAILED(hr, "Failed to compile DirectML Operator Graph");

    m_graphCompiled = true;
    std::cout << "[DirectML] Operator graph compiled successfully." << std::endl;
}

void ColorGradeEffect::AllocateGPUResources() {
    if (m_resourcesAllocated) return;

    std::cout << "[DirectML] Allocating D3D12 GPU resources..." << std::endl;

    ID3D12Device* device = m_context.GetD3D12Device();

    // 1. Create Frame buffers
    m_inputUploadBuffer = CreateD3D12Buffer(
        device, m_tensorSizeInBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ
    );
    m_inputGPUBuffer = CreateD3D12Buffer(
        device, m_tensorSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    m_outputGPUBuffer = CreateD3D12Buffer(
        device, m_tensorSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    m_outputReadbackBuffer = CreateD3D12Buffer(
        device, m_tensorSizeInBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST
    );

    // 2. Create Constant buffers (16 bytes each)
    m_scaleUploadBuffer = CreateD3D12Buffer(
        device, 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ
    );
    m_scaleGPUBuffer = CreateD3D12Buffer(
        device, 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );
    m_biasUploadBuffer = CreateD3D12Buffer(
        device, 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ
    );
    m_biasGPUBuffer = CreateD3D12Buffer(
        device, 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
    );

    // 3. Query DirectML compiled operator binding properties
    DML_BINDING_PROPERTIES bindingProps = m_compiledOperator->GetBindingProperties();
    
    // Allocate Persistent Buffer if required
    if (bindingProps.PersistentResourceSize > 0) {
        m_persistentBuffer = CreateD3D12Buffer(
            device,
            bindingProps.PersistentResourceSize,
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
        );
    }

    // Allocate Temporary Buffer if required
    if (bindingProps.TemporaryResourceSize > 0) {
        m_temporaryBuffer = CreateD3D12Buffer(
            device,
            bindingProps.TemporaryResourceSize,
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
        );
    }

    // 4. Create descriptor heap for bindings
    DML_BINDING_TABLE_DESC bindingTableDesc = {};
    bindingTableDesc.Dispatchable = m_compiledOperator.Get();
    bindingTableDesc.CPUDescriptorHandle = {};
    bindingTableDesc.GPUDescriptorHandle = {};
    bindingTableDesc.SizeInDescriptors = bindingProps.RequiredDescriptorCount;

    if (bindingProps.RequiredDescriptorCount > 0) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = bindingProps.RequiredDescriptorCount;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));
        THROW_IF_FAILED(hr, "Failed to create Descriptor Heap");

        bindingTableDesc.CPUDescriptorHandle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        bindingTableDesc.GPUDescriptorHandle = m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    }

    // 5. Create DirectML Binding Table
    HRESULT hr = m_context.GetDMLDevice()->CreateBindingTable(
        &bindingTableDesc,
        IID_PPV_ARGS(&m_bindingTable)
    );
    THROW_IF_FAILED(hr, "Failed to create DirectML Binding Table");

    // 6. Bind inputs, outputs, persistent, and temporary buffers
    // Bind Persistent Buffer (if allocated)
    if (m_persistentBuffer) {
        DML_BUFFER_BINDING persistentBinding = { m_persistentBuffer.Get(), 0, bindingProps.PersistentResourceSize };
        DML_BINDING_DESC bindingDesc = { DML_BINDING_TYPE_BUFFER, &persistentBinding };
        m_bindingTable->BindPersistentResource(&bindingDesc);
    }

    // Bind Temporary Buffer (if allocated) during ProcessFrameGPU (since it's transient)
    
    // Bind inputs/outputs
    // Bind Input 0 (Image)
    DML_BUFFER_BINDING inputBindings[3] = {};
    inputBindings[0] = { m_inputGPUBuffer.Get(), 0, m_tensorSizeInBytes };
    // Bind Input 1 (Scale)
    inputBindings[1] = { m_scaleGPUBuffer.Get(), 0, 16 };
    // Bind Input 2 (Bias)
    inputBindings[2] = { m_biasGPUBuffer.Get(), 0, 16 };

    DML_BINDING_DESC inputBindingDescs[3] = {};
    inputBindingDescs[0] = { DML_BINDING_TYPE_BUFFER, &inputBindings[0] };
    inputBindingDescs[1] = { DML_BINDING_TYPE_BUFFER, &inputBindings[1] };
    inputBindingDescs[2] = { DML_BINDING_TYPE_BUFFER, &inputBindings[2] };
    m_bindingTable->BindInputs(3, inputBindingDescs);

    // Bind Output 0 (Image)
    DML_BUFFER_BINDING outputBinding = { m_outputGPUBuffer.Get(), 0, m_tensorSizeInBytes };
    DML_BINDING_DESC outputBindingDesc = { DML_BINDING_TYPE_BUFFER, &outputBinding };
    m_bindingTable->BindOutputs(1, &outputBindingDesc);

    m_resourcesAllocated = true;
    std::cout << "[DirectML] GPU resources successfully allocated and bound." << std::endl;
}

void ColorGradeEffect::UpdateConstantTensors(const ColorGradeParameters& params) {
    float E = std::pow(2.0f, params.exposure);
    float C = params.contrast;
    float S = params.saturation;
    float temp = params.temperature;
    float tint = params.tint;

    // Final scales
    float scale[4] = { E * C * S, E * C * S, E * C * S, 1.0f };

    // Final biases
    float bias[4] = {
        0.5f * (1.0f - C * S) + temp * S,
        0.5f * (1.0f - C * S) + tint * S,
        0.5f * (1.0f - C * S) - temp * S,
        0.0f
    };

    // Copy to upload buffers
    void* data = nullptr;
    HRESULT hr = m_scaleUploadBuffer->Map(0, nullptr, &data);
    THROW_IF_FAILED(hr, "Failed to map scale upload buffer");
    memcpy(data, scale, sizeof(scale));
    m_scaleUploadBuffer->Unmap(0, nullptr);

    hr = m_biasUploadBuffer->Map(0, nullptr, &data);
    THROW_IF_FAILED(hr, "Failed to map bias upload buffer");
    memcpy(data, bias, sizeof(bias));
    m_biasUploadBuffer->Unmap(0, nullptr);
}

void ColorGradeEffect::ProcessFrameGPU(
    const float* inputRGBA,
    float* outputRGBA,
    const ColorGradeParameters& params
) {
    // 1. Ensure initialization
    CompileOperatorGraph(params);
    AllocateGPUResources();
    UpdateConstantTensors(params);

    // 2. Map input host memory and copy data
    void* mappedInput = nullptr;
    HRESULT hr = m_inputUploadBuffer->Map(0, nullptr, &mappedInput);
    THROW_IF_FAILED(hr, "Failed to map input upload buffer");
    memcpy(mappedInput, inputRGBA, m_tensorSizeInBytes);
    m_inputUploadBuffer->Unmap(0, nullptr);

    // 3. Open D3D12 command list and record copy/execution commands
    hr = m_commandAllocator->Reset();
    THROW_IF_FAILED(hr, "Failed to reset Command Allocator");

    hr = m_commandList->Reset(m_commandAllocator.Get(), nullptr);
    THROW_IF_FAILED(hr, "Failed to reset Command List");

    // Copy input host to GPU default heaps
    m_commandList->CopyBufferRegion(m_inputGPUBuffer.Get(), 0, m_inputUploadBuffer.Get(), 0, m_tensorSizeInBytes);
    m_commandList->CopyBufferRegion(m_scaleGPUBuffer.Get(), 0, m_scaleUploadBuffer.Get(), 0, 16);
    m_commandList->CopyBufferRegion(m_biasGPUBuffer.Get(), 0, m_biasUploadBuffer.Get(), 0, 16);

    // Transition copies to UAV compatibility
    D3D12_RESOURCE_BARRIER barriers[3] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = m_inputGPUBuffer.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = m_scaleGPUBuffer.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[2].Transition.pResource = m_biasGPUBuffer.Get();
    barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(3, barriers);

    // Bind temporary buffer (if required)
    DML_BINDING_PROPERTIES bindingProps = m_compiledOperator->GetBindingProperties();
    if (m_temporaryBuffer) {
        DML_BUFFER_BINDING tempBinding = { m_temporaryBuffer.Get(), 0, bindingProps.TemporaryResourceSize };
        DML_BINDING_DESC bindingDesc = { DML_BINDING_TYPE_BUFFER, &tempBinding };
        m_bindingTable->BindTemporaryResource(&bindingDesc);
    }

    // Set descriptor heap
    if (m_descriptorHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
    }

    // Record DirectML execution dispatch
    ComPtr<IDMLCommandRecorder> recorder;
    hr = m_context.GetDMLDevice()->CreateCommandRecorder(IID_PPV_ARGS(&recorder));
    THROW_IF_FAILED(hr, "Failed to create DirectML Command Recorder");

    recorder->RecordDispatch(
        m_commandList.Get(),
        m_compiledOperator.Get(),
        m_bindingTable.Get()
    );

    // Transition output to COPY_SOURCE for readback
    D3D12_RESOURCE_BARRIER readBarrier = {};
    readBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    readBarrier.Transition.pResource = m_outputGPUBuffer.Get();
    readBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    readBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    readBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &readBarrier);

    // Copy to readback buffer
    m_commandList->CopyBufferRegion(m_outputReadbackBuffer.Get(), 0, m_outputGPUBuffer.Get(), 0, m_tensorSizeInBytes);

    // Transition back to UAV state
    D3D12_RESOURCE_BARRIER restoreBarriers[4] = {};
    restoreBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarriers[0].Transition.pResource = m_outputGPUBuffer.Get();
    restoreBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    restoreBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restoreBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    restoreBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarriers[1].Transition.pResource = m_inputGPUBuffer.Get();
    restoreBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restoreBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    restoreBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    restoreBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarriers[2].Transition.pResource = m_scaleGPUBuffer.Get();
    restoreBarriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restoreBarriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    restoreBarriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    restoreBarriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarriers[3].Transition.pResource = m_biasGPUBuffer.Get();
    restoreBarriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restoreBarriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    restoreBarriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(4, restoreBarriers);

    hr = m_commandList->Close();
    THROW_IF_FAILED(hr, "Failed to close Command List");

    // Execute the recorded list and wait for CPU synchronization
    m_context.ExecuteCommandList(m_commandList.Get());
    m_context.FlushGPU();

    // 4. Map output host memory and retrieve frame results
    void* mappedOutput = nullptr;
    hr = m_outputReadbackBuffer->Map(0, nullptr, &mappedOutput);
    THROW_IF_FAILED(hr, "Failed to map output readback buffer");
    memcpy(outputRGBA, mappedOutput, m_tensorSizeInBytes);
    m_outputReadbackBuffer->Unmap(0, nullptr);
}

void ColorGradeEffect::ProcessFrameGPUTexture(
    ID3D12Resource* inputBuffer,
    ID3D12Resource* outputBuffer,
    const ColorGradeParameters& params
) {
    // 1. Ensure initialization
    CompileOperatorGraph(params);
    AllocateGPUResources();
    UpdateConstantTensors(params);

    // 2. Open command list and record commands
    HRESULT hr = m_commandAllocator->Reset();
    THROW_IF_FAILED(hr, "Failed to reset Command Allocator");

    hr = m_commandList->Reset(m_commandAllocator.Get(), nullptr);
    THROW_IF_FAILED(hr, "Failed to reset Command List");

    // Copy constant upload buffers to GPU default buffers
    m_commandList->CopyBufferRegion(m_scaleGPUBuffer.Get(), 0, m_scaleUploadBuffer.Get(), 0, 16);
    m_commandList->CopyBufferRegion(m_biasGPUBuffer.Get(), 0, m_biasUploadBuffer.Get(), 0, 16);

    // Transition constant buffers and input/output resources to proper states
    D3D12_RESOURCE_BARRIER barriers[4] = {};
    
    // Scale GPU Buffer transition
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = m_scaleGPUBuffer.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Bias GPU Buffer transition
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = m_biasGPUBuffer.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Input Pool Buffer transition (COMMON -> UNORDERED_ACCESS)
    barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[2].Transition.pResource = inputBuffer;
    barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Output Pool Buffer transition (COMMON -> UNORDERED_ACCESS)
    barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[3].Transition.pResource = outputBuffer;
    barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(4, barriers);

    // Dynamic binding for the passed input and output buffers
    DML_BUFFER_BINDING inputBindings[3] = {};
    inputBindings[0] = { inputBuffer, 0, m_tensorSizeInBytes };
    inputBindings[1] = { m_scaleGPUBuffer.Get(), 0, 16 };
    inputBindings[2] = { m_biasGPUBuffer.Get(), 0, 16 };

    DML_BINDING_DESC inputBindingDescs[3] = {};
    inputBindingDescs[0] = { DML_BINDING_TYPE_BUFFER, &inputBindings[0] };
    inputBindingDescs[1] = { DML_BINDING_TYPE_BUFFER, &inputBindings[1] };
    inputBindingDescs[2] = { DML_BINDING_TYPE_BUFFER, &inputBindings[2] };
    m_bindingTable->BindInputs(3, inputBindingDescs);

    DML_BUFFER_BINDING outputBinding = { outputBuffer, 0, m_tensorSizeInBytes };
    DML_BINDING_DESC outputBindingDesc = { DML_BINDING_TYPE_BUFFER, &outputBinding };
    m_bindingTable->BindOutputs(1, &outputBindingDesc);

    // Bind temporary buffer (if required)
    DML_BINDING_PROPERTIES bindingProps = m_compiledOperator->GetBindingProperties();
    if (m_temporaryBuffer) {
        DML_BUFFER_BINDING tempBinding = { m_temporaryBuffer.Get(), 0, bindingProps.TemporaryResourceSize };
        DML_BINDING_DESC bindingDesc = { DML_BINDING_TYPE_BUFFER, &tempBinding };
        m_bindingTable->BindTemporaryResource(&bindingDesc);
    }

    // Set descriptor heap
    if (m_descriptorHeap) {
        ID3D12DescriptorHeap* heaps[] = { m_descriptorHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
    }

    // Record DirectML execution dispatch
    ComPtr<IDMLCommandRecorder> recorder;
    hr = m_context.GetDMLDevice()->CreateCommandRecorder(IID_PPV_ARGS(&recorder));
    THROW_IF_FAILED(hr, "Failed to create DirectML Command Recorder");

    recorder->RecordDispatch(
        m_commandList.Get(),
        m_compiledOperator.Get(),
        m_bindingTable.Get()
    );

    // Transition constant buffers and input/output resources back
    D3D12_RESOURCE_BARRIER restoreBarriers[4] = {};
    
    restoreBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarriers[0].Transition.pResource = m_scaleGPUBuffer.Get();
    restoreBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restoreBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    restoreBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    restoreBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarriers[1].Transition.pResource = m_biasGPUBuffer.Get();
    restoreBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restoreBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    restoreBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    restoreBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarriers[2].Transition.pResource = inputBuffer;
    restoreBarriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restoreBarriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    restoreBarriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    restoreBarriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarriers[3].Transition.pResource = outputBuffer;
    restoreBarriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    restoreBarriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    restoreBarriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    m_commandList->ResourceBarrier(4, restoreBarriers);

    hr = m_commandList->Close();
    THROW_IF_FAILED(hr, "Failed to close Command List");

    // Execute the recorded list. NO Flushes or host stalls!
    m_context.ExecuteCommandList(m_commandList.Get());
}

void ColorGradeEffect::ProcessFrameCPU(
    const float* inputRGBA,
    float* outputRGBA,
    const ColorGradeParameters& params
) {
    float E = std::pow(2.0f, params.exposure);
    float C = params.contrast;
    float S = params.saturation;
    float temp = params.temperature;
    float tint = params.tint;

    // Final scales
    float scale_r = E * C * S;
    float scale_g = E * C * S;
    float scale_b = E * C * S;
    float scale_a = 1.0f; // Alpha is untouched

    // Final biases
    float bias_r = 0.5f * (1.0f - C * S) + temp * S;
    float bias_g = 0.5f * (1.0f - C * S) + tint * S;
    float bias_b = 0.5f * (1.0f - C * S) - temp * S;
    float bias_a = 0.0f;

#if defined(__arm64__) || defined(_M_ARM64)
    float E_val = std::pow(2.0f, params.exposure);
    float C_val = params.contrast;
    float temp_offset = params.temperature * 0.1f;
    float tint_offset = params.tint * 0.1f;
    float sat = params.saturation;

    float32x4_t v_exp     = vdupq_n_f32(E_val);
    float32x4_t v_contrast = vdupq_n_f32(C_val);
    float32x4_t v_half    = vdupq_n_f32(0.5f);

    // Temp & tint offsets applied per-channel: [+temp, +tint, -temp, 0]
    float offset_arr[4] = { temp_offset, tint_offset, -temp_offset, 0.0f };
    float32x4_t v_offsets = vld1q_f32(offset_arr);

    // REC.709 luma weights [R, G, B, 0]
    float luma_arr[4] = { 0.2126f, 0.7152f, 0.0722f, 0.0f };
    float32x4_t v_luma_weights = vld1q_f32(luma_arr);

    float32x4_t v_sat  = vdupq_n_f32(sat);
    float32x4_t v_zero = vdupq_n_f32(0.0f);
    float32x4_t v_one  = vdupq_n_f32(1.0f);

    for (uint32_t i = 0; i < m_pixelCount; ++i) {
        float32x4_t pixel = vld1q_f32(inputRGBA + i * 4);

        // Step 1: Exposure — pixel *= 2^exposure
        pixel = vmulq_f32(pixel, v_exp);

        // Step 2: Contrast — pixel = (pixel - 0.5) * contrast + 0.5
        pixel = vsubq_f32(pixel, v_half);
        pixel = vmlaq_f32(v_half, pixel, v_contrast);

        // Step 3: Temperature & Tint — add channel offsets
        pixel = vaddq_f32(pixel, v_offsets);

        // Step 4: Saturation — lerp(luma, pixel, saturation)
        float32x4_t v_weighted = vmulq_f32(pixel, v_luma_weights);
        float luma = vgetq_lane_f32(v_weighted, 0)
                   + vgetq_lane_f32(v_weighted, 1)
                   + vgetq_lane_f32(v_weighted, 2);
        float32x4_t v_luma = vdupq_n_f32(luma);
        pixel = vmlaq_f32(v_luma, vsubq_f32(pixel, v_luma), v_sat);

        // Restore original alpha (saturation must not touch alpha)
        pixel = vsetq_lane_f32(inputRGBA[i * 4 + 3], pixel, 3);

        // Step 5: Clamp [0, 1]
        pixel = vmaxq_f32(pixel, v_zero);
        pixel = vminq_f32(pixel, v_one);

        vst1q_f32(outputRGBA + i * 4, pixel);
    }
#else
    // Scalar fallback
    uint32_t totalPixels = m_pixelCount;
    for (uint32_t i = 0; i < totalPixels; ++i) {
        float r = inputRGBA[i * 4 + 0];
        float g = inputRGBA[i * 4 + 1];
        float b = inputRGBA[i * 4 + 2];
        float a = inputRGBA[i * 4 + 3];

        r = r * scale_r + bias_r;
        g = g * scale_g + bias_g;
        b = b * scale_b + bias_b;
        a = a * scale_a + bias_a;

        outputRGBA[i * 4 + 0] = std::max(0.0f, std::min(1.0f, r));
        outputRGBA[i * 4 + 1] = std::max(0.0f, std::min(1.0f, g));
        outputRGBA[i * 4 + 2] = std::max(0.0f, std::min(1.0f, b));
        outputRGBA[i * 4 + 3] = std::max(0.0f, std::min(1.0f, a));
    }
#endif
}
