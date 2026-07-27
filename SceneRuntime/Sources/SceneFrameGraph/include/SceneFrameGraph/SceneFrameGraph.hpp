#ifndef WE_SCENE_FRAME_GRAPH_HPP
#define WE_SCENE_FRAME_GRAPH_HPP

#include <SceneGraph/SceneGraph.hpp>
#include <SceneParticle/ParticleSimulation.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace we::scene {

enum class FrameResourceKind {
    assetTexture,
    framebuffer,
    userPropertyTexture,
    hostTexture,
};
enum class FramebufferFormat { rgba8, r8, rg16f, r16f };
enum class FramebufferWrapMode { clampToEdge, clampToBorder, repeat };
enum class FrameGeometryKind {
    imageLocal,
    fullscreenLocal,
    imageScene,
    passthroughCapture,
    puppetMesh,
};
enum class FrameTexCoordKind { image, full };
enum class FrameOperationKind {
    render = 0,
    copy = 1,
    swap = 2,
    clear = 3,
    text = 4,
    particle = 5,
};
enum class FrameSoundPlaybackMode { loop, random, single };
enum class FrameSoundPlaybackCommandAction { play, pause, stop };
struct FrameSoundPlaybackCommand final {
    FrameSoundPlaybackCommandAction action =
        FrameSoundPlaybackCommandAction::play;
    std::uint64_t generation = 0;
};
enum class FramePlanIssueCode {
    textRenderingUnavailable,
    soundRuntimeUnavailable,
    scriptRuntimeUnavailable,
    passthroughUnavailable,
    puppetUnavailable,
    composeUnavailable,
    imageMaterialUnavailable,
    framebufferDescriptorMissing,
    framebufferReadBeforeWrite,
    framebufferFeedbackLoop,
    audioInputUnavailable,
    perspectiveProjectionUnavailable,
    effectPassPlanningFailed,
    objectPlanningFailed,
};

// A plan can remain useful when one optional object or pass is unavailable.
// Severity describes the smallest unit that must be suppressed to keep the
// rest of the frame executable.
enum class FramePlanIssueSeverity {
    warning,
    skipPass,
    skipObject,
    frameFatal,
};

struct FrameVector2 {
    double x = 0.0;
    double y = 0.0;

    [[nodiscard]] friend bool operator==(
        const FrameVector2& lhs,
        const FrameVector2& rhs
    ) = default;
};

struct FrameColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;

    [[nodiscard]] friend bool operator==(
        const FrameColor& lhs,
        const FrameColor& rhs
    ) = default;
};

struct FrameResourceRef {
    FrameResourceKind kind = FrameResourceKind::assetTexture;
    // Stable identity used by an executor. logicalName preserves the name
    // authored in Wallpaper Engine JSON for diagnostics and introspection.
    std::string id;
    std::string logicalName;
    // Only userPropertyTexture uses resolvedAssetName. The stable id remains
    // the property key while each immutable frame captures the selected asset
    // name separately, so a property update cannot alias an old GPU resource.
    std::string resolvedAssetName;

    [[nodiscard]] friend bool operator==(
        const FrameResourceRef& lhs,
        const FrameResourceRef& rhs
    ) = default;
};

[[nodiscard]] FrameResourceRef frameAssetTextureResource(
    std::string_view name
);

enum class FrameTextureCandidateSource {
    shaderVertexDefault,
    shaderFragmentDefault,
    materialTexture,
    materialUserTexture,
    overrideTexture,
    overrideUserTexture,
    bind,
};

struct FrameTextureCandidate final {
    FrameTextureCandidateSource source =
        FrameTextureCandidateSource::materialTexture;
    FrameResourceRef resource;

    [[nodiscard]] friend bool operator==(
        const FrameTextureCandidate& lhs,
        const FrameTextureCandidate& rhs
    ) = default;
};

// Candidates are stored from lowest to highest precedence. Readiness is a GPU
// execution concern: the executor walks the vector in reverse and only falls
// back to previousInput/input after every authored/default provider is
// unavailable. This is the TextureProvider chain used by the Linux runtime,
// represented as immutable frame data instead of raw renderer pointers.
struct FrameTextureBinding final {
    std::vector<FrameTextureCandidate> candidates;

    [[nodiscard]] friend bool operator==(
        const FrameTextureBinding& lhs,
        const FrameTextureBinding& rhs
    ) = default;
};

struct FramebufferDescriptor {
    FrameResourceRef resource;
    FramebufferFormat format = FramebufferFormat::rgba8;
    FramebufferWrapMode wrapMode = FramebufferWrapMode::clampToEdge;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double scale = 1.0;
    bool unique = false;
};

struct FrameCameraDescriptor {
    Vector3 center;
    Vector3 eye;
    Vector3 up;
    double nearPlane = 0.0;
    double farPlane = 1000.0;
    double fieldOfView = 50.0;
    bool orthogonalProjectionAuto = false;
    std::uint32_t orthogonalProjectionWidth = 0;
    std::uint32_t orthogonalProjectionHeight = 0;
};

