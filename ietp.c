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

#define HID_DEBUG_VAR   ietp_debug
#include "hid_debug.h"

static SYSCTL_NODE(_hw_hid, OID_AUTO, ietp, CTLFLAG_RW, 0,
    "Elantech Touchpad");
#ifdef HID_DEBUG
static int ietp_debug = 1;
SYSCTL_INT(_hw_hid_ietp, OID_AUTO, debug, CTLFLAG_RWTUN,
    &ietp_debug, 1, "Debug level");
#endif

#define	IETP_INPUT		0x0003
#define	IETP_PATTERN		0x0100
#define	IETP_UNIQUEID		0x0101
#define	IETP_FW_VERSION		0x0102
#define	IETP_IC_TYPE		0x0103
#define	IETP_OSM_VERSION	0x0103
#define	IETP_NSM_VERSION	0x0104
#define	IETP_TRACENUM		0x0105
#define	IETP_MAX_X_AXIS		0x0106
#define	IETP_MAX_Y_AXIS		0x0107
#define	IETP_RESOLUTION		0x0108
#define	IETP_PRESSURE		0x010A
#define	IETP_POWER		0x0307

#define	IETP_DESC_CMD		0x0001
#define	IETP_REPORT_DESC_CMD	0x0002
#define	IETP_COMMAND		0x0005
#define	IETP_CONTROL		0x0300

#define	IETP_CMD_WAKEUP		0x0800
#define	IETP_CMD_SLEEP		0x0801
#define	IETP_CMD_RESET		0x0100

#define IETP_CTRL_ABSOLUTE	0x0001
#define IETP_CTRL_STANDARD	0x0000

#define	IETP_DISABLE_POWER	0x0001

#define	IETP_REP_LEN_LO		32
#define	IETP_REP_LEN_HI		37
#define	IETP_DESC_LENGTH	30
#define	IETP_REPORT_DESC_LENGTH	158
#define	IETP_MAX_FINGERS	5

#define	IETP_REP_ID_LO		0x5D
#define	IETP_REP_ID_HI		0x60

#define	IETP_TOUCH_INFO		1
#define	IETP_FINGER_DATA	2
#define	IETP_FINGER_DATA_LEN	5
#define	IETP_HOVER_INFO		28
#define	IETP_WH_DATA		31

#define	IETP_TOUCH_LMB		(1 << 0)
#define	IETP_TOUCH_RMB		(1 << 1)
#define	IETP_TOUCH_MMB		(1 << 2)

#define	IETP_MAX_PRESSURE	255
#define	IETP_FWIDTH_REDUCE	90
#define IETP_FINGER_MAX_WIDTH	15
#define	IETP_PRESSURE_BASE	25

struct ietp_softc {
	device_t		dev;

	device_t		iichid;
	uint16_t		addr;

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
	uint16_t		res_x;		/* DPI */
	uint16_t		res_y;
	bool			hi_precission;
	bool			is_clickpad;
};

static device_probe_t   ietp_probe;
static device_attach_t  ietp_attach;
static device_detach_t  ietp_detach;
static device_resume_t	ietp_resume;
static device_suspend_t	ietp_suspend;

static evdev_open_t	ietp_ev_open;
static evdev_close_t	ietp_ev_close;

static hid_intr_t	ietp_intr;

static int	ietp_read_reg(struct ietp_softc *, uint16_t, size_t, void *);
static int	ietp_write_reg(struct ietp_softc *, uint16_t, uint16_t);
static int	ietp_init(struct ietp_softc *);
static int	ietp_set_absolute_mode(struct ietp_softc *, bool);
static int	ietp_i2chid_set_power(struct ietp_softc *, bool);
#if 0
static int	ietp_set_power(struct ietp_softc *, bool);*/
#endif

