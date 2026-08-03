// Local copy of UEVR example renderlib's D3D12 implementation.
//
// This is compiled into CutsceneComfort.dll; UEVR itself is not linked or
// rebuilt. The upstream helper currently submits its desktop command list
// twice. That was mostly hidden while the plugin rendered to only one output,
// but is unsafe now that the desktop mirror remains active alongside VR.

#include "uevr/API.hpp"
#include "imgui/imgui_impl_dx12.h"
#include "rendering/d3d12.hpp"

D3D12 g_d3d12{};

bool D3D12::initialize() {
    const auto renderer_data = uevr::API::get()->param()->renderer;
    auto device = static_cast<ID3D12Device*>(renderer_data->device);
    auto swapchain = static_cast<IDXGISwapChain3*>(renderer_data->swapchain);

    for (auto& cmd : cmds) {
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd.allocator)))) {
            uevr::API::get()->log_error("[CutsceneComfort] Failed to create D3D12 command allocator");
            return false;
        }

        cmd.allocator->SetName(L"CutsceneComfort desktop command allocator");

        if (FAILED(device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                cmd.allocator.Get(),
                nullptr,
                IID_PPV_ARGS(&cmd.list)))) {
            uevr::API::get()->log_error("[CutsceneComfort] Failed to create D3D12 command list");
            return false;
        }

        cmd.list->SetName(L"CutsceneComfort desktop command list");

        if (FAILED(cmd.list->Close())) {
            uevr::API::get()->log_error("[CutsceneComfort] Failed to close initial D3D12 command list");
            return false;
        }

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cmd.fence)))) {
            uevr::API::get()->log_error("[CutsceneComfort] Failed to create D3D12 fence");
            return false;
        }

        cmd.fence->SetName(L"CutsceneComfort desktop fence");
        cmd.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (cmd.fence_event == nullptr) {
            uevr::API::get()->log_error("[CutsceneComfort] Failed to create D3D12 fence event");
            return false;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = static_cast<UINT>(D3D12::RTV::COUNT);
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 1;

        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_desc_heap)))) {
            return false;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = static_cast<UINT>(D3D12::SRV::COUNT);
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srv_desc_heap)))) {
            return false;
        }
    }

    for (auto i = 0; i <= static_cast<int>(D3D12::RTV::BACKBUFFER_3); ++i) {
        if (SUCCEEDED(swapchain->GetBuffer(i, IID_PPV_ARGS(&rts[i])))) {
            device->CreateRenderTargetView(
                rts[i].Get(), nullptr, get_cpu_rtv(device, static_cast<D3D12::RTV>(i)));
        }
    }

    auto& backbuffer = get_rt(D3D12::RTV::BACKBUFFER_0);
    if (backbuffer == nullptr) {
        return false;
    }

    const auto desc = backbuffer->GetDesc();
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = desc.Format;

    if (FAILED(device->CreateCommittedResource(
            &props,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear_value,
            IID_PPV_ARGS(&get_rt(D3D12::RTV::IMGUI))))) {
        return false;
    }

    if (FAILED(device->CreateCommittedResource(
            &props,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear_value,
            IID_PPV_ARGS(&get_rt(D3D12::RTV::BLANK))))) {
        return false;
    }

    device->CreateRenderTargetView(
        get_rt(D3D12::RTV::IMGUI).Get(), nullptr, get_cpu_rtv(device, D3D12::RTV::IMGUI));
    device->CreateRenderTargetView(
        get_rt(D3D12::RTV::BLANK).Get(), nullptr, get_cpu_rtv(device, D3D12::RTV::BLANK));
    device->CreateShaderResourceView(
        get_rt(D3D12::RTV::IMGUI).Get(), nullptr, get_cpu_srv(device, D3D12::SRV::IMGUI));
    device->CreateShaderResourceView(
        get_rt(D3D12::RTV::BLANK).Get(), nullptr, get_cpu_srv(device, D3D12::SRV::BLANK));

    rt_width = static_cast<uint32_t>(desc.Width);
    rt_height = desc.Height;

    return ImGui_ImplDX12_Init(
        device,
        1,
        desc.Format,
        srv_desc_heap.Get(),
        get_cpu_srv(device, D3D12::SRV::IMGUI_FONT),
        get_gpu_srv(device, D3D12::SRV::IMGUI_FONT));
}

void D3D12::render_imgui() {
    auto draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr) {
        return;
    }

    auto& cmd = cmds[frame_count++ % cmds.size()];
    if (cmd.fence_event != nullptr && cmd.fence != nullptr &&
        cmd.fence->GetCompletedValue() < cmd.fence_value) {
        WaitForSingleObject(cmd.fence_event, 2000);
        ResetEvent(cmd.fence_event);
    }

    const auto renderer_data = uevr::API::get()->param()->renderer;
    auto command_queue = static_cast<ID3D12CommandQueue*>(renderer_data->command_queue);
    auto device = static_cast<ID3D12Device*>(renderer_data->device);
    auto swapchain = static_cast<IDXGISwapChain3*>(renderer_data->swapchain);
    if (command_queue == nullptr || device == nullptr || swapchain == nullptr) {
        return;
    }

    if (FAILED(cmd.allocator->Reset()) ||
        FAILED(cmd.list->Reset(cmd.allocator.Get(), nullptr))) {
        uevr::API::get()->log_error("[CutsceneComfort] Failed to reset D3D12 desktop commands");
        return;
    }

    const auto bb_index = swapchain->GetCurrentBackBufferIndex();
    if (bb_index >= static_cast<UINT>(D3D12::RTV::IMGUI) || rts[bb_index] == nullptr) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rts[bb_index].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd.list->ResourceBarrier(1, &barrier);

    const auto rtv = get_cpu_rtv(device, static_cast<D3D12::RTV>(bb_index));
    cmd.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[]{srv_desc_heap.Get()};
    cmd.list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(draw_data, cmd.list.Get());

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd.list->ResourceBarrier(1, &barrier);

    if (FAILED(cmd.list->Close())) {
        uevr::API::get()->log_error("[CutsceneComfort] Failed to close D3D12 desktop command list");
        return;
    }

    // Exactly one submission. The upstream example currently executes this
    // same list twice, which can race allocator reuse and duplicate UI work.
    ID3D12CommandList* const command_lists[]{cmd.list.Get()};
    command_queue->ExecuteCommandLists(1, command_lists);
    command_queue->Signal(cmd.fence.Get(), ++cmd.fence_value);
    cmd.fence->SetEventOnCompletion(cmd.fence_value, cmd.fence_event);
}

void D3D12::render_imgui_vr(
    ID3D12GraphicsCommandList* command_list,
    D3D12_CPU_DESCRIPTOR_HANDLE* rtv) {
    if (command_list == nullptr || rtv == nullptr) {
        return;
    }

    auto draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr) {
        return;
    }

    command_list->OMSetRenderTargets(1, rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[]{srv_desc_heap.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(draw_data, command_list);
}
