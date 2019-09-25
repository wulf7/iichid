/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
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
 * HID spec: http://www.usb.org/developers/devclass_docs/HID1_11.pdf
 */

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/sbuf.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usbhid.h>
#include "usbdevs.h"

#define	USB_DEBUG_VAR ums_debug
#include <dev/usb/usb_debug.h>

#include <dev/usb/quirk/usb_quirk.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "hidbus.h"

#ifdef USB_DEBUG
static int ums_debug = 0;

static SYSCTL_NODE(_hw_usb, OID_AUTO, ums, CTLFLAG_RW, 0, "USB ums");
SYSCTL_INT(_hw_usb_ums, OID_AUTO, debug, CTLFLAG_RWTUN,
    &ums_debug, 0, "Debug level");
#endif

#define	MOUSE_FLAGS_MASK (HIO_CONST|HIO_RELATIVE)
#define	MOUSE_FLAGS (HIO_RELATIVE)

#define	UMS_BUTTON_MAX   31		/* exclusive, must be less than 32 */
#define	UMS_BUT(i) ((i) < 16 ? BTN_MOUSE + (i) : BTN_MISC + (i) - 16)
#define	UMS_INFO_MAX	  2		/* maximum number of HID sets */

struct ums_info {
	struct hid_location sc_loc_w;
	struct hid_location sc_loc_x;
	struct hid_location sc_loc_y;
	struct hid_location sc_loc_z;
	struct hid_location sc_loc_t;
	struct hid_location sc_loc_btn[UMS_BUTTON_MAX];

	uint32_t sc_flags;
#define	UMS_FLAG_X_AXIS     0x0001
#define	UMS_FLAG_Y_AXIS     0x0002
#define	UMS_FLAG_Z_AXIS     0x0004
#define	UMS_FLAG_T_AXIS     0x0008
#define	UMS_FLAG_REVZ	    0x0020	/* Z-axis is reversed */
#define	UMS_FLAG_W_AXIS     0x0040

	uint8_t	sc_iid_w;
	uint8_t	sc_iid_x;
	uint8_t	sc_iid_y;
	uint8_t	sc_iid_z;
	uint8_t	sc_iid_t;
	uint8_t	sc_iid_btn[UMS_BUTTON_MAX];
	uint8_t	sc_buttons;
};

struct ums_softc {
	struct ums_info sc_info[UMS_INFO_MAX];

	uint8_t	sc_buttons;
	uint8_t	sc_iid;

	struct evdev_dev *sc_evdev;
};

static hid_intr_t ums_intr;

static device_probe_t ums_probe;
static device_attach_t ums_attach;
static device_detach_t ums_detach;

static evdev_open_t ums_ev_open;
static evdev_close_t ums_ev_close;

static int	ums_sysctl_handler_parseinfo(SYSCTL_HANDLER_ARGS);

static const struct evdev_methods ums_evdev_methods = {
	.ev_open = &ums_ev_open,
	.ev_close = &ums_ev_close,
};

