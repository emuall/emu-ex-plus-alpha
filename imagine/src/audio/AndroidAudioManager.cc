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

#include <imagine/audio/AudioManager.hh>
#include <imagine/audio/defs.hh>
#include <imagine/base/ApplicationContext.hh>
#include <imagine/util/jni.hh>
#include <imagine/util/utility.h>
#include <imagine/logger/logger.h>
#include "../base/android/android.hh"

namespace IG::AudioManager
{

static jobject audioManager{};
static JavaInstMethod<jint(jobject, jint, jint)> jRequestAudioFocus{};
static JavaInstMethod<jint(jobject)> jAbandonAudioFocus{};
static JavaInstMethod<jobject(jstring)> jGetProperty{};
static bool soloMix_ = true;
static bool sessionActive = false;
static constexpr uint32_t defaultOutputBufferFrames = 192; // default used in Google Oboe library

static int audioManagerIntProperty(JNIEnv* env, JavaInstMethod<jobject(jstring)> jGetProperty, const char *propStr)
{
	auto propJStr = env->NewStringUTF(propStr);
	auto valJStr = (jstring)jGetProperty(env, audioManager, propJStr);
	if(!valJStr)
	{
		logWarn("%s is null", propStr);
		return 0;
	}
	auto valStr = env->GetStringUTFChars(valJStr, nullptr);
	int val = atoi(valStr);
	env->ReleaseStringUTFChars(valJStr, valStr);
	return val;
}

static void setupAudioManagerJNI(JNIEnv* env, jobject baseActivity, jclass baseActivityClass, int sdk)
{
	if(audioManager)
		return;
	JavaInstMethod<jobject()> jAudioManager{env, baseActivityClass, "audioManager", "()Landroid/media/AudioManager;"};
	audioManager = jAudioManager(env, baseActivity);
	assert(audioManager);
	audioManager = env->NewGlobalRef(audioManager);
	jclass jAudioManagerCls = env->GetObjectClass(audioManager);
	jRequestAudioFocus.setup(env, jAudioManagerCls, "requestAudioFocus", "(Landroid/media/AudioManager$OnAudioFocusChangeListener;II)I");
	jAbandonAudioFocus.setup(env, jAudioManagerCls, "abandonAudioFocus", "(Landroid/media/AudioManager$OnAudioFocusChangeListener;)I");
	if(sdk >= 17)
		jGetProperty.setup(env, jAudioManagerCls, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
}

static void requestAudioFocus(JNIEnv* env, jobject baseActivity, jclass baseActivityClass, int sdk)
{
	setupAudioManagerJNI(env, baseActivity, baseActivityClass, sdk);
	auto res = jRequestAudioFocus(env, audioManager, baseActivity, 3, 1);
	//logMsg("%d from requestAudioFocus()", (int)res);
}

static void abandonAudioFocus(JNIEnv* env, jobject baseActivity, jclass baseActivityClass, int sdk)
{
	setupAudioManagerJNI(env, baseActivity, baseActivityClass, sdk);
	jAbandonAudioFocus(env, audioManager, baseActivity);
}

Audio::SampleFormat nativeSampleFormat(Base::ApplicationContext ctx)
{
	return ctx.androidSDK() >= 21 ? Audio::SampleFormats::f32 : Audio::SampleFormats::i16;
}

uint32_t nativeRate(Base::ApplicationContext ctx)
{
	uint32_t rate = 44100;
	if(ctx.androidSDK() >= 17)
	{
		auto env = ctx.mainThreadJniEnv();
		setupAudioManagerJNI(env, ctx.baseActivityObject(), ctx.baseActivityClass(), ctx.androidSDK());
		rate = audioManagerIntProperty(env, jGetProperty, "android.media.property.OUTPUT_SAMPLE_RATE");
		if(rate != 44100 && rate != 48000)
		{
			// only support 44KHz and 48KHz for now
			logWarn("ignoring OUTPUT_SAMPLE_RATE value:%u", rate);
			rate = 44100;
		}
		else
		{
			logMsg("native sample rate: %u", rate);
		}
	}
	return rate;
}

Audio::Format nativeFormat(Base::ApplicationContext ctx)
{
	return {nativeRate(ctx), nativeSampleFormat(ctx), 2};
}

uint32_t nativeOutputFramesPerBuffer(Base::ApplicationContext ctx)
{
	if(ctx.androidSDK() >= 17)
	{
		auto env = ctx.mainThreadJniEnv();
		setupAudioManagerJNI(env, ctx.baseActivityObject(), ctx.baseActivityClass(), ctx.androidSDK());
		int outputBufferFrames = audioManagerIntProperty(env, jGetProperty, "android.media.property.OUTPUT_FRAMES_PER_BUFFER");
		//logMsg("native buffer frames: %d", outputBufferFrames);
		if(outputBufferFrames <= 0 || outputBufferFrames > 4096)
		{
			logWarn("ignoring OUTPUT_FRAMES_PER_BUFFER value:%d", outputBufferFrames);
			return defaultOutputBufferFrames;
		}
		return outputBufferFrames;
	}
	else
	{
		return defaultOutputBufferFrames;
	}
}

void setSoloMix(Base::ApplicationContext ctx, bool newSoloMix)
{
	if(soloMix_ != newSoloMix)
	{
		logMsg("setting solo mix: %d", newSoloMix);
		soloMix_ = newSoloMix;
		if(sessionActive)
		{
			// update the current audio focus
			if(newSoloMix)
				requestAudioFocus(ctx.mainThreadJniEnv(), ctx.baseActivityObject(), ctx.baseActivityClass(), ctx.androidSDK());
			else
				abandonAudioFocus(ctx.mainThreadJniEnv(), ctx.baseActivityObject(), ctx.baseActivityClass(), ctx.androidSDK());
		}
	}
}

bool soloMix()
{
	return soloMix_;
}

void setMusicVolumeControlHint(Base::ApplicationContext ctx)
{
	using namespace Base;
	auto env = ctx.mainThreadJniEnv();
	JavaInstMethod<void(jint)> jSetVolumeControlStream{env, ctx.baseActivityClass(), "setVolumeControlStream", "(I)V"};
	jSetVolumeControlStream(env, ctx.baseActivityObject(), 3);
}

void startSession(Base::ApplicationContext ctx)
{
	if(sessionActive)
		return;
	sessionActive = true;
	if(soloMix_)
	{
		requestAudioFocus(ctx.mainThreadJniEnv(), ctx.baseActivityObject(), ctx.baseActivityClass(), ctx.androidSDK());
	}
}

void endSession(Base::ApplicationContext ctx)
{
	if(!sessionActive)
		return;
	sessionActive = false;
	if(soloMix_)
	{
		abandonAudioFocus(ctx.mainThreadJniEnv(), ctx.baseActivityObject(), ctx.baseActivityClass(), ctx.androidSDK());
	}
}

}

namespace IG::Audio
{

std::vector<ApiDesc> audioAPIs(Base::ApplicationContext ctx)
{
	std::vector<ApiDesc> desc;
	if(ctx.androidSDK() >= 26)
	{
		desc.reserve(2);
		desc.emplace_back("AAudio", Api::AAUDIO);
	}
	desc.emplace_back("OpenSL ES", Api::OPENSL_ES);
	return desc;
}

Api makeValidAPI(Base::ApplicationContext ctx, Api api)
{
	// Don't default to AAudio on Android 8.0 (SDK 26) due
	// too various device-specific driver bugs:
	// ASUS ZenFone 4 (ZE554KL) crashes randomly ~5 mins after playback starts
	// Samsung Galaxy S7 may crash when closing audio stream even when stopping it beforehand
	if(ctx.androidSDK() >= 27)
	{
		if(api == Api::OPENSL_ES)
			return Api::OPENSL_ES; // OpenSL ES was explicitly requested
		return Api::AAUDIO;
	}
	else
	{
		return Api::OPENSL_ES;
	}
}

}
