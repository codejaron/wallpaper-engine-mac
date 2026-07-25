#ifndef WE_SCENE_GRAPH_SCENE_GRAPH_HPP
#define WE_SCENE_GRAPH_SCENE_GRAPH_HPP

#include <SceneModel/SceneModel.hpp>
#include <SceneScript/SceneScript.hpp>

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
    double pointerX = 0;
    double pointerY = 0;
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
    std::size_t objectIndex = 0;
    int id = 0;
    std::optional<int> parent;
    bool disablePropagation = false;
    EvaluatedValue origin;
    EvaluatedValue scale;
    EvaluatedValue angles;
    EvaluatedValue visible;
    ObjectTransform localTransform;
    ObjectTransform worldTransform;
    bool isVisible = true;
};

struct SceneGraphSnapshot {
    std::uint64_t modelRevision = 0;
    // Frame planning must evaluate every dynamic field against the same
    // property revision as transforms and visibility. Keeping the values in
    // the immutable snapshot prevents a property update between graph and
    // material evaluation from producing a torn frame.
    std::map<std::string, Value> propertyValues;
    std::vector<SceneGraphNodeSnapshot> nodes;
    // Both orders contain indices into nodes/source scene objects. Keeping
    // indices rather than copied objects gives FrameGraph one authoritative
    // path back to SceneModel.
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
        std::shared_ptr<SceneModel> model
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
    explicit SceneGraph(std::shared_ptr<SceneModel> model);

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
        std::string pointer
    );
    [[nodiscard]] std::uint64_t modelRevision() const noexcept;
    [[nodiscard]] const std::map<std::string, Value>& propertyValues() const noexcept;
    [[nodiscard]] const std::map<std::string, EvaluatedValue>&
    evaluatedScriptValues() const noexcept;
    [[nodiscard]] std::vector<ScriptEvaluationStats> scriptEvaluationStats() const;

private:
    friend class SceneGraph;
    struct Impl;
    explicit EvaluationFrame(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace we::scene

#endif
