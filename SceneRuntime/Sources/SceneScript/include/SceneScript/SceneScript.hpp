#ifndef WE_SCENE_SCRIPT_SCENE_SCRIPT_HPP
#define WE_SCENE_SCRIPT_SCENE_SCRIPT_HPP

#include <SceneModel/SceneModel.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace we::scene::script {

enum class ScriptErrorCode : std::int32_t {
    module = 1,
    exception = 2,
    invalidResultType = 3,
    nonFiniteResult = 4,
    audioInputUnavailable = 5,
    resourceLimit = 6,
};

class ScriptError final : public std::runtime_error {
public:
    ScriptError(ScriptErrorCode code, std::string message);

    [[nodiscard]] ScriptErrorCode code() const noexcept;

private:
    ScriptErrorCode code_;
};

struct ScriptFrameInputs {
    double runtimeSeconds = 0;
    double frameTimeSeconds = 0;
    double pointerX = 0;
    double pointerY = 0;
};

struct ScriptLimits {
    std::size_t memoryBytes = 32 * 1024 * 1024;
    std::size_t stackBytes = 512 * 1024;
    std::chrono::nanoseconds executionTime = std::chrono::milliseconds(10);
};

class ScriptInstance;

class ScriptRuntime final {
public:
    explicit ScriptRuntime(ScriptLimits limits = {});
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    [[nodiscard]] std::unique_ptr<ScriptInstance> createInstance(
        std::string moduleSource,
        RuntimeValue initialValue,
        std::map<std::string, RuntimeValue> properties = {},
        std::optional<std::string> condition = std::nullopt
    );

private:
    friend class ScriptInstance;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class ScriptInstance final {
public:
    ~ScriptInstance();

    ScriptInstance(const ScriptInstance&) = delete;
    ScriptInstance& operator=(const ScriptInstance&) = delete;

    [[nodiscard]] RuntimeValue evaluate(const ScriptFrameInputs& inputs);
    [[nodiscard]] RuntimeValue currentValue() const;

    // Mirrors an upstream DynamicValue connection update. This replaces the
    // script's current value without recreating its module or rerunning init.
    void updateCurrent(RuntimeValue value);

    // Replaces the host-owned script-property values without replacing the
    // JavaScript scriptProperties object. Scripts may safely retain a
    // reference to that object across frames.
    void updateProperties(std::map<std::string, RuntimeValue> properties);

    // System audio capture is intentionally outside the migration goal.
    [[noreturn]] void registerAudioBuffers();

private:
    friend class ScriptRuntime;
    struct Impl;
    explicit ScriptInstance(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace we::scene::script

#endif
