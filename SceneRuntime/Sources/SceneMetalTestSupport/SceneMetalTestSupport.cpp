#include <SceneMetalTestSupport/SceneMetalTestSupport.h>
#include <SceneMetal/FramebufferPlanRequirements.hpp>
#include <SceneMetal/FramePlanExecutor.hpp>
#include "../SceneMetal/SceneMetalDevice.hpp"
#include "../SceneMetal/SceneMetalPresentation.hpp"
#include "../SceneMetal/SceneVideoDecoder.hpp"
#include "../SceneMetal/TextCoverageRenderer.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <span>
#include <thread>
#include <utility>
#include <vector>
using namespace we::scene;

namespace {

metal::PresentationViewport presentationViewport(
    const WESceneMetalTestPresentationViewport& viewport
) {
    return {
        .canvasWidth = viewport.canvas_width,
        .canvasHeight = viewport.canvas_height,
        .viewportX = viewport.viewport_x,
        .viewportY = viewport.viewport_y,
        .viewportWidth = viewport.viewport_width,
        .viewportHeight = viewport.viewport_height,
        .drawableWidth = viewport.drawable_width,
        .drawableHeight = viewport.drawable_height,
    };
}

bool presentationScaling(int value, metal::PresentationScaling& scaling) {
    switch (value) {
        case 0:
            scaling = metal::PresentationScaling::stretch;
            return true;
        case 1:
            scaling = metal::PresentationScaling::aspectFit;
            return true;
        case 2:
            scaling = metal::PresentationScaling::aspectFill;
            return true;
        case 3:
            scaling = metal::PresentationScaling::automatic;
            return true;
        default:
            return false;
    }
}

WESceneMetalTestPresentationRect presentationRect(
    const metal::PresentationRect& rect
) {
    return {
        .x = rect.x,
        .y = rect.y,
        .width = rect.width,
        .height = rect.height,
    };
}

std::vector<std::uint8_t> presentationPattern(
    std::uint32_t width,
    std::uint32_t height
) {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4, 0
    );
    const auto set = [&](std::uint32_t x, std::uint32_t y,
                         std::array<std::uint8_t, 4> color) {
        const std::size_t offset =
            (static_cast<std::size_t>(y) * width + x) * 4;
        std::copy(color.begin(), color.end(), pixels.begin() + offset);
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            set(x, y, {0, 0, 0, 255});
        }
        set(0, y, {255, 0, 0, 255});
        set(width - 1, y, {0, 255, 0, 255});
    }
    for (std::uint32_t x = 1; x + 1 < width; ++x) {
        set(x, 0, {0, 0, 255, 255});
        set(x, height - 1, {255, 255, 0, 255});
    }
    return pixels;
}

metal::FramebufferResource uploadedFramebuffer(
    metal::Device::Session& session,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::uint8_t> pixels
) {
    auto texture = session.uploadRGBA8Texture(width, height, pixels);
    return {
        .colorTexture = std::move(texture),
        .width = width,
        .height = height,
        .format = metal::PixelFormat::rgba8,
    };
}

FrameResourceRef framebufferRequirementResource(std::string id) {
    return {
        .kind = FrameResourceKind::framebuffer,
        .id = std::move(id),
    };
}

