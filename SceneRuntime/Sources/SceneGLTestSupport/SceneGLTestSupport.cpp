#include <SceneGLTestSupport/SceneGLTestSupport.h>
#include <SceneGL/FramebufferPlanRequirements.hpp>
#include <SceneGL/FramePlanExecutor.hpp>
#include "../SceneGL/SceneGLDevice.hpp"
#include "../SceneGL/SceneGLPresentation.hpp"
#include "../SceneGL/SceneVideoDecoder.hpp"
#include "../SceneGL/TextCoverageRenderer.hpp"
#include <algorithm>
#include <limits>
#include <span>
using namespace we::scene;

namespace {

gl::PresentationViewport presentationViewport(
    const WESceneGLTestPresentationViewport& viewport
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

bool presentationScaling(int value, gl::PresentationScaling& scaling) {
    switch (value) {
        case 0:
            scaling = gl::PresentationScaling::stretch;
            return true;
        case 1:
            scaling = gl::PresentationScaling::aspectFit;
            return true;
        case 2:
            scaling = gl::PresentationScaling::aspectFill;
            return true;
        case 3:
            scaling = gl::PresentationScaling::automatic;
            return true;
        default:
            return false;
    }
}

WESceneGLTestPresentationRect presentationRect(
    const gl::PresentationRect& rect
) {
    return {
        .x = rect.x,
        .y = rect.y,
        .width = rect.width,
        .height = rect.height,
    };
}

void paintPresentationPattern(
    std::uint32_t width,
    std::uint32_t height
) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // The scene framebuffer stores the user-visible top row at GL y=0.
    glScissor(0, 0, 1, static_cast<GLsizei>(height));
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(
        static_cast<GLint>(width - 1),
        0,
        1,
        static_cast<GLsizei>(height)
    );
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(1, 0, static_cast<GLsizei>(width - 2), 1);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(
        1,
        static_cast<GLint>(height - 1),
        static_cast<GLsizei>(width - 2),
        1
    );
    glClearColor(1, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
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
extern "C" int we_scene_gl_test_framebuffer_plan_requirements(void) {
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

        const gl::FramebufferPlanRequirements requirements =
            gl::analyzeFramebufferPlanRequirements(plan);
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
        const gl::FramebufferPlanRequirements colorOnly =
            gl::analyzeFramebufferPlanRequirements(plan);
        if (colorOnly.active.size() != 1 ||
            colorOnly.active.at("output").requiresDepthAttachment) {
            return 0;
        }
        plan.operations.emplace_back(FrameRenderPass{
            .depthWrite = DepthMode::enabled,
            .destination = framebufferRequirementResource("output"),
        });
        const gl::FramebufferPlanRequirements depthChanged =
            gl::analyzeFramebufferPlanRequirements(plan);
        return depthChanged.active.size() == 1 &&
                depthChanged.active.at("output").requiresDepthAttachment
            ? 1 : 0;
    } catch (...) {
        return 0;
    }
}
extern "C" int we_scene_gl_test_physical_render_policy(void) {
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
        const gl::PhysicalRenderTarget balancedTarget{
            .backingWidth = 2560,
            .backingHeight = 1664,
            .quality = gl::PhysicalRenderQuality::balanced,
        };
        const gl::PhysicalRenderSize cover = gl::physicalRenderSize(
            plan, balancedTarget, gl::PresentationScaling::aspectFill
        );
        const gl::PhysicalRenderSize automatic = gl::physicalRenderSize(
            plan, balancedTarget, gl::PresentationScaling::automatic
        );
        const gl::PhysicalRenderSize stretch = gl::physicalRenderSize(
            plan, balancedTarget, gl::PresentationScaling::stretch
        );
        const gl::PhysicalRenderSize fit = gl::physicalRenderSize(
            plan, balancedTarget, gl::PresentationScaling::aspectFit
        );
        if (cover != gl::PhysicalRenderSize{1920, 1080} ||
            automatic != cover || stretch != cover ||
            fit != cover) {
            return 0;
        }
        const gl::PhysicalRenderSize noUpscale = gl::physicalRenderSize(
            plan,
            {
                .backingWidth = 7680,
                .backingHeight = 4320,
                .quality = gl::PhysicalRenderQuality::balanced,
            },
            gl::PresentationScaling::aspectFill
        );
        if (noUpscale != gl::PhysicalRenderSize{1920, 1080}) return 0;
        const gl::PhysicalRenderSize power = gl::physicalRenderSize(
            plan,
            {
                .backingWidth = 2560,
                .backingHeight = 1664,
                .quality = gl::PhysicalRenderQuality::powerSaving,
            },
            gl::PresentationScaling::aspectFill
        );
        if (power != gl::PhysicalRenderSize{960, 540}) return 0;

        const FramePlan physical = gl::withPhysicalRenderSize(plan, cover);
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
extern "C" int we_scene_gl_test_render_text(uint8_t* rgba,size_t length,size_t* count){
 try{if(!rgba||length!=8*8*4||!count)return 0;gl::Device device;gl::TextCoverageRenderer renderer;auto s=device.activate();
 auto fb=s.createFramebuffer(gl::PixelFormat::rgba8,8,8,gl::TextureWrap::clampToEdge);glBindFramebuffer(GL_FRAMEBUFFER,fb.framebuffer);glClearColor(0.2F,0.4F,0.6F,1);glClear(GL_COLOR_BUFFER_BIT);
 text::RasterizedText t{.width=2,.height=2,.bytesPerRow=2,.coverage={255,0,128,255}};
 std::array<float,16> m={0.5F,0,0,0, 0,-0.5F,0,0, 0,0,1,0, -0.5F,0.5F,0,1};
 renderer.draw(s,fb,t,{.modelViewProjection=m,.color={1,0,0,0.5F}});renderer.draw(s,fb,t,{.modelViewProjection=m,.color={1,0,0,0.5F}});
 *count=renderer.cachedTextureCount();s.readRGBA8(fb,std::span<uint8_t>(rgba,length));renderer.release(s);s.destroyFramebuffer(fb);return 1;}catch(...){return 0;}}
extern "C" int we_scene_gl_test_text_cache_bound(size_t updates,size_t* count){
 try{if(!count)return 0;gl::Device device;gl::TextCoverageRenderer renderer;auto s=device.activate();auto fb=s.createFramebuffer(gl::PixelFormat::rgba8,2,2,gl::TextureWrap::clampToEdge);
 std::array<float,16> m={1,0,0,0,0,1,0,0,0,0,1,0,-1,1,0,1};
 for(size_t i=0;i<updates;i++){text::RasterizedText t{.width=1,.height=1,.bytesPerRow=1,.coverage={uint8_t(i)}};renderer.draw(s,fb,t,{.modelViewProjection=m});}
 *count=renderer.cachedTextureCount();renderer.release(s);s.destroyFramebuffer(fb);return 1;}catch(...){return 0;}}
extern "C" int we_scene_gl_test_render_text_orientation(uint8_t* rgba,size_t length){
 try{if(!rgba||length!=2*2*4)return 0;gl::Device device;gl::TextCoverageRenderer renderer;auto s=device.activate();
 auto fb=s.createFramebuffer(gl::PixelFormat::rgba8,2,2,gl::TextureWrap::clampToEdge);glBindFramebuffer(GL_FRAMEBUFFER,fb.framebuffer);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
 text::RasterizedText t{.width=2,.height=2,.bytesPerRow=2,.coverage={255,0,0,0}};
 std::array<float,16> m={1,0,0,0,0,1,0,0,0,0,1,0,-1,-1,0,1};
 renderer.draw(s,fb,t,{.modelViewProjection=m,.color={1,1,1,1}});s.readRGBA8(fb,std::span<uint8_t>(rgba,length));renderer.release(s);s.destroyFramebuffer(fb);return 1;}catch(...){return 0;}}
extern "C" int we_scene_gl_test_current_particle_objects(WESceneGLTestParticleObjects* objects){
 try{if(!objects||CGLGetCurrentContext()==nullptr)return 0;GLint vao=0,vbo=0,ebo=0;glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&vbo);glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&ebo);if(vao<=0||vbo<=0||ebo<=0)return 0;objects->vertex_array=static_cast<uint32_t>(vao);objects->vertex_buffer=static_cast<uint32_t>(vbo);objects->element_buffer=static_cast<uint32_t>(ebo);return glGetError()==GL_NO_ERROR?1:0;}catch(...){return 0;}}
extern "C" int we_scene_gl_test_particle_objects_exist(const WESceneGLTestParticleObjects* objects,int* all_exist){
 try{if(!objects||!all_exist||CGLGetCurrentContext()==nullptr)return 0;*all_exist=glIsVertexArray(objects->vertex_array)==GL_TRUE&&glIsBuffer(objects->vertex_buffer)==GL_TRUE&&glIsBuffer(objects->element_buffer)==GL_TRUE?1:0;return glGetError()==GL_NO_ERROR?1:0;}catch(...){return 0;}}
extern "C" int we_scene_gl_test_particle_first_lifetime(const WESceneGLTestParticleObjects* objects,float* lifetime){
 try{if(!objects||!lifetime||CGLGetCurrentContext()==nullptr)return 0;GLint previous=0;glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&previous);glBindBuffer(GL_ARRAY_BUFFER,objects->vertex_buffer);GLint size=0;glGetBufferParameteriv(GL_ARRAY_BUFFER,GL_BUFFER_SIZE,&size);if(size<static_cast<GLint>(15*sizeof(float))){glBindBuffer(GL_ARRAY_BUFFER,static_cast<GLuint>(previous));return 0;}float value=0.0F;glGetBufferSubData(GL_ARRAY_BUFFER,14*sizeof(float),sizeof(float),&value);glBindBuffer(GL_ARRAY_BUFFER,static_cast<GLuint>(previous));if(glGetError()!=GL_NO_ERROR)return 0;*lifetime=value;return 1;}catch(...){return 0;}}
