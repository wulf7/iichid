/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
 * Copyright (c) 2019 Greg V <greg@unrelenting.technology>
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
 * Generic / MS Windows compatible HID pen tablet driver:
 * https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/required-hid-top-level-collections
 *
 * Tested on: Wacom WCOM50C1 (Google Pixelbook "eve")
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

#define	HID_DEBUG_VAR	hpen_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hpen_debug = 1;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hpen, CTLFLAG_RW, 0,
		"Generic HID tablet");
SYSCTL_INT(_hw_hid_hpen, OID_AUTO, debug, CTLFLAG_RWTUN,
		&hpen_debug, 0, "Debug level");
#endif

static hmap_cb_t	hpen_battery_strenght_cb;
static hmap_cb_t	hpen_compl_cb;

#define HPEN_MAP_BUT(usage, code)        \
	HMAP_KEY(#usage, HUP_DIGITIZERS, HUD_##usage, code)
#define HPEN_MAP_ABS(usage, code)        \
	HMAP_ABS(#usage, HUP_DIGITIZERS, HUD_##usage, code)
#define HPEN_MAP_ABS_GD(usage, code)        \
	HMAP_ABS(#usage, HUP_GENERIC_DESKTOP, HUG_##usage, code)
#define HPEN_MAP_ABS_CB(usage, cb)       \
	HMAP_ABS_CB(#usage, HUP_DIGITIZERS, HUD_##usage, cb)

static const struct hmap_item hpen_map[] = {
    { HPEN_MAP_ABS_GD(X,		ABS_X),		  .required = true },
    { HPEN_MAP_ABS_GD(Y,		ABS_Y),		  .required = true },
    { HPEN_MAP_ABS(   TIP_PRESSURE,	ABS_PRESSURE),	  .required = true },
    { HPEN_MAP_ABS(   X_TILT,		ABS_TILT_X) },
    { HPEN_MAP_ABS(   Y_TILT,		ABS_TILT_Y) },
    { HPEN_MAP_ABS_CB(BATTERY_STRENGTH,	hpen_battery_strenght_cb) },
    { HPEN_MAP_BUT(   TIP_SWITCH,	BTN_TOUCH),	  .required = true },
    { HPEN_MAP_BUT(   IN_RANGE,		BTN_TOOL_PEN),	  .required = true },
    { HPEN_MAP_BUT(   BARREL_SWITCH,	BTN_STYLUS) },
    { HPEN_MAP_BUT(   INVERT,		BTN_TOOL_RUBBER), .required = true },
    { HPEN_MAP_BUT(   ERASER,		BTN_TOUCH),	  .required = true },
    { HMAP_COMPL_CB(  "COMPL_CB",	&hpen_compl_cb) },
};

static const struct hid_device_id hpen_devs[] = {
	{ HID_TLC(HUP_DIGITIZERS, HUD_PEN) },
};

static int
hpen_battery_strenght_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV;
	int32_t data;

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		evdev_support_event(evdev, EV_PWR);
		/* TODO */
		break;
	case HMAP_CB_IS_RUNNING:
		data = ctx;
		/* TODO */
	}

	return (0);
}

static int
hpen_compl_cb(HMAP_CB_ARGS)
{
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV;

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING)
		evdev_support_prop(evdev, INPUT_PROP_DIRECT);

	/* Do not execute callback at interrupt handler and detach */
	return (ENOSYS);
}

static int
hpen_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, hpen_devs, sizeof(hpen_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	/* Check if report descriptor belongs to a HID tablet device */
	error = hmap_add_map(dev, hpen_map, nitems(hpen_map), NULL);
	if (error != 0)
		return (error);

	hidbus_set_desc(dev, "Pen");

	return (BUS_PROBE_DEFAULT);
}

static devclass_t hpen_devclass;

static device_method_t hpen_methods[] = {
	DEVMETHOD(device_probe, hpen_probe),
	DEVMETHOD_END
};

DEFINE_CLASS_1(hpen, hpen_driver, hpen_methods,
    sizeof(struct hmap_softc), hmap_driver);
DRIVER_MODULE(hpen, hidbus, hpen_driver, hpen_devclass, NULL, 0);
MODULE_DEPEND(hpen, hid, 1, 1, 1);
MODULE_DEPEND(hpen, hmap, 1, 1, 1);
MODULE_DEPEND(hpen, evdev, 1, 1, 1);
MODULE_VERSION(hpen, 1);