void appendFramebufferRequirementDescriptor(
    FramePlan& plan,
    std::string id
) {
    plan.framebuffers.push_back({
        .resource = framebufferRequirementResource(std::move(id)),
        .format = FramebufferFormat::rgba8,
        .width = 4,
        .height = 4,
        .scale = 1.0,
    });
}

}  // namespace
extern "C" int we_scene_metal_test_framebuffer_plan_requirements(void) {
    try {
        FramePlan plan{
            .width = 4,
            .height = 4,
            .output = framebufferRequirementResource("output"),
        };
        for (const char* id : {
                 "output", "input", "previous", "binding", "copy",
                 "swap", "clear", "text", "unused",
             }) {
            appendFramebufferRequirementDescriptor(plan, id);
        }
        plan.operations.emplace_back(FrameRenderPass{
            .depthTest = DepthMode::disabled,
            .depthWrite = DepthMode::disabled,
            .input = framebufferRequirementResource("input"),
            .previousInput = framebufferRequirementResource("previous"),
            .destination = framebufferRequirementResource("output"),
            .textures = {{
                1,
                FrameTextureBinding{.candidates = {{
                    .resource = framebufferRequirementResource("binding"),
                }}},
            }},
        });
        plan.operations.emplace_back(FrameRenderPass{
            .depthTest = DepthMode::enabled,
            .destination = framebufferRequirementResource("output"),
        });
        plan.operations.emplace_back(FrameCopyCommand{
            .source = framebufferRequirementResource("copy"),
            .destination = framebufferRequirementResource("output"),
        });
        plan.operations.emplace_back(FrameSwapCommand{
            .source = framebufferRequirementResource("output"),
            .destination = framebufferRequirementResource("swap"),
        });
        plan.operations.emplace_back(FrameClearCommand{
            .destination = framebufferRequirementResource("clear"),
        });
        plan.operations.emplace_back(FrameTextCommand{
            .destination = framebufferRequirementResource("text"),
        });
        plan.particles.push_back({
            .depthWrite = DepthMode::enabled,
            .texture0 = frameAssetTextureResource("particle"),
        });
        plan.operations.emplace_back(FrameParticleCommand{
            .particleIndex = 0,
            .destination = framebufferRequirementResource("output"),
        });

        const metal::FramebufferPlanRequirements requirements =
            metal::analyzeFramebufferPlanRequirements(plan);
        if (requirements.active.size() != 8 ||
            requirements.active.contains("unused") ||
            !requirements.active.at("output").requiresDepthAttachment ||
            !requirements.active.at("swap").requiresDepthAttachment) {
            return 0;
        }
        for (const auto& [id, requirement] : requirements.active) {
            if (id != "output" && id != "swap" &&
                requirement.requiresDepthAttachment) {
                return 0;
            }
        }

        plan.operations.clear();
        plan.operations.emplace_back(FrameClearCommand{
            .destination = framebufferRequirementResource("output"),
        });
        const metal::FramebufferPlanRequirements colorOnly =
            metal::analyzeFramebufferPlanRequirements(plan);
        if (colorOnly.active.size() != 1 ||
            colorOnly.active.at("output").requiresDepthAttachment) {
            return 0;
        }
        plan.operations.emplace_back(FrameRenderPass{
            .depthWrite = DepthMode::enabled,
            .destination = framebufferRequirementResource("output"),
        });
        const metal::FramebufferPlanRequirements depthChanged =
            metal::analyzeFramebufferPlanRequirements(plan);
        return depthChanged.active.size() == 1 &&
                depthChanged.active.at("output").requiresDepthAttachment
            ? 1 : 0;
    } catch (...) {
        return 0;
    }
}
extern "C" int we_scene_metal_test_physical_render_policy(void) {
    try {
        FramePlan plan;
        plan.width = 3840;
        plan.height = 2160;
        plan.camera.orthogonalProjectionAuto = true;
        plan.camera.orthogonalProjectionWidth = 3840;
        plan.camera.orthogonalProjectionHeight = 2160;
        plan.output = framebufferRequirementResource("output");
        plan.framebuffers = {
            {
                .resource = plan.output,
                .format = FramebufferFormat::rgba8,
                .width = 3840,
                .height = 2160,
                .scale = 1.0,
            },
            {
                .resource = framebufferRequirementResource("quarter"),
                .format = FramebufferFormat::rgba8,
                .width = 960,
                .height = 540,
                .scale = 0.25,
            },
        };
        const metal::PhysicalRenderTarget balancedTarget{
            .backingWidth = 2560,
            .backingHeight = 1664,
            .quality = metal::PhysicalRenderQuality::balanced,
        };
        const metal::PhysicalRenderSize cover = metal::physicalRenderSize(
            plan, balancedTarget, metal::PresentationScaling::aspectFill
        );
        const metal::PhysicalRenderSize automatic = metal::physicalRenderSize(
            plan, balancedTarget, metal::PresentationScaling::automatic
        );
        const metal::PhysicalRenderSize stretch = metal::physicalRenderSize(
            plan, balancedTarget, metal::PresentationScaling::stretch
        );
        const metal::PhysicalRenderSize fit = metal::physicalRenderSize(
            plan, balancedTarget, metal::PresentationScaling::aspectFit
        );
        if (cover != metal::PhysicalRenderSize{1920, 1080} ||
            automatic != cover || stretch != cover ||
            fit != cover) {
            return 0;
        }
        const metal::PhysicalRenderSize noUpscale = metal::physicalRenderSize(
            plan,
            {
                .backingWidth = 7680,
                .backingHeight = 4320,
                .quality = metal::PhysicalRenderQuality::balanced,
            },
            metal::PresentationScaling::aspectFill
        );
        if (noUpscale != metal::PhysicalRenderSize{1920, 1080}) return 0;
        const metal::PhysicalRenderSize power = metal::physicalRenderSize(
            plan,
            {
                .backingWidth = 2560,
                .backingHeight = 1664,
                .quality = metal::PhysicalRenderQuality::powerSaving,
            },
            metal::PresentationScaling::aspectFill
        );
        if (power != metal::PhysicalRenderSize{960, 540}) return 0;

        const FramePlan physical = metal::withPhysicalRenderSize(plan, cover);
        if (physical.width != 1920 || physical.height != 1080 ||
            physical.camera.orthogonalProjectionWidth != 3840 ||
            physical.camera.orthogonalProjectionHeight != 2160 ||
            physical.framebuffers.size() != 2 ||
            physical.framebuffers[0].width != 1920 ||
            physical.framebuffers[0].height != 1080 ||
            physical.framebuffers[1].width != 480 ||
            physical.framebuffers[1].height != 270 ||
            plan.width != 3840 || plan.height != 2160 ||
            plan.framebuffers[0].width != 3840 ||
            plan.framebuffers[1].width != 960) {
            return 0;
        }
        return 1;
    } catch (...) {
        return 0;
    }
}
extern "C" int we_scene_metal_test_render_text(uint8_t* rgba,size_t length,size_t* count){
 try{if(!rgba||length!=8*8*4||!count)return 0;metal::Device device;metal::TextCoverageRenderer renderer;auto s=device.activate();
 auto fb=s.createFramebuffer(metal::PixelFormat::rgba8,8,8,metal::TextureWrap::clampToEdge);s.clear(fb,{0.2F,0.4F,0.6F,1.0F},false);
 text::RasterizedText t{.width=2,.height=2,.bytesPerRow=2,.coverage={255,0,128,255}};
 std::array<float,16> m={0.5F,0,0,0, 0,-0.5F,0,0, 0,0,1,0, -0.5F,0.5F,0,1};
 renderer.draw(s,fb,t,{.modelViewProjection=m,.color={1,0,0,0.5F}});renderer.draw(s,fb,t,{.modelViewProjection=m,.color={1,0,0,0.5F}});
 *count=renderer.cachedTextureCount();s.readRGBA8(fb,std::span<uint8_t>(rgba,length));renderer.release(s);s.destroyFramebuffer(fb);return 1;}catch(...){return 0;}}
