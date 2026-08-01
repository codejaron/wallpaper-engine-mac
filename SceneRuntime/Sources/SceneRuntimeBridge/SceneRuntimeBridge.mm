#include <SceneRuntimeBridge/SceneRuntimeBridge.h>

#include "SceneRuntimeBridgeInternal.hpp"

#include <SceneCore/AssetResolver.hpp>
#include <SceneCore/FormatError.hpp>
#include <SceneCore/Texture.hpp>
#include <SceneShader/ShaderCompiler.hpp>
#include <SceneShader/ShaderPreprocessor.hpp>

#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

struct WESceneRuntimeAsset {
    we::scene::ResolvedAsset asset;
};

struct WESceneTexture {
    we::scene::Texture texture;
};

struct WESceneShaderTranslation {
    std::optional<we::scene::PreprocessedShaderPair> preprocessed;
    we::scene::TranslatedShaderPair sources;
};

namespace {

using we::scene::bridge::assignError;
using we::scene::bridge::assignExceptionError;
using we::scene::bridge::clearError;
using we::scene::bridge::requireOutput;

const we::scene::PreprocessedShader* shaderForMetadataStage(
    WESceneShaderTranslationRef translation,
    WESceneShaderStage stage,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (translation == nullptr || !translation->preprocessed) {
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "A preprocessed shader translation is required"
        );
        return nullptr;
    }
    switch (stage) {
        case WE_SCENE_SHADER_STAGE_VERTEX:
            return &translation->preprocessed->vertex;
        case WE_SCENE_SHADER_STAGE_FRAGMENT:
            return &translation->preprocessed->fragment;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Shader stage is invalid"
    );
    return nullptr;
}

WESceneRuntimeErrorCode bridgeErrorCode(
    we::scene::RuntimeErrorCode code
) noexcept {
    using CoreCode = we::scene::RuntimeErrorCode;

    switch (code) {
        case CoreCode::none:
            return WE_SCENE_RUNTIME_ERROR_NONE;
        case CoreCode::invalidArgument:
            return WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT;
        case CoreCode::assetsDirectoryNotFound:
            return WE_SCENE_RUNTIME_ERROR_ASSETS_DIRECTORY_NOT_FOUND;
        case CoreCode::assetsPathNotDirectory:
            return WE_SCENE_RUNTIME_ERROR_ASSETS_PATH_NOT_DIRECTORY;
        case CoreCode::assetsLayoutInvalid:
            return WE_SCENE_RUNTIME_ERROR_ASSETS_LAYOUT_INVALID;
        case CoreCode::scenePackageNotFound:
            return WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_NOT_FOUND;
        case CoreCode::scenePackageNotRegularFile:
            return WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_NOT_REGULAR_FILE;
        case CoreCode::scenePackageUnreadable:
            return WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_UNREADABLE;
        case CoreCode::filesystemFailure:
            return WE_SCENE_RUNTIME_ERROR_FILESYSTEM_FAILURE;
        case CoreCode::internalFailure:
            return WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE;
        case CoreCode::scenePackageInvalid:
            return WE_SCENE_RUNTIME_ERROR_SCENE_PACKAGE_INVALID;
        case CoreCode::gifScenePackageInvalid:
            return WE_SCENE_RUNTIME_ERROR_GIF_SCENE_PACKAGE_INVALID;
        case CoreCode::assetResolverFailure:
            return WE_SCENE_RUNTIME_ERROR_ASSET_RESOLVER_FAILURE;
    }

    return WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE;
}

