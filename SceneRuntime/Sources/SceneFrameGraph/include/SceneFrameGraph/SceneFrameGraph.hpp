#ifndef WE_SCENE_FRAME_GRAPH_HPP
#define WE_SCENE_FRAME_GRAPH_HPP

#include <SceneGraph/SceneGraph.hpp>
#include <SceneParticle/ParticleSimulation.hpp>

#include <algorithm>
#include <array>
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
// Matches the official renderer's integer quality threshold used by
// volumetric targets: levels below 3 use the low-resolution two-buffer path.
enum class FrameRenderQuality : std::uint8_t {
    powerSaving = 1,
    balanced = 2,
    high = 3,
    ultra = 4,
};
enum class FrameGeometryKind {
    imageLocal,
    fullscreenLocal,
    imageScene,
    passthroughCapture,
    puppetMesh,
    lightVolume,
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
    // Back-buffer samplers read the depth attachment produced by the bound
    // framebuffer rather than its color attachment.
    bool sampleDepth = false;

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
    double perspectiveOverrideFieldOfView = 0.0;
    bool orthographic = true;
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

struct FrameFogDescriptor {
    bool enabled = false;
    FrameColor color;
    double start = 0.0;
    double end = 0.0;
    double startDensity = 0.0;
    double endDensity = 0.0;
};

enum class FrameLightType {
    point,
    spot,
    tube,
    directional,
};

struct FrameLightConfiguration {
    std::size_t point = 0;
    std::size_t pointShadow = 0;
    std::size_t spot = 0;
    std::size_t spotCookie = 0;
    std::size_t spotShadow = 0;
    std::size_t spotShadowCookie = 0;
    std::size_t tube = 0;
    std::size_t directional = 0;
    std::size_t directionalShadow = 0;
};

struct FrameLightDescriptor {
    std::size_t objectIndex = 0;
    int objectId = 0;
    FrameLightType type = FrameLightType::point;
    bool visible = false;
    ObjectTransform worldTransform;
    FrameColor color;
    double intensity = 0.0;
    double radius = 0.0;
    double exponent = 0.0;
    double innerCone = 0.0;
    double outerCone = 0.0;
    Vector3 controlPoint;
    bool castShadow = false;
    std::string cookie;
    bool useCookie = false;
    bool castVolumetrics = false;
    double density = 1.0;
    double volumetricsExponent = 1.0;
    double lightSourceSize = 0.0;
    std::array<double, 3> cascadeDistances{};
};

struct FrameTextureAnimationOverride final {
    std::string assetIdentity;
    std::size_t frame = 0;
};

struct FramePuppetAnimationLayer final {
    int animationId = 0;
    double rate = 1.0;
    double blend = 1.0;
    bool additive = false;
};

struct FrameImageDescriptor {
    std::size_t objectIndex = 0;
    int objectId = 0;
    bool visible = false;
    // Layer-level Solid is an explicit SceneScript cursor hit-test opt-in. It
    // is distinct from the model's procedural `solidLayer` image source flag.
    bool solid = false;
    // Runtime callback exports are also an interaction opt-in. Published
    // scenes exist where the editor serialized the callback binding without a
    // redundant Solid field, so capability must be resolved after scripts are
    // initialized rather than inferred from JSON alone.
    bool cursorInteractive = false;
    bool castShadow = false;
    bool passthrough = false;
    bool fullscreen = false;
    bool perspective = false;
    FrameVector2 size;
    ObjectTransform worldTransform;
    FrameResourceRef source;
    FrameResourceRef compositeA;
    FrameResourceRef compositeB;
    std::shared_ptr<const PuppetMesh> puppetMesh;
    std::vector<FramePuppetAnimationLayer> puppetAnimationLayers;
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
    bool perspective = false;
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
    bool limitRows = false;
    bool limitUseEllipsis = false;
    bool limitWidth = false;
    int maxRows = 0;
    double maxWidth = 0.0;
    bool msdf = false;
    bool blur = false;
    double blurSize = 0.0;
    bool dropShadow = false;
    FrameColor dropShadowColor;
    FrameVector2 dropShadowOffset;
    double dropShadowOpacity = 1.0;
    double dropShadowSize = 0.0;
    bool outline = false;
    FrameColor outlineColor;
    double outlineThickness = 0.0;
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

// Render variables are intentionally pass-local. Wallpaper Engine reuses the
// g_RenderVarN names for unrelated stock materials (particles, MSDF text, and
// volumetric lights), so they cannot be represented as frame-wide defaults.
// A pass that activates one of these uniforms must carry the corresponding
// payload; the Metal executor leaves an absent payload unbound and reports the
// missing provider during validation.
struct FrameRenderVariablePayload final {
    std::array<double, 4> values{};

