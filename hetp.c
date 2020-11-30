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

/*
 * Elan I2C Touchpad driver. Based on Linux driver.
 * https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/input/mouse/elan_i2c_core.c
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
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

#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iic.h>
#include <dev/iicbus/iiconf.h>

#include "hid.h"
#include "hidbus.h"
#include "hidquirk.h"

#define HID_DEBUG_VAR   hetp_debug
#include "hid_debug.h"

static SYSCTL_NODE(_hw_hid, OID_AUTO, hetp, CTLFLAG_RW, 0,
    "Elantech Touchpad");
#ifdef HID_DEBUG
static int hetp_debug = 1;
SYSCTL_INT(_hw_hid_hetp, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hetp_debug, 1, "Debug level");
#endif

#define	HETP_INPUT		0x0003
#define	HETP_PATTERN		0x0100
#define	HETP_UNIQUEID		0x0101
#define	HETP_FW_VERSION		0x0102
#define	HETP_IC_TYPE		0x0103
#define	HETP_OSM_VERSION	0x0103
#define	HETP_NSM_VERSION	0x0104
#define	HETP_TRACENUM		0x0105
#define	HETP_MAX_X_AXIS		0x0106
#define	HETP_MAX_Y_AXIS		0x0107
#define	HETP_RESOLUTION		0x0108
#define	HETP_PRESSURE		0x010A
#define	HETP_POWER		0x0307

#define	HETP_DESC_CMD		0x0001
#define	HETP_REPORT_DESC_CMD	0x0002
#define	HETP_COMMAND		0x0005
#define	HETP_CONTROL		0x0300

#define	HETP_CMD_WAKEUP		0x0800
#define	HETP_CMD_SLEEP		0x0801
#define	HETP_CMD_RESET		0x0100

#define	HETP_CTRL_ABSOLUTE	0x0001
#define	HETP_CTRL_STANDARD	0x0000

#define	HETP_DISABLE_POWER	0x0001

#define	HETP_REPORT_LEN_LO	32
#define	HETP_REPORT_LEN_HI	37
#define	HETP_DESC_LENGTH	30
#define	HETP_REPORT_DESC_LENGTH	158
#define	HETP_MAX_FINGERS	5

#define	HETP_REPORT_ID_LO	0x5D
#define	HETP_REPORT_ID_HI	0x60

#define	HETP_TOUCH_INFO		1
#define	HETP_FINGER_DATA	2
#define	HETP_FINGER_DATA_LEN	5
#define	HETP_HOVER_INFO		28
#define	HETP_WH_DATA		31

#define	HETP_TOUCH_LMB		(1 << 0)
#define	HETP_TOUCH_RMB		(1 << 1)
#define	HETP_TOUCH_MMB		(1 << 2)

#define	HETP_MAX_PRESSURE	255
#define	HETP_FWIDTH_REDUCE	90
#define	HETP_FINGER_MAX_WIDTH	15
#define	HETP_PRESSURE_BASE	25

struct hetp_softc {
	device_t		dev;

	struct evdev_dev	*evdev;
	bool			initialized;
	uint8_t			report_id;
	hid_size_t		report_len;

	uint16_t		product_id;
	uint16_t		ic_type;

	int32_t			pressure_base;
	uint16_t		max_x;
	uint16_t		max_y;
	uint16_t		trace_x;
	uint16_t		trace_y;
	uint16_t		res_x;		/* dots per mm */
	uint16_t		res_y;
	bool			hi_precission;
	bool			is_clickpad;
};

static evdev_open_t	hetp_ev_open;
static evdev_close_t	hetp_ev_close;
static hid_intr_t	hetp_intr;

static int		hetp_probe(struct hetp_softc *);
static int		hetp_attach(struct hetp_softc *);
static int		hetp_detach(struct hetp_softc *);
static int32_t		hetp_res2dpmm(uint8_t, bool);

static device_probe_t   hetp_iic_probe;
static device_attach_t  hetp_iic_attach;
static device_detach_t  hetp_iic_detach;
static device_resume_t	hetp_iic_resume;
static device_suspend_t	hetp_iic_suspend;

static int		hetp_iic_read_reg(device_t, uint16_t, size_t, void *);
static int		hetp_iic_write_reg(device_t, uint16_t, uint16_t);
static int		hetp_iic_set_absolute_mode(device_t, bool);
static int		hetp_iic_set_power(device_t, bool);
#if 0
static int		hetp_iic_set_power(struct hetp_softc *, bool);
#endif