static void
ums_intr(void *context, void *data, uint16_t len)
{
	device_t dev = context;
	struct ums_softc *sc = device_get_softc(dev);
	struct ums_info *info = &sc->sc_info[0];
	uint8_t *buf = data;
	int32_t buttons = 0;
	int32_t dw = 0;
	int32_t dx = 0;
	int32_t dy = 0;
	int32_t dz = 0;
	int32_t dt = 0;
	uint8_t i;
	uint8_t id;

		DPRINTFN(6, "data = %02x %02x %02x %02x "
		    "%02x %02x %02x %02x\n",
		    (len > 0) ? buf[0] : 0, (len > 1) ? buf[1] : 0,
		    (len > 2) ? buf[2] : 0, (len > 3) ? buf[3] : 0,
		    (len > 4) ? buf[4] : 0, (len > 5) ? buf[5] : 0,
		    (len > 6) ? buf[6] : 0, (len > 7) ? buf[7] : 0);

		if (sc->sc_iid) {
			id = *buf;

			len--;
			buf++;
		}

	repeat:
		if ((info->sc_flags & UMS_FLAG_W_AXIS) &&
		    (id == info->sc_iid_w))
			dw += hid_get_data(buf, len, &info->sc_loc_w);

		if ((info->sc_flags & UMS_FLAG_X_AXIS) && 
		    (id == info->sc_iid_x))
			dx += hid_get_data(buf, len, &info->sc_loc_x);

		if ((info->sc_flags & UMS_FLAG_Y_AXIS) &&
		    (id == info->sc_iid_y))
			dy -= hid_get_data(buf, len, &info->sc_loc_y);

		if ((info->sc_flags & UMS_FLAG_Z_AXIS) &&
		    (id == info->sc_iid_z)) {
			int32_t temp;
			temp = hid_get_data(buf, len, &info->sc_loc_z);
			if (info->sc_flags & UMS_FLAG_REVZ)
				temp = -temp;
			dz -= temp;
		}

		if ((info->sc_flags & UMS_FLAG_T_AXIS) &&
		    (id == info->sc_iid_t)) {
			dt += hid_get_data(buf, len, &info->sc_loc_t);
		}

		for (i = 0; i < info->sc_buttons; i++) {
			uint32_t mask;
			mask = 1UL << i;
			/* check for correct button ID */
			if (id != info->sc_iid_btn[i])
				continue;
			/* check for button pressed */
			if (hid_get_data(buf, len, &info->sc_loc_btn[i]))
				buttons |= mask;
		}

		if (++info != &sc->sc_info[UMS_INFO_MAX])
			goto repeat;

	/* Push evdev event */
	evdev_push_rel(sc->sc_evdev, REL_X, dx);
	evdev_push_rel(sc->sc_evdev, REL_Y, -dy);
	evdev_push_rel(sc->sc_evdev, REL_WHEEL, -dz);
	evdev_push_rel(sc->sc_evdev, REL_HWHEEL, dt);
	for (i = 0; i < UMS_BUTTON_MAX; i++)
		evdev_push_key(sc->sc_evdev, UMS_BUT(i), buttons & (1 << i));
	evdev_sync(sc->sc_evdev);
}

/* A match on these entries will load ums */
static const STRUCT_USB_HOST_ID __used ums_devs[] = {
	{USB_IFACE_CLASS(UICLASS_HID),
	 USB_IFACE_SUBCLASS(UISUBCLASS_BOOT),
	 USB_IFACE_PROTOCOL(UIPROTO_MOUSE),},
};

static int
ums_probe(device_t dev)
{
	void *d_ptr;
	int error;
	uint16_t d_len;

	DPRINTFN(11, "\n");

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error)
		return (ENXIO);

	if (hid_is_mouse(d_ptr, d_len))
		error = BUS_PROBE_DEFAULT;
	else
		error = ENXIO;

	return (error);
}