extern "C" int we_scene_metal_test_text_cache_bound(size_t updates,size_t* count){
 try{if(!count)return 0;metal::Device device;metal::TextCoverageRenderer renderer;auto s=device.activate();auto fb=s.createFramebuffer(metal::PixelFormat::rgba8,2,2,metal::TextureWrap::clampToEdge);
 std::array<float,16> m={1,0,0,0,0,1,0,0,0,0,1,0,-1,1,0,1};
 for(size_t i=0;i<updates;i++){text::RasterizedText t{.width=1,.height=1,.bytesPerRow=1,.coverage={uint8_t(i)}};renderer.draw(s,fb,t,{.modelViewProjection=m});}
 *count=renderer.cachedTextureCount();renderer.release(s);s.destroyFramebuffer(fb);return 1;}catch(...){return 0;}}
extern "C" int we_scene_metal_test_render_text_orientation(uint8_t* rgba,size_t length){
 try{if(!rgba||length!=2*2*4)return 0;metal::Device device;metal::TextCoverageRenderer renderer;auto s=device.activate();
 auto fb=s.createFramebuffer(metal::PixelFormat::rgba8,2,2,metal::TextureWrap::clampToEdge);s.clear(fb,{0,0,0,0},false);
 text::RasterizedText t{.width=2,.height=2,.bytesPerRow=2,.coverage={255,0,0,0}};
 std::array<float,16> m={1,0,0,0,0,1,0,0,0,0,1,0,-1,-1,0,1};
 renderer.draw(s,fb,t,{.modelViewProjection=m,.color={1,1,1,1}});s.readRGBA8(fb,std::span<uint8_t>(rgba,length));renderer.release(s);s.destroyFramebuffer(fb);return 1;}catch(...){return 0;}}