WESceneRuntimeErrorCode bridgeFormatErrorCode(
    const we::scene::FormatError& error
) noexcept {
    using Code = we::scene::FormatErrorCode;
    switch (error.code()) {
        case Code::assetNotFound:
            return WE_SCENE_RUNTIME_ERROR_ASSET_NOT_FOUND;
        case Code::invalidMagic:
        case Code::invalidValue:
        case Code::invalidOffset:
        case Code::duplicateEntry:
        case Code::unsupportedFormat:
        case Code::decompressionFailed:
        case Code::malformedMetadata:
            return WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID;
        case Code::invalidArgument:
        case Code::unsafePath:
            return WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT;
        case Code::ioFailure:
            return WE_SCENE_RUNTIME_ERROR_FILESYSTEM_FAILURE;
        case Code::unexpectedEndOfFile:
            return WE_SCENE_RUNTIME_ERROR_ASSET_FORMAT_INVALID;
    }
    return WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE;
}

WESceneRuntimeErrorCode bridgeShaderErrorCode(
    we::scene::ShaderCompilePhase phase
) noexcept {
    using Phase = we::scene::ShaderCompilePhase;
    switch (phase) {
        case Phase::input:
            return WE_SCENE_RUNTIME_ERROR_SHADER_INPUT_INVALID;
        case Phase::vertexParse:
        case Phase::fragmentParse:
            return WE_SCENE_RUNTIME_ERROR_SHADER_PARSE_FAILURE;
        case Phase::link:
            return WE_SCENE_RUNTIME_ERROR_SHADER_LINK_FAILURE;
        case Phase::spirvGeneration:
        case Phase::crossCompilation:
            return WE_SCENE_RUNTIME_ERROR_SHADER_TRANSLATION_FAILURE;
    }
    return WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE;
}

void assignFormatError(
    WESceneRuntimeErrorRef* outError,
    const we::scene::FormatError& error
) noexcept {
    assignError(outError, bridgeFormatErrorCode(error), error.what());
}

}  // namespace

extern "C" WESceneRuntimeRef we_scene_runtime_create(
    const WESceneRuntimeConfiguration* configuration,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);

    if (configuration == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene runtime configuration is required"
        );
        return nullptr;
    }

    // This catch boundary is intentional: C++ exceptions must never cross the
    // public C ABI consumed by Swift.
    try {
        we::scene::RuntimeError coreError;
        auto runtime = we::scene::Runtime::create(
            {
                configuration->assets_directory != nullptr
                    ? configuration->assets_directory
                    : "",
                configuration->scene_package_path != nullptr
                    ? configuration->scene_package_path
                    : "",
            },
            coreError
        );

        if (!runtime) {
            assignError(
                out_error,
                bridgeErrorCode(coreError.code),
                coreError.message
            );
            return nullptr;
        }

        auto handle = std::make_unique<WESceneRuntime>();
        handle->runtime = std::shared_ptr<const we::scene::Runtime>(
            std::move(runtime)
        );
        return handle.release();
    } catch (const std::exception& exception) {
        assignExceptionError(out_error, "creating the scene runtime", exception.what());
        return nullptr;
    } catch (...) {
        assignExceptionError(out_error, "creating the scene runtime", nullptr);
        return nullptr;
    }
}

extern "C" void we_scene_runtime_destroy(WESceneRuntimeRef runtime) {
    delete runtime;
}

extern "C" const char* we_scene_runtime_package_version(
    WESceneRuntimeRef runtime
) {
    if (runtime == nullptr || !runtime->runtime) {
        return nullptr;
    }
    return runtime->runtime->assetResolver().scenePackage().version().c_str();
}

extern "C" int we_scene_runtime_package_entry_count(
    WESceneRuntimeRef runtime,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (runtime == nullptr || !runtime->runtime) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Scene runtime is required");
        return 0;
    }
    if (!requireOutput(out_count, out_error, "package entry count")) {
        return 0;
    }
    *out_count = runtime->runtime->assetResolver().scenePackage().entries().size();
    return 1;
}

extern "C" int we_scene_runtime_package_entry(
    WESceneRuntimeRef runtime,
    size_t index,
    WEScenePackageEntryInfo* out_entry,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (runtime == nullptr || !runtime->runtime) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Scene runtime is required");
        return 0;
    }
    if (!requireOutput(out_entry, out_error, "package entry")) {
        return 0;
    }
    const auto& entries = runtime->runtime->assetResolver().scenePackage().entries();
    if (index >= entries.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Package entry index is out of range");
        return 0;
    }
    const auto& entry = entries[index];
    out_entry->path = entry.path.c_str();
    out_entry->offset = entry.offset;
    out_entry->length = entry.length;
    return 1;
}

