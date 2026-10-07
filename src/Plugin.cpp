// CutsceneComfort, a UEVR plugin.
//
// When a game hands the camera to a cinematic, whatever VR camera tweaks the
// player has dialed in (forward offset, height, decoupled pitch) suddenly
// apply to a camera the director framed very deliberately. The result is a
// headset floating above the shot or clipping through an actor's face.
//
// This plugin watches what the engine is actually rendering from, via
// PlayerController -> PlayerCameraManager -> ViewTarget.Target. When the view
// target becomes a CameraActor (CineCameraActor included), we are in a
// cutscene: the camera offsets get zeroed and decoupled pitch is flattened so
// the player sees the shot as intended. When gameplay takes the camera back,
// everything is eased back to how it was.
//
// Optionally, the same detection fades the headset view through black and
// hands the shot to UEVR's 2D screen: a fixed camera on a quad in front of
// the player. The default keeps the per-eye stereo portal. Flat 2D feeds
// both eyes one picture. The screen fades back to tracked VR when the
// shot ends.
//
// A note on the detection choice: an earlier version of this idea checked
// whether a CineCameraActor existed anywhere in the level. Plenty of games
// keep one loaded at all times, so that fires constantly. Asking the camera
// manager for its actual view target is the same signal the engine uses to
// decide what to render, which is why it holds up across games.

// NOMINMAX comes in from the build so std::max survives Windows.h
#include <Windows.h>
#include <CommCtrl.h>
#include <windowsx.h>

#include <d3d11.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"

#include "uevr/Plugin.hpp"

#include "UnvrTransition.hpp"
#include "Vignette.hpp"
#include "VignetteD3D11.hpp"

using namespace uevr;

namespace {

std::string narrow(const std::wstring& ws) {
    if (ws.empty()) {
        return {};
    }

    const auto len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), out.data(), len, nullptr, nullptr);
    return out;
}

// get_mod_value<float> throws on an empty string, so parse by hand.
float read_mod_float(const char* key) {
    const auto s = API::VR::get_mod_value<std::string>(key);
    return s.empty() ? 0.0f : std::strtof(s.c_str(), nullptr);
}

float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

class CutsceneComfort : public uevr::Plugin {
public:
    ~CutsceneComfort() override {
        if (m_input_subclass_installed.load() && m_wnd != nullptr && IsWindow(m_wnd)) {
            RemoveWindowSubclass(m_wnd, window_subclass_proc, k_input_subclass_id);
        }
    }

    void on_initialize() override {
        API::get()->log_info("[CutsceneComfort] loaded");
        load_settings();
        detect_operator_window_bridge();
        publish_frame_settings();
    }

    // ------------------------------------------------------------------
    // Game thread: detection, state transitions, and building the UI frame
    // ------------------------------------------------------------------

    void on_pre_engine_tick(API::UGameEngine*, float delta) override {
        // Time our own work so the UI can show what the plugin actually
        // costs per tick. It should stay well under a tenth of a millisecond.
        LARGE_INTEGER t0{};
        QueryPerformanceCounter(&t0);

        // Nothing in here should throw, but this runs inside the engine's
        // tick and an escaped exception would be caught by UEVR with a vague
        // "one of the plugins has an error" message. Catch at the boundary
        // and log it as ours so problems are attributable.
        try {
            // The object tracker needs to be running before exists() means anything.
            if (!m_hook_activated) {
                API::UObjectHook::activate();
                m_hook_activated = true;
            }

            poll_settings_file(delta);

            // UEVR performs a second config load immediately after plugin
            // initialization. Recovering in on_initialize() looks successful
            // but that later load silently puts persisted zero offsets back.
            // Wait until engine ticks are underway, then recover before the
            // detector is allowed to enter a cutscene.
            if (!m_startup_recovery_done) {
                m_startup_recovery_timer += std::max(delta, 0.0f);
                if (m_startup_recovery_timer >= k_startup_recovery_delay_seconds) {
                    m_startup_recovery_safe = recover_parked_state();
                    m_startup_recovery_done = true;
                }
            }

            find_engine_classes(delta);
            if (m_startup_recovery_done && m_startup_recovery_safe) {
                poll_cutscene_state(delta);
                advance_unvr(delta);
            }
            advance_restore(delta);

            // The hotkey handler on the window thread only sets a flag, the
            // actual file write happens here on the game thread.
            if (m_settings_dirty.exchange(false)) {
                save_settings();
            }

            // The renderer has its own thread. Publish the handful of values
            // it needs through atomics rather than reading m_settings there.
            publish_frame_settings();
            ensure_menu_input_subclass();
        } catch (const std::exception& e) {
            if (++m_tick_errors <= 5) {
                API::get()->log_error("[CutsceneComfort] tick error: %s", e.what());
            }
        } catch (...) {
            if (++m_tick_errors <= 5) {
                API::get()->log_error("[CutsceneComfort] tick error: unknown exception");
            }
        }

        LARGE_INTEGER t1{};
        QueryPerformanceCounter(&t1);
        track_tick_cost(t0, t1);

        // The ready check has to happen under the lock. A device reset on the
        // render thread can tear the backends down between an early check and
        // the NewFrame call, and that is a crash.
        {
            std::scoped_lock _{m_imgui_mutex};

            if (m_imgui_ready) {
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                draw_ui();

                ImGui::EndFrame();
                ImGui::Render();
            }
        }
    }

    // ------------------------------------------------------------------
    // Render callbacks. The frame is built on the game thread above, then the
    // same immutable ImGui draw data is rendered to both the desktop
    // backbuffer and the VR UI target. Keeping both paths live means the
    // controls remain visible in recordings and on the monitor while an HMD
    // is active.
    // ------------------------------------------------------------------

    void on_present() override {
        std::scoped_lock _{m_imgui_mutex};

        if (!m_imgui_ready && !initialize_imgui()) {
            return;
        }

        const auto renderer = API::get()->param()->renderer;

        if (renderer->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_NewFrame();
            g_d3d11.render_imgui();
        } else if (renderer->renderer_type == UEVR_RENDERER_D3D12) {
            if (renderer->command_queue == nullptr) {
                return;
            }

            ImGui_ImplDX12_NewFrame();
            g_d3d12.render_imgui();
        }
    }

    void on_post_render_vr_framework_dx11(ID3D11DeviceContext* context, ID3D11Texture2D*, ID3D11RenderTargetView* rtv) override {
        draw_theater_frame_d3d11(context);

        if (!m_imgui_ready || !API::VR::is_hmd_active()) {
            return;
        }

        std::scoped_lock _{m_imgui_mutex};
        ImGui_ImplDX11_NewFrame();
        g_d3d11.render_imgui_vr(context, rtv);
    }

    void on_post_render_vr_framework_dx12(ID3D12GraphicsCommandList* command_list, ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE* rtv) override {
        draw_theater_frame(command_list);

        if (!m_imgui_ready || !API::VR::is_hmd_active()) {
            return;
        }

        std::scoped_lock _{m_imgui_mutex};
        ImGui_ImplDX12_NewFrame();
        g_d3d12.render_imgui_vr(command_list, rtv);
    }

    void on_device_reset() override {
        m_vignette.reset();
        m_vignette_d3d11.reset();
        invalidate_frame_anchor();

        std::scoped_lock _{m_imgui_mutex};

        if (!m_imgui_ready) {
            return;
        }

        const auto renderer = API::get()->param()->renderer;

        if (renderer->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_Shutdown();
            g_d3d11 = {};
        } else if (renderer->renderer_type == UEVR_RENDERER_D3D12) {
            g_d3d12.reset();
            ImGui_ImplDX12_Shutdown();
            g_d3d12 = {};
        }

        ImGui_ImplWin32_Shutdown();
        m_imgui_ready = false;
    }

    // Flat 2D collapses UEVR's remaining eye separation after the 2D screen
    // has stopped applying headset pose. The stereo portal leaves this alone,
    // so each eye keeps the offset UEVR already wrote.
    void on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle, int view_index, float,
        UEVR_Vector3f* position, UEVR_Rotatorf*, bool is_double) override {
        if (!m_unvr_flatten_stereo.load() || position == nullptr || view_index < 0) {
            m_mono_anchor.valid = false;
            return;
        }

        if (m_mono_anchor.valid && m_mono_anchor.view_index != view_index) {
            if (m_mono_anchor.is_double == is_double) {
                if (is_double) {
                    auto* wide = reinterpret_cast<UEVR_Vector3d*>(position);
                    wide->x = m_mono_anchor.x;
                    wide->y = m_mono_anchor.y;
                    wide->z = m_mono_anchor.z;
                } else {
                    position->x = static_cast<float>(m_mono_anchor.x);
                    position->y = static_cast<float>(m_mono_anchor.y);
                    position->z = static_cast<float>(m_mono_anchor.z);
                }
            }
            m_mono_anchor.valid = false;
            return;
        }

        m_mono_anchor.valid = true;
        m_mono_anchor.view_index = view_index;
        m_mono_anchor.is_double = is_double;
        if (is_double) {
            const auto* wide = reinterpret_cast<const UEVR_Vector3d*>(position);
            m_mono_anchor.x = wide->x;
            m_mono_anchor.y = wide->y;
            m_mono_anchor.z = wide->z;
        } else {
            m_mono_anchor.x = position->x;
            m_mono_anchor.y = position->y;
            m_mono_anchor.z = position->z;
        }
    }

    bool on_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override {
        // Hotkey first, it should work even while the window is hidden.
        // Bit 30 of lparam filters out key repeat.
        const auto hotkey = m_ui_hotkey_vk.load();
        if (msg == WM_KEYDOWN && hotkey != 0 && (int)wparam == hotkey && (lparam & (1 << 30)) == 0) {
            const auto* param = API::get()->param();
            const bool uevr_menu_open = param != nullptr &&
                param->functions != nullptr &&
                param->functions->is_drawing_ui != nullptr &&
                param->functions->is_drawing_ui();

            // While UEVR's menu is open the game-thread polling path owns
            // this hotkey. That path still works when UEVR consumes window
            // messages before they reach external plugins.
            if (!uevr_menu_open) {
                m_show_ui = !m_show_ui;
                m_settings_dirty = true;
            }
            return false; // swallow the keypress so the game does not react to it too
        }

        if (!m_imgui_ready) {
            return true;
        }

        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        return !ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard;
    }

