/*-
 * Copyright (c) 2014-2019 Vladimir Kondratyev <wulf@FreeBSD.org>
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
 * MS Windows 7/8/10 compatible I2C HID Multi-touch Device driver.
 * https://msdn.microsoft.com/en-us/library/windows/hardware/jj151569(v=vs.85).aspx
 * http://download.microsoft.com/download/7/d/d/7dd44bb7-2a7a-4505-ac1c-7227d3d96d5b/hid-over-i2c-protocol-spec-v1-0.docx
 * https://www.kernel.org/doc/Documentation/input/multi-touch-protocol.txt
 */

#include <sys/param.h>
#include <sys/bitstring.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <dev/evdev/evdev.h>
#include <dev/evdev/input.h>

#include "hid.h"
#include "hidbus.h"

#define	HID_DEBUG_VAR	hmt_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hmt_debug = 0;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hmt, CTLFLAG_RW, 0,
    "MSWindows 7/8/10 compatible HID Multi-touch Device");
SYSCTL_INT(_hw_hid_hmt, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hmt_debug, 1, "Debug level");
#endif

#define	HMT_BTN_MAX	8	/* Number of buttons supported */

enum {
	HMT_INTR_DT,
	HMT_N_TRANSFER,
};

enum hmt_input_mode {
	HMT_INPUT_MODE_MOUSE =		0x0,
	HMT_INPUT_MODE_MT_TOUCHSCREEN =	0x2,
	HMT_INPUT_MODE_MT_TOUCHPAD =	0x3,
};

enum {
	HMT_TIP_SWITCH,
#define	HMT_SLOT	HMT_TIP_SWITCH
	HMT_WIDTH,
#define	HMT_MAJOR	HMT_WIDTH
	HMT_HEIGHT,
#define HMT_MINOR	HMT_HEIGHT
	HMT_ORIENTATION,
	HMT_X,
	HMT_Y,
	HMT_CONTACTID,
	HMT_PRESSURE,
	HMT_IN_RANGE,
	HMT_CONFIDENCE,
	HMT_TOOL_X,
	HMT_TOOL_Y,
	HMT_N_USAGES,
};

#define	HMT_NO_CODE	(ABS_MAX + 10)
#define	HMT_NO_USAGE	-1

struct hmt_hid_map_item {
	char		name[5];
	int32_t 	usage;		/* HID usage */
	uint32_t	code;		/* Evdev event code */
	bool		required;	/* Required for MT Digitizers */
};

static const struct hmt_hid_map_item hmt_hid_map[HMT_N_USAGES] = {

	[HMT_TIP_SWITCH] = {	/* HMT_SLOT */
		.name = "TIP",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_SWITCH),
		.code = ABS_MT_SLOT,
		.required = true,
	},
	[HMT_WIDTH] = {		/* HMT_MAJOR */
		.name = "WDTH",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_WIDTH),
		.code = ABS_MT_TOUCH_MAJOR,
		.required = false,
	},
	[HMT_HEIGHT] = {	/* HMT_MINOR */
		.name = "HGHT",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_HEIGHT),
		.code = ABS_MT_TOUCH_MINOR,
		.required = false,
	},
	[HMT_ORIENTATION] = {
		.name = "ORIE",
		.usage = HMT_NO_USAGE,
		.code = ABS_MT_ORIENTATION,
		.required = false,
	},
	[HMT_X] = {
		.name = "X",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X),
		.code = ABS_MT_POSITION_X,
		.required = true,
	},
	[HMT_Y] = {
		.name = "Y",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y),
		.code = ABS_MT_POSITION_Y,
		.required = true,
	},
	[HMT_CONTACTID] = {
		.name = "C_ID",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACTID),
		.code = ABS_MT_TRACKING_ID,
		.required = true,
	},
	[HMT_PRESSURE] = {
		.name = "PRES",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_TIP_PRESSURE),
		.code = ABS_MT_PRESSURE,
		.required = false,
	},
	[HMT_IN_RANGE] = {
		.name = "RANG",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_IN_RANGE),
		.code = ABS_MT_DISTANCE,
		.required = false,
	},
	[HMT_CONFIDENCE] = {
		.name = "CONF",
		.usage = HID_USAGE2(HUP_DIGITIZERS, HUD_CONFIDENCE),
		.code = HMT_NO_CODE,
		.required = false,
	},
	[HMT_TOOL_X] = {	/* Shares HID usage with HMT_X */
		.name = "TL_X",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_X),
		.code = ABS_MT_TOOL_X,
		.required = false,
	},
	[HMT_TOOL_Y] = {	/* Shares HID usage with HMT_Y */
		.name = "TL_Y",
		.usage = HID_USAGE2(HUP_GENERIC_DESKTOP, HUG_Y),
		.code = ABS_MT_TOOL_Y,
		.required = false,
	},
};

