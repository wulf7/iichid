/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
 * Copyright (c) 2019 Greg V <greg@unrelenting.technology>
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
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
#include <sys/bitstring.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/sbuf.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "hid.h"
#include "hidbus.h"

#define	HID_DEBUG_VAR	hpen_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hpen_debug = 1;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hpen, CTLFLAG_RW, 0,
		"Generic HID tablet");
SYSCTL_INT(_hw_hid_hpen, OID_AUTO, debug, CTLFLAG_RWTUN,
		&hpen_debug, 0, "Debug level");
#endif


#define	HPEN_NO_CODE	(ABS_MAX + 10)

struct hpen_hid_map_item {
	char		name[5];
	int32_t 	usage;		/* HID usage */
	uint32_t	code;		/* Evdev event code */
	bool		required;	/* Required for Integrated Windows Pen tablets */
};

enum {
	HPEN_X,
	HPEN_Y,
	HPEN_TIP_PRESSURE,
	HPEN_X_TILT,
	HPEN_Y_TILT,
	HPEN_BATTERY_STRENGTH,
	HPEN_N_USAGES_ABS,
};

static const struct hpen_hid_map_item hpen_hid_map_abs[HPEN_N_USAGES_ABS] = {
	[HPEN_X] = {
		.name = "X",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X),
		.code = ABS_X,
		.required = true,
	},
	[HPEN_Y] = {
		.name = "Y",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y),
		.code = ABS_Y,
		.required = true,
	},
	[HPEN_TIP_PRESSURE] = {
		.name = "TPRS",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_PRESSURE),
		.code = ABS_PRESSURE,
		.required = true,
	},
	[HPEN_X_TILT] = {
		.name = "XTLT",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_X_TILT),
		.code = ABS_TILT_X,
		.required = false,
	},
	[HPEN_Y_TILT] = {
		.name = "YTLT",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_Y_TILT),
		.code = ABS_TILT_Y,
		.required = false,
	},
	[HPEN_BATTERY_STRENGTH] = {
		.name = "BATT",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_BATTERY_STRENGTH),
		.code = HPEN_NO_CODE, // TODO
		.required = false,
	},
};

enum {
	HPEN_TIP_SWITCH,
	HPEN_IN_RANGE,
	HPEN_BARREL_SWITCH,
	HPEN_INVERT,
	HPEN_ERASER,
	HPEN_N_USAGES_KEY,
};

static const struct hpen_hid_map_item hpen_hid_map_key[HPEN_N_USAGES_KEY] = {
	[HPEN_TIP_SWITCH] = {
		.name = "TIP",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_SWITCH),
		.code = BTN_TOUCH,
		.required = true,
	},
	[HPEN_IN_RANGE] = {
		.name = "RNGE",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_IN_RANGE),
		.code = BTN_TOOL_PEN,
		.required = true,
	},
	[HPEN_BARREL_SWITCH] = {
		.name = "BARL",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_BARREL_SWITCH),
		.code = BTN_STYLUS,
		.required = false,
	},
	[HPEN_INVERT] = {
		.name = "INVR",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_INVERT),
		.code = BTN_TOOL_RUBBER,
		.required = true,
	},
	[HPEN_ERASER] = {
		.name = "ERSR",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_ERASER),
		.code = BTN_TOUCH,
		.required = true,
	},
};


#define	USAGE_SUPPORTED(caps, usage)	bit_test(caps, usage)
#define	HPEN_FOREACH_USAGE_ABS(caps, usage)			\
	for ((usage) = 0; (usage) < HPEN_N_USAGES_ABS; ++(usage))	\
		if (USAGE_SUPPORTED((caps), (usage)))
#define	HPEN_FOREACH_USAGE_KEY(caps, usage)			\
	for ((usage) = 0; (usage) < HPEN_N_USAGES_KEY; ++(usage))	\
		if (USAGE_SUPPORTED((caps), (usage)))


struct hpen_softc {
	device_t sc_dev;

	struct evdev_dev *sc_evdev;

