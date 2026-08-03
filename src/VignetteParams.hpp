// Shared between the D3D11 and D3D12 cutscene-window renderers so the two
// cannot drift apart. The geometry is expressed in tracking-space meters,
// not screen-space NDC: each eye gets a current ray basis while the aperture
// anchor remains fixed at the pose captured when the cutscene began.

#pragma once

#include <algorithm>
#include <cstdint>

struct VignetteParams {
    enum class Layout {
        // Both eyes in one texture, left half and right half.
        DOUBLE_WIDE,
        // One eye per frame, or a flat 2D view. A single centered aperture.
        SINGLE,
    };

    struct EyeProjection {
        float origin[3]{};
        float ray_center[3]{};
        float ray_x[3]{};
        float ray_y[3]{};
        bool valid{false};
    };

    Layout layout{Layout::DOUBLE_WIDE};
    EyeProjection eyes[2]{};

    float anchor_origin[3]{};
    float anchor_right[3]{1.0f, 0.0f, 0.0f};
    float anchor_up[3]{0.0f, 1.0f, 0.0f};
    float anchor_back[3]{0.0f, 0.0f, 1.0f};

    float width{2.4f};
    float height{1.35f};
    float distance{2.0f};
    float feather{0.10f};
    float corner_radius{0.0f};
    float curvature{0.0f};
    float surround_color[3]{0.0f, 0.0f, 0.0f};
    // Master surround fade, 0 is invisible and 1 is fully applied.
    float alpha{0.0f};
};

// Exact layout of the shader constant buffer/root constants. Nine float4s is
// 144 bytes: D3D11-aligned and comfortably below D3D12's 64-DWORD limit.
struct VignetteConstants {
    float eye_origin_and_full_mask[4]{};
    float ray_center_and_curvature[4]{};
    float ray_x_and_feather[4]{};
    float ray_y_and_corner_radius[4]{};
    float anchor_origin_and_distance[4]{};
    float anchor_right_and_half_width[4]{};
    float anchor_up_and_half_height[4]{};
    float anchor_back[4]{};
    float surround_color_and_opacity[4]{};
};

static_assert(sizeof(VignetteConstants) == sizeof(float) * 36);
static_assert(sizeof(VignetteConstants) % 16 == 0);

inline bool build_vignette_constants(const VignetteParams& params, uint32_t eye,
    VignetteConstants& constants) {
    if (eye > 1 || !params.eyes[eye].valid) {
        return false;
    }

    constants = {};
    const auto& projection = params.eyes[eye];
    const auto set_vector = [](float (&destination)[4], const float (&source)[3], float scalar) {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        destination[3] = scalar;
    };

    set_vector(constants.eye_origin_and_full_mask, projection.origin, 0.0f);
    set_vector(constants.ray_center_and_curvature, projection.ray_center,
        std::clamp(params.curvature, 0.0f, 1.0f));
    set_vector(constants.ray_x_and_feather, projection.ray_x,
        std::clamp(params.feather, 0.0f, 0.5f));
    set_vector(constants.ray_y_and_corner_radius, projection.ray_y,
        std::min(std::clamp(params.corner_radius, 0.0f, 2.0f),
            std::min(params.width, params.height) * 0.5f));
    set_vector(constants.anchor_origin_and_distance, params.anchor_origin,
        std::clamp(params.distance, 0.25f, 12.0f));
    set_vector(constants.anchor_right_and_half_width, params.anchor_right,
        std::clamp(params.width, 0.1f, 12.0f) * 0.5f);
    set_vector(constants.anchor_up_and_half_height, params.anchor_up,
        std::clamp(params.height, 0.1f, 8.0f) * 0.5f);
    set_vector(constants.anchor_back, params.anchor_back, 0.0f);
    set_vector(constants.surround_color_and_opacity, params.surround_color,
        std::clamp(params.alpha, 0.0f, 1.0f));
    return true;
}

// The pixel shader reconstructs each tracking-space ray, intersects the fixed
// flat plane or cylinder, and evaluates a rounded-rectangle distance field in
// physical meters. Both backends compile this exact source.
inline constexpr char k_vignette_shader[] = R"(
cbuffer Constants : register(b0) {
    float4 eye_origin_and_full_mask;
    float4 ray_center_and_curvature;
    float4 ray_x_and_feather;
    float4 ray_y_and_corner_radius;
    float4 anchor_origin_and_distance;
    float4 anchor_right_and_half_width;
    float4 anchor_up_and_half_height;
    float4 anchor_back;
    float4 surround_color_and_opacity;
};

