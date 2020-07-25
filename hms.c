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
#include "hid_quirk.h"
#include "hmap.h"

#define	HID_DEBUG_VAR	hms_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hms_debug = 0;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hms, CTLFLAG_RW, 0, "USB hms");
SYSCTL_INT(_hw_hid_hms, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hms_debug, 0, "Debug level");
#endif

static const uint8_t hms_boot_desc[] = {
	0x05, 0x01,	// Usage Page (Generic Desktop Ctrls)
	0x09, 0x02,	// Usage (Mouse)
	0xA1, 0x01,	// Collection (Application)
	0x09, 0x01,	//   Usage (Pointer)
	0xA1, 0x00,	//   Collection (Physical)
	0x95, 0x03,	//     Report Count (3)
	0x75, 0x01,	//     Report Size (1)
	0x05, 0x09,	//     Usage Page (Button)
	0x19, 0x01,	//     Usage Minimum (0x01)
	0x29, 0x03,	//     Usage Maximum (0x03)
	0x15, 0x00,	//     Logical Minimum (0)
	0x25, 0x01,	//     Logical Maximum (1)
	0x81, 0x02,	//     Input (Data,Var,Abs)
	0x95, 0x01,	//     Report Count (1)
	0x75, 0x05,	//     Report Size (5)
	0x81, 0x03,	//     Input (Const)
	0x75, 0x08,	//     Report Size (8)
	0x95, 0x02,	//     Report Count (2)
	0x05, 0x01,	//     Usage Page (Generic Desktop Ctrls)
	0x09, 0x30,	//     Usage (X)
	0x09, 0x31,	//     Usage (Y)
	0x15, 0x81,	//     Logical Minimum (-127)
	0x25, 0x7F,	//     Logical Maximum (127)
	0x81, 0x06,	//     Input (Data,Var,Rel)
	0xC0,		//   End Collection
	0xC0,		// End Collection
};

enum {
	HMS_REL_X,
	HMS_REL_Y,
	HMS_REL_Z,
	HMS_ABS_X,
	HMS_ABS_Y,
	HMS_ABS_Z,
	HMS_WHEEL,
	HMS_HWHEEL,
	HMS_BTN,
	HMS_BTN_MS1,
	HMS_BTN_MS2,
	HMS_COMPL_CB,
};

static hmap_cb_t	hms_wheel_cb;
static hmap_cb_t	hms_compl_cb;

#define HMS_MAP_BUT_RG(usage_from, usage_to, code)	\
	{ HMAP_KEY_RANGE(HUP_BUTTON, usage_from, usage_to, code) }
#define HMS_MAP_BUT_MS(usage, code)	\
	{ HMAP_KEY(HUP_MICROSOFT, usage, code) }
#define HMS_MAP_ABS(usage, code)	\
	{ HMAP_ABS(HUP_GENERIC_DESKTOP, usage, code) }
#define HMS_MAP_REL(usage, code)	\
	{ HMAP_REL(HUP_GENERIC_DESKTOP, usage, code) }
#define HMS_MAP_REL_CN(usage, code)	\
	{ HMAP_REL(HUP_CONSUMER, usage, code) }
#define HMS_MAP_REL_CB(usage, cb)	\
	{ HMAP_REL_CB(HUP_GENERIC_DESKTOP, usage, &cb) }
#define	HMS_COMPL_CB(cb)		\
	{ HMAP_COMPL_CB(&cb) }