	uint8_t			report_id;
	bitstr_t		bit_decl(abs_caps, HPEN_N_USAGES_ABS);
	bitstr_t		bit_decl(key_caps, HPEN_N_USAGES_KEY);
	uint32_t		isize;
	struct hid_absinfo	ai[HPEN_N_USAGES_ABS];
	struct hid_location	locs_abs[HPEN_N_USAGES_ABS];
	struct hid_location	locs_key[HPEN_N_USAGES_KEY];
};


static device_probe_t hpen_probe;
static device_attach_t hpen_attach;
static device_detach_t hpen_detach;

static evdev_open_t hpen_ev_open;
static evdev_close_t hpen_ev_close;

static const struct evdev_methods hpen_evdev_methods = {
	.ev_open = &hpen_ev_open,
	.ev_close = &hpen_ev_close,
};

static const struct hid_device_id hpen_devs[] = {
	{ HID_TLC(HUP_DIGITIZERS, HUD_PEN) },
};

static int
hpen_ev_close(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	return (hidbus_set_xfer(dev, 0));
}

static int
hpen_ev_open(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	return (hidbus_set_xfer(dev, HID_XFER_READ));
}

static void
hpen_intr(void *context, void *buf, uint16_t len)
{
	device_t dev = context;
	struct hpen_softc *sc = device_get_softc(dev);
	size_t usage;
	uint8_t id;

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	/* Ignore irrelevant reports */
	id = sc->report_id != 0 ? *(uint8_t *)buf : 0;
	if (sc->report_id != id) {
		DPRINTF("Skip report with unexpected ID: %hhu\n", id);
		return;
	}

	/* Make sure we don't process old data */
	if (len < sc->isize)
		bzero((uint8_t *)buf + len, sc->isize - len);

	/* Strip leading "report ID" byte */
	if (sc->report_id) {
		len--;
		buf = (uint8_t *)buf + 1;
	}

	HPEN_FOREACH_USAGE_ABS(sc->abs_caps, usage) {
		if (hpen_hid_map_abs[usage].code != HPEN_NO_CODE)
			evdev_push_abs(sc->sc_evdev,
			    hpen_hid_map_abs[usage].code,
			    hid_get_udata(buf, len, &sc->locs_abs[usage]));
	}

	HPEN_FOREACH_USAGE_KEY(sc->key_caps, usage) {
		if (hpen_hid_map_key[usage].code != HPEN_NO_CODE)
			evdev_push_key(sc->sc_evdev,
			    hpen_hid_map_key[usage].code,
			    hid_get_data(buf, len, &sc->locs_key[usage]));
	}

	evdev_sync(sc->sc_evdev);
}

static int
hpen_hid_parse(struct hpen_softc *sc, const void *d_ptr, uint16_t d_len,
    uint32_t tlc_usage, uint8_t tlc_index)
{
	struct hid_item hi;
	struct hid_data *hd;
	size_t i;

	bzero(sc->abs_caps, bitstr_size(HPEN_N_USAGES_ABS));
	bzero(sc->key_caps, bitstr_size(HPEN_N_USAGES_KEY));

	/* Parse inputs */
	hd = hid_start_parse(d_ptr, d_len, 1 << hid_input);
	HID_TLC_FOREACH_ITEM(hd, &hi, tlc_index) {
		if (hi.kind != hid_input)
			continue;
		sc->report_id = hi.report_ID;
		for (size_t i = 0; i < HPEN_N_USAGES_ABS; i++) {
			if (hi.usage == hpen_hid_map_abs[i].usage) {
				bit_set(sc->abs_caps, i);
				sc->locs_abs[i] = hi.loc;
				sc->ai[i] = (struct hid_absinfo) {
				    .max = hi.logical_maximum,
				    .min = hi.logical_minimum,
				    .res = hid_item_resolution(&hi),
				};
			}
		}
		for (size_t i = 0; i < HPEN_N_USAGES_KEY; i++) {
			if (hi.usage == hpen_hid_map_key[i].usage) {
				bit_set(sc->key_caps, i);
				sc->locs_key[i] = hi.loc;
			}
		}
	}
	hid_end_parse(hd);

	for (i = 0; i < HPEN_N_USAGES_ABS; i++) {
		if (hpen_hid_map_abs[i].required &&
		    !USAGE_SUPPORTED(sc->abs_caps, i)) {
			DPRINTF("required report %s not found\n",
			    hpen_hid_map_abs[i].name);
			return (ENXIO);
		}
	}

	for (i = 0; i < HPEN_N_USAGES_KEY; i++) {
		if (hpen_hid_map_key[i].required &&
		    !USAGE_SUPPORTED(sc->key_caps, i)) {
			DPRINTF("required report %s not found\n",
			    hpen_hid_map_abs[i].name);
			return (ENXIO);
		}
	}

	sc->isize = hid_report_size_1(d_ptr, d_len, hid_input, sc->report_id);

	return (0);
}