extern "C" WESceneRuntimeAssetRef we_scene_runtime_asset_create(
    WESceneRuntimeRef runtime,
    const char* path,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (runtime == nullptr || !runtime->runtime || path == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Scene runtime and asset path are required");
        return nullptr;
    }
    try {
        auto result = std::make_unique<WESceneRuntimeAsset>();
        result->asset = runtime->runtime->assetResolver().resolve(path);
        return result.release();
    } catch (const we::scene::FormatError& error) {
        assignFormatError(out_error, error);
        return nullptr;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "resolving an asset", error.what());
        return nullptr;
    } catch (...) {
        assignExceptionError(out_error, "resolving an asset", nullptr);
        return nullptr;
    }
}

extern "C" void we_scene_runtime_asset_destroy(
    WESceneRuntimeAssetRef asset
) {
    delete asset;
}

extern "C" const uint8_t* we_scene_runtime_asset_bytes(
    WESceneRuntimeAssetRef asset
) {
    if (asset == nullptr || asset->asset.bytes.empty()) {
        return nullptr;
    }
    return asset->asset.bytes.data();
}

extern "C" size_t we_scene_runtime_asset_length(
    WESceneRuntimeAssetRef asset
) {
    return asset == nullptr ? 0 : asset->asset.bytes.size();
}

extern "C" WESceneAssetSource we_scene_runtime_asset_source(
    WESceneRuntimeAssetRef asset
) {
    if (asset == nullptr) {
        return WE_SCENE_ASSET_SOURCE_UNKNOWN;
    }
    switch (asset->asset.source) {
        case we::scene::AssetSource::virtualMemory:
            return WE_SCENE_ASSET_SOURCE_VIRTUAL;
        case we::scene::AssetSource::projectDirectory:
            return WE_SCENE_ASSET_SOURCE_PROJECT_DIRECTORY;
        case we::scene::AssetSource::scenePackage:
            return WE_SCENE_ASSET_SOURCE_SCENE_PACKAGE;
        case we::scene::AssetSource::gifScenePackage:
            return WE_SCENE_ASSET_SOURCE_GIF_SCENE_PACKAGE;
        case we::scene::AssetSource::officialAssets:
            return WE_SCENE_ASSET_SOURCE_OFFICIAL_ASSETS;
    }
    return WE_SCENE_ASSET_SOURCE_UNKNOWN;
}

extern "C" WESceneTextureRef we_scene_runtime_texture_create(
    WESceneRuntimeRef runtime,
    const char* path,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (runtime == nullptr || !runtime->runtime || path == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Scene runtime and texture path are required");
        return nullptr;
    }
    try {
        auto result = std::make_unique<WESceneTexture>();
        result->texture = runtime->runtime->assetResolver().parseTexture(path);
        return result.release();
    } catch (const we::scene::FormatError& error) {
        assignFormatError(out_error, error);
        return nullptr;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "parsing a texture", error.what());
        return nullptr;
    } catch (...) {
        assignExceptionError(out_error, "parsing a texture", nullptr);
        return nullptr;
    }
}

extern "C" void we_scene_runtime_texture_destroy(
    WESceneTextureRef texture
) {
    delete texture;
}