extern "C" int we_scene_gl_test_decode_video(const uint8_t* bytes,size_t length,const char* source,uint32_t* width,uint32_t* height){
 try{if(!bytes||length==0||!width||!height)return 0;void* decoder=gl::createVideoDecoder(bytes,length,source);if(!decoder)return 0;gl::VideoFrameRGBA8 frame;const bool decoded=gl::decodeVideoFrame(decoder,0.0,frame);gl::destroyVideoDecoder(decoder);if(!decoded||!frame.bytes||frame.byteCount!=static_cast<size_t>(frame.width)*frame.height*4)return 0;*width=frame.width;*height=frame.height;return 1;}catch(...){return 0;}}

extern "C" int we_scene_gl_test_presentation_transform(
    uint32_t source_width,
    uint32_t source_height,
    const WESceneGLTestPresentationViewport* viewport,
    int scaling,
    double pointer_x,
    double pointer_y,
    WESceneGLTestPresentationResult* result
) {
    try {
        if (viewport == nullptr || result == nullptr) return 0;
        gl::PresentationScaling mode;
        if (!presentationScaling(scaling, mode)) return 0;
        const auto transform = gl::makePresentationTransform(
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

extern "C" int we_scene_gl_test_present_pattern(
    uint32_t source_width,
    uint32_t source_height,
    const WESceneGLTestPresentationViewport* viewport,
    int scaling,
    uint8_t* rgba,
    size_t length
) {
    try {
        if (source_width < 4 || source_height < 2 || viewport == nullptr ||
            rgba == nullptr) {
            return 0;
        }
        gl::PresentationScaling mode;
        if (!presentationScaling(scaling, mode)) return 0;
        const auto nativeViewport = presentationViewport(*viewport);
        gl::validatePresentationViewport(nativeViewport);
        if (nativeViewport.drawableWidth >
            std::numeric_limits<size_t>::max() / 4 /
                nativeViewport.drawableHeight) {
            return 0;
        }
        const size_t expected = static_cast<size_t>(
            nativeViewport.drawableWidth
        ) * nativeViewport.drawableHeight * 4;
        if (length != expected) return 0;

        gl::Device device;
        auto session = device.activate();
        auto source = session.createFramebuffer(
            gl::PixelFormat::rgba8,
            source_width,
            source_height,
            gl::TextureWrap::clampToEdge
        );
        auto destination = session.createFramebuffer(
            gl::PixelFormat::rgba8,
            nativeViewport.drawableWidth,
            nativeViewport.drawableHeight,
            gl::TextureWrap::clampToEdge
        );

        glBindFramebuffer(GL_FRAMEBUFFER, source.framebuffer);
        paintPresentationPattern(source_width, source_height);
        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

        const auto transform = gl::makePresentationTransform(
            source_width, source_height, nativeViewport, mode
        );
        const auto slice = transform.slice();
        gl::PresentationRenderer renderer;
        if (slice.hasContent) {
            renderer.draw(
                session,
                source,
                destination.framebuffer,
                GL_COLOR_ATTACHMENT0,
                nativeViewport.drawableWidth,
                nativeViewport.drawableHeight,
                slice,
                GL_NEAREST
            );
        }
        session.checkError(
            gl::ErrorCode::draw,
            "testing cover presentation"
        );
        session.readRGBA8(destination, std::span<uint8_t>(rgba, length));
        renderer.release(session);
        session.destroyFramebuffer(destination);
        session.destroyFramebuffer(source);
        return 1;
    } catch (...) {
        return 0;
    }
}

extern "C" int we_scene_gl_test_blit_presentation_slice(
    const WESceneGLTestPresentationViewport* viewport,
    int scaling,
    uint8_t* rgba,
    size_t length
) {
    try {
        if (viewport == nullptr || rgba == nullptr) return 0;
        gl::PresentationScaling mode;
        if (!presentationScaling(scaling, mode)) return 0;
        const auto nativeViewport = presentationViewport(*viewport);
        gl::validatePresentationViewport(nativeViewport);
        if (nativeViewport.drawableWidth >
            std::numeric_limits<size_t>::max() / 4 /
                nativeViewport.drawableHeight) {
            return 0;
        }
        const size_t expected = static_cast<size_t>(
            nativeViewport.drawableWidth
        ) * nativeViewport.drawableHeight * 4;
        if (length != expected) return 0;

        gl::Device device;
        auto session = device.activate();
        auto source = session.createFramebuffer(
            gl::PixelFormat::rgba8, 4, 4, gl::TextureWrap::clampToEdge
        );
        auto destination = session.createFramebuffer(
            gl::PixelFormat::rgba8,
            nativeViewport.drawableWidth,
            nativeViewport.drawableHeight,
            gl::TextureWrap::clampToEdge
        );

        glBindFramebuffer(GL_FRAMEBUFFER, source.framebuffer);
        glEnable(GL_SCISSOR_TEST);
        // Scene output keeps Wallpaper Engine's top-down rows at the bottom of
        // its OpenGL framebuffer. These four quadrants therefore represent a
        // user-visible red/green top row and blue/yellow bottom row.
        glScissor(0, 0, 2, 2);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(2, 0, 2, 2);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(0, 2, 2, 2);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(2, 2, 2, 2);
        glClearColor(1, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);

        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        const auto slice = gl::makePresentationTransform(
            4, 4, nativeViewport, mode
        ).slice();
        gl::PresentationRenderer renderer;
        if (slice.hasContent) {
            renderer.draw(
                session,
                source,
                destination.framebuffer,
                GL_COLOR_ATTACHMENT0,
                nativeViewport.drawableWidth,
                nativeViewport.drawableHeight,
                slice,
                GL_NEAREST
            );
        }
        session.checkError(gl::ErrorCode::draw, "testing presentation slicing");
        session.readRGBA8(destination, std::span<uint8_t>(rgba, length));
        renderer.release(session);
        session.destroyFramebuffer(destination);
        session.destroyFramebuffer(source);
        return 1;
    } catch (...) {
        return 0;
    }
}
