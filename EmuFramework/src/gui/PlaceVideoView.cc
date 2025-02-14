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

#include "PlaceVideoView.hh"
#include <emuframework/EmuApp.hh>
#include <imagine/gfx/RendererCommands.hh>

namespace EmuEx
{

PlaceVideoView::PlaceVideoView(ViewAttachParams attach, EmuVideoLayer &layer, VController &vController):
	View(attach),
	layer{layer},
	vController{vController},
	exitText{"Exit", &defaultFace()},
	resetText{"Reset", &defaultFace()}
{
	app().applyOSNavStyle(appContext(), true);
	layer.setBrightness(app().videoBrightnessAsRGB());
}

PlaceVideoView::~PlaceVideoView()
{
	app().applyOSNavStyle(appContext(), false);
	layer.setBrightness(app().videoBrightnessAsRGB() * menuVideoBrightnessScale);
}

void PlaceVideoView::place()
{
	auto offsetLimit = viewRect().size() - layer.contentRect().size();
	posOffsetLimit = viewRect().isLandscape() ? offsetLimit.x : offsetLimit.y;
	dragState = {};
	exitText.compile(renderer());
	resetText.compile(renderer());
	WRect btnBounds{{0, 0}, {viewRect().xSize(), exitText.nominalHeight() * 2}};
	btnBounds.setPos(viewRect().pos(LB2DO), LB2DO);
	exitBounds = btnBounds;
	exitBounds.x2 = viewRect().xSize() / 2;
	resetBounds = btnBounds;
	resetBounds.x = viewRect().xSize() / 2;
}

bool PlaceVideoView::inputEvent(const Input::Event &e)
{
	return visit(overloaded
	{
		[&](const Input::KeyEvent &e)
		{
			if(e.pushed(Input::DefaultKey::UP) && viewRect().isPortrait())
			{
				updateVideo(layer.portraitOffset - 1);
				return true;
			}
			else if(e.pushed(Input::DefaultKey::DOWN) && viewRect().isPortrait())
			{
				updateVideo(layer.portraitOffset + 1);
				return true;
			}
			else if(e.pushed(Input::DefaultKey::LEFT) && viewRect().isLandscape())
			{
				updateVideo(layer.landscapeOffset - 1);
				return true;
			}
			else if(e.pushed(Input::DefaultKey::RIGHT) && viewRect().isLandscape())
			{
				updateVideo(layer.landscapeOffset + 1);
				return true;
			}
			else if(e.pushed(Input::DefaultKey::CONFIRM))
			{
				updateVideo(0); // reset to default
				return true;
			}
			else if(e.pushed(Input::DefaultKey::CANCEL))
			{
				dismiss();
				return true;
			}
			return false;
		},
		[&](const Input::MotionEvent &e)
		{
			auto id = e.pointerId();
			switch(e.state())
			{
				case Input::Action::PUSHED:
				{
					if(!dragState.isTracking() && !exitBounds.overlaps(e.pos()) && !resetBounds.overlaps(e.pos()))
					{
						dragState.start(id, e.pos());
						startPosOffset = viewRect().isLandscape() ? layer.landscapeOffset : layer.portraitOffset;
					}
					break;
				}
				case Input::Action::MOVED:
				{
					if(dragState.isTracking(id))
					{
						dragState.update(e.pos(), 0);
						updateVideo(viewRect().isLandscape() ? startPosOffset + dragState.downPosDiff().x
							: startPosOffset + dragState.downPosDiff().y);
					}
					break;
				}
				case Input::Action::RELEASED:
				case Input::Action::CANCELED:
				{
					if(dragState.isTracking(id))
					{
						dragState = {};
					}
					else if(e.released())
					{
						if(exitBounds.overlaps(e.pos()))
						{
							dismiss();
						}
						else if(resetBounds.overlaps(e.pos()))
						{
							updateVideo(0);
						}
					}
					break;
				}
				default:
					break;
			}
			return true;
		}
	}, e);
}

void PlaceVideoView::draw(Gfx::RendererCommands &__restrict__ cmds)
{
	using namespace IG::Gfx;
	vController.draw(cmds, true);
	cmds.setColor({.5, .5, .5});
	auto &basicEffect = cmds.basicEffect();
	basicEffect.disableTexture(cmds);
	const int lineSize = 1;
	cmds.drawRect({{viewRect().x, viewRect().yCenter()},
		{viewRect().x2, viewRect().yCenter() + lineSize}});
	cmds.drawRect({{viewRect().xCenter(), viewRect().y},
		{viewRect().xCenter() + lineSize, viewRect().y2}});
	cmds.setColor({.2, .2, .2, .5});
	cmds.drawRect(exitBounds);
	cmds.drawRect(resetBounds);
	basicEffect.enableAlphaTexture(cmds);
	cmds.setColor(ColorName::WHITE);
	exitText.draw(cmds, exitBounds.pos(C2DO), C2DO);
	resetText.draw(cmds, resetBounds.pos(C2DO), C2DO);
}

void PlaceVideoView::updateVideo(int offset)
{
	if(viewRect().isLandscape())
	{
		layer.landscapeOffset = std::clamp(offset, -posOffsetLimit / 2, posOffsetLimit / 2);
	}
	else
	{
		int uiElementHeight = app().viewController().inputView.uiElementHeight();
		layer.portraitOffset = std::clamp(offset, -uiElementHeight, posOffsetLimit - uiElementHeight);
	}
	app().viewController().placeEmuViews();
	app().viewController().postDrawToEmuWindows();
}

void PlaceVideoView::onShow()
{
	vController.applyButtonAlpha(.50);
}

}
