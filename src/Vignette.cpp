#include "Vignette.hpp"

#include <algorithm>
#include <d3dcompiler.h>

namespace {

// The shader itself lives in VignetteParams.hpp, shared with the D3D11
// backend so the two cannot drift into looking different.

DXGI_FORMAT resolve_typeless(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

} // namespace

bool Vignette::ensure_device_objects(ID3D12Device* device, DXGI_FORMAT format) {
    if (m_device.Get() == device && m_pso != nullptr && m_pso_format == format) {
        return true;
    }

    if (m_device.Get() != device) {
        reset();
        m_device = device;
    }

    if (m_root_signature == nullptr) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;
        param.Constants.Num32BitValues = sizeof(VignetteConstants) / sizeof(float);
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 1;
        rs_desc.pParameters = &param;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serialized{};
        if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr))) {
            return false;
        }

        if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&m_root_signature)))) {
            return false;
        }
    }

    ComPtr<ID3DBlob> vs{};
    ComPtr<ID3DBlob> ps{};
    if (FAILED(D3DCompile(k_vignette_shader, sizeof(k_vignette_shader) - 1, nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs, nullptr))) {
        return false;
    }
    if (FAILED(D3DCompile(k_vignette_shader, sizeof(k_vignette_shader) - 1, nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps, nullptr))) {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC layout[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_root_signature.Get();
    pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso_desc.InputLayout = {layout, _countof(layout)};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = format;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleMask = UINT_MAX;

    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = FALSE;

    auto& blend = pso_desc.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D12PipelineState> pso{};
    if (FAILED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)))) {
        return false;
    }

    m_pso = pso;
    m_pso_format = format;

    if (m_rtv_heap == nullptr) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = RTV_SLOTS;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_rtv_heap)))) {
            return false;
        }
    }

    if (m_vertex_buffer == nullptr) {
        // Two triangles covering NDC. Never changes, so upload it once.
        const float verts[]{
            -1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f,
            -1.0f, -1.0f,
             1.0f,  1.0f,
             1.0f, -1.0f,
        };

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = sizeof(verts);
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.Format = DXGI_FORMAT_UNKNOWN;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &buf_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vertex_buffer)))) {
            return false;
        }

        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        if (FAILED(m_vertex_buffer->Map(0, &no_read, &mapped))) {
            return false;
        }
        memcpy(mapped, verts, sizeof(verts));
        m_vertex_buffer->Unmap(0, nullptr);

        m_vbv.BufferLocation = m_vertex_buffer->GetGPUVirtualAddress();
        m_vbv.SizeInBytes = sizeof(verts);
        m_vbv.StrideInBytes = sizeof(float) * 2;
    }

    return true;
}


bool Vignette::draw(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* target, const Params& params,
    D3D12_RESOURCE_STATES target_state) {

    if (cmd_list == nullptr || target == nullptr || params.alpha <= 0.001f) {
        return false;
    }

    ComPtr<ID3D12Device> device{};
    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device)))) {
        return false;
    }

    const auto desc = target->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0) {
        return false;
    }

    const auto format = resolve_typeless(desc.Format);
    if (!ensure_device_objects(device.Get(), format)) {
        return false;
    }

    const bool stereo = params.layout == Layout::DOUBLE_WIDE;
    // A 2 slice array means one eye per slice, otherwise the eyes sit side
    // by side in one texture and the viewport picks which half we touch.
    const bool is_array = stereo && desc.DepthOrArraySize >= 2;

    // Fresh descriptors from the ring every draw. Reusing the same two slots
    // would rewrite descriptors that a command list from a previous frame may
    // still be executing against, which corrupts the GPU's view of them.
    const auto rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base_slot = m_rtv_slot;
    m_rtv_slot = (m_rtv_slot + 2) % RTV_SLOTS;

    D3D12_CPU_DESCRIPTOR_HANDLE eye_rtv[2]{};

    for (uint32_t i = 0; i < 2; ++i) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
        rtv_desc.Format = format;

        if (is_array) {
            rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv_desc.Texture2DArray.FirstArraySlice = i;
            rtv_desc.Texture2DArray.ArraySize = 1;
        } else {
            rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        }

        eye_rtv[i] = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        eye_rtv[i].ptr += ((base_slot + i) % RTV_SLOTS) * rtv_stride;
        device->CreateRenderTargetView(target, &rtv_desc, eye_rtv[i]);
    }

    // The caller owns the resource state, so borrow it and hand it back
    // exactly as we found it.
    const bool needs_transition = target_state != D3D12_RESOURCE_STATE_RENDER_TARGET;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (needs_transition) {
        barrier.Transition.StateBefore = target_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd_list->ResourceBarrier(1, &barrier);
    }

    // We are borrowing the host's list, so set every piece of state we rely
    // on rather than assuming anything about what they left bound.
    cmd_list->SetPipelineState(m_pso.Get());
    cmd_list->SetGraphicsRootSignature(m_root_signature.Get());
    cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd_list->IASetVertexBuffers(0, 1, &m_vbv);

    const auto eye_count = stereo ? 2u : 1u;
    const auto eye_width = (stereo && !is_array) ? (float)desc.Width * 0.5f : (float)desc.Width;
    const auto height = (float)desc.Height;

    for (uint32_t eye = 0; eye < eye_count; ++eye) {
        // A double wide target is one texture, so both eyes share the first
        // view and the viewport is what picks the half.
        cmd_list->OMSetRenderTargets(1, &eye_rtv[is_array ? eye : 0], FALSE, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = (stereo && !is_array && eye == 1) ? eye_width : 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = eye_width;
        viewport.Height = height;
        viewport.MaxDepth = 1.0f;
        cmd_list->RSSetViewports(1, &viewport);

        D3D12_RECT scissor{};
        scissor.left = (LONG)viewport.TopLeftX;
        scissor.top = 0;
        scissor.right = (LONG)(viewport.TopLeftX + eye_width);
        scissor.bottom = (LONG)height;
        cmd_list->RSSetScissorRects(1, &scissor);

        VignetteConstants constants{};
        if (!build_vignette_constants(params, eye, constants)) {
            return false;
        }

        cmd_list->SetGraphicsRoot32BitConstants(0, sizeof(VignetteConstants) / sizeof(float), &constants, 0);
        cmd_list->DrawInstanced(6, 1, 0, 0);
    }

    if (needs_transition) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = target_state;
        cmd_list->ResourceBarrier(1, &barrier);
    }

    // The list belongs to the caller, so it is theirs to close and submit.
    return true;
}

void Vignette::reset() {
    m_vertex_buffer.Reset();
    m_rtv_heap.Reset();
    m_pso.Reset();
    m_root_signature.Reset();
    m_device.Reset();

    m_vbv = {};
    m_pso_format = DXGI_FORMAT_UNKNOWN;
    m_rtv_slot = 0;
}
