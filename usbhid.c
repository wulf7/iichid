/*-
 * SPDX-License-Identifier: BSD-2-Clause-NetBSD
 *
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
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
 * HID spec: https://www.usb.org/sites/default/files/documents/hid1_11.pdf
 */

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>
#include <sys/conf.h>
#include <sys/fcntl.h>

#include <dev/evdev/input.h>

#include "usbdevs.h"
#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_ioctl.h>

#define	USB_DEBUG_VAR usbhid_debug
#include <dev/usb/usb_debug.h>

#include <dev/usb/input/usb_rdesc.h>
#include <dev/usb/quirk/usb_quirk.h>

#include "hid.h"
#include "hidbus.h"
#include "hid_if.h"

/* Set default probe priority lesser than other USB device drivers have */
#ifndef USBHID_BUS_PROBE_PRIO
#define	USBHID_BUS_PROBE_PRIO	(BUS_PROBE_GENERIC - 1)
#endif

#ifdef USB_DEBUG
static int usbhid_debug = 0;

static SYSCTL_NODE(_hw_usb, OID_AUTO, usbhid, CTLFLAG_RW, 0, "USB usbhid");
SYSCTL_INT(_hw_usb_usbhid, OID_AUTO, debug, CTLFLAG_RWTUN,
    &usbhid_debug, 0, "Debug level");
#endif

#define	USBHID_RSIZE		2048		/* bytes, max report size */
#define	USBHID_FRAME_NUM 	50		/* bytes, frame number */

enum {
	USBHID_INTR_DT_WR,
	USBHID_INTR_DT_RD,
	USBHID_CTRL_DT_WR,
#ifdef NOT_YET
	USBHID_CTRL_DT_RD,
#endif
	USBHID_N_TRANSFER,
};

struct usbhid_softc {
	device_t sc_child;

	hid_intr_t *sc_intr_handler;
	void *sc_intr_context;
	struct mtx *sc_intr_mtx;

	struct hid_device_info sc_hw;

	struct usb_config sc_config[USBHID_N_TRANSFER];
	struct usb_xfer *sc_xfer[USBHID_N_TRANSFER];
	struct usb_device *sc_udev;
	void   *sc_repdesc_ptr;
	void   *sc_ibuf;

	uint32_t sc_isize;
	uint32_t sc_osize;
	uint32_t sc_fsize;

	uint16_t sc_repdesc_size;

	uint8_t	sc_iface_no;
	uint8_t	sc_iface_index;
	uint8_t	sc_iid;
	uint8_t	sc_oid;
	uint8_t	sc_fid;
	uint8_t	sc_flags;
#define	USBHID_FLAG_IMMED        0x01	/* set if read should be immediate */
#define	USBHID_FLAG_STATIC_DESC  0x04	/* set if report descriptors are
					 * static */
	uint8_t *sc_tr_buf;
	uint16_t sc_tr_len;
	int sc_tr_error;
};

static const uint8_t usbhid_xb360gp_report_descr[] = {UHID_XB360GP_REPORT_DESCR()};
static const uint8_t usbhid_graphire_report_descr[] = {UHID_GRAPHIRE_REPORT_DESCR()};
static const uint8_t usbhid_graphire3_4x5_report_descr[] = {UHID_GRAPHIRE3_4X5_REPORT_DESCR()};

/* prototypes */

static device_probe_t usbhid_probe;
static device_attach_t usbhid_attach;
static device_detach_t usbhid_detach;

static usb_callback_t usbhid_write_callback;
static usb_callback_t usbhid_read_callback;
static usb_callback_t usbhid_set_report_callback;
#ifdef NOT_YET
static usb_callback_t usbhid_get_report_callback;
#endif

static void
usbhid_write_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct usbhid_softc *sc = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;
	uint16_t io_len;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
	case USB_ST_SETUP:
		sc->sc_tr_error = 0;
tr_setup:
		if (sc->sc_tr_len == 0)
			goto tr_exit;
		io_len = MIN(sc->sc_tr_len, usbd_xfer_max_len(xfer));

		pc = usbd_xfer_get_frame(xfer, 0);
		usbd_copy_in(pc, 0, sc->sc_tr_buf, io_len);
		usbd_xfer_set_frame_len(xfer, 0, io_len);
		usbd_transfer_submit(xfer);

		sc->sc_tr_len -= io_len;
		sc->sc_tr_buf += io_len;
		return;

	default:			/* Error */
		if (error != USB_ERR_CANCELLED) {
			/* try to clear stall first */
			usbd_xfer_set_stall(xfer);
			goto tr_setup;
		}
		sc->sc_tr_error = EIO;