extern "C" int we_scene_runtime_texture_info(
    WESceneTextureRef texture,
    WESceneTextureInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (texture == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Texture is required");
        return 0;
    }
    if (!requireOutput(out_info, out_error, "texture information")) {
        return 0;
    }
    const auto& value = texture->texture;
    *out_info = {};
    out_info->container_version = static_cast<uint32_t>(value.containerVersion);
    out_info->animation_version = static_cast<uint32_t>(value.animationVersion);
    out_info->format = static_cast<uint32_t>(value.format);
    out_info->file_format = static_cast<uint32_t>(value.fileFormat);
    out_info->flags = value.flags;
    out_info->width = value.width;
    out_info->height = value.height;
    out_info->texture_width = value.textureWidth;
    out_info->texture_height = value.textureHeight;
    out_info->gif_width = value.gifWidth;
    out_info->gif_height = value.gifHeight;
    out_info->image_count = value.imageCount;
    out_info->frame_count = static_cast<uint32_t>(value.frames.size());
    out_info->spritesheet_columns = value.spritesheetColumns;
    out_info->spritesheet_rows = value.spritesheetRows;
    out_info->spritesheet_frame_count = value.spritesheetFrameCount;
    out_info->spritesheet_duration = value.spritesheetDuration;
    out_info->is_video_mp4 = value.isVideoMp4 ? 1 : 0;
    out_info->has_extra_texi_field = value.hasExtraTEXIField ? 1 : 0;
    return 1;
}

extern "C" int we_scene_runtime_texture_mipmap_count(
    WESceneTextureRef texture,
    size_t image_index,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (texture == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Texture is required");
        return 0;
    }
    if (!requireOutput(out_count, out_error, "mipmap count")) {
        return 0;
    }
    if (image_index >= texture->texture.images.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Texture image index is out of range");
        return 0;
    }
    *out_count = texture->texture.images[image_index].mipmaps.size();
    return 1;
}

extern "C" int we_scene_runtime_texture_mipmap_info(
    WESceneTextureRef texture,
    size_t image_index,
    size_t mipmap_index,
    WESceneTextureMipmapInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (texture == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Texture is required");
        return 0;
    }
    if (!requireOutput(out_info, out_error, "mipmap information")) {
        return 0;
    }
    if (image_index >= texture->texture.images.size() ||
        mipmap_index >= texture->texture.images[image_index].mipmaps.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Texture mipmap index is out of range");
        return 0;
    }
    const auto& mipmap = texture->texture.images[image_index].mipmaps[mipmap_index];
    out_info->width = mipmap.width;
    out_info->height = mipmap.height;
    out_info->compression = mipmap.compression;
    out_info->uncompressed_size = mipmap.uncompressedSize;
    out_info->compressed_size = mipmap.compressedSize;
    return 1;
}

extern "C" int we_scene_runtime_texture_frame_info(
    WESceneTextureRef texture,
    size_t frame_index,
    WESceneTextureFrameInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (texture == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Texture is required");
        return 0;
    }
    if (!requireOutput(out_info, out_error, "frame information")) {
        return 0;
    }
    if (frame_index >= texture->texture.frames.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Texture frame index is out of range");
        return 0;
    }
    const auto& frame = texture->texture.frames[frame_index];
    out_info->frame_number = frame.frameNumber;
    out_info->frame_time = frame.frameTime;
    out_info->x = frame.x;
    out_info->y = frame.y;
    out_info->width = frame.width;
    out_info->height = frame.height;
    return 1;
}

extern "C" WESceneShaderTranslationRef we_scene_shader_translate(
    const WESceneShaderSources* sources,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (sources == nullptr || sources->vertex_source == nullptr ||
        sources->fragment_source == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_SHADER_INPUT_INVALID,
            "Vertex and fragment shader sources are required"
        );
        return nullptr;
    }

    try {
        auto translation = std::make_unique<WESceneShaderTranslation>();
        translation->sources = we::scene::ShaderCompiler::translate(
            sources->vertex_source,
            sources->fragment_source,
            sources->vertex_name != nullptr ? sources->vertex_name : "vertex",
            sources->fragment_name != nullptr ? sources->fragment_name : "fragment"
        );
        return translation.release();
    } catch (const we::scene::ShaderCompileError& error) {
        assignError(out_error, bridgeShaderErrorCode(error.phase()), error.what());
        return nullptr;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "translating a shader program", error.what());
        return nullptr;
    } catch (...) {
        assignExceptionError(out_error, "translating a shader program", nullptr);
        return nullptr;
    }
}

