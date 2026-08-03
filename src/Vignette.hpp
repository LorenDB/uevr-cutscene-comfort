// A soft edged rectangular mask drawn into the double wide backbuffer just
// before UEVR copies it into the OpenXR eye swapchains. The center stays
// clear so the cutscene shows through in full stereo 3D, the edges fade to
// black. Nothing about the scene render changes, so stereo depth and head
// translation keep working normally inside the aperture.
//
// The backbuffer is the right target here specifically because that is what
// D3D12Component copies from: left eye is the left half, right eye is the
// right half.
//
// The mask is one full viewport quad per eye whose pixel shader evaluates a
// rounded rectangle distance field. Doing it in the shader rather than
// stitching gradient quads keeps the falloff even all the way around,
// corners included, and leaves nothing to seam.
//
// This is deliberately self contained D3D12: its own root signature, shaders,
// vertex buffer and fence. It borrows nothing from the game or from UEVR's
// imgui plumbing, so it cannot fight either of them over state.

#pragma once

#include <cstdint>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "VignetteParams.hpp"

class Vignette {
public:
    // Both backends share one parameter block and one shader so they cannot
    // drift into looking different from each other.
    using Layout = VignetteParams::Layout;
    using Params = VignetteParams;

    // Records into a command list the caller owns, rather than building and
    // submitting our own. Submitting separately races with whatever the host
    // is recording at the same time, and gets the resource states wrong,
    // because our work would reach the GPU before theirs. Recording into
    // their list keeps everything in one ordered stream.
    //
    // `target_state` is the state the resource is in at this point in the
    // list, which we transition away from and put back.
    //
    // Render thread only. Returns false when something was not ready, which
    // is fine to ignore for a frame or two.
    bool draw(ID3D12GraphicsCommandList* cmd_list, ID3D12Resource* target, const Params& params,
        D3D12_RESOURCE_STATES target_state = D3D12_RESOURCE_STATE_PRESENT);

    // Drop everything device related. Safe to call from any thread as long
    // as draw is not running at the same time.
    void reset();

    ~Vignette() { reset(); }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // Two render target views per draw, and we rotate through the heap so a
    // command list still running on the GPU never has the descriptors it
    // references rewritten underneath it. Sixteen slots is eight draws of
    // headroom, far more than the two or three frames that are ever in
    // flight.
    static constexpr uint32_t RTV_SLOTS = 16;

    bool ensure_device_objects(ID3D12Device* device, DXGI_FORMAT format);

    // Everything here is device level and immutable once built, so there is
    // no per frame state to synchronize and nothing the GPU can still be
    // reading when we touch it again.
    ComPtr<ID3D12Device> m_device{};
    ComPtr<ID3D12RootSignature> m_root_signature{};
    ComPtr<ID3D12PipelineState> m_pso{};
    ComPtr<ID3D12DescriptorHeap> m_rtv_heap{};
    ComPtr<ID3D12Resource> m_vertex_buffer{};

    D3D12_VERTEX_BUFFER_VIEW m_vbv{};
    DXGI_FORMAT m_pso_format{DXGI_FORMAT_UNKNOWN};
    uint32_t m_rtv_slot{0};
};