tr_exit:
		if (!HID_IN_POLLING_MODE_FUNC())
			wakeup(sc);
		return;
	}
}

static void
usbhid_read_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct usbhid_softc *sc = usbd_xfer_softc(xfer);
	struct usb_page_cache *pc;
	int actlen;

	usbd_xfer_status(xfer, &actlen, NULL, NULL, NULL);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		DPRINTF("transferred!\n");

		pc = usbd_xfer_get_frame(xfer, 0);

		/* 
		 * If the ID byte is non zero we allow descriptors
		 * having multiple sizes:
		 */
		if ((actlen >= (int)sc->sc_isize) ||
		    ((actlen > 0) && (sc->sc_iid != 0))) {
			/* limit report length to the maximum */
			if (actlen > (int)sc->sc_isize)
				actlen = sc->sc_isize;
			usbd_copy_out(pc, 0, sc->sc_ibuf, actlen);
			sc->sc_intr_handler(sc->sc_intr_context, sc->sc_ibuf,
			    actlen);
		} else {
			/* ignore it */
			DPRINTF("ignored transfer, %d bytes\n", actlen);
		}

	case USB_ST_SETUP:
re_submit:
		usbd_xfer_set_frame_len(xfer, 0, usbd_xfer_max_len(xfer));
		usbd_transfer_submit(xfer);
		return;

	default:			/* Error */
		if (error != USB_ERR_CANCELLED) {
			/* try to clear stall first */
			usbd_xfer_set_stall(xfer);
			goto re_submit;
		}
		return;
	}
}

static void
usbhid_fill_set_report(struct usb_device_request *req, uint8_t iface_no,
    uint8_t type, uint8_t id, uint16_t size)
{
	req->bmRequestType = UT_WRITE_CLASS_INTERFACE;
	req->bRequest = UR_SET_REPORT;
	USETW2(req->wValue, type, id);
	req->wIndex[0] = iface_no;
	req->wIndex[1] = 0;
	USETW(req->wLength, size);
}

#ifdef NOT_YET
static void
usbhid_fill_get_report(struct usb_device_request *req, uint8_t iface_no,
    uint8_t type, uint8_t id, uint16_t size)
{
	req->bmRequestType = UT_READ_CLASS_INTERFACE;
	req->bRequest = UR_GET_REPORT;
	USETW2(req->wValue, type, id);
	req->wIndex[0] = iface_no;
	req->wIndex[1] = 0;
	USETW(req->wLength, size);
}
#endif

static void
usbhid_set_report_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct usbhid_softc *sc = usbd_xfer_softc(xfer);
	struct usb_device_request req;
	struct usb_page_cache *pc;
	uint8_t id;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_SETUP:
		if (sc->sc_tr_len > usbd_xfer_max_len(xfer)) {
			sc->sc_tr_error = ENOBUFS;
			goto tr_exit;
		}

		/* try to extract the ID byte */
		id = (sc->sc_oid & (sc->sc_tr_len > 0)) ? sc->sc_tr_buf[0] : 0;

		if (sc->sc_tr_len > 0) {
			pc = usbd_xfer_get_frame(xfer, 1);
			usbd_copy_in(pc, 0, sc->sc_tr_buf, sc->sc_tr_len);
			usbd_xfer_set_frame_len(xfer, 1, sc->sc_tr_len);
		}

		usbhid_fill_set_report(&req, sc->sc_iface_no,
		    UHID_OUTPUT_REPORT, id, sc->sc_tr_len);

		pc = usbd_xfer_get_frame(xfer, 0);
		usbd_copy_in(pc, 0, &req, sizeof(req));
		usbd_xfer_set_frame_len(xfer, 0, sizeof(req));

		usbd_xfer_set_frames(xfer, sc->sc_tr_len > 0 ? 2 : 1);
		usbd_transfer_submit(xfer);
		return;

	case USB_ST_TRANSFERRED:
		sc->sc_tr_error = 0;
		goto tr_exit;

	default:			/* Error */
		DPRINTFN(1, "error=%s\n", usbd_errstr(error));
		sc->sc_tr_error = EIO;
tr_exit:
		if (!HID_IN_POLLING_MODE_FUNC())
			wakeup(sc);
		return;
	}
}

