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
#include <emuframework/EmuAppInlines.hh>
#include <emuframework/EmuSystemInlines.hh>
#include <imagine/fs/FS.hh>
#include <imagine/io/FileIO.hh>
#include <imagine/util/format.hh>
#include <imagine/util/string.h>
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

const char *EmuSystem::creditsViewStr = CREDITS_INFO_STRING "(c) 2012-2022\nRobert Broglia\nwww.explusalpha.com\n\nPortions (c) the\nVBA-m Team\nvba-m.com";
bool EmuSystem::hasBundledGames = true;
bool EmuSystem::hasCheats = true;
bool EmuApp::needsGlobalInstance = true;
constexpr WSize lcdSize{240, 160};

EmuSystem::NameFilterFunc EmuSystem::defaultFsFilter =
	[](std::string_view name)
	{
		return IG::endsWithAnyCaseless(name, ".gba");
	};

const BundledGameInfo &EmuSystem::bundledGameInfo(int idx) const
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

void GbaSystem::reset(EmuApp &, ResetMode mode)
{
	assert(hasContent());
	CPUReset(gGba);
}

FS::FileString GbaSystem::stateFilename(int slot, std::string_view name) const
{
	return IG::format<FS::FileString>("{}{}.sgm", name, saveSlotChar(slot));
}

void GbaSystem::saveState(IG::CStringView path)
{
	if(!CPUWriteState(appContext(), gGba, path))
		return throwFileWriteError();
}

void GbaSystem::loadState(EmuApp &app, IG::CStringView path)
{
	if(!CPUReadState(app.appContext(), gGba, path))
		return throwFileReadError();
}

void GbaSystem::loadBackupMemory(EmuApp &app)
{
	if(coreOptions.saveType == GBA_SAVE_NONE)
		return;
	if(!saveFileIO)
		saveFileIO = staticBackupMemoryFile(app.contentSaveFilePath(".sav"), saveMemorySize(), 0xFF);
	if(!saveFileIO)
		throw std::runtime_error("Error accessing .sav file, please verify it has write access");
	auto buff = saveFileIO.buffer(IOBufferMode::Release);
	if(buff.isMappedFile())
		saveFileIO = {};
	saveMemoryIsMappedFile = buff.isMappedFile();
	setSaveMemory(std::move(buff));
}

void GbaSystem::onFlushBackupMemory(EmuApp &app, BackupMemoryDirtyFlags)
{
	if(coreOptions.saveType == GBA_SAVE_NONE)
		return;
	const ByteBuffer &saveData = eepromInUse ? eepromData : flashSaveMemory;
	if(saveMemoryIsMappedFile)
	{
		logMsg("flushing backup memory");
		msync(saveData.data(), saveData.size(), MS_SYNC);
	}
	else
	{
		logMsg("saving backup memory");
		saveFileIO.write(saveData.span(), 0);
	}
}

WallClockTimePoint GbaSystem::backupMemoryLastWriteTime(const EmuApp &app) const
{
	return appContext().fileUriLastWriteTime(app.contentSaveFilePath(".sav").c_str());
}

void GbaSystem::closeSystem()
{
	assert(hasContent());
	CPUCleanUp();
	saveFileIO = {};
	coreOptions.saveType = GBA_SAVE_NONE;
	detectedRtcGame = 0;
	detectedSensorType = {};
	sensorListener = {};
	darknessLevel = darknessLevelDefault;
	cheatsList.clear();
}