static void
ums_hid_parse(struct ums_softc *sc, device_t dev, const uint8_t *buf,
    uint16_t len, uint8_t index)
{
	struct ums_info *info = &sc->sc_info[index];
	uint32_t flags;
	uint8_t i;
	uint8_t j;

	if (hid_locate(buf, len, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X),
	    hid_input, index, &info->sc_loc_x, &flags, &info->sc_iid_x)) {

		if ((flags & MOUSE_FLAGS_MASK) == MOUSE_FLAGS) {
			info->sc_flags |= UMS_FLAG_X_AXIS;
		}
	}
	if (hid_locate(buf, len, HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y),
	    hid_input, index, &info->sc_loc_y, &flags, &info->sc_iid_y)) {

		if ((flags & MOUSE_FLAGS_MASK) == MOUSE_FLAGS) {
			info->sc_flags |= UMS_FLAG_Y_AXIS;
		}
	}
	/* Try the wheel first as the Z activator since it's tradition. */
	if (hid_locate(buf, len, HID_USAGE2(HUP_GENERIC_DESKTOP,
	    HUG_WHEEL), hid_input, index, &info->sc_loc_z, &flags,
	    &info->sc_iid_z)) {
		if ((flags & MOUSE_FLAGS_MASK) == MOUSE_FLAGS) {
			info->sc_flags |= UMS_FLAG_Z_AXIS;
		}
		/*
		 * We might have both a wheel and Z direction, if so put
		 * put the Z on the W coordinate.
		 */
		if (hid_locate(buf, len, HID_USAGE2(HUP_GENERIC_DESKTOP,
		    HUG_Z), hid_input, index, &info->sc_loc_w, &flags,
		    &info->sc_iid_w)) {

			if ((flags & MOUSE_FLAGS_MASK) == MOUSE_FLAGS) {
				info->sc_flags |= UMS_FLAG_W_AXIS;
			}
		}
	} else if (hid_locate(buf, len, HID_USAGE2(HUP_GENERIC_DESKTOP,
	    HUG_Z), hid_input, index, &info->sc_loc_z, &flags, 
	    &info->sc_iid_z)) {

		if ((flags & MOUSE_FLAGS_MASK) == MOUSE_FLAGS) {
			info->sc_flags |= UMS_FLAG_Z_AXIS;
		}
	}
	if (hid_locate(buf, len, HID_USAGE2(HUP_CONSUMER,
		HUC_AC_PAN), hid_input, index, &info->sc_loc_t,
		&flags, &info->sc_iid_t)) {

		if ((flags & MOUSE_FLAGS_MASK) == MOUSE_FLAGS)
			info->sc_flags |= UMS_FLAG_T_AXIS;
	}
	/* figure out the number of buttons */

	for (i = 0; i < UMS_BUTTON_MAX; i++) {
		if (!hid_locate(buf, len, HID_USAGE2(HUP_BUTTON, (i + 1)),
		    hid_input, index, &info->sc_loc_btn[i], NULL, 
		    &info->sc_iid_btn[i])) {
			break;
		}
	}

	/* detect other buttons */

	for (j = 0; (i < UMS_BUTTON_MAX) && (j < 2); i++, j++) {
		if (!hid_locate(buf, len, HID_USAGE2(HUP_MICROSOFT, (j + 1)),
		    hid_input, index, &info->sc_loc_btn[i], NULL, 
		    &info->sc_iid_btn[i])) {
			break;
		}
	}

	info->sc_buttons = i;

	if (i > sc->sc_buttons)
		sc->sc_buttons = i;

	if (info->sc_flags == 0)
		return;

	/* announce information about the mouse */
	device_printf(dev, "%d buttons and [%s%s%s%s%s] coordinates ID=%u\n",
	    (info->sc_buttons),
	    (info->sc_flags & UMS_FLAG_X_AXIS) ? "X" : "",
	    (info->sc_flags & UMS_FLAG_Y_AXIS) ? "Y" : "",
	    (info->sc_flags & UMS_FLAG_Z_AXIS) ? "Z" : "",
	    (info->sc_flags & UMS_FLAG_T_AXIS) ? "T" : "",
	    (info->sc_flags & UMS_FLAG_W_AXIS) ? "W" : "",
	    info->sc_iid_x);
}

