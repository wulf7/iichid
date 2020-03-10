/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2020 Vladimir Kondratyev <wulf@FreeBSD.org>
 * Copyright (c) 2020 Greg V <greg@unrelenting.technology>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * Generic HID game controller (joystick/gamepad) driver,
 * also supports XBox 360 gamepads thanks to the custom descriptor in usbhid.
 * 
 * Tested on: SVEN GC-5070 in both XInput (XBox 360) and DirectInput modes
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "usbdevs.h"
#include "hid.h"
#include "hidbus.h"
#include "hmap.h"

#define	HID_DEBUG_VAR	hgame_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hgame_debug = 1;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hgame, CTLFLAG_RW, 0,
		"Generic HID joystick/gamepad");
SYSCTL_INT(_hw_hid_hgame, OID_AUTO, debug, CTLFLAG_RWTUN,
		&hgame_debug, 0, "Debug level");
#endif

static hmap_cb_t hgame_dpad_cb;

#define HGAME_MAP_BUT(base, number)	\
	HMAP_KEY(#number, HID_USAGE2(HUP_BUTTON, number), base + number - 1)
#define HGAME_MAP_BUT_CUSTOM(number, code)	\
	HMAP_KEY(#number, HID_USAGE2(HUP_BUTTON, number), code)
#define HGAME_MAP_ABS(usage, code)        \
	HMAP_ABS(#usage, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_##usage), code)
#define HGAME_MAP_ABS_CB(usage, callback)        \
	HMAP_ABS_CB(#usage, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_##usage), callback)

#ifndef HUG_D_PAD_UP
#define HUG_D_PAD_UP 0x90
#define HUG_D_PAD_DOWN 0x91
#define HUG_D_PAD_RIGHT 0x92
#define HUG_D_PAD_LEFT 0x93
#endif

#define BASE_OVERFLOW (BTN_TRIGGER_HAPPY - 0x10)

static const struct hmap_item hgame_common_map[] = {
	{ HGAME_MAP_BUT(BASE_OVERFLOW, 17) },
	{ HGAME_MAP_BUT(BASE_OVERFLOW, 18) },
	{ HGAME_MAP_BUT(BASE_OVERFLOW, 19) },
	{ HGAME_MAP_BUT(BASE_OVERFLOW, 20) },
	{ HGAME_MAP_BUT(BASE_OVERFLOW, 21) },
	{ HGAME_MAP_BUT(BASE_OVERFLOW, 22) },
	{ HGAME_MAP_BUT(BASE_OVERFLOW, 23) },
	{ HGAME_MAP_BUT(BASE_OVERFLOW, 24) },
	{ HGAME_MAP_ABS(X, ABS_X) },
	{ HGAME_MAP_ABS(Y, ABS_Y) },
	{ HGAME_MAP_ABS(Z, ABS_Z) },
	{ HGAME_MAP_ABS(RX, ABS_RX) },
	{ HGAME_MAP_ABS(RY, ABS_RY) },
	{ HGAME_MAP_ABS(RZ, ABS_RZ) },
	{ HGAME_MAP_ABS(HAT_SWITCH, ABS_HAT0X) },
	{ HGAME_MAP_ABS_CB(D_PAD_UP, hgame_dpad_cb) },
	{ HGAME_MAP_ABS_CB(D_PAD_DOWN, hgame_dpad_cb) },
	{ HGAME_MAP_ABS_CB(D_PAD_RIGHT, hgame_dpad_cb) },
	{ HGAME_MAP_ABS_CB(D_PAD_LEFT, hgame_dpad_cb) },
};

static const struct hmap_item hgame_joystick_map[] = {
	{ HGAME_MAP_BUT(BTN_TRIGGER, 1) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 2) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 3) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 4) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 5) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 6) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 7) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 8) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 9) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 10) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 11) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 12) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 13) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 14) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 15) },
	{ HGAME_MAP_BUT(BTN_TRIGGER, 16) },
};

static const struct hmap_item hgame_gamepad_map[] = {
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 1) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 2) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 3) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 4) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 5) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 6) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 7) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 8) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 9) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 10) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 11) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 12) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 13) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 14) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 15) },
	{ HGAME_MAP_BUT(BTN_GAMEPAD, 16) },
};