struct hmt_absinfo {
	int32_t			min;
	int32_t			max;
	int32_t			res;
};

#define	USAGE_SUPPORTED(caps, usage)	bit_test(caps, usage)
#define	HMT_FOREACH_USAGE(caps, usage)			\
	for ((usage) = 0; (usage) < HMT_N_USAGES; ++(usage))	\
		if (USAGE_SUPPORTED((caps), (usage)))
#define	HMT_FOREACH_BUTTON(buttons, button)			\
	for ((button) = 0; (button) < HMT_BTN_MAX; ++(button))	\
		if (bit_test(buttons, button))

struct hmt_softc {
	device_t dev;
	int			type;

	struct hmt_absinfo      ai[HMT_N_USAGES];
	struct hid_location     locs[MAX_MT_SLOTS][HMT_N_USAGES];
	struct hid_location     cont_count_loc;
	struct hid_location	btn_loc[HMT_BTN_MAX];

	struct evdev_dev        *evdev;

	uint32_t                slot_data[HMT_N_USAGES];
	bitstr_t		bit_decl(caps, HMT_N_USAGES);
	bitstr_t		bit_decl(buttons, HMT_BTN_MAX);
	uint32_t                isize;
	uint32_t                nconts_per_report;
	uint32_t		nconts_todo;
	uint8_t                 report_id;
	bool			has_buttons;
	bool			is_clickpad;

	struct hid_location     cont_max_loc;
	uint32_t                cont_max_rlen;
	uint8_t                 cont_max_rid;
	struct hid_location	btn_type_loc;
	uint32_t		btn_type_rlen;
	uint8_t			btn_type_rid;
	uint32_t                thqa_cert_rlen;
	uint8_t                 thqa_cert_rid;
	struct hid_location	input_mode_loc;
	uint32_t		input_mode_rlen;
	uint8_t			input_mode_rid;
};

static int hmt_hid_parse(struct hmt_softc *, const void *, uint16_t);
static void hmt_devcaps_parse(struct hmt_softc *, const void *, uint16_t);
static int hmt_set_input_mode(struct hmt_softc *, enum hmt_input_mode);

static hid_intr_t		hmt_intr;

static device_probe_t		hmt_probe;
static device_attach_t		hmt_attach;
static device_detach_t		hmt_detach;
static device_resume_t		hmt_resume;

static evdev_open_t	hmt_ev_open;
static evdev_close_t	hmt_ev_close;

static devclass_t hmt_devclass;

static device_method_t hmt_methods[] = {

	DEVMETHOD(device_probe,		hmt_probe),
	DEVMETHOD(device_attach,	hmt_attach),
	DEVMETHOD(device_detach,	hmt_detach),
	DEVMETHOD(device_resume,	hmt_resume),

	DEVMETHOD_END
};

static driver_t hmt_driver = {
	.name = "hmt",
	.methods = hmt_methods,
	.size = sizeof(struct hmt_softc),
};

static const struct evdev_methods hmt_evdev_methods = {
	.ev_open = &hmt_ev_open,
	.ev_close = &hmt_ev_close,
};

static const struct hid_device_id hmt_devs[] = {
	{ HID_TLC(HUP_DIGITIZERS, HUD_TOUCHSCREEN) },
	{ HID_TLC(HUP_DIGITIZERS, HUD_TOUCHPAD) },
};

static int
hmt_ev_close(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	return (hid_stop(dev));
}

static int
hmt_ev_open(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	return (hid_start(dev));
}