#ifdef NOT_YET
static void
usbhid_get_report_callback(struct usb_xfer *xfer, usb_error_t error)
{
	struct usbhid_softc *sc = usbd_xfer_softc(xfer);
	struct usb_device_request req;
	struct usb_page_cache *pc;

	pc = usbd_xfer_get_frame(xfer, 0);

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		usb_fifo_put_data(sc->sc_fifo.fp[USB_FIFO_RX], pc, sizeof(req),
		    sc->sc_isize, 1);
		return;

	case USB_ST_SETUP:

		if (usb_fifo_put_bytes_max(sc->sc_fifo.fp[USB_FIFO_RX]) > 0) {

			usbhid_fill_get_report
			    (&req, sc->sc_iface_no, UHID_INPUT_REPORT,
			    sc->sc_iid, sc->sc_isize);

			usbd_copy_in(pc, 0, &req, sizeof(req));

			usbd_xfer_set_frame_len(xfer, 0, sizeof(req));
			usbd_xfer_set_frame_len(xfer, 1, sc->sc_isize);
			usbd_xfer_set_frames(xfer, sc->sc_isize ? 2 : 1);
			usbd_transfer_submit(xfer);
		}
		return;

	default:			/* Error */
		/* bomb out */
		usb_fifo_put_data_error(sc->sc_fifo.fp[USB_FIFO_RX]);
		return;
	}
}
#endif

static const struct usb_config usbhid_config[USBHID_N_TRANSFER] = {

	[USBHID_INTR_DT_WR] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_OUT,
		.flags = {.pipe_bof = 1,.no_pipe_ok = 1,.proxy_buffer = 1},
		.callback = &usbhid_write_callback,
	},
	[USBHID_INTR_DT_RD] = {
		.type = UE_INTERRUPT,
		.endpoint = UE_ADDR_ANY,
		.direction = UE_DIR_IN,
		.flags = {.pipe_bof = 1,.short_xfer_ok = 1,.proxy_buffer = 1},
		.callback = &usbhid_read_callback,
	},
	[USBHID_CTRL_DT_WR] = {
		.type = UE_CONTROL,
		.endpoint = 0x00,	/* Control pipe */
		.direction = UE_DIR_ANY,
		.flags = {.proxy_buffer = 1},
		.callback = &usbhid_set_report_callback,
		.timeout = 1000,	/* 1 second */
	},
#ifdef NOT_YET
	[USBHID_CTRL_DT_RD] = {
		.type = UE_CONTROL,
		.endpoint = 0x00,	/* Control pipe */
		.direction = UE_DIR_ANY,
		.flags = {.proxy_buffer = 1},
		.callback = &usbhid_get_report_callback,
		.timeout = 1000,	/* 1 second */
	},
#endif
};

static void
usbhid_intr_setup(device_t dev, struct mtx *mtx, hid_intr_t intr,
    void *context)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	int error;

	sc->sc_intr_handler = intr;
	sc->sc_intr_context = context;
	sc->sc_intr_mtx = mtx;
	bcopy(usbhid_config, sc->sc_config, sizeof(usbhid_config));

	/* Set buffer sizes to match HID report sizes */
	sc->sc_config[USBHID_INTR_DT_WR].bufsize = sc->sc_osize;
	sc->sc_config[USBHID_INTR_DT_RD].bufsize = sc->sc_isize;
	sc->sc_config[USBHID_CTRL_DT_WR].bufsize =
	    MAX(sc->sc_osize, sc->sc_fsize);
#ifdef NOT_YET
	sc->sc_config[USBHID_CTRL_DT_RD].bufsize =
	    MAX(sc->sc_isize, sc->sc_fsize);
#endif

	error = usbd_transfer_setup(sc->sc_udev,
	    &sc->sc_iface_index, sc->sc_xfer, sc->sc_config,
	    USBHID_N_TRANSFER, sc, sc->sc_intr_mtx);

	if (error)
		DPRINTF("error=%s\n", usbd_errstr(error));
}

static void
usbhid_intr_unsetup(device_t dev)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	usbd_transfer_unsetup(sc->sc_xfer, USBHID_N_TRANSFER);
}

static int
usbhid_intr_start(device_t dev)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	mtx_assert(sc->sc_intr_mtx, MA_OWNED);

	usbd_transfer_start(sc->sc_xfer[USBHID_INTR_DT_RD]);

	return (0);
}

