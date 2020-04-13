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
 * Simple evdev-only HID kbd driver. Does not support or depend on VT/SysCons.
 * HID specs: https://www.usb.org/sites/default/files/documents/hid1_11.pdf
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "hid.h"
#include "hidbus.h"
#include "hmap.h"

#define	HID_DEBUG_VAR	hskbd_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hskbd_debug = 1;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hskbd, CTLFLAG_RW, 0,
		"Simple keyboard");
SYSCTL_INT(_hw_hid_hskbd, OID_AUTO, debug, CTLFLAG_RWTUN,
		&hskbd_debug, 0, "Debug level");
#endif

static const uint8_t hskbd_boot_desc[] = {
	0x05, 0x01,		// Usage Page (Generic Desktop Ctrls)
	0x09, 0x06,		// Usage (Keyboard)
	0xA1, 0x01,		// Collection (Application)
	0x05, 0x07,		//   Usage Page (Kbrd/Keypad)
	0x19, 0xE0,		//   Usage Minimum (0xE0)
	0x29, 0xE7,		//   Usage Maximum (0xE7)
	0x15, 0x00,		//   Logical Minimum (0)
	0x25, 0x01,		//   Logical Maximum (1)
	0x75, 0x01,		//   Report Size (1)
	0x95, 0x08,		//   Report Count (8)
	0x81, 0x02,		//   Input (Data,Var,Abs)
	0x95, 0x01,		//   Report Count (1)
	0x75, 0x08,		//   Report Size (8)
	0x81, 0x01,		//   Input (Const,Array,Abs)
	0x95, 0x03,		//   Report Count (3)
	0x75, 0x01,		//   Report Size (1)
	0x05, 0x08,		//   Usage Page (LEDs)
	0x19, 0x01,		//   Usage Minimum (Num Lock)
	0x29, 0x03,		//   Usage Maximum (Scroll Lock)
	0x91, 0x02,		//   Output (Data,Var,Abs)
	0x95, 0x05,		//   Report Count (5)
	0x75, 0x01,		//   Report Size (1)
	0x91, 0x01,		//   Output (Const,Array,Abs)
	0x95, 0x06,		//   Report Count (6)
	0x75, 0x08,		//   Report Size (8)
	0x15, 0x00,		//   Logical Minimum (0)
	0x26, 0xFF, 0x00,	//   Logical Maximum (255)
	0x05, 0x07,		//   Usage Page (Kbrd/Keypad)
	0x19, 0x00,		//   Usage Minimum (0x00)
	0x2A, 0xFF, 0x00,	//   Usage Maximum (0xFF)
	0x81, 0x00,		//   Input (Data,Array,Abs)
	0xC0,			// End Collection
};

#define	HSKBD_BUFFER_SIZE	8	/* bytes */

static evdev_event_t	hskbd_ev_event;
static hmap_cb_t	hskbd_compl_cb;

#define HSKBD_KEY(name, usage, code) \
	{ HMAP_KEY(name, HUP_KEYBOARD, usage, code) }
#define	HSKBD_COMPL_CB(cb) { HMAP_COMPL_CB("COMPL_CB", &cb) }

static struct hmap_item hskbd_map[256] = {
	HSKBD_KEY("0x00", 0x00, KEY_RESERVED),	/* No event indicated */
	HSKBD_KEY("0x01", 0x01, HMAP_KEY_NULL),	/* Error RollOver */
	HSKBD_KEY("0x02", 0x02, HMAP_KEY_NULL),	/* POSTFail */
	HSKBD_KEY("0x03", 0x03, KEY_RESERVED),	/* Error Undefined */
	HSKBD_COMPL_CB(hskbd_compl_cb),
};
/* Map items starting from 5-th are filled by MOD_LOAD handler */
static int hskbd_nmap_items = 5;

static const struct hid_device_id hskbd_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_KEYBOARD) },
};

struct hskbd_softc {
	struct hmap_softc	super_sc;

	/* LED report parameters */
	struct hid_location	sc_loc_numlock;
	struct hid_location	sc_loc_capslock;
	struct hid_location	sc_loc_scrolllock;
	int			sc_led_size;
	uint8_t			sc_id_leds;

	/* Flags */
	bool			sc_numlock_exists:1;
	bool			sc_numlock_enabled:1;
	bool			sc_capslock_exists:1;
	bool			sc_capslock_enabled:1;
	bool			sc_scrolllock_exists:1;
	bool			sc_scrolllock_enabled:1;
	u_char			reserved:2;