/* Customized to match usbhid's XBox 360 descriptor */
static const struct hmap_item hgame_xb360_map[] = {
	{ HGAME_MAP_BUT_CUSTOM(1, BTN_SOUTH) },
	{ HGAME_MAP_BUT_CUSTOM(2, BTN_EAST) },
	{ HGAME_MAP_BUT_CUSTOM(3, BTN_WEST) },
	{ HGAME_MAP_BUT_CUSTOM(4, BTN_NORTH) },
	{ HGAME_MAP_BUT_CUSTOM(5, BTN_TL) },
	{ HGAME_MAP_BUT_CUSTOM(6, BTN_TR) },
	{ HGAME_MAP_BUT_CUSTOM(7, BTN_SELECT) },
	{ HGAME_MAP_BUT_CUSTOM(8, BTN_START) },
	{ HGAME_MAP_BUT_CUSTOM(9, BTN_THUMBL) },
	{ HGAME_MAP_BUT_CUSTOM(10, BTN_THUMBR) },
	{ HGAME_MAP_BUT_CUSTOM(11, BTN_MODE) },
};

#define XBOX_360 360

static const struct hid_device_id hgame_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_JOYSTICK), HID_DRIVER_INFO(HUG_JOYSTICK) },
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_GAME_PAD),
	  HID_VENDOR(USB_VENDOR_MICROSOFT), HID_DRIVER_INFO(XBOX_360) },
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_GAME_PAD), HID_DRIVER_INFO(HUG_GAME_PAD) },
};

struct hgame_softc {
	struct hmap_softc	super_sc;
	int			dpad_up;
	int			dpad_down;
	int			dpad_right;
	int			dpad_left;
};

/* Emulate the hat switch report via the D-pad usages
 * found on XInput/XBox style devices */
static int
hgame_dpad_cb(HMAP_CB_ARGS)
{
	struct hgame_softc *sc = HMAP_CB_GET_SOFTC;
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV;

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		evdev_support_event(evdev, EV_ABS);
		evdev_support_abs(evdev, ABS_HAT0X, 0, -1, 1, 0, 0, 0);
		evdev_support_abs(evdev, ABS_HAT0Y, 0, -1, 1, 0, 0, 0);
		break;
	case HMAP_CB_IS_RUNNING:
		switch (HMAP_CB_GET_MAP_ITEM->usage) {
		case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_D_PAD_UP):
			if (sc->dpad_down)
				break;
			evdev_push_abs(evdev, ABS_HAT0Y, (ctx == 0) ? 0 : -1);
			sc->dpad_up = (ctx != 0);
			break;
		case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_D_PAD_DOWN):
			if (sc->dpad_up)
				break;
			evdev_push_abs(evdev, ABS_HAT0Y, (ctx == 0) ? 0 : 1);
			sc->dpad_down = (ctx != 0);
			break;
		case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_D_PAD_RIGHT):
			if (sc->dpad_left)
				break;
			evdev_push_abs(evdev, ABS_HAT0X, (ctx == 0) ? 0 : 1);
			sc->dpad_right = (ctx != 0);
			break;
		case HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_D_PAD_LEFT):
			if (sc->dpad_right)
				break;
			evdev_push_abs(evdev, ABS_HAT0X, (ctx == 0) ? 0 : -1);
			sc->dpad_left = (ctx != 0);
			break;
		}
	}

	return (0);
}

static int
hgame_probe(device_t dev)
{
	int error, error2;

	error = hidbus_lookup_driver_info(dev, hgame_devs, sizeof(hgame_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);


	if (hidbus_get_driver_info(dev) == HUG_GAME_PAD)
		error = hmap_add_map(dev, hgame_gamepad_map, nitems(hgame_gamepad_map), NULL);
	else if (hidbus_get_driver_info(dev) == XBOX_360)
		error = hmap_add_map(dev, hgame_xb360_map, nitems(hgame_xb360_map), NULL);
	else
		error = hmap_add_map(dev, hgame_joystick_map, nitems(hgame_joystick_map), NULL);
	error2 = hmap_add_map(dev, hgame_common_map, nitems(hgame_common_map), NULL);
	if (error != 0 && error2 != 0)
		return (error);

	hmap_set_evdev_prop(dev, INPUT_PROP_DIRECT);
	hidbus_set_desc(dev,
		(hidbus_get_driver_info(dev) == HUG_GAME_PAD) ? "Gamepad" : "Joystick");

	return (BUS_PROBE_DEFAULT);
}

static devclass_t hgame_devclass;

static device_method_t hgame_methods[] = {
	DEVMETHOD(device_probe, hgame_probe),
	DEVMETHOD_END
};

DEFINE_CLASS_1(hgame, hgame_driver, hgame_methods,
    sizeof(struct hgame_softc), hmap_driver);
DRIVER_MODULE(hgame, hidbus, hgame_driver, hgame_devclass, NULL, 0);
MODULE_DEPEND(hgame, hid, 1, 1, 1);
MODULE_DEPEND(hgame, hmap, 1, 1, 1);
MODULE_DEPEND(hgame, evdev, 1, 1, 1);
MODULE_VERSION(hgame, 1);
