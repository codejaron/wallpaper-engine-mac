#include <SceneRuntimeBridge/SceneRuntimeBridge.h>

#include "SceneRuntimeBridgeInternal.hpp"

#include <SceneFrameGraph/SceneFrameGraph.hpp>

#include <exception>
#include <memory>
#include <string>
#include <variant>

namespace {

using namespace we::scene;
using we::scene::bridge::assignError;
using we::scene::bridge::assignExceptionError;
using we::scene::bridge::assignModelError;
using we::scene::bridge::clearError;
using we::scene::bridge::requireOutput;

bool requireFrameGraph(
    WESceneFrameGraphRef graph,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (graph != nullptr && graph->graph) {
        return true;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Scene frame graph is required"
    );
    return false;
}

bool requirePlan(
    WESceneFramePlanRef plan,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (plan != nullptr) {
        return true;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Scene frame plan is required"
    );
    return false;
}

std::optional<FrameRenderQuality> frameRenderQuality(
    WESceneFrameRenderQuality quality
) noexcept {
    switch (quality) {
        case WE_SCENE_FRAME_RENDER_POWER_SAVING:
            return FrameRenderQuality::powerSaving;
        case WE_SCENE_FRAME_RENDER_BALANCED:
            return FrameRenderQuality::balanced;
        case WE_SCENE_FRAME_RENDER_HIGH:
            return FrameRenderQuality::high;
        case WE_SCENE_FRAME_RENDER_ULTRA:
            return FrameRenderQuality::ultra;
    }
    return std::nullopt;
}

WESceneFrameResourceKind resourceKind(FrameResourceKind kind) noexcept {
    switch (kind) {
        case FrameResourceKind::assetTexture:
            return WE_SCENE_FRAME_RESOURCE_ASSET_TEXTURE;
        case FrameResourceKind::framebuffer:
            return WE_SCENE_FRAME_RESOURCE_FRAMEBUFFER;
        case FrameResourceKind::userPropertyTexture:
            return WE_SCENE_FRAME_RESOURCE_USER_PROPERTY_TEXTURE;
        case FrameResourceKind::hostTexture:
            return WE_SCENE_FRAME_RESOURCE_HOST_TEXTURE;
    }
    std::terminate();
}

WESceneFramebufferFormat framebufferFormatValue(FramebufferFormat format) noexcept {
    switch (format) {
        case FramebufferFormat::rgba8:
            return WE_SCENE_FRAMEBUFFER_RGBA8;
        case FramebufferFormat::r8:
            return WE_SCENE_FRAMEBUFFER_R8;
        case FramebufferFormat::rg16f:
            return WE_SCENE_FRAMEBUFFER_RG16F;
        case FramebufferFormat::r16f:
            return WE_SCENE_FRAMEBUFFER_R16F;
    }
    std::terminate();
}

WESceneFramebufferWrapMode framebufferWrapModeValue(
    FramebufferWrapMode mode
) noexcept {
    switch (mode) {
        case FramebufferWrapMode::clampToEdge:
            return WE_SCENE_FRAMEBUFFER_WRAP_CLAMP_TO_EDGE;
        case FramebufferWrapMode::clampToBorder:
            return WE_SCENE_FRAMEBUFFER_WRAP_CLAMP_TO_BORDER;
        case FramebufferWrapMode::repeat:
            return WE_SCENE_FRAMEBUFFER_WRAP_REPEAT;
    }
    std::terminate();
}

WESceneFrameOperationKind operationKindValue(FrameOperationKind kind) noexcept {
    switch (kind) {
        case FrameOperationKind::render:
            return WE_SCENE_FRAME_OPERATION_RENDER;
        case FrameOperationKind::copy:
            return WE_SCENE_FRAME_OPERATION_COPY;
        case FrameOperationKind::swap:
            return WE_SCENE_FRAME_OPERATION_SWAP;
        case FrameOperationKind::clear:
            return WE_SCENE_FRAME_OPERATION_CLEAR;
        case FrameOperationKind::text:
            return WE_SCENE_FRAME_OPERATION_TEXT;
        case FrameOperationKind::particle:
            return WE_SCENE_FRAME_OPERATION_PARTICLE;
    }
    std::terminate();
}

WESceneFrameGeometryKind geometryKind(FrameGeometryKind kind) noexcept {
    switch (kind) {
        case FrameGeometryKind::imageLocal:
            return WE_SCENE_FRAME_GEOMETRY_IMAGE_LOCAL;
        case FrameGeometryKind::fullscreenLocal:
            return WE_SCENE_FRAME_GEOMETRY_FULLSCREEN_LOCAL;
        case FrameGeometryKind::imageScene:
            return WE_SCENE_FRAME_GEOMETRY_IMAGE_SCENE;
        case FrameGeometryKind::passthroughCapture:
            return WE_SCENE_FRAME_GEOMETRY_PASSTHROUGH_CAPTURE;
        case FrameGeometryKind::puppetMesh:
            return WE_SCENE_FRAME_GEOMETRY_PUPPET_MESH;
        case FrameGeometryKind::lightVolume:
            return WE_SCENE_FRAME_GEOMETRY_LIGHT_VOLUME;
    }
    std::terminate();
}

WESceneFrameTexCoordKind texCoordKind(FrameTexCoordKind kind) noexcept {
    switch (kind) {
        case FrameTexCoordKind::image:
            return WE_SCENE_FRAME_TEXCOORD_IMAGE;
        case FrameTexCoordKind::full:
            return WE_SCENE_FRAME_TEXCOORD_FULL;
    }
    std::terminate();
}

WESceneFrameBlendingMode blendingMode(BlendingMode mode) noexcept {
    switch (mode) {
        case BlendingMode::normal:
            return WE_SCENE_FRAME_BLENDING_NORMAL;
        case BlendingMode::translucent:
            return WE_SCENE_FRAME_BLENDING_TRANSLUCENT;
        case BlendingMode::additive:
            return WE_SCENE_FRAME_BLENDING_ADDITIVE;
        case BlendingMode::alphaToCoverage:
            return WE_SCENE_FRAME_BLENDING_ALPHA_TO_COVERAGE;
    }
    std::terminate();
}

WESceneFrameCullingMode cullingMode(CullingMode mode) noexcept {
    switch (mode) {
        case CullingMode::normal:
            return WE_SCENE_FRAME_CULLING_NORMAL;
        case CullingMode::disabled:
            return WE_SCENE_FRAME_CULLING_DISABLED;
    }
    std::terminate();
}

WESceneFrameDepthMode depthMode(DepthMode mode) noexcept {
    switch (mode) {
        case DepthMode::disabled:
            return WE_SCENE_FRAME_DEPTH_DISABLED;
        case DepthMode::enabled:
            return WE_SCENE_FRAME_DEPTH_ENABLED;
        case DepthMode::greater:
            // The public bridge exposes enablement only; the Metal executor
            // retains the internal Greater comparison for shadow casters.
            return WE_SCENE_FRAME_DEPTH_ENABLED;
    }
    std::terminate();
}

WESceneFramePlanIssueCode issueCode(FramePlanIssueCode code) noexcept {
    switch (code) {
        case FramePlanIssueCode::textRenderingUnavailable:
            return WE_SCENE_FRAME_ISSUE_TEXT_RENDERING_UNAVAILABLE;
        case FramePlanIssueCode::soundRuntimeUnavailable:
            return WE_SCENE_FRAME_ISSUE_SOUND_RUNTIME_UNAVAILABLE;
        case FramePlanIssueCode::scriptRuntimeUnavailable:
            return WE_SCENE_FRAME_ISSUE_SCRIPT_RUNTIME_UNAVAILABLE;
        case FramePlanIssueCode::passthroughUnavailable:
            return WE_SCENE_FRAME_ISSUE_PASSTHROUGH_UNAVAILABLE;
        case FramePlanIssueCode::puppetUnavailable:
            return WE_SCENE_FRAME_ISSUE_PUPPET_UNAVAILABLE;
        case FramePlanIssueCode::composeUnavailable:
            return WE_SCENE_FRAME_ISSUE_COMPOSE_UNAVAILABLE;
        case FramePlanIssueCode::imageMaterialUnavailable:
            return WE_SCENE_FRAME_ISSUE_IMAGE_MATERIAL_UNAVAILABLE;
        case FramePlanIssueCode::framebufferDescriptorMissing:
            return WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_DESCRIPTOR_MISSING;
        case FramePlanIssueCode::framebufferReadBeforeWrite:
            return WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_READ_BEFORE_WRITE;
        case FramePlanIssueCode::framebufferFeedbackLoop:
            return WE_SCENE_FRAME_ISSUE_FRAMEBUFFER_FEEDBACK_LOOP;
        case FramePlanIssueCode::audioInputUnavailable:
            return WE_SCENE_FRAME_ISSUE_AUDIO_INPUT_UNAVAILABLE;
        case FramePlanIssueCode::perspectiveProjectionUnavailable:
            return WE_SCENE_FRAME_ISSUE_PERSPECTIVE_PROJECTION_UNAVAILABLE;
        case FramePlanIssueCode::effectPassPlanningFailed:
            return WE_SCENE_FRAME_ISSUE_EFFECT_PASS_PLANNING_FAILED;
        case FramePlanIssueCode::objectPlanningFailed:
            return WE_SCENE_FRAME_ISSUE_OBJECT_PLANNING_FAILED;
    }
    std::terminate();
}

WESceneFramePlanIssueSeverity issueSeverity(
    FramePlanIssueSeverity severity
) noexcept {
    switch (severity) {
        case FramePlanIssueSeverity::warning:
            return WE_SCENE_FRAME_ISSUE_WARNING;
        case FramePlanIssueSeverity::skipPass:
            return WE_SCENE_FRAME_ISSUE_SKIP_PASS;
        case FramePlanIssueSeverity::skipObject:
            return WE_SCENE_FRAME_ISSUE_SKIP_OBJECT;
        case FramePlanIssueSeverity::frameFatal:
            return WE_SCENE_FRAME_ISSUE_FRAME_FATAL;
    }
    std::terminate();
}

void resourceInfo(
    const FrameResourceRef& resource,
    WESceneFrameResourceInfo& output
) noexcept {
    output.kind = resourceKind(resource.kind);
    output.id = resource.id.c_str();
    output.logical_name = resource.logicalName.c_str();
}

void valueInfo(
    const EvaluatedValue& evaluated,
    WEScenePropertyValue& output
) noexcept {
    output = {};
    output.boolean_value = evaluated.value.boolean() ? 1 : 0;
    output.integer_value = evaluated.value.integer();
    output.number_value = evaluated.value.number();
    const auto& vector = evaluated.value.vector();
    output.vector_value = {vector[0], vector[1], vector[2], vector[3]};
    switch (evaluated.value.type()) {
        case RuntimeValueType::null:
            output.type = WE_SCENE_VALUE_NULL;
            break;
        case RuntimeValueType::boolean:
            output.type = WE_SCENE_VALUE_BOOLEAN;
            break;
        case RuntimeValueType::integer:
            output.type = WE_SCENE_VALUE_INTEGER;
            break;
        case RuntimeValueType::floating:
            output.type = WE_SCENE_VALUE_NUMBER;
            break;
        case RuntimeValueType::string:
            output.type = WE_SCENE_VALUE_STRING;
            output.string_value = evaluated.value.string().c_str();
            break;
        case RuntimeValueType::vector2:
        case RuntimeValueType::vector3:
        case RuntimeValueType::vector4:
            output.type = WE_SCENE_VALUE_OBJECT;
            output.component_count = evaluated.value.componentCount();
            break;
    }
}

void originInfo(
    const FramePassOrigin& origin,
    WESceneFrameOperationInfo& output
) noexcept {
    output.image_index = origin.imageIndex;
    output.object_id = origin.objectId;
    output.has_effect = origin.effectIndex.has_value() ? 1 : 0;
    output.effect_index = origin.effectIndex.value_or(0);
    output.has_effect_pass = origin.effectPassIndex.has_value() ? 1 : 0;
    output.effect_pass_index = origin.effectPassIndex.value_or(0);
    output.material_pass_index = origin.materialPassIndex;
}

bool operationAt(
    WESceneFramePlanRef plan,
    std::size_t index,
    const FrameOperation** out,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (!requirePlan(plan, outError) ||
        !requireOutput(out, outError, "frame operation")) {
        return false;
    }
    if (index >= plan->plan.operations.size()) {
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Scene frame operation index is out of range"
        );
        return false;
    }
    *out = &plan->plan.operations[index];
    return true;
}

}  // namespace