static int
usbhid_intr_stop(device_t dev)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	mtx_assert(sc->sc_intr_mtx, MA_OWNED);

	usbd_transfer_stop(sc->sc_xfer[USBHID_INTR_DT_RD]);

	return (0);
}

static void
usbhid_intr_poll(device_t dev)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	usbd_transfer_poll(sc->sc_xfer, USBHID_N_TRANSFER);
}

/*
 * HID interface
 */
static int
usbhid_get_report_desc(device_t dev, void **buf, uint16_t *len)
{
	struct usbhid_softc* sc = device_get_softc(dev);

	*buf = sc->sc_repdesc_ptr;
	*len = sc->sc_repdesc_size;

	return (0);
}

static int
usbhid_read(device_t dev, void *buf, uint16_t maxlen, uint16_t *actlen)
{

	return (ENOTSUP);
}

static int
usbhid_write(device_t dev, void *buf, uint16_t len)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	int error = 0;

	HID_MTX_LOCK(sc->sc_intr_mtx);
	sc->sc_tr_buf = buf;
	sc->sc_tr_len = len;

	if (sc->sc_xfer[USBHID_INTR_DT_WR] == NULL)
		usbd_transfer_start(sc->sc_xfer[USBHID_CTRL_DT_WR]);
	else
		usbd_transfer_start(sc->sc_xfer[USBHID_INTR_DT_WR]);

	if (!HID_IN_POLLING_MODE_FUNC() &&
	    msleep_sbt(sc, sc->sc_intr_mtx, 0, "usbhid wr",
	    SBT_1MS * USB_DEFAULT_TIMEOUT, 0, C_HARDCLOCK) == EWOULDBLOCK) {
		DPRINTF("USB write timed out\n");
		usbd_transfer_stop(sc->sc_xfer[USBHID_CTRL_DT_WR]);
		usbd_transfer_stop(sc->sc_xfer[USBHID_INTR_DT_WR]);
		error = ETIMEDOUT;
	} else
		error = sc->sc_tr_error;

	HID_MTX_UNLOCK(sc->sc_intr_mtx);

	return (error);
}

static int
usbhid_get_report(device_t dev, void *buf, uint16_t maxlen, uint16_t *actlen,
    uint8_t type, uint8_t id)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	int err;

	err = usbd_req_get_report(sc->sc_udev, NULL, buf,
	    maxlen, sc->sc_iface_index, type, id);
	if (err)
                err = ENXIO;
	else
		if (actlen != NULL)
			*actlen = maxlen;

	return (err);
}

static int
usbhid_set_report(device_t dev, void *buf, uint16_t len, uint8_t type,
    uint8_t id)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	int err;

	err = usbd_req_set_report(sc->sc_udev, NULL, buf,
	    len, sc->sc_iface_index, type, id);
	if (err)
                err = ENXIO;

	return (err);
}

static int
usbhid_set_idle(device_t dev, uint16_t duration, uint8_t id)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	int err;

	/* Duration is measured in 4 milliseconds per unit. */
	err = usbd_req_set_idle(sc->sc_udev, NULL, sc->sc_iface_index,
	    (duration + 3) / 4, id);
	if (err)
                err = ENXIO;

	return (err);
}

static int
usbhid_set_protocol(device_t dev, uint16_t protocol)
{
	struct usbhid_softc* sc = device_get_softc(dev);
	int err;

	err = usbd_req_set_protocol(sc->sc_udev, NULL, sc->sc_iface_index,
	    protocol);
	if (err)
                err = ENXIO;

	return (err);
}

static const STRUCT_USB_HOST_ID usbhid_devs[] = {
	/* generic HID class */
	{USB_IFACE_CLASS(UICLASS_HID),},
	/* the Xbox 360 gamepad doesn't use the HID class */
	{USB_IFACE_CLASS(UICLASS_VENDOR),
	 USB_IFACE_SUBCLASS(UISUBCLASS_XBOX360_CONTROLLER),
	 USB_IFACE_PROTOCOL(UIPROTO_XBOX360_GAMEPAD),},
};

static int
usbhid_probe(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	int error;

	DPRINTFN(11, "\n");

	if (uaa->usb_mode != USB_MODE_HOST)
		return (ENXIO);

	error = usbd_lookup_id_by_uaa(usbhid_devs, sizeof(usbhid_devs), uaa);
	if (error)
		return (error);

	if (usb_test_quirk(uaa, UQ_HID_IGNORE))
		return (ENXIO);

	return (USBHID_BUS_PROBE_PRIO);
}