private:
    // ------------------------------------------------------------------
    // Detection
    // ------------------------------------------------------------------

    // CameraActor covers CineCameraActor too, it is the parent class.
    void find_engine_classes(float delta) {
        if (m_camera_actor_class != nullptr) {
            return;
        }

        // No point hammering the name table every tick while the game is
        // still booting, retry every couple of seconds instead.
        m_class_retry_timer -= delta;
        if (m_class_retry_timer > 0.0f) {
            return;
        }
        m_class_retry_timer = 2.0f;

        m_camera_actor_class = API::get()->find_uobject<API::UClass>(L"Class /Script/Engine.CameraActor");
        if (m_camera_actor_class != nullptr) {
            API::get()->log_info("[CutsceneComfort] found Engine.CameraActor, detection is live");
        }
    }

    API::UObject* current_view_target() {
        const auto pc = API::get()->get_player_controller(0);
        if (pc == nullptr) {
            m_detect_note = "waiting for a player controller";
            return nullptr;
        }

        const auto mgr_slot = pc->get_property_data<API::UObject*>(L"PlayerCameraManager");
        if (mgr_slot == nullptr || *mgr_slot == nullptr) {
            m_detect_note = "controller has no camera manager yet";
            return nullptr;
        }

        // ViewTarget is an FTViewTarget struct. Its first member has been the
        // target actor pointer in every UE4 and UE5 build I have looked at,
        // and every UEVR script that reads ViewTarget.Target relies on that.
        const auto vt_struct = (*mgr_slot)->get_property_data(L"ViewTarget");
        if (vt_struct == nullptr) {
            m_detect_note = "camera manager has no ViewTarget property";
            return nullptr;
        }

        m_detect_note.clear();

        const auto target = *reinterpret_cast<API::UObject**>(vt_struct);
        if (target == nullptr) {
            return nullptr;
        }

        // The pointer can go stale for a frame or two during level travel.
        if (!API::UObjectHook::exists(target)) {
            return nullptr;
        }

        return target;
    }

    bool is_cinematic_target(API::UObject* target) {
        if (target == nullptr) {
            return false;
        }

        const auto pawn = API::get()->get_local_pawn(0);
        if (target == pawn) {
            return false;
        }

        const auto cls = target->get_class();
        if (cls == nullptr) {
            return false;
        }

        // Classify once per class, not once per tick. The name lookup below
        // allocates and there is no reason to repeat it 90 times a second.
        if (cls != m_last_class) {
            m_last_class = cls;
            m_last_class_cinematic = false;

            if (m_camera_actor_class != nullptr && target->is_a(m_camera_actor_class)) {
                m_last_class_cinematic = true;
            } else {
                // Some games roll their own camera blueprint instead of
                // subclassing CameraActor. Catch the obvious names.
                const auto name = narrow(cls->get_fname()->to_string());
                m_last_class_cinematic =
                    name.find("CineCamera") != std::string::npos ||
                    name.find("CinematicCamera") != std::string::npos ||
                    name.find("CutsceneCamera") != std::string::npos;
            }
        }

        if (m_last_class_cinematic) {
            return true;
        }

        // Loose mode: anything that is not the pawn counts. Off by default
        // because vehicles and turrets would trip it in a lot of games.
        return m_settings.loose_detection && pawn != nullptr;
    }

    void poll_cutscene_state(float delta) {
        const auto target = current_view_target();

        if (target != m_last_target) {
            m_last_target = target;
            update_target_label(target);
        }

        const bool now = m_simulate || is_cinematic_target(target);

        // Small debounce so a single odd frame during a camera blend or a
        // level load does not slam settings back and forth.
        if (now != m_candidate_state) {
            m_candidate_state = now;
            m_candidate_timer = 0.0f;
            return;
        }

        m_candidate_timer += delta;

        if (m_candidate_state != m_in_cutscene && m_candidate_timer >= k_debounce_seconds) {
            m_in_cutscene = m_candidate_state;

            if (m_in_cutscene) {
                enter_cutscene();
            } else {
                leave_cutscene();
            }
        }
    }

    void update_target_label(API::UObject* target) {
        if (target == nullptr) {
            m_target_label = "none";
            return;
        }

        const auto cls = target->get_class();
        m_target_label = narrow(target->get_fname()->to_string());

        if (cls != nullptr) {
            m_target_label += " (" + narrow(cls->get_fname()->to_string()) + ")";
        }
    }

    // ------------------------------------------------------------------
    // Applying and restoring the comfort changes
    // ------------------------------------------------------------------

    void enter_cutscene() {
        API::get()->log_info("[CutsceneComfort] cutscene started, view target: %s", m_target_label.c_str());
        request_frame_recenter();

        // If a new scene starts while we are still easing back from the last
        // one, keep the original baseline. Capturing mid ease would slowly
        // walk the player's offsets toward zero across a chain of cutscenes.
        if (!m_baseline.valid) {
            m_baseline.forward = read_mod_float("VR_CameraForwardOffset");
            m_baseline.right = read_mod_float("VR_CameraRightOffset");
            m_baseline.up = read_mod_float("VR_CameraUpOffset");
            m_baseline.decoupled_pitch = API::VR::is_decoupled_pitch_enabled();
            m_baseline.aim = API::VR::get_aim_method();
            m_baseline.hook_disabled = API::UObjectHook::is_disabled();
            m_baseline.valid = true;
        }

        m_restoring = false;

        // Journal every temporary change before applying any of them. UEVR
        // may persist these values while the game is running, so a hard exit
        // must be able to recover more than just the camera offsets.
        m_baseline.offsets_changed = m_baseline.offsets_changed || m_settings.zero_offsets;
        m_baseline.pitch_changed = m_baseline.pitch_changed || m_settings.flatten_pitch;
        m_baseline.aim_changed = m_baseline.aim_changed || m_settings.game_aim;
        m_baseline.hook_changed = m_baseline.hook_changed || m_settings.pause_uobject_hook;
        if (m_baseline.any_changed()) {
            save_settings();
        }

        if (m_settings.zero_offsets) {
            write_offsets(0.0f, 0.0f, 0.0f);
        }

        if (m_settings.flatten_pitch) {
            // Runtime toggle only. Writing the mod value would persist our
            // temporary change into the user's config, which we do not want.
            API::VR::set_decoupled_pitch_enabled(false);
        }

        if (m_settings.game_aim) {
            API::VR::set_aim_method(API::VR::AimMethod::GAME);
        }

        if (m_settings.pause_uobject_hook) {
            // Head or hand attached meshes pinned by UObjectHook float in
            // front of cinematic cameras otherwise.
            API::UObjectHook::set_disabled(true);
        }
    }

    void leave_cutscene() {
        API::get()->log_info("[CutsceneComfort] cutscene ended, restoring");

        if (!m_baseline.valid) {
            return;
        }

        if (m_baseline.pitch_changed) {
            API::VR::set_decoupled_pitch_enabled(m_baseline.decoupled_pitch);
        }

        if (m_baseline.aim_changed) {
            API::VR::set_aim_method(m_baseline.aim);
        }

        if (m_baseline.hook_changed) {
            API::UObjectHook::set_disabled(m_baseline.hook_disabled);
        }

        if (m_baseline.offsets_changed && m_settings.smooth_restore && m_settings.restore_seconds > 0.05f) {
            m_restoring = true;
            m_restore_t = 0.0f;
        } else {
            finish_restore();
        }
    }

    void advance_restore(float delta) {
        if (!m_restoring) {
            return;
        }

        m_restore_t += delta / std::max(m_settings.restore_seconds, 0.05f);

        if (m_restore_t >= 1.0f) {
            finish_restore();
            return;
        }

        const auto s = smoothstep01(m_restore_t);
        write_offsets(m_baseline.forward * s, m_baseline.right * s, m_baseline.up * s);
    }

    void finish_restore() {
        m_restoring = false;

        const bool had_parked = m_baseline.valid && m_baseline.any_changed();

        if (m_baseline.valid && m_baseline.offsets_changed) {
            write_offsets(m_baseline.forward, m_baseline.right, m_baseline.up);
        }

        // set_mod_value and the runtime toggles update UEVR in memory. Make
        // the restored baseline authoritative on disk as well; otherwise a
        // later clean launch can reload the temporary cutscene values after
        // our own journal has already been cleared.
        if (had_parked) {
            if (m_unvr_hold.active) {
                // The fixed screen still owns UEVR settings. Writing config
                // now would persist that temporary screen.
                m_pending_uevr_save = true;
            } else {
                API::VR::save_config();
            }
        }

        m_baseline = {};

        // Everything is back where it belongs, so drop the parked copy.
        if (had_parked) {
            save_settings();
        }
    }

    static void write_offsets(float forward, float right, float up) {
        API::VR::set_mod_value("VR_CameraForwardOffset", forward);
        API::VR::set_mod_value("VR_CameraRightOffset", right);
        API::VR::set_mod_value("VR_CameraUpOffset", up);
    }

    // ------------------------------------------------------------------
    // Settings, stored as a tiny ini in the game's UEVR profile folder
    // ------------------------------------------------------------------

    struct Settings {
        bool zero_offsets{true};
        bool smooth_restore{true};
        float restore_seconds{1.5f};
        bool flatten_pitch{true};
        bool game_aim{false};
        bool pause_uobject_hook{true};
        bool loose_detection{false};

        // Theater frame. Off by default, it is a strong stylistic choice
        // and people should opt into it.
        bool theater_frame{false};
        bool frame_lock_aspect{false};
        float frame_width_m{2.4f};
        float frame_height_m{1.35f};
        float frame_distance_m{2.0f};
        float frame_feather_m{0.10f};
        float frame_corner_radius_m{0.0f};
        float frame_curvature{0.0f};
        float frame_surround_red{0.0f};
        float frame_surround_green{0.0f};
        float frame_surround_blue{0.0f};
        float frame_opacity{1.0f};
        float frame_fade_seconds{0.6f};
        // For alternate frame rendering or flat 2D output, where the
        // backbuffer holds one view instead of two side by side.
        bool frame_single_view{false};

        // Fixed screen. Off by default. When on, a cutscene fades through
        // black into UEVR's 2D screen and fades back afterwards.
        bool unvr_enabled{false};
        bool unvr_stereo{true};
        bool unvr_follow_view{true};
        float unvr_distance_m{2.0f};
        float unvr_height_m{1.5f};
        float unvr_fade_seconds{0.15f};
    };

    std::filesystem::path settings_path() {
        return API::get()->get_persistent_dir(L"cutscene_comfort.ini");
    }

    void remember_settings_timestamp() {
        std::error_code ec{};
        const auto timestamp = std::filesystem::last_write_time(settings_path(), ec);
        if (!ec) {
            m_settings_last_write = timestamp;
            m_have_settings_timestamp = true;
        }
    }

    void poll_settings_file(float delta) {
        m_settings_poll_timer -= delta;
        if (m_settings_poll_timer > 0.0f) {
            return;
        }
        m_settings_poll_timer = 0.25f;

        std::error_code ec{};
        const auto timestamp = std::filesystem::last_write_time(settings_path(), ec);
        if (ec) {
            return;
        }

        if (!m_have_settings_timestamp) {
            m_settings_last_write = timestamp;
            m_have_settings_timestamp = true;
            return;
        }

        if (timestamp == m_settings_last_write) {
            return;
        }

        load_settings();
        API::get()->log_info("[CutsceneComfort] settings reloaded from disk");
    }

    void load_settings() {
        std::ifstream file{settings_path()};
        if (!file) {
            return;
        }

        ParkedState parked{};
        UnvrJournal unvr{};

        std::string line{};
        while (std::getline(file, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }

            const auto key = line.substr(0, eq);
            const auto value = line.substr(eq + 1);
            const bool on = value == "1" || value == "true";

            if (key == "zero_offsets") m_settings.zero_offsets = on;
            else if (key == "smooth_restore") m_settings.smooth_restore = on;
            else if (key == "restore_seconds") m_settings.restore_seconds = std::strtof(value.c_str(), nullptr);
            else if (key == "flatten_pitch") m_settings.flatten_pitch = on;
            else if (key == "game_aim") m_settings.game_aim = on;
            else if (key == "pause_uobject_hook") m_settings.pause_uobject_hook = on;
            else if (key == "loose_detection") m_settings.loose_detection = on;
            else if (key == "show_ui") m_show_ui = on;
            else if (key == "ui_hotkey_vk") m_ui_hotkey_vk = std::atoi(value.c_str());
            else if (key == "theater_frame") m_settings.theater_frame = on;
            else if (key == "frame_lock_aspect") m_settings.frame_lock_aspect = on;
            else if (key == "frame_width_m") m_settings.frame_width_m = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_height_m") m_settings.frame_height_m = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_distance_m") m_settings.frame_distance_m = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_feather_m") m_settings.frame_feather_m = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_corner_radius_m") m_settings.frame_corner_radius_m = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_curvature") m_settings.frame_curvature = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_surround_red") m_settings.frame_surround_red = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_surround_green") m_settings.frame_surround_green = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_surround_blue") m_settings.frame_surround_blue = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_opacity") m_settings.frame_opacity = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_fade_seconds") m_settings.frame_fade_seconds = std::strtof(value.c_str(), nullptr);
            else if (key == "frame_single_view") m_settings.frame_single_view = on;
            else if (key == "unvr_enabled") m_settings.unvr_enabled = on;
            else if (key == "unvr_stereo") m_settings.unvr_stereo = on;
            else if (key == "unvr_follow_view") m_settings.unvr_follow_view = on;
            else if (key == "unvr_distance_m") m_settings.unvr_distance_m = std::strtof(value.c_str(), nullptr);
            else if (key == "unvr_height_m") m_settings.unvr_height_m = std::strtof(value.c_str(), nullptr);
            else if (key == "unvr_fade_seconds") m_settings.unvr_fade_seconds = std::strtof(value.c_str(), nullptr);
            else if (key == "parked_unvr") unvr.active = on;
            else if (key == "parked_unvr_screen") {
                unvr.screen = on;
                unvr.have_screen = true;
            }
            else if (key == "parked_unvr_gui") {
                unvr.gui = on;
                unvr.have_gui = true;
            }
            else if (key == "parked_unvr_roomscale") {
                unvr.roomscale = on;
                unvr.have_roomscale = true;
            }
            else if (key == "parked_unvr_distance") {
                unvr.distance = std::strtof(value.c_str(), nullptr);
                unvr.have_distance = true;
            }
            else if (key == "parked_unvr_height") {
                unvr.height = std::strtof(value.c_str(), nullptr);
                unvr.have_height = true;
            }
            else if (key == "parked_unvr_follow") {
                unvr.follow = on;
                unvr.have_follow = true;
            }
            else if (key == "parked_unvr_x") {
                unvr.offset_x = std::strtof(value.c_str(), nullptr);
                unvr.have_offset_x = true;
            }
            else if (key == "parked_unvr_y") {
                unvr.offset_y = std::strtof(value.c_str(), nullptr);
                unvr.have_offset_y = true;
            }
            else if (key == "parked_unvr_overlay") {
                unvr.overlay = std::atoi(value.c_str());
                unvr.have_overlay = true;
            }
            else if (key == "parked_state") parked.active = on;
            else if (key == "parked_offsets") {
                parked.offsets_changed = on;
                parked.active = parked.active || on; // old offset-only journals
            }
            else if (key == "parked_forward") {
                parked.forward = std::strtof(value.c_str(), nullptr);
                parked.have_forward = true;
            }
            else if (key == "parked_right") {
                parked.right = std::strtof(value.c_str(), nullptr);
                parked.have_right = true;
            }
            else if (key == "parked_up") {
                parked.up = std::strtof(value.c_str(), nullptr);
                parked.have_up = true;
            }
            else if (key == "parked_pitch") {
                parked.pitch_changed = on;
                parked.active = parked.active || on;
            }
            else if (key == "parked_decoupled_pitch") {
                parked.decoupled_pitch = on;
                parked.have_decoupled_pitch = true;
            }
            else if (key == "parked_aim") {
                parked.aim = std::atoi(value.c_str());
                parked.have_aim = true;
                parked.aim_changed = true;
                parked.active = true;
            }
            else if (key == "parked_hook") {
                parked.hook_changed = on;
                parked.active = parked.active || on;
            }
            else if (key == "parked_hook_disabled") {
                parked.hook_disabled = on;
                parked.have_hook_disabled = true;
            }
            // Handy for setting the frame up, and for testing without
            // hunting down an actual cutscene.
            else if (key == "simulate_cutscene") m_simulate = on;
        }

        m_settings.restore_seconds = std::clamp(m_settings.restore_seconds, 0.1f, 10.0f);
        m_settings.frame_width_m = std::clamp(m_settings.frame_width_m, 0.1f, 12.0f);
        m_settings.frame_height_m = std::clamp(m_settings.frame_height_m, 0.1f, 8.0f);
        m_settings.frame_distance_m = std::clamp(m_settings.frame_distance_m, 0.25f, 12.0f);
        m_settings.frame_feather_m = std::clamp(m_settings.frame_feather_m, 0.0f, 0.5f);
        m_settings.frame_corner_radius_m = std::clamp(m_settings.frame_corner_radius_m, 0.0f, 2.0f);
        m_settings.frame_curvature = std::clamp(m_settings.frame_curvature, 0.0f, 1.0f);
        m_settings.frame_surround_red = std::clamp(m_settings.frame_surround_red, 0.0f, 1.0f);
        m_settings.frame_surround_green = std::clamp(m_settings.frame_surround_green, 0.0f, 1.0f);
        m_settings.frame_surround_blue = std::clamp(m_settings.frame_surround_blue, 0.0f, 1.0f);
        m_settings.frame_opacity = std::clamp(m_settings.frame_opacity, 0.0f, 1.0f);
        m_settings.frame_fade_seconds = std::clamp(m_settings.frame_fade_seconds, 0.05f, 5.0f);
        m_settings.unvr_distance_m = std::clamp(m_settings.unvr_distance_m, 0.5f, 8.0f);
        m_settings.unvr_height_m = std::clamp(m_settings.unvr_height_m, 0.5f, 6.0f);
        m_settings.unvr_fade_seconds = std::clamp(m_settings.unvr_fade_seconds, 0.10f, 0.30f);

        m_parked_state = parked;
        // A reload while a screen is up must not replace the live originals.
        if (!m_unvr_hold.active) {
            m_parked_unvr = unvr;
        }

        remember_settings_timestamp();
    }

    void save_settings() {
        const auto path = settings_path();
        auto temp_path = path;
        temp_path += L".tmp";

        std::ofstream file{temp_path, std::ios::trunc};
        if (!file) {
            API::get()->log_warn("[CutsceneComfort] could not write settings file");
            return;
        }

        file << "zero_offsets=" << (m_settings.zero_offsets ? 1 : 0) << "\n";
        file << "smooth_restore=" << (m_settings.smooth_restore ? 1 : 0) << "\n";
        file << "restore_seconds=" << m_settings.restore_seconds << "\n";
        file << "flatten_pitch=" << (m_settings.flatten_pitch ? 1 : 0) << "\n";
        file << "game_aim=" << (m_settings.game_aim ? 1 : 0) << "\n";
        file << "pause_uobject_hook=" << (m_settings.pause_uobject_hook ? 1 : 0) << "\n";
        file << "loose_detection=" << (m_settings.loose_detection ? 1 : 0) << "\n";
        file << "show_ui=" << (m_show_ui ? 1 : 0) << "\n";
        file << "ui_hotkey_vk=" << m_ui_hotkey_vk.load() << "\n";
        file << "theater_frame=" << (m_settings.theater_frame ? 1 : 0) << "\n";
        file << "frame_lock_aspect=" << (m_settings.frame_lock_aspect ? 1 : 0) << "\n";
        file << "frame_width_m=" << m_settings.frame_width_m << "\n";
        file << "frame_height_m=" << m_settings.frame_height_m << "\n";
        file << "frame_distance_m=" << m_settings.frame_distance_m << "\n";
        file << "frame_feather_m=" << m_settings.frame_feather_m << "\n";
        file << "frame_corner_radius_m=" << m_settings.frame_corner_radius_m << "\n";
        file << "frame_curvature=" << m_settings.frame_curvature << "\n";
        file << "frame_surround_red=" << m_settings.frame_surround_red << "\n";
        file << "frame_surround_green=" << m_settings.frame_surround_green << "\n";
        file << "frame_surround_blue=" << m_settings.frame_surround_blue << "\n";
        file << "frame_opacity=" << m_settings.frame_opacity << "\n";
        file << "frame_fade_seconds=" << m_settings.frame_fade_seconds << "\n";
        file << "frame_single_view=" << (m_settings.frame_single_view ? 1 : 0) << "\n";
        file << "unvr_enabled=" << (m_settings.unvr_enabled ? 1 : 0) << "\n";
        file << "unvr_stereo=" << (m_settings.unvr_stereo ? 1 : 0) << "\n";
        file << "unvr_follow_view=" << (m_settings.unvr_follow_view ? 1 : 0) << "\n";
        file << "unvr_distance_m=" << m_settings.unvr_distance_m << "\n";
        file << "unvr_height_m=" << m_settings.unvr_height_m << "\n";
        file << "unvr_fade_seconds=" << m_settings.unvr_fade_seconds << "\n";
        file << "simulate_cutscene=" << (m_simulate ? 1 : 0) << "\n";

        // Startup recovery has not consumed the on-disk copy yet. Keep writing
        // that copy so an early settings save cannot drop it.
        const UnvrJournal& unvr_journal = (!m_startup_recovery_done && m_parked_unvr.active)
            ? m_parked_unvr
            : m_unvr_hold;
        file << "parked_unvr=" << (unvr_journal.active ? 1 : 0) << "\n";
        if (unvr_journal.active && unvr_journal.have_screen) {
            file << "parked_unvr_screen=" << (unvr_journal.screen ? 1 : 0) << "\n";
        }
        if (unvr_journal.active && unvr_journal.have_gui) {
            file << "parked_unvr_gui=" << (unvr_journal.gui ? 1 : 0) << "\n";
        }
        if (unvr_journal.active && unvr_journal.have_roomscale) {
            file << "parked_unvr_roomscale=" << (unvr_journal.roomscale ? 1 : 0) << "\n";
        }
        if (unvr_journal.active && unvr_journal.have_distance) {
            file << "parked_unvr_distance=" << unvr_journal.distance << "\n";
        }
        if (unvr_journal.active && unvr_journal.have_height) {
            file << "parked_unvr_height=" << unvr_journal.height << "\n";
        }
        if (unvr_journal.active && unvr_journal.have_follow) {
            file << "parked_unvr_follow=" << (unvr_journal.follow ? 1 : 0) << "\n";
        }
        if (unvr_journal.active && unvr_journal.have_offset_x) {
            file << "parked_unvr_x=" << unvr_journal.offset_x << "\n";
        }
        if (unvr_journal.active && unvr_journal.have_offset_y) {
            file << "parked_unvr_y=" << unvr_journal.offset_y << "\n";
        }
        if (unvr_journal.active && unvr_journal.have_overlay) {
            file << "parked_unvr_overlay=" << unvr_journal.overlay << "\n";
        }

        const bool have_parked = m_baseline.valid && m_baseline.any_changed();
        file << "parked_state=" << (have_parked ? 1 : 0) << "\n";
        file << "parked_offsets=" << (have_parked && m_baseline.offsets_changed ? 1 : 0) << "\n";
        if (have_parked && m_baseline.offsets_changed) {
            file << "parked_forward=" << m_baseline.forward << "\n";
            file << "parked_right=" << m_baseline.right << "\n";
            file << "parked_up=" << m_baseline.up << "\n";
        }
        file << "parked_pitch=" << (have_parked && m_baseline.pitch_changed ? 1 : 0) << "\n";
        if (have_parked && m_baseline.pitch_changed) {
            file << "parked_decoupled_pitch=" << (m_baseline.decoupled_pitch ? 1 : 0) << "\n";
        }
        if (have_parked && m_baseline.aim_changed) {
            file << "parked_aim=" << static_cast<int>(m_baseline.aim) << "\n";
        }
        file << "parked_hook=" << (have_parked && m_baseline.hook_changed ? 1 : 0) << "\n";
        if (have_parked && m_baseline.hook_changed) {
            file << "parked_hook_disabled=" << (m_baseline.hook_disabled ? 1 : 0) << "\n";
        }

        file.flush();
        const bool write_ok = file.good();
        file.close();

        if (!write_ok || !MoveFileExW(temp_path.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            API::get()->log_warn("[CutsceneComfort] could not replace settings file atomically");
            std::error_code ec{};
            std::filesystem::remove(temp_path, ec);
            return;
        }

        remember_settings_timestamp();
    }

    bool recover_parked_state() {
        const bool had_unvr = m_parked_unvr.active;
        if (had_unvr && !restore_unvr_journal(m_parked_unvr)) {
            API::get()->log_error("[CutsceneComfort] parked fixed-screen state did not restore; detection remains disabled");
            return false;
        }
        m_parked_unvr = {};

        if (!m_parked_state.active) {
            if (had_unvr) {
                API::VR::save_config();
                save_settings();
                API::get()->log_info("[CutsceneComfort] restored fixed-screen settings after an interrupted cutscene");
            }
            return true;
        }

        if (!m_parked_state.complete() || !m_parked_state.valid()) {
            API::get()->log_error("[CutsceneComfort] parked state is incomplete or invalid; detection remains disabled");
            return false;
        }

        if (m_parked_state.offsets_changed) {
            write_offsets(m_parked_state.forward, m_parked_state.right, m_parked_state.up);
        }
        if (m_parked_state.pitch_changed) {
            API::VR::set_decoupled_pitch_enabled(m_parked_state.decoupled_pitch);
        }
        if (m_parked_state.aim_changed) {
            API::VR::set_aim_method(static_cast<API::VR::AimMethod>(m_parked_state.aim));
        }
        if (m_parked_state.hook_changed) {
            API::UObjectHook::set_disabled(m_parked_state.hook_disabled);
        }

        const auto recovered_forward = read_mod_float("VR_CameraForwardOffset");
        const auto recovered_right = read_mod_float("VR_CameraRightOffset");
        const auto recovered_up = read_mod_float("VR_CameraUpOffset");
        constexpr float k_verify_epsilon = 0.0001f;
        const bool offsets_ok = !m_parked_state.offsets_changed ||
            (std::abs(recovered_forward - m_parked_state.forward) <= k_verify_epsilon &&
             std::abs(recovered_right - m_parked_state.right) <= k_verify_epsilon &&
             std::abs(recovered_up - m_parked_state.up) <= k_verify_epsilon);
        const bool pitch_ok = !m_parked_state.pitch_changed ||
            API::VR::is_decoupled_pitch_enabled() == m_parked_state.decoupled_pitch;
        const bool aim_ok = !m_parked_state.aim_changed ||
            API::VR::get_aim_method() == static_cast<API::VR::AimMethod>(m_parked_state.aim);
        const bool hook_ok = !m_parked_state.hook_changed ||
            API::UObjectHook::is_disabled() == m_parked_state.hook_disabled;
        if (!offsets_ok || !pitch_ok || !aim_ok || !hook_ok) {
            API::get()->log_error("[CutsceneComfort] parked state recovery did not stick; detection remains disabled");
            return false;
        }

        if (had_unvr) {
            API::get()->log_info("[CutsceneComfort] restored fixed-screen settings after an interrupted cutscene");
        }

        API::get()->log_info(
            "[CutsceneComfort] recovered parked state after an interrupted cutscene: F %.3f R %.3f U %.3f pitch %d aim %d hook_disabled %d",
            m_parked_state.forward, m_parked_state.right, m_parked_state.up,
            m_parked_state.decoupled_pitch ? 1 : 0, m_parked_state.aim,
            m_parked_state.hook_disabled ? 1 : 0);

        // UEVR can have persisted our temporary values before the crash.
        // Save the verified recovery before clearing the only other copy.
        API::VR::save_config();
        m_parked_state = {};
        save_settings();
        return true;
    }

    // ------------------------------------------------------------------
    // UI
    // ------------------------------------------------------------------

    static LRESULT CALLBACK window_subclass_proc(
        HWND hwnd,
        UINT msg,
        WPARAM wparam,
        LPARAM lparam,
        UINT_PTR subclass_id,
        DWORD_PTR reference_data) {
        auto* self = reinterpret_cast<CutsceneComfort*>(reference_data);

        if (self == nullptr) {
            return DefSubclassProc(hwnd, msg, wparam, lparam);
        }

        if (msg == WM_NCDESTROY) {
            RemoveWindowSubclass(hwnd, window_subclass_proc, subclass_id);
            self->m_input_subclass_installed = false;
            return DefSubclassProc(hwnd, msg, wparam, lparam);
        }

        if (self->m_uevr_menu_open.load()) {
            const auto hotkey = self->m_ui_hotkey_vk.load();
            if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
                hotkey != 0 && static_cast<int>(wparam) == hotkey &&
                (lparam & (1LL << 30)) == 0) {
                self->m_show_ui = !self->m_show_ui.load();
                self->m_settings_dirty = true;
                return 0;
            }

            if (msg == WM_LBUTTONUP) {
                const auto x = static_cast<float>(GET_X_LPARAM(lparam));
                const auto y = static_cast<float>(GET_Y_LPARAM(lparam));
                if (x >= self->m_companion_hit_min_x.load() &&
                    x <= self->m_companion_hit_max_x.load() &&
                    y >= self->m_companion_hit_min_y.load() &&
                    y <= self->m_companion_hit_max_y.load()) {
                    self->m_show_ui = !self->m_show_ui.load();
                    self->m_settings_dirty = true;
                    return 0;
                }
            }
        }

        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }

    void ensure_menu_input_subclass() {
        if (m_input_subclass_installed.load() || m_wnd == nullptr || !IsWindow(m_wnd)) {
            return;
        }

        if (SetWindowSubclass(
                m_wnd,
                window_subclass_proc,
                k_input_subclass_id,
                reinterpret_cast<DWORD_PTR>(this))) {
            m_input_subclass_installed = true;
            API::get()->log_info("[CutsceneComfort] UEVR-menu input bridge installed");
        } else if (!m_input_subclass_error_logged.exchange(true)) {
            API::get()->log_warn(
                "[CutsceneComfort] could not install UEVR-menu input bridge (error %lu)",
                GetLastError());
        }
    }

    void draw_ui() {
        bool changed = false;

        // The public plugin ABI deliberately owns a separate ImGui context
        // and has no callback for inserting widgets into UEVR's native pages.
        // It does expose the native menu's visibility, though, so present a
        // small companion panel whenever that menu is open. This gives every
        // stock UEVR build a discoverable way to reopen or hide our controls
        // without patching or recompiling the framework.
        const auto* param = API::get()->param();
        const bool uevr_menu_open = param != nullptr &&
            param->functions != nullptr &&
            param->functions->is_drawing_ui != nullptr &&
            param->functions->is_drawing_ui();
        m_uevr_menu_open = uevr_menu_open;

        if (uevr_menu_open) {
            bool controls_visible = m_show_ui.load();
            const auto display_size = ImGui::GetIO().DisplaySize;

            ImGui::SetNextWindowPos(
                ImVec2{display_size.x - 18.0f, 18.0f},
                ImGuiCond_Always,
                ImVec2{1.0f, 0.0f});
            ImGui::SetNextWindowBgAlpha(0.96f);

            constexpr auto companion_flags =
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings;

            if (ImGui::Begin("Cutscene Comfort plugin##UEVRMenu", nullptr, companion_flags)) {
                // UEVR consumes mouse/key messages before external plugin
                // callbacks while its menu owns input. Draw a passive item
                // and poll just this item's client-space hit rectangle below,
                // so the toggle still works without requiring users to turn
                // on UEVR's global Input Passthrough option.
                ImGui::Text("%s  Show cinematic controls", controls_visible ? "[x]" : "[ ]");
                const auto hit_min = ImGui::GetItemRectMin();
                const auto hit_max = ImGui::GetItemRectMax();
                m_companion_hit_min_x = hit_min.x;
                m_companion_hit_min_y = hit_min.y;
                m_companion_hit_max_x = hit_max.x;
                m_companion_hit_max_y = hit_max.y;

                ImGui::TextDisabled("Available whenever the UEVR menu is open");
            }
            ImGui::End();
        }

        if (!m_show_ui.load()) {
            if (changed) {
                save_settings();
            }
            return;
        }

        bool keep_open = true;

        ImGui::SetNextWindowSize(ImVec2{420.0f, 0.0f}, ImGuiCond_FirstUseEver);

        if (ImGui::Begin("Cutscene Comfort", &keep_open)) {
            if (m_in_cutscene) {
                ImGui::TextColored(ImVec4{1.0f, 0.75f, 0.2f, 1.0f}, "In a cutscene");
            } else {
                ImGui::TextColored(ImVec4{0.4f, 0.9f, 0.4f, 1.0f}, "Gameplay");
            }

            ImGui::Text("View target: %s", m_target_label.c_str());

            if (!m_detect_note.empty()) {
                ImGui::TextDisabled("%s", m_detect_note.c_str());
            }

            if (m_camera_actor_class == nullptr) {
                ImGui::TextDisabled("still looking for Engine.CameraActor");
            }

            ImGui::Separator();

            changed |= ImGui::Checkbox("Zero camera offsets during cutscenes", &m_settings.zero_offsets);
            changed |= ImGui::Checkbox("Ease offsets back after the scene", &m_settings.smooth_restore);

            if (m_settings.smooth_restore) {
                changed |= ImGui::SliderFloat("Ease duration", &m_settings.restore_seconds, 0.1f, 5.0f, "%.1f s");
            }

            changed |= ImGui::Checkbox("Flatten decoupled pitch during cutscenes", &m_settings.flatten_pitch);
            changed |= ImGui::Checkbox("Switch aim back to game during cutscenes", &m_settings.game_aim);
            changed |= ImGui::Checkbox("Pause UObjectHook during cutscenes", &m_settings.pause_uobject_hook);
            changed |= ImGui::Checkbox("Loose detection (any non pawn view target)", &m_settings.loose_detection);

            if (m_settings.loose_detection) {
                ImGui::TextDisabled("careful: vehicles and turrets can trip this");
            }

            ImGui::Separator();
            ImGui::Text("Fixed screen during cutscenes:");
            if (!m_unvr_support_known) {
                ImGui::TextDisabled("Waiting for UEVR's 2D screen mode");
            } else if (!m_unvr_supported) {
                ImGui::TextDisabled("This UEVR build does not expose 2D screen mode");
            } else {
                changed |= ImGui::Checkbox("Fade to a fixed screen during cutscenes", &m_settings.unvr_enabled);
                if (m_settings.unvr_enabled && m_settings.theater_frame) {
                    ImGui::TextDisabled("The spatial window stays off while this is enabled");
                }

                if (m_settings.unvr_enabled) {
                    int picture = m_settings.unvr_stereo ? 0 : 1;
                    static const char* pictures[] = {"Stereo portal", "Flat 2D"};
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::Combo("Picture", &picture, pictures, IM_ARRAYSIZE(pictures))) {
                        m_settings.unvr_stereo = picture == 0;
                        changed = true;
                    }
                    ImGui::TextWrapped(
                        "Stereo portal keeps the game's two eye views on UEVR's screen, so the shot still has depth. Flat 2D shows one picture to both eyes. Either way the headset stops driving the camera.");

                    changed |= ImGui::Checkbox("Keep the screen in front of my view", &m_settings.unvr_follow_view);
                    changed |= ImGui::SliderFloat("Screen distance", &m_settings.unvr_distance_m, 0.5f, 8.0f, "%.2f m");
                    changed |= ImGui::SliderFloat("Screen height", &m_settings.unvr_height_m, 0.5f, 6.0f, "%.2f m");
                    changed |= ImGui::SliderFloat("Fade each way", &m_settings.unvr_fade_seconds, 0.10f, 0.30f, "%.2f s");
                    ImGui::TextDisabled("Short on purpose, so a prompt right after the scene is still readable.");

                    if (ImGui::Button("Preview screen for 5 seconds")) {
                        m_unvr_preview_until_ms = GetTickCount64() + 5000;
                    }
                }
            }

            if (m_unvr.phase() != comfort::UnvrPhase::Idle) {
                ImGui::TextDisabled("%s", unvr_phase_label());
            }

            ImGui::Separator();
            ImGui::Text("Room-locked 6DOF cutscene window:");
            ImGui::TextDisabled("Native stereo scene inside a fixed physical aperture");

            changed |= ImGui::Checkbox("Place cutscenes in a spatial window", &m_settings.theater_frame);

            if (m_settings.theater_frame) {
                ImGui::TextDisabled("Ctrl+click a slider to type an exact value.");
                changed |= ImGui::SliderFloat("Window X width", &m_settings.frame_width_m, 0.1f, 12.0f, "%.3f m");
                changed |= ImGui::Checkbox("Lock to 16:9 aspect", &m_settings.frame_lock_aspect);
                if (m_settings.frame_lock_aspect) {
                    ImGui::TextDisabled("Window Y height: %.3f m (derived)",
                        m_settings.frame_width_m * 9.0f / 16.0f);
                } else {
                    changed |= ImGui::SliderFloat("Window Y height", &m_settings.frame_height_m, 0.1f, 8.0f, "%.3f m");
                }
                changed |= ImGui::SliderFloat("Distance from recenter", &m_settings.frame_distance_m, 0.25f, 12.0f, "%.3f m");
                changed |= ImGui::SliderFloat("Feather width", &m_settings.frame_feather_m, 0.0f, 0.5f, "%.3f m");
                changed |= ImGui::SliderFloat("Corner radius", &m_settings.frame_corner_radius_m, 0.0f, 2.0f, "%.3f m");
                changed |= ImGui::SliderFloat("Horizontal curvature", &m_settings.frame_curvature, 0.0f, 1.0f, "%.3f");

                float surround_color[]{
                    m_settings.frame_surround_red,
                    m_settings.frame_surround_green,
                    m_settings.frame_surround_blue,
                };
                if (ImGui::ColorEdit3("Surround color", surround_color,
                        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB)) {
                    m_settings.frame_surround_red = surround_color[0];
                    m_settings.frame_surround_green = surround_color[1];
                    m_settings.frame_surround_blue = surround_color[2];
                    changed = true;
                }
                changed |= ImGui::SliderFloat("Surround opacity", &m_settings.frame_opacity, 0.0f, 1.0f, "%.3f");
                changed |= ImGui::SliderFloat("Fade time", &m_settings.frame_fade_seconds, 0.05f, 3.0f, "%.2f s");
                changed |= ImGui::Checkbox("Single view output", &m_settings.frame_single_view);

                if (ImGui::Button("Preview frame for 5 seconds")) {
                    request_frame_recenter();
                    m_frame_preview_until_ms = GetTickCount64() + 5000;
                }

                ImGui::SameLine();
                if (ImGui::Button("Recenter in front of me")) {
                    request_frame_recenter();
                }

                ImGui::TextDisabled("fade: %.0f%%", m_frame_fade * 100.0f);
                {
                    std::scoped_lock lock{m_frame_anchor_mutex};
                    if (m_operator_window_bridge_available.load()) {
                        ImGui::TextDisabled("anchor: Operator final-eye bridge");
                    } else {
                        ImGui::TextDisabled(m_frame_anchor_valid ? "anchor: fixed in tracking space" : "anchor: waiting for valid HMD pose");
                    }
                }
                ImGui::TextWrapped("The aperture is captured when the cutscene starts or when you recenter. It does not follow later head movement. Curvature 1 wraps it around that captured origin.");
            }

            ImGui::Separator();

            if (ImGui::Button(m_simulate ? "Stop simulated cutscene" : "Simulate a cutscene")) {
                m_simulate = !m_simulate;
                changed = true;
            }

            ImGui::SameLine();

            if (ImGui::Button("Restore now")) {
                m_simulate = false;
                changed = true;
                if (m_restoring || m_baseline.valid) {
                    finish_restore();
                }
            }

            if (m_restoring) {
                ImGui::ProgressBar(m_restore_t, ImVec2{-1.0f, 0.0f}, "easing back");
            }

            if (m_baseline.valid) {
                ImGui::TextDisabled("saved offsets: F %.1f  R %.1f  U %.1f",
                    m_baseline.forward, m_baseline.right, m_baseline.up);
            }

            ImGui::Separator();

            // Window visibility. The X on the title bar hides it too, the
            // hotkey below brings it back.
            static const char* hotkey_names[] = {"None", "F7", "F8", "F9", "F10", "Insert", "Home"};
            static const int hotkey_vks[] = {0, VK_F7, VK_F8, VK_F9, VK_F10, VK_INSERT, VK_HOME};

            int hotkey_index = 0;
            for (int i = 0; i < IM_ARRAYSIZE(hotkey_vks); ++i) {
                if (hotkey_vks[i] == m_ui_hotkey_vk.load()) {
                    hotkey_index = i;
                    break;
                }
            }

            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo("Show/hide hotkey", &hotkey_index, hotkey_names, IM_ARRAYSIZE(hotkey_names))) {
                m_ui_hotkey_vk = hotkey_vks[hotkey_index];
                changed = true;
            }

            ImGui::SameLine();
            if (ImGui::Button("Hide window")) {
                keep_open = false;
            }

            if (m_ui_hotkey_vk.load() == 0) {
                ImGui::TextDisabled("no hotkey set: to get the window back later,");
                ImGui::TextDisabled("set show_ui=1 in cutscene_comfort.ini");
            }

            ImGui::TextDisabled("plugin cost: %.1f us per tick", m_tick_cost_us);

            if (m_tick_errors > 0) {
                ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.4f, 1.0f},
                    "tick errors: %d (see UEVR log)", m_tick_errors);
            }
        }
        ImGui::End();

        if (!keep_open) {
            m_show_ui = false;
            changed = true;
        }

        if (changed) {
            save_settings();
        }
    }

    // ------------------------------------------------------------------
    // Room-locked cutscene window
    //
    // The scene itself is never touched, so both eyes keep rendering the
    // cutscene in stereo and head translation still moves the viewpoint.
    // All we add is a physical flat or curved aperture captured in tracking
    // space. Current per-eye rays are intersected with that fixed surface, so
    // the shot stays stereo and head translation changes the view through it.
    // ------------------------------------------------------------------

    // Advances the fade and reports the alpha to draw with, or zero when
    // there is nothing to do. Both backends call this so they stay in step.
    float advance_frame_fade() {
        // Fade is driven here rather than on the game thread because this
        // runs once per submitted frame, which is what the eye actually sees.
        // Deliberately not ImGui's clock: this runs on the render thread and
        // should not care whether an ImGui context exists.
        const auto now_ms = GetTickCount64();
        const bool previewing = now_ms < m_frame_preview_until_ms.load();
        const auto target = ((m_in_cutscene && !m_restoring) || previewing) ? 1.0f : 0.0f;
        const auto elapsed_ms = m_frame_last_tick_ms == 0 ? 0ull : now_ms - m_frame_last_tick_ms;
        m_frame_last_tick_ms = now_ms;
        const auto frame_delta = std::min((float)elapsed_ms / 1000.0f, 0.1f);
        const auto step = frame_delta / std::max(m_render_frame_fade_seconds.load(), 0.05f);

        if (m_frame_fade < target) {
            m_frame_fade = std::min(m_frame_fade + step, target);
        } else if (m_frame_fade > target) {
            m_frame_fade = std::max(m_frame_fade - step, target);
        }

        if (m_frame_fade <= 0.001f) {
            return 0.0f;
        }

        return m_frame_fade * m_render_frame_opacity.load();
    }

    static bool finite_vector(const glm::vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    static void store_vector(float (&destination)[3], const glm::vec3& value) {
        destination[0] = value.x;
        destination[1] = value.y;
        destination[2] = value.z;
    }

    bool build_eye_projection(bool right_eye, const glm::mat4& hmd_transform,
        VignetteParams::EyeProjection& output) {
        const auto eye = right_eye ? API::VR::Eye::RIGHT : API::VR::Eye::LEFT;
        const auto raw_offset = API::VR::get_eye_offset(eye);
        const glm::vec3 eye_offset{raw_offset.x, raw_offset.y, raw_offset.z};
        if (!finite_vector(eye_offset)) {
            return false;
        }

        // The public plugin API exposes each eye's tracking-space offset and
        // projection. Eye cant is not separately exposed; current consumer
        // headsets use the HMD orientation for both eyes, matching UEVR's
        // normal stereo path.
        const auto eye_world = hmd_transform * glm::translate(glm::mat4{1.0f}, eye_offset);
        const glm::vec3 eye_origin{eye_world[3]};

        const auto raw_projection = API::VR::get_ue_projection_matrix(eye);
        static_assert(sizeof(raw_projection) == sizeof(glm::mat4));
        glm::mat4 projection{};
        std::memcpy(&projection, &raw_projection, sizeof(projection));

        const glm::mat4 tracking_to_projection{
            1.0f, 0.0f,  0.0f, 0.0f,
            0.0f, 1.0f,  0.0f, 0.0f,
            0.0f, 0.0f, -1.0f, 0.0f,
            0.0f, 0.0f,  0.0f, 1.0f,
        };
        const auto view = tracking_to_projection * glm::inverse(eye_world);
        const auto projection_view = projection * view;
        const auto determinant = glm::determinant(projection_view);
        if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001f) {
            return false;
        }
        const auto clip_to_tracking = glm::inverse(projection_view);

        const auto make_ray = [&](float x, float y, glm::vec3& ray) {
            const auto tracking = clip_to_tracking * glm::vec4{x, y, 1.0f, 1.0f};
            if (!std::isfinite(tracking.x) || !std::isfinite(tracking.y) ||
                !std::isfinite(tracking.z) || !std::isfinite(tracking.w) ||
                std::abs(tracking.w) <= 0.000001f) {
                return false;
            }
            ray = glm::vec3{tracking} / tracking.w - eye_origin;
            return finite_vector(ray) && glm::length(ray) > 0.000001f;
        };

        glm::vec3 center{};
        glm::vec3 at_x{};
        glm::vec3 at_y{};
        if (!make_ray(0.0f, 0.0f, center) ||
            !make_ray(1.0f, 0.0f, at_x) ||
            !make_ray(0.0f, 1.0f, at_y)) {
            return false;
        }

        store_vector(output.origin, eye_origin);
        store_vector(output.ray_center, center);
        store_vector(output.ray_x, at_x - center);
        store_vector(output.ray_y, at_y - center);
        output.valid = true;
        return true;
    }

    bool build_frame_params(float alpha, VignetteParams& params) {
        if (!API::VR::is_hmd_active()) {
            return false;
        }

        const auto pose = API::VR::get_pose(API::VR::get_hmd_index());
        const glm::vec3 hmd_position{pose.position.x, pose.position.y, pose.position.z};
        glm::quat hmd_rotation{pose.rotation.w, pose.rotation.x, pose.rotation.y, pose.rotation.z};
        if (!finite_vector(hmd_position) ||
            !std::isfinite(hmd_rotation.w) || !std::isfinite(hmd_rotation.x) ||
            !std::isfinite(hmd_rotation.y) || !std::isfinite(hmd_rotation.z) ||
            glm::length(hmd_rotation) < 0.5f) {
            return false;
        }

        hmd_rotation = glm::normalize(hmd_rotation);
        auto hmd_transform = glm::mat4_cast(hmd_rotation);
        hmd_transform[3] = glm::vec4{hmd_position, 1.0f};

        params = {};
        params.layout = m_render_frame_single_view.load()
            ? VignetteParams::Layout::SINGLE
            : VignetteParams::Layout::DOUBLE_WIDE;
        if (!build_eye_projection(false, hmd_transform, params.eyes[0]) ||
            (params.layout == VignetteParams::Layout::DOUBLE_WIDE &&
                !build_eye_projection(true, hmd_transform, params.eyes[1]))) {
            return false;
        }

        const glm::vec3 hmd_right = glm::normalize(glm::vec3{hmd_transform[0]});
        const glm::vec3 hmd_up = glm::normalize(glm::vec3{hmd_transform[1]});
        const glm::vec3 hmd_back = glm::normalize(glm::vec3{hmd_transform[2]});
        if (!finite_vector(hmd_right) || !finite_vector(hmd_up) || !finite_vector(hmd_back)) {
            return false;
        }

        glm::vec3 anchor_origin{};
        glm::vec3 anchor_right{};
        glm::vec3 anchor_up{};
        glm::vec3 anchor_back{};
        {
            std::scoped_lock lock{m_frame_anchor_mutex};
            const bool recenter = m_frame_recenter_requested.exchange(false);
            if (!m_frame_anchor_valid || recenter) {
                m_frame_anchor_origin = hmd_position;
                m_frame_anchor_right = hmd_right;
                m_frame_anchor_up = hmd_up;
                m_frame_anchor_back = hmd_back;
                m_frame_anchor_valid = true;

                const auto center = m_frame_anchor_origin - m_frame_anchor_back *
                    m_render_frame_distance_m.load();
                API::get()->log_info(
                    "[CutsceneComfort] anchored spatial window origin (%.3f, %.3f, %.3f), center (%.3f, %.3f, %.3f)",
                    m_frame_anchor_origin.x, m_frame_anchor_origin.y, m_frame_anchor_origin.z,
                    center.x, center.y, center.z);
            }

            anchor_origin = m_frame_anchor_origin;
            anchor_right = m_frame_anchor_right;
            anchor_up = m_frame_anchor_up;
            anchor_back = m_frame_anchor_back;
        }

        store_vector(params.anchor_origin, anchor_origin);
        store_vector(params.anchor_right, anchor_right);
        store_vector(params.anchor_up, anchor_up);
        store_vector(params.anchor_back, anchor_back);
        params.width = m_render_frame_width_m.load();
        params.height = m_render_frame_lock_aspect.load()
            ? params.width * 9.0f / 16.0f
            : m_render_frame_height_m.load();
        params.distance = m_render_frame_distance_m.load();
        params.feather = m_render_frame_feather_m.load();
        params.corner_radius = m_render_frame_corner_radius_m.load();
        params.curvature = m_render_frame_curvature.load();
        params.surround_color[0] = m_render_frame_surround_red.load();
        params.surround_color[1] = m_render_frame_surround_green.load();
        params.surround_color[2] = m_render_frame_surround_blue.load();
        params.alpha = alpha;
        return true;
    }

    void request_frame_recenter() {
        m_frame_recenter_requested = true;
    }

    void invalidate_frame_anchor() {
        std::scoped_lock lock{m_frame_anchor_mutex};
        m_frame_anchor_valid = false;
        m_frame_recenter_requested = true;
    }

    void detect_operator_window_bridge() {
        const auto value = API::VR::get_mod_value<std::string>("WindowMode_ExternalBridgeAvailable");
        const bool available = value == "true" || value == "1";
        m_operator_window_bridge_available = available;
        API::get()->log_info("[CutsceneComfort] final-eye spatial bridge: %s",
            available ? "Operator" : "standalone backbuffer fallback");
    }

    void publish_operator_window(float alpha) {
        const bool active = m_render_frame_enabled.load() && alpha > 0.001f;
        const bool recenter = active && m_frame_recenter_requested.exchange(false);

        std::ostringstream payload{};
        payload << std::setprecision(9)
            << 1 << ' '
            << (active ? 1 : 0) << ' '
            << (recenter ? 1 : 0) << ' '
            << (m_render_frame_lock_aspect.load() ? 1 : 0) << ' '
            << m_render_frame_width_m.load() << ' '
            << m_render_frame_height_m.load() << ' '
            << m_render_frame_distance_m.load() << ' '
            << m_render_frame_feather_m.load() << ' '
            << m_render_frame_corner_radius_m.load() << ' '
            << m_render_frame_curvature.load() << ' '
            << m_render_frame_surround_red.load() << ' '
            << m_render_frame_surround_green.load() << ' '
            << m_render_frame_surround_blue.load() << ' '
            << std::clamp(alpha, 0.0f, 1.0f);
        API::get()->dispatch_custom_event("CutsceneComfort.WindowMode.v1", payload.str());
    }

    void draw_theater_frame_d3d11(ID3D11DeviceContext* context) {
        if (context == nullptr) {
            return;
        }

        const auto alpha = m_render_frame_enabled.load() ? advance_frame_fade() : 0.0f;
        if (m_operator_window_bridge_available.load()) {
            publish_operator_window(alpha);
            return;
        }
        if (alpha <= 0.0f) {
            return;
        }

        const auto renderer = API::get()->param()->renderer;
        if (renderer == nullptr || renderer->swapchain == nullptr) {
            return;
        }

        // Standalone fallback only. On the tested D3D11 host this callback is
        // after the eye copy, so it reaches the desktop backbuffer but not the
        // submitted OpenXR eyes. The Operator bridge above is the validated
        // final-eye path.
        auto swapchain = (IDXGISwapChain*)renderer->swapchain;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer{};
        if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
            return;
        }

        VignetteParams params{};
        if (!build_frame_params(alpha, params)) {
            if (!m_logged_frame_param_failure.exchange(true)) {
                API::get()->log_error("[CutsceneComfort] could not build D3D11 spatial window parameters");
            }
            return;
        }
        const bool drew = m_vignette_d3d11.draw(context, backbuffer.Get(), params);
        if (!m_logged_frame_draw.exchange(true)) {
            API::get()->log_info(
                "[CutsceneComfort] D3D11 spatial draw %s alpha %.3f size %.3f x %.3f m distance %.3f m curvature %.3f; left eye %.3f %.3f %.3f ray center %.3f %.3f %.3f ray x %.3f %.3f %.3f ray y %.3f %.3f %.3f",
                drew ? "started" : "failed", alpha, params.width, params.height, params.distance, params.curvature,
                params.eyes[0].origin[0], params.eyes[0].origin[1], params.eyes[0].origin[2],
                params.eyes[0].ray_center[0], params.eyes[0].ray_center[1], params.eyes[0].ray_center[2],
                params.eyes[0].ray_x[0], params.eyes[0].ray_x[1], params.eyes[0].ray_x[2],
                params.eyes[0].ray_y[0], params.eyes[0].ray_y[1], params.eyes[0].ray_y[2]);
        }
    }

    void draw_theater_frame(ID3D12GraphicsCommandList* command_list) {
        if (command_list == nullptr) {
            return;
        }

        const auto alpha = m_render_frame_enabled.load() ? advance_frame_fade() : 0.0f;
        if (m_operator_window_bridge_available.load()) {
            publish_operator_window(alpha);
            return;
        }
        if (alpha <= 0.0f) {
            return;
        }

        const auto renderer = API::get()->param()->renderer;
        if (renderer == nullptr || renderer->swapchain == nullptr) {
            return;
        }

        // Standalone fallback only. The D3D12 callback ordering has not been
        // re-proven for the current build; use the Operator bridge when the
        // mask must be visible in the submitted OpenXR eyes.
        auto swapchain = (IDXGISwapChain3*)renderer->swapchain;
        Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer{};
        if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
            return;
        }

        // Recorded into the host's list rather than submitted separately, so
        // it lands in order with everything else at present time. At this
        // point in that list the backbuffer has not been transitioned yet,
        // so it is still in PRESENT.
        VignetteParams params{};
        if (!build_frame_params(alpha, params)) {
            if (!m_logged_frame_param_failure.exchange(true)) {
                API::get()->log_error("[CutsceneComfort] could not build D3D12 spatial window parameters");
            }
            return;
        }
        const bool drew = m_vignette.draw(command_list, backbuffer.Get(), params, D3D12_RESOURCE_STATE_PRESENT);
        if (!m_logged_frame_draw.exchange(true)) {
            API::get()->log_info("[CutsceneComfort] D3D12 spatial draw %s alpha %.3f size %.3f x %.3f m distance %.3f m curvature %.3f",
                drew ? "started" : "failed", alpha, params.width, params.height, params.distance, params.curvature);
        }
    }

    void publish_frame_settings() {
        // The fixed screen replaces the headset view, so the spatial mask
        // stays down while that option is selected and the mode exists.
        const bool fixed_screen = m_settings.unvr_enabled && m_unvr_supported;
        const bool theater = m_settings.theater_frame && !fixed_screen;
        const bool was_enabled = m_render_frame_enabled.exchange(theater);
        if (m_settings.theater_frame && !was_enabled) {
            request_frame_recenter();
        }
        m_render_frame_lock_aspect = m_settings.frame_lock_aspect;
        m_render_frame_width_m = m_settings.frame_width_m;
        m_render_frame_height_m = m_settings.frame_height_m;
        m_render_frame_distance_m = m_settings.frame_distance_m;
        m_render_frame_feather_m = m_settings.frame_feather_m;
        m_render_frame_corner_radius_m = m_settings.frame_corner_radius_m;
        m_render_frame_curvature = m_settings.frame_curvature;
        m_render_frame_surround_red = m_settings.frame_surround_red;
        m_render_frame_surround_green = m_settings.frame_surround_green;
        m_render_frame_surround_blue = m_settings.frame_surround_blue;
        m_render_frame_opacity = m_settings.frame_opacity;
        m_render_frame_fade_seconds = m_settings.frame_fade_seconds;
        m_render_frame_single_view = m_settings.frame_single_view;
    }

    // ------------------------------------------------------------------
    // Fixed screen. UEVR's 2D screen mode already drops headset position and
    // rotation, clears the tracked view, and submits the shot as quads. The
    // stereo portal is that mode with per-eye separation left in place. Flat
    // 2D copies one eye's camera onto the other after UEVR has written it.
    // ------------------------------------------------------------------

    const char* unvr_phase_label() const {
        switch (m_unvr.phase()) {
        case comfort::UnvrPhase::FadeOutWorld: return "Fading the VR view out";
        case comfort::UnvrPhase::Screen: return m_settings.unvr_stereo ? "Stereo portal" : "Flat 2D screen";
        case comfort::UnvrPhase::FadeOutScreen: return "Fading the screen out";
        case comfort::UnvrPhase::FadeInWorld: return "Fading the VR view back in";
        case comfort::UnvrPhase::Idle: break;
        }
        return "";
    }

    void poll_unvr_support(float delta) {
        if (m_unvr_support_known) {
            return;
        }

        m_unvr_support_timer -= delta;
        if (m_unvr_support_timer > 0.0f) {
            return;
        }
        m_unvr_support_timer = 1.0f;

        if (!mod_string("VR_2DScreenMode").empty()) {
            m_unvr_supported = true;
            m_unvr_support_known = true;
            API::get()->log_info("[CutsceneComfort] UEVR 2D screen mode is available");
            return;
        }

        m_unvr_support_wait += 1.0f;
        if (m_unvr_support_wait >= 5.0f && !m_logged_unvr_missing) {
            m_logged_unvr_missing = true;
            API::get()->log_warn("[CutsceneComfort] VR_2DScreenMode was not found; the fixed screen stays off");
        }
    }

    void advance_unvr(float delta) {
        poll_unvr_support(delta);

        const bool preview = GetTickCount64() < m_unvr_preview_until_ms;
        if (m_unvr_suppress && !preview && !m_in_cutscene.load() && m_unvr.phase() == comfort::UnvrPhase::Idle) {
            m_unvr_suppress = false;
        }
        const bool want = !m_unvr_suppress && m_unvr_supported && m_settings.unvr_enabled &&
            (m_in_cutscene.load() || preview);
        const auto step = m_unvr.update(delta, want, m_settings.unvr_fade_seconds);

        if (step.fade_to_black) {
            if (!m_unvr_hold.active && !capture_unvr_journal()) {
                m_unvr_suppress = true;
                API::get()->log_error("[CutsceneComfort] could not read VR_2DScreenMode; fixed screen cancelled");
            }
            if (m_unvr_hold.active) {
                if (m_unvr_hold.have_roomscale) {
                    write_mod_bool("VR_RoomscaleMovement", false);
                }
                start_camera_fade(current_camera_fade(0.0f), 1.0f, step.fade_seconds, true);
                m_unvr_fade_armed = true;
            }
        }

        if (step.apply_screen && m_unvr_hold.active) {
            apply_unvr_screen();
        }

        if (step.release_screen) {
            release_unvr_hold();
        }

        if (step.fade_from_black && m_unvr_fade_armed) {
            start_camera_fade(current_camera_fade(1.0f), 0.0f, step.fade_seconds, false);
            m_unvr_fade_armed = false;
        }

        reassert_unvr_overrides();
    }

    void reassert_unvr_overrides() {
        if (!m_unvr_hold.active) {
            return;
        }

        const auto phase = m_unvr.phase();
        if (phase == comfort::UnvrPhase::Idle || phase == comfort::UnvrPhase::FadeInWorld) {
            return;
        }

        if (m_unvr_hold.have_roomscale && read_mod_bool("VR_RoomscaleMovement") != false) {
            write_mod_bool("VR_RoomscaleMovement", false);
        }

        if (!m_unvr_hold.screen_applied) {
            return;
        }

        if (read_mod_bool("VR_2DScreenMode") != true) {
            write_mod_bool("VR_2DScreenMode", true);
        }
        if (m_unvr_hold.have_gui && read_mod_bool("VR_EnableGUI") != true) {
            write_mod_bool("VR_EnableGUI", true);
        }

        if (m_unvr_hold.have_distance && std::abs(m_settings.unvr_distance_m - m_unvr_applied_distance) > 0.0005f) {
            write_mod_float("UI_Distance", m_settings.unvr_distance_m);
            m_unvr_applied_distance = m_settings.unvr_distance_m;
        }
        if (m_unvr_hold.have_height && std::abs(m_settings.unvr_height_m - m_unvr_applied_height) > 0.0005f) {
            write_mod_float("UI_Size", m_settings.unvr_height_m);
            m_unvr_applied_height = m_settings.unvr_height_m;
        }
        if (m_unvr_hold.have_follow && m_settings.unvr_follow_view != m_unvr_applied_follow) {
            write_mod_bool("UI_FollowView", m_settings.unvr_follow_view);
            m_unvr_applied_follow = m_settings.unvr_follow_view;
        }

        m_unvr_flatten_stereo.store(!m_settings.unvr_stereo);
    }

    bool capture_unvr_journal() {
        UnvrJournal next{};
        if (!read_mod_bool_into("VR_2DScreenMode", next.have_screen, next.screen) || !next.have_screen) {
            return false;
        }

        read_mod_bool_into("VR_EnableGUI", next.have_gui, next.gui);
        read_mod_bool_into("VR_RoomscaleMovement", next.have_roomscale, next.roomscale);
        read_mod_float_into("UI_Distance", next.have_distance, next.distance);
        read_mod_float_into("UI_Size", next.have_height, next.height);
        read_mod_bool_into("UI_FollowView", next.have_follow, next.follow);
        read_mod_float_into("UI_X_Offset", next.have_offset_x, next.offset_x);
        read_mod_float_into("UI_Y_Offset", next.have_offset_y, next.offset_y);
        read_mod_int_into("UI_OverlayType", next.have_overlay, next.overlay);
        next.active = true;
        m_unvr_hold = next;
        save_settings();
        return true;
    }

    void apply_unvr_screen() {
        write_mod_bool("VR_2DScreenMode", true);
        if (m_unvr_hold.have_gui) {
            write_mod_bool("VR_EnableGUI", true);
        }
        if (m_unvr_hold.have_distance) {
            write_mod_float("UI_Distance", m_settings.unvr_distance_m);
            m_unvr_applied_distance = m_settings.unvr_distance_m;
        }
        if (m_unvr_hold.have_height) {
            write_mod_float("UI_Size", m_settings.unvr_height_m);
            m_unvr_applied_height = m_settings.unvr_height_m;
        }
        if (m_unvr_hold.have_follow) {
            write_mod_bool("UI_FollowView", m_settings.unvr_follow_view);
            m_unvr_applied_follow = m_settings.unvr_follow_view;
        }
        if (m_unvr_hold.have_offset_x) {
            write_mod_float("UI_X_Offset", 0.0f);
        }
        if (m_unvr_hold.have_offset_y) {
            write_mod_float("UI_Y_Offset", 0.0f);
        }
        if (m_unvr_hold.have_overlay) {
            write_mod_int("UI_OverlayType", 0);
        }

        m_unvr_hold.screen_applied = true;
        m_unvr_flatten_stereo.store(!m_settings.unvr_stereo);
        API::get()->log_info("[CutsceneComfort] fixed screen up (%s), fade %.2fs",
            m_settings.unvr_stereo ? "stereo portal" : "flat 2D", m_settings.unvr_fade_seconds);
    }

    void release_unvr_hold() {
        m_unvr_flatten_stereo.store(false);

        if (!m_unvr_hold.active) {
            return;
        }

        const bool restored = restore_unvr_journal(m_unvr_hold);
        m_unvr_hold.screen_applied = false;
        if (!restored) {
            API::get()->log_error("[CutsceneComfort] fixed-screen restore did not stick; the journal is kept");
            save_settings();
            return;
        }

        API::VR::save_config();
        m_pending_uevr_save = false;
        m_unvr_hold = {};
        save_settings();
        API::get()->log_info("[CutsceneComfort] fixed screen released");
    }

    bool restore_unvr_journal(const UnvrJournal& journal) {
        if (!journal.active) {
            return true;
        }
        if (!journal.usable()) {
            return false;
        }

        bool ok = true;
        if (journal.have_screen) {
            ok = write_mod_bool("VR_2DScreenMode", journal.screen) && ok;
        }
        if (journal.have_gui) {
            ok = write_mod_bool("VR_EnableGUI", journal.gui) && ok;
        }
        if (journal.have_roomscale) {
            ok = write_mod_bool("VR_RoomscaleMovement", journal.roomscale) && ok;
        }
        if (journal.have_distance) {
            ok = write_mod_float("UI_Distance", journal.distance) && ok;
        }
        if (journal.have_height) {
            ok = write_mod_float("UI_Size", journal.height) && ok;
        }
        if (journal.have_follow) {
            ok = write_mod_bool("UI_FollowView", journal.follow) && ok;
        }
        if (journal.have_offset_x) {
            ok = write_mod_float("UI_X_Offset", journal.offset_x) && ok;
        }
        if (journal.have_offset_y) {
            ok = write_mod_float("UI_Y_Offset", journal.offset_y) && ok;
        }
        if (journal.have_overlay) {
            ok = write_mod_int("UI_OverlayType", journal.overlay) && ok;
        }
        return ok;
    }

    static std::string mod_string(const char* key) {
        return API::VR::get_mod_value<std::string>(key);
    }

    static bool mod_is_true(const std::string& value) {
        return value == "true" || value == "1";
    }

    static bool read_mod_bool(const char* key) {
        return mod_is_true(mod_string(key));
    }

    static bool read_mod_bool_into(const char* key, bool& have, bool& dest) {
        const auto value = mod_string(key);
        if (value.empty()) {
            have = false;
            return false;
        }
        have = true;
        dest = mod_is_true(value);
        return true;
    }

    static bool read_mod_float_into(const char* key, bool& have, float& dest) {
        const auto value = mod_string(key);
        if (value.empty()) {
            have = false;
            return false;
        }
        char* end = nullptr;
        const auto parsed = std::strtof(value.c_str(), &end);
        if (end == value.c_str() || !std::isfinite(parsed)) {
            have = false;
            return false;
        }
        have = true;
        dest = parsed;
        return true;
    }

    static bool read_mod_int_into(const char* key, bool& have, int& dest) {
        const auto value = mod_string(key);
        if (value.empty()) {
            have = false;
            return false;
        }
        char* end = nullptr;
        const auto parsed = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str()) {
            have = false;
            return false;
        }
        have = true;
        dest = static_cast<int>(parsed);
        return true;
    }

    static bool write_mod_bool(const char* key, bool value) {
        API::VR::set_mod_value(key, value);
        const auto after = mod_string(key);
        return !after.empty() && mod_is_true(after) == value;
    }

    static bool write_mod_float(const char* key, float value) {
        API::VR::set_mod_value(key, value);
        const auto after = mod_string(key);
        if (after.empty()) {
            return false;
        }
        char* end = nullptr;
        const auto parsed = std::strtof(after.c_str(), &end);
        return end != after.c_str() && std::abs(parsed - value) <= 0.02f;
    }

    static bool write_mod_int(const char* key, int value) {
        API::VR::set_mod_value(key, value);
        const auto after = mod_string(key);
        if (after.empty()) {
            return false;
        }
        return std::atoi(after.c_str()) == value;
    }

    static std::wstring clean_fname(const std::wstring& value) {
        auto name = value;
        while (!name.empty() && name.back() == L'\0') {
            name.pop_back();
        }
        return name;
    }

    float current_camera_fade(float fallback) {
        const auto manager = camera_manager();
        if (manager == nullptr) {
            return fallback;
        }

        const auto amount = manager->get_property_data<float>(L"FadeAmount");
        if (amount == nullptr || !std::isfinite(*amount)) {
            return fallback;
        }
        return std::clamp(*amount, 0.0f, 1.0f);
    }

    API::UObject* camera_manager() {
        const auto controller = API::get()->get_player_controller(0);
        if (controller == nullptr) {
            return nullptr;
        }

        const auto slot = controller->get_property_data<API::UObject*>(L"PlayerCameraManager");
        if (slot == nullptr || *slot == nullptr) {
            return nullptr;
        }
        if (!API::UObjectHook::exists(*slot)) {
            return nullptr;
        }
        return *slot;
    }

    API::UFunction* find_fade_function(API::UClass* cls) {
        int depth = 0;
        for (API::UStruct* type = cls; type != nullptr && depth < 64; type = type->get_super_struct(), ++depth) {
            if (auto* function = type->find_function(L"StartCameraFade")) {
                return function;
            }
        }
        return nullptr;
    }

    template <typename Visitor>
    void for_each_property(API::UStruct* type, Visitor&& visitor) {
        if (type == nullptr) {
            return;
        }

        if (auto* field = type->get_child_properties()) {
            for (; field != nullptr; field = field->get_next()) {
                visitor(static_cast<API::FProperty*>(field));
            }
            return;
        }

        for (auto* child = type->get_children(); child != nullptr; child = child->get_next()) {
            visitor(static_cast<API::FProperty*>(child));
        }
    }

    void start_camera_fade(float from_alpha, float to_alpha, float duration, bool hold) {
        if (m_fade_unavailable) {
            return;
        }

        const auto manager = camera_manager();
        if (manager == nullptr || manager->get_class() == nullptr) {
            return;
        }

        auto* function = find_fade_function(manager->get_class());
        if (function == nullptr) {
            m_fade_unavailable = true;
            if (!m_logged_fade_failure) {
                m_logged_fade_failure = true;
                API::get()->log_warn("[CutsceneComfort] PlayerCameraManager has no StartCameraFade; the screen switch will not fade");
            }
            return;
        }

        const auto size = function->get_properties_size();
        if (size <= 0 || size > 1024) {
            m_fade_unavailable = true;
            if (!m_logged_fade_failure) {
                m_logged_fade_failure = true;
                API::get()->log_warn("[CutsceneComfort] StartCameraFade has an unexpected parameter block");
            }
            return;
        }

        std::vector<unsigned char> params(static_cast<size_t>(size), 0);
        bool have_from = false;
        bool have_to = false;
        bool have_duration = false;
        bool have_color = false;
        bool have_hold = false;

        auto write_number = [&](int offset, bool as_double, float value) {
            if (offset < 0 || offset >= size) {
                return;
            }
            auto* dest = params.data() + offset;
            if (as_double) {
                const double wide = value;
                if (offset + static_cast<int>(sizeof(wide)) <= size) {
                    std::memcpy(dest, &wide, sizeof(wide));
                }
            } else if (offset + static_cast<int>(sizeof(value)) <= size) {
                std::memcpy(dest, &value, sizeof(value));
            }
        };

        for_each_property(function, [&](API::FProperty* prop) {
            if (prop == nullptr || prop->get_fname() == nullptr) {
                return;
            }

            const auto name = clean_fname(prop->get_fname()->to_string());
            const auto klass = prop->get_class() != nullptr ? clean_fname(prop->get_class()->get_name()) : std::wstring{};
            const auto offset = prop->get_offset();
            const bool as_double = klass.find(L"Double") != std::wstring::npos;

            if (name == L"FromAlpha") {
                write_number(offset, as_double, from_alpha);
                have_from = true;
            } else if (name == L"ToAlpha") {
                write_number(offset, as_double, to_alpha);
                have_to = true;
            } else if (name == L"Duration") {
                write_number(offset, as_double, duration);
                have_duration = true;
            } else if (name == L"Color") {
                have_color = write_fade_color(prop, klass, params, offset);
            } else if (name == L"bShouldFadeAudio") {
                set_param_bool(prop, klass, params, offset, false);
            } else if (name == L"bHoldWhenFinished") {
                have_hold = set_param_bool(prop, klass, params, offset, hold);
            }
        });

        if (!have_from || !have_to || !have_duration || !have_color || !have_hold) {
            m_fade_unavailable = true;
            if (!m_logged_fade_failure) {
                m_logged_fade_failure = true;
                API::get()->log_warn(
                    "[CutsceneComfort] StartCameraFade is missing a parameter (from %d to %d duration %d color %d hold %d)",
                    have_from ? 1 : 0, have_to ? 1 : 0, have_duration ? 1 : 0, have_color ? 1 : 0, have_hold ? 1 : 0);
            }
            return;
        }

        if (!m_logged_fade_layout) {
            m_logged_fade_layout = true;
            API::get()->log_info("[CutsceneComfort] StartCameraFade bound (%d byte parameter block)", size);
        }

        manager->process_event(function, params.data());
    }

    bool write_fade_color(API::FProperty* prop, const std::wstring& klass, std::vector<unsigned char>& params, int offset) {
        const auto size = static_cast<int>(params.size());
        if (offset < 0 || offset >= size) {
            return false;
        }

        if (klass.find(L"Struct") != std::wstring::npos) {
            auto* script = static_cast<API::FStructProperty*>(prop)->get_struct();
            bool wrote_any = false;
            bool wrote_alpha = false;
            for_each_property(script, [&](API::FProperty* member) {
                if (member == nullptr || member->get_fname() == nullptr) {
                    return;
                }
                const auto member_name = clean_fname(member->get_fname()->to_string());
                const auto member_class = member->get_class() != nullptr
                    ? clean_fname(member->get_class()->get_name())
                    : std::wstring{};
                float component = 0.0f;
                if (member_name == L"A") {
                    component = 1.0f;
                } else if (member_name != L"R" && member_name != L"G" && member_name != L"B") {
                    return;
                }

                const auto member_offset = offset + member->get_offset();
                const bool as_double = member_class.find(L"Double") != std::wstring::npos;
                bool wrote = false;
                if (as_double) {
                    const double wide = component;
                    if (member_offset >= 0 && member_offset + static_cast<int>(sizeof(wide)) <= size) {
                        std::memcpy(params.data() + member_offset, &wide, sizeof(wide));
                        wrote = true;
                    }
                } else if (member_offset >= 0 && member_offset + static_cast<int>(sizeof(component)) <= size) {
                    std::memcpy(params.data() + member_offset, &component, sizeof(component));
                    wrote = true;
                }
                if (wrote) {
                    wrote_any = true;
                    if (member_name == L"A") {
                        wrote_alpha = true;
                    }
                }
            });
            if (wrote_any && wrote_alpha) {
                return true;
            }
        }

        // FLinearColor is four floats, R G B A, when the struct members are
        // not reflected individually.
        if (offset + static_cast<int>(sizeof(float) * 4) > size) {
            return false;
        }
        const float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        std::memcpy(params.data() + offset, rgba, sizeof(rgba));
        return true;
    }

    bool set_param_bool(API::FProperty* prop, const std::wstring& klass, std::vector<unsigned char>& params,
        int offset, bool value) {
        if (klass.find(L"Bool") == std::wstring::npos || offset < 0 || offset >= static_cast<int>(params.size())) {
            return false;
        }

        static_cast<API::FBoolProperty*>(prop)->set_value_in_propbase(params.data() + offset, value);
        return true;
    }

    // Rolling average of what our tick work costs, for the UI readout.
    void track_tick_cost(LARGE_INTEGER t0, LARGE_INTEGER t1) {
        static LARGE_INTEGER freq{};
        if (freq.QuadPart == 0) {
            QueryPerformanceFrequency(&freq);
        }

        const auto us = (float)((t1.QuadPart - t0.QuadPart) * 1000000.0 / freq.QuadPart);
        m_tick_cost_us = m_tick_cost_us * 0.99f + us * 0.01f;

        // One line, once, after the average has had time to settle. Saves
        // having to read the number off the in game window.
        if (++m_ticks == 2000) {
            API::get()->log_info("[CutsceneComfort] averaging %.1f us per tick after 2000 ticks", m_tick_cost_us);
        }
    }

    // ------------------------------------------------------------------
    // ImGui plumbing
    // ------------------------------------------------------------------

    bool initialize_imgui() {
        if (m_imgui_ready) {
            return true;
        }

        if (!m_imgui_context_created) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();

            // Keep imgui from dropping an ini file into the game directory.
            ImGui::GetIO().IniFilename = nullptr;

            auto& style = ImGui::GetStyle();
            style.WindowRounding = 6.0f;
            style.FrameRounding = 4.0f;

            m_imgui_context_created = true;
        }

        const auto renderer = API::get()->param()->renderer;
        if (renderer == nullptr || renderer->swapchain == nullptr) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(((IDXGISwapChain*)renderer->swapchain)->GetDesc(&desc))) {
            return false;
        }

        m_wnd = desc.OutputWindow;

        if (!ImGui_ImplWin32_Init(m_wnd)) {
            return false;
        }

        if (renderer->renderer_type == UEVR_RENDERER_D3D11) {
            if (!g_d3d11.initialize()) {
                return false;
            }
        } else if (renderer->renderer_type == UEVR_RENDERER_D3D12) {
            if (!g_d3d12.initialize()) {
                return false;
            }
        } else {
            return false;
        }

        m_imgui_ready = true;
        API::get()->log_info("[CutsceneComfort] imgui up (%s)",
            renderer->renderer_type == UEVR_RENDERER_D3D11 ? "d3d11" : "d3d12");
        return true;
    }