struct FrameParallaxDescriptor {
    bool enabled = false;
    double amount = 1.0;
    double delay = 0.0;
    double mouseInfluence = 1.0;
};

struct FrameTextureAnimationOverride final {
    std::string assetIdentity;
    std::size_t frame = 0;
};

struct FrameImageDescriptor {
    std::size_t objectIndex = 0;
    int objectId = 0;
    bool visible = false;
    // Layer-level Solid flag controls SceneScript cursor hit testing. This is
    // distinct from the model's procedural `solidLayer` image source flag.
    bool solid = false;
    bool passthrough = false;
    bool fullscreen = false;
    FrameVector2 size;
    ObjectTransform worldTransform;
    FrameResourceRef source;
    FrameResourceRef compositeA;
    FrameResourceRef compositeB;
    std::shared_ptr<const PuppetMesh> puppetMesh;
    EvaluatedValue alpha;
    EvaluatedValue color;
    EvaluatedValue brightness;
    EvaluatedValue colorBlendMode;
    EvaluatedValue parallaxDepth;
    std::string horizontalAlignment;
    std::optional<FrameTextureAnimationOverride> textureAnimation;
};

struct FrameTextDescriptor {
    std::size_t objectIndex = 0;
    int objectId = 0;
    bool visible = false;
    std::string text;
    std::string font;
    double pointSize = 0.0;
    FrameVector2 size;
    FrameColor color;
    double alpha = 1.0;
    FrameVector2 padding;
    FrameVector2 spacing;
    ObjectTransform worldTransform;
    std::string horizontalAlignment;
    std::string verticalAlignment;
};

enum class FrameParticleRendererKind {
    sprite,
    spriteTrail,
    rope,
    ropeTrail,
};

struct FrameParticleRendererDescriptor {
    FrameParticleRendererKind kind = FrameParticleRendererKind::sprite;
    double length = 0.05;
    double maxLength = 10.0;
    double minLength = 0.0;
    double subdivision = 1.0;
    double segments = 4.0;
    double uvScale = 1.0;
    bool uvScrolling = false;
    bool uvSmoothing = true;
    bool fadeAlpha = false;
    bool fadeSize = false;
};

struct FrameParticleDescriptor {
    std::size_t objectIndex = 0;
    int objectId = 0;
    bool visible = false;
    ObjectTransform worldTransform;
    std::string definitionIdentity;
    std::string shader;
    std::string vertexShaderPath;
    std::string fragmentShaderPath;
    BlendingMode blending = BlendingMode::normal;
    CullingMode culling = CullingMode::disabled;
    DepthMode depthTest = DepthMode::disabled;
    DepthMode depthWrite = DepthMode::disabled;
    FrameResourceRef texture0;
    std::map<int, FrameTextureBinding> textures;
    ComboMap combos;
    std::map<std::string, EvaluatedValue> constants;
    FrameVector2 parallaxDepth;
    bool perspective = false;
    std::string animationMode;
    double sequenceMultiplier = 1.0;
    FrameParticleRendererDescriptor renderer;
    particle::Configuration configuration;
};

struct FrameSoundDescriptor {
    std::size_t objectIndex = 0;
    int objectId = 0;
    bool visible = false;
    std::vector<std::string> sources;
    FrameSoundPlaybackMode playbackMode = FrameSoundPlaybackMode::loop;
    double volume = 1.0;
    bool startSilent = false;
    bool muteInEditor = false;
    double minimumTime = 0.0;
    double maximumTime = 0.0;
    std::optional<FrameSoundPlaybackCommand> playbackCommand;
};

struct FramePassOrigin {
    std::size_t imageIndex = 0;
    int objectId = 0;
    std::optional<std::size_t> effectIndex;
    std::optional<std::size_t> effectPassIndex;
    std::size_t materialPassIndex = 0;
};

struct FrameRenderPass {
    FramePassOrigin origin;
    std::string shader;
    std::string vertexShaderPath;
    std::string fragmentShaderPath;
    BlendingMode blending = BlendingMode::normal;
    CullingMode culling = CullingMode::disabled;
    DepthMode depthTest = DepthMode::disabled;
    DepthMode depthWrite = DepthMode::disabled;
    FrameGeometryKind geometry = FrameGeometryKind::imageLocal;
    FrameTexCoordKind textureCoordinates = FrameTexCoordKind::image;
    FrameResourceRef input;
    std::optional<FrameResourceRef> previousInput;
    FrameResourceRef destination;
    std::map<int, FrameTextureBinding> textures;
    ComboMap combos;
    std::map<std::string, EvaluatedValue> constants;
    bool writeAlpha = true;
};

struct FrameCopyCommand {
    FramePassOrigin origin;
    FrameResourceRef source;
    FrameResourceRef destination;
};