	uint8_t			sc_buffer[HSKBD_BUFFER_SIZE];
};

static void
hskbd_ev_event(struct evdev_dev *evdev, uint16_t type, uint16_t code,
    int32_t value)
{
	device_t dev = evdev_get_softc(evdev);
	struct hskbd_softc *sc = device_get_softc(dev);
	uint8_t id, *buf;
	int len;

	if (type != EV_LED)
		return;

	/* If no leds, nothing to do. */
	if (!sc->sc_numlock_exists && !sc->sc_scrolllock_exists &&
	    !sc->sc_capslock_exists)
		return;

	DPRINTF("led(%u)=%d\n", type, value);

	mtx_lock(hidbus_get_lock(dev));

	switch (code) {
	case LED_CAPSL:
		sc->sc_capslock_enabled = value != 0;
		break;
	case LED_NUML:
		sc->sc_numlock_enabled = value != 0;
		break;
	case LED_SCROLLL:
		sc->sc_scrolllock_enabled = value != 0;
		break;
	}

	memset(sc->sc_buffer, 0, HSKBD_BUFFER_SIZE);
	id = sc->sc_id_leds;

	/* Assumption: All led bits must be in the same ID. */
	if (sc->sc_numlock_exists)
		hid_put_data_unsigned(sc->sc_buffer + 1, HSKBD_BUFFER_SIZE - 1,
		    &sc->sc_loc_numlock, sc->sc_numlock_enabled ? 1 : 0);
	if (sc->sc_scrolllock_exists)
		hid_put_data_unsigned(sc->sc_buffer + 1, HSKBD_BUFFER_SIZE - 1,
		    &sc->sc_loc_scrolllock, sc->sc_scrolllock_enabled ? 1 : 0);
	if (sc->sc_capslock_exists)
		hid_put_data_unsigned(sc->sc_buffer + 1, HSKBD_BUFFER_SIZE - 1,
		    &sc->sc_loc_capslock, sc->sc_capslock_enabled ? 1 : 0);

	/* Range check output report length. */
	len = sc->sc_led_size;
	if (len > (HSKBD_BUFFER_SIZE - 1))
		len = (HSKBD_BUFFER_SIZE - 1);

	/* Check if we need to prefix an ID byte. */
	if (id != 0) {
		sc->sc_buffer[0] = id;
		buf = sc->sc_buffer;
	} else {
		buf = sc->sc_buffer + 1;
	}

	DPRINTF("len=%d, id=%d\n", len, id);

	/* Start data transfer. */
	evdev_push_event(sc->super_sc.evdev, type, code, value);
	mtx_unlock(hidbus_get_lock(dev));
	hid_write(dev, buf, len);
}

static int
hskbd_compl_cb(HMAP_CB_ARGS)
{
	struct hskbd_softc *sc = HMAP_CB_GET_SOFTC;
	struct evdev_dev *evdev = HMAP_CB_GET_EVDEV;

	if (HMAP_CB_GET_STATE() == HMAP_CB_IS_ATTACHING) {
		if (sc->sc_numlock_exists || sc->sc_capslock_exists ||
		    sc->sc_scrolllock_exists)
			evdev_support_event(evdev, EV_LED);
		if (sc->sc_numlock_exists)
			evdev_support_led(evdev, LED_NUML);
		if (sc->sc_capslock_exists)
			evdev_support_led(evdev, LED_CAPSL);
		if (sc->sc_scrolllock_exists)
			evdev_support_led(evdev, LED_SCROLLL);
		evdev_support_event(evdev, EV_REP);
		evdev_set_flag(evdev, EVDEV_FLAG_SOFTREPEAT);
		sc->super_sc.evdev_methods.ev_event = &hskbd_ev_event;
	}

	/* Do not execute callback at interrupt handler and detach */
	return (ENOSYS);
}

static void
hskbd_identify(driver_t *driver, device_t parent)
{
	const struct hid_device_info *hw = hid_get_device_info(parent);

	/*
	 * If device claimed boot protocol support but do not have report
	 * descriptor, load one defined in "Appendix B.2" of HID1_11.pdf
	 */
	if (hid_get_report_descr(parent, NULL, NULL) != 0 && hw->pBootKbd)
		(void)hid_set_report_descr(parent, hskbd_boot_desc,
		    sizeof(hskbd_boot_desc));
}

