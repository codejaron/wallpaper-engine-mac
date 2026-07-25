#include <SceneGraph/SceneGraph.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <set>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace we::scene {
namespace {

enum class VisitState : std::uint8_t { unvisited, visiting, visited };

std::string objectPointer(std::size_t index, std::string_view field = {}) {
    std::string result = "/objects/" + std::to_string(index);
    if (!field.empty()) {
        result += '/';
        result += field;
    }
    return result;
}

[[noreturn]] void graphError(
    const SceneModel& model,
    SceneModelErrorCode code,
    std::string pointer,
    std::vector<std::string> chain,
    std::string message
) {
    throw SceneModelError(
        code,
        model.project().scene.assetPath,
        std::move(pointer),
        std::move(chain),
        std::move(message)
    );
}

std::vector<std::string> cycleChain(
    const std::vector<std::size_t>& stack,
    std::size_t repeated,
    const std::vector<SceneObject>& objects
) {
    const auto start = std::find(stack.begin(), stack.end(), repeated);
    std::vector<std::string> result;
    const auto first = start == stack.end() ? stack.begin() : start;
    result.reserve(static_cast<std::size_t>(stack.end() - first) + 1);
    for (auto iterator = first; iterator != stack.end(); ++iterator) {
        result.push_back("object " + std::to_string(objects[*iterator].base.id));
    }
    result.push_back("object " + std::to_string(objects[repeated].base.id));
    return result;
}

std::vector<std::size_t> buildOrder(
    const SceneModel& model,
    const std::map<int, std::size_t>& indices,
    bool includeParents
) {
    const auto& objects = model.project().scene.objects;
    std::vector<VisitState> states(objects.size(), VisitState::unvisited);
    std::vector<std::size_t> stack;
    std::vector<std::size_t> order;
    order.reserve(objects.size());

    struct Frame {
        std::size_t index = 0;
        std::size_t nextDependency = 0;
        bool parentProcessed = false;
    };
    std::vector<Frame> frames;
    frames.reserve(objects.size());

    const auto enter = [&](std::size_t index, std::string pointer) {
        if (states[index] == VisitState::visiting) {
            graphError(
                model,
                SceneModelErrorCode::referenceCycle,
                std::move(pointer),
                cycleChain(stack, index, objects),
                includeParents
                    ? "Scene object initialization cycle detected"
                    : "Scene object dependency cycle detected"
            );
        }
        if (states[index] == VisitState::unvisited) {
            states[index] = VisitState::visiting;
            stack.push_back(index);
            frames.push_back({.index = index});
        }
    };

    for (std::size_t index = 0; index < objects.size(); ++index) {
        if (states[index] != VisitState::unvisited) {
            continue;
        }
        enter(index, objectPointer(index));
        while (!frames.empty()) {
            Frame& frame = frames.back();
            const ObjectBase& object = objects[frame.index].base;
            bool descended = false;

            while (frame.nextDependency < object.dependencies.size()) {
                const std::size_t dependencyIndex = frame.nextDependency++;
                const int dependencyId = object.dependencies[dependencyIndex].id;
                // Wallpaper Engine scene files can contain a self dependency;
                // the upstream runtime explicitly treats it as a no-op.
                if (dependencyId == object.id) {
                    continue;
                }
                const std::string pointer =
                    objectPointer(frame.index, "dependencies") + '/' +
                    std::to_string(dependencyIndex);
                const auto dependency = indices.find(dependencyId);
                if (dependency == indices.end()) {
                    graphError(
                        model,
                        SceneModelErrorCode::danglingReference,
                        pointer,
                        {"object " + std::to_string(object.id)},
                        "Object dependency references unknown id " +
                            std::to_string(dependencyId)
                    );
                }
                if (states[dependency->second] == VisitState::visited) {
                    continue;
                }
                enter(dependency->second, pointer);
                descended = true;
                break;
            }
            if (descended) {
                continue;
            }

            if (includeParents && !frame.parentProcessed) {
                frame.parentProcessed = true;
                if (object.parent) {
                    const std::string pointer = objectPointer(
                        frame.index,
                        "parent"
                    );
                    const auto parent = indices.find(*object.parent);
                    if (parent == indices.end()) {
                        graphError(
                            model,
                            SceneModelErrorCode::danglingReference,
                            pointer,
                            {"object " + std::to_string(object.id)},
                            "Object parent references unknown id " +
                                std::to_string(*object.parent)
                        );
                    }
                    if (states[parent->second] != VisitState::visited) {
                        enter(parent->second, pointer);
                        continue;
                    }
                }
            }

            states[frame.index] = VisitState::visited;
            order.push_back(frame.index);
            frames.pop_back();
            stack.pop_back();
        }
    }
    return order;
}

Vector3 vector3Value(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer,
    std::string_view description
) {
    const auto& projected = evaluated.value.vector();
    const Vector3 result{
        projected[0], projected[1], projected[2],
    };
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) {
        graphError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            {},
            std::string(description) +
                " must project to three finite numbers"
        );
    }
    return result;
}

