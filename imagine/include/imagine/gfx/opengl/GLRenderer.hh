#pragma once

/*  This file is part of Imagine.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Imagine.  If not, see <http://www.gnu.org/licenses/> */

#include <imagine/config/defs.hh>
#include <imagine/base/GLContext.hh>
#include <imagine/base/CustomEvent.hh>
#include <imagine/gfx/defs.hh>
#include <imagine/gfx/TextureSizeSupport.hh>
#include <imagine/gfx/RendererTask.hh>
#include <imagine/gfx/BasicEffect.hh>
#include <imagine/util/used.hh>
#include <memory>
#include <optional>
#include <string_view>
#ifdef CONFIG_BASE_GL_PLATFORM_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

namespace IG
{
class ApplicationContext;
}

namespace IG::Gfx
{

class RendererCommands;
class TextureSampler;
class GLSLProgram;
class RendererTask;

class DrawContextSupport
{
public:
	#ifdef CONFIG_GFX_OPENGL_ES
	void (* GL_APIENTRY glGenSamplers) (GLsizei count, GLuint* samplers){};
	void (* GL_APIENTRY glDeleteSamplers) (GLsizei count, const GLuint* samplers){};
	void (* GL_APIENTRY glBindSampler) (GLuint unit, GLuint sampler){};
	void (* GL_APIENTRY glSamplerParameteri) (GLuint sampler, GLenum pname, GLint param){};
	void (* GL_APIENTRY glTexStorage2D) (GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height){};
	GLvoid* (* GL_APIENTRY glMapBufferRange) (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access){};
	using UnmapBufferProto = GLboolean (* GL_APIENTRY) (GLenum target);
	UnmapBufferProto glUnmapBuffer{};
	//void (* GL_APIENTRY glDrawBuffers) (GLsizei size, const GLenum *bufs){};
	//void (* GL_APIENTRY glReadBuffer) (GLenum src){};
	void (* GL_APIENTRY glBufferStorage) (GLenum target, GLsizeiptr size, const void *data, GLbitfield flags){};
	void (* GL_APIENTRY glFlushMappedBufferRange)(GLenum target, GLintptr offset, GLsizeiptr length){};
	//void (* GL_APIENTRY glMemoryBarrier) (GLbitfield barriers){};
		#ifdef CONFIG_BASE_GL_PLATFORM_EGL
		// Prototypes based on EGL_KHR_fence_sync/EGL_KHR_wait_sync versions
		EGLSync (EGLAPIENTRY *eglCreateSync)(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list){};
		EGLBoolean (EGLAPIENTRY *eglDestroySync)(EGLDisplay dpy, EGLSync sync){};
		EGLint (EGLAPIENTRY *eglClientWaitSync)(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout){};
		//EGLint (EGLAPIENTRY *eglWaitSync)(EGLDisplay dpy, EGLSync sync, EGLint flags){};
		#else
		GLsync (* GL_APIENTRY glFenceSync) (GLenum condition, GLbitfield flags){};
		void (* GL_APIENTRY glDeleteSync) (GLsync sync){};
		GLenum (* GL_APIENTRY glClientWaitSync) (GLsync sync, GLbitfield flags, GLuint64 timeout){};
		//void (* GL_APIENTRY glWaitSync) (GLsync sync, GLbitfield flags, GLuint64 timeout){};
		#endif
	#else
	static void glGenSamplers(GLsizei count, GLuint* samplers) { ::glGenSamplers(count, samplers); };
	static void glDeleteSamplers(GLsizei count, const GLuint* samplers) { ::glDeleteSamplers(count,samplers); };
	static void glBindSampler(GLuint unit, GLuint sampler) { ::glBindSampler(unit, sampler); };
	static void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) { ::glSamplerParameteri(sampler, pname, param); };
	static void glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) { ::glTexStorage2D(target, levels, internalformat, width, height); };
	static GLvoid* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) { return ::glMapBufferRange(target, offset, length, access); };
	static GLboolean glUnmapBuffer(GLenum target) { return ::glUnmapBuffer(target); }
	static void glDrawBuffers(GLsizei size, const GLenum *bufs) { ::glDrawBuffers(size, bufs); };
	static void glReadBuffer(GLenum src) { ::glReadBuffer(src); };
	static void glBufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags) { ::glBufferStorage(target, size, data, flags); }
	static void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) { ::glFlushMappedBufferRange(target, offset, length); }
	//static void glMemoryBarrier(GLbitfield barriers) { ::glMemoryBarrier(barriers); }
		#ifdef CONFIG_BASE_GL_PLATFORM_EGL
		static EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list) { return ::eglCreateSync(dpy, type, attrib_list); }
		static EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync) { return ::eglDestroySync(dpy, sync); }
		static EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout) { return ::eglClientWaitSync(dpy, sync, flags, timeout); }
		//static EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags) { return ::eglWaitSync(dpy, sync, flags); }
		#else
		static GLsync glFenceSync(GLenum condition, GLbitfield flags) { return ::glFenceSync(condition, flags); }
		static void glDeleteSync(GLsync sync) { ::glDeleteSync(sync); }
		static GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) { return ::glClientWaitSync(sync, flags, timeout); }
		//static void glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) { ::glWaitSync(sync, flags, timeout); }
		#endif
	#endif
	#ifdef __ANDROID__
	void (GL_APIENTRYP glEGLImageTargetTexStorageEXT)(GLenum target, GLeglImageOES image, const GLint* attrib_list){};
	#endif
	#if defined CONFIG_GFX_OPENGL_DEBUG_CONTEXT && defined CONFIG_GFX_OPENGL_ES
	void GL_APIENTRY (*glDebugMessageCallback)(GLDEBUGPROCKHR callback, const void *userParam){};
	static constexpr auto DEBUG_OUTPUT = GL_DEBUG_OUTPUT_KHR;
	#elif defined CONFIG_GFX_OPENGL_DEBUG_CONTEXT
	void GL_APIENTRY (*glDebugMessageCallback)(GLDEBUGPROC callback, const void *userParam){};
	static constexpr auto DEBUG_OUTPUT = GL_DEBUG_OUTPUT;
	#endif
	#if defined CONFIG_GFX_OPENGL_ES && CONFIG_GFX_OPENGL_ES > 1
	static void generateMipmaps(GLenum target) { ::glGenerateMipmap(target); };
	#else
	using GenerateMipmapsProto = void (* GL_APIENTRY)(GLenum target);
	GenerateMipmapsProto generateMipmaps{}; // set via extensions
	#endif
	GLenum luminanceFormat = GL_LUMINANCE;
	GLenum luminanceInternalFormat = GL_LUMINANCE8;
	GLenum luminanceAlphaFormat = GL_LUMINANCE_ALPHA;
	GLenum luminanceAlphaInternalFormat = GL_LUMINANCE8_ALPHA8;
	GLenum alphaFormat = GL_ALPHA;
	GLenum alphaInternalFormat = GL_ALPHA8;
	TextureSizeSupport textureSizeSupport{};
	//bool hasMemoryBarrier = false;
	IG_UseMemberIfOrConstant((bool)Config::Gfx::OPENGL_ES, bool, true, hasBGRPixels){};
	bool hasVBOFuncs{};
	bool hasTextureSwizzle{};
	bool hasUnpackRowLength = !Config::Gfx::OPENGL_ES;
	bool hasSamplerObjects = !Config::Gfx::OPENGL_ES;
	bool hasImmutableTexStorage{};
	bool hasPBOFuncs{};
	bool useLegacyGLSL = Config::Gfx::OPENGL_ES;
	IG_UseMemberIf(Config::Gfx::OPENGL_DEBUG_CONTEXT, bool, hasDebugOutput){};
	IG_UseMemberIf(!Config::Gfx::OPENGL_ES, bool, hasBufferStorage){};
	IG_UseMemberIf(Config::envIsAndroid, bool, hasEGLImages){};
	IG_UseMemberIf(Config::Gfx::OPENGL_TEXTURE_TARGET_EXTERNAL, bool, hasExternalEGLImages){};
	IG_UseMemberIf(Config::Gfx::OPENGL_FIXED_FUNCTION_PIPELINE, bool, useFixedFunctionPipeline){true};
	bool hasSrgbWriteControl{};
	bool isConfigured{};

	bool hasDrawReadBuffers() const;
	bool hasSyncFences() const;
	bool hasServerWaitSync() const;
	bool hasEGLTextureStorage() const;
	bool hasImmutableBufferStorage() const;
	bool hasMemoryBarriers() const;
	GLsync fenceSync(GLDisplay dpy);
	void deleteSync(GLDisplay dpy, GLsync sync);
	GLenum clientWaitSync(GLDisplay dpy, GLsync sync, GLbitfield flags, GLuint64 timeout);
	void waitSync(GLDisplay dpy, GLsync sync);
	void setGLDebugOutput(bool on);
};