static int
usbhid_attach(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	struct usbhid_softc *sc = device_get_softc(dev);
	char *sep;
	int error = 0;

	DPRINTFN(10, "sc=%p\n", sc);

	device_set_usb_desc(dev);

	sc->sc_udev = uaa->device;

	sc->sc_iface_no = uaa->info.bIfaceNum;
	sc->sc_iface_index = uaa->info.bIfaceIndex;

	if (uaa->info.idVendor == USB_VENDOR_WACOM) {

		/* the report descriptor for the Wacom Graphire is broken */

		if (uaa->info.idProduct == USB_PRODUCT_WACOM_GRAPHIRE) {

			sc->sc_repdesc_size = sizeof(usbhid_graphire_report_descr);
			sc->sc_repdesc_ptr = __DECONST(void *, &usbhid_graphire_report_descr);
			sc->sc_flags |= USBHID_FLAG_STATIC_DESC;

		} else if (uaa->info.idProduct == USB_PRODUCT_WACOM_GRAPHIRE3_4X5) {

			static uint8_t reportbuf[] = {2, 2, 2};

			/*
			 * The Graphire3 needs 0x0202 to be written to
			 * feature report ID 2 before it'll start
			 * returning digitizer data.
			 */
			error = usbd_req_set_report(uaa->device, NULL,
			    reportbuf, sizeof(reportbuf),
			    uaa->info.bIfaceIndex, UHID_FEATURE_REPORT, 2);

			if (error) {
				DPRINTF("set report failed, error=%s (ignored)\n",
				    usbd_errstr(error));
			}
			sc->sc_repdesc_size = sizeof(usbhid_graphire3_4x5_report_descr);
			sc->sc_repdesc_ptr = __DECONST(void *, &usbhid_graphire3_4x5_report_descr);
			sc->sc_flags |= USBHID_FLAG_STATIC_DESC;
		}
	} else if ((uaa->info.bInterfaceClass == UICLASS_VENDOR) &&
	    (uaa->info.bInterfaceSubClass == UISUBCLASS_XBOX360_CONTROLLER) &&
	    (uaa->info.bInterfaceProtocol == UIPROTO_XBOX360_GAMEPAD)) {
		static const uint8_t reportbuf[3] = {1, 3, 0};
		/*
		 * Turn off the four LEDs on the gamepad which
		 * are blinking by default:
		 */
		error = usbd_req_set_report(uaa->device, NULL,
		    __DECONST(void *, reportbuf), sizeof(reportbuf),
		    uaa->info.bIfaceIndex, UHID_OUTPUT_REPORT, 0);
		if (error) {
			DPRINTF("set output report failed, error=%s (ignored)\n",
			    usbd_errstr(error));
		}
		/* the Xbox 360 gamepad has no report descriptor */
		sc->sc_repdesc_size = sizeof(usbhid_xb360gp_report_descr);
		sc->sc_repdesc_ptr = __DECONST(void *, &usbhid_xb360gp_report_descr);
		sc->sc_flags |= USBHID_FLAG_STATIC_DESC;
	}
	if (sc->sc_repdesc_ptr == NULL) {

		error = usbd_req_get_hid_desc(uaa->device, NULL,
		    &sc->sc_repdesc_ptr, &sc->sc_repdesc_size,
		    M_USBDEV, uaa->info.bIfaceIndex);

		if (error) {
			device_printf(dev, "no report descriptor\n");
			goto detach;
		}
	}
	error = usbd_req_set_idle(uaa->device, NULL,
	    uaa->info.bIfaceIndex, 0, 0);

	if (error) {
		DPRINTF("set idle failed, error=%s (ignored)\n",
		    usbd_errstr(error));
	}
	sc->sc_isize = hid_report_size
	    (sc->sc_repdesc_ptr, sc->sc_repdesc_size, hid_input, &sc->sc_iid);

	sc->sc_osize = hid_report_size
	    (sc->sc_repdesc_ptr, sc->sc_repdesc_size, hid_output, &sc->sc_oid);

	sc->sc_fsize = hid_report_size
	    (sc->sc_repdesc_ptr, sc->sc_repdesc_size, hid_feature, &sc->sc_fid);

	if (sc->sc_isize > USBHID_RSIZE) {
		DPRINTF("input size is too large, "
		    "%d bytes (truncating)\n",
		    sc->sc_isize);
		sc->sc_isize = USBHID_RSIZE;
	}
	if (sc->sc_osize > USBHID_RSIZE) {
		DPRINTF("output size is too large, "
		    "%d bytes (truncating)\n",
		    sc->sc_osize);
		sc->sc_osize = USBHID_RSIZE;
	}
	if (sc->sc_fsize > USBHID_RSIZE) {
		DPRINTF("feature size is too large, "
		    "%d bytes (truncating)\n",
		    sc->sc_fsize);
		sc->sc_fsize = USBHID_RSIZE;
	}
	sc->sc_ibuf = malloc(sc->sc_isize, M_USBDEV, M_ZERO | M_WAITOK);

	sc->sc_hw.parent = dev;
	strlcpy(sc->sc_hw.name, device_get_desc(dev), sizeof(sc->sc_hw.name));
	/* Strip extra parameters from device name created by usb_devinfo */
	sep = strchr(sc->sc_hw.name, ',');
	if (sep != NULL)
		*sep = '\0';
	strlcpy(sc->sc_hw.serial, usb_get_serial(uaa->device),
	    sizeof(sc->sc_hw.serial));
	sc->sc_hw.idBus = BUS_USB;
	sc->sc_hw.idVendor = uaa->info.idVendor;
	sc->sc_hw.idProduct = uaa->info.idProduct;
	sc->sc_hw.idVersion = 0;

	sc->sc_child = device_add_child(dev, "hidbus", -1);
	if (sc->sc_child == NULL) {
		device_printf(dev, "Could not add hidbus device\n");
		error = ENXIO;
		goto detach;
	}

	device_set_ivars(sc->sc_child, &sc->sc_hw);
	error = bus_generic_attach(dev);
	if (error)
		device_printf(dev, "failed to attach child: %d\n", error);

	return (0);			/* success */

detach:
	usbhid_detach(dev);
	return (ENOMEM);
}

