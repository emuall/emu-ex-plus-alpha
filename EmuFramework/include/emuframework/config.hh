#pragma once

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

#include <imagine/config/env.hh>
#include <imagine/input/config.hh>
#include <imagine/bluetooth/config.hh>

#ifdef ENV_NOTE
#define PLATFORM_INFO_STR ENV_NOTE " (" CONFIG_ARCH_STR ")"
#else
#define PLATFORM_INFO_STR "(" CONFIG_ARCH_STR ")"
#endif
#define CREDITS_INFO_STRING "Built : " __DATE__ "\n" PLATFORM_INFO_STR "\n\n"

#if defined CONFIG_INPUT_KEYBOARD_DEVICES
#define CONFIG_INPUT_ICADE
#endif


namespace EmuEx
{

#if defined CONFIG_INPUT_POINTING_DEVICES
#define CONFIG_EMUFRAMEWORK_VCONTROLS
#endif
constexpr bool VCONTROLS = Config::Input::POINTING_DEVICES;

#if defined __ANDROID__ || \
	defined CONFIG_BASE_IOS || \
	(defined CONFIG_BASE_X11 && !defined CONFIG_MACHINE_PANDORA)
#define CONFIG_VCONTROLS_GAMEPAD
constexpr bool VCONTROLS_GAMEPAD = true;
#else
constexpr bool VCONTROLS_GAMEPAD = false;
#endif

constexpr bool HAS_MULTIPLE_WINDOW_PIXEL_FORMATS = Config::envIsLinux || Config::envIsAndroid || Config::envIsIOS;

#ifdef __ANDROID__
#define CONFIG_INPUT_ANDROID_MOGA
constexpr bool MOGA_INPUT = false;
#else
constexpr bool MOGA_INPUT = false;
#endif

constexpr bool CAN_HIDE_TITLE_BAR = !Config::envIsIOS;

}