struct FrameSwapCommand {
    FramePassOrigin origin;
    FrameResourceRef source;
    FrameResourceRef destination;
};

struct FrameClearCommand {
    FramePassOrigin origin;
    FrameResourceRef destination;
    FrameColor color;
};

struct FrameTextCommand {
    std::size_t textIndex = 0;
    int objectId = 0;
    FrameResourceRef destination;
};

struct FrameParticleCommand {
    std::size_t particleIndex = 0;
    int objectId = 0;
    FrameResourceRef destination;
};

using FrameOperation =
    std::variant<FrameRenderPass, FrameCopyCommand, FrameSwapCommand, FrameClearCommand,
                 FrameTextCommand, FrameParticleCommand>;

struct FramePlanIssue {
    FramePlanIssueCode code = FramePlanIssueCode::scriptRuntimeUnavailable;
    FramePlanIssueSeverity severity = FramePlanIssueSeverity::frameFatal;
    std::optional<int> objectId;
    std::string assetPath;
    std::string jsonPointer;
    std::string message;
};

struct FramePlan {
    std::uint64_t modelRevision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    FrameCameraDescriptor camera;
    FrameParallaxDescriptor parallax;
    FrameColor ambientColor;
    FrameColor skylightColor;
    bool clearEnabled = true;
    FrameColor clearColor;
    FrameResourceRef output;
    std::vector<FramebufferDescriptor> framebuffers;
    std::vector<FrameImageDescriptor> images;
    std::vector<FrameTextDescriptor> texts;
    std::vector<FrameParticleDescriptor> particles;
    std::vector<FrameSoundDescriptor> sounds;
    std::vector<FrameOperation> operations;
    std::vector<FramePlanIssue> issues;
    std::vector<SceneGraph::EvaluationFrame::ScriptEvaluationStats> scriptEvaluations;

    [[nodiscard]] bool isExecutable() const noexcept {
        return std::none_of(
            issues.begin(), issues.end(), [](const FramePlanIssue& issue) {
                return issue.severity == FramePlanIssueSeverity::frameFatal;
            }
        );
    }
};

struct FrameProjectionSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Immutable dynamic evaluation captured from one successful planning pass.
// Reprojection consumes only this frozen state and never re-enters QuickJS.
class FrameEvaluationState final {
public:
    FrameEvaluationState(const FrameEvaluationState&) = delete;
    FrameEvaluationState& operator=(const FrameEvaluationState&) = delete;
    FrameEvaluationState(FrameEvaluationState&&) noexcept = default;
    FrameEvaluationState& operator=(FrameEvaluationState&&) noexcept = default;
    ~FrameEvaluationState() = default;

private:
    friend class SceneFrameGraph;
    FrameEvaluationState(
        std::shared_ptr<const SceneGraph> graph,
        SceneGraphSnapshot graphSnapshot,
        SceneFrameInputs inputs
    );

    std::shared_ptr<const SceneGraph> graph_;
    SceneGraphSnapshot graphSnapshot_;
    SceneFrameInputs inputs_;
    std::map<std::string, EvaluatedValue> scriptedValues_;
    std::vector<SceneGraph::EvaluationFrame::ScriptEvaluationStats> scriptEvaluations_;
};

struct EvaluatedFramePlan final {
    FramePlan plan;
    FrameEvaluationState evaluation;
};

[[nodiscard]] FrameOperationKind operationKind(
    const FrameOperation& operation
) noexcept;

// Retains the graph/model/runtime and creates a coherent immutable plan for
// each property revision. Planning performs no OpenGL work.
class SceneFrameGraph final {
public:
    [[nodiscard]] static std::shared_ptr<SceneFrameGraph> create(
        std::shared_ptr<SceneGraph> graph
    );

    SceneFrameGraph(const SceneFrameGraph&) = delete;
    SceneFrameGraph& operator=(const SceneFrameGraph&) = delete;
    ~SceneFrameGraph();

    [[nodiscard]] FramePlan snapshot(
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt
    ) const;
    [[nodiscard]] FramePlan snapshot(
        const SceneFrameInputs& inputs,
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt
    ) const;
    [[nodiscard]] EvaluatedFramePlan evaluate(
        const SceneFrameInputs& inputs,
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt
    ) const;
    [[nodiscard]] FramePlan reproject(
        const FrameEvaluationState& evaluation,
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt
    ) const;
    [[nodiscard]] bool requiresDrawableProjectionFallback() const noexcept;
    [[nodiscard]] std::shared_ptr<const SceneGraph> graph() const noexcept;

private:
    explicit SceneFrameGraph(std::shared_ptr<SceneGraph> graph);

    [[nodiscard]] FrameProjectionSize projectionSize(
        std::optional<FrameProjectionSize> drawableFallback
    ) const;

    std::shared_ptr<SceneGraph> graph_;
    std::optional<FrameProjectionSize> automaticProjectionSize_;
};

}  // namespace we::scene

#endif