extern "C" int we_scene_metal_test_decode_video(const uint8_t* bytes,size_t length,const char* source,uint32_t* width,uint32_t* height){
 try{if(!bytes||length==0||!width||!height)return 0;void* decoder=metal::createVideoDecoder(bytes,length,source);if(!decoder)return 0;metal::VideoFrame frame;const bool decoded=metal::copyLatestVideoFrame(decoder,frame);metal::destroyVideoDecoder(decoder);if(!decoded||!frame.bytes||frame.bytesPerRow<static_cast<size_t>(frame.width)*4||frame.byteCount<frame.bytesPerRow*frame.height)return 0;*width=frame.width;*height=frame.height;return 1;}catch(...){return 0;}}

extern "C" int we_scene_metal_test_video_pipeline(
    const uint8_t* bytes,
    size_t length,
    const char* source,
    double targetTime,
    WESceneMetalTestVideoPipelineResult* result
) {
    try {
        if (bytes == nullptr || length == 0 || result == nullptr) return 0;
        TextureMipmap mipmap;
        mipmap.bytes.assign(bytes, bytes + length);
        Texture texture{
            .format = TextureFormat::argb8888,
            .flags = textureFlagVideo,
            .width = 16,
            .height = 16,
            .textureWidth = 16,
            .textureHeight = 16,
            .imageCount = 1,
            .isVideoMp4 = true,
            .images = {{.mipmaps = {std::move(mipmap)}}},
        };
        metal::Device device;
        auto session = device.activate();
        metal::AssetTextureResource resource = session.uploadTexture(
            texture, source == nullptr ? "test-video.mp4" : source
        );
        const std::uint64_t initialSerial =
            resource.lastUploadedVideoFrameSerial;
        session.requestVideoTextureFrame(resource, targetTime);

        metal::VideoFrame decoded;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        do {
            if (!metal::copyLatestVideoFrame(resource.videoDecoder, decoded)) {
                session.destroyTexture(resource);
                return 0;
            }
            if (decoded.serial != initialSerial) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);

        if (decoded.serial == initialSerial) {
            session.destroyTexture(resource);
            return 0;
        }
        const bool firstUpdate = session.updateVideoTexture(resource, 41);
        const bool secondUpdate = session.updateVideoTexture(resource, 41);
        *result = {
            .initial_serial = initialSerial,
            .decoded_serial = decoded.serial,
            .bytes_per_row = static_cast<std::uint32_t>(decoded.bytesPerRow),
            .same_frame_update_skipped = firstUpdate && !secondUpdate ? 1 : 0,
        };
        session.destroyTexture(resource);
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int we_scene_metal_test_presentation_transform(
    uint32_t source_width,
    uint32_t source_height,
    const WESceneMetalTestPresentationViewport* viewport,
    int scaling,
    double pointer_x,
    double pointer_y,
    WESceneMetalTestPresentationResult* result
) {
    try {
        if (viewport == nullptr || result == nullptr) return 0;
        metal::PresentationScaling mode;
        if (!presentationScaling(scaling, mode)) return 0;
        const auto transform = metal::makePresentationTransform(
            source_width, source_height, presentationViewport(*viewport), mode
        );
        const auto mapped = transform.map({pointer_x, pointer_y});
        const auto slice = transform.slice();
        *result = {
            .mapped_pointer_x = mapped.x,
            .mapped_pointer_y = mapped.y,
            .has_content = slice.hasContent ? 1 : 0,
            .source = presentationRect(slice.source),
            .destination = presentationRect(slice.destination),
        };
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int we_scene_metal_test_present_pattern(
    uint32_t source_width,
    uint32_t source_height,
    const WESceneMetalTestPresentationViewport* viewport,
    int scaling,
    uint8_t* rgba,
    size_t length
) {
    try {
        if (source_width < 4 || source_height < 2 || viewport == nullptr ||
            rgba == nullptr) {
            return 0;
        }
        metal::PresentationScaling mode;
        if (!presentationScaling(scaling, mode)) return 0;
        const auto nativeViewport = presentationViewport(*viewport);
        metal::validatePresentationViewport(nativeViewport);
        if (nativeViewport.drawableWidth >
            std::numeric_limits<size_t>::max() / 4 /
                nativeViewport.drawableHeight) {
            return 0;
        }
        const size_t expected = static_cast<size_t>(
            nativeViewport.drawableWidth
        ) * nativeViewport.drawableHeight * 4;
        if (length != expected) return 0;

        metal::Device device;
        auto session = device.activate();
        auto source = uploadedFramebuffer(
            session,
            source_width,
            source_height,
            presentationPattern(source_width, source_height)
        );
        auto destination = session.createFramebuffer(
            metal::PixelFormat::bgra8,
            nativeViewport.drawableWidth,
            nativeViewport.drawableHeight,
            metal::TextureWrap::clampToEdge
        );

        session.clear(destination, {0, 0, 0, 0}, false);

        const auto transform = metal::makePresentationTransform(
            source_width, source_height, nativeViewport, mode
        );
        const auto slice = transform.slice();
        metal::PresentationRenderer renderer;
        if (slice.hasContent) {
            renderer.draw(
                session,
                source,
                destination,
                slice,
                metal::TextureFilter::nearest
            );
        }
        session.readRGBA8(destination, std::span<uint8_t>(rgba, length));
        renderer.release(session);
        session.destroyFramebuffer(destination);
        session.destroyFramebuffer(source);
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int we_scene_metal_test_blit_presentation_slice(
    const WESceneMetalTestPresentationViewport* viewport,
    int scaling,
    uint8_t* rgba,
    size_t length
) {
    try {
        if (viewport == nullptr || rgba == nullptr) return 0;
        metal::PresentationScaling mode;
        if (!presentationScaling(scaling, mode)) return 0;
        const auto nativeViewport = presentationViewport(*viewport);
        metal::validatePresentationViewport(nativeViewport);
        if (nativeViewport.drawableWidth >
            std::numeric_limits<size_t>::max() / 4 /
                nativeViewport.drawableHeight) {
            return 0;
        }
        const size_t expected = static_cast<size_t>(
            nativeViewport.drawableWidth
        ) * nativeViewport.drawableHeight * 4;
        if (length != expected) return 0;

        metal::Device device;
        auto session = device.activate();
        std::vector<std::uint8_t> quadrants(4 * 4 * 4);
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const std::array<std::uint8_t, 4> color = y < 2
                    ? (x < 2
                           ? std::array<std::uint8_t, 4>{255, 0, 0, 255}
                           : std::array<std::uint8_t, 4>{0, 255, 0, 255})
                    : (x < 2
                           ? std::array<std::uint8_t, 4>{0, 0, 255, 255}
                           : std::array<std::uint8_t, 4>{255, 255, 0, 255});
                std::copy(
                    color.begin(), color.end(),
                    quadrants.begin() + (y * 4 + x) * 4
                );
            }
        }
        auto source = uploadedFramebuffer(
            session, 4, 4, std::move(quadrants)
        );
        auto destination = session.createFramebuffer(
            metal::PixelFormat::rgba8,
            nativeViewport.drawableWidth,
            nativeViewport.drawableHeight,
            metal::TextureWrap::clampToEdge
        );

        session.clear(destination, {0, 0, 0, 0}, false);

        const auto slice = metal::makePresentationTransform(
            4, 4, nativeViewport, mode
        ).slice();
        metal::PresentationRenderer renderer;
        if (slice.hasContent) {
            renderer.draw(
                session,
                source,
                destination,
                slice,
                metal::TextureFilter::nearest
            );
        }
        session.readRGBA8(destination, std::span<uint8_t>(rgba, length));
        renderer.release(session);
        session.destroyFramebuffer(destination);
        session.destroyFramebuffer(source);
        return 1;
    } catch (...) {
        return 0;
    }
}