#define	IETP_DEV(pnp)	{HID_TLC(0xff00, 0x01), HID_BUS(BUS_I2C), HID_PNP(pnp)}
static const struct hid_device_id ietp_devs[] = {
	IETP_DEV("ELAN0000"),
	IETP_DEV("ELAN0100"),
	IETP_DEV("ELAN0600"),
	IETP_DEV("ELAN0601"),
	IETP_DEV("ELAN0602"),
	IETP_DEV("ELAN0603"),
	IETP_DEV("ELAN0604"),
	IETP_DEV("ELAN0605"),
	IETP_DEV("ELAN0606"),
	IETP_DEV("ELAN0607"),
	IETP_DEV("ELAN0608"),
	IETP_DEV("ELAN0609"),
	IETP_DEV("ELAN060B"),
	IETP_DEV("ELAN060C"),
	IETP_DEV("ELAN060F"),
	IETP_DEV("ELAN0610"),
	IETP_DEV("ELAN0611"),
	IETP_DEV("ELAN0612"),
	IETP_DEV("ELAN0615"),
	IETP_DEV("ELAN0616"),
	IETP_DEV("ELAN0617"),
	IETP_DEV("ELAN0618"),
	IETP_DEV("ELAN0619"),
	IETP_DEV("ELAN061A"),
	IETP_DEV("ELAN061B"),
	IETP_DEV("ELAN061C"),
	IETP_DEV("ELAN061D"),
	IETP_DEV("ELAN061E"),
	IETP_DEV("ELAN061F"),
	IETP_DEV("ELAN0620"),
	IETP_DEV("ELAN0621"),
	IETP_DEV("ELAN0622"),
	IETP_DEV("ELAN0623"),
	IETP_DEV("ELAN0624"),
	IETP_DEV("ELAN0625"),
	IETP_DEV("ELAN0626"),
	IETP_DEV("ELAN0627"),
	IETP_DEV("ELAN0628"),
	IETP_DEV("ELAN0629"),
	IETP_DEV("ELAN062A"),
	IETP_DEV("ELAN062B"),
	IETP_DEV("ELAN062C"),
	IETP_DEV("ELAN062D"),
	IETP_DEV("ELAN062E"),	/* Lenovo V340 Whiskey Lake U */
	IETP_DEV("ELAN062F"),	/* Lenovo V340 Comet Lake U */
	IETP_DEV("ELAN0631"),
	IETP_DEV("ELAN0632"),
	IETP_DEV("ELAN0633"),	/* Lenovo S145 */
	IETP_DEV("ELAN0634"),	/* Lenovo V340 Ice lake */
	IETP_DEV("ELAN0635"),	/* Lenovo V1415-IIL */
	IETP_DEV("ELAN0636"),	/* Lenovo V1415-Dali */
	IETP_DEV("ELAN0637"),	/* Lenovo V1415-IGLR */
	IETP_DEV("ELAN1000"),
};

static const struct evdev_methods ietp_evdev_methods = {
	.ev_open = &ietp_ev_open,
	.ev_close = &ietp_ev_close,
};

static int
ietp_ev_open(struct evdev_dev *evdev)
{
 	device_t dev = evdev_get_softc(evdev);

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	return (hidbus_intr_start(dev));
}

static int
ietp_ev_close(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	return (hidbus_intr_stop(dev));
}

static int
ietp_probe(device_t dev)
{
	device_t iichid;
	void *d_ptr;
	hid_size_t d_len;
	int error;

	error = HIDBUS_LOOKUP_DRIVER_INFO(dev, ietp_devs);
	if (error != 0)
		return (error);

	iichid = device_get_parent(device_get_parent(dev));
	if (device_get_devclass(iichid) != devclass_find("iichid"))
		return (ENXIO);

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error != 0) {
		device_printf(dev, "could not retrieve report descriptor from "
		    "device: %d\n", error);
		return (ENXIO);
	}
	if (hid_is_collection(d_ptr, d_len,
	    HID_USAGE2(HUP_DIGITIZERS, HUD_TOUCHPAD))) {
		DPRINTFN(5, "Ignore HID-compatible touchpad on %s\n",
		    device_get_nameunit(device_get_parent(dev)));
		return (ENXIO);
	}

	device_set_desc(dev, "Elan I2C Touchpad");

	return (BUS_PROBE_DEFAULT);
}

