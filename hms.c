/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2020 Vladimir Kondratyev <wulf@FreeBSD.org>
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
 * HID spec: https://www.usb.org/sites/default/files/documents/hid1_11.pdf
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

#define	HID_DEBUG_VAR	hms_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hms_debug = 0;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hms, CTLFLAG_RW, 0, "USB hms");
SYSCTL_INT(_hw_hid_hms, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hms_debug, 0, "Debug level");
#endif

enum {
	HMT_REL_X,
	HMT_REL_Y,
	HMT_REL_Z,
	HMT_ABS_X,
	HMT_ABS_Y,
	HMT_ABS_Z,
	HMT_WHEEL,
	HMT_HWHEEL,
	HMT_BTN,
	HMT_BTN_MS1,
	HMT_BTN_MS2,
};

static hmap_cb_t	hms_wheel_cb;

#define HMS_MAP_BUT_RG(usage_from, usage_to, code)	\
	{ HMAP_KEY_RANGE(#code, HUP_BUTTON, usage_from, usage_to, code) }
#define HMS_MAP_BUT_MS(usage, code)	\
	{ HMAP_KEY(#code, HUP_MICROSOFT, usage, code) }
#define HMS_MAP_ABS(usage, code)	\
	{ HMAP_ABS(#usage, HUP_GENERIC_DESKTOP, usage, code) }
#define HMS_MAP_REL(usage, code)	\
	{ HMAP_REL(#usage, HUP_GENERIC_DESKTOP, usage, code) }
#define HMS_MAP_REL_CN(usage, code)	\
	{ HMAP_REL(#usage, HUP_CONSUMER, usage, code) }
#define HMS_MAP_REL_CB(usage, cb)	\
	{ HMAP_REL_CB(#usage, HUP_GENERIC_DESKTOP, usage, &cb) }

static const struct hmap_item hms_map[] = {
	[HMT_REL_X]	= HMS_MAP_REL(HUG_X,		REL_X),
	[HMT_REL_Y]	= HMS_MAP_REL(HUG_Y,		REL_Y),
	[HMT_REL_Z]	= HMS_MAP_REL(HUG_Z,		REL_Z),
	[HMT_ABS_X]	= HMS_MAP_ABS(HUG_X,		ABS_X),
	[HMT_ABS_Y]	= HMS_MAP_ABS(HUG_Y,		ABS_Y),
	[HMT_ABS_Z]	= HMS_MAP_ABS(HUG_Z,		ABS_Z),
	[HMT_WHEEL]	= HMS_MAP_REL_CB(HUG_WHEEL,	hms_wheel_cb),
	[HMT_HWHEEL]	= HMS_MAP_REL_CN(HUC_AC_PAN,	REL_HWHEEL),
	[HMT_BTN]	= HMS_MAP_BUT_RG(1, 16,		BTN_MOUSE),
	[HMT_BTN_MS1]	= HMS_MAP_BUT_MS(1,		BTN_RIGHT),
	[HMT_BTN_MS2]	= HMS_MAP_BUT_MS(2,		BTN_MIDDLE),
};

/* A match on these entries will load hms */
static const struct hid_device_id hms_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_MOUSE) },
};

struct hms_softc {
	struct hmap_softc	super_sc;
	HMAP_CAPS(caps, hms_map);
	bool			rev_wheel;
};

/* Reverse wheel if required. */
static int
hms_wheel_cb(HMAP_CB_ARGS)
{
	struct hms_softc *sc = HMAP_CB_GET_SOFTC;
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV;
	int32_t data;

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		evdev_support_event(evdev, EV_REL);
		evdev_support_rel(evdev, REL_WHEEL);
		break;
	case HMAP_CB_IS_RUNNING:
		data = ctx;
		if (sc->rev_wheel)
			data = -data;
		evdev_push_rel(evdev, REL_WHEEL, data);
	}

	return (0);
}

static int
hms_probe(device_t dev)
{
	struct hms_softc *sc = device_get_softc(dev);
	int error;

	error = hidbus_lookup_driver_info(dev, hms_devs, sizeof(hms_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	/* Check if report descriptor belongs to mouse */
	error = hmap_add_map(dev, hms_map, nitems(hms_map), sc->caps);
	if (error != 0)
		return (error);

	/* There should be at least one X or Y axis */
	if (!hmap_test_cap(sc->caps, HMT_REL_X) &&
	    !hmap_test_cap(sc->caps, HMT_REL_X) &&
	    !hmap_test_cap(sc->caps, HMT_ABS_X) &&
	    !hmap_test_cap(sc->caps, HMT_ABS_Y))
		return (ENXIO);

	return (BUS_PROBE_DEFAULT);
}

static int
hms_attach(device_t dev)
{
	struct hms_softc *sc = device_get_softc(dev);
	struct hmap_hid_item *hi;
	int error, nbuttons = 0;

	/*
         * Force the report (non-boot) protocol.
         *
         * Mice without boot protocol support may choose not to implement
         * Set_Protocol at all; Ignore any error.
         */
	(void)hid_set_protocol(dev, 1);

#ifdef NOT_YET
	if (usb_test_quirk(uaa, UQ_MS_REVZ)) {
		/* Some wheels need the Z axis reversed. */
		sc->rev_wheel = true;
	}
#endif

	if (hmap_test_cap(sc->caps, HMT_ABS_X) ||
	    hmap_test_cap(sc->caps, HMT_ABS_Y)) {
		hidbus_set_desc(dev, "Tablet");
		hmap_set_evdev_prop(dev, INPUT_PROP_DIRECT);
	} else {
		hidbus_set_desc(dev, "Mouse");
		hmap_set_evdev_prop(dev, INPUT_PROP_POINTER);
	}

	error = hmap_attach(dev);
	if (error)
		return (error);

	/* Count number of input usages of variable type mapped to buttons */
	for (hi = sc->super_sc.hid_items;
	     hi < sc->super_sc.hid_items + sc->super_sc.nhid_items;
	     hi++)
		if (hi->type == HMAP_TYPE_VARIABLE && hi->evtype == EV_KEY)
			nbuttons++;

	/* announce information about the mouse */
	device_printf(dev, "%d buttons and [%s%s%s%s%s] coordinates ID=%u\n",
	    nbuttons,
	    (hmap_test_cap(sc->caps, HMT_REL_X) ||
	     hmap_test_cap(sc->caps, HMT_ABS_X)) ? "X" : "",
	    (hmap_test_cap(sc->caps, HMT_REL_Y) ||
	     hmap_test_cap(sc->caps, HMT_ABS_Y)) ? "Y" : "",
	    (hmap_test_cap(sc->caps, HMT_REL_Z) ||
	     hmap_test_cap(sc->caps, HMT_ABS_Z)) ? "Z" : "",
	    hmap_test_cap(sc->caps, HMT_WHEEL) ? "W" : "",
	    hmap_test_cap(sc->caps, HMT_HWHEEL) ? "H" : "",
	    sc->super_sc.hid_items[0].id);

	return (0);
}

static devclass_t hms_devclass;
static device_method_t hms_methods[] = {
	DEVMETHOD(device_probe, hms_probe),
	DEVMETHOD(device_attach, hms_attach),
	DEVMETHOD_END
};

DEFINE_CLASS_1(hms, hms_driver, hms_methods, sizeof(struct hms_softc),
    hmap_driver);
DRIVER_MODULE(hms, hidbus, hms_driver, hms_devclass, NULL, 0);
MODULE_DEPEND(hms, hid, 1, 1, 1);
MODULE_DEPEND(hms, hmap, 1, 1, 1);
MODULE_DEPEND(hms, evdev, 1, 1, 1);
MODULE_VERSION(hms, 1);
/* USB_PNP_HOST_INFO(hms_devs); */