extern "C" WESceneFrameGraphRef we_scene_graph_frame_graph_create(
    WESceneGraphRef graph,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (graph == nullptr || !graph->graph) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene graph is required"
        );
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneFrameGraph>();
        handle->graph = SceneFrameGraph::create(graph->graph);
        return handle.release();
    } catch (const SceneModelError& error) {
        assignModelError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating the scene frame graph", error.what());
    } catch (...) {
        assignExceptionError(out_error, "creating the scene frame graph", nullptr);
    }
    return nullptr;
}

extern "C" void we_scene_frame_graph_destroy(WESceneFrameGraphRef graph) {
    delete graph;
}

extern "C" WESceneFramePlanRef we_scene_frame_graph_plan_create(
    WESceneFrameGraphRef graph,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireFrameGraph(graph, out_error)) {
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneFramePlan>();
        handle->plan = graph->graph->snapshot();
        return handle.release();
    } catch (const SceneModelError& error) {
        assignModelError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating a scene frame plan", error.what());
    } catch (...) {
        assignExceptionError(out_error, "creating a scene frame plan", nullptr);
    }
    return nullptr;
}

extern "C" WESceneFramePlanRef we_scene_frame_graph_plan_create_with_inputs(
    WESceneFrameGraphRef graph,
    const WESceneFrameInputs* inputs,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireFrameGraph(graph, out_error)) return nullptr;
    if (inputs == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Scene frame inputs are required");
        return nullptr;
    }
    if (!std::isfinite(inputs->time_seconds) || inputs->time_seconds < 0 ||
        !std::isfinite(inputs->frame_time_seconds) || inputs->frame_time_seconds < 0 ||
        !std::isfinite(inputs->pointer_x) || !std::isfinite(inputs->pointer_y)) {
        assignError(
            out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene frame inputs must be finite and time values must be non-negative"
        );
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneFramePlan>();
        handle->plan = graph->graph->snapshot({
            .runtimeSeconds = inputs->time_seconds,
            .frameTimeSeconds = inputs->frame_time_seconds,
            .pointerX = inputs->pointer_x,
            .pointerY = inputs->pointer_y,
        });
        return handle.release();
    } catch (const SceneModelError& error) {
        assignModelError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating an evaluated scene frame plan", error.what());
    } catch (...) {
        assignExceptionError(out_error, "creating an evaluated scene frame plan", nullptr);
    }
    return nullptr;
}