static int
usbhid_detach(device_t dev)
{
	struct usbhid_softc *sc = device_get_softc(dev);

	if (device_is_attached(dev))
		bus_generic_detach(dev);
	if (sc->sc_child)
		device_delete_child(dev, sc->sc_child);

	if (sc->sc_repdesc_ptr) {
		if (!(sc->sc_flags & USBHID_FLAG_STATIC_DESC)) {
			free(sc->sc_repdesc_ptr, M_USBDEV);
		}
	}
	free(sc->sc_ibuf, M_USBDEV);

	return (0);
}

static devclass_t usbhid_devclass;

static device_method_t usbhid_methods[] = {
	DEVMETHOD(device_probe, usbhid_probe),
	DEVMETHOD(device_attach, usbhid_attach),
	DEVMETHOD(device_detach, usbhid_detach),

	DEVMETHOD(hid_intr_setup,	usbhid_intr_setup),
	DEVMETHOD(hid_intr_unsetup,	usbhid_intr_unsetup),
	DEVMETHOD(hid_intr_start,	usbhid_intr_start),
	DEVMETHOD(hid_intr_stop,	usbhid_intr_stop),
	DEVMETHOD(hid_intr_poll,	usbhid_intr_poll),

	/* HID interface */
	DEVMETHOD(hid_get_report_descr,	usbhid_get_report_desc),
	DEVMETHOD(hid_read,		usbhid_read),
	DEVMETHOD(hid_write,		usbhid_write),
	DEVMETHOD(hid_get_report,	usbhid_get_report),
	DEVMETHOD(hid_set_report,	usbhid_set_report),
	DEVMETHOD(hid_set_idle,		usbhid_set_idle),
	DEVMETHOD(hid_set_protocol,	usbhid_set_protocol),

	DEVMETHOD_END
};

static driver_t usbhid_driver = {
	.name = "usbhid",
	.methods = usbhid_methods,
	.size = sizeof(struct usbhid_softc),
};

DRIVER_MODULE(usbhid, uhub, usbhid_driver, usbhid_devclass, NULL, 0);
MODULE_DEPEND(usbhid, usb, 1, 1, 1);
MODULE_DEPEND(usbhid, hid, 1, 1, 1);
MODULE_DEPEND(usbhid, hidbus, 1, 1, 1);
MODULE_VERSION(usbhid, 1);
USB_PNP_HOST_INFO(usbhid_devs);