private:
    static constexpr float k_debounce_seconds = 0.2f;
    static constexpr float k_startup_recovery_delay_seconds = 0.25f;
    static constexpr UINT_PTR k_input_subclass_id = 0x43434D45;

    // imgui plumbing
    HWND m_wnd{};
    bool m_imgui_ready{false};
    bool m_imgui_context_created{false};
    std::recursive_mutex m_imgui_mutex{};

    // engine lookups
    bool m_hook_activated{false};
    API::UClass* m_camera_actor_class{nullptr};
    float m_class_retry_timer{0.0f};

    // detection state, all touched on the game thread only
    std::atomic<bool> m_in_cutscene{false};
    bool m_candidate_state{false};
    float m_candidate_timer{0.0f};
    bool m_simulate{false};
    API::UObject* m_last_target{nullptr};
    API::UClass* m_last_class{nullptr};
    bool m_last_class_cinematic{false};
    std::string m_target_label{"none"};
    std::string m_detect_note{};

    // what the player had before we touched anything
    struct Baseline {
        float forward{};
        float right{};
        float up{};
        bool decoupled_pitch{};
        API::VR::AimMethod aim{};
        bool hook_disabled{};
        bool offsets_changed{};
        bool pitch_changed{};
        bool aim_changed{};
        bool hook_changed{};
        bool valid{};

        bool any_changed() const {
            return offsets_changed || pitch_changed || aim_changed || hook_changed;
        }
    } m_baseline{};

    // restore easing
    std::atomic<bool> m_restoring{false};
    float m_restore_t{0.0f};

    // UI visibility, touched from the window thread by the hotkey
    std::atomic<bool> m_show_ui{true};
    std::atomic<int> m_ui_hotkey_vk{VK_F7};
    std::atomic<bool> m_settings_dirty{false};
    std::atomic<bool> m_uevr_menu_open{false};
    std::atomic<bool> m_input_subclass_installed{false};
    std::atomic<bool> m_input_subclass_error_logged{false};
    std::atomic<float> m_companion_hit_min_x{0.0f};
    std::atomic<float> m_companion_hit_min_y{0.0f};
    std::atomic<float> m_companion_hit_max_x{0.0f};
    std::atomic<float> m_companion_hit_max_y{0.0f};
    float m_tick_cost_us{0.0f};
    int m_tick_errors{0};
    uint64_t m_ticks{0};

    // Spatial cutscene window. GPU objects and fade are render-thread only;
    // settings are published atomically and the anchor is mutex protected so
    // the UI can report/recenter it safely.
    Vignette m_vignette{};
    VignetteD3D11 m_vignette_d3d11{};
    float m_frame_fade{0.0f};
    uint64_t m_frame_last_tick_ms{0};
    std::atomic<uint64_t> m_frame_preview_until_ms{0};

    std::atomic<bool> m_render_frame_enabled{false};
    std::atomic<bool> m_render_frame_lock_aspect{false};
    std::atomic<float> m_render_frame_width_m{2.4f};
    std::atomic<float> m_render_frame_height_m{1.35f};
    std::atomic<float> m_render_frame_distance_m{2.0f};
    std::atomic<float> m_render_frame_feather_m{0.10f};
    std::atomic<float> m_render_frame_corner_radius_m{0.0f};
    std::atomic<float> m_render_frame_curvature{0.0f};
    std::atomic<float> m_render_frame_surround_red{0.0f};
    std::atomic<float> m_render_frame_surround_green{0.0f};
    std::atomic<float> m_render_frame_surround_blue{0.0f};
    std::atomic<float> m_render_frame_opacity{1.0f};
    std::atomic<float> m_render_frame_fade_seconds{0.6f};
    std::atomic<bool> m_render_frame_single_view{false};

    std::mutex m_frame_anchor_mutex{};
    bool m_frame_anchor_valid{false};
    glm::vec3 m_frame_anchor_origin{};
    glm::vec3 m_frame_anchor_right{1.0f, 0.0f, 0.0f};
    glm::vec3 m_frame_anchor_up{0.0f, 1.0f, 0.0f};
    glm::vec3 m_frame_anchor_back{0.0f, 0.0f, 1.0f};
    std::atomic<bool> m_frame_recenter_requested{true};
    std::atomic<bool> m_logged_frame_draw{false};
    std::atomic<bool> m_logged_frame_param_failure{false};
    std::atomic<bool> m_operator_window_bridge_available{false};

    // Originals for every UEVR setting the fixed screen touches. Kept in the
    // ini so a kill during the scene can put them back on the next launch.
    struct UnvrJournal {
        bool active{false};
        bool screen{false};
        bool have_screen{false};
        bool gui{true};
        bool have_gui{false};
        bool roomscale{false};
        bool have_roomscale{false};
        float distance{2.0f};
        bool have_distance{false};
        float height{1.5f};
        bool have_height{false};
        bool follow{false};
        bool have_follow{false};
        float offset_x{0.0f};
        bool have_offset_x{false};
        float offset_y{0.0f};
        bool have_offset_y{false};
        int overlay{0};
        bool have_overlay{false};
        bool screen_applied{false};

        bool usable() const {
            if (!active) {
                return true;
            }
            if (!have_screen) {
                return false;
            }

            auto finite_if = [](bool have, float value) {
                return !have || std::isfinite(value);
            };
            return finite_if(have_distance, distance) &&
                finite_if(have_height, height) &&
                finite_if(have_offset_x, offset_x) &&
                finite_if(have_offset_y, offset_y) &&
                (!have_overlay || (overlay >= 0 && overlay <= 3));
        }
    };

    struct ParkedState {
        float forward{};
        float right{};
        float up{};
        bool decoupled_pitch{};
        int aim{};
        bool hook_disabled{};
        bool active{};
        bool offsets_changed{};
        bool pitch_changed{};
        bool aim_changed{};
        bool hook_changed{};
        bool have_forward{};
        bool have_right{};
        bool have_up{};
        bool have_decoupled_pitch{};
        bool have_aim{};
        bool have_hook_disabled{};

        bool complete() const {
            return (!offsets_changed || (have_forward && have_right && have_up)) &&
                (!pitch_changed || have_decoupled_pitch) &&
                (!aim_changed || have_aim) &&
                (!hook_changed || have_hook_disabled);
        }

        bool valid() const {
            return (offsets_changed || pitch_changed || aim_changed || hook_changed) &&
                (!offsets_changed || (std::isfinite(forward) && std::isfinite(right) && std::isfinite(up))) &&
                (!aim_changed || (aim >= 0 && aim <= 3));
        }
    } m_parked_state{};

    comfort::UnvrTransition m_unvr{};
    UnvrJournal m_unvr_hold{};
    UnvrJournal m_parked_unvr{};
    std::atomic<bool> m_unvr_flatten_stereo{false};
    bool m_unvr_fade_armed{false};
    bool m_unvr_suppress{false};
    bool m_unvr_supported{false};
    bool m_unvr_support_known{false};
    bool m_logged_unvr_missing{false};
    bool m_pending_uevr_save{false};
    bool m_fade_unavailable{false};
    bool m_logged_fade_failure{false};
    bool m_logged_fade_layout{false};
    float m_unvr_support_timer{0.0f};
    float m_unvr_support_wait{0.0f};
    float m_unvr_applied_distance{0.0f};
    float m_unvr_applied_height{0.0f};
    bool m_unvr_applied_follow{true};
    uint64_t m_unvr_preview_until_ms{0};

    struct MonoAnchor {
        bool valid{false};
        bool is_double{false};
        int view_index{-1};
        double x{0.0};
        double y{0.0};
        double z{0.0};
    } m_mono_anchor{};

    float m_settings_poll_timer{0.0f};
    bool m_have_settings_timestamp{false};
    std::filesystem::file_time_type m_settings_last_write{};
    float m_startup_recovery_timer{0.0f};
    bool m_startup_recovery_done{false};
    bool m_startup_recovery_safe{true};

    Settings m_settings{};
};

// Instantiating the plugin registers it with the framework.
std::unique_ptr<CutsceneComfort> g_plugin{std::make_unique<CutsceneComfort>()};