#define	HETP_IIC_DEV(pnp) \
	{ HID_TLC(0xff00, 0x0001), HID_BUS(BUS_I2C), HID_PNP(pnp) }

static const struct hid_device_id hetp_iic_devs[] = {
	HETP_IIC_DEV("ELAN0000"),
	HETP_IIC_DEV("ELAN0100"),
	HETP_IIC_DEV("ELAN0600"),
	HETP_IIC_DEV("ELAN0601"),
	HETP_IIC_DEV("ELAN0602"),
	HETP_IIC_DEV("ELAN0603"),
	HETP_IIC_DEV("ELAN0604"),
	HETP_IIC_DEV("ELAN0605"),
	HETP_IIC_DEV("ELAN0606"),
	HETP_IIC_DEV("ELAN0607"),
	HETP_IIC_DEV("ELAN0608"),
	HETP_IIC_DEV("ELAN0609"),
	HETP_IIC_DEV("ELAN060B"),
	HETP_IIC_DEV("ELAN060C"),
	HETP_IIC_DEV("ELAN060F"),
	HETP_IIC_DEV("ELAN0610"),
	HETP_IIC_DEV("ELAN0611"),
	HETP_IIC_DEV("ELAN0612"),
	HETP_IIC_DEV("ELAN0615"),
	HETP_IIC_DEV("ELAN0616"),
	HETP_IIC_DEV("ELAN0617"),
	HETP_IIC_DEV("ELAN0618"),
	HETP_IIC_DEV("ELAN0619"),
	HETP_IIC_DEV("ELAN061A"),
	HETP_IIC_DEV("ELAN061B"),
	HETP_IIC_DEV("ELAN061C"),
	HETP_IIC_DEV("ELAN061D"),
	HETP_IIC_DEV("ELAN061E"),
	HETP_IIC_DEV("ELAN061F"),
	HETP_IIC_DEV("ELAN0620"),
	HETP_IIC_DEV("ELAN0621"),
	HETP_IIC_DEV("ELAN0622"),
	HETP_IIC_DEV("ELAN0623"),
	HETP_IIC_DEV("ELAN0624"),
	HETP_IIC_DEV("ELAN0625"),
	HETP_IIC_DEV("ELAN0626"),
	HETP_IIC_DEV("ELAN0627"),
	HETP_IIC_DEV("ELAN0628"),
	HETP_IIC_DEV("ELAN0629"),
	HETP_IIC_DEV("ELAN062A"),
	HETP_IIC_DEV("ELAN062B"),
	HETP_IIC_DEV("ELAN062C"),
	HETP_IIC_DEV("ELAN062D"),
	HETP_IIC_DEV("ELAN062E"),	/* Lenovo V340 Whiskey Lake U */
	HETP_IIC_DEV("ELAN062F"),	/* Lenovo V340 Comet Lake U */
	HETP_IIC_DEV("ELAN0631"),
	HETP_IIC_DEV("ELAN0632"),
	HETP_IIC_DEV("ELAN0633"),	/* Lenovo S145 */
	HETP_IIC_DEV("ELAN0634"),	/* Lenovo V340 Ice lake */
	HETP_IIC_DEV("ELAN0635"),	/* Lenovo V1415-IIL */
	HETP_IIC_DEV("ELAN0636"),	/* Lenovo V1415-Dali */
	HETP_IIC_DEV("ELAN0637"),	/* Lenovo V1415-IGLR */
	HETP_IIC_DEV("ELAN1000"),
};

static const struct evdev_methods hetp_evdev_methods = {
	.ev_open = &hetp_ev_open,
	.ev_close = &hetp_ev_close,
};

static int
hetp_ev_open(struct evdev_dev *evdev)
{
 	device_t dev = evdev_get_softc(evdev);

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	return (hidbus_intr_start(dev));
}

static int
hetp_ev_close(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	return (hidbus_intr_stop(dev));
}

static int
hetp_probe(struct hetp_softc *sc)
{
	if (hidbus_find_child(device_get_parent(sc->dev),
	    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHPAD)) != NULL) {
		DPRINTFN(5, "Ignore HID-compatible touchpad on %s\n",
		    device_get_nameunit(device_get_parent(sc->dev)));
		return (ENXIO);
	}

	device_set_desc(sc->dev, "Elan Touchpad");

	return (BUS_PROBE_DEFAULT);
}

