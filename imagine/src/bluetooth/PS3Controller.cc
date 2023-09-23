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

#define LOGTAG "PS3Ctrl"
#include <imagine/bluetooth/PS3Controller.hh>
#include <imagine/base/Application.hh>
#include <imagine/base/Error.hh>
#include <imagine/logger/logger.h>
#include <imagine/time/Time.hh>
#include <imagine/util/bit.hh>
#include <imagine/util/ranges.hh>
#include "../input/PackedInputAccess.hh"

static constexpr uint32_t CELL_PAD_BTN_OFFSET_DIGITAL1 = 0, CELL_PAD_BTN_OFFSET_DIGITAL2 = 1;

// CELL_PAD_BTN_OFFSET_DIGITAL1
#define CELL_PAD_CTRL_LEFT      (1 << 7)
#define CELL_PAD_CTRL_DOWN      (1 << 6)
#define CELL_PAD_CTRL_RIGHT     (1 << 5)
#define CELL_PAD_CTRL_UP        (1 << 4)
#define CELL_PAD_CTRL_START     (1 << 3)
#define CELL_PAD_CTRL_R3        (1 << 2)
#define CELL_PAD_CTRL_L3        (1 << 1)
#define CELL_PAD_CTRL_SELECT    (1 << 0)

// CELL_PAD_BTN_OFFSET_DIGITAL2
#define CELL_PAD_CTRL_SQUARE    (1 << 7)
#define CELL_PAD_CTRL_CROSS     (1 << 6)
#define CELL_PAD_CTRL_CIRCLE    (1 << 5)
#define CELL_PAD_CTRL_TRIANGLE  (1 << 4)
#define CELL_PAD_CTRL_R1        (1 << 3)
#define CELL_PAD_CTRL_L1        (1 << 2)
#define CELL_PAD_CTRL_R2        (1 << 1)
#define CELL_PAD_CTRL_L2        (1 << 0)

#define CELL_PAD_CTRL_PS        (1 << 0)