static int
ietp_attach(device_t dev)
{
	struct ietp_softc *sc = device_get_softc(dev);
	const struct hid_device_info *hw = hid_get_device_info(dev);
	int32_t minor, major;
	int error;

	sc->dev = dev;
	sc->iichid = device_get_parent(device_get_parent(dev));
	sc->addr = iicbus_get_addr(sc->iichid) << 1;

	hidbus_set_intr(dev, ietp_intr, sc);

	if (ietp_init(sc) != 0)
		return(ENXIO);

	sc->evdev = evdev_alloc();
	evdev_set_name(sc->evdev, device_get_desc(dev));
	evdev_set_phys(sc->evdev, device_get_nameunit(dev));
	evdev_set_id(sc->evdev, hw->idBus, hw->idVendor, hw->idProduct,
            hw->idVersion);
	evdev_set_serial(sc->evdev, hw->serial);
	evdev_set_methods(sc->evdev, dev, &ietp_evdev_methods);
	evdev_set_flag(sc->evdev, EVDEV_FLAG_MT_STCOMPAT);

	evdev_support_event(sc->evdev, EV_SYN);
	evdev_support_event(sc->evdev, EV_ABS);
	evdev_support_prop(sc->evdev, INPUT_PROP_POINTER);
	evdev_support_key(sc->evdev, BTN_LEFT);
	if (sc->is_clickpad) {
		evdev_support_prop(sc->evdev, INPUT_PROP_BUTTONPAD);
	} else {
		evdev_support_key(sc->evdev, BTN_RIGHT);
#if 0
		/* A there any way to detect middle button presence? */
		evdev_support_key(sc->evdev, BTN_MIDDLE);
#endif
	}

	major = IETP_FINGER_MAX_WIDTH * MAX(sc->trace_x, sc->trace_y);
	minor = IETP_FINGER_MAX_WIDTH * MIN(sc->trace_x, sc->trace_y);

	evdev_support_abs(sc->evdev, ABS_MT_SLOT,
	    0, 0, IETP_MAX_FINGERS - 1, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_TRACKING_ID,
	    0, -1, IETP_MAX_FINGERS - 1, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_POSITION_X,
	    0, 0, sc->max_x, 0, 0, sc->res_x * 10 / 254);
	evdev_support_abs(sc->evdev, ABS_MT_POSITION_Y,
	    0, 0, sc->max_y, 0, 0, sc->res_y * 10 / 254);
	evdev_support_abs(sc->evdev, ABS_MT_PRESSURE,
	    0, 0, IETP_MAX_PRESSURE, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_ORIENTATION, 0, 0, 1, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_TOUCH_MAJOR, 0, 0, major, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_MT_TOUCH_MINOR, 0, 0, minor, 0, 0, 0);
	evdev_support_abs(sc->evdev, ABS_DISTANCE, 0, 0, 1, 0, 0, 0);

	error = evdev_register_mtx(sc->evdev, hidbus_get_lock(dev));
	if (error != 0) {
		ietp_detach(dev);
		return (ENOMEM);
	}

	sc->initialized = true;
	device_printf(dev, "[%d:%d], %s\n", sc->max_x, sc->max_y,
	    sc->is_clickpad ? "clickpad" : "2 buttons");

	return (0);
}

static int
ietp_detach(device_t dev)
{
	struct ietp_softc *sc = device_get_softc(dev);

	evdev_free(sc->evdev);

	return (0);
}