static int
hmt_probe(device_t dev)
{
	struct hid_tlc_info *tlc = device_get_ivars(dev);
	void *d_ptr;
	uint16_t d_len;
	int error;

	error = hid_lookup_driver_info(hmt_devs, sizeof(hmt_devs), tlc);
	if (error != 0)
		return (error);

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error != 0) {
		device_printf(dev, "could not retrieve report descriptor from "
		     "device: %d\n", error);
		return (ENXIO);
	}

	/* Check if report descriptor belongs to a HID multitouch device */
	if (hmt_hid_parse(NULL, d_ptr, d_len) == 0)
		return (ENXIO);

	return (BUS_PROBE_DEFAULT);
}

static int
hmt_attach(device_t dev)
{
	struct hmt_softc *sc = device_get_softc(dev);
	struct hid_tlc_info *tlc = device_get_ivars(dev);
	struct hid_device_info *hw = tlc->device_info;
	void *d_ptr, *fbuf = NULL;
	uint16_t d_len, fsize;
	int nbuttons;
	size_t i;
	uint8_t fid;
	int error;

	device_set_desc(dev, hw->name);

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error) {
		device_printf(dev, "could not retrieve report descriptor from "
		    "device: %d\n", error);
		return (ENXIO);
	}

	sc->dev = dev;

	sc->type = hmt_hid_parse(sc, d_ptr, d_len);
	if (sc->type == 0) {
		DPRINTF("multi-touch HID descriptor not found\n");
		return (ENXIO);
	}

	fsize = hid_report_size(d_ptr, d_len, hid_feature, &fid);
	if (fsize != 0)
		fbuf = malloc(fsize, M_TEMP, M_WAITOK | M_ZERO);

	/* Fetch and parse "Contact count maximum" feature report */
	if (sc->cont_max_rlen > 1) {
		error = hid_get_report(dev, fbuf, sc->cont_max_rlen,
		    HID_FEATURE_REPORT, sc->cont_max_rid);
		if (error == 0)
			hmt_devcaps_parse(sc, fbuf, sc->cont_max_rlen);
		else
			DPRINTF("usbd_req_get_report error=%d\n", error);
	} else
		DPRINTF("Feature report %hhu size invalid: %u\n",
		    sc->cont_max_rid, sc->cont_max_rlen);

	/* Fetch THQA certificate to enable some devices like WaveShare */
	if (sc->thqa_cert_rlen > 1 && sc->thqa_cert_rid != sc->cont_max_rid)
		(void)hid_get_report(dev, fbuf, sc->thqa_cert_rlen,
		    HID_FEATURE_REPORT, sc->thqa_cert_rid);

	free(fbuf, M_TEMP);

	if (sc->type == HUD_TOUCHPAD && sc->input_mode_rlen > 1) {
		error = hmt_set_input_mode(sc, HMT_INPUT_MODE_MT_TOUCHPAD);
		if (error) {
			DPRINTF("Failed to set input mode: %d\n", error);
			return (ENXIO);
		}
	}

	/* Cap contact count maximum to MAX_MT_SLOTS */
	if (sc->ai[HMT_SLOT].max >= MAX_MT_SLOTS) {
		DPRINTF("Hardware reported %d contacts while only %d is "
		    "supported\n", (int)sc->ai[HMT_SLOT].max+1, MAX_MT_SLOTS);
		sc->ai[HMT_SLOT].max = MAX_MT_SLOTS - 1;
	}

	hid_set_intr(dev, hmt_intr);

	sc->evdev = evdev_alloc();
	evdev_set_name(sc->evdev, device_get_desc(dev));
	evdev_set_phys(sc->evdev, device_get_nameunit(dev));
	evdev_set_id(sc->evdev, hw->idBus, hw->idVendor, hw->idProduct,
	    hw->idVersion);
	evdev_set_serial(sc->evdev, hw->serial);
	evdev_set_methods(sc->evdev, dev, &hmt_evdev_methods);
	evdev_set_flag(sc->evdev, EVDEV_FLAG_MT_STCOMPAT);
	switch (sc->type) {
	case HUD_TOUCHSCREEN:
		evdev_support_prop(sc->evdev, INPUT_PROP_DIRECT);
		break;
	case HUD_TOUCHPAD:
		evdev_support_prop(sc->evdev, INPUT_PROP_POINTER);
		if (sc->is_clickpad)
			evdev_support_prop(sc->evdev, INPUT_PROP_BUTTONPAD);
	}
	evdev_support_event(sc->evdev, EV_SYN);
	evdev_support_event(sc->evdev, EV_ABS);
	if (sc->has_buttons) {
		evdev_support_event(sc->evdev, EV_KEY);
		HMT_FOREACH_BUTTON(sc->buttons, i)
			evdev_support_key(sc->evdev, BTN_MOUSE + i);
	}
	HMT_FOREACH_USAGE(sc->caps, i) {
		if (hmt_hid_map[i].code != HMT_NO_CODE)
			evdev_support_abs(sc->evdev, hmt_hid_map[i].code, 0,
			    sc->ai[i].min, sc->ai[i].max, 0, 0, sc->ai[i].res);
	}

	error = evdev_register_mtx(sc->evdev, hid_get_lock(sc->dev));
	if (error) {
		hmt_detach(dev);
		return (ENXIO);
	}

	/* Announce information about the touch device */
	bit_count(sc->buttons, 0, HMT_BTN_MAX, &nbuttons);
	device_printf(sc->dev, "Multitouch %s with %d button%s%s\n",
	    sc->type == HUD_TOUCHSCREEN ? "touchscreen" : "touchpad",
	    nbuttons, nbuttons != 1 ? "s" : "",
	    sc->is_clickpad ? ", click-pad" : "");
	device_printf(sc->dev,
	    "%d contacts with [%s%s%s%s%s] properties. Report range [%d:%d] - [%d:%d]\n",
	    (int)sc->ai[HMT_SLOT].max + 1,
	    USAGE_SUPPORTED(sc->caps, HMT_IN_RANGE) ? "R" : "",
	    USAGE_SUPPORTED(sc->caps, HMT_CONFIDENCE) ? "C" : "",
	    USAGE_SUPPORTED(sc->caps, HMT_WIDTH) ? "W" : "",
	    USAGE_SUPPORTED(sc->caps, HMT_HEIGHT) ? "H" : "",
	    USAGE_SUPPORTED(sc->caps, HMT_PRESSURE) ? "P" : "",
	    (int)sc->ai[HMT_X].min, (int)sc->ai[HMT_Y].min,
	    (int)sc->ai[HMT_X].max, (int)sc->ai[HMT_Y].max);

	return (0);
}