extern "C" WESceneFramePlanRef
we_scene_frame_graph_plan_create_with_inputs_and_render_quality(
    WESceneFrameGraphRef graph,
    const WESceneFrameInputs* inputs,
    WESceneFrameRenderQuality renderQuality,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireFrameGraph(graph, out_error)) return nullptr;
    if (inputs == nullptr) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene frame inputs are required"
        );
        return nullptr;
    }
    const auto quality = frameRenderQuality(renderQuality);
    if (!quality) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Unknown scene frame render quality"
        );
        return nullptr;
    }
    if (!std::isfinite(inputs->time_seconds) || inputs->time_seconds < 0 ||
        !std::isfinite(inputs->frame_time_seconds) || inputs->frame_time_seconds < 0 ||
        !std::isfinite(inputs->pointer_x) || !std::isfinite(inputs->pointer_y)) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "Scene frame inputs must be finite and time values must be non-negative"
        );
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneFramePlan>();
        handle->plan = graph->graph->snapshot(
            {
                .runtimeSeconds = inputs->time_seconds,
                .frameTimeSeconds = inputs->frame_time_seconds,
                .pointerX = inputs->pointer_x,
                .pointerY = inputs->pointer_y,
            },
            std::nullopt,
            *quality
        );
        return handle.release();
    } catch (const SceneModelError& error) {
        assignModelError(out_error, error);
    } catch (const std::exception& error) {
        assignExceptionError(
            out_error,
            "creating an evaluated scene frame plan",
            error.what()
        );
    } catch (...) {
        assignExceptionError(
            out_error,
            "creating an evaluated scene frame plan",
            nullptr
        );
    }
    return nullptr;
}