class GLRenderer
{
public:
	DrawContextSupport support{};
	[[no_unique_address]] GLManager glManager;
	RendererTask mainTask;
	BasicEffect basicEffect_{};
	CustomEvent releaseShaderCompilerEvent{CustomEvent::NullInit{}};

	GLRenderer(ApplicationContext);
	GLDisplay glDisplay() const;
	bool makeWindowDrawable(RendererTask &task, Window &, GLBufferConfig, GLColorSpace);
	int toSwapInterval(const Window &win, PresentMode mode) const;

protected:
	void addEventHandlers(ApplicationContext, RendererTask &);
	std::optional<GLBufferConfig> makeGLBufferConfig(ApplicationContext, PixelFormat, const Window * = {});
	void setCurrentDrawable(GLDisplay, GLContext, Drawable);
	void setupNonPow2Textures();
	void setupNonPow2MipmapTextures();
	void setupNonPow2MipmapRepeatTextures();
	void setupBGRPixelSupport();
	void setupFBOFuncs(bool &useFBOFuncs);
	void setupTextureSwizzle();
	void setupImmutableTexStorage(bool extSuffix);
	void setupRGFormats();
	void setupSamplerObjects();
	void setupPBO();
	void setupSpecifyDrawReadBuffers();
	void setupUnmapBufferFunc();
	void setupImmutableBufferStorage();
	void setupMemoryBarrier();
	void setupFenceSync();
	void setupAppleFenceSync();
	void setupEglFenceSync(std::string_view eglExtenstionStr);
	void checkExtensionString(std::string_view extStr, bool &useFBOFuncs);
	void checkFullExtensionString(const char *fullExtStr);
	bool attachWindow(Window &, GLBufferConfig, GLColorSpace);
	NativeWindowFormat nativeWindowFormat(GLBufferConfig) const;
	bool initBasicEffect();
};

using RendererImpl = GLRenderer;

}