static const struct hmap_item hms_map[] = {
	[HMS_REL_X]	= HMS_MAP_REL(HUG_X,		REL_X),
	[HMS_REL_Y]	= HMS_MAP_REL(HUG_Y,		REL_Y),
	[HMS_REL_Z]	= HMS_MAP_REL(HUG_Z,		REL_Z),
	[HMS_ABS_X]	= HMS_MAP_ABS(HUG_X,		ABS_X),
	[HMS_ABS_Y]	= HMS_MAP_ABS(HUG_Y,		ABS_Y),
	[HMS_ABS_Z]	= HMS_MAP_ABS(HUG_Z,		ABS_Z),
	[HMS_WHEEL]	= HMS_MAP_REL_CB(HUG_WHEEL,	hms_wheel_cb),
	[HMS_HWHEEL]	= HMS_MAP_REL_CN(HUC_AC_PAN,	REL_HWHEEL),
	[HMS_BTN]	= HMS_MAP_BUT_RG(1, 16,		BTN_MOUSE),
	[HMS_BTN_MS1]	= HMS_MAP_BUT_MS(1,		BTN_RIGHT),
	[HMS_BTN_MS2]	= HMS_MAP_BUT_MS(2,		BTN_MIDDLE),
	[HMS_COMPL_CB]	= HMS_COMPL_CB(hms_compl_cb),
};

/* A match on these entries will load hms */
static const struct hid_device_id hms_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_MOUSE) },
};

struct hms_softc {
	struct hmap		hm;
	HMAP_CAPS(caps, hms_map);
	bool			rev_wheel;
};

/* Reverse wheel if required. */
static int
hms_wheel_cb(HMAP_CB_ARGS)
{
	struct hms_softc *sc = HMAP_CB_GET_SOFTC();
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();
	int32_t data;

	switch (HMAP_CB_GET_STATE()) {
	case HMAP_CB_IS_ATTACHING:
		evdev_support_event(evdev, EV_REL);
		evdev_support_rel(evdev, REL_WHEEL);
		break;
	case HMAP_CB_IS_RUNNING:
		data = ctx;
		/* No movement. Nothing to report. */
		if (data == 0)
			return (ENOMSG);
		if (sc->rev_wheel)
			data = -data;
		evdev_push_rel(evdev, REL_WHEEL, data);
	}

	return (0);
}

static int
hms_compl_cb(HMAP_CB_ARGS)
{
	struct hms_softc *sc = HMAP_CB_GET_SOFTC();
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV();

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING) {
		if (hmap_test_cap(sc->caps, HMS_ABS_X) ||
		    hmap_test_cap(sc->caps, HMS_ABS_Y))
			evdev_support_prop(evdev, INPUT_PROP_DIRECT);
		else
			evdev_support_prop(evdev, INPUT_PROP_POINTER);
	}

	/* Do not execute callback at interrupt handler and detach */
	return (ENOSYS);
}

static void
hms_identify(driver_t *driver, device_t parent)
{
	const struct hid_device_info *hw = hid_get_device_info(parent);
	void *d_ptr;
	hid_size_t d_len;
	int error;

	/*
	 * If device claimed boot protocol support but do not have report
	 * descriptor, load one defined in "Appendix B.2" of HID1_11.pdf
	 */
	error = hid_get_report_descr(parent, &d_ptr, &d_len);
	if ((error != 0 && hid_test_quirk(hw, HQ_HAS_MS_BOOTPROTO)) ||
	    (error == 0 && hid_test_quirk(hw, HQ_MS_BOOTPROTO) &&
	     hid_is_mouse(d_ptr, d_len)))
		(void)hid_set_report_descr(parent, hms_boot_desc,
		    sizeof(hms_boot_desc));
}

static int
hms_probe(device_t dev)
{
	struct hms_softc *sc = device_get_softc(dev);
	int error;

	error = hidbus_lookup_driver_info(dev, hms_devs, sizeof(hms_devs));
	if (error != 0)
		return (error);

	hmap_set_dev(&sc->hm, dev);
	hmap_set_debug_var(&sc->hm, &HID_DEBUG_VAR);

	/* Check if report descriptor belongs to mouse */
	error = hmap_add_map(&sc->hm, hms_map, nitems(hms_map), sc->caps);
	if (error != 0)
		return (error);

	/* There should be at least one X or Y axis */
	if (!hmap_test_cap(sc->caps, HMS_REL_X) &&
	    !hmap_test_cap(sc->caps, HMS_REL_X) &&
	    !hmap_test_cap(sc->caps, HMS_ABS_X) &&
	    !hmap_test_cap(sc->caps, HMS_ABS_Y))
		return (ENXIO);

	if (hmap_test_cap(sc->caps, HMS_ABS_X) ||
	    hmap_test_cap(sc->caps, HMS_ABS_Y))
		hidbus_set_desc(dev, "Tablet");
	 else
		hidbus_set_desc(dev, "Mouse");

	return (BUS_PROBE_DEFAULT);
}

