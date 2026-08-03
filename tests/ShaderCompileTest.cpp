#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>

#include "VignetteParams.hpp"

namespace {

bool compile(const char* entry, const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> shader{};
    Microsoft::WRL::ComPtr<ID3DBlob> errors{};
    const auto result = D3DCompile(
        k_vignette_shader,
        sizeof(k_vignette_shader) - 1,
        "CutsceneComfortSpatialWindow.hlsl",
        nullptr,
        nullptr,
        entry,
        target,
        D3DCOMPILE_WARNINGS_ARE_ERRORS,
        0,
        &shader,
        &errors);

    if (FAILED(result)) {
        std::fprintf(stderr, "%s/%s failed:\n%s\n", entry, target,
            errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown compiler error");
        return false;
    }
    return true;
}

} // namespace

int main() {
    const bool ok =
        compile("vs_main", "vs_4_0") &&
        compile("ps_main", "ps_4_0") &&
        compile("vs_main", "vs_5_0") &&
        compile("ps_main", "ps_5_0");
    return ok ? 0 : 1;
}
