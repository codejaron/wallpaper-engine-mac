#include <SceneRuntimeBridge/SceneRuntimeBridge.h>

#include "SceneRuntimeBridgeInternal.hpp"

#include <SceneGraph/SceneGraph.hpp>

#include <exception>
#include <memory>
#include <vector>

namespace {

using we::scene::bridge::assignError;
using we::scene::bridge::assignExceptionError;
using we::scene::bridge::assignModelError;
using we::scene::bridge::clearError;
using we::scene::bridge::requireOutput;

bool requireModel(
    WESceneModelRef model,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (model != nullptr && model->model) {
        return true;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Scene model is required"
    );
    return false;
}

bool requireGraph(
    WESceneGraphRef graph,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (graph != nullptr && graph->graph) {
        return true;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Scene graph is required"
    );
    return false;
}

bool requireGraphSnapshot(
    WESceneGraphSnapshotRef snapshot,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (snapshot != nullptr) {
        return true;
    }
    assignError(
        outError,
        WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
        "Scene graph snapshot is required"
    );
    return false;
}

WESceneDynamicValueSource dynamicValueSource(
    we::scene::DynamicValueSource source
) noexcept {
    using Source = we::scene::DynamicValueSource;
    switch (source) {
        case Source::literal:
            return WE_SCENE_DYNAMIC_VALUE_LITERAL;
        case Source::user:
            return WE_SCENE_DYNAMIC_VALUE_USER;
        case Source::scriptInitial:
            return WE_SCENE_DYNAMIC_VALUE_SCRIPT_INITIAL;
        case Source::script:
            return WE_SCENE_DYNAMIC_VALUE_SCRIPT;
        case Source::scriptUnavailable:
            return WE_SCENE_DYNAMIC_VALUE_SCRIPT_UNAVAILABLE;
    }
    return WE_SCENE_DYNAMIC_VALUE_LITERAL;
}

WESceneVector3 sceneVector(const we::scene::Vector3& value) noexcept {
    return {.x = value.x, .y = value.y, .z = value.z};
}

WESceneObjectTransform sceneTransform(
    const we::scene::ObjectTransform& value
) noexcept {
    return {
        .origin = sceneVector(value.origin),
        .scale = sceneVector(value.scale),
        .angles = sceneVector(value.angles),
    };
}

int graphOrderObjectId(
    WESceneGraphSnapshotRef snapshot,
    const std::vector<std::size_t>& order,
    std::size_t orderIndex,
    std::int32_t* outObjectId,
    WESceneRuntimeErrorRef* outError
) noexcept {
    if (!requireGraphSnapshot(snapshot, outError) ||
        !requireOutput(outObjectId, outError, "graph order object id")) {
        return 0;
    }
    if (orderIndex >= order.size()) {
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Scene graph order index is out of range"
        );
        return 0;
    }
    const std::size_t nodeIndex = order[orderIndex];
    if (nodeIndex >= snapshot->snapshot.nodes.size()) {
        assignError(
            outError,
            WE_SCENE_RUNTIME_ERROR_INTERNAL_FAILURE,
            "Scene graph order contains an invalid node index"
        );
        return 0;
    }
    *outObjectId = snapshot->snapshot.nodes[nodeIndex].id;
    return 1;
}

}  // namespace

extern "C" WESceneGraphRef we_scene_model_graph_create(
    WESceneModelRef model,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error)) {
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneGraph>();
        handle->graph = we::scene::SceneGraph::create(model->model);
        return handle.release();
    } catch (const we::scene::SceneModelError& error) {
        assignModelError(out_error, error);
        return nullptr;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating the scene graph", error.what());
        return nullptr;
    } catch (...) {
        assignExceptionError(out_error, "creating the scene graph", nullptr);
        return nullptr;
    }
}

extern "C" void we_scene_graph_destroy(WESceneGraphRef graph) {
    delete graph;
}

extern "C" WESceneGraphSnapshotRef we_scene_graph_snapshot_create(
    WESceneGraphRef graph,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireGraph(graph, out_error)) {
        return nullptr;
    }
    try {
        auto handle = std::make_unique<WESceneGraphSnapshot>();
        handle->snapshot = graph->graph->snapshot();
        return handle.release();
    } catch (const we::scene::SceneModelError& error) {
        assignModelError(out_error, error);
        return nullptr;
    } catch (const std::exception& error) {
        assignExceptionError(out_error, "creating a scene graph snapshot", error.what());
        return nullptr;
    } catch (...) {
        assignExceptionError(out_error, "creating a scene graph snapshot", nullptr);
        return nullptr;
    }
}

extern "C" void we_scene_graph_snapshot_destroy(
    WESceneGraphSnapshotRef snapshot
) {
    delete snapshot;
}

extern "C" int we_scene_graph_snapshot_revision(
    WESceneGraphSnapshotRef snapshot,
    uint64_t* out_revision,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireGraphSnapshot(snapshot, out_error) ||
        !requireOutput(out_revision, out_error, "graph snapshot revision")) {
        return 0;
    }
    *out_revision = snapshot->snapshot.modelRevision;
    return 1;
}

extern "C" int we_scene_graph_snapshot_node_count(
    WESceneGraphSnapshotRef snapshot,
    size_t* out_count,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireGraphSnapshot(snapshot, out_error) ||
        !requireOutput(out_count, out_error, "graph snapshot node count")) {
        return 0;
    }
    *out_count = snapshot->snapshot.nodes.size();
    return 1;
}

extern "C" int we_scene_graph_snapshot_node_info(
    WESceneGraphSnapshotRef snapshot,
    size_t index,
    WESceneGraphNodeInfo* out_info,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireGraphSnapshot(snapshot, out_error) ||
        !requireOutput(out_info, out_error, "graph node information")) {
        return 0;
    }
    if (index >= snapshot->snapshot.nodes.size()) {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INDEX_OUT_OF_RANGE,
            "Scene graph node index is out of range"
        );
        return 0;
    }
    const auto& node = snapshot->snapshot.nodes[index];
    *out_info = {};
    out_info->object_index = node.objectIndex;
    out_info->id = node.id;
    out_info->has_parent = node.parent.has_value() ? 1 : 0;
    out_info->parent_id = node.parent.value_or(0);
    out_info->disable_propagation = node.disablePropagation ? 1 : 0;
    out_info->visible = node.isVisible ? 1 : 0;
    out_info->origin_source = dynamicValueSource(node.origin.source);
    out_info->scale_source = dynamicValueSource(node.scale.source);
    out_info->angles_source = dynamicValueSource(node.angles.source);
    out_info->visible_source = dynamicValueSource(node.visible.source);
    out_info->local_transform = sceneTransform(node.localTransform);
    out_info->world_transform = sceneTransform(node.worldTransform);
    return 1;
}

extern "C" int we_scene_graph_snapshot_initialization_object_id(
    WESceneGraphSnapshotRef snapshot,
    size_t order_index,
    int32_t* out_object_id,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireGraphSnapshot(snapshot, out_error)) {
        return 0;
    }
    return graphOrderObjectId(
        snapshot,
        snapshot->snapshot.initializationOrder,
        order_index,
        out_object_id,
        out_error
    );
}

extern "C" int we_scene_graph_snapshot_render_object_id(
    WESceneGraphSnapshotRef snapshot,
    size_t order_index,
    int32_t* out_object_id,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireGraphSnapshot(snapshot, out_error)) {
        return 0;
    }
    return graphOrderObjectId(
        snapshot,
        snapshot->snapshot.renderOrder,
        order_index,
        out_object_id,
        out_error
    );
}
