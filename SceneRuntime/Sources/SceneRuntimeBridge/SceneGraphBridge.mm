#include <SceneRuntimeBridge/SceneRuntimeBridge.h>

#import <Foundation/Foundation.h>

#include "SceneRuntimeBridgeInternal.hpp"

#include <SceneGraph/SceneGraph.hpp>

#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using we::scene::bridge::assignError;
using we::scene::bridge::assignExceptionError;
using we::scene::bridge::assignModelError;
using we::scene::bridge::clearError;
using we::scene::bridge::requireOutput;
using we::scene::script::ScriptLocalStorage;
using we::scene::script::ScriptLocalStorageLocation;

constexpr std::size_t localStorageQuotaBytes = 100 * 1024;
constexpr std::string_view localStoragePrefix =
    "OpenWallpaperEngine.SceneScriptLocalStorage.v1/";

std::mutex& localStorageMutex() {
    static std::mutex mutex;
    return mutex;
}

std::string hexEncoded(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

NSString* foundationString(std::string_view value) {
    NSString* result = [[NSString alloc]
        initWithBytes:value.data()
        length:value.size()
        encoding:NSUTF8StringEncoding
    ];
    if (result == nil) {
        throw std::invalid_argument(
            "SceneScript localStorage identity is not valid UTF-8"
        );
    }
    return result;
}

std::string standardString(NSString* value) {
    if (value == nil) return {};
    const char* utf8 = value.UTF8String;
    if (utf8 == nullptr) {
        throw std::runtime_error(
            "SceneScript localStorage contains invalid UTF-8"
        );
    }
    return std::string(utf8);
}

class UserDefaultsScriptLocalStorage final : public ScriptLocalStorage {
public:
    UserDefaultsScriptLocalStorage(
        std::string wallpaperIdentity,
        std::string screenIdentity
    ) : wallpaperPrefix_(
            std::string(localStoragePrefix) +
            hexEncoded(wallpaperIdentity) + "/"
        ),
        screenIdentity_(hexEncoded(screenIdentity)) {
        if (wallpaperIdentity.empty() || screenIdentity.empty()) {
            throw std::invalid_argument(
                "SceneScript localStorage identities must not be empty"
            );
        }
    }

    std::optional<std::string> get(
        std::string_view key,
        ScriptLocalStorageLocation location
    ) override {
        std::lock_guard lock(localStorageMutex());
        @autoreleasepool {
            NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
            NSString* storageKey = foundationString(entryKey(key, location));
            id stored = [defaults objectForKey:storageKey];
            if (stored == nil) return std::nullopt;
            if (![stored isKindOfClass:NSString.class]) {
                throw std::runtime_error(
                    "SceneScript localStorage contains a non-string payload"
                );
            }
            return standardString(static_cast<NSString*>(stored));
        }
    }

    void set(
        std::string_view key,
        std::string_view jsonValue,
        ScriptLocalStorageLocation location
    ) override {
        std::lock_guard lock(localStorageMutex());
        @autoreleasepool {
            NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
            const std::string encodedKey = entryKey(key, location);
            NSString* storageKey = foundationString(encodedKey);
            std::size_t size = wallpaperStorageSize(
                defaults,
                encodedKey
            );
            if (key.size() > std::numeric_limits<std::size_t>::max() - size ||
                jsonValue.size() >
                    std::numeric_limits<std::size_t>::max() - size - key.size()) {
                throw std::length_error(
                    "SceneScript localStorage exceeds its 100 KiB wallpaper quota"
                );
            }
            size += key.size() + jsonValue.size();
            if (size > localStorageQuotaBytes) {
                throw std::length_error(
                    "SceneScript localStorage exceeds its 100 KiB wallpaper quota"
                );
            }
            [defaults setObject:foundationString(jsonValue) forKey:storageKey];
            persist(defaults);
        }
    }

    bool erase(
        std::string_view key,
        ScriptLocalStorageLocation location
    ) override {
        std::lock_guard lock(localStorageMutex());
        @autoreleasepool {
            NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
            NSString* storageKey = foundationString(entryKey(key, location));
            if ([defaults objectForKey:storageKey] == nil) return false;
            [defaults removeObjectForKey:storageKey];
            persist(defaults);
            return true;
        }
    }

    void clear(ScriptLocalStorageLocation location) override {
        std::lock_guard lock(localStorageMutex());
        @autoreleasepool {
            NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
            const std::string prefix = scopePrefix(location);
            NSString* foundationPrefix = foundationString(prefix);
            NSDictionary<NSString*, id>* values = defaults.dictionaryRepresentation;
            bool changed = false;
            for (NSString* key in values) {
                if (![key hasPrefix:foundationPrefix]) continue;
                [defaults removeObjectForKey:key];
                changed = true;
            }
            if (changed) persist(defaults);
        }
    }

private:
    std::string scopePrefix(ScriptLocalStorageLocation location) const {
        if (location == ScriptLocalStorageLocation::global) {
            return wallpaperPrefix_ + "global/";
        }
        return wallpaperPrefix_ + "screen/" + screenIdentity_ + "/";
    }

    std::string entryKey(
        std::string_view key,
        ScriptLocalStorageLocation location
    ) const {
        return scopePrefix(location) + hexEncoded(key);
    }

    std::size_t wallpaperStorageSize(
        NSUserDefaults* defaults,
        const std::string& replacedKey
    ) const {
        NSDictionary<NSString*, id>* values = defaults.dictionaryRepresentation;
        NSString* foundationPrefix = foundationString(wallpaperPrefix_);
        NSString* foundationReplacedKey = foundationString(replacedKey);
        std::size_t result = 0;
        for (NSString* key in values) {
            if (![key hasPrefix:foundationPrefix] ||
                [key isEqualToString:foundationReplacedKey]) {
                continue;
            }
            id stored = values[key];
            if (![stored isKindOfClass:NSString.class]) {
                throw std::runtime_error(
                    "SceneScript localStorage contains a non-string payload"
                );
            }
            const std::string encodedEntry = standardString(key);
            const std::size_t separator = encodedEntry.rfind('/');
            if (separator == std::string::npos ||
                (encodedEntry.size() - separator - 1) % 2 != 0) {
                throw std::runtime_error(
                    "SceneScript localStorage contains an invalid key"
                );
            }
            const std::size_t decodedKeySize =
                (encodedEntry.size() - separator - 1) / 2;
            const std::size_t valueSize = standardString(
                static_cast<NSString*>(stored)
            ).size();
            if (decodedKeySize >
                    std::numeric_limits<std::size_t>::max() - result ||
                valueSize > std::numeric_limits<std::size_t>::max() -
                    result - decodedKeySize) {
                throw std::length_error(
                    "SceneScript localStorage size overflow"
                );
            }
            result += decodedKeySize + valueSize;
        }
        return result;
    }

    static void persist(NSUserDefaults* defaults) {
        if (![defaults synchronize]) {
            throw std::runtime_error(
                "SceneScript localStorage could not be persisted"
            );
        }
    }

    std::string wallpaperPrefix_;
    std::string screenIdentity_;
};

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

WESceneGraphRef createGraph(
    WESceneModelRef model,
    std::shared_ptr<ScriptLocalStorage> localStorage,
    WESceneRuntimeErrorRef* outError
) noexcept {
    try {
        auto handle = std::make_unique<WESceneGraph>();
        handle->graph = we::scene::SceneGraph::create(
            model->model,
            std::move(localStorage)
        );
        return handle.release();
    } catch (const we::scene::SceneModelError& error) {
        assignModelError(outError, error);
    } catch (const std::exception& error) {
        assignExceptionError(outError, "creating the scene graph", error.what());
    } catch (...) {
        assignExceptionError(outError, "creating the scene graph", nullptr);
    }
    return nullptr;
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
    return createGraph(model, nullptr, out_error);
}

extern "C" WESceneGraphRef we_scene_model_graph_create_with_local_storage(
    WESceneModelRef model,
    const WESceneLocalStorageConfiguration* configuration,
    WESceneRuntimeErrorRef* out_error
) {
    clearError(out_error);
    if (!requireModel(model, out_error)) return nullptr;
    if (configuration == nullptr ||
        configuration->wallpaper_identity == nullptr ||
        configuration->screen_identity == nullptr ||
        configuration->wallpaper_identity[0] == '\0' ||
        configuration->screen_identity[0] == '\0') {
        assignError(
            out_error,
            WE_SCENE_RUNTIME_ERROR_INVALID_ARGUMENT,
            "SceneScript localStorage wallpaper and screen identities are required"
        );
        return nullptr;
    }
    try {
        auto storage = std::make_shared<UserDefaultsScriptLocalStorage>(
            configuration->wallpaper_identity,
            configuration->screen_identity
        );
        return createGraph(model, std::move(storage), out_error);
    } catch (const std::exception& error) {
        assignExceptionError(
            out_error,
            "configuring SceneScript localStorage",
            error.what()
        );
    } catch (...) {
        assignExceptionError(
            out_error,
            "configuring SceneScript localStorage",
            nullptr
        );
    }
    return nullptr;
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
