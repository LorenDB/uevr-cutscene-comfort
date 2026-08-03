// The same theater frame as Vignette, for games running D3D11.
//
// It draws the identical rounded rectangle distance field, so the two paths
// look the same, but D3D11 needs none of the command list and descriptor
// machinery the D3D12 version carries. What it does need is manners: the
// immediate context is shared global state, so every piece of it this touches
// gets saved on the way in and put back on the way out.

#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "VignetteParams.hpp"

class VignetteD3D11 {
public:
    // Draws into `target`, which is expected to be the swapchain backbuffer
    // holding both eyes side by side. Returns false when something was not
    // ready, which is fine to ignore for a frame or two.
    bool draw(ID3D11DeviceContext* context, ID3D11Texture2D* target, const VignetteParams& params);

    void reset();

    ~VignetteD3D11() { reset(); }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool ensure_device_objects(ID3D11Device* device);
    bool ensure_rtv(ID3D11Device* device, ID3D11Texture2D* target);

    ComPtr<ID3D11Device> m_device{};
    ComPtr<ID3D11VertexShader> m_vs{};
    ComPtr<ID3D11PixelShader> m_ps{};
    ComPtr<ID3D11InputLayout> m_layout{};
    ComPtr<ID3D11Buffer> m_vertex_buffer{};
    ComPtr<ID3D11Buffer> m_constant_buffer{};
    ComPtr<ID3D11BlendState> m_blend{};
    ComPtr<ID3D11DepthStencilState> m_depth{};
    ComPtr<ID3D11RasterizerState> m_raster{};

    ComPtr<ID3D11RenderTargetView> m_rtv{};
    ID3D11Texture2D* m_rtv_source{nullptr};
};