static int
hmt_detach(device_t dev)
{
	struct hmt_softc *sc = device_get_softc(dev);

	evdev_free(sc->evdev);

	return (0);
}

static int
hmt_resume(device_t dev)
{
	struct hmt_softc *sc = device_get_softc(dev);
	int error;

	if (sc->type == HUD_TOUCHPAD && sc->input_mode_rlen > 1) {
		error = hmt_set_input_mode(sc, HMT_INPUT_MODE_MT_TOUCHPAD);
		if (error)
			DPRINTF("Failed to set input mode: %d\n", error);
	}

	return (0);
}

static void
hmt_intr(void *context, void *buf, uint16_t len)
{
	device_t dev = context;
	struct hmt_softc *sc = device_get_softc(dev);
	size_t usage;
	uint32_t *slot_data = sc->slot_data;
	uint32_t cont, btn;
	uint32_t cont_count;
	uint32_t width;
	uint32_t height;
	int32_t slot;
	uint8_t id;

	mtx_assert(hid_get_lock(sc->dev), MA_OWNED);

	/*
	 * Special packet of zero length is generated by iichid driver running
	 * in polling mode at the start of inactivity period to workaround
	 * "stuck touch" problem caused by miss of finger release events.
	 * This snippet is to be removed after GPIO interrupt support is added.
	 */
	if (len == 0) {
		for (slot = 0; slot <= sc->ai[HMT_SLOT].max; slot++) {
			evdev_push_abs(sc->evdev, ABS_MT_SLOT, slot);
			evdev_push_abs(sc->evdev, ABS_MT_TRACKING_ID, -1);
		}
		evdev_sync(sc->evdev);
		return;
	}

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

	/*
	 * "In Parallel mode, devices report all contact information in a
	 * single packet. Each physical contact is represented by a logical
	 * collection that is embedded in the top-level collection."
	 *
	 * Since additional contacts that were not present will still be in the
	 * report with contactid=0 but contactids are zero-based, find
	 * contactcount first.
	 */
	cont_count = hid_get_data_unsigned(buf, len, &sc->cont_count_loc);
	/*
	 * "In Hybrid mode, the number of contacts that can be reported in one
	 * report is less than the maximum number of contacts that the device
	 * supports. For example, a device that supports a maximum of
	 * 4 concurrent physical contacts, can set up its top-level collection
	 * to deliver a maximum of two contacts in one report. If four contact
	 * points are present, the device can break these up into two serial
	 * reports that deliver two contacts each.
	 *
	 * "When a device delivers data in this manner, the Contact Count usage
	 * value in the first report should reflect the total number of
	 * contacts that are being delivered in the hybrid reports. The other
	 * serial reports should have a contact count of zero (0)."
	 */
	if (cont_count != 0)
		sc->nconts_todo = cont_count;

#ifdef HID_DEBUG
	DPRINTFN(6, "cont_count:%2u", (unsigned)cont_count);
	if (hmt_debug >= 6) {
		HMT_FOREACH_USAGE(sc->caps, usage) {
			if (hmt_hid_map[usage].usage != HMT_NO_USAGE)
				printf(" %-4s", hmt_hid_map[usage].name);
		}
		printf("\n");
	}
#endif

	/* Find the number of contacts reported in current report */
	cont_count = MIN(sc->nconts_todo, sc->nconts_per_report);

	/* Use protocol Type B for reporting events */
	for (cont = 0; cont < cont_count; cont++) {

		bzero(slot_data, sizeof(sc->slot_data));
		HMT_FOREACH_USAGE(sc->caps, usage) {
			if (sc->locs[cont][usage].size > 0)
				slot_data[usage] = hid_get_data_unsigned(
				    buf, len, &sc->locs[cont][usage]);
		}

		slot = evdev_get_mt_slot_by_tracking_id(sc->evdev,
		    slot_data[HMT_CONTACTID]);

#ifdef HID_DEBUG
		DPRINTFN(6, "cont%01x: data = ", cont);
		if (hmt_debug >= 6) {
			HMT_FOREACH_USAGE(sc->caps, usage) {
				if (hmt_hid_map[usage].usage != HMT_NO_USAGE)
					printf("%04x ", slot_data[usage]);
			}
			printf("slot = %d\n", (int)slot);
		}
#endif

		if (slot == -1) {
			DPRINTF("Slot overflow for contact_id %u\n",
			    (unsigned)slot_data[HMT_CONTACTID]);
			continue;
		}

		if (slot_data[HMT_TIP_SWITCH] != 0 &&
		    !(USAGE_SUPPORTED(sc->caps, HMT_CONFIDENCE) &&
		      slot_data[HMT_CONFIDENCE] == 0)) {
			/* This finger is in proximity of the sensor */
			slot_data[HMT_SLOT] = slot;
			slot_data[HMT_IN_RANGE] = !slot_data[HMT_IN_RANGE];
			/* Divided by two to match visual scale of touch */
			width = slot_data[HMT_WIDTH] >> 1;
			height = slot_data[HMT_HEIGHT] >> 1;
			slot_data[HMT_ORIENTATION] = width > height;
			slot_data[HMT_MAJOR] = MAX(width, height);
			slot_data[HMT_MINOR] = MIN(width, height);

			HMT_FOREACH_USAGE(sc->caps, usage)
				if (hmt_hid_map[usage].code != HMT_NO_CODE)
					evdev_push_abs(sc->evdev,
					    hmt_hid_map[usage].code,
					    slot_data[usage]);
		} else {
			evdev_push_abs(sc->evdev, ABS_MT_SLOT, slot);
			evdev_push_abs(sc->evdev, ABS_MT_TRACKING_ID, -1);
		}
	}

	sc->nconts_todo -= cont_count;
	if (sc->nconts_todo == 0) {
		if (sc->has_buttons) {
			HMT_FOREACH_BUTTON(sc->buttons, btn)
				evdev_push_key(sc->evdev, BTN_MOUSE + btn,
				    hid_get_data(buf,
						 len,
						 &sc->btn_loc[btn]) != 0);
		}
		evdev_sync(sc->evdev);
	}
}

