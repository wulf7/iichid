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
 * Generic HID game controller (joystick/gamepad) driver
 *
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

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

static hmap_cb_t	hgame_button_cb;

#define HGAME_MAP_BUT(usage)	\
	HMAP_ABS_CB(#usage, HID_USAGE2(HUP_BUTTON, usage), hgame_button_cb)
#define HGAME_MAP_ABS(usage, code)        \
	HMAP_ABS(#usage, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_##usage), code)

static const struct hmap_item hgame_map[] = {
	{ HGAME_MAP_BUT(1) },
	{ HGAME_MAP_BUT(2) },
	{ HGAME_MAP_BUT(3) },
	{ HGAME_MAP_BUT(4) },
	{ HGAME_MAP_BUT(5) },
	{ HGAME_MAP_BUT(6) },
	{ HGAME_MAP_BUT(7) },
	{ HGAME_MAP_BUT(8) },
	{ HGAME_MAP_BUT(9) },
	{ HGAME_MAP_BUT(10) },
	{ HGAME_MAP_BUT(11) },
	{ HGAME_MAP_BUT(12) },
	{ HGAME_MAP_BUT(13) },
	{ HGAME_MAP_BUT(14) },
	{ HGAME_MAP_BUT(15) },
	{ HGAME_MAP_BUT(16) },
	{ HGAME_MAP_BUT(17) },
	{ HGAME_MAP_BUT(18) },
	{ HGAME_MAP_BUT(19) },
	{ HGAME_MAP_BUT(20) },
	{ HGAME_MAP_BUT(21) },
	{ HGAME_MAP_BUT(22) },
	{ HGAME_MAP_BUT(23) },
	{ HGAME_MAP_BUT(24) },

	{ HGAME_MAP_ABS(X, ABS_X) },
	{ HGAME_MAP_ABS(Y, ABS_Y) },
	{ HGAME_MAP_ABS(Z, ABS_Z) },
	{ HGAME_MAP_ABS(RX, ABS_RX) },
	{ HGAME_MAP_ABS(RY, ABS_RY) },
	{ HGAME_MAP_ABS(RZ, ABS_RZ) },
	{ HGAME_MAP_ABS(HAT_SWITCH, ABS_HAT0X) },
};

static const struct hid_device_id hgame_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_JOYSTICK), HID_DRIVER_INFO(HUG_JOYSTICK) },
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_GAME_PAD), HID_DRIVER_INFO(HUG_GAME_PAD) },
};

static void
hgame_button_cb(HMAP_CB_ARGS)
{
	struct hmap_softc *sc = HMAP_CB_GET_SOFTC;
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV;
	const uint16_t btn = (HMAP_CB_GET_MAP_ITEM->usage & 0xffff) - 1;
	uint16_t base = BTN_TRIGGER; /* == BTN_JOYSTICK */
	int32_t data;

	if (hidbus_get_driver_info(sc->dev) == HUG_GAME_PAD)
		base = BTN_GAMEPAD;

	/* First button over 16 is BTN_TRIGGER_HAPPY */
	if (btn >= 0x10)
		base = BTN_TRIGGER_HAPPY - 0x10;

	if (HMAP_CB_IS_ATTACHING) {
		evdev_support_event(evdev, EV_KEY);
		evdev_support_key(evdev, base + btn);
	} else {
		data = ctx;
		evdev_push_key(evdev, base + btn, data);
	}
}

static int
hgame_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, hgame_devs, sizeof(hgame_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	/* Check if report descriptor belongs to a HID joystick device */
	error = hmap_add_map(dev, hgame_map, nitems(hgame_map), NULL);
	if (error != 0)
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
    sizeof(struct hmap_softc), hmap_driver);
DRIVER_MODULE(hgame, hidbus, hgame_driver, hgame_devclass, NULL, 0);
MODULE_DEPEND(hgame, hid, 1, 1, 1);
MODULE_DEPEND(hgame, hmap, 1, 1, 1);
MODULE_DEPEND(hgame, evdev, 1, 1, 1);
MODULE_VERSION(hgame, 1);