bool booleanValue(
    const SceneModel& model,
    const EvaluatedValue& evaluated,
    std::string pointer
) {
    (void)model;
    (void)pointer;
    return evaluated.value.boolean();
}

Vector3 multiply(const Vector3& lhs, const Vector3& rhs) {
    return {lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z};
}

Vector3 add(const Vector3& lhs, const Vector3& rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

ObjectTransform combine(
    const ObjectTransform& parent,
    const ObjectTransform& local
) {
    const Vector3 scaled = multiply(local.origin, parent.scale);
    const double cosine = std::cos(parent.angles.z);
    const double sine = std::sin(parent.angles.z);
    const Vector3 rotated{
        scaled.x * cosine - scaled.y * sine,
        scaled.x * sine + scaled.y * cosine,
        scaled.z,
    };
    return {
        .origin = add(parent.origin, rotated),
        .scale = multiply(local.scale, parent.scale),
        .angles = add(local.angles, parent.angles),
    };
}

}  // namespace

struct SceneGraph::ScriptState final {
    struct Instance final {
        std::unique_ptr<script::ScriptInstance> script;
        std::optional<RuntimeValue> connectedUserValue;
    };

    script::ScriptRuntime runtime;
    std::map<std::string, Instance> instances;
    std::recursive_mutex mutex;
};

struct SceneGraph::EvaluationFrame::Impl final {
    Impl(SceneGraph& owner, SceneFrameInputs frameInputs)
        : graph(owner), frameLock(owner.scriptState_->mutex), inputs(frameInputs),
          properties(owner.model_->propertyState()) {
        if (!std::isfinite(inputs.runtimeSeconds) || inputs.runtimeSeconds < 0 ||
            !std::isfinite(inputs.frameTimeSeconds) || inputs.frameTimeSeconds < 0 ||
            !std::isfinite(inputs.pointerX) || !std::isfinite(inputs.pointerY)) {
            graphError(
                *graph.model_, SceneModelErrorCode::invalidValue, "/frameInputs", {},
                "Scene frame inputs must be finite and time values must be non-negative"
            );
        }
    }

