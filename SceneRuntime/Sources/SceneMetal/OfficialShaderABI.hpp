#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace we::scene::metal {

// Array index is the official numeric builtin ID for Wallpaper Engine 2.8.0.42.
// Keep this registry versioned: it is an executable ABI, not the smaller
// public shader documentation surface.
inline constexpr std::array<std::string_view, 140>
    wallpaperEngine2842UniformNames{{
    std::string_view{"g_Alpha"},
    std::string_view{"g_Color"},
    std::string_view{"g_Color4"},
    std::string_view{"g_Time"},
    std::string_view{"g_Frametime"},
    std::string_view{"g_Daytime"},
    std::string_view{"g_TexelSizeHalf"},
    std::string_view{"g_TexelSize"},
    std::string_view{"g_Screen"},
    std::string_view{"g_ModelMatrix"},
    std::string_view{"g_ModelMatrixInverse"},
    std::string_view{"g_ViewProjectionMatrix"},
    std::string_view{"g_ModelViewProjectionMatrixInverse"},
    std::string_view{"g_ModelViewProjectionMatrix"},
    std::string_view{"g_NormalModelMatrix"},
    std::string_view{"g_AltModelMatrix"},
    std::string_view{"g_AltNormalModelMatrix"},
    std::string_view{"g_AltViewProjectionMatrix"},
    std::string_view{"g_ViewportViewProjectionMatrices"},
    std::string_view{"g_EffectModelMatrix"},
    std::string_view{"g_EffectModelViewProjectionMatrix"},
    std::string_view{"g_EffectModelViewProjectionMatrixInverse"},
    std::string_view{"g_EffectTextureProjectionMatrix"},
    std::string_view{"g_EffectTextureProjectionMatrixInverse"},
    std::string_view{"g_LayerModelMatrix"},
    std::string_view{"g_EyePosition"},
    std::string_view{"g_ViewForward"},
    std::string_view{"g_ViewRight"},
    std::string_view{"g_ViewUp"},
    std::string_view{"g_OrientationForward"},
    std::string_view{"g_OrientationRight"},
    std::string_view{"g_OrientationUp"},
    std::string_view{"g_Texture0"},
    std::string_view{"g_Texture1"},
    std::string_view{"g_Texture2"},
    std::string_view{"g_Texture3"},
    std::string_view{"g_Texture4"},
    std::string_view{"g_Texture5"},
    std::string_view{"g_Texture6"},
    std::string_view{"g_Texture7"},
    std::string_view{"g_Texture8"},
    std::string_view{"g_Texture9"},
    std::string_view{"g_Texture0Rotation"},
    std::string_view{"g_Texture1Rotation"},
    std::string_view{"g_Texture2Rotation"},
    std::string_view{"g_Texture3Rotation"},
    std::string_view{"g_Texture4Rotation"},
    std::string_view{"g_Texture5Rotation"},
    std::string_view{"g_Texture6Rotation"},
    std::string_view{"g_Texture7Rotation"},
    std::string_view{"g_Texture8Rotation"},
    std::string_view{"g_Texture9Rotation"},
    std::string_view{"g_Texture0Translation"},
    std::string_view{"g_Texture1Translation"},
    std::string_view{"g_Texture2Translation"},
    std::string_view{"g_Texture3Translation"},
    std::string_view{"g_Texture4Translation"},
    std::string_view{"g_Texture5Translation"},
    std::string_view{"g_Texture6Translation"},
    std::string_view{"g_Texture7Translation"},
    std::string_view{"g_Texture8Translation"},
    std::string_view{"g_Texture9Translation"},
    std::string_view{"g_Texture0Resolution"},
    std::string_view{"g_Texture1Resolution"},
    std::string_view{"g_Texture2Resolution"},
    std::string_view{"g_Texture3Resolution"},
    std::string_view{"g_Texture4Resolution"},
    std::string_view{"g_Texture5Resolution"},
    std::string_view{"g_Texture6Resolution"},
    std::string_view{"g_Texture7Resolution"},
    std::string_view{"g_Texture8Resolution"},
    std::string_view{"g_Texture9Resolution"},
    std::string_view{"g_Texture0Texel"},
    std::string_view{"g_Texture1Texel"},
    std::string_view{"g_Texture2Texel"},
    std::string_view{"g_Texture3Texel"},
    std::string_view{"g_Texture4Texel"},
    std::string_view{"g_Texture5Texel"},
    std::string_view{"g_Texture6Texel"},
    std::string_view{"g_Texture7Texel"},
    std::string_view{"g_Texture8Texel"},
    std::string_view{"g_Texture9Texel"},
    std::string_view{"g_Texture0MipMapInfo"},
    std::string_view{"g_Texture1MipMapInfo"},
    std::string_view{"g_Texture2MipMapInfo"},
    std::string_view{"g_Texture3MipMapInfo"},
    std::string_view{"g_Texture4MipMapInfo"},
    std::string_view{"g_Texture5MipMapInfo"},
    std::string_view{"g_Texture6MipMapInfo"},
    std::string_view{"g_Texture7MipMapInfo"},
    std::string_view{"g_Texture8MipMapInfo"},
    std::string_view{"g_Texture9MipMapInfo"},
    std::string_view{"g_TextureReductionScale"},
    std::string_view{"g_LightsColorRadius"},
    std::string_view{"g_LightsColorPremultiplied"},
    std::string_view{"g_LightsPosition"},
    std::string_view{"g_LightAmbientColor"},
    std::string_view{"g_LightSkylightColor"},
    std::string_view{"g_AudioSpectrum16Left"},
    std::string_view{"g_AudioSpectrum16Right"},
    std::string_view{"g_AudioSpectrum32Left"},
    std::string_view{"g_AudioSpectrum32Right"},
    std::string_view{"g_AudioSpectrum64Left"},
    std::string_view{"g_AudioSpectrum64Right"},
    std::string_view{"g_PointerPositionLast"},
    std::string_view{"g_PointerPosition"},
    std::string_view{"g_PointerState"},
    std::string_view{"g_ParallaxPosition"},
    std::string_view{"g_RenderVar0"},
    std::string_view{"g_RenderVar1"},
    std::string_view{"g_RenderVar2"},
    std::string_view{"g_RenderVar3"},
    std::string_view{"g_RenderVar4"},
    std::string_view{"g_Bones"},
    std::string_view{"g_BonesAlpha"},
    std::string_view{"g_BlendMap"},
    std::string_view{"g_MorphOffsets"},
    std::string_view{"g_MorphWeights"},
    std::string_view{"g_MorphBoneTransform"},
    std::string_view{"g_MorphBoneRules"},
    std::string_view{"g_LPoint_Color"},
    std::string_view{"g_LPoint_Origin"},
    std::string_view{"g_LSpot_Color"},
    std::string_view{"g_LSpot_Origin"},
    std::string_view{"g_LSpot_Direction"},
    std::string_view{"g_LSpot_Exponent"},
    std::string_view{"g_LTube_Color"},
    std::string_view{"g_LTube_OriginA"},
    std::string_view{"g_LTube_OriginB"},
    std::string_view{"g_LDirectional_Color"},
    std::string_view{"g_LDirectional_Direction"},
    std::string_view{"g_LFeature_ShadowProjection"},
    std::string_view{"g_LFeature_ShadowProjectionTransform"},
    std::string_view{"g_LFeature_ShadowPointProjection"},
    std::string_view{"g_LFeature_ShadowPointProjectionTransform"},
    std::string_view{"g_FogDistanceColor"},
    std::string_view{"g_FogDistanceParams"},
    std::string_view{"g_FogHeightColor"},
    std::string_view{"g_FogHeightParams"},
    std::string_view{"g_HDRParams"},
}};

[[nodiscard]] constexpr std::optional<std::size_t>
wallpaperEngine2842UniformId(std::string_view name) noexcept {
    for (std::size_t index = 0;
         index < wallpaperEngine2842UniformNames.size();
         ++index) {
        if (wallpaperEngine2842UniformNames[index] == name) return index;
    }
    return std::nullopt;
}

static_assert(wallpaperEngine2842UniformNames.size() == 140);
static_assert(
    wallpaperEngine2842UniformNames[106] == "g_PointerState"
);
static_assert(
    wallpaperEngine2842UniformId("g_PointerState") ==
    std::optional<std::size_t>{106}
);

}  // namespace we::scene::metal