extern "C" void we_scene_frame_plan_destroy(WESceneFramePlanRef plan) {
    delete plan;
}

extern "C" int we_scene_frame_plan_info(
    WESceneFramePlanRef plan,
    WESceneFramePlanInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "frame plan information")) {
        return 0;
    }
    const FramePlan& value = plan->plan;
    *out_info = {};
    out_info->model_revision = value.modelRevision;
    out_info->width = value.width;
    out_info->height = value.height;
    out_info->clear_enabled = value.clearEnabled ? 1 : 0;
    out_info->clear_red = value.clearColor.red;
    out_info->clear_green = value.clearColor.green;
    out_info->clear_blue = value.clearColor.blue;
    out_info->clear_alpha = value.clearColor.alpha;
    out_info->camera_center = {
        value.camera.center.x,
        value.camera.center.y,
        value.camera.center.z,
    };
    out_info->camera_eye = {
        value.camera.eye.x,
        value.camera.eye.y,
        value.camera.eye.z,
    };
    out_info->camera_up = {
        value.camera.up.x,
        value.camera.up.y,
        value.camera.up.z,
    };
    out_info->camera_near_plane = value.camera.nearPlane;
    out_info->camera_far_plane = value.camera.farPlane;
    out_info->camera_field_of_view = value.camera.fieldOfView;
    out_info->camera_projection_auto =
        value.camera.orthogonalProjectionAuto ? 1 : 0;
    out_info->camera_projection_width =
        value.camera.orthogonalProjectionWidth;
    out_info->camera_projection_height =
        value.camera.orthogonalProjectionHeight;
    out_info->parallax_enabled = value.parallax.enabled ? 1 : 0;
    out_info->parallax_amount = value.parallax.amount;
    out_info->parallax_delay = value.parallax.delay;
    out_info->parallax_mouse_influence = value.parallax.mouseInfluence;
    out_info->is_executable = value.isExecutable() ? 1 : 0;
    out_info->framebuffer_count = value.framebuffers.size();
    out_info->image_count = value.images.size();
    out_info->text_count = value.texts.size();
    out_info->particle_count = value.particles.size();
    out_info->camera_orthographic = value.camera.orthographic ? 1 : 0;
    out_info->camera_perspective_override_field_of_view =
        value.camera.perspectiveOverrideFieldOfView;
    out_info->operation_count = value.operations.size();
    out_info->issue_count = value.issues.size();
    out_info->script_evaluation_count = value.scriptEvaluations.size();
    resourceInfo(value.output, out_info->output);
    return 1;
}