    EvaluatedValue evaluate(
        const DynamicValue& dynamic,
        const std::string& pointer
    ) {
        const EvaluatedValue connected = evaluateDynamicValue(
            *graph.model_, dynamic, properties.values, pointer
        );
        if (!dynamic.script) {
            return connected;
        }
        if (const auto found = values.find(pointer); found != values.end()) {
            ++scriptStats.at(pointer).cacheHitCount;
            return found->second;
        }
        if (!evaluating.emplace(pointer).second) {
            graphError(
                *graph.model_, SceneModelErrorCode::referenceCycle, pointer, {},
                "Dynamic script properties contain a recursive evaluation cycle"
            );
        }
        struct EraseGuard final {
            std::set<std::string>& set;
            const std::string& key;
            ~EraseGuard() { set.erase(key); }
        } guard{evaluating, pointer};

        std::map<std::string, RuntimeValue> scriptProperties;
        for (const auto& [name, child] : dynamic.scriptProperties) {
            scriptProperties.emplace(
                name,
                evaluate(child, pointer + "/scriptproperties/" + name).value
            );
        }

        auto& instance = graph.scriptState_->instances[pointer];
        auto& stats = scriptStats[pointer];
        stats.jsonPointer = pointer;
        ++stats.executionCount;
        try {
            if (!instance.script) {
                instance.script = graph.scriptState_->runtime.createInstance(
                    *dynamic.script,
                    connected.value,
                    scriptProperties,
                    dynamic.user ? dynamic.user->condition : std::nullopt
                );
                if (dynamic.user) {
                    instance.connectedUserValue = connected.value;
                }
            } else {
                if (dynamic.user &&
                    (!instance.connectedUserValue ||
                     *instance.connectedUserValue != connected.value)) {
                    instance.script->updateCurrent(connected.value);
                    instance.connectedUserValue = connected.value;
                }
                instance.script->updateProperties(std::move(scriptProperties));
            }
            EvaluatedValue result{
                .value = instance.script->evaluate({
                    .runtimeSeconds = inputs.runtimeSeconds,
                    .frameTimeSeconds = inputs.frameTimeSeconds,
                    .pointerX = inputs.pointerX,
                    .pointerY = inputs.pointerY,
                }),
                .source = DynamicValueSource::script,
            };
            values.emplace(pointer, result);
            return result;
        } catch (const script::ScriptError& error) {
            if (error.code() == script::ScriptErrorCode::audioInputUnavailable) {
                EvaluatedValue unavailable{
                    .value = instance.script->currentValue(),
                    .source = DynamicValueSource::scriptUnavailable,
                };
                stats.status = EvaluationFrame::ScriptEvaluationStatus::unavailable;
                values.emplace(pointer, unavailable);
                return unavailable;
            }
            graphError(
                *graph.model_, SceneModelErrorCode::invalidValue, pointer, {},
                std::string("Dynamic script failed: ") + error.what()
            );
        }
    }

    SceneGraph& graph;
    // A QuickJS instance is stateful. Keep every scripted value in one frame
    // serialized as a unit so concurrent snapshots cannot interleave updates.
    std::unique_lock<std::recursive_mutex> frameLock;
    SceneFrameInputs inputs;
    PropertyStateSnapshot properties;
    std::map<std::string, EvaluatedValue> values;
    std::map<std::string, EvaluationFrame::ScriptEvaluationStats> scriptStats;
    std::set<std::string> evaluating;
};

EvaluatedValue evaluateDynamicValue(
    const SceneModel& model,
    const DynamicValue& dynamic,
    const std::map<std::string, Value>& properties,
    std::string pointer
) {
    if (!dynamic.user) {
        return {
            .value = dynamic.value,
            .source = dynamic.script
                ? DynamicValueSource::scriptInitial
                : DynamicValueSource::literal,
        };
    }

    const auto property = properties.find(dynamic.user->property);
    if (property == properties.end()) {
        graphError(
            model,
            SceneModelErrorCode::danglingReference,
            std::move(pointer),
            {dynamic.user->property},
            "User binding has no current value for property '" +
                dynamic.user->property + "'"
        );
    }

    RuntimeValue value;
    try {
        const auto definition = model.project().properties.find(
            dynamic.user->property
        );
        const bool color = definition != model.project().properties.end() &&
            definition->second.type == PropertyType::color;
        if (color) {
            const auto* source = std::get_if<std::string>(&property->second.storage);
            value = source == nullptr
                ? RuntimeValue::fromValue(property->second)
                : RuntimeValue::colorString(*source);
        } else {
            value = RuntimeValue::fromValue(property->second);
        }
    } catch (const std::exception& error) {
        graphError(
            model,
            SceneModelErrorCode::invalidValue,
            std::move(pointer),
            {dynamic.user->property},
            "User property '" + dynamic.user->property +
                "' cannot update its DynamicValue: " + error.what()
        );
    }
    if (dynamic.user->condition) {
        // Upstream applies a condition only when the connected property is a
        // string. Other property types propagate their value unchanged.
        if (value.type() == RuntimeValueType::string) {
            value = RuntimeValue::condition(
                value.string(),
                *dynamic.user->condition
            );
        }
    }
    return {
        .value = std::move(value),
        // A connected property replaces the script's current input; it does
        // not turn a scripted DynamicValue into a pure user value. Static
        // snapshots must still report that QuickJS has not run yet.
        .source = dynamic.script
            ? DynamicValueSource::scriptInitial
            : DynamicValueSource::user,
    };
}