void GbaSystem::applyGamePatches(uint8_t *rom, int &romSize)
{
	auto ctx = appContext();
	// The patchApply* functions are responsible for closing the FILE
	if(auto f = IG::FileUtils::fopenUri(ctx, userFilePath(patchesDir, ".ips"), "rb");
		f)
	{
		logMsg("applying IPS patch:%s", userFilePath(patchesDir, ".ips").data());
		if(!patchApplyIPS(f, &rom, &romSize))
		{
			throw std::runtime_error(std::format("Error applying IPS patch in:\n{}", patchesDir));
		}
	}
	else if(auto f = IG::FileUtils::fopenUri(ctx, userFilePath(patchesDir, ".ups"), "rb");
		f)
	{
		logMsg("applying UPS patch:%s", userFilePath(patchesDir, ".ups").data());
		if(!patchApplyUPS(f, &rom, &romSize))
		{
			throw std::runtime_error(std::format("Error applying UPS patch in:\n{}", patchesDir));
		}
	}
	else if(auto f = IG::FileUtils::fopenUri(ctx, userFilePath(patchesDir, ".ppf"), "rb");
		f)
	{
		logMsg("applying UPS patch:%s", userFilePath(patchesDir, ".ppf").data());
		if(!patchApplyPPF(f, &rom, &romSize))
		{
			throw std::runtime_error(std::format("Error applying PPF patch in:\n{}", patchesDir));
		}
	}
}

void GbaSystem::loadContent(IO &io, EmuSystemCreateParams, OnLoadProgressDelegate)
{
	int size = CPULoadRomWithIO(gGba, io);
	if(!size)
	{
		throwFileReadError();
	}
	setGameSpecificSettings(gGba, size);
	applyGamePatches(gGba.mem.rom, size);
	CPUInit(gGba, 0, 0);
	CPUReset(gGba);
	readCheatFile(*this);
}

static void updateColorMap(auto &map, const PixelDesc &pxDesc)
{
	for(auto i : iotaCount(0x10000))
	{
		auto r = remap(i & 0x1f, 0, 0x1f, 0.f, 1.f);
		auto g = remap((i & 0x3e0) >> 5, 0, 0x1f, 0.f, 1.f);
		auto b = remap((i & 0x7c00) >> 10, 0, 0x1f, 0.f, 1.f);
		map[i] = pxDesc.build(r, g, b, 1.f);
	}
}

bool GbaSystem::onVideoRenderFormatChange(EmuVideo &video, IG::PixelFormat fmt)
{
	logMsg("updating system color maps");
	video.setFormat({lcdSize, fmt});
	if(fmt == PIXEL_RGB565)
		updateColorMap(systemColorMap.map16, PIXEL_DESC_RGB565);
	else
		updateColorMap(systemColorMap.map32, fmt.desc().nativeOrder());
	return true;
}

void GbaSystem::renderFramebuffer(EmuVideo &video)
{
	systemDrawScreen({}, video);
}

void GbaSystem::runFrame(EmuSystemTaskContext taskCtx, EmuVideo *video, EmuAudio *audio)
{
	CPULoop(gGba, taskCtx, video, audio);
}

void GbaSystem::configAudioRate(FrameTime outputFrameTime, int outputRate)
{
	long mixRate = std::round(audioMixRate(outputRate, outputFrameTime));
	logMsg("set sound mix rate:%ld", mixRate);
	soundSetSampleRate(gGba, mixRate);
}

void GbaSystem::onStart()
{
	setSensorActive(true);
}

void GbaSystem::onStop()
{
	setSensorActive(false);
}

void EmuApp::onCustomizeNavView(EmuApp::NavView &view)
{
	const Gfx::LGradientStopDesc navViewGrad[] =
	{
		{ .0, Gfx::PackedColor::format.build(42./255., 82./255., 190./255., 1.) },
		{ .3, Gfx::PackedColor::format.build(42./255., 82./255., 190./255., 1.) },
		{ .97, Gfx::PackedColor::format.build((42./255.) * .6, (82./255.) * .6, (190./255.) * .6, 1.) },
		{ 1., view.separatorColor() },
	};
	view.setBackgroundGradient(navViewGrad);
}

}

void systemDrawScreen(EmuEx::EmuSystemTaskContext taskCtx, EmuEx::EmuVideo &video)
{
	using namespace EmuEx;
	auto img = video.startFrame(taskCtx);
	IG::PixmapView framePix{{lcdSize, IG::PIXEL_RGB565}, gGba.lcd.pix};
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
		int frames = length >> 1; // stereo samples
		//logMsg("%d audio frames", frames);
		audio->writeFrames(finalWave, frames);
	}
}