extern "C" int we_scene_frame_plan_framebuffer_info(
    WESceneFramePlanRef plan,
    std::size_t index,
    WESceneFramebufferInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "framebuffer information")) {
        return 0;
    }
    if (index >= plan->plan.framebuffers.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene framebuffer index is out of range");
        return 0;
    }
    const auto& value = plan->plan.framebuffers[index];
    *out_info = {};
    resourceInfo(value.resource, out_info->resource);
    out_info->format = framebufferFormatValue(value.format);
    out_info->wrap_mode = framebufferWrapModeValue(value.wrapMode);
    out_info->width = value.width;
    out_info->height = value.height;
    out_info->scale = value.scale;
    out_info->unique = value.unique ? 1 : 0;
    return 1;
}

extern "C" int we_scene_frame_plan_image_info(
    WESceneFramePlanRef plan,
    std::size_t index,
    WESceneFrameImageInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "frame image information")) {
        return 0;
    }
    if (index >= plan->plan.images.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame image index is out of range");
        return 0;
    }
    const auto& value = plan->plan.images[index];
    *out_info = {};
    out_info->object_index = value.objectIndex;
    out_info->object_id = value.objectId;
    out_info->visible = value.visible ? 1 : 0;
    out_info->width = value.size.x;
    out_info->height = value.size.y;
    out_info->world_transform = {
        .origin = {value.worldTransform.origin.x, value.worldTransform.origin.y,
                   value.worldTransform.origin.z},
        .scale = {value.worldTransform.scale.x, value.worldTransform.scale.y,
                  value.worldTransform.scale.z},
        .angles = {value.worldTransform.angles.x, value.worldTransform.angles.y,
                   value.worldTransform.angles.z},
    };
    resourceInfo(value.source, out_info->source);
    resourceInfo(value.compositeA, out_info->composite_a);
    resourceInfo(value.compositeB, out_info->composite_b);
    out_info->perspective = value.perspective ? 1 : 0;
    return 1;
}

extern "C" int we_scene_frame_plan_text_info(
    WESceneFramePlanRef plan,
    std::size_t index,
    WESceneFrameTextInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "frame text information")) return 0;
    if (index >= plan->plan.texts.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame text index is out of range");
        return 0;
    }
    const FrameTextDescriptor& value = plan->plan.texts[index];
    *out_info = {};
    out_info->object_index = value.objectIndex;
    out_info->object_id = value.objectId;
    out_info->visible = value.visible ? 1 : 0;
    out_info->text = value.text.c_str();
    out_info->font = value.font.c_str();
    out_info->point_size = value.pointSize;
    out_info->width = value.size.x;
    out_info->height = value.size.y;
    out_info->color_red = value.color.red;
    out_info->color_green = value.color.green;
    out_info->color_blue = value.color.blue;
    out_info->color_alpha = value.color.alpha;
    out_info->alpha = value.alpha;
    out_info->padding_x = value.padding.x;
    out_info->padding_y = value.padding.y;
    out_info->spacing_x = value.spacing.x;
    out_info->spacing_y = value.spacing.y;
    out_info->world_transform = {
        .origin = {value.worldTransform.origin.x, value.worldTransform.origin.y,
                   value.worldTransform.origin.z},
        .scale = {value.worldTransform.scale.x, value.worldTransform.scale.y,
                  value.worldTransform.scale.z},
        .angles = {value.worldTransform.angles.x, value.worldTransform.angles.y,
                   value.worldTransform.angles.z},
    };
    out_info->horizontal_alignment = value.horizontalAlignment.c_str();
    out_info->vertical_alignment = value.verticalAlignment.c_str();
    out_info->limit_rows = value.limitRows ? 1 : 0;
    out_info->limit_use_ellipsis = value.limitUseEllipsis ? 1 : 0;
    out_info->limit_width = value.limitWidth ? 1 : 0;
    out_info->max_rows = value.maxRows;
    out_info->max_width = value.maxWidth;
    out_info->perspective = value.perspective ? 1 : 0;
    return 1;
}