const SceneGraphNodeSnapshot* SceneGraphSnapshot::node(int id) const noexcept {
    const auto found = std::find_if(
        nodes.begin(),
        nodes.end(),
        [id](const SceneGraphNodeSnapshot& node) { return node.id == id; }
    );
    return found == nodes.end() ? nullptr : &*found;
}

std::shared_ptr<SceneGraph> SceneGraph::create(
    std::shared_ptr<SceneModel> model
) {
    return std::shared_ptr<SceneGraph>(new SceneGraph(std::move(model)));
}

SceneGraph::SceneGraph(std::shared_ptr<SceneModel> model)
    : model_(std::move(model)), scriptState_(std::make_unique<ScriptState>()) {
    if (!model_) {
        throw SceneModelError(
            SceneModelErrorCode::invalidValue,
            {},
            {},
            {},
            "Scene model is required to create a scene graph"
        );
    }
    const auto& objects = model_->project().scene.objects;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        objectIndices_.emplace(objects[index].base.id, index);
    }
    initializationOrder_ = buildOrder(*model_, objectIndices_, true);
    renderOrder_ = buildOrder(*model_, objectIndices_, false);
}

SceneGraph::~SceneGraph() = default;

SceneGraphSnapshot SceneGraph::snapshot() const {
    PropertyStateSnapshot propertyState = model_->propertyState();
    const auto& objects = model_->project().scene.objects;

    SceneGraphSnapshot result;
    result.modelRevision = propertyState.revision;
    result.propertyValues = std::move(propertyState.values);
    result.initializationOrder = initializationOrder_;
    result.renderOrder = renderOrder_;
    result.nodes.reserve(objects.size());

    const auto& snapshotProperties = result.propertyValues;

    for (std::size_t index = 0; index < objects.size(); ++index) {
        const ObjectBase& object = objects[index].base;
        SceneGraphNodeSnapshot node;
        node.objectIndex = index;
        node.id = object.id;
        node.parent = object.parent;
        node.disablePropagation = object.disablePropagation;
        node.origin = evaluateDynamicValue(
            *model_, object.origin, snapshotProperties,
            objectPointer(index, "origin")
        );
        node.scale = evaluateDynamicValue(
            *model_, object.scale, snapshotProperties,
            objectPointer(index, "scale")
        );
        node.angles = evaluateDynamicValue(
            *model_, object.angles, snapshotProperties,
            objectPointer(index, "angles")
        );
        node.visible = evaluateDynamicValue(
            *model_, object.visible, snapshotProperties,
            objectPointer(index, "visible")
        );
        node.localTransform = {
            .origin = vector3Value(
                *model_, node.origin, objectPointer(index, "origin"), "Object origin"
            ),
            .scale = vector3Value(
                *model_, node.scale, objectPointer(index, "scale"), "Object scale"
            ),
            .angles = vector3Value(
                *model_, node.angles, objectPointer(index, "angles"), "Object angles"
            ),
        };
        node.worldTransform = node.localTransform;
        node.isVisible = booleanValue(
            *model_, node.visible, objectPointer(index, "visible")
        );
        result.nodes.push_back(std::move(node));
    }

    // Initialization order guarantees every parent has already been resolved,
    // even when the child appeared first in scene.json.
    for (const std::size_t index : initializationOrder_) {
        SceneGraphNodeSnapshot& node = result.nodes[index];
        if (!node.parent) {
            continue;
        }
        const std::size_t parentIndex = objectIndices_.at(*node.parent);
        node.worldTransform = combine(
            result.nodes[parentIndex].worldTransform,
            node.localTransform
        );
    }
    return result;
}