static int
ietp_resume(device_t dev)
{
	struct ietp_softc *sc = device_get_softc(dev);

#if 0
	if (ietp_set_power(sc, true) != 0) {
		device_printf(sc->dev, "power up when resuming failed\n");
		return (EIO);
	}
#endif
	if (ietp_set_absolute_mode(sc, true) != 0) {
		device_printf(sc->dev, "reset when resuming failed: \n");
		return (EIO);
	}

	return (0);
}

static int
ietp_suspend(device_t dev)
{
#if 0
	struct ietp_softc *sc = device_get_softc(dev);

	if (ietp_set_power(sc, false) != 0) {
		device_printf(sc->dev, "power down on suspending failed\n");
		return (EIO);
	}
#endif
	return (0);
}

static int
ietp_set_absolute_mode(struct ietp_softc *sc, bool enable)
{
	static const struct {
		uint16_t	ic_type;
		uint16_t	product_id;
	} special_fw[] = {
	    { 0x0E, 0x05 }, { 0x0E, 0x06 }, { 0x0E, 0x07 }, { 0x0E, 0x09 },
	    { 0x0E, 0x13 }, { 0x08, 0x26 },
	};
#if 0
	static uint8_t dummy[IETP_REPORT_DESC_LENGTH];
#endif
	device_t iicbus;
	uint16_t buf, val;
	uint8_t *buf8;
	int i, error;
	bool require_wakeup;

	buf8 = (uint8_t *)&buf;

	iicbus = device_get_parent(sc->iichid);
	error = iic2errno(iicbus_request_bus(iicbus, sc->iichid, IIC_WAIT));
	if (error != 0)
		return (error);

	error = EIO;
#if 0
	if (ietp_write_reg(sc, IETP_COMMAND, IETP_CMD_RESET) != 0) {
		device_printf(sc->dev, "failed writing reset command\n");
		goto release;
	}

	pause("ietp", (100 * hz + 900) / 1000);

	if (ietp_read_reg(sc, 0x0000, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading reset ack\n");
		goto release;
	}

	if (ietp_read_reg(sc, IETP_DESC_CMD, IETP_DESC_LENGTH, dummy) != 0) {
		device_printf(sc->dev, "failed reading device descr\n");
		goto release;
	}

	if (ietp_read_reg(
	    sc, IETP_REPORT_DESC_CMD, IETP_REPORT_DESC_LENGTH, dummy) != 0) {
		device_printf(sc->dev, "failed reading report descr\n");
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

	if (require_wakeup && ietp_i2chid_set_power(sc, true) != 0) {
		device_printf(sc->dev, "failed writing poweron command\n");
		goto release;
	}

	val = enable ? IETP_CTRL_ABSOLUTE : IETP_CTRL_STANDARD;
	if (ietp_write_reg(sc, IETP_CONTROL, val) != 0) {
		device_printf(sc->dev, "failed setting absolute mode\n");
		goto release;
	}

	if (require_wakeup && ietp_i2chid_set_power(sc, false) != 0)
		device_printf(sc->dev, "failed writing poweroff command\n");
	else
		error = 0;
release:
	iicbus_release_bus(iicbus, sc->iichid);
	return (error);
}

static int
ietp_init(struct ietp_softc *sc)
{
	uint16_t buf, reg;
	uint8_t *buf8;
	uint8_t pattern;

	buf8 = (uint8_t *)&buf;

	if (ietp_read_reg(sc, IETP_UNIQUEID,  sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading product ID\n");
		return (EIO);
	}
	sc->product_id = le16toh(buf);

	if (ietp_read_reg(sc, IETP_PATTERN, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading pattern\n");
		return (EIO);
	}
	pattern = buf == 0xFFFF ? 0 : buf8[1];
	sc->report_id = pattern < 0x02 ? IETP_REP_ID_LO : IETP_REP_ID_HI;
	sc->report_len = pattern < 0x02 ? IETP_REP_LEN_LO : IETP_REP_LEN_HI;
	sc->hi_precission = pattern >= 0x02;

	reg = pattern >= 0x01 ? IETP_IC_TYPE : IETP_OSM_VERSION;
	if (ietp_read_reg(sc, reg, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading IC type\n");
		return (EIO);
	}
	sc->ic_type = pattern >= 0x01 ? be16toh(buf) : buf8[1];

	if (ietp_read_reg(sc, IETP_NSM_VERSION, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading SM version\n");
		return (EIO);
	}
	sc->is_clickpad = (buf8[0] & 0x10) != 0;

	if (ietp_set_absolute_mode(sc, true) != 0) {
		device_printf(sc->dev, "failed to reset\n");
		return (EIO);
	}

	if (ietp_read_reg(sc, IETP_MAX_X_AXIS, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading max x\n");
		return (EIO);
	}
	sc->max_x = le16toh(buf) & 0xFFF;

	if (ietp_read_reg(sc, IETP_MAX_Y_AXIS, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading max y\n");
		return (EIO);
	}
	sc->max_y = le16toh(buf) & 0xFFF;

	if (ietp_read_reg(sc, IETP_TRACENUM, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading trace info\n");
		return (EIO);
	}
	sc->trace_x = sc->max_x / buf8[0];
	sc->trace_y = sc->max_y / buf8[1];

	if (ietp_read_reg(sc, IETP_PRESSURE, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading pressure format");
		return (EIO);
	}
	sc->pressure_base = (buf8[0] & 0x10) ? 0 : IETP_PRESSURE_BASE;

	if (ietp_read_reg(sc, IETP_RESOLUTION, sizeof(buf), &buf)  != 0) {
		device_printf(sc->dev, "failed reading resolution\n");
		return (EIO);
	}
	/* Conversion from internal format to DPI */
	sc->res_x = pattern < 0x02 ? 790 + buf8[0] * 10 : 300 + buf8[0] * 100;
	sc->res_y = pattern < 0x02 ? 790 + buf8[1] * 10 : 300 + buf8[1] * 100;

	return (0);
}

static int
ietp_i2chid_set_power(struct ietp_softc *sc, bool enable)
{
	return (ietp_write_reg(sc, IETP_COMMAND,
	    enable ? IETP_CMD_WAKEUP : IETP_CMD_SLEEP));
}

#if 0
static int
ietp_set_power(struct ietp_softc *sc, bool enable)
{
	uint16_t buf, reg;

	if (ietp_read_reg(sc, IETP_POWER, sizeof(buf), &buf) != 0) {
		device_printf(sc->dev, "failed reading power state\n");
		return (EIO);
	}

	reg = le16toh(buf);
	if (enable)
		reg &= ~IETP_DISABLE_POWER;
	else
		reg |= IETP_DISABLE_POWER;

	if (ietp_write_reg(sc, IETP_POWER, reg) != 0) {
		device_printf(sc->dev, "failed setting power state\n");
		return (EIO);
	}

	return (0);
}
#endif

static int
ietp_read_reg(struct ietp_softc *sc, uint16_t reg, size_t len, void *val)
{
	uint8_t cmd[2] = { reg & 0xff, (reg >> 8) & 0xff };
	struct iic_msg msgs[2] = {
	    { sc->addr, IIC_M_WR | IIC_M_NOSTOP,  sizeof(cmd), cmd },
	    { sc->addr, IIC_M_RD, len, val },
	};
	int error;

	DPRINTF("Read reg 0x%04x with size %zu\n", reg, len);

	error = iicbus_transfer(sc->iichid, msgs, nitems(msgs));
	if (error != 0)
		return (iic2errno(error));

	DPRINTF("Response: %*D\n", (int)len, val, " ");

	return (0);
}

static int
ietp_write_reg(struct ietp_softc *sc, uint16_t reg, uint16_t val)
{
	uint8_t cmd[4] = { reg & 0xff, (reg >> 8) & 0xff,
			   val & 0xff, (val >> 8) & 0xff };
	struct iic_msg msgs[1] = {
	    { sc->addr, IIC_M_WR, sizeof(cmd), cmd },
	};

	DPRINTF("Write reg 0x%04x with value 0x%04x\n", reg, val);

	return (iic2errno(iicbus_transfer(sc->iichid, msgs, nitems(msgs))));
}

static void
ietp_intr(void *context, void *buf, hid_size_t len)
{
	struct ietp_softc *sc = context;
	uint8_t *report, *fdata;
	int32_t finger;
	int32_t x, y, p, w, h, wh, ori, min, maj;

	/* we seem to get 0 length reports sometimes, ignore them */
	report = buf;
	if (*report != sc->report_id || len < sc->report_len ||
	    !sc->initialized)
		return;

	fdata = report + IETP_FINGER_DATA;
	for (finger = 0; finger < IETP_MAX_FINGERS; finger++) {
		if (report[IETP_TOUCH_INFO] & (1 << (finger + 3))) {
			if (sc->hi_precission) {
				x = fdata[0] << 8 | fdata[1];
				y = fdata[2] << 8 | fdata[3];
				wh = report[IETP_WH_DATA + finger];
			} else {
				x = ((fdata[0] & 0xf0) << 4) | fdata[1];
				y = ((fdata[0] & 0x0f) << 8) | fdata[2];
				wh = fdata[3];
			}
			p = fdata[4];

			fdata += IETP_FINGER_DATA_LEN;
			if (x > sc->max_x || y > sc->max_y) {
				DPRINTF("[%d] x=%d y=%d over max (%d, %d)",
				    finger, x, y, sc->max_x, sc->max_y);
				continue;
			}

			y = sc->max_y - y;
			/* Reduce trace size to not treat large finger as palm */
			w = (wh & 0x0F)*(sc->trace_x - IETP_FWIDTH_REDUCE);
			h = (wh >> 4)*(sc->trace_y - IETP_FWIDTH_REDUCE);
			ori = w > h ? 1 : 0;
			maj = MAX(w, h);
			min = MIN(w, h);
			p = MIN(p + sc->pressure_base, IETP_MAX_PRESSURE);

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
	    report[IETP_TOUCH_INFO] & IETP_TOUCH_LMB);
	evdev_push_key(sc->evdev, BTN_MIDDLE,
	    report[IETP_TOUCH_INFO] & IETP_TOUCH_MMB);
	evdev_push_key(sc->evdev, BTN_RIGHT,
	    report[IETP_TOUCH_INFO] & IETP_TOUCH_RMB);
	evdev_push_abs(sc->evdev, ABS_DISTANCE,
	    (report[IETP_HOVER_INFO] & 0x40) >> 6);

	evdev_sync(sc->evdev);
}

static devclass_t ietp_devclass;
static device_method_t ietp_methods[] = {
	DEVMETHOD(device_probe,		ietp_probe),
	DEVMETHOD(device_attach,	ietp_attach),
	DEVMETHOD(device_detach,	ietp_detach),
	DEVMETHOD(device_resume,	ietp_resume),
	DEVMETHOD(device_suspend,	ietp_suspend),
	DEVMETHOD_END
};

static driver_t ietp_driver = {
	.name = "ietp",
	.methods = ietp_methods,
	.size = sizeof(struct ietp_softc),
};

DRIVER_MODULE(ietp, hidbus, ietp_driver, ietp_devclass, NULL, 0);
MODULE_DEPEND(ietp, hidbus, 1, 1, 1);
MODULE_DEPEND(ietp, hid, 1, 1, 1);
MODULE_DEPEND(ietp, iicbus, IICBUS_MINVER, IICBUS_PREFVER, IICBUS_MAXVER);
MODULE_DEPEND(ietp, evdev, 1, 1, 1);
MODULE_VERSION(ietp, 1);
HID_PNP_INFO(ietp_devs);
