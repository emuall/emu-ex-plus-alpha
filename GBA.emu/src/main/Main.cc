/*  This file is part of GBA.emu.

	GBA.emu is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	GBA.emu is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with GBA.emu.  If not, see <http://www.gnu.org/licenses/> */

#define LOGTAG "main"
#include <emuframework/EmuApp.hh>
#include <emuframework/EmuAppInlines.hh>
#include <emuframework/EmuAudio.hh>
#include <emuframework/EmuVideo.hh>
#include "internal.hh"
#include <imagine/fs/FS.hh>
#include <imagine/util/format.hh>
#include <vbam/gba/GBA.h>
#include <vbam/gba/GBAGfx.h>
#include <vbam/gba/Sound.h>
#include <vbam/gba/RTC.h>
#include <vbam/common/SoundDriver.h>
#include <vbam/common/Patch.h>
#include <vbam/Util.h>
#include <sys/mman.h>

namespace EmuEx
{

bool detectedRtcGame = 0;
const char *EmuSystem::creditsViewStr = CREDITS_INFO_STRING "(c) 2012-2022\nRobert Broglia\nwww.explusalpha.com\n\nPortions (c) the\nVBA-m Team\nvba-m.com";
bool EmuSystem::hasBundledGames = true;
bool EmuSystem::hasCheats = true;
constexpr IG::WP lcdSize{240, 160};

EmuSystem::NameFilterFunc EmuSystem::defaultFsFilter =
	[](std::string_view name)
	{
		return IG::stringEndsWithAny(name, ".gba", ".GBA");
	};
EmuSystem::NameFilterFunc EmuSystem::defaultBenchmarkFsFilter = defaultFsFilter;

const BundledGameInfo &EmuSystem::bundledGameInfo(unsigned idx) const
{
	static const BundledGameInfo info[]
	{
		{"Motocross Challenge", "Motocross Challenge.7z"}
	};

	return info[0];
}

const char *EmuSystem::shortSystemName() const
{
	return "GBA";
}

const char *EmuSystem::systemName() const
{
	return "Game Boy Advance";
}

void EmuSystem::reset(ResetMode mode)
{
	assert(hasContent());
	CPUReset(gGba);
}

FS::FileString EmuSystem::stateFilename(int slot, std::string_view name) const
{
	return IG::format<FS::FileString>("{}{}.sgm", name, saveSlotChar(slot));
}

void EmuSystem::saveState(IG::CStringView path)
{
	if(!CPUWriteState(appContext(), gGba, path))
		return throwFileWriteError();
}

void EmuSystem::loadState(EmuApp &app, IG::CStringView path)
{
	if(!CPUReadState(app.appContext(), gGba, path))
		return throwFileReadError();
}

void EmuSystem::onFlushBackupMemory(BackupMemoryDirtyFlags)
{
	if(!hasContent() || saveType == GBA_SAVE_NONE)
		return;
	if(saveMemoryIsMappedFile)
	{
		logMsg("flushing backup memory");
		ByteBuffer &saveData = eepromInUse ? eepromData : flashSaveMemory;
		msync(saveData.data(), saveData.size(), MS_SYNC);
	}
	else
	{
		logMsg("saving backup memory");
		auto saveStr = EmuSystem::contentSaveFilePath(".sav");
		CPUWriteBatteryFile(appContext(), gGba, saveStr.data());
	}
}

void EmuSystem::closeSystem()
{
	assert(hasContent());
	CPUCleanUp();
	detectedRtcGame = 0;
	cheatsList.clear();
}

static void applyGamePatches(EmuSystem &sys, uint8_t *rom, int &romSize)
{
	auto ctx = sys.appContext();
	if(auto patchStr = sys.contentSaveFilePath(".ips");
		ctx.fileUriExists(patchStr))
	{
		logMsg("applying IPS patch: %s", patchStr.data());
		if(!patchApplyIPS(ctx, patchStr.data(), &rom, &romSize))
		{
			throw std::runtime_error("Error applying IPS patch");
		}
	}
	else if(auto patchStr = sys.contentSaveFilePath(".ups");
		ctx.fileUriExists(patchStr))
	{
		logMsg("applying UPS patch: %s", patchStr.data());
		if(!patchApplyUPS(ctx, patchStr.data(), &rom, &romSize))
		{
			throw std::runtime_error("Error applying UPS patch");
		}
	}
	else if(auto patchStr = sys.contentSaveFilePath(".ppf");
		ctx.fileUriExists(patchStr))
	{
		logMsg("applying UPS patch: %s", patchStr.data());
		if(!patchApplyPPF(ctx, patchStr.data(), &rom, &romSize))
		{
			throw std::runtime_error("Error applying PPF patch");
		}
	}
}

void EmuSystem::loadContent(IO &io, EmuSystemCreateParams, OnLoadProgressDelegate)
{
	int size = CPULoadRomWithIO(gGba, io);
	if(!size)
	{
		throwFileReadError();
	}
	setGameSpecificSettings(gGba, size);
	applyGamePatches(*this, gGba.mem.rom, size);
	CPUInit(gGba, 0, 0);
	CPUReset(gGba);
	auto saveStr = EmuSystem::contentSaveFilePath(".sav");
	CPUReadBatteryFile(appContext(), gGba, saveStr.data());
	readCheatFile(*this);
}

bool EmuSystem::onVideoRenderFormatChange(EmuVideo &video, IG::PixelFormat fmt)
{
	logMsg("updating system color maps");
	video.setFormat({lcdSize, fmt});
	if(fmt == IG::PIXEL_RGB565)
		utilUpdateSystemColorMaps(16, 11, 6, 0);
	else if(fmt == IG::PIXEL_BGRA8888)
		utilUpdateSystemColorMaps(32, 19, 11, 3);
	else
		utilUpdateSystemColorMaps(32, 3, 11, 19);
	return true;
}

void EmuSystem::renderFramebuffer(EmuVideo &video)
{
	systemDrawScreen({}, video);
}

void EmuSystem::runFrame(EmuSystemTaskContext taskCtx, EmuVideo *video, EmuAudio *audio)
{
	CPULoop(gGba, taskCtx, video, audio);
}

void EmuSystem::configAudioRate(IG::FloatSeconds frameTime, uint32_t rate)
{
	double mixRate = std::round(rate * (59.7275 * frameTime.count()));
	logMsg("set audio rate:%d, mix rate:%d", rate, (int)mixRate);
	soundSetSampleRate(gGba, mixRate);
}

void EmuApp::onCustomizeNavView(EmuApp::NavView &view)
{
	const Gfx::LGradientStopDesc navViewGrad[] =
	{
		{ .0, Gfx::VertexColorPixelFormat.build(.5, .5, .5, 1.) },
		{ .03, Gfx::VertexColorPixelFormat.build(42./255., 82./255., 190./255., 1.) },
		{ .3, Gfx::VertexColorPixelFormat.build(42./255., 82./255., 190./255., 1.) },
		{ .97, Gfx::VertexColorPixelFormat.build((42./255.) * .6, (82./255.) * .6, (190./255.) * .6, 1.) },
		{ 1., Gfx::VertexColorPixelFormat.build(.5, .5, .5, 1.) },
	};
	view.setBackgroundGradient(navViewGrad);
}

}

void systemDrawScreen(EmuEx::EmuSystemTaskContext taskCtx, EmuEx::EmuVideo &video)
{
	using namespace EmuEx;
	auto img = video.startFrame(taskCtx);
	IG::Pixmap framePix{{lcdSize, IG::PIXEL_RGB565}, gGba.lcd.pix};
	assumeExpr(img.pixmap().size() == framePix.size());
	if(img.pixmap().format() == IG::PIXEL_FMT_RGB565)
	{
		img.pixmap().writeTransformed([](uint16_t p){ return systemColorMap.map16[p]; }, framePix);
	}
	else
	{
		assumeExpr(img.pixmap().format().bytesPerPixel() == 4);
		img.pixmap().writeTransformed([](uint16_t p){ return systemColorMap.map32[p]; }, framePix);
	}
	img.endFrame();
}

void systemOnWriteDataToSoundBuffer(EmuEx::EmuAudio *audio, const uint16_t *finalWave, int length)
{
	if(audio)
	{
		//logMsg("%d audio frames", audio->format().bytesToFrames(length));
		audio->writeFrames(finalWave, audio->format().bytesToFrames(length));
	}
}
