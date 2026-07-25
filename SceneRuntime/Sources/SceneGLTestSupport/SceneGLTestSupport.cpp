#include <SceneGLTestSupport/SceneGLTestSupport.h>
#include "../SceneGL/SceneGLDevice.hpp"
#include "../SceneGL/TextCoverageRenderer.hpp"
#include <algorithm>
#include <span>
using namespace we::scene;
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
