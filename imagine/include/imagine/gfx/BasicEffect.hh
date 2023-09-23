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

#include <imagine/gfx/defs.hh>

#ifdef CONFIG_GFX_OPENGL
#include <imagine/gfx/opengl/GLBasicEffect.hh>
#endif

namespace IG::Gfx
{

class RendererCommands;
class Mat4;
class Texture;

class BasicEffect : public BasicEffectImpl
{
public:
	BasicEffect &operator=(BasicEffect &&) = delete;
	void enableTexture(RendererCommands &, const Texture &);
	void enableTexture(RendererCommands &, TextureBinding);
	void enableTexture(RendererCommands &);
	void enableAlphaTexture(RendererCommands &);
	void disableTexture(RendererCommands &);
	void setModelView(RendererCommands &, Mat4);
	void setProjection(RendererCommands &, Mat4);
	void setModelViewProjection(RendererCommands &, Mat4 modelView, Mat4 proj);
	void prepareDraw(RendererCommands &);
};

}