static int
hms_attach(device_t dev)
{
	struct hms_softc *sc = device_get_softc(dev);
	const struct hid_device_info *hw = hid_get_device_info(dev);
	struct hmap_hid_item *hi;
	void *d_ptr;
	hid_size_t d_len;
	bool set_report_proto;
	int error, nbuttons = 0;

	/*
	 * Set the report (non-boot) protocol if report descriptor has not been
	 * overloaded with boot protocol report descriptor.
	 *
	 * Mice without boot protocol support may choose not to implement
	 * Set_Protocol at all; Ignore any error.
	 */
	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	set_report_proto = !(error == 0 && d_len == sizeof(hms_boot_desc) &&
	    memcmp(d_ptr, hms_boot_desc, sizeof(hms_boot_desc)) == 0);
	(void)hid_set_protocol(dev, set_report_proto ? 1 : 0);

	if (hid_test_quirk(hw, HQ_MS_REVZ)) {
		/* Some wheels need the Z axis reversed. */
		sc->rev_wheel = true;
	}

	error = hmap_attach(&sc->hm);
	if (error)
		return (error);

	/* Count number of input usages of variable type mapped to buttons */
	for (hi = sc->hm.hid_items;
	     hi < sc->hm.hid_items + sc->hm.nhid_items;
	     hi++)
		if (hi->type == HMAP_TYPE_VARIABLE && hi->evtype == EV_KEY)
			nbuttons++;

	/* announce information about the mouse */
	device_printf(dev, "%d buttons and [%s%s%s%s%s] coordinates ID=%u\n",
	    nbuttons,
	    (hmap_test_cap(sc->caps, HMS_REL_X) ||
	     hmap_test_cap(sc->caps, HMS_ABS_X)) ? "X" : "",
	    (hmap_test_cap(sc->caps, HMS_REL_Y) ||
	     hmap_test_cap(sc->caps, HMS_ABS_Y)) ? "Y" : "",
	    (hmap_test_cap(sc->caps, HMS_REL_Z) ||
	     hmap_test_cap(sc->caps, HMS_ABS_Z)) ? "Z" : "",
	    hmap_test_cap(sc->caps, HMS_WHEEL) ? "W" : "",
	    hmap_test_cap(sc->caps, HMS_HWHEEL) ? "H" : "",
	    sc->hm.hid_items[0].id);

	return (0);
}

static int
hms_detach(device_t dev)
{
	struct hms_softc *sc = device_get_softc(dev);

	return (hmap_detach(&sc->hm));
}

static devclass_t hms_devclass;
static device_method_t hms_methods[] = {
	DEVMETHOD(device_identify,	hms_identify),
	DEVMETHOD(device_probe,		hms_probe),
	DEVMETHOD(device_attach,	hms_attach),
	DEVMETHOD(device_detach,	hms_detach),

	DEVMETHOD_END
};

DEFINE_CLASS_0(hms, hms_driver, hms_methods, sizeof(struct hms_softc));
DRIVER_MODULE(hms, hidbus, hms_driver, hms_devclass, NULL, 0);
MODULE_DEPEND(hms, hid, 1, 1, 1);
MODULE_DEPEND(hms, hmap, 1, 1, 1);
MODULE_DEPEND(hms, evdev, 1, 1, 1);
MODULE_VERSION(hms, 1);
/* USB_PNP_HOST_INFO(hms_devs); */
