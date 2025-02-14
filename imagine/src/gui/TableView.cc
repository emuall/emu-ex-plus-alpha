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

#define LOGTAG "TableView"

#include <imagine/gui/TableView.hh>
#include <imagine/gui/ViewManager.hh>
#include <imagine/gui/MenuItem.hh>
#include <imagine/gfx/GeomQuad.hh>
#include <imagine/gfx/RendererCommands.hh>
#include <imagine/gfx/Renderer.hh>
#include <imagine/input/Event.hh>
#include <imagine/base/Window.hh>
#include <imagine/util/algorithm.h>
#include <imagine/util/variant.hh>
#include <imagine/util/math/int.hh>
#include <imagine/logger/logger.h>

namespace IG
{

TableView::TableView(ViewAttachParams attach, ItemsDelegate items, ItemDelegate item):
	ScrollView{attach}, items{items}, item{item}
{}

size_t TableView::cells() const
{
	return items(*this);
}

WSize TableView::cellSize() const
{
	return {viewRect().x, yCellSize};
}

void TableView::highlightCell(int idx)
{
	auto cells_ = items(*this);
	if(!cells_)
		return;
	if(idx >= 0)
		selected = nextSelectableElement(idx, cells_);
	else
		selected = -1;
	postDraw();
}

void TableView::setAlign(_2DOrigin align)
{
	this->align = align;
}

void TableView::prepareDraw()
{
	auto &r = renderer();
	for(auto i : iotaCount(items(*this)))
	{
		item(*this, i).prepareDraw(r);
	}
}

void TableView::draw(Gfx::RendererCommands &__restrict__ cmds)
{
	ssize_t cells_ = items(*this);
	if(!cells_)
		return;
	using namespace IG::Gfx;
	auto visibleRect = viewRect() + WindowRect{{}, {0, displayRect().y2 - viewRect().y2}};
	cmds.setClipRect(renderer().makeClipRect(window(), visibleRect));
	cmds.setClipTest(true);
	auto y = viewRect().yPos(LT2DO);
	auto x = viewRect().xPos(LT2DO);
	int startYCell = std::min(scrollOffset() / yCellSize, (int)cells_);
	size_t endYCell = std::clamp(startYCell + visibleCells, 0, (int)cells_);
	if(startYCell < 0)
	{
		// skip non-existent cells
		y += -startYCell * yCellSize;
		startYCell = 0;
	}
	//logMsg("draw cells [%d,%d)", startYCell, (int)endYCell);
	y -= scrollOffset() % yCellSize;

	// draw separators
	int yStart = y;
	cmds.basicEffect().disableTexture(cmds);
	int selectedCellY = INT_MAX;
	{
		StaticArrayList<IColQuad, 80> vRect;
		StaticArrayList<std::array<VertexIndex, 6>, vRect.capacity()> vRectIdx;
		const auto headingColor = PackedColor::format.build(.4, .4, .4, 1.);
		const auto regularColor = PackedColor::format.build(.2, .2, .2, 1.);
		auto regularYSize = std::max(1, window().heightMMInPixels(.2));
		auto headingYSize = std::max(2, window().heightMMInPixels(.4));
		for(size_t i = startYCell; i < endYCell; i++)
		{
			if((int)i == selected)
			{
				selectedCellY = y;
			}
			if(i != 0)
			{
				int ySize = regularYSize;
				auto color = regularColor;
				if(!elementIsSelectable(item(*this, i - 1)))
				{
					ySize = headingYSize;
					color = headingColor;
				}
				vRectIdx.emplace_back(makeRectIndexArray(vRect.size()));
				auto rect = makeWindowRectRel({x, y-1}, {viewRect().xSize(), ySize}).as<int16_t>();
				vRect.emplace_back(IColQuad::RectInitParams{.bounds = rect, .color = color});
			}
			y += yCellSize;
			if(vRect.size() == vRect.capacity()) [[unlikely]]
				break;
		}
		if(vRect.size())
		{
			cmds.set(BlendMode::OFF);
			cmds.setColor(ColorName::WHITE);
			drawQuads(cmds, vRect, vRectIdx);
		}
	}

	// draw scroll bar
	ScrollView::drawScrollContent(cmds);

	// draw selected rectangle
	if(selectedCellY != INT_MAX)
	{
		cmds.set(BlendMode::ALPHA);
		if(hasFocus)
			cmds.setColor({.2, .71, .9, 1./3.});
		else
			cmds.setColor({.2 / 3., .71 / 3., .9 / 3., 1./3.});
		auto rect = IG::makeWindowRectRel({x, selectedCellY}, {viewRect().xSize(), yCellSize-1});
		cmds.drawRect(rect);
	}

	// draw elements
	y = yStart;
	auto xIndent = manager().tableXIndentPx;
	for(size_t i = startYCell; i < endYCell; i++)
	{
		auto rect = IG::makeWindowRectRel({x, y}, {viewRect().xSize(), yCellSize});
		drawElement(cmds, i, item(*this, i), rect, xIndent);
		y += yCellSize;
	}
	cmds.setClipTest(false);
}

void TableView::place()
{
	auto cells_ = items(*this);
	for(auto i : iotaCount(cells_))
	{
		//logMsg("compile item %d", i);
		item(*this, i).compile(renderer());
	}
	if(cells_)
	{
		setYCellSize(IG::makeEvenRoundedUp(item(*this, 0).ySize()*2));
		visibleCells = IG::divRoundUp(displayRect().ySize(), yCellSize) + 1;
		scrollToFocusRect();
	}
	else
		visibleCells = 0;
}

void TableView::onShow()
{
	ScrollView::onShow();
}

void TableView::onHide()
{
	ScrollView::onHide();
}

void TableView::onAddedToController(ViewController *, const Input::Event &e)
{
	if(e.keyEvent())
	{
		auto cells = items(*this);
		if(!cells)
			return;
		selected = nextSelectableElement(0, cells);
	}
}

void TableView::setFocus(bool focused)
{
	hasFocus = focused;
}

void TableView::setOnSelectElement(SelectElementDelegate del)
{
	selectElementDel = del;
}

void TableView::setScrollableIfNeeded(bool on)
{
	onlyScrollIfNeeded = on;
}

void TableView::scrollToFocusRect()
{
	if(selected < 0)
		return;
	int topFocus = yCellSize * selected;
	int bottomFocus = topFocus + yCellSize;
	if(topFocus < scrollOffset())
	{
		setScrollOffset(topFocus);
	}
	else if(bottomFocus > scrollOffset() + viewRect().ySize())
	{
		setScrollOffset(bottomFocus - viewRect().ySize());
	}
}

void TableView::resetScroll()
{
	setScrollOffset(0);
}

bool TableView::inputEvent(const Input::Event &e)
{
	bool handleScroll = !onlyScrollIfNeeded || contentIsBiggerThanView;
	auto motionEv = e.motionEvent();
	if(motionEv && handleScroll && scrollInputEvent(*motionEv))
	{
		selected = -1;
		return true;
	}
	bool movedSelected = false;
	if(handleTableInput(e, movedSelected))
	{
		if(movedSelected && handleScroll && !motionEv)
		{
			scrollToFocusRect();
		}
		return true;
	}
	return false;
}

void TableView::clearSelection()
{
	selected = -1;
}

void TableView::setYCellSize(int s)
{
	yCellSize = s;
	ScrollView::setContentSize({viewRect().xSize(), (int)items(*this) * s});
}

IG::WindowRect TableView::focusRect()
{
	if(selected >= 0)
		return makeWindowRectRel(viewRect().pos(LT2DO) + WPt{0, yCellSize*selected}, {viewRect().xSize(), yCellSize});
	else
		return {};
}

int TableView::nextSelectableElement(int start, int items)
{
	int elem = wrapMinMax(start, 0, items);
	for(auto i : iotaCount(items))
	{
		if(elementIsSelectable(item(*this, elem)))
		{
			return elem;
		}
		elem = wrapMinMax(elem+1, 0, items);
	}
	return -1;
}

int TableView::prevSelectableElement(int start, int items)
{
	int elem = wrapMinMax(start, 0, items);
	for(auto i : iotaCount(items))
	{
		if(elementIsSelectable(item(*this, elem)))
		{
			return elem;
		}
		elem = wrapMinMax(elem-1, 0, items);
	}
	return -1;
}

bool TableView::handleTableInput(const Input::Event &e, bool &movedSelected)
{
	ssize_t cells_ = items(*this);
	return visit(overloaded
	{
		[&](const Input::KeyEvent &keyEv)
		{
			if(!cells_)
			{
				if(keyEv.pushed(Input::DefaultKey::UP) && moveFocusToNextView(e, CT2DO))
				{
					return true;
				}
				else if(keyEv.pushed(Input::DefaultKey::DOWN) && moveFocusToNextView(e, CB2DO))
				{
					return true;
				}
				else
				{
					return false;
				}
			}
			if(keyEv.pushed(Input::DefaultKey::UP))
			{
				if(!hasFocus)
				{
					logMsg("gained focus from key input");
					hasFocus = true;
					if(selected != -1)
					{
						postDraw();
						return true;
					}
				}
				//logMsg("move up %d", selected);
				if(selected == -1)
					selected = prevSelectableElement(cells_ - 1, cells_);
				else
				{
					auto prevSelected = selected;
					selected = prevSelectableElement(selected - 1, cells_);
					if(selected > prevSelected || cells_ == 1)
					{
						if(keyEv.repeated())
						{
							selected = prevSelected;
							return true;
						}
						else if(moveFocusToNextView(e, CT2DO))
						{
							selected = -1;
							return true;
						}
					}
				}
				logMsg("up, selected %d", selected);
				postDraw();
				movedSelected = true;
				return true;
			}
			else if(keyEv.pushed(Input::DefaultKey::DOWN))
			{
				if(!hasFocus)
				{
					logMsg("gained focus from key input");
					hasFocus = true;
					if(selected != -1)
					{
						postDraw();
						return true;
					}
				}
				if(selected == -1)
					selected = nextSelectableElement(0, cells_);
				else
				{
					auto prevSelected = selected;
					selected = nextSelectableElement(selected + 1, cells_);
					if(selected < prevSelected || cells_ == 1)
					{
						if(keyEv.repeated())
						{
							selected = prevSelected;
							return true;
						}
						else if(moveFocusToNextView(e, CB2DO))
						{
							selected = -1;
							return true;
						}
					}
				}
				logMsg("down, selected %d", selected);
				postDraw();
				movedSelected = true;
				return true;
			}
			else if(keyEv.pushed(Input::DefaultKey::CONFIRM))
			{
				if(selected != -1)
				{
					logDMsg("entry %d pushed", selected);
					selectedIsActivated = true;
					onSelectElement(e, selected, item(*this, selected));
				}
				return true;
			}
			else if(keyEv.pushed(Input::DefaultKey::PAGE_UP))
			{
				if(selected == -1)
					selected = cells_ - 1;
				else
					selected = std::clamp(selected - visibleCells, 0, (int)cells_ - 1);
				logMsg("selected %d", selected);
				postDraw();
				movedSelected = true;
				return true;
			}
			else if(keyEv.pushed(Input::DefaultKey::PAGE_DOWN))
			{
				if(selected == -1)
					selected = 0;
				else
					selected = std::clamp(selected + visibleCells, 0, (int)cells_ - 1);
				logMsg("selected %d", selected);
				postDraw();
				movedSelected = true;
				return true;
			}
			return false;
		},
		[&](const Input::MotionEvent &motionEv)
		{
			if(!cells_ || !motionEv.isAbsolute())
				return false;
			if(!pointIsInView(motionEv.pos()) || !(motionEv.mapKey() & Input::Pointer::LBUTTON))
			{
				//logMsg("cursor not in table");
				return false;
			}
			int i = ((motionEv.pos().y + scrollOffset()) - viewRect().y) / yCellSize;
			if(i < 0 || i >= cells_)
			{
				//logMsg("pushed outside of item bounds");
				return false;
			}
			auto &it = item(*this, i);
			if(motionEv.pushed())
			{
				//logMsg("input pushed on cell %d", i);
				hasFocus = true;
				if(i >= 0 && i < cells_ && elementIsSelectable(it))
				{
					selected = i;
					postDraw();
				}
			}
			else if(motionEv.isOff()) // TODO, need to check that Input::PUSHED was sent on entry
			{
				//logMsg("input released on cell %d", i);
				if(i >= 0 && i < cells_ && selected == i && elementIsSelectable(it))
				{
					postDraw();
					selected = -1;
					if(!motionEv.canceled())
					{
						logDMsg("entry %d pushed", i);
						selectedIsActivated = true;
						onSelectElement(e, i, it);
					}
				}
			}
			return true;
		}
	}, e);
}

void TableView::drawElement(Gfx::RendererCommands &__restrict__ cmds, size_t i, MenuItem &item, WRect rect, int xIndent) const
{
	static constexpr Gfx::Color highlightColor{0.f, .8f, 1.f};
	item.draw(cmds, rect.x, rect.pos(C2DO).y, rect.xSize(), rect.ySize(), xIndent, align,
		item.highlighted() ? highlightColor : Gfx::Color(Gfx::ColorName::WHITE));
}

void TableView::onSelectElement(const Input::Event &e, size_t i, MenuItem &item)
{
	if(selectElementDel)
		selectElementDel(e, i, item);
	else
		item.select(*this, e);
}

bool TableView::elementIsSelectable(MenuItem &item)
{
	return item.selectable();
}

std::u16string_view TableView::name() const
{
	return nameStr;
}

}