extern "C" int we_scene_frame_plan_particle_info(
    WESceneFramePlanRef plan,
    std::size_t index,
    WESceneFrameParticleInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "frame particle information")) {
        return 0;
    }
    if (index >= plan->plan.particles.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame particle index is out of range");
        return 0;
    }
    const FrameParticleDescriptor& value = plan->plan.particles[index];
    *out_info = {};
    out_info->object_index = value.objectIndex;
    out_info->object_id = value.objectId;
    out_info->visible = value.visible ? 1 : 0;
    out_info->world_transform = {
        .origin = {value.worldTransform.origin.x, value.worldTransform.origin.y,
                   value.worldTransform.origin.z},
        .scale = {value.worldTransform.scale.x, value.worldTransform.scale.y,
                  value.worldTransform.scale.z},
        .angles = {value.worldTransform.angles.x, value.worldTransform.angles.y,
                   value.worldTransform.angles.z},
    };
    out_info->definition_identity = value.definitionIdentity.c_str();
    out_info->shader = value.shader.c_str();
    out_info->vertex_shader_path = value.vertexShaderPath.c_str();
    out_info->fragment_shader_path = value.fragmentShaderPath.c_str();
    out_info->blending = blendingMode(value.blending);
    out_info->culling = cullingMode(value.culling);
    out_info->depth_test = depthMode(value.depthTest);
    out_info->depth_write = depthMode(value.depthWrite);
    resourceInfo(value.texture0, out_info->texture0);
    out_info->parallax_depth_x = value.parallaxDepth.x;
    out_info->parallax_depth_y = value.parallaxDepth.y;
    out_info->perspective = value.perspective ? 1 : 0;
    out_info->animation_mode = value.animationMode.c_str();
    out_info->sequence_multiplier = value.sequenceMultiplier;
    out_info->max_count = value.configuration.maxCount;
    out_info->fixed_step_seconds = value.configuration.fixedStepSeconds;
    out_info->start_time = value.configuration.startTime;
    out_info->flags = value.configuration.flags;
    out_info->override_enabled = value.configuration.overrides.enabled ? 1 : 0;
    out_info->override_alpha = value.configuration.overrides.alpha;
    out_info->override_size = value.configuration.overrides.size;
    out_info->override_lifetime = value.configuration.overrides.lifetime;
    out_info->override_rate = value.configuration.overrides.rate;
    out_info->override_speed = value.configuration.overrides.speed;
    out_info->override_count = value.configuration.overrides.count;
    out_info->override_color = {
        value.configuration.overrides.color.x,
        value.configuration.overrides.color.y,
        value.configuration.overrides.color.z,
    };
    out_info->override_color_multiplier = {
        value.configuration.overrides.colorMultiplier.x,
        value.configuration.overrides.colorMultiplier.y,
        value.configuration.overrides.colorMultiplier.z,
    };
    out_info->emitter_count = value.configuration.emitters.size();
    out_info->initializer_count = value.configuration.initializers.size();
    out_info->operator_count = value.configuration.operators.size();
    out_info->control_point_count = value.configuration.controlPoints.size();
    out_info->combo_count = value.combos.size();
    out_info->renderer_kind = static_cast<int>(value.renderer.kind);
    out_info->renderer_length = value.renderer.length;
    out_info->renderer_max_length = value.renderer.maxLength;
    out_info->renderer_min_length = value.renderer.minLength;
    out_info->renderer_subdivision = value.renderer.subdivision;
    out_info->renderer_segments = value.renderer.segments;
    out_info->renderer_uv_scale = value.renderer.uvScale;
    out_info->renderer_uv_scrolling = value.renderer.uvScrolling ? 1 : 0;
    out_info->renderer_uv_smoothing = value.renderer.uvSmoothing ? 1 : 0;
    out_info->renderer_fade_alpha = value.renderer.fadeAlpha ? 1 : 0;
    out_info->renderer_fade_size = value.renderer.fadeSize ? 1 : 0;
    out_info->texture_count = value.textures.size();
    out_info->constant_count = value.constants.size();
    return 1;
}

extern "C" int we_scene_frame_plan_particle_control_point_info(
    WESceneFramePlanRef plan,
    std::size_t particle_index,
    std::size_t control_point_index,
    WESceneFrameParticleControlPointInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "particle control point information")) {
        return 0;
    }
    if (particle_index >= plan->plan.particles.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame particle index is out of range");
        return 0;
    }
    const auto& control_points =
        plan->plan.particles[particle_index].configuration.controlPoints;
    if (control_point_index >= control_points.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame particle control point index is out of range");
        return 0;
    }
    const particle::ControlPoint& value = control_points[control_point_index];
    *out_info = {
        .id = value.id,
        .position = {value.position.x, value.position.y, value.position.z},
    };
    return 1;
}

extern "C" int we_scene_frame_plan_particle_emitter_info(
    WESceneFramePlanRef plan,
    std::size_t particle_index,
    std::size_t emitter_index,
    WESceneFrameParticleEmitterInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "particle emitter information")) {
        return 0;
    }
    if (particle_index >= plan->plan.particles.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame particle index is out of range");
        return 0;
    }
    const auto& emitters =
        plan->plan.particles[particle_index].configuration.emitters;
    if (emitter_index >= emitters.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame particle emitter index is out of range");
        return 0;
    }
    const particle::EmitterBase& value = std::visit(
        [](const auto& emitter) -> const particle::EmitterBase& {
            return emitter.base;
        },
        emitters[emitter_index]
    );
    *out_info = {
        .control_point = value.controlPoint,
        .flags = value.flags,
        .rate = value.rate,
        .delay = value.delay,
        .duration = value.duration,
        .minimum_periodic_delay = value.minPeriodicDelay,
        .maximum_periodic_delay = value.maxPeriodicDelay,
        .minimum_periodic_duration = value.minPeriodicDuration,
        .maximum_periodic_duration = value.maxPeriodicDuration,
        .maximum_to_emit_per_period = value.maxToEmitPerPeriod,
    };
    return 1;
}

