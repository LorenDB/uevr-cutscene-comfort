#include "VignetteD3D11.hpp"

#include <algorithm>
#include <d3dcompiler.h>

namespace {

// Everything the immediate context loses when we draw, so it can be handed
// back exactly as found. D3D11 state is global, and a plugin that quietly
// leaves a blend state behind will produce bugs in the host that look like
// anything but a plugin.
struct ContextState {
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv{nullptr};
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewport_count{D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
    ID3D11BlendState* blend{nullptr};
    FLOAT blend_factor[4]{};
    UINT sample_mask{0};
    ID3D11DepthStencilState* depth{nullptr};
    UINT stencil_ref{0};
    ID3D11RasterizerState* raster{nullptr};
    ID3D11InputLayout* layout{nullptr};
    ID3D11VertexShader* vs{nullptr};
    ID3D11PixelShader* ps{nullptr};
    ID3D11Buffer* vs_cb{nullptr};
    ID3D11Buffer* ps_cb{nullptr};
    ID3D11Buffer* vb{nullptr};
    UINT vb_stride{0};
    UINT vb_offset{0};
    D3D11_PRIMITIVE_TOPOLOGY topology{D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};

    void capture(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        c->RSGetViewports(&viewport_count, viewports);
        c->OMGetBlendState(&blend, blend_factor, &sample_mask);
        c->OMGetDepthStencilState(&depth, &stencil_ref);
        c->RSGetState(&raster);
        c->IAGetInputLayout(&layout);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->VSGetConstantBuffers(0, 1, &vs_cb);
        c->PSGetConstantBuffers(0, 1, &ps_cb);
        c->IAGetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
        c->IAGetPrimitiveTopology(&topology);
    }

    void restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
        if (viewport_count > 0) {
            c->RSSetViewports(viewport_count, viewports);
        }
        c->OMSetBlendState(blend, blend_factor, sample_mask);
        c->OMSetDepthStencilState(depth, stencil_ref);
        c->RSSetState(raster);
        c->IASetInputLayout(layout);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &vs_cb);
        c->PSSetConstantBuffers(0, 1, &ps_cb);
        c->IASetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
        c->IASetPrimitiveTopology(topology);

        // OMGetRenderTargets and friends all hand back a reference.
        for (auto* rtv : rtvs) {
            if (rtv != nullptr) rtv->Release();
        }
        if (dsv != nullptr) dsv->Release();
        if (blend != nullptr) blend->Release();
        if (depth != nullptr) depth->Release();
        if (raster != nullptr) raster->Release();
        if (layout != nullptr) layout->Release();
        if (vs != nullptr) vs->Release();
        if (ps != nullptr) ps->Release();
        if (vs_cb != nullptr) vs_cb->Release();
        if (ps_cb != nullptr) ps_cb->Release();
        if (vb != nullptr) vb->Release();
    }
};

} // namespace

bool VignetteD3D11::ensure_device_objects(ID3D11Device* device) {
    if (m_device.Get() == device && m_ps != nullptr) {
        return true;
    }

    if (m_device.Get() != device) {
        reset();
        m_device = device;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob{};

    if (FAILED(D3DCompile(k_vignette_shader, sizeof(k_vignette_shader) - 1, nullptr, nullptr, nullptr,
            "vs_main", "vs_4_0", 0, 0, &vs_blob, nullptr))) {
        return false;
    }
    if (FAILED(D3DCompile(k_vignette_shader, sizeof(k_vignette_shader) - 1, nullptr, nullptr, nullptr,
            "ps_main", "ps_4_0", 0, 0, &ps_blob, nullptr))) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_vs))) {
        return false;
    }
    if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_ps))) {
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };

    if (FAILED(device->CreateInputLayout(layout, 1, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_layout))) {
        return false;
    }

    // Two triangles covering NDC. Never changes.
    const float verts[]{
        -1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f,  1.0f,
         1.0f, -1.0f,
    };

    D3D11_BUFFER_DESC vb_desc{};
    vb_desc.ByteWidth = sizeof(verts);
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vb_data{};
    vb_data.pSysMem = verts;

    if (FAILED(device->CreateBuffer(&vb_desc, &vb_data, &m_vertex_buffer))) {
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(VignetteConstants);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &m_constant_buffer))) {
        return false;
    }

    D3D11_BLEND_DESC blend_desc{};
    auto& rt = blend_desc.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(device->CreateBlendState(&blend_desc, &m_blend))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = FALSE;
    depth_desc.StencilEnable = FALSE;

    if (FAILED(device->CreateDepthStencilState(&depth_desc, &m_depth))) {
        return false;
    }

    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = FALSE;

    if (FAILED(device->CreateRasterizerState(&raster_desc, &m_raster))) {
        return false;
    }

    return true;
}

bool VignetteD3D11::ensure_rtv(ID3D11Device* device, ID3D11Texture2D* target) {
    if (m_rtv != nullptr && m_rtv_source == target) {
        return true;
    }

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    ComPtr<ID3D11RenderTargetView> rtv{};
    if (FAILED(device->CreateRenderTargetView(target, &rtv_desc, &rtv))) {
        // A typeless backbuffer needs an explicit format, so let the runtime
        // pick one by passing no description at all.
        if (FAILED(device->CreateRenderTargetView(target, nullptr, &rtv))) {
            return false;
        }
    }

    m_rtv = rtv;
    m_rtv_source = target;
    return true;
}

bool VignetteD3D11::draw(ID3D11DeviceContext* context, ID3D11Texture2D* target, const VignetteParams& params) {
    if (context == nullptr || target == nullptr || params.alpha <= 0.001f) {
        return false;
    }

    ComPtr<ID3D11Device> device{};
    context->GetDevice(&device);
    if (device == nullptr) {
        return false;
    }

    if (!ensure_device_objects(device.Get()) || !ensure_rtv(device.Get(), target)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        return false;
    }

    ContextState saved{};
    saved.capture(context);

    const bool stereo = params.layout == VignetteParams::Layout::DOUBLE_WIDE;
    const auto eye_count = stereo ? 2u : 1u;
    const auto eye_width = stereo ? (float)desc.Width * 0.5f : (float)desc.Width;
    const auto height = (float)desc.Height;

    ID3D11RenderTargetView* rtv = m_rtv.Get();
    context->OMSetRenderTargets(1, &rtv, nullptr);

    const FLOAT blend_factor[4]{0.0f, 0.0f, 0.0f, 0.0f};
    context->OMSetBlendState(m_blend.Get(), blend_factor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depth.Get(), 0);
    context->RSSetState(m_raster.Get());

    context->IASetInputLayout(m_layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11Buffer* vb = m_vertex_buffer.Get();
    UINT stride = sizeof(float) * 2;
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);

    for (uint32_t eye = 0; eye < eye_count; ++eye) {
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = (stereo && eye == 1) ? eye_width : 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = eye_width;
        viewport.Height = height;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        VignetteConstants constants{};
        if (!build_vignette_constants(params, eye, constants)) {
            saved.restore(context);
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(m_constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(m_constant_buffer.Get(), 0);
        }

        ID3D11Buffer* cb = m_constant_buffer.Get();
        context->PSSetConstantBuffers(0, 1, &cb);

        context->Draw(6, 0);
    }

    saved.restore(context);
    return true;
}

void VignetteD3D11::reset() {
    m_rtv.Reset();
    m_rtv_source = nullptr;
    m_raster.Reset();
    m_depth.Reset();
    m_blend.Reset();
    m_constant_buffer.Reset();
    m_vertex_buffer.Reset();
    m_layout.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_device.Reset();
}