namespace IG
{

using namespace IG::Input;

static const PackedInputAccess padDataAccess[] =
{
	{ CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT, PS3::SELECT, Keycode::GAME_SELECT },
	{ CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_L3, PS3::L3, Keycode::GAME_LEFT_THUMB },
	{ CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_R3, PS3::R3, Keycode::GAME_RIGHT_THUMB },
	{ CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START, PS3::START, Keycode::GAME_START },
	{ CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP, PS3::UP, Keycode::UP },
	{ CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT, PS3::RIGHT, Keycode::RIGHT },
	{ CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN, PS3::DOWN, Keycode::DOWN },
	{ CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT, PS3::LEFT, Keycode::LEFT },

	{ CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L2, PS3::L2, Keycode::GAME_L2 },
	{ CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R2, PS3::R2, Keycode::GAME_R2 },
	{ CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1, PS3::L1, Keycode::GAME_L1 },
	{ CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1, PS3::R1, Keycode::GAME_R1 },
	{ CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE, PS3::TRIANGLE, Keycode::GAME_Y },
	{ CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE, PS3::CIRCLE, Keycode::GAME_B },
	{ CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS, PS3::CROSS, Keycode::GAME_A },
	{ CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE, PS3::SQUARE, Keycode::GAME_X },

	{ CELL_PAD_BTN_OFFSET_DIGITAL2+1, CELL_PAD_CTRL_PS, PS3::PS, Keycode::MENU },
};

static const char *ps3ButtonName(Input::Key k)
{
	using namespace IG::Input;
	switch(k)
	{
		case 0: return "None";
		case PS3::CROSS: return "Cross";
		case PS3::CIRCLE: return "Circle";
		case PS3::SQUARE: return "Square";
		case PS3::TRIANGLE: return "Triangle";
		case PS3::L1: return "L1";
		case PS3::L2: return "L2";
		case PS3::L3: return "L3";
		case PS3::R1: return "R1";
		case PS3::R2: return "R2";
		case PS3::R3: return "R3";
		case PS3::SELECT: return "Select";
		case PS3::START: return "Start";
		case PS3::UP: return "Up";
		case PS3::RIGHT: return "Right";
		case PS3::DOWN: return "Down";
		case PS3::LEFT: return "Left";
		case PS3::PS: return "PS";
		case PS3::LSTICK_UP: return "L:Up";
		case PS3::LSTICK_RIGHT: return "L:Right";
		case PS3::LSTICK_DOWN: return "L:Down";
		case PS3::LSTICK_LEFT: return "L:Left";
		case PS3::RSTICK_UP: return "R:Up";
		case PS3::RSTICK_RIGHT: return "R:Right";
		case PS3::RSTICK_DOWN: return "R:Down";
		case PS3::RSTICK_LEFT: return "R:Left";
	}
	return "";
}

PS3Controller::PS3Controller(ApplicationContext ctx, BluetoothAddr addr):
	BluetoothInputDevice{ctx, Input::Map::PS3PAD, {.gamepad = true}, "PS3 Controller"},
	ctlSock{ctx}, intSock{ctx},
	addr{addr}
{}

const char *PS3Controller::keyName(Input::Key k) const
{
	return ps3ButtonName(k);
}

IG::ErrorCode PS3Controller::open(BluetoothAdapter &adapter)
{
	return {ENOTSUP};
}

IG::ErrorCode PS3Controller::open1Ctl(BluetoothAdapter &adapter, BluetoothPendingSocket &pending)
{
	ctlSock.onData() = intSock.onData() =
		[this](const char *packet, size_t size)
		{
			return dataHandler(packet, size);
		};
	ctlSock.onStatus() = intSock.onStatus() =
		[this](BluetoothSocket &sock, uint32_t status)
		{
			return statusHandler(sock, status);
		};
	logMsg("accepting PS3 control channel");
	if(auto err = ctlSock.open(adapter, pending);
		err)
	{
		logErr("error opening control socket");
		return err;
	}
	return {};
}

IG::ErrorCode PS3Controller::open2Int(BluetoothAdapter &adapter, BluetoothPendingSocket &pending)
{
	logMsg("accepting PS3 interrupt channel");
	if(auto err = intSock.open(adapter, pending);
		err)
	{
		logErr("error opening interrupt socket");
		return err;
	}
	return {};
}

uint32_t PS3Controller::statusHandler(BluetoothSocket &sock, uint32_t status)
{
	if(status == BluetoothSocket::STATUS_OPENED && &sock == (BluetoothSocket*)&ctlSock)
	{
		logMsg("opened PS3 control socket, waiting for interrupt socket");
		return 0; // don't add ctlSock to event loop
	}
	else if(status == BluetoothSocket::STATUS_OPENED && &sock == (BluetoothSocket*)&intSock)
	{
		logMsg("PS3 controller opened successfully");
		ctx.application().bluetoothInputDeviceStatus(ctx, *this, status);
		sendFeatureReport();
		return BluetoothSocket::OPEN_USAGE_READ_EVENTS;
	}
	else if(status == BluetoothSocket::STATUS_CONNECT_ERROR)
	{
		logErr("PS3 controller connection error");
		ctx.application().bluetoothInputDeviceStatus(ctx, *this, status);
	}
	else if(status == BluetoothSocket::STATUS_READ_ERROR)
	{
		logErr("PS3 controller read error, disconnecting");
		ctx.application().bluetoothInputDeviceStatus(ctx, *this, status);
	}
	return 1;
}

void PS3Controller::close()
{
	intSock.close();
	ctlSock.close();
}

bool PS3Controller::dataHandler(const char *packetPtr, size_t size)
{
	auto packet = (const uint8_t*)packetPtr;
	/*logMsg("data with size %d", (int)size);
	iterateTimes(size, i)
	{
		logger_printf(0, "0x%X ", packet[i]);
	}
	if(size)
		logger_printf(0, "\n");*/
	if(!didSetLEDs) [[unlikely]]
	{
		setLEDs(enumId());
		didSetLEDs = true;
	}

	switch(packet[0])
	{
		case 0xA1:
		{
			auto time = SteadyClock::now();
			const uint8_t *digitalBtnData = &packet[3];
			for(auto &e : padDataAccess)
			{
				int newState = e.updateState(prevData, digitalBtnData);
				if(newState != -1)
				{
					//logMsg("%s %s @ PS3 Pad %d", device->keyName(e.keyEvent), newState ? "pushed" : "released", player);
					ctx.endIdleByUserActivity();
					KeyEvent event{Map::PS3PAD, e.keyEvent, e.sysKey, newState ? Action::PUSHED : Action::RELEASED, 0, 0, Source::GAMEPAD, time, this};
					ctx.application().dispatchRepeatableKeyInputEvent(event);
				}
			}
			memcpy(prevData, digitalBtnData, sizeof(prevData));

			const uint8_t *stickData = &packet[7];
			//logMsg("left: %d,%d right: %d,%d", stickData[0], stickData[1], stickData[2], stickData[3]);
			for(auto i : iotaCount(4))
			{
				if(axis[i].update(int(stickData[i]) - 127, Map::PS3PAD, time, *this, ctx.mainWindow()))
					ctx.endIdleByUserActivity();
			}
		}
	}

	return 1;
}

static constexpr uint32_t HIDP_TRANSACTION_SET_REPORT = 0x50;
static constexpr uint32_t HIDP_DATA_HEADER_RTYPE_OUTPUT = 0x02;
static constexpr uint32_t HIDP_DATA_HEADER_RTYPE_FEATURE = 0x03;

void PS3Controller::sendFeatureReport()
{
	logMsg("sending feature report");
	const uint8_t featureReport[]
	{
		HIDP_TRANSACTION_SET_REPORT | HIDP_DATA_HEADER_RTYPE_FEATURE,
		0xf4, 0x42, 0x03, 0x00, 0x00
	};
	ctlSock.write(featureReport, sizeof(featureReport));
}

void PS3Controller::setLEDs(uint32_t player)
{
	logMsg("setting LEDs for player %d", player);
	uint8_t setLEDs[] =
	{
		HIDP_TRANSACTION_SET_REPORT | HIDP_DATA_HEADER_RTYPE_OUTPUT,
		0x01,
		0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00,
		0xff, 0x27, 0x10, 0x00, 0x32,
		0xff, 0x27, 0x10, 0x00, 0x32,
		0xff, 0x27, 0x10, 0x00, 0x32,
		0xff, 0x27, 0x10, 0x00, 0x32,
		0x00, 0x00, 0x00, 0x00, 0x00
	};
	setLEDs[11] = playerLEDs(player);
	ctlSock.write(setLEDs, sizeof(setLEDs));
}

uint8_t PS3Controller::playerLEDs(uint32_t player)
{
	switch(player)
	{
		default:
		case 0: return bit(1);
		case 1: return bit(2);
		case 2: return bit(3);
		case 3: return bit(4);
		case 4: return bit(4) | bit(1);
	}
}

std::span<Input::Axis> PS3Controller::motionAxes()
{
	return axis;
}

std::pair<Input::Key, Input::Key> PS3Controller::joystickKeys(Input::AxisId axisId)
{
	switch(axisId)
	{
		case Input::AxisId::X: return {Input::PS3::LSTICK_LEFT, Input::PS3::LSTICK_RIGHT};
		case Input::AxisId::Y: return {Input::PS3::LSTICK_DOWN, Input::PS3::LSTICK_UP};
		case Input::AxisId::Z: return {Input::PS3::RSTICK_LEFT, Input::PS3::RSTICK_RIGHT};
		case Input::AxisId::RZ: return {Input::PS3::RSTICK_DOWN, Input::PS3::RSTICK_UP};
		default: return {};
	}
}

}