/* Port of userland hid_report_size() from usbhid(3) to kernel */
static int
hmt_hid_report_size(const void *buf, uint16_t len, enum hid_kind k, uint8_t id)
{
	struct hid_data *d;
	struct hid_item h;
	uint32_t temp;
	uint32_t hpos;
	uint32_t lpos;
	int report_id = 0;

	hpos = 0;
	lpos = 0xFFFFFFFF;

	for (d = hid_start_parse(buf, len, 1 << k); hid_get_item(d, &h);) {
		if (h.kind == k && h.report_ID == id) {
			/* compute minimum */
			if (lpos > h.loc.pos)
				lpos = h.loc.pos;
			/* compute end position */
			temp = h.loc.pos + (h.loc.size * h.loc.count);
			/* compute maximum */
			if (hpos < temp)
				hpos = temp;
			if (h.report_ID != 0)
				report_id = 1;
		}
	}
	hid_end_parse(d);

	/* safety check - can happen in case of currupt descriptors */
	if (lpos > hpos)
		temp = 0;
	else
		temp = hpos - lpos;

	/* return length in bytes rounded up */
	return ((temp + 7) / 8 + report_id);
}

static int
hmt_hid_parse(struct hmt_softc *sc, const void *d_ptr, uint16_t d_len)
{
	struct hid_item hi;
	struct hid_data *hd;
	size_t i;
	size_t cont = 0;
	int type = 0;
	int nbuttons = 0;
	bitstr_t bit_decl(caps, HMT_N_USAGES);
	bitstr_t bit_decl(buttons, HMT_BTN_MAX);
	uint32_t btn;
	int32_t cont_count_max = 0;
	uint8_t report_id = 0;
	uint8_t cont_max_rid = 0;
	uint8_t btn_type_rid = 0;
	uint8_t thqa_cert_rid = 0;
	uint8_t input_mode_rid = 0;
	bool touch_coll = false;
	bool finger_coll = false;
	bool conf_coll = false;
	bool cont_count_found = false;
	bool scan_time_found = false;

#define HMT_HI_ABSOLUTE(hi)	\
	(((hi).flags & (HIO_CONST|HIO_VARIABLE|HIO_RELATIVE)) == HIO_VARIABLE)
#define	HUMS_THQA_CERT	0xC5

	/* Parse features for maximum contact count */
	hd = hid_start_parse(d_ptr, d_len, 1 << hid_feature);
	while (hid_get_item(hd, &hi)) {
		switch (hi.kind) {
		case hid_collection:
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHSCREEN))
				touch_coll = true;
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHPAD))
				touch_coll = true;
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_CONFIG))
				conf_coll = true;
			break;
		case hid_endcollection:
			if (hi.collevel == 0 && touch_coll)
				touch_coll = false;
			break;
			if (hi.collevel == 0 && conf_coll)
				conf_coll = false;
			break;
		case hid_feature:
			if (hi.collevel == 1 && touch_coll && hi.usage ==
			      HID_USAGE2(HUP_MICROSOFT, HUMS_THQA_CERT)) {
				thqa_cert_rid = hi.report_ID;
				break;
			}
			if (hi.collevel == 1 && touch_coll &&
			    HMT_HI_ABSOLUTE(hi) && hi.usage ==
			      HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACT_MAX)) {
				cont_count_max = hi.logical_maximum;
				cont_max_rid = hi.report_ID;
				if (sc != NULL)
					sc->cont_max_loc = hi.loc;
			}
			if (hi.collevel == 1 && touch_coll &&
			    HMT_HI_ABSOLUTE(hi) && hi.usage ==
			      HID_USAGE2(HUP_DIGITIZERS, HUD_BUTTON_TYPE)) {
				btn_type_rid = hi.report_ID;
				if (sc != NULL)
					sc->btn_type_loc = hi.loc;
			}
			if (conf_coll && HMT_HI_ABSOLUTE(hi) && hi.usage ==
			      HID_USAGE2(HUP_DIGITIZERS, HUD_INPUT_MODE)) {
				input_mode_rid = hi.report_ID;
				if (sc != NULL)
					sc->input_mode_loc = hi.loc;
			}
			break;
		default:
			break;
		}
	}
	hid_end_parse(hd);

	/* Maximum contact count is required usage */
	if (cont_max_rid == 0)
		return (0);

	touch_coll = false;
	bzero(caps, bitstr_size(HMT_N_USAGES));
	bzero(buttons, bitstr_size(HMT_BTN_MAX));

	/* Parse input for other parameters */
	hd = hid_start_parse(d_ptr, d_len, 1 << hid_input);
	while (hid_get_item(hd, &hi)) {
		switch (hi.kind) {
		case hid_collection:
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHSCREEN)) {
				touch_coll = true;
				type = HUD_TOUCHSCREEN;
			}
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHPAD)) {
				touch_coll = true;
				type = HUD_TOUCHPAD;
			}
			else if (touch_coll && hi.collevel == 2 &&
			    (report_id == 0 || report_id == hi.report_ID) &&
			    hi.usage == HID_USAGE2(HUP_DIGITIZERS, HUD_FINGER))
				finger_coll = true;
			break;
		case hid_endcollection:
			if (hi.collevel == 1 && finger_coll) {
				finger_coll = false;
				cont++;
			} else if (hi.collevel == 0 && touch_coll)
				touch_coll = false;
			break;
		case hid_input:
			/*
			 * Ensure that all usages are located within the same
			 * report and proper collection.
			 */
			if (HMT_HI_ABSOLUTE(hi) && touch_coll &&
			    (report_id == 0 || report_id == hi.report_ID))
				report_id = hi.report_ID;
			else
				break;

			if (hi.collevel == 1 &&
			    hi.usage > HID_USAGE2(HUP_BUTTON, 0) &&
			    hi.usage <= HID_USAGE2(HUP_BUTTON, HMT_BTN_MAX)) {
				btn = (hi.usage & 0xFFFF) - 1;
				bit_set(buttons, btn);
				if (sc != NULL)
					sc->btn_loc[btn] = hi.loc;
				break;
			}
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_CONTACTCOUNT)) {
				cont_count_found = true;
				if (sc != NULL)
					sc->cont_count_loc = hi.loc;
				break;
			}
			/* Scan time is required but clobbered by evdev */
			if (hi.collevel == 1 && hi.usage ==
			    HID_USAGE2(HUP_DIGITIZERS, HUD_SCAN_TIME)) {
				scan_time_found = true;
				break;
			}

			if (!finger_coll || hi.collevel != 2)
				break;
			if (sc == NULL && cont > 0)
				break;
			if (cont >= MAX_MT_SLOTS) {
				DPRINTF("Finger %zu ignored\n", cont);
				break;
			}

			for (i = 0; i < HMT_N_USAGES; i++) {
				if (hi.usage == hmt_hid_map[i].usage) {
					if (sc == NULL) {
						if (USAGE_SUPPORTED(caps, i))
							continue;
						bit_set(caps, i);
						break;
					}
					/*
					 * HUG_X usage is an array mapped to
					 * both ABS_MT_POSITION and ABS_MT_TOOL
					 * events. So don`t stop search if we
					 * already have HUG_X mapping done.
					 */
					if (sc->locs[cont][i].size)
						continue;
					sc->locs[cont][i] = hi.loc;
					/*
					 * Hid parser returns valid logical and
					 * physical sizes for first finger only
					 * at least on ElanTS 0x04f3:0x0012.
					 */
					if (cont > 0)
						break;
					bit_set(caps, i);
					sc->ai[i] = (struct hmt_absinfo) {
					    .max = hi.logical_maximum,
					    .min = hi.logical_minimum,
					    .res = hid_item_resolution(&hi),
					};
					break;
				}
			}
			break;
		default:
			break;
		}
	}
	hid_end_parse(hd);

	/* Check for required HID Usages */
	if (!cont_count_found || !scan_time_found || cont == 0)
		return (0);
	for (i = 0; i < HMT_N_USAGES; i++) {
		if (hmt_hid_map[i].required && !USAGE_SUPPORTED(caps, i))
			return (0);
	}

	/* Touchpads must have at least one button */
	bit_count(buttons, 0, HMT_BTN_MAX, &nbuttons);
	if (type == HUD_TOUCHPAD && nbuttons == 0)
		return (0);

	/* Stop probing here */
	if (sc == NULL)
		return (type);

	/*
	 * According to specifications 'Contact Count Maximum' should be read
	 * from Feature Report rather than from HID descriptor. Set sane
	 * default value now to handle the case of 'Get Report' request failure
	 */
	if (cont_count_max < 1)
		cont_count_max = cont;

	/* Set number of MT protocol type B slots */
	sc->ai[HMT_SLOT] = (struct hmt_absinfo) {
		.min = 0,
		.max = cont_count_max - 1,
		.res = 0,
	};

	/* Report touch orientation if both width and height are supported */
	if (USAGE_SUPPORTED(caps, HMT_WIDTH) &&
	    USAGE_SUPPORTED(caps, HMT_HEIGHT)) {
		bit_set(caps, HMT_ORIENTATION);
		sc->ai[HMT_ORIENTATION].max = 1;
	}

	sc->isize = hmt_hid_report_size(d_ptr, d_len, hid_input, report_id);
	sc->cont_max_rlen = hmt_hid_report_size(d_ptr, d_len, hid_feature,
	    cont_max_rid);
	if (btn_type_rid > 0)
		sc->btn_type_rlen = hmt_hid_report_size(d_ptr, d_len,
		    hid_feature, btn_type_rid);
	if (thqa_cert_rid > 0)
		sc->thqa_cert_rlen = hmt_hid_report_size(d_ptr, d_len,
		    hid_feature, thqa_cert_rid);
	if (input_mode_rid > 0)
		sc->input_mode_rlen = hmt_hid_report_size(d_ptr, d_len,
		    hid_feature, input_mode_rid);

	sc->report_id = report_id;
	bcopy(caps, sc->caps, bitstr_size(HMT_N_USAGES));
	bcopy(buttons, sc->buttons, bitstr_size(HMT_BTN_MAX));
	sc->nconts_per_report = cont;
	sc->cont_max_rid = cont_max_rid;
	sc->btn_type_rid = btn_type_rid;
	sc->thqa_cert_rid = thqa_cert_rid;
	sc->input_mode_rid = input_mode_rid;
	sc->has_buttons = nbuttons != 0;

	return (type);
}

