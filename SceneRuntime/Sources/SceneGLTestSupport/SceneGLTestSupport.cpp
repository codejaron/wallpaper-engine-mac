#include <SceneGLTestSupport/SceneGLTestSupport.h>
#include <SceneGL/FramePlanExecutor.hpp>
#include "../SceneGL/SceneGLDevice.hpp"
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

}  // namespace
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
        glScissor(0, 0, 2, 4);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(2, 0, 2, 4);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);

        glBindFramebuffer(GL_FRAMEBUFFER, destination.framebuffer);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        const auto slice = gl::makePresentationTransform(
            4, 4, nativeViewport, mode
        ).slice();
        if (slice.hasContent) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, source.framebuffer);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination.framebuffer);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glBlitFramebuffer(
                static_cast<GLint>(slice.source.x),
                static_cast<GLint>(slice.source.y),
                static_cast<GLint>(slice.source.x + slice.source.width),
                static_cast<GLint>(slice.source.y + slice.source.height),
                static_cast<GLint>(slice.destination.x),
                static_cast<GLint>(slice.destination.y),
                static_cast<GLint>(
                    slice.destination.x + slice.destination.width
                ),
                static_cast<GLint>(
                    slice.destination.y + slice.destination.height
                ),
                GL_COLOR_BUFFER_BIT,
                GL_NEAREST
            );
        }
        session.checkError(gl::ErrorCode::draw, "testing presentation slicing");
        session.readRGBA8(destination, std::span<uint8_t>(rgba, length));
        session.destroyFramebuffer(destination);
        session.destroyFramebuffer(source);
        return 1;
    } catch (...) {
        return 0;
    }
}