static int
hetp_attach(struct hetp_softc *sc)
{
	const struct hid_device_info *hw = hid_get_device_info(sc->dev);
	int32_t minor, major;
	int error;

	sc->report_id = sc->hi_precission ?
	    HETP_REPORT_ID_HI : HETP_REPORT_ID_LO;
	sc->report_len = sc->hi_precission ?
	    HETP_REPORT_LEN_HI : HETP_REPORT_LEN_LO;

	sc->evdev = evdev_alloc();
	evdev_set_name(sc->evdev, device_get_desc(sc->dev));
	evdev_set_phys(sc->evdev, device_get_nameunit(sc->dev));
	evdev_set_id(sc->evdev, hw->idBus, hw->idVendor, hw->idProduct,
            hw->idVersion);
	evdev_set_serial(sc->evdev, hw->serial);
	evdev_set_methods(sc->evdev, sc->dev, &hetp_evdev_methods);
	evdev_set_flag(sc->evdev, EVDEV_FLAG_MT_STCOMPAT);

	evdev_support_event(sc->evdev, EV_SYN);
	evdev_support_event(sc->evdev, EV_ABS);
	evdev_support_event(sc->evdev, EV_KEY);
	evdev_support_prop(sc->evdev, INPUT_PROP_POINTER);
	evdev_support_key(sc->evdev, BTN_LEFT);
	if (sc->is_clickpad) {
		evdev_support_prop(sc->evdev, INPUT_PROP_BUTTONPAD);
	} else {
		evdev_support_key(sc->evdev, BTN_RIGHT);
#if 0
		/* Is there any way to detect middle button presence? */
		evdev_support_key(sc->evdev, BTN_MIDDLE);
#endif
	}

	major = HETP_FINGER_MAX_WIDTH * MAX(sc->trace_x, sc->trace_y);
	minor = HETP_FINGER_MAX_WIDTH * MIN(sc->trace_x, sc->trace_y);

	evdev_support_abs(sc->evdev, ABS_MT_SLOT,
	    0, 0, HETP_MAX_FINGERS - 1, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_TRACKING_ID,
	    0, -1, HETP_MAX_FINGERS - 1, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_POSITION_X,
	    0, 0, sc->max_x, 0, 0, sc->res_x);
	evdev_support_abs(sc->evdev, ABS_MT_POSITION_Y,
	    0, 0, sc->max_y, 0, 0, sc->res_y);
	evdev_support_abs(sc->evdev, ABS_MT_PRESSURE,
	    0, 0, HETP_MAX_PRESSURE, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_ORIENTATION, 0, 0, 1, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_TOUCH_MAJOR, 0, 0, major, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_TOUCH_MINOR, 0, 0, minor, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_DISTANCE, 0, 0, 1, 0, 0, 0);

	error = evdev_register_mtx(sc->evdev, hidbus_get_lock(sc->dev));
	if (error != 0) {
		hetp_detach(sc);
		return (ENOMEM);
	}

	sc->initialized = true;
	device_printf(sc->dev, "[%d:%d], %s\n", sc->max_x, sc->max_y,
	    sc->is_clickpad ? "clickpad" : "2 buttons");

	return (0);
}

static int
hetp_detach(struct hetp_softc *sc)
{
	evdev_free(sc->evdev);

	return (0);
}