/* Device capabilities feature report */
static void
hmt_devcaps_parse(struct hmt_softc *sc, const void *r_ptr, uint16_t r_len)
{
	uint32_t cont_count_max;
	const uint8_t *rep = (const uint8_t *)r_ptr + 1;
	uint16_t len = r_len - 1;

	/* Feature report is a primary source of 'Contact Count Maximum' */
	cont_count_max = hid_get_data_unsigned(rep, len, &sc->cont_max_loc);
	if (cont_count_max > 0)
		sc->ai[HMT_SLOT].max = cont_count_max - 1;

	/* Assume that contact count shares the same report */
	if (sc->btn_type_rid == sc->cont_max_rid)
		sc->is_clickpad =
		    hid_get_data_unsigned(rep, len, &sc->btn_type_loc) == 0;
}

static int
hmt_set_input_mode(struct hmt_softc *sc, enum hmt_input_mode mode)
{
	uint8_t *fbuf;
	int error;

	fbuf = malloc(sc->input_mode_rlen, M_TEMP, M_WAITOK | M_ZERO);

	/* Input Mode report is not strictly required to be readable */
	error = hid_get_report(sc->dev, fbuf, sc->input_mode_rlen,
	    HID_FEATURE_REPORT, sc->input_mode_rid);
	if (error)
		bzero(fbuf + 1, sc->input_mode_rlen - 1);

	fbuf[0] = sc->input_mode_rid;
	hid_put_data_unsigned(fbuf + 1, sc->input_mode_rlen - 1,
	    &sc->input_mode_loc, mode);

	error = hid_set_report(sc->dev, fbuf, sc->input_mode_rlen,
	    HID_FEATURE_REPORT, sc->input_mode_rid);

	free(fbuf, M_TEMP);

	return (error);
}

DRIVER_MODULE(hmt, hidbus, hmt_driver, hmt_devclass, NULL, 0);
MODULE_DEPEND(hmt, hidbus, 1, 1, 1);
MODULE_DEPEND(hmt, hid, 1, 1, 1);
MODULE_DEPEND(hmt, evdev, 1, 1, 1);
MODULE_VERSION(hmt, 1);