extern "C" WESceneShaderTranslationRef we_scene_runtime_shader_translate_files(
    WESceneRuntimeRef runtime,
    const char* vertex_path,
    const char* fragment_path,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (runtime == nullptr || !runtime->runtime || vertex_path == nullptr ||
        fragment_path == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Runtime, vertex shader path, and fragment shader path are required"
        );
        return nullptr;
    }

    try {
        auto translation = std::make_unique<WESceneShaderTranslation>();
        we::scene::ShaderPreprocessor preprocessor(
            runtime->runtime->assetResolver()
        );
        translation->preprocessed = preprocessor.preprocessFiles(
            vertex_path,
            fragment_path
        );
        translation->sources = we::scene::ShaderCompiler::translate(
            translation->preprocessed->vertex.source,
            translation->preprocessed->fragment.source,
            translation->preprocessed->vertex.name,
            translation->preprocessed->fragment.name
        );
        return translation.release();
    } catch (const we::scene::ShaderCompileError& error) {
        assignError(out_error, bridgeShaderErrorCode(error.phase()), error.what());
        return nullptr;
    } catch (const we::scene::FormatError& error) {
        const WESceneRuntimeErrorCode code =
            error.code() == we::scene::FormatErrorCode::assetNotFound
                ? WE_SCENE_RUNTIME_ERROR_ASSET_NOT_FOUND
                : WE_SCENE_RUNTIME_ERROR_SHADER_INPUT_INVALID;
        assignError(out_error, code, error.what());
        return nullptr;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "preprocessing a shader program", error.what());
        return nullptr;
    } catch (...) {
        assignExceptionError(out_error, "preprocessing a shader program", nullptr);
        return nullptr;
    }
}

extern "C" void we_scene_shader_translation_destroy(
    WESceneShaderTranslationRef translation
) {
    delete translation;
}

extern "C" const char* we_scene_shader_translation_vertex_source(
    WESceneShaderTranslationRef translation
) {
    return translation == nullptr ? nullptr : translation->sources.vertex.c_str();
}

extern "C" const char* we_scene_shader_translation_fragment_source(
    WESceneShaderTranslationRef translation
) {
    return translation == nullptr ? nullptr : translation->sources.fragment.c_str();
}

extern "C" const char* we_scene_shader_translation_preprocessed_vertex_source(
    WESceneShaderTranslationRef translation
) {
    if (translation == nullptr || !translation->preprocessed) {
        return nullptr;
    }
    return translation->preprocessed->vertex.source.c_str();
}

extern "C" const char* we_scene_shader_translation_preprocessed_fragment_source(
    WESceneShaderTranslationRef translation
) {
    if (translation == nullptr || !translation->preprocessed) {
        return nullptr;
    }
    return translation->preprocessed->fragment.source.c_str();
}

extern "C" int we_scene_shader_translation_parameter_count(
    WESceneShaderTranslationRef translation,
    WESceneShaderStage stage,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireOutput(out_count, out_error, "shader parameter count")) {
        return 0;
    }
    const we::scene::PreprocessedShader* shader = shaderForMetadataStage(
        translation,
        stage,
        out_error
    );
    if (shader == nullptr) {
        return 0;
    }
    *out_count = shader->parameters.size();
    return 1;
}