static void
hetp_intr(void *context, void *buf, hid_size_t len)
{
	struct hetp_softc *sc = context;
	uint8_t *report, *fdata;
	int32_t finger;
	int32_t x, y, p, w, h, wh, ori, min, maj;

	/* we seem to get 0 length reports sometimes, ignore them */
	report = buf;
	if (*report != sc->report_id || len < sc->report_len ||
	    !sc->initialized)
		return;

	fdata = report + HETP_FINGER_DATA;
	for (finger = 0; finger < HETP_MAX_FINGERS; finger++) {
		if (report[HETP_TOUCH_INFO] & (1 << (finger + 3))) {
			if (sc->hi_precission) {
				x = fdata[0] << 8 | fdata[1];
				y = fdata[2] << 8 | fdata[3];
				wh = report[HETP_WH_DATA + finger];
			} else {
				x = (fdata[0] & 0xf0) << 4 | fdata[1];
				y = (fdata[0] & 0x0f) << 8 | fdata[2];
				wh = fdata[3];
			}
			p = fdata[4];

			fdata += HETP_FINGER_DATA_LEN;
			if (x > sc->max_x || y > sc->max_y) {
				DPRINTF("[%d] x=%d y=%d over max (%d, %d)",
				    finger, x, y, sc->max_x, sc->max_y);
				continue;
			}

			y = sc->max_y - y;
			/* Reduce trace size to not treat large finger as palm */
			w = (wh & 0x0F) * (sc->trace_x - HETP_FWIDTH_REDUCE);
			h = (wh >> 4) * (sc->trace_y - HETP_FWIDTH_REDUCE);
			ori = w > h ? 1 : 0;
			maj = MAX(w, h);
			min = MIN(w, h);
			p = MIN(p + sc->pressure_base, HETP_MAX_PRESSURE);

			evdev_push_abs(sc->evdev, ABS_MT_SLOT, finger);
			evdev_push_abs(sc->evdev, ABS_MT_TRACKING_ID, finger);
			evdev_push_abs(sc->evdev, ABS_MT_POSITION_X, x);
			evdev_push_abs(sc->evdev, ABS_MT_POSITION_Y, y);
			evdev_push_abs(sc->evdev, ABS_MT_PRESSURE, p);
			evdev_push_abs(sc->evdev, ABS_MT_ORIENTATION, ori);
			evdev_push_abs(sc->evdev, ABS_MT_TOUCH_MAJOR, maj);
			evdev_push_abs(sc->evdev, ABS_MT_TOUCH_MINOR, min);
		} else {
			evdev_push_abs(sc->evdev, ABS_MT_SLOT, finger);
			evdev_push_abs(sc->evdev, ABS_MT_TRACKING_ID, -1);
		}
	}

	evdev_push_key(sc->evdev, BTN_LEFT,
	    report[HETP_TOUCH_INFO] & HETP_TOUCH_LMB);
	evdev_push_key(sc->evdev, BTN_MIDDLE,
	    report[HETP_TOUCH_INFO] & HETP_TOUCH_MMB);
	evdev_push_key(sc->evdev, BTN_RIGHT,
	    report[HETP_TOUCH_INFO] & HETP_TOUCH_RMB);
	evdev_push_abs(sc->evdev, ABS_DISTANCE,
	    (report[HETP_HOVER_INFO] & 0x40) >> 6);

	evdev_sync(sc->evdev);
}

static int32_t
hetp_res2dpmm(uint8_t res, bool hi_precission)
{
	int32_t dpi;

	dpi = hi_precission ? 300 + res * 100 : 790 + res * 10;

	return (dpi * 10 /254);
}

static int
hetp_iic_probe(device_t dev)
{
	struct hetp_softc *sc = device_get_softc(dev);
	device_t iichid;
	int error;

	error = HIDBUS_LOOKUP_DRIVER_INFO(dev, hetp_iic_devs);
	if (error != 0)
		return (error);

	iichid = device_get_parent(device_get_parent(dev));
	if (device_get_devclass(iichid) != devclass_find("iichid"))
		return (ENXIO);

	sc->dev = dev;

	return (hetp_probe(sc));
}

