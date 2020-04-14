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
#include <dev/usb/input/usb_rdesc.h>

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

static const uint8_t	hgame_xb360gp_rdesc[] = {UHID_XB360GP_REPORT_DESCR()};

static hmap_cb_t	hgame_dpad_cb;
static hmap_cb_t	hgame_compl_cb;

#define HGAME_MAP_BUT(number, code)	\
	{ HMAP_KEY(#number, HUP_BUTTON, number, code) }
#define HGAME_MAP_BUT_RG(number_from, number_to, code)	\
	{ HMAP_KEY_RANGE(#code, HUP_BUTTON, number_from, number_to, code) }
#define HGAME_MAP_ABS(usage, code)	\
	{ HMAP_ABS(#usage, HUP_GENERIC_DESKTOP, HUG_##usage, code) }
#define HGAME_MAP_ABS_CB(usage, callback)	\
	{ HMAP_ABS_CB(#usage, HUP_GENERIC_DESKTOP, HUG_##usage, callback) }
#define HGAME_COMPL_CB(cb)		\
	{ HMAP_COMPL_CB("COMPL_CB", &cb) }

#ifndef HUG_D_PAD_UP
#define	HUG_D_PAD_UP	0x90
#define	HUG_D_PAD_DOWN	0x91
#define	HUG_D_PAD_RIGHT	0x92
#define	HUG_D_PAD_LEFT	0x93
#endif

static const struct hmap_item hgame_common_map[] = {
	HGAME_MAP_ABS(X,		ABS_X),
	HGAME_MAP_ABS(Y,		ABS_Y),
	HGAME_MAP_ABS(Z,		ABS_Z),
	HGAME_MAP_ABS(RX,		ABS_RX),
	HGAME_MAP_ABS(RY,		ABS_RY),
	HGAME_MAP_ABS(RZ,		ABS_RZ),
	HGAME_MAP_ABS(HAT_SWITCH,	ABS_HAT0X),
	HGAME_MAP_ABS_CB(D_PAD_UP,	hgame_dpad_cb),
	HGAME_MAP_ABS_CB(D_PAD_DOWN,	hgame_dpad_cb),
	HGAME_MAP_ABS_CB(D_PAD_RIGHT,	hgame_dpad_cb),
	HGAME_MAP_ABS_CB(D_PAD_LEFT,	hgame_dpad_cb),
	HGAME_MAP_BUT_RG(17, 57,	BTN_TRIGGER_HAPPY),
	HGAME_COMPL_CB(hgame_compl_cb),
};

static const struct hmap_item hgame_joystick_map[] = {
	HGAME_MAP_BUT_RG(1, 16,		BTN_TRIGGER),
};

static const struct hmap_item hgame_gamepad_map[] = {
	HGAME_MAP_BUT_RG(1, 16,		BTN_GAMEPAD),
};

/* Customized to match usbhid's XBox 360 descriptor */
static const struct hmap_item hgame_xb360_map[] = {
	HGAME_MAP_BUT(1,		BTN_SOUTH),
	HGAME_MAP_BUT(2,		BTN_EAST),
	HGAME_MAP_BUT(3,		BTN_WEST),
	HGAME_MAP_BUT(4,		BTN_NORTH),
	HGAME_MAP_BUT(5,		BTN_TL),
	HGAME_MAP_BUT(6,		BTN_TR),
	HGAME_MAP_BUT(7,		BTN_SELECT),
	HGAME_MAP_BUT(8,		BTN_START),
	HGAME_MAP_BUT(9,		BTN_THUMBL),
	HGAME_MAP_BUT(10,		BTN_THUMBR),
	HGAME_MAP_BUT(11,		BTN_MODE),
};

static const struct hid_device_id hgame_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_JOYSTICK), HID_DRIVER_INFO(HUG_JOYSTICK) },
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_GAME_PAD), HID_DRIVER_INFO(HUG_GAME_PAD) },
};

struct hgame_softc {
	struct hmap_softc	super_sc;
	bool			dpad_up;
	bool			dpad_down;
	bool			dpad_right;
	bool			dpad_left;
};

/* Emulate the hat switch report via the D-pad usages
 * found on XInput/XBox style devices */
static int
hgame_dpad_cb(HMAP_CB_ARGS)
{
	struct hgame_softc *sc = HMAP_CB_GET_SOFTC();
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		evdev_support_event(evdev, EV_ABS);
		evdev_support_abs(evdev, ABS_HAT0X, 0, -1, 1, 0, 0, 0);
		evdev_support_abs(evdev, ABS_HAT0Y, 0, -1, 1, 0, 0, 0);
		break;

	case HMAP_CB_IS_RUNNING:
		switch (HMAP_CB_MAP_ITEM->usage) {
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
hgame_compl_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING)
		evdev_support_prop(evdev, INPUT_PROP_DIRECT);

        /* Do not execute callback at interrupt handler and detach */
        return (ENOSYS);
}

static void
hgame_identify(driver_t *driver, device_t parent)
{
	const struct hid_device_info *hw = hid_get_device_info(parent);

	/* the Xbox 360 gamepad has no report descriptor */
	if (hw->isXBox360GP)
		hid_set_report_descr(parent, hgame_xb360gp_rdesc,
		    sizeof(hgame_xb360gp_rdesc));
}

static int
hgame_probe(device_t dev)
{
	const struct hid_device_info *hw = hid_get_device_info(dev);
	int error, error2;

	error = hidbus_lookup_driver_info(dev, hgame_devs, sizeof(hgame_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	if (hidbus_get_driver_info(dev) == HUG_GAME_PAD)
		error = hmap_add_map(dev, hgame_gamepad_map, nitems(hgame_gamepad_map), NULL);
	else if (hw->isXBox360GP)
		error = hmap_add_map(dev, hgame_xb360_map, nitems(hgame_xb360_map), NULL);
	else
		error = hmap_add_map(dev, hgame_joystick_map, nitems(hgame_joystick_map), NULL);
	error2 = hmap_add_map(dev, hgame_common_map, nitems(hgame_common_map), NULL);
	if (error != 0 && error2 != 0)
		return (error);

	return (BUS_PROBE_DEFAULT);
}

static int
hgame_attach(device_t dev)
{
	const struct hid_device_info *hw = hid_get_device_info(dev);
	int error;

	hidbus_set_desc(dev, hidbus_get_driver_info(dev) == HUG_GAME_PAD ?
	    "Gamepad" : "Joystick");

	if (hidbus_get_driver_info(dev) == HUG_GAME_PAD && hw->isXBox360GP) {
		/*
		 * Turn off the four LEDs on the gamepad which
		 * are blinking by default:
		 */
		static const uint8_t reportbuf[3] = {1, 3, 0};
		error = hid_set_report(dev, reportbuf, sizeof(reportbuf),
		    HID_OUTPUT_REPORT, 0);
		if (error)
                        DPRINTF("set output report failed, error=%d "
                            "(ignored)\n", error);
	}

	return (hmap_attach(dev));
}

static devclass_t hgame_devclass;

static device_method_t hgame_methods[] = {
	DEVMETHOD(device_identify,	hgame_identify),
	DEVMETHOD(device_probe,		hgame_probe),
	DEVMETHOD(device_attach,	hgame_attach),
	DEVMETHOD_END
};

DEFINE_CLASS_1(hgame, hgame_driver, hgame_methods,
    sizeof(struct hgame_softc), hmap_driver);
DRIVER_MODULE(hgame, hidbus, hgame_driver, hgame_devclass, NULL, 0);
MODULE_DEPEND(hgame, hid, 1, 1, 1);
MODULE_DEPEND(hgame, hmap, 1, 1, 1);
MODULE_DEPEND(hgame, evdev, 1, 1, 1);
MODULE_VERSION(hgame, 1);
