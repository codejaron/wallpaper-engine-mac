#ifndef WE_SCENE_GRAPH_SCENE_GRAPH_HPP
#define WE_SCENE_GRAPH_SCENE_GRAPH_HPP

#include <SceneCore/AudioSpectrum.hpp>
#include <SceneModel/SceneModel.hpp>
#include <SceneScript/SceneScript.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace we::scene {

enum class DynamicValueSource {
    literal,
    user,
    scriptInitial,
    script,
    scriptUnavailable,
};

struct SceneFrameInputs final {
    double runtimeSeconds = 0;
    double frameTimeSeconds = 0;
    std::optional<double> timeOfDay;
    std::optional<bool> isScreensaver;
    std::optional<AudioSpectrumFrame> audioSpectrum;
    // Filled by SceneFrameGraph from its authoritative logical projection.
    // Standalone SceneGraph callers may supply it when their scripts consume
    // engine.canvasSize; the graph never invents a drawable-size fallback.
    std::optional<std::array<double, 2>> canvasSize;
    // Optional host override. When absent, EvaluationFrame derives the
    // snapshot from the model's coherent DynamicValue state before invoking
    // scripts. Keeping this on the graph input also lets an embedding host
    // provide a newer scene snapshot without creating a second state store.
    std::optional<script::ScriptSceneSnapshot> sceneSnapshot;
    double pointerX = 0;
    double pointerY = 0;
    // Host-sampled desktop pointer state is separate from normalized scene
    // coordinates. It lets the script runtime distinguish a cursor outside
    // this wallpaper window from a cursor at the edge of the drawable.
    bool pointerActive = false;
    bool pointerLeftDown = false;
    // Filled by SceneFrameGraph once the authoritative projection size is
    // known. SceneGraph itself deliberately does not guess automatic canvas
    // dimensions.
    std::optional<std::array<double, 3>> cursorWorldPosition;
    std::vector<script::ScriptCursorEvent> cursorEvents;
    std::optional<script::ScriptMediaSnapshot> mediaSnapshot;
    std::vector<script::ScriptSoundRuntimeSnapshot> soundRuntimeStates;
};

struct EvaluatedValue {
    RuntimeValue value;
    DynamicValueSource source = DynamicValueSource::literal;
};

struct Vector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    [[nodiscard]] friend bool operator==(
        const Vector3& lhs,
        const Vector3& rhs
    ) = default;
};

struct ObjectTransform {
    Vector3 origin;
    Vector3 scale{1.0, 1.0, 1.0};
    Vector3 angles;
};

struct SceneGraphNodeSnapshot {
    // Immutable resource data lives on the SceneModel object at this index.
    // Multiple runtime instances may reference the same source object.
    std::size_t objectIndex = 0;
    int id = 0;
    bool dynamic = false;
    std::optional<int> parent;
    bool disablePropagation = false;
    EvaluatedValue origin;
    EvaluatedValue scale;
    EvaluatedValue angles;
    EvaluatedValue visible;
    ObjectTransform localTransform;
    ObjectTransform worldTransform;
    bool isVisible = true;
    std::map<std::string, RuntimeValue> layerProperties;
};

struct SceneGraphSnapshot {
    std::uint64_t modelRevision = 0;
    // Frame planning must evaluate every dynamic field against the same
    // property revision as transforms and visibility. Keeping the values in
    // the immutable snapshot prevents a property update between graph and
    // material evaluation from producing a torn frame.
    std::map<std::string, Value> propertyValues;
    std::vector<SceneGraphNodeSnapshot> nodes;
    // Behavior state is copied only after all scripts participating in the
    // frame have run. Downstream planning consumes this immutable snapshot;
    // it never reaches back into QuickJS or a mutable registry.
    std::vector<script::ScriptTextureAnimationSnapshot> textureAnimations;
    std::vector<script::ScriptSoundSnapshot> sounds;
    // Both orders contain indices into nodes. Each node then selects its
    // immutable source object through objectIndex.
    std::vector<std::size_t> initializationOrder;
    std::vector<std::size_t> renderOrder;