static int
ums_attach(device_t dev)
{
	struct ums_softc *sc = device_get_softc(dev);
	struct hid_hw *hw = device_get_ivars(dev);
	struct ums_info *info;
	void *d_ptr = NULL;
	int isize;
	int err;
	uint16_t d_len;
	uint8_t i;
#ifdef USB_DEBUG
	uint8_t j;
#endif

	DPRINTFN(11, "sc=%p\n", sc);

	device_set_desc(dev, hw->hid);

#ifdef NOT_YET
	/*
         * Force the report (non-boot) protocol.
         *
         * Mice without boot protocol support may choose not to implement
         * Set_Protocol at all; Ignore any error.
         */
	err = usbd_req_set_protocol(uaa->device, NULL,
	    uaa->info.bIfaceIndex, 1);
#endif

	/* Get HID descriptor */
	err = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (err) {
		device_printf(dev, "error reading report description\n");
		goto detach;
	}

	isize = hid_report_size(d_ptr, d_len, hid_input, &sc->sc_iid);

	/* Search the HID descriptor and announce device */
	for (i = 0; i < UMS_INFO_MAX; i++) {
		ums_hid_parse(sc, dev, d_ptr, d_len, i);
	}

#ifdef NOT_YET
	if (usb_test_quirk(uaa, UQ_MS_REVZ)) {
		info = &sc->sc_info[0];
		/* Some wheels need the Z axis reversed. */
		info->sc_flags |= UMS_FLAG_REVZ;
	}
#endif

#ifdef USB_DEBUG
	for (j = 0; j < UMS_INFO_MAX; j++) {
		info = &sc->sc_info[j];

		DPRINTF("sc=%p, index=%d\n", sc, j);
		DPRINTF("X\t%d/%d id=%d\n", info->sc_loc_x.pos,
		    info->sc_loc_x.size, info->sc_iid_x);
		DPRINTF("Y\t%d/%d id=%d\n", info->sc_loc_y.pos,
		    info->sc_loc_y.size, info->sc_iid_y);
		DPRINTF("Z\t%d/%d id=%d\n", info->sc_loc_z.pos,
		    info->sc_loc_z.size, info->sc_iid_z);
		DPRINTF("T\t%d/%d id=%d\n", info->sc_loc_t.pos,
		    info->sc_loc_t.size, info->sc_iid_t);
		DPRINTF("W\t%d/%d id=%d\n", info->sc_loc_w.pos,
		    info->sc_loc_w.size, info->sc_iid_w);

		for (i = 0; i < info->sc_buttons; i++) {
			DPRINTF("B%d\t%d/%d id=%d\n",
			    i + 1, info->sc_loc_btn[i].pos,
			    info->sc_loc_btn[i].size, info->sc_iid_btn[i]);
		}
	}
	DPRINTF("size=%d, id=%d\n", isize, sc->sc_iid);
#endif

	hid_set_intr(dev, ums_intr);

	sc->sc_evdev = evdev_alloc();
	evdev_set_name(sc->sc_evdev, device_get_desc(dev));
	evdev_set_phys(sc->sc_evdev, device_get_nameunit(dev));
	evdev_set_id(sc->sc_evdev, BUS_USB, hw->idVendor, hw->idProduct,
	    hw->idVersion);
//	evdev_set_serial(sc->sc_evdev, usb_get_serial(uaa->device));
	evdev_set_methods(sc->sc_evdev, dev, &ums_evdev_methods);
	evdev_support_prop(sc->sc_evdev, INPUT_PROP_POINTER);
	evdev_support_event(sc->sc_evdev, EV_SYN);
	evdev_support_event(sc->sc_evdev, EV_REL);
	evdev_support_event(sc->sc_evdev, EV_KEY);

	info = &sc->sc_info[0];

	if (info->sc_flags & UMS_FLAG_X_AXIS)
		evdev_support_rel(sc->sc_evdev, REL_X);

	if (info->sc_flags & UMS_FLAG_Y_AXIS)
		evdev_support_rel(sc->sc_evdev, REL_Y);

	if (info->sc_flags & UMS_FLAG_Z_AXIS)
		evdev_support_rel(sc->sc_evdev, REL_WHEEL);

	if (info->sc_flags & UMS_FLAG_T_AXIS)
		evdev_support_rel(sc->sc_evdev, REL_HWHEEL);

	for (i = 0; i < info->sc_buttons; i++)
		evdev_support_key(sc->sc_evdev, UMS_BUT(i));

	err = evdev_register_mtx(sc->sc_evdev, hid_get_lock(dev));
	if (err)
		goto detach;

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
	    OID_AUTO, "parseinfo", CTLTYPE_STRING|CTLFLAG_RD,
	    sc, 0, ums_sysctl_handler_parseinfo,
	    "", "Dump of parsed HID report descriptor");

	return (0);