extern "C" int we_scene_shader_translation_parameter_info(
    WESceneShaderTranslationRef translation,
    WESceneShaderStage stage,
    size_t index,
    WESceneShaderParameterInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireOutput(out_info, out_error, "shader parameter information")) {
        return 0;
    }
    const we::scene::PreprocessedShader* shader = shaderForMetadataStage(
        translation,
        stage,
        out_error
    );
    if (shader == nullptr) {
        return 0;
    }
    if (index >= shader->parameters.size()) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Shader parameter index is out of range"
        );
        return 0;
    }

    const we::scene::ShaderParameterMetadata& parameter =
        shader->parameters[index];
    *out_info = {};
    out_info->type = parameter.type.c_str();
    out_info->name = parameter.name.c_str();
    out_info->material = parameter.material
        ? parameter.material->c_str()
        : nullptr;
    out_info->metadata_json = parameter.json.c_str();
    out_info->default_type = WE_SCENE_SHADER_PARAMETER_DEFAULT_NONE;
    if (!parameter.defaultValue) {
        return 1;
    }

    const we::scene::ShaderParameterDefault& value = *parameter.defaultValue;
    if (const bool* boolean = std::get_if<bool>(&value)) {
        out_info->default_type = WE_SCENE_SHADER_PARAMETER_DEFAULT_BOOLEAN;
        out_info->default_boolean = *boolean ? 1 : 0;
    } else if (const std::int64_t* integer = std::get_if<std::int64_t>(&value)) {
        out_info->default_type = WE_SCENE_SHADER_PARAMETER_DEFAULT_INTEGER;
        out_info->default_integer = *integer;
    } else if (const double* number = std::get_if<double>(&value)) {
        out_info->default_type = WE_SCENE_SHADER_PARAMETER_DEFAULT_NUMBER;
        out_info->default_number = *number;
    } else if (const std::string* string = std::get_if<std::string>(&value)) {
        out_info->default_type = WE_SCENE_SHADER_PARAMETER_DEFAULT_STRING;
        out_info->default_string = string->c_str();
    } else if (const std::vector<double>* vector =
                   std::get_if<std::vector<double>>(&value)) {
        out_info->default_type = WE_SCENE_SHADER_PARAMETER_DEFAULT_VECTOR;
        out_info->default_vector = vector->data();
        out_info->default_vector_count = vector->size();
    }
    return 1;
}

extern "C" const char* we_scene_shader_glslang_revision(void) {
    return we::scene::ShaderCompiler::glslangRevision();
}

extern "C" const char* we_scene_shader_spirv_cross_revision(void) {
    return we::scene::ShaderCompiler::spirvCrossRevision();
}

extern "C" const char* we_scene_runtime_assets_directory(
    WESceneRuntimeRef runtime
) {
    if (runtime == nullptr || !runtime->runtime) {
        return nullptr;
    }
    return runtime->runtime->assetsDirectory().c_str();
}

extern "C" const char* we_scene_runtime_scene_package_path(
    WESceneRuntimeRef runtime
) {
    if (runtime == nullptr || !runtime->runtime) {
        return nullptr;
    }
    return runtime->runtime->scenePackagePath().c_str();
}

extern "C" WESceneRuntimeErrorCode we_scene_runtime_error_code(
    WESceneRuntimeErrorRef error
) {
    return error != nullptr ? error->code : WE_SCENE_RUNTIME_ERROR_NONE;
}

extern "C" const char* we_scene_runtime_error_message(
    WESceneRuntimeErrorRef error
) {
    return error != nullptr ? error->message.c_str() : nullptr;
}

extern "C" const char* we_scene_runtime_error_asset_path(
    WESceneRuntimeErrorRef error
) {
    return error != nullptr && !error->assetPath.empty()
        ? error->assetPath.c_str()
        : nullptr;
}

extern "C" const char* we_scene_runtime_error_json_pointer(
    WESceneRuntimeErrorRef error
) {
    return error != nullptr && !error->jsonPointer.empty()
        ? error->jsonPointer.c_str()
        : nullptr;
}

extern "C" size_t we_scene_runtime_error_reference_count(
    WESceneRuntimeErrorRef error
) {
    return error != nullptr ? error->referenceChain.size() : 0;
}

extern "C" const char* we_scene_runtime_error_reference_at(
    WESceneRuntimeErrorRef error,
    size_t index
) {
    if (error == nullptr || index >= error->referenceChain.size()) {
        return nullptr;
    }
    return error->referenceChain[index].c_str();
}

extern "C" void we_scene_runtime_error_destroy(
    WESceneRuntimeErrorRef error
) {
    delete error;
}