    [[nodiscard]] friend bool operator==(
        const FrameRenderVariablePayload& lhs,
        const FrameRenderVariablePayload& rhs
    ) = default;
};

struct FrameRenderMatrixOverrides final {
    std::optional<std::array<double, 16>> model;
    std::optional<std::array<double, 16>> viewProjection;
    std::optional<std::array<double, 16>> effectModel;
    std::optional<std::array<double, 16>> alternateModel;
    std::optional<std::array<double, 16>> alternateViewProjection;
    std::vector<std::array<double, 16>> viewportViewProjections;
};

struct FrameRenderRegion final {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct FrameShadowAtlasEntry final {
    std::size_t lightIndex = 0;
    std::size_t cascade = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t size = 0;
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
    std::array<std::optional<FrameRenderVariablePayload>, 5> renderVariables{};
    FrameRenderMatrixOverrides matrixOverrides;
    std::optional<FrameRenderRegion> viewport;
    std::optional<FrameRenderRegion> scissor;
    std::uint32_t instanceCount = 1;
    double depthBias = 0.0;
    double depthSlopeScale = 0.0;
    double depthBiasClamp = 0.0;
    std::size_t puppetSubmeshIndex = 0;
    std::optional<std::size_t> lightIndex;
    bool shadowCaster = false;
    std::string shadowSourceVertexShaderPath;
    std::string shadowSourceFragmentShaderPath;
    ComboMap shadowSourceCombos;
    bool writeAlpha = true;
    bool writeColor = true;
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
    bool clearDepth = false;
    double depthValue = 1.0;
};

struct FrameTextCommand {
    std::size_t textIndex = 0;
    int objectId = 0;
    FrameResourceRef destination;
    // Text without effects is drawn directly in scene space. Effect-backed
    // text is first rasterized into its local layer framebuffer, then enters
    // the ordinary image effect pipeline.
    bool localSpace = false;
};

// Text effects enter the ordinary image pipeline through a generated proxy
// image. Its framebuffer geometry cannot be fixed while the frame graph is
// built because scripted media text may change its raster bounds every frame.
// The executor resolves this relationship after rasterization and before any
// framebuffer allocation.
enum class FrameTextEffectFramebufferSizing {
    relative,
    fit,
    fixed,
};

struct FrameTextEffectFramebufferDescriptor {
    std::size_t framebufferIndex = 0;
    FrameTextEffectFramebufferSizing sizing =
        FrameTextEffectFramebufferSizing::relative;
    // Relative sizing stores the authored divisor. Fit sizing stores the
    // authored maximum dimension. Fixed sizing ignores this value.
    double value = 1.0;
};

struct FrameTextEffectDescriptor {
    std::size_t textIndex = 0;
    std::size_t imageIndex = 0;
    std::vector<FrameTextEffectFramebufferDescriptor> framebuffers;
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
    FrameRenderQuality renderQuality = FrameRenderQuality::high;
    FrameCameraDescriptor camera;
    FrameParallaxDescriptor parallax;
    FrameColor ambientColor;
    FrameColor skylightColor;
    FrameFogDescriptor distanceFog;
    FrameFogDescriptor heightFog;
    FrameLightConfiguration lightConfiguration;
    // Per-entry square size used by the official shadow atlas allocator.
    std::uint32_t shadowAtlasResolution = 0;
    std::uint32_t shadowAtlasWidth = 2;
    std::uint32_t shadowAtlasHeight = 2;
    std::vector<FrameShadowAtlasEntry> shadowAtlasEntries;
    // The single cookie sampler used by the official lighting module. The
    // selected light asset is captured per frame because scripted light
    // properties can change between evaluations.
    FrameResourceRef lightCookie;
    std::vector<FrameLightDescriptor> lights;
    bool clearEnabled = true;
    FrameColor clearColor;
    FrameResourceRef output;
    std::vector<FramebufferDescriptor> framebuffers;
    std::vector<FrameImageDescriptor> images;
    std::vector<FrameTextDescriptor> texts;
    std::vector<FrameTextEffectDescriptor> textEffects;
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
        SceneFrameInputs inputs,
        FrameRenderQuality renderQuality
    );

    std::shared_ptr<const SceneGraph> graph_;
    SceneGraphSnapshot graphSnapshot_;
    SceneFrameInputs inputs_;
    FrameRenderQuality renderQuality_ = FrameRenderQuality::high;
    std::map<std::string, EvaluatedValue> scriptedValues_;
    std::vector<int> cursorInteractiveLayerIds_;
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
// each property revision. Planning performs no GPU work.
class SceneFrameGraph final {
public:
    [[nodiscard]] static std::shared_ptr<SceneFrameGraph> create(
        std::shared_ptr<SceneGraph> graph
    );

    SceneFrameGraph(const SceneFrameGraph&) = delete;
    SceneFrameGraph& operator=(const SceneFrameGraph&) = delete;
    ~SceneFrameGraph();

    [[nodiscard]] FramePlan snapshot(
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt,
        FrameRenderQuality renderQuality = FrameRenderQuality::high
    ) const;
    [[nodiscard]] FramePlan snapshot(
        const SceneFrameInputs& inputs,
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt,
        FrameRenderQuality renderQuality = FrameRenderQuality::high
    ) const;
    [[nodiscard]] EvaluatedFramePlan evaluate(
        const SceneFrameInputs& inputs,
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt,
        FrameRenderQuality renderQuality = FrameRenderQuality::high
    ) const;
    [[nodiscard]] FramePlan reproject(
        const FrameEvaluationState& evaluation,
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt,
        std::optional<FrameRenderQuality> renderQuality = std::nullopt
    ) const;
    // Resolves the logical scene projection without evaluating scripts. Hosts
    // use this for pointer mapping before a frame has been rendered; physical
    // framebuffer quality must never feed back into these world dimensions.
    [[nodiscard]] FrameProjectionSize projectionSize(
        std::optional<FrameProjectionSize> drawableFallback = std::nullopt
    ) const;
    [[nodiscard]] bool requiresDrawableProjectionFallback() const noexcept;
    [[nodiscard]] std::shared_ptr<const SceneGraph> graph() const noexcept;

private:
    explicit SceneFrameGraph(std::shared_ptr<SceneGraph> graph);

    std::shared_ptr<SceneGraph> graph_;
    std::optional<FrameProjectionSize> automaticProjectionSize_;
};

}  // namespace we::scene

#endif