static int
hpen_probe(device_t dev)
{
	struct hpen_softc *sc = device_get_softc(dev);
	void *d_ptr;
	uint16_t d_len;
	int error;

	error = hidbus_lookup_driver_info(dev, hpen_devs, sizeof(hpen_devs));
	if (error != 0)
		return (error);

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error != 0) {
		DPRINTF("could not retrieve report descriptor from "
		     "device: %d\n", error);
		return (ENXIO);
	}

	sc->sc_dev = dev;
	/* Check if report descriptor belongs to a HID tablet device */
	if (hpen_hid_parse(sc, d_ptr, d_len,
		    hidbus_get_usage(dev), hidbus_get_index(dev)) != 0)
		return (ENXIO);

	return (BUS_PROBE_DEFAULT);
}

static int
hpen_attach(device_t dev)
{
	struct hpen_softc *sc = device_get_softc(dev);
	struct hid_device_info *hw = device_get_ivars(dev);
	int err;
	size_t i;

	device_set_desc(dev, hw->name);

	hidbus_set_intr(dev, hpen_intr);

	sc->sc_evdev = evdev_alloc();
	evdev_set_name(sc->sc_evdev, device_get_desc(dev));
	evdev_set_phys(sc->sc_evdev, device_get_nameunit(dev));
	evdev_set_id(sc->sc_evdev, hw->idBus, hw->idVendor,
			hw->idProduct, hw->idVersion);
	evdev_set_serial(sc->sc_evdev, hw->serial);
	evdev_set_methods(sc->sc_evdev, dev, &hpen_evdev_methods);
	evdev_support_prop(sc->sc_evdev, INPUT_PROP_DIRECT);
	evdev_support_event(sc->sc_evdev, EV_SYN);
	evdev_support_event(sc->sc_evdev, EV_ABS);
	evdev_support_event(sc->sc_evdev, EV_KEY);
	evdev_support_event(sc->sc_evdev, EV_PWR);
	HPEN_FOREACH_USAGE_ABS(sc->abs_caps, i) {
		if (hpen_hid_map_abs[i].code != HPEN_NO_CODE)
			evdev_support_abs(
			    sc->sc_evdev, hpen_hid_map_abs[i].code, 0,
			    sc->ai[i].min, sc->ai[i].max, 0, 0, sc->ai[i].res);
	}
	HPEN_FOREACH_USAGE_KEY(sc->key_caps, i) {
		if (hpen_hid_map_key[i].code != HPEN_NO_CODE)
			evdev_support_key(
			    sc->sc_evdev, hpen_hid_map_key[i].code);
	}

	err = evdev_register_mtx(sc->sc_evdev, hidbus_get_lock(dev));
	if (err) {
		hpen_detach(dev);
		return (ENXIO);
	}

	return (0);
}

static int
hpen_detach(device_t self)
{
	struct hpen_softc *sc = device_get_softc(self);

	if (sc->sc_evdev)
	evdev_free(sc->sc_evdev);

	return (0);
}

static devclass_t hpen_devclass;

static device_method_t hpen_methods[] = {
	DEVMETHOD(device_probe, hpen_probe),
	DEVMETHOD(device_attach, hpen_attach),
	DEVMETHOD(device_detach, hpen_detach),

	DEVMETHOD_END
};

static driver_t hpen_driver = {
	.name = "hpen",
	.methods = hpen_methods,
	.size = sizeof(struct hpen_softc),
};

DRIVER_MODULE(hpen, hidbus, hpen_driver, hpen_devclass, NULL, 0);
MODULE_DEPEND(hpen, hid, 1, 1, 1);
MODULE_DEPEND(hpen, evdev, 1, 1, 1);
MODULE_VERSION(hpen, 1);