    [[nodiscard]] const SceneGraphNodeSnapshot* node(int id) const noexcept;
};

// Shared by SceneGraph and downstream frame planning so DynamicValue source
// and user-binding semantics have one implementation.
[[nodiscard]] EvaluatedValue evaluateDynamicValue(
    const SceneModel& model,
    const DynamicValue& dynamic,
    const std::map<std::string, Value>& properties,
    std::string pointer
);

// Retains SceneModel and its Runtime. Topology is immutable; every snapshot
// evaluates the latest coherent user-property state and remains unchanged if
// the model is subsequently updated.
class SceneGraph final {
public:
    [[nodiscard]] static std::shared_ptr<SceneGraph> create(
        std::shared_ptr<SceneModel> model,
        std::shared_ptr<script::ScriptLocalStorage> localStorage = nullptr
    );

    SceneGraph(const SceneGraph&) = delete;
    SceneGraph& operator=(const SceneGraph&) = delete;
    ~SceneGraph();

    [[nodiscard]] SceneGraphSnapshot snapshot() const;
    class EvaluationFrame;
    [[nodiscard]] std::unique_ptr<EvaluationFrame> evaluationFrame(
        const SceneFrameInputs& inputs
    );
    [[nodiscard]] SceneGraphSnapshot snapshot(EvaluationFrame& frame) const;
    [[nodiscard]] std::shared_ptr<const SceneModel> model() const noexcept;

private:
    explicit SceneGraph(
        std::shared_ptr<SceneModel> model,
        std::shared_ptr<script::ScriptLocalStorage> localStorage
    );

    std::shared_ptr<SceneModel> model_;
    std::map<int, std::size_t> objectIndices_;
    std::vector<std::size_t> initializationOrder_;
    std::vector<std::size_t> renderOrder_;
    struct ScriptState;
    std::unique_ptr<ScriptState> scriptState_;
};

class SceneGraph::EvaluationFrame final {
public:
    enum class ScriptEvaluationStatus { success, unavailable };
    struct ScriptEvaluationStats {
        std::string jsonPointer;
        ScriptEvaluationStatus status = ScriptEvaluationStatus::success;
        std::size_t executionCount = 0;
        std::size_t cacheHitCount = 0;
    };

    ~EvaluationFrame();
    EvaluationFrame(const EvaluationFrame&) = delete;
    EvaluationFrame& operator=(const EvaluationFrame&) = delete;

    [[nodiscard]] EvaluatedValue evaluate(
        const DynamicValue& dynamic,
        std::string pointer,
        script::ScriptPropertyOwner owner = {}
    );
    void initialize(
        const DynamicValue& dynamic,
        std::string pointer,
        script::ScriptPropertyOwner owner = {}
    );
    void registerScriptPropertyObject(
        script::ScriptPropertyObjectDescriptor descriptor
    );
    [[nodiscard]] bool scriptPropertyObjectsCurrent() const;
    void commitScriptPropertyObjects();
    [[nodiscard]] std::uint64_t modelRevision() const noexcept;
    [[nodiscard]] const std::map<std::string, Value>& propertyValues() const noexcept;
    [[nodiscard]] const std::map<std::string, EvaluatedValue>&
    evaluatedScriptValues() const noexcept;
    [[nodiscard]] std::vector<ScriptEvaluationStats> scriptEvaluationStats() const;
    [[nodiscard]] std::vector<script::ScriptTextureAnimationSnapshot>
    textureAnimationSnapshots() const;
    [[nodiscard]] std::vector<script::ScriptSoundSnapshot> soundSnapshots() const;
    [[nodiscard]] std::vector<int> cursorInteractiveLayerIds() const;
    [[nodiscard]] std::optional<RuntimeValue> layerProperty(
        int layerId,
        std::string_view property
    ) const;

private:
    friend class SceneGraph;
    struct Impl;
    explicit EvaluationFrame(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace we::scene

#endif