struct PSIn {
    float4 pos : SV_POSITION;
    float2 ndc : TEXCOORD0;
};

PSIn vs_main(float2 pos : POSITION) {
    PSIn output;
    output.pos = float4(pos, 0.0f, 1.0f);
    output.ndc = pos;
    return output;
}

float4 ps_main(PSIn input) : SV_TARGET {
    float mask = 1.0f;

    if (eye_origin_and_full_mask.w < 0.5f) {
        float3 eye_origin = eye_origin_and_full_mask.xyz;
        float3 ray_direction = normalize(
            ray_center_and_curvature.xyz +
            input.ndc.x * ray_x_and_feather.xyz +
            input.ndc.y * ray_y_and_corner_radius.xyz);
        float3 origin = anchor_origin_and_distance.xyz;
        float3 right = anchor_right_and_half_width.xyz;
        float3 up = anchor_up_and_half_height.xyz;
        float3 back = anchor_back.xyz;
        float distance = max(anchor_origin_and_distance.w, 0.001f);
        float curvature = saturate(ray_center_and_curvature.w);
        float hit_t = -1.0f;
        float local_x = 0.0f;
        float local_y = 0.0f;

        if (curvature <= 0.0001f) {
            float3 plane_center = origin - back * distance;
            float denominator = dot(ray_direction, back);
            if (abs(denominator) > 0.00001f) {
                hit_t = dot(plane_center - eye_origin, back) / denominator;
                if (hit_t > 0.0001f) {
                    float3 local = eye_origin + ray_direction * hit_t - plane_center;
                    local_x = dot(local, right);
                    local_y = dot(local, up);
                }
            }
        } else {
            // Curvature 1 places the cylinder axis at the captured recenter
            // origin. Smaller values move it backward toward a flat plane.
            float radius = distance / curvature;
            float3 cylinder_center = origin + back * (radius - distance);
            float3 offset = eye_origin - cylinder_center;
            float3 horizontal_ray = ray_direction - up * dot(ray_direction, up);
            float3 horizontal_offset = offset - up * dot(offset, up);
            float a = dot(horizontal_ray, horizontal_ray);
            float b = 2.0f * dot(horizontal_offset, horizontal_ray);
            float c = dot(horizontal_offset, horizontal_offset) - radius * radius;
            float discriminant = b * b - 4.0f * a * c;
            if (a > 0.000001f && discriminant >= 0.0f) {
                float root = sqrt(discriminant);
                float near_t = (-b - root) / (2.0f * a);
                float far_t = (-b + root) / (2.0f * a);
                hit_t = near_t > 0.0001f ? near_t : (far_t > 0.0001f ? far_t : -1.0f);
                if (hit_t > 0.0f) {
                    float3 hit = eye_origin + ray_direction * hit_t;
                    float3 cylinder_local = hit - cylinder_center;
                    local_x = atan2(dot(cylinder_local, right), -dot(cylinder_local, back)) * radius;
                    local_y = dot(hit - origin, up);
                }
            }
        }

        if (hit_t > 0.0f) {
            float2 half_size = float2(
                anchor_right_and_half_width.w,
                anchor_up_and_half_height.w);
            float corner_radius = clamp(
                ray_y_and_corner_radius.w,
                0.0f,
                min(half_size.x, half_size.y));
            float2 rounded = abs(float2(local_x, local_y)) - (half_size - corner_radius);
            float signed_distance =
                length(max(rounded, 0.0f)) +
                min(max(rounded.x, rounded.y), 0.0f) -
                corner_radius;
            float feather = max(ray_x_and_feather.w, 0.0f);
            mask = feather > 0.00001f
                ? smoothstep(-feather, feather, signed_distance)
                : (signed_distance >= 0.0f ? 1.0f : 0.0f);
        }
    }

    return float4(
        surround_color_and_opacity.rgb,
        mask * saturate(surround_color_and_opacity.a));
}
)";
