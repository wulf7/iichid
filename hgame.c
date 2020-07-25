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
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "hgame.h"
#include "hid.h"
#include "hidbus.h"
#include "hid_quirk.h"
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

#define HGAME_MAP_BRG(number_from, number_to, code)	\
	{ HMAP_KEY_RANGE(HUP_BUTTON, number_from, number_to, code) }
#define HGAME_MAP_ABS(usage, code)	\
	{ HMAP_ABS(HUP_GENERIC_DESKTOP, HUG_##usage, code) }
#define HGAME_MAP_CRG(usage_from, usage_to, callback)	\
	{ HMAP_ANY_CB_RANGE(HUP_GENERIC_DESKTOP,	\
	    HUG_##usage_from, HUG_##usage_to, callback) }
#define HGAME_COMPLCB(cb)	\
	{ HMAP_COMPL_CB(&cb) }

static const struct hmap_item hgame_common_map[] = {
	HGAME_MAP_ABS(X,		ABS_X),
	HGAME_MAP_ABS(Y,		ABS_Y),
	HGAME_MAP_ABS(Z,		ABS_Z),
	HGAME_MAP_ABS(RX,		ABS_RX),
	HGAME_MAP_ABS(RY,		ABS_RY),
	HGAME_MAP_ABS(RZ,		ABS_RZ),
	HGAME_MAP_ABS(HAT_SWITCH,	ABS_HAT0X),
	HGAME_MAP_CRG(D_PAD_UP, D_PAD_LEFT, hgame_dpad_cb),
	HGAME_MAP_BRG(17, 57,		BTN_TRIGGER_HAPPY),
	HGAME_COMPLCB(			hgame_compl_cb),
};

static const struct hmap_item hgame_joystick_map[] = {
	HGAME_MAP_BRG(1, 16,		BTN_TRIGGER),
};

static const struct hmap_item hgame_gamepad_map[] = {
	HGAME_MAP_BRG(1, 16,		BTN_GAMEPAD),
};

static const struct hid_device_id hgame_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_JOYSTICK), HID_DRIVER_INFO(HUG_JOYSTICK) },
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_GAME_PAD), HID_DRIVER_INFO(HUG_GAME_PAD) },
};

/* Emulate the hat switch report via the D-pad usages
 * found on XInput/XBox style devices */
int
hgame_dpad_cb(HMAP_CB_ARGS)
{
	struct hgame_softc *sc = HMAP_CB_GET_SOFTC();
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();
	struct hid_item *hid_item;

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		hid_item = (struct hid_item *)ctx;
		HMAP_CB_UDATA64 = HID_GET_USAGE(hid_item->usage);
		evdev_support_event(evdev, EV_ABS);
		evdev_support_abs(evdev, ABS_HAT0X, 0, -1, 1, 0, 0, 0);
		evdev_support_abs(evdev, ABS_HAT0Y, 0, -1, 1, 0, 0, 0);
		break;

	case HMAP_CB_IS_RUNNING:
		switch (HMAP_CB_UDATA64) {
		case HUG_D_PAD_UP:
			if (sc->dpad_down)
				break;
			evdev_push_abs(evdev, ABS_HAT0Y, (ctx == 0) ? 0 : -1);
			sc->dpad_up = (ctx != 0);
			break;
		case HUG_D_PAD_DOWN:
			if (sc->dpad_up)
				break;
			evdev_push_abs(evdev, ABS_HAT0Y, (ctx == 0) ? 0 : 1);
			sc->dpad_down = (ctx != 0);
			break;
		case HUG_D_PAD_RIGHT:
			if (sc->dpad_left)
				break;
			evdev_push_abs(evdev, ABS_HAT0X, (ctx == 0) ? 0 : 1);
			sc->dpad_right = (ctx != 0);
			break;
		case HUG_D_PAD_LEFT:
			if (sc->dpad_right)
				break;
			evdev_push_abs(evdev, ABS_HAT0X, (ctx == 0) ? 0 : -1);
			sc->dpad_left = (ctx != 0);
			break;
		}
	}

	return (0);
}

int
hgame_compl_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING)
		evdev_support_prop(evdev, INPUT_PROP_DIRECT);

	/* Do not execute callback at interrupt handler and detach */
	return (ENOSYS);
}

static int
hgame_probe(device_t dev)
{
	const struct hid_device_info *hw = hid_get_device_info(dev);
	struct hgame_softc *sc = device_get_softc(dev);
	int error, error2;

	if (hid_test_quirk(hw, HQ_IS_XBOX360GP))
		return(ENXIO);

	error = hidbus_lookup_driver_info(dev, hgame_devs, sizeof(hgame_devs));
	if (error != 0)
		return (error);

	hmap_set_dev(&sc->hm, dev);
	hmap_set_debug_var(&sc->hm, &HID_DEBUG_VAR);

	if (hidbus_get_driver_info(dev) == HUG_GAME_PAD)
		error = hmap_add_map(&sc->hm, hgame_gamepad_map, nitems(hgame_gamepad_map), NULL);
	else
		error = hmap_add_map(&sc->hm, hgame_joystick_map, nitems(hgame_joystick_map), NULL);
	error2 = hmap_add_map(&sc->hm, hgame_common_map, nitems(hgame_common_map), NULL);
	if (error != 0 && error2 != 0)
		return (error);

	hidbus_set_desc(dev, hidbus_get_driver_info(dev) == HUG_GAME_PAD ?
	    "Gamepad" : "Joystick");

	return (BUS_PROBE_GENERIC);
}

static int
hgame_attach(device_t dev)
{
	struct hgame_softc *sc = device_get_softc(dev);

	return (hmap_attach(&sc->hm));
}

static int
hgame_detach(device_t dev)
{
	struct hgame_softc *sc = device_get_softc(dev);

	return (hmap_detach(&sc->hm));
}

static devclass_t hgame_devclass;
static device_method_t hgame_methods[] = {
	DEVMETHOD(device_probe,		hgame_probe),
	DEVMETHOD(device_attach,	hgame_attach),
	DEVMETHOD(device_detach,	hgame_detach),

	DEVMETHOD_END
};

DEFINE_CLASS_0(hgame, hgame_driver, hgame_methods, sizeof(struct hgame_softc));
DRIVER_MODULE(hgame, hidbus, hgame_driver, hgame_devclass, NULL, 0);
MODULE_DEPEND(hgame, hid, 1, 1, 1);
MODULE_DEPEND(hgame, hmap, 1, 1, 1);
MODULE_DEPEND(hgame, evdev, 1, 1, 1);
MODULE_VERSION(hgame, 1);
