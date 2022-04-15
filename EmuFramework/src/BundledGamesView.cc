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

#include <emuframework/BundledGamesView.hh>
#include <emuframework/EmuApp.hh>
#include <imagine/logger/logger.h>
#include <imagine/io/FileIO.hh>
#include <imagine/fs/ArchiveFS.hh>
#include <imagine/base/ApplicationContext.hh>

namespace EmuEx
{

BundledGamesView::BundledGamesView(ViewAttachParams attach):
	TableView
	{
		"Bundled Content",
		attach,
		[this](const TableView &)
		{
			return 1;
		},
		[this](const TableView &, size_t idx) -> MenuItem&
		{
			return game[0];
		}
	}
{
	auto &info = system().bundledGameInfo(0);
	game[0] = {info.displayName, &defaultFace(),
		[this, &info](const Input::Event &e)
		{
			auto file = appContext().openAsset(info.assetName, IO::AccessHint::ALL, IO::TEST_BIT);
			if(!file)
			{
				logErr("error opening bundled game asset: %s", info.assetName);
				return;
			}
			app().createSystemWithMedia(std::move(file), info.assetName, info.assetName, e, {}, attachParams(),
				[this](const Input::Event &e)
				{
					app().launchSystemWithResumePrompt(e);
				});
		}};
}

[[gnu::weak]] const BundledGameInfo &EmuSystem::bundledGameInfo(unsigned idx) const
{
	static const BundledGameInfo info[]
	{
		{ "test", "test" }
	};

	return info[0];
}

}