static int
hetp_iic_attach(device_t dev)
{
	struct hetp_softc *sc = device_get_softc(dev);
	uint16_t buf, reg;
	uint8_t *buf8;
	uint8_t pattern;

	buf8 = (uint8_t *)&buf;

	hidbus_set_intr(dev, hetp_intr, sc);

	if (hetp_iic_read_reg(dev, HETP_UNIQUEID, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading product ID\n");
		return (EIO);
	}
	sc->product_id = le16toh(buf);

	if (hetp_iic_read_reg(dev, HETP_PATTERN, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading pattern\n");
		return (EIO);
	}
	pattern = buf == 0xFFFF ? 0 : buf8[1];
	sc->hi_precission = pattern >= 0x02;

	reg = pattern >= 0x01 ? HETP_IC_TYPE : HETP_OSM_VERSION;
	if (hetp_iic_read_reg(dev, reg, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading IC type\n");
		return (EIO);
	}
	sc->ic_type = pattern >= 0x01 ? be16toh(buf) : buf8[1];

	if (hetp_iic_read_reg(dev, HETP_NSM_VERSION, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading SM version\n");
		return (EIO);
	}
	sc->is_clickpad = (buf8[0] & 0x10) != 0;

	if (hetp_iic_set_absolute_mode(dev, true) != 0) {
		device_printf(sc->dev, "failed to reset\n");
		return (EIO);
	}

	if (hetp_iic_read_reg(dev, HETP_MAX_X_AXIS, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading max x\n");
		return (EIO);
	}
	sc->max_x = le16toh(buf);

	if (hetp_iic_read_reg(dev, HETP_MAX_Y_AXIS, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading max y\n");
		return (EIO);
	}
	sc->max_y = le16toh(buf);

	if (hetp_iic_read_reg(dev, HETP_TRACENUM, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading trace info\n");
		return (EIO);
	}
	sc->trace_x = sc->max_x / buf8[0];
	sc->trace_y = sc->max_y / buf8[1];

	if (hetp_iic_read_reg(dev, HETP_PRESSURE, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading pressure format");
		return (EIO);
	}
	sc->pressure_base = (buf8[0] & 0x10) ? 0 : HETP_PRESSURE_BASE;

	if (hetp_iic_read_reg(dev, HETP_RESOLUTION, sizeof(buf), &buf)  != 0) {
		device_printf(sc->dev, "failed reading resolution\n");
		return (EIO);
	}
	/* Conversion from internal format to dot per mm */
	sc->res_x = hetp_res2dpmm(buf8[0], sc->hi_precission);
	sc->res_y = hetp_res2dpmm(buf8[1], sc->hi_precission);

	return (hetp_attach(sc));
}

static int
hetp_iic_detach(device_t dev)
{
	struct hetp_softc *sc = device_get_softc(dev);

	if (hetp_iic_set_absolute_mode(dev, false) != 0)
		device_printf(dev, "failed setting standard mode\n");

	return (hetp_detach(sc));
}

static int
hetp_iic_resume(device_t dev)
{
#if 0
	if (hetp_iic_set_power(dev, true) != 0) {
		device_printf(dev, "power up when resuming failed\n");
		return (EIO);
	}
#endif
	if (hetp_iic_set_absolute_mode(dev, true) != 0) {
		device_printf(dev, "reset when resuming failed: \n");
		return (EIO);
	}

	return (0);
}

static int
hetp_iic_suspend(device_t dev)
{
#if 0
	if (hetp_iic_set_power(dev, false) != 0) {
		device_printf(dev, "power down on suspending failed\n");
		return (EIO);
	}
#endif
	return (0);
}

static int
hetp_iic_set_absolute_mode(device_t dev, bool enable)
{
	device_t iichid = device_get_parent(device_get_parent(dev));
	device_t iicbus = device_get_parent(iichid);
	struct hetp_softc *sc = device_get_softc(dev);
	static const struct {
		uint16_t	ic_type;
		uint16_t	product_id;
	} special_fw[] = {
	    { 0x0E, 0x05 }, { 0x0E, 0x06 }, { 0x0E, 0x07 }, { 0x0E, 0x09 },
	    { 0x0E, 0x13 }, { 0x08, 0x26 },
	};
#if 0
	static uint8_t dummy[HETP_REPORT_DESC_LENGTH];
#endif
	uint16_t buf, val;
	uint8_t *buf8;
	int i, error;
	bool require_wakeup;

	buf8 = (uint8_t *)&buf;

	error = iic2errno(iicbus_request_bus(iicbus, iichid, IIC_WAIT));
	if (error != 0)
		return (error);

	error = EIO;
#if 0
	if (hetp_write_reg(dev, HETP_COMMAND, HETP_CMD_RESET) != 0) {
		device_printf(dev, "failed writing reset command\n");
		goto release;
	}

	pause("hetp", (100 * hz + 900) / 1000);

	if (hetp_read_reg(dev, 0x0000, sizeof(buf), &buf) != 0) {
		device_printf(dev, "failed reading reset ack\n");
		goto release;
	}

	if (hetp_read_reg(dev, HETP_DESC_CMD, HETP_DESC_LENGTH, dummy) != 0) {
		device_printf(dev, "failed reading device descr\n");
		goto release;
	}

	if (hetp_read_reg(
	    dev, HETP_REPORT_DESC_CMD, HETP_REPORT_DESC_LENGTH, dummy) != 0) {
		device_printf(dev, "failed reading report descr\n");
		goto release;
	}
#endif
	/*
	 * Some ASUS touchpads need to be powered on to enter absolute mode.
	 */
	require_wakeup = false;
	for (i = 0; i < nitems(special_fw); i++) {
		if (sc->ic_type == special_fw[i].ic_type &&
		    sc->product_id == special_fw[i].product_id) {
			require_wakeup = true;
			break;
		}
	}

	if (require_wakeup && hetp_iic_set_power(dev, true) != 0) {
		device_printf(dev, "failed writing poweron command\n");
		goto release;
	}

	val = enable ? HETP_CTRL_ABSOLUTE : HETP_CTRL_STANDARD;
	if (hetp_iic_write_reg(dev, HETP_CONTROL, val) != 0) {
		device_printf(dev, "failed setting absolute mode\n");
		goto release;
	}

	if (require_wakeup && hetp_iic_set_power(dev, false) != 0)
		device_printf(dev, "failed writing poweroff command\n");
	else
		error = 0;
release:
	iicbus_release_bus(iicbus, iichid);
	return (error);
}

static int
hetp_iic_set_power(device_t dev, bool enable)
{
	return (hetp_iic_write_reg(dev, HETP_COMMAND,
	    enable ? HETP_CMD_WAKEUP : HETP_CMD_SLEEP));
}

#if 0
static int
hetp_iic_set_power(device_t dev, bool enable)
{
	uint16_t buf, reg;

	if (hetp_iic_read_reg(dev, HETP_POWER, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading power state\n");
		return (EIO);
	}

	reg = le16toh(buf);
	if (enable)
		reg &= ~HETP_DISABLE_POWER;
	else
		reg |= HETP_DISABLE_POWER;

	if (hetp_iic_write_reg(dev, HETP_POWER, reg) != 0) {
		device_printf(dev, "failed setting power state\n");
		return (EIO);
	}

	return (0);
}
#endif

static int
hetp_iic_read_reg(device_t dev, uint16_t reg, size_t len, void *val)
{
	device_t iichid = device_get_parent(device_get_parent(dev));
	uint16_t addr = iicbus_get_addr(iichid) << 1;
	uint8_t cmd[2] = { reg & 0xff, (reg >> 8) & 0xff };
	struct iic_msg msgs[2] = {
	    { addr, IIC_M_WR | IIC_M_NOSTOP,  sizeof(cmd), cmd },
	    { addr, IIC_M_RD, len, val },
	};
	int error;

	DPRINTF("Read reg 0x%04x with size %zu\n", reg, len);

	error = iicbus_transfer(iichid, msgs, nitems(msgs));
	if (error != 0)
		return (iic2errno(error));

	DPRINTF("Response: %*D\n", (int)len, val, " ");

	return (0);
}

static int
hetp_iic_write_reg(device_t dev, uint16_t reg, uint16_t val)
{
	device_t iichid = device_get_parent(device_get_parent(dev));
	uint16_t addr = iicbus_get_addr(iichid) << 1;
	uint8_t cmd[4] = { reg & 0xff, (reg >> 8) & 0xff,
			   val & 0xff, (val >> 8) & 0xff };
	struct iic_msg msgs[1] = {
	    { addr, IIC_M_WR, sizeof(cmd), cmd },
	};

	DPRINTF("Write reg 0x%04x with value 0x%04x\n", reg, val);

	return (iic2errno(iicbus_transfer(iichid, msgs, nitems(msgs))));
}

static devclass_t hetp_devclass;
static device_method_t hetp_iic_methods[] = {
	DEVMETHOD(device_probe,		hetp_iic_probe),
	DEVMETHOD(device_attach,	hetp_iic_attach),
	DEVMETHOD(device_detach,	hetp_iic_detach),
	DEVMETHOD(device_resume,	hetp_iic_resume),
	DEVMETHOD(device_suspend,	hetp_iic_suspend),
	DEVMETHOD_END
};

static driver_t hetp_iic_driver = {
	.name = "hetp",
	.methods = hetp_iic_methods,
	.size = sizeof(struct hetp_softc),
};

DRIVER_MODULE(hetp_iic, hidbus, hetp_iic_driver, hetp_devclass, NULL, 0);
MODULE_DEPEND(hetp_iic, hidbus, 1, 1, 1);
MODULE_DEPEND(hetp_iic, hid, 1, 1, 1);
MODULE_DEPEND(hetp_iic, iicbus, IICBUS_MINVER, IICBUS_PREFVER, IICBUS_MAXVER);
MODULE_DEPEND(hetp_iic, evdev, 1, 1, 1);
MODULE_VERSION(hetp_iic, 1);
HID_PNP_INFO(hetp_iic_devs);