extern "C" int we_scene_frame_plan_operation_info(
    WESceneFramePlanRef plan,
    std::size_t index,
    WESceneFrameOperationInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    const FrameOperation* operation = nullptr;
    if (!operationAt(plan, index, &operation, out_error) ||
        !requireOutput(out_info, out_error, "frame operation information")) {
        return 0;
    }
    *out_info = {};
    out_info->kind = operationKindValue(operationKind(*operation));
    if (const auto* render = std::get_if<FrameRenderPass>(operation)) {
        originInfo(render->origin, *out_info);
        out_info->shader = render->shader.c_str();
        out_info->vertex_shader_path = render->vertexShaderPath.c_str();
        out_info->fragment_shader_path = render->fragmentShaderPath.c_str();
        out_info->blending = blendingMode(render->blending);
        out_info->culling = cullingMode(render->culling);
        out_info->depth_test = depthMode(render->depthTest);
        out_info->depth_write = depthMode(render->depthWrite);
        out_info->geometry = geometryKind(render->geometry);
        out_info->texture_coordinates = texCoordKind(render->textureCoordinates);
        resourceInfo(render->input, out_info->input);
        out_info->has_previous_input = render->previousInput.has_value() ? 1 : 0;
        if (render->previousInput) {
            resourceInfo(*render->previousInput, out_info->previous_input);
        }
        resourceInfo(render->destination, out_info->destination);
        out_info->texture_count = render->textures.size();
        out_info->combo_count = render->combos.size();
        out_info->constant_count = render->constants.size();
        out_info->write_alpha = render->writeAlpha ? 1 : 0;
        out_info->has_light_index = render->lightIndex.has_value() ? 1 : 0;
        if (render->lightIndex) {
            out_info->light_index = *render->lightIndex;
        }
        if (render->matrixOverrides.alternateViewProjection) {
            out_info->has_alternate_view_projection = 1;
            for (std::size_t index = 0; index < 16; ++index) {
                out_info->alternate_view_projection.values[index] =
                    render->matrixOverrides.alternateViewProjection->at(index);
            }
        }
    } else if (const auto* copy = std::get_if<FrameCopyCommand>(operation)) {
        originInfo(copy->origin, *out_info);
        resourceInfo(copy->source, out_info->source);
        resourceInfo(copy->destination, out_info->destination);
    } else if (const auto* swap = std::get_if<FrameSwapCommand>(operation)) {
        originInfo(swap->origin, *out_info);
        resourceInfo(swap->source, out_info->source);
        resourceInfo(swap->destination, out_info->destination);
    } else if (const auto* clear = std::get_if<FrameClearCommand>(operation)) {
        originInfo(clear->origin, *out_info);
        resourceInfo(clear->destination, out_info->destination);
        out_info->clear_red = clear->color.red;
        out_info->clear_green = clear->color.green;
        out_info->clear_blue = clear->color.blue;
        out_info->clear_alpha = clear->color.alpha;
        out_info->clear_depth = clear->clearDepth ? 1 : 0;
    } else if (const auto* text = std::get_if<FrameTextCommand>(operation)) {
        out_info->text_index = text->textIndex;
        out_info->object_id = text->objectId;
        resourceInfo(text->destination, out_info->destination);
    } else {
        const auto& particle = std::get<FrameParticleCommand>(*operation);
        out_info->particle_index = particle.particleIndex;
        out_info->object_id = particle.objectId;
        resourceInfo(particle.destination, out_info->destination);
    }
    return 1;
}

extern "C" int we_scene_frame_plan_texture_binding_info(
    WESceneFramePlanRef plan,
    std::size_t operation_index,
    std::size_t binding_index,
    WESceneFrameTextureBindingInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    const FrameOperation* operation = nullptr;
    if (!operationAt(plan, operation_index, &operation, out_error) ||
        !requireOutput(out_info, out_error, "frame texture binding information")) {
        return 0;
    }
    const auto* render = std::get_if<FrameRenderPass>(operation);
    if (render == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Texture bindings are only available for render operations");
        return 0;
    }
    if (binding_index >= render->textures.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame texture binding index is out of range");
        return 0;
    }
    auto iterator = render->textures.begin();
    std::advance(iterator, static_cast<std::ptrdiff_t>(binding_index));
    *out_info = {};
    out_info->slot = iterator->first;
    out_info->sample_depth = iterator->second.sampleDepth ? 1 : 0;
    if (iterator->second.candidates.empty()) {
        resourceInfo(render->input, out_info->resource);
    } else {
        resourceInfo(
            iterator->second.candidates.back().resource,
            out_info->resource
        );
    }
    return 1;
}