detach:
	ums_detach(dev);
	return (ENOMEM);
}

static int
ums_detach(device_t self)
{
	struct ums_softc *sc = device_get_softc(self);

	DPRINTF("sc=%p\n", sc);

	evdev_free(sc->sc_evdev);

	return (0);
}

static int
ums_ev_open(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	return (hid_start(dev));
}

static int
ums_ev_close(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	return (hid_stop(dev));
}

static int
ums_sysctl_handler_parseinfo(SYSCTL_HANDLER_ARGS)
{
	struct ums_softc *sc = arg1;
	struct ums_info *info;
	struct sbuf *sb;
	int i, j, err, had_output;

	sb = sbuf_new_auto();
	for (i = 0, had_output = 0; i < UMS_INFO_MAX; i++) {
		info = &sc->sc_info[i];

		/* Don't emit empty info */
		if ((info->sc_flags &
		    (UMS_FLAG_X_AXIS | UMS_FLAG_Y_AXIS | UMS_FLAG_Z_AXIS |
		     UMS_FLAG_T_AXIS | UMS_FLAG_W_AXIS)) == 0 &&
		    info->sc_buttons == 0)
			continue;

		if (had_output)
			sbuf_printf(sb, "\n");
		had_output = 1;
		sbuf_printf(sb, "i%d:", i + 1);
		if (info->sc_flags & UMS_FLAG_X_AXIS)
			sbuf_printf(sb, " X:r%d, p%d, s%d;",
			    (int)info->sc_iid_x,
			    (int)info->sc_loc_x.pos,
			    (int)info->sc_loc_x.size);
		if (info->sc_flags & UMS_FLAG_Y_AXIS)
			sbuf_printf(sb, " Y:r%d, p%d, s%d;",
			    (int)info->sc_iid_y,
			    (int)info->sc_loc_y.pos,
			    (int)info->sc_loc_y.size);
		if (info->sc_flags & UMS_FLAG_Z_AXIS)
			sbuf_printf(sb, " Z:r%d, p%d, s%d;",
			    (int)info->sc_iid_z,
			    (int)info->sc_loc_z.pos,
			    (int)info->sc_loc_z.size);
		if (info->sc_flags & UMS_FLAG_T_AXIS)
			sbuf_printf(sb, " T:r%d, p%d, s%d;",
			    (int)info->sc_iid_t,
			    (int)info->sc_loc_t.pos,
			    (int)info->sc_loc_t.size);
		if (info->sc_flags & UMS_FLAG_W_AXIS)
			sbuf_printf(sb, " W:r%d, p%d, s%d;",
			    (int)info->sc_iid_w,
			    (int)info->sc_loc_w.pos,
			    (int)info->sc_loc_w.size);

		for (j = 0; j < info->sc_buttons; j++) {
			sbuf_printf(sb, " B%d:r%d, p%d, s%d;", j + 1,
			    (int)info->sc_iid_btn[j],
			    (int)info->sc_loc_btn[j].pos,
			    (int)info->sc_loc_btn[j].size);
		}
	}
	sbuf_finish(sb);
	err = SYSCTL_OUT(req, sbuf_data(sb), sbuf_len(sb) + 1);
	sbuf_delete(sb);

	return (err);
}

static devclass_t ums_devclass;

static device_method_t ums_methods[] = {
	DEVMETHOD(device_probe, ums_probe),
	DEVMETHOD(device_attach, ums_attach),
	DEVMETHOD(device_detach, ums_detach),

	DEVMETHOD_END
};

static driver_t ums_driver = {
	.name = "ums",
	.methods = ums_methods,
	.size = sizeof(struct ums_softc),
};

DRIVER_MODULE(ums, hidbus, ums_driver, ums_devclass, NULL, 0);
MODULE_DEPEND(ums, usb, 1, 1, 1);
MODULE_DEPEND(ums, evdev, 1, 1, 1);
MODULE_VERSION(ums, 1);
USB_PNP_HOST_INFO(ums_devs);