SceneGraph::EvaluationFrame::EvaluationFrame(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
SceneGraph::EvaluationFrame::~EvaluationFrame() = default;
EvaluatedValue SceneGraph::EvaluationFrame::evaluate(
    const DynamicValue& dynamic, std::string pointer
) {
    return impl_->evaluate(dynamic, pointer);
}
std::uint64_t SceneGraph::EvaluationFrame::modelRevision() const noexcept {
    return impl_->properties.revision;
}
const std::map<std::string, Value>&
SceneGraph::EvaluationFrame::propertyValues() const noexcept {
    return impl_->properties.values;
}
const std::map<std::string, EvaluatedValue>&
SceneGraph::EvaluationFrame::evaluatedScriptValues() const noexcept {
    return impl_->values;
}
std::vector<SceneGraph::EvaluationFrame::ScriptEvaluationStats>
SceneGraph::EvaluationFrame::scriptEvaluationStats() const {
    std::vector<ScriptEvaluationStats> result;
    result.reserve(impl_->scriptStats.size());
    for (const auto& [pointer, stats] : impl_->scriptStats) result.push_back(stats);
    return result;
}

std::unique_ptr<SceneGraph::EvaluationFrame> SceneGraph::evaluationFrame(
    const SceneFrameInputs& inputs
) {
    return std::unique_ptr<EvaluationFrame>(new EvaluationFrame(
        std::make_unique<EvaluationFrame::Impl>(*this, inputs)
    ));
}

SceneGraphSnapshot SceneGraph::snapshot(EvaluationFrame& frame) const {
    const auto& objects = model_->project().scene.objects;
    SceneGraphSnapshot result;
    result.modelRevision = frame.modelRevision();
    result.propertyValues = frame.propertyValues();
    result.initializationOrder = initializationOrder_;
    result.renderOrder = renderOrder_;
    result.nodes.reserve(objects.size());
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const ObjectBase& object = objects[index].base;
        SceneGraphNodeSnapshot node;
        node.objectIndex = index;
        node.id = object.id;
        node.parent = object.parent;
        node.disablePropagation = object.disablePropagation;
        node.origin = frame.evaluate(object.origin, objectPointer(index, "origin"));
        node.scale = frame.evaluate(object.scale, objectPointer(index, "scale"));
        node.angles = frame.evaluate(object.angles, objectPointer(index, "angles"));
        node.visible = frame.evaluate(object.visible, objectPointer(index, "visible"));
        node.localTransform = {
            .origin = vector3Value(*model_, node.origin, objectPointer(index, "origin"), "Object origin"),
            .scale = vector3Value(*model_, node.scale, objectPointer(index, "scale"), "Object scale"),
            .angles = vector3Value(*model_, node.angles, objectPointer(index, "angles"), "Object angles"),
        };
        node.worldTransform = node.localTransform;
        node.isVisible = booleanValue(*model_, node.visible, objectPointer(index, "visible"));
        result.nodes.push_back(std::move(node));
    }
    for (const std::size_t index : initializationOrder_) {
        SceneGraphNodeSnapshot& node = result.nodes[index];
        if (node.parent) {
            node.worldTransform = combine(
                result.nodes[objectIndices_.at(*node.parent)].worldTransform,
                node.localTransform
            );
        }
    }
    return result;
}

std::shared_ptr<const SceneModel> SceneGraph::model() const noexcept {
    return model_;
}

}  // namespace we::scene