extern "C" int we_scene_frame_plan_combo_info(
    WESceneFramePlanRef plan,
    std::size_t operation_index,
    std::size_t combo_index,
    WESceneFrameComboInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    const FrameOperation* operation = nullptr;
    if (!operationAt(plan, operation_index, &operation, out_error) ||
        !requireOutput(out_info, out_error, "frame combo information")) {
        return 0;
    }
    const auto* render = std::get_if<FrameRenderPass>(operation);
    if (render == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Combos are only available for render operations");
        return 0;
    }
    if (combo_index >= render->combos.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame combo index is out of range");
        return 0;
    }
    auto iterator = render->combos.begin();
    std::advance(iterator, static_cast<std::ptrdiff_t>(combo_index));
    *out_info = {.name = iterator->first.c_str(), .value = iterator->second};
    return 1;
}

extern "C" int we_scene_frame_plan_constant_info(
    WESceneFramePlanRef plan,
    std::size_t operation_index,
    std::size_t constant_index,
    WESceneFrameConstantInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    const FrameOperation* operation = nullptr;
    if (!operationAt(plan, operation_index, &operation, out_error) ||
        !requireOutput(out_info, out_error, "frame constant information")) {
        return 0;
    }
    const auto* render = std::get_if<FrameRenderPass>(operation);
    if (render == nullptr) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
                    "Constants are only available for render operations");
        return 0;
    }
    if (constant_index >= render->constants.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame constant index is out of range");
        return 0;
    }
    auto iterator = render->constants.begin();
    std::advance(iterator, static_cast<std::ptrdiff_t>(constant_index));
    *out_info = {};
    out_info->name = iterator->first.c_str();
    switch (iterator->second.source) {
        case DynamicValueSource::literal:
            out_info->source = WE_SCENE_DYNAMIC_VALUE_LITERAL;
            break;
        case DynamicValueSource::user:
            out_info->source = WE_SCENE_DYNAMIC_VALUE_USER;
            break;
        case DynamicValueSource::scriptInitial:
            out_info->source = WE_SCENE_DYNAMIC_VALUE_SCRIPT_INITIAL;
            break;
        case DynamicValueSource::script:
            out_info->source = WE_SCENE_DYNAMIC_VALUE_SCRIPT;
            break;
        case DynamicValueSource::scriptUnavailable:
            out_info->source = WE_SCENE_DYNAMIC_VALUE_SCRIPT_UNAVAILABLE;
            break;
    }
    valueInfo(iterator->second, out_info->value);
    return 1;
}

extern "C" int we_scene_frame_plan_issue_info(
    WESceneFramePlanRef plan,
    std::size_t issue_index,
    WESceneFramePlanIssueInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "frame plan issue information")) {
        return 0;
    }
    if (issue_index >= plan->plan.issues.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Scene frame plan issue index is out of range");
        return 0;
    }
    const auto& issue = plan->plan.issues[issue_index];
    *out_info = {};
    out_info->code = issueCode(issue.code);
    out_info->severity = issueSeverity(issue.severity);
    out_info->has_object = issue.objectId.has_value() ? 1 : 0;
    out_info->object_id = issue.objectId.value_or(0);
    out_info->asset_path = issue.assetPath.c_str();
    out_info->json_pointer = issue.jsonPointer.c_str();
    out_info->message = issue.message.c_str();
    return 1;
}

extern "C" int we_scene_frame_plan_script_evaluation_info(
    WESceneFramePlanRef plan,
    size_t evaluation_index,
    WESceneScriptEvaluationInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requirePlan(plan, out_error) ||
        !requireOutput(out_info, out_error, "script evaluation information")) {
        return 0;
    }
    if (evaluation_index >= plan->plan.scriptEvaluations.size()) {
        assignError(out_error, WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
                    "Script evaluation index is out of range");
        return 0;
    }
    const auto& evaluation = plan->plan.scriptEvaluations[evaluation_index];
    *out_info = {};
    out_info->json_pointer = evaluation.jsonPointer.c_str();
    out_info->status = evaluation.status ==
            SceneGraph::EvaluationFrame::ScriptEvaluationStatus::success
        ? WE_SCENE_SCRIPT_EVALUATION_SUCCESS
        : WE_SCENE_SCRIPT_EVALUATION_UNAVAILABLE;
    out_info->execution_count = evaluation.executionCount;
    out_info->cache_hit_count = evaluation.cacheHitCount;
    return 1;
}