static int
hskbd_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, hskbd_devs, sizeof(hskbd_devs));
	if (error != 0)
		return (error);

	hmap_set_debug_var(dev, &HID_DEBUG_VAR);

	/* Check if report descriptor belongs to keyboard */
	error = hmap_add_map(dev, hskbd_map, hskbd_nmap_items, NULL);
	if (error != 0)
		return (error);

	hidbus_set_desc(dev, "Simple Keyboard");

	return (BUS_PROBE_DEFAULT);
}

static int
hskbd_attach(device_t dev)
{
	struct hskbd_softc *sc = device_get_softc(dev);
	void *d_ptr;
	uint16_t d_len;
	bool set_report_proto;
	int error;
	uint32_t flags;
	uint8_t id;
	uint8_t tlc_index = hidbus_get_index(dev);

	/*
	 * Set the report (non-boot) protocol if report descriptor has not been
	 * overloaded with boot protocol report descriptor.
	 *
	 * Keyboards without boot protocol support may choose not to implement
	 * Set_Protocol at all; Ignore any error.
	 */
	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	set_report_proto = !(error == 0 && d_len == sizeof(hskbd_boot_desc) &&
	    memcmp(d_ptr, hskbd_boot_desc, sizeof(hskbd_boot_desc)) == 0);
	(void)hid_set_protocol(dev, set_report_proto ? 1 : 0);

	/* figure out leds on keyboard */
	if (hid_tlc_locate(d_ptr, d_len, HID_USAGE2(HUP_LEDS, 0x01),
	    hid_output, tlc_index, 0, &sc->sc_loc_numlock, &flags,
	    &sc->sc_id_leds, NULL)) {
		if (flags & HIO_VARIABLE)
			sc->sc_numlock_exists = true;
		DPRINTFN(1, "Found keyboard numlock\n");
	}
	if (hid_tlc_locate(d_ptr, d_len, HID_USAGE2(HUP_LEDS, 0x02),
	    hid_output, tlc_index, 0, &sc->sc_loc_capslock, &flags,
	    &id, NULL)) {
		if (!sc->sc_numlock_exists)
			sc->sc_id_leds = id;
		if (flags & HIO_VARIABLE && sc->sc_id_leds == id)
			sc->sc_capslock_exists = true;
		DPRINTFN(1, "Found keyboard capslock\n");
	}
	if (hid_tlc_locate(d_ptr, d_len, HID_USAGE2(HUP_LEDS, 0x03),
	    hid_output, tlc_index, 0, &sc->sc_loc_scrolllock, &flags,
	    &id, NULL)) {
		if (!sc->sc_numlock_exists && !sc->sc_capslock_exists)
			sc->sc_id_leds = id;
		if (flags & HIO_VARIABLE && sc->sc_id_leds == id)
			sc->sc_scrolllock_exists = true;
		DPRINTFN(1, "Found keyboard scrolllock\n");
	}

	if (sc->sc_numlock_exists || sc->sc_capslock_exists ||
	    sc->sc_scrolllock_exists)
		sc->sc_led_size = hid_report_size_1(d_ptr, d_len,
		    hid_output, sc->sc_id_leds);

	return (hmap_attach(dev));
}

static int
hskbd_driver_load(module_t mod, int what, void *arg)
{
	uint16_t code, i;

	if (what == MOD_LOAD) {
		for (i = 4; i < 0x100; i++)
			if ((code = evdev_hid2key(i)) != KEY_RESERVED)
				hskbd_map[hskbd_nmap_items++] =
				    (struct hmap_item) HSKBD_KEY("K", i, code);
	}
        return (0);
}

static devclass_t hskbd_devclass;
static device_method_t hskbd_methods[] = {
	DEVMETHOD(device_identify,	hskbd_identify),
	DEVMETHOD(device_probe,		hskbd_probe),
	DEVMETHOD(device_attach,	hskbd_attach),
	DEVMETHOD_END
};

DEFINE_CLASS_1(hskbd, hskbd_driver, hskbd_methods, sizeof(struct hskbd_softc),
    hmap_driver);
DRIVER_MODULE(hskbd, hidbus, hskbd_driver, hskbd_devclass, hskbd_driver_load,
    0);
MODULE_DEPEND(hskbd, hid, 1, 1, 1);
MODULE_DEPEND(hskbd, hmap, 1, 1, 1);
MODULE_DEPEND(hskbd, evdev, 1, 1, 1);
MODULE_VERSION(hskbd, 1);
