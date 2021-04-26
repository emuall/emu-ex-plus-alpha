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

#define LOGTAG "EGL"
#include <imagine/base/GLContext.hh>
#include <imagine/base/EGLContextBase.hh>
#include <imagine/base/Window.hh>
#include <imagine/thread/Thread.hh>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <imagine/util/egl.hh>
#include <imagine/util/container/ArrayList.hh>

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif

#ifndef EGL_NO_CONFIG_KHR
#define EGL_NO_CONFIG_KHR ((EGLConfig)0)
#endif

#ifndef EGL_CONTEXT_OPENGL_NO_ERROR_KHR
#define EGL_CONTEXT_OPENGL_NO_ERROR_KHR 0x31B3
#endif

#ifndef EGL_TRIPLE_BUFFER_NV
#define EGL_TRIPLE_BUFFER_NV 0x3230
#endif

namespace Base
{

// Android reference counts the EGL display internally,
// use refCount on other platforms to simulate this
static constexpr bool HAS_DISPLAY_REF_COUNT = Config::envIsAndroid;
static uint32_t refCount = 0;

static constexpr bool CAN_USE_DEBUG_CONTEXT = !Config::MACHINE_IS_PANDORA;
static uint8_t eglVersion = 0;
static bool supportsSurfaceless = false;
static bool supportsNoConfig = false;
static bool supportsNoError = false;
static bool supportsTripleBufferSurfaces = false;
static bool hasDummyPbuffConfig = false;
static EGLConfig dummyPbuffConfig{};
using EGLAttrList = StaticArrayList<int, 24>;
using EGLContextAttrList = StaticArrayList<int, 16>;

static EGLAttrList glConfigAttrsToEGLAttrs(int renderableType, GLBufferConfigAttributes attr)
{
	EGLAttrList list;
	// don't accept slow configs
	list.push_back(EGL_CONFIG_CAVEAT);
	list.push_back(EGL_NONE);
	switch(attr.pixelFormat().id())
	{
		bdefault:
			bug_unreachable("format id == %d", attr.pixelFormat().id());
		bcase PIXEL_NONE:
			// don't set any color bits
		bcase PIXEL_RGB565:
			list.push_back(EGL_BUFFER_SIZE);
			list.push_back(16);
		bcase PIXEL_RGB888:
			list.push_back(EGL_RED_SIZE);
			list.push_back(8);
			list.push_back(EGL_GREEN_SIZE);
			list.push_back(8);
			list.push_back(EGL_BLUE_SIZE);
			list.push_back(8);
		bcase PIXEL_RGBX8888:
			list.push_back(EGL_RED_SIZE);
			list.push_back(8);
			list.push_back(EGL_GREEN_SIZE);
			list.push_back(8);
			list.push_back(EGL_BLUE_SIZE);
			list.push_back(8);
			list.push_back(EGL_BUFFER_SIZE);
			list.push_back(32);
		bcase PIXEL_RGBA8888:
			list.push_back(EGL_RED_SIZE);
			list.push_back(8);
			list.push_back(EGL_GREEN_SIZE);
			list.push_back(8);
			list.push_back(EGL_BLUE_SIZE);
			list.push_back(8);
			list.push_back(EGL_ALPHA_SIZE);
			list.push_back(8);
	}
	if(renderableType)
	{
		list.push_back(EGL_RENDERABLE_TYPE);
		list.push_back(renderableType);
	}
	list.push_back(EGL_NONE);
	return list;
}

static EGLContextAttrList glContextAttrsToEGLAttrs(GLContextAttributes attr)
{
	EGLContextAttrList list;

	if(attr.openGLESAPI())
	{
		list.push_back(EGL_CONTEXT_CLIENT_VERSION);
		list.push_back(attr.majorVersion());
		//logDMsg("using OpenGL ES client version:%d", attr.majorVersion());
	}
	else
	{
		list.push_back(EGL_CONTEXT_MAJOR_VERSION_KHR);
		list.push_back(attr.majorVersion());
		list.push_back(EGL_CONTEXT_MINOR_VERSION_KHR);
		list.push_back(attr.minorVersion());

		if(attr.majorVersion() > 3
			|| (attr.majorVersion() == 3 && attr.minorVersion() >= 2))
		{
			list.push_back(EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR);
			list.push_back(EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR);
		}
	}

	if(CAN_USE_DEBUG_CONTEXT && attr.debug())
	{
		list.push_back(EGL_CONTEXT_FLAGS_KHR);
		list.push_back(EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR);
	}

	if(!attr.debug() && supportsNoError)
	{
		list.push_back(EGL_CONTEXT_OPENGL_NO_ERROR_KHR);
		list.push_back(EGL_TRUE);
	}

	list.push_back(EGL_NONE);
	return list;
}

// GLContext

std::optional<EGLConfig> EGLContextBase::chooseConfig(EGLDisplay display, int renderableType, GLBufferConfigAttributes attr)
{
	EGLConfig config;
	EGLint configs = 0;
	{
		auto eglAttr = glConfigAttrsToEGLAttrs(renderableType, attr);
		eglChooseConfig(display, &eglAttr[0], &config, 1, &configs);
	}
	if(!configs)
	{
		logErr("no EGL configs found, retrying with no color bits set");
		attr.setPixelFormat(IG::PIXEL_NONE);
		auto eglAttr = glConfigAttrsToEGLAttrs(renderableType, attr);
		eglChooseConfig(display, &eglAttr[0], &config, 1, &configs);
	}
	if(!configs)
	{
		logErr("no usable EGL configs found with renderable type:%s", eglRenderableTypeToStr(renderableType));
		return {};
	}
	if(Config::DEBUG_BUILD)
		printEGLConf(display, config);
	return config;
}

void *GLContext::procAddress(const char *funcName)
{
	//logDMsg("getting proc address for:%s", funcName);
	return (void*)eglGetProcAddress(funcName);
}

EGLContextBase::EGLContextBase(EGLDisplay display, GLContextAttributes attr, EGLBufferConfig config, EGLContext shareContext, IG::ErrorCode &ec)
{
	EGLConfig glConfig = supportsNoConfig ? EGL_NO_CONFIG_KHR : config.glConfig;
	logMsg("making context with version: %d.%d config:0x%llX share context:%p",
		attr.majorVersion(), attr.minorVersion(), (long long)glConfig, shareContext);
	context = eglCreateContext(display, glConfig, shareContext, &glContextAttrsToEGLAttrs(attr)[0]);
	if(context == EGL_NO_CONTEXT)
	{
		if(attr.debug())
		{
			logMsg("retrying without debug bit");
			attr.setDebug(false);
			context = eglCreateContext(display, glConfig, shareContext, &glContextAttrsToEGLAttrs(attr)[0]);
		}
		if(context == EGL_NO_CONTEXT)
		{
			if(Config::DEBUG_BUILD)
				logErr("error creating context: 0x%X", (int)eglGetError());
			ec = {EINVAL};
			return;
		}
	}
	// Ignore surfaceless context support when using GL versions below 3.0 due to possible driver issues,
	// such as on Tegra 3 GPUs
	if(attr.majorVersion() <= 2 || !supportsSurfaceless)
	{
		if(!hasDummyPbuffConfig)
		{
			logMsg("surfaceless context not supported:%s, saving config for dummy pbuffer",
				supportsSurfaceless ? "context version below 3.0" : "missing extension");
			dummyPbuffConfig = config.glConfig;
			hasDummyPbuffConfig = true;
		}
		else
		{
			// all contexts must use same config if surfaceless isn't supported
			assert(dummyPbuffConfig == config.glConfig);
		}
	}
	//logDMsg("created context:%p", context);
	ec = {};
}

void EGLContextBase::setCurrentContext(EGLDisplay display, EGLContext context, GLDrawable surface)
{
	assert(display != EGL_NO_DISPLAY);
	if(Config::DEBUG_BUILD)
	{
		//logDMsg("called setCurrentContext() with current context:%p surface:%p", eglGetCurrentContext(), eglGetCurrentSurface(EGL_DRAW));
	}
	if(context == EGL_NO_CONTEXT)
	{
		if(Config::DEBUG_BUILD)
		{
			logDMsg("setting no context current on thread:0x%lx", IG::thisThreadID<long>());
		}
		assert(!surface);
		if(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_FALSE)
		{
			if(Config::DEBUG_BUILD)
				logErr("error:%s setting no context current", GLDisplay::errorString(eglGetError()));
		}
	}
	else if(surface)
	{
		assert(context != EGL_NO_CONTEXT);
		if(Config::DEBUG_BUILD)
		{
			logDMsg("setting surface %p current on context:%p thread:0x%lx", (EGLSurface)surface, context, IG::thisThreadID<long>());
		}
		if(eglMakeCurrent(display, surface, surface, context) == EGL_FALSE)
		{
			if(Config::DEBUG_BUILD)
				logErr("error:%s setting surface current", GLDisplay::errorString(eglGetError()));
		}
	}
	else
	{
		assert(context != EGL_NO_CONTEXT);
		if(hasDummyPbuffConfig)
		{
			if(Config::DEBUG_BUILD)
			{
				logDMsg("setting dummy pbuffer surface current on context:%p thread:0x%lx", context, IG::thisThreadID<long>());
			}
			auto dummyPbuff = makeDummyPbuffer(display, dummyPbuffConfig);
			if(dummyPbuff == EGL_NO_SURFACE)
			{
				if(Config::DEBUG_BUILD)
					logErr("error:%s making dummy pbuffer", GLDisplay::errorString(eglGetError()));
			}
			if(eglMakeCurrent(display, dummyPbuff, dummyPbuff, context) == EGL_FALSE)
			{
				if(Config::DEBUG_BUILD)
					logErr("error:%s setting dummy pbuffer current", GLDisplay::errorString(eglGetError()));
			}
			eglDestroySurface(display, dummyPbuff);
		}
		else
		{
			if(Config::DEBUG_BUILD)
			{
				logDMsg("setting no surface current on context:%p thread:0x%lx", context, IG::thisThreadID<long>());
			}
			if(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_FALSE)
			{
				if(Config::DEBUG_BUILD)
					logErr("error:%s setting no surface current", GLDisplay::errorString(eglGetError()));
			}
		}
	}
}

void GLContext::setDrawable(GLDisplay display, GLDrawable win)
{
	setDrawable(display, win, current(display));
}

void GLContext::setDrawable(GLDisplay display, GLDrawable win, GLContext cachedCurrentContext)
{
	setCurrentContext(display, cachedCurrentContext.context, win);
}

GLContext GLContext::current(GLDisplay display)
{
	GLContext c;
	c.context = eglGetCurrentContext();
	return c;
}

bool GLContext::hasCurrentDrawable(GLDisplay display, GLDrawable drawable)
{
	return eglGetCurrentSurface(EGL_DRAW) == (EGLSurface)drawable;
}

bool GLContext::hasCurrentDrawable(GLDisplay display)
{
	return eglGetCurrentSurface(EGL_DRAW) != EGL_NO_SURFACE;
}

void EGLContextBase::swapBuffers(EGLDisplay display, GLDrawable surface)
{
	assert(display != EGL_NO_DISPLAY);
	assert(surface);
	if(eglSwapBuffers(display, surface) == EGL_FALSE)
	{
		if(Config::DEBUG_BUILD)
			logErr("error 0x%X swapping buffers for surface:%p", eglGetError(), (EGLSurface)surface);
	}
}

GLContext::operator bool() const
{
	return context != EGL_NO_CONTEXT;
}

bool GLContext::operator ==(GLContext const &rhs) const
{
	return context == rhs.context;
}

void EGLContextBase::deinit(EGLDisplay display)
{
	if(context != EGL_NO_CONTEXT)
	{
		logMsg("destroying context:%p", context);
		eglDestroyContext(display, context);
		context = EGL_NO_CONTEXT;
	}
}

bool GLContext::supportsNoConfig()
{
	return Base::supportsNoConfig;
}

NativeGLContext GLContext::nativeObject()
{
	return context;
}

// GLDisplay

std::pair<IG::ErrorCode, GLDisplay> GLDisplay::makeDefault(Base::NativeDisplayConnection ctx)
{
	auto display = getDefault(ctx);
	auto ec = initDisplay(display.display);
	return {ec, display};
}

std::pair<IG::ErrorCode, GLDisplay> GLDisplay::makeDefault(Base::NativeDisplayConnection ctx, GL::API api)
{
	if(!bindAPI(api))
	{
		logErr("error binding requested API");
		return {{EINVAL}, {}};
	}
	return makeDefault(ctx);
}

GLDisplay GLDisplay::getDefault(Base::NativeDisplayConnection ctx, GL::API api)
{
	if(!bindAPI(api))
	{
		logErr("error binding requested API");
		return {};
	}
	return getDefault(ctx);
}

static void printFeatures(bool supportsSurfaceless, bool supportsNoConfig, bool supportsNoError, bool supportsTripleBufferSurfaces)
{
	if(!Config::DEBUG_BUILD)
		return;
	std::string featuresStr{};

	if(supportsSurfaceless)
	{
		featuresStr.append(" [Surfaceless]");
	}
	if(supportsNoConfig)
	{
		featuresStr.append(" [No Config]");
	}
	if(supportsNoError)
	{
		featuresStr.append(" [No Error Mode]");
	}
	if(supportsTripleBufferSurfaces)
	{
		featuresStr.append(" [Triple Buffer Surfaces]");
	}

	if(featuresStr.empty())
		return;
	logMsg("features:%s", featuresStr.c_str());
}

IG::ErrorCode EGLDisplayConnection::initDisplay(EGLDisplay display)
{
	logMsg("initializing EGL with display:%p", display);
	EGLint major, minor;
	if(!eglInitialize(display, &major, &minor))
	{
		logErr("error initializing EGL for display:%p", display);
		return {EINVAL};
	}
	if(!eglVersion)
	{
		eglVersion = 10 * major + minor;
		auto extStr = eglQueryString(display, EGL_EXTENSIONS);
		supportsSurfaceless = eglVersion >= 15 || strstr(extStr, "EGL_KHR_surfaceless_context");
		supportsNoConfig = strstr(extStr, "EGL_KHR_no_config_context");
		supportsNoError = strstr(extStr, "EGL_KHR_create_context_no_error");
		if(Config::envIsLinux)
		{
			supportsTripleBufferSurfaces = strstr(extStr, "EGL_NV_triple_buffer");
		}
		printFeatures(supportsSurfaceless, supportsNoConfig, supportsNoError, supportsTripleBufferSurfaces);
	}
	if(!HAS_DISPLAY_REF_COUNT)
	{
		refCount++;
		logDMsg("referenced EGL display:%p, count:%u", display, refCount);
	}
	return {};
}

const char *EGLDisplayConnection::queryExtensions()
{
	return eglQueryString(display, EGL_EXTENSIONS);
}

const char *EGLDisplayConnection::errorString(EGLint error)
{
	switch(error)
	{
		case EGL_SUCCESS: return "Success";
		case EGL_NOT_INITIALIZED: return "EGL not initialized";
		case EGL_BAD_ACCESS: return "Bad access";
		case EGL_BAD_ALLOC: return "Bad allocation";
		case EGL_BAD_ATTRIBUTE: return "Bad attribute";
		case EGL_BAD_CONTEXT: return "Bad context";
		case EGL_BAD_CONFIG: return "Bad frame buffer config";
		case EGL_BAD_CURRENT_SURFACE: return "Bad current surface";
		case EGL_BAD_DISPLAY: return "Bad display";
		case EGL_BAD_SURFACE: return "Bad surface";
		case EGL_BAD_MATCH: return "Inconsistent arguments";
		case EGL_BAD_PARAMETER: return "Bad parameter";
		case EGL_BAD_NATIVE_PIXMAP: return "Bad native pixmap";
		case EGL_BAD_NATIVE_WINDOW: return "Bad native window";
		case EGL_CONTEXT_LOST: return "Context lost";
	}
	return "Unknown error";
}

int EGLDisplayConnection::makeRenderableType(GL::API api, unsigned majorVersion)
{
	if(api == GL::API::OPENGL)
	{
		return EGL_OPENGL_BIT;
	}
	else
	{
		switch(majorVersion)
		{
			default: return 0;
			case 2: return EGL_OPENGL_ES2_BIT;
			case 3: return EGL_OPENGL_ES3_BIT;
		}
	}
}

GLDisplay::operator bool() const
{
	return display != EGL_NO_DISPLAY;
}

bool GLDisplay::operator ==(GLDisplay const &rhs) const
{
	return display == rhs.display;
}

bool GLDisplay::deinit()
{
	if(display == EGL_NO_DISPLAY)
		return true;
	auto dpy = display;
	display = EGL_NO_DISPLAY;
	if(!HAS_DISPLAY_REF_COUNT)
	{
		if(!refCount)
		{
			logErr("EGL display:%p already has no references", dpy);
			return false;
		}
		refCount--;
		if(refCount)
		{
			logDMsg("unreferenced EGL display:%p, count:%u", dpy, refCount);
			return true;
		}
	}
	logMsg("terminating EGL display:%p", dpy);
	return eglTerminate(dpy);
}

static const EGLint *windowSurfaceAttribs()
{
	if(Config::envIsLinux && supportsTripleBufferSurfaces)
	{
		// request triple-buffering on Nvidia GPUs
		static const EGLint tripleBufferAttribs[]{EGL_RENDER_BUFFER, EGL_TRIPLE_BUFFER_NV, EGL_NONE};
		return tripleBufferAttribs;
	}
	return nullptr;
}

std::pair<IG::ErrorCode, GLDrawable> GLDisplay::makeDrawable(Window &win, GLBufferConfig config) const
{
	auto surface = eglCreateWindowSurface(display, config.glConfig,
		Config::MACHINE_IS_PANDORA ? (EGLNativeWindowType)0 : (EGLNativeWindowType)win.nativeObject(),
		windowSurfaceAttribs());
	if(surface == EGL_NO_SURFACE)
	{
		return {{EINVAL}, {}};
	}
	logMsg("made surface:%p", surface);
	return {{}, surface};
}

void GLDisplay::logInfo() const
{
	if(!Config::DEBUG_BUILD)
		return;
	logMsg("version: %s (%s)", eglQueryString(display, EGL_VENDOR), eglQueryString(display, EGL_VERSION));
	logMsg("APIs: %s", eglQueryString(display, EGL_CLIENT_APIS));
	logMsg("extensions: %s", eglQueryString(display, EGL_EXTENSIONS));
	//printEGLConfs(display);
}

// GLDrawable

void GLDrawable::freeCaches() {}

void GLDrawable::restoreCaches() {}

GLDrawable::operator bool() const
{
	return surface != EGL_NO_SURFACE;
}

bool GLDrawable::operator ==(GLDrawable const &rhs) const
{
	return surface == rhs.surface;
}

bool GLDrawable::destroy(GLDisplay display)
{
	if(surface == EGL_NO_SURFACE)
		return true;
	logMsg("destroying surface:%p", surface);
	auto success = eglDestroySurface(display, std::exchange(surface, EGL_NO_SURFACE));
	if(Config::DEBUG_BUILD && !success)
	{
		logErr("error:%s in eglDestroySurface(%p, %p)",
			display.errorString(eglGetError()), (EGLDisplay)display, surface);
	}
	return success;
}

// EGLBufferConfig

EGLint EGLBufferConfig::renderableTypeBits(GLDisplay display) const
{
	EGLint bits;
	eglGetConfigAttrib(display, glConfig, EGL_RENDERABLE_TYPE, &bits);
	return bits;
}

bool EGLBufferConfig::maySupportGLES(GLDisplay display, unsigned majorVersion) const
{
	switch(majorVersion)
	{
		default: return false;
		case 1: return true;
		case 2: return renderableTypeBits(display) & EGL_OPENGL_ES2_BIT;
		case 3: return renderableTypeBits(display) & EGL_OPENGL_ES3_BIT;
	}
}

}
