/*  This file is part of EmuFramework.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with EmuFramework.  If not, see <http://www.gnu.org/licenses/> */

#include <emuframework/VController.hh>
#include <emuframework/EmuSystem.hh>
#include <emuframework/EmuApp.hh>
#include <imagine/util/math/int.hh>
#include <imagine/base/Window.hh>
#include <imagine/gfx/Renderer.hh>
#include <imagine/gfx/RendererCommands.hh>
#include <imagine/pixmap/MemPixmap.hh>
#include <imagine/gui/View.hh>
#include <imagine/logger/logger.h>

namespace EmuEx
{

constexpr SystemLogger log{"VControllerGamepad"};

void VControllerDPad::setImage(Gfx::TextureSpan img)
{
	spr.set(img);
}

void VControllerDPad::updateBoundingAreaGfx(Gfx::Renderer &r)
{
	if(!config.visualizeBounds || !padArea.xSize())
		return;
	MemPixmap mapMemPix{{padArea.size(), PIXEL_FMT_RGB565}};
	auto mapPix = mapMemPix.view();
	auto pixels = mapPix.mdspan<uint16_t>();
	for(auto y : iotaCount(pixels.extent(0)))
		for(auto x : iotaCount(pixels.extent(1)))
		{
			auto input = getInput({padArea.xPos(LT2DO) + int(x), padArea.yPos(LT2DO) + int(y)});
			//log.info("got input {}", input);
			pixels[y, x] = input == std::array<KeyInfo, 2>{} ? PIXEL_DESC_RGB565.build(1., 0., 0.)
									: (input[0] && input[1]) ? PIXEL_DESC_RGB565.build(0., 1., 0.)
									: PIXEL_DESC_RGB565.build(1., 1., 1.);
		}
	mapImg = r.makeTexture({mapPix.desc(), View::imageSamplerConfig});
	mapImg.write(0, mapPix, {});
	mapSpr.set(mapImg);
	mapSpr.setPos(padArea);
}

static bool isValidDeadzone(int val) { return val >= 100 && val <= 300; }

bool VControllerDPad::setDeadzone(Gfx::Renderer &r, int newDeadzone, const Window &win)
{
	if(!isValidDeadzone(newDeadzone))
		return false;
	if(config.deadzoneMM100x == newDeadzone)
		return true;
	config.deadzoneMM100x = newDeadzone;
	deadzonePixels = win.widthMMInPixels(newDeadzone / 100.f);
	updateBoundingAreaGfx(r);
	return true;
}

static bool isValidDiagonalSensitivity(float val) { return val >= .01f && val <= 1.f; }

bool VControllerDPad::setDiagonalSensitivity(Gfx::Renderer &r, float newDiagonalSensitivity)
{
	if(!isValidDiagonalSensitivity(newDiagonalSensitivity))
		return false;
	if(config.diagonalSensitivity == newDiagonalSensitivity)
		return true;
	log.info("set diagonal sensitivity: {}", (double)newDiagonalSensitivity);
	config.diagonalSensitivity = newDiagonalSensitivity;
	updateBoundingAreaGfx(r);
	return true;
}

void VControllerDPad::setSize(Gfx::Renderer &r, int sizeInPixels)
{
	//log.info("set dpad pixel size: {}", sizeInPixels);
	btnSizePixels = sizeInPixels;
	auto rect = makeWindowRectRel({0, 0}, {btnSizePixels, btnSizePixels});
	bool changedSize = rect.xSize() != padBaseArea.xSize();
	padBaseArea = rect;
	padArea = {{}, {int(padBaseArea.xSize() * 1.5f), int(padBaseArea.xSize() * 1.5f)}};
	if(changedSize)
		updateBoundingAreaGfx(r);
}

void VControllerDPad::setPos(WPt pos, WRect viewBounds)
{
	padBaseArea.setPos(pos, C2DO);
	padBaseArea.fitIn(viewBounds);
	spr.setPos(padBaseArea);
	//log.info("set dpad pos {}:{}:{}:{}, {}:{}:{}:{}", padBaseArea.x, padBaseArea.y, padBaseArea.x2, padBaseArea.y2,
	//	(double)padBase.x, (double)padBase.y, (double)padBase.x2, (double)padBase.y2);
	padArea.setPos(padBaseArea.pos(C2DO), C2DO);
	if(config.visualizeBounds)
	{
		mapSpr.setPos(padArea);
	}
}

void VControllerDPad::setShowBounds(Gfx::Renderer &r, bool on)
{
	if(config.visualizeBounds == on)
		return;
	config.visualizeBounds = on;
	if(!on)
	{
		log.info("deallocating bounding box display resources");
		mapImg = {};
	}
	else
	{
		updateBoundingAreaGfx(r);
	}
}

void VControllerDPad::updateMeasurements(const Window &win)
{
	deadzonePixels = win.widthMMInPixels(config.deadzoneMM100x / 100.f);
}

void VControllerDPad::transposeKeysForPlayer(const EmuApp &app, int player)
{
	for(auto &k : config.keys)
	{
		k = app.inputManager.transpose(k, player);
	}
}

void VControllerDPad::drawButtons(Gfx::RendererCommands &__restrict__ cmds) const
{
	cmds.basicEffect().enableTexture(cmds);
	spr.draw(cmds);
}

void VControllerDPad::drawBounds(Gfx::RendererCommands &__restrict__ cmds) const
{
	cmds.basicEffect().enableTexture(cmds);
	if(!config.visualizeBounds)
		return;
	mapSpr.draw(cmds);
}

std::array<KeyInfo, 2> VControllerDPad::getInput(WPt c) const
{
	std::array<KeyInfo, 2> pad{};
	if(!padArea.overlaps(c))
		return pad;
	c -= padArea.center();
	int xDeadzone = deadzonePixels, yDeadzone = deadzonePixels;
	if(std::abs(c.x) > deadzonePixels)
		yDeadzone += (std::abs(c.x) - deadzonePixels) * config.diagonalSensitivity;
	if(std::abs(c.y) > deadzonePixels)
		xDeadzone += (std::abs(c.y) - deadzonePixels) * config.diagonalSensitivity;
	if(std::abs(c.x) > xDeadzone)
	{
		if(c.x > 0)
			pad[0] = config.keys[1]; // right
		else
			pad[0] = config.keys[3]; // left
	}
	if(std::abs(c.y) > yDeadzone)
	{
		if(c.y > 0)
			pad[1] = config.keys[2]; // down
		else
			pad[1] = config.keys[0]; // up
	}
	return pad;
}

void VControllerDPad::Config::validate(const EmuApp &app)
{
	for(auto &k : keys) { k = app.inputManager.validateSystemKey(k, false); }
	if(!isValidDiagonalSensitivity(diagonalSensitivity))
		diagonalSensitivity = defaultDPadDiagonalSensitivity;
	if(!isValidDeadzone(deadzoneMM100x))
		deadzoneMM100x = defaultDPadDeadzoneMM100x;
}

void VControllerDPad::setAlpha(float alpha)
{
	std::array<Gfx::Color, 4> colors;
	colors.fill({alpha});
	if(isHighlighted[0] || isHighlighted[3])
		colors[0] = colors[0].multiplyRGB(2.f);
	if(isHighlighted[2] || isHighlighted[1])
		colors[3] = colors[3].multiplyRGB(2.f);
	if(isHighlighted[2] || isHighlighted[3])
		colors[1] = colors[1].multiplyRGB(2.f);
	if(isHighlighted[0] || isHighlighted[1])
		colors[2] = colors[2].multiplyRGB(2.f);
	for(auto &&[i, vtx] : enumerate(spr)) { vtx.color = colors[i]; }
	for(auto &&[i, vtx] : enumerate(mapSpr)) { vtx.color = colors[i]; }
}

}
