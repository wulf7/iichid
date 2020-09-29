/*-
 * Copyright (c) 2018-2019 Marc Priggemeyer <marc.priggemeyer@gmail.com>
 * Copyright (c) 2019-2020 Vladimir Kondratyev <wulf@FreeBSD.org>
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

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/sx.h>
#include <sys/taskqueue.h>

#include <machine/resource.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>
#include <dev/acpica/acpivar.h>

#include <dev/evdev/input.h>

#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iic.h>
#include <dev/iicbus/iiconf.h>

#include "hid.h"
#include "hidbus.h"
#include "hid_if.h"
#include "hidquirk.h"

#include "iichid.h"

#ifdef IICHID_DEBUG
static int iichid_debug = 1;

static SYSCTL_NODE(_hw, OID_AUTO, iichid, CTLFLAG_RW, 0, "I2C HID");
SYSCTL_INT(_hw_iichid, OID_AUTO, debug, CTLFLAG_RWTUN,
    &iichid_debug, 1, "Debug level");

#define	DPRINTFN(sc, n, ...) do {			\
	if (iichid_debug >= (n))			\
		device_printf((sc)->dev, __VA_ARGS__);	\
} while (0)
#define	DPRINTF(sc, ...)	DPRINTFN((sc), 1, __VA_ARGS__)
#else
#define	DPRINTFN(...)
#define	DPRINTF(...)
#endif

#define	IICHID_SAMPLING

typedef	hid_size_t	iichid_size_t;
#define	IICHID_SIZE_MAX	(UINT16_MAX - 2)

static char *iichid_ids[] = {
	"PNP0C50",
	"ACPI0C50",
	NULL
};

enum iichid_powerstate_how {
	IICHID_PS_NOCHANGE,
	IICHID_PS_SUSPEND,
	IICHID_PS_RESUME,
};

struct iichid_softc {
	device_t		dev;

	bool			probe_done;
	int			probe_result;

	struct hid_device_info	hw;
	uint16_t		addr;	/* Shifted left by 1 */
	uint16_t		config_reg;
	struct i2c_hid_desc	desc;

	hid_intr_t		*intr_handler;
	void			*intr_ctx;
	struct mtx		*intr_mtx;
	uint8_t			*intr_buf;
	iichid_size_t		intr_bufsize;

	int			irq_rid;
	struct resource		*irq_res;
	void			*irq_cookie;

#ifdef IICHID_SAMPLING
	int			sampling_rate_slow;
	int			sampling_rate_fast;
	int			sampling_hysteresis;
	int			missing_samples;
	struct timeout_task	periodic_task;
	bool			callout_setup;
#endif

	struct taskqueue	*taskqueue;
	struct task		event_task;
	struct task		power_task;

	bool			open;		/* intr_mtx */
	bool			suspend;	/* iicbus lock */
	bool			power_on;	/* iicbus lock */
};

#ifdef IICHID_SAMPLING
static int	iichid_setup_callout(struct iichid_softc *);
static int	iichid_reset_callout(struct iichid_softc *);
static void	iichid_teardown_callout(struct iichid_softc *);
#endif

static __inline bool
acpi_is_iichid(ACPI_HANDLE handle)
{
	char	**ids;
	UINT32	sta;

	for (ids = iichid_ids; *ids != NULL; ids++) {
		if (acpi_MatchHid(handle, *ids))
			break;
	}
	if (*ids == NULL)
                return (false);

	/*
	 * If no _STA method or if it failed, then assume that
	 * the device is present.
	 */
	if (ACPI_FAILURE(acpi_GetInteger(handle, "_STA", &sta)) ||
	    ACPI_DEVICE_PRESENT(sta))
		return (true);

	return (false);
}

static ACPI_STATUS
iichid_get_config_reg(ACPI_HANDLE handle, uint16_t *config_reg)
{
	ACPI_OBJECT *result;
	ACPI_BUFFER acpi_buf;
	ACPI_STATUS status;

	/*
	 * function (_DSM) to be evaluated to retrieve the address of
	 * the configuration register of the HID device
	 */
	/* 3cdff6f7-4267-4555-ad05-b30a3d8938de */
	static uint8_t dsm_guid[ACPI_UUID_LENGTH] = {
		0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
		0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE,
	};

#ifdef HAVE_ACPI_EVALUATEDSMTYPED
	status = acpi_EvaluateDSMTyped(handle, dsm_guid, 1, 1, NULL, &acpi_buf,
	    ACPI_TYPE_INTEGER);
	if (ACPI_FAILURE(status)) {
		printf("%s: error evaluating _DSM\n", __func__);
		return (status);
	}
	result = (ACPI_OBJECT *) acpi_buf.Pointer;
	*config_reg = result->Integer.Value & 0xFFFF;
#else
	status = acpi_EvaluateDSM(handle, dsm_guid, 1, 1, NULL, &acpi_buf);
	if (ACPI_FAILURE(status)) {
		printf("%s: error evaluating _DSM\n", __func__);
		return (status);
	}

	/* the result will contain the register address (int type) */
	result = (ACPI_OBJECT *) acpi_buf.Pointer;
	if (result->Type != ACPI_TYPE_INTEGER) {
		printf("%s: _DSM should return descriptor register address "
		    "as integer\n", __func__);
		status = AE_TYPE;
	} else {
		*config_reg = result->Integer.Value & 0xFFFF;
		status = AE_OK;
	}
#endif

	AcpiOsFree(result);
	return (status);
}

#ifndef HAVE_ACPI_IICBUS
static ACPI_STATUS
iichid_addr_cb(ACPI_RESOURCE *res, void *context)
{
	uint16_t *device_addr = context;

	if (res->Type == ACPI_RESOURCE_TYPE_SERIAL_BUS &&
	    res->Data.CommonSerialBus.Type == ACPI_RESOURCE_SERIAL_TYPE_I2C) {
		*device_addr = le16toh(res->Data.I2cSerialBus.SlaveAddress);
		return (AE_CTRL_TERMINATE);
	}

	return (AE_OK);
}

static uint16_t
acpi_get_iichid_addr(ACPI_HANDLE handle)
{
	ACPI_STATUS status;
	uint16_t addr = 0;

	/* _CRS holds device addr and needs a callback to evaluate */
	status = AcpiWalkResources(handle, "_CRS", iichid_addr_cb, &addr);
	if (ACPI_FAILURE(status))
		return (0);

	return (addr);
}

static ACPI_STATUS
iichid_get_handle_cb(ACPI_HANDLE handle, UINT32 level, void *context,
    void **retval)
{
	ACPI_HANDLE *dev_handle = context;
	uint16_t addr = (uintptr_t) *dev_handle;

	if (acpi_is_iichid(handle) && acpi_get_iichid_addr(handle) == addr) {

		*dev_handle = handle;
		return(AE_CTRL_TERMINATE);
	}

	return (AE_OK);
}

static ACPI_HANDLE
iichid_get_handle(device_t dev)
{
	ACPI_HANDLE ctrl_handle, dev_handle;
	ACPI_STATUS status;
	device_t iicbus = device_get_parent(dev);
	dev_handle = (void *)(uintptr_t) iicbus_get_addr(dev);

	ctrl_handle = acpi_get_handle(device_get_parent(iicbus));
	status = AcpiWalkNamespace(ACPI_TYPE_DEVICE, ctrl_handle,
	    1, iichid_get_handle_cb, NULL, &dev_handle, NULL);

	if (ACPI_FAILURE(status))
		return (NULL);

	if (dev_handle == (void *)(uintptr_t)iicbus_get_addr(dev))
		return (NULL);

	return (dev_handle);
}
#endif /* HAVE_ACPI_IICBUS */

static int
iichid_cmd_read(struct iichid_softc* sc, void *buf, iichid_size_t maxlen,
    iichid_size_t *actual_len)
{
	/*
	 * 6.1.3 - Retrieval of Input Reports
	 * DEVICE returns the length (2 Bytes) and the entire Input Report.
	 */
	uint8_t actbuf[2] = { 0, 0 };
	/* Read actual input report length */
	struct iic_msg msgs[] = {
	    { sc->addr, IIC_M_RD | IIC_M_NOSTOP, sizeof(actbuf), actbuf },
	};
	uint16_t actlen;
	int error;

	error = iicbus_transfer(sc->dev, msgs, nitems(msgs));
	if (error != 0)
		return (error);

	actlen = actbuf[0] | actbuf[1] << 8;
	if (actlen <= 2 || actlen == 0xFFFF || maxlen == 0) {
		/* Read and discard 1 byte to send I2C STOP condition */
		msgs[0] = (struct iic_msg)
		    { sc->addr, IIC_M_RD | IIC_M_NOSTART, 1, actbuf };
		actlen = 0;
	} else {
		actlen -= 2;
		if (actlen > maxlen) {
			DPRINTF(sc, "input report too big. requested=%d "
			    "received=%d\n", maxlen, actlen);
			actlen = maxlen;
		}
		/* Read input report itself */
		msgs[0] = (struct iic_msg)
		    { sc->addr, IIC_M_RD | IIC_M_NOSTART, actlen, buf };
	}

	error = iicbus_transfer(sc->dev, msgs, 1);
	if (error == 0 && actual_len != NULL)
		*actual_len = actlen;

	DPRINTFN(sc, 5,
	    "%*D - %*D\n", 2, actbuf, " ", msgs[0].len, msgs[0].buf, " ");

	return (error);
}

static int
iichid_cmd_write(struct iichid_softc *sc, const void *buf, iichid_size_t len)
{
	/* 6.2.3 - Sending Output Reports */
	uint8_t *cmdreg = (uint8_t *)&sc->desc.wOutputRegister;
	uint16_t replen = 2 + len;
	uint8_t cmd[4] = { cmdreg[0], cmdreg[1], replen & 0xFF, replen >> 8 };
	struct iic_msg msgs[] = {
	    {sc->addr, IIC_M_WR | IIC_M_NOSTOP, sizeof(cmd), cmd},
	    {sc->addr, IIC_M_WR | IIC_M_NOSTART, len, __DECONST(void *, buf)},
	};

	if (le16toh(sc->desc.wMaxOutputLength) == 0)
		return (IIC_ENOTSUPP);
	if (len < 2)
		return (IIC_ENOTSUPP);

	DPRINTF(sc, "HID command I2C_HID_CMD_WRITE (len %d): "
	    "%*D\n", len, len, buf, " ");

	return (iicbus_transfer(sc->dev, msgs, nitems(msgs)));
}

static int
iichid_cmd_get_hid_desc(struct iichid_softc *sc, uint16_t config_reg,
    struct i2c_hid_desc *hid_desc)
{
	/*
	 * 5.2.2 - HID Descriptor Retrieval
	 * register is passed from the controller
	 */
	uint16_t cmd = htole16(config_reg);
	struct iic_msg msgs[] = {
	    { sc->addr, IIC_M_WR | IIC_M_NOSTOP, 2, (uint8_t *)&cmd },
	    { sc->addr, IIC_M_RD, sizeof(*hid_desc), (uint8_t *)hid_desc },
	};
	int error;

	DPRINTF(sc, "HID command I2C_HID_CMD_DESCR at 0x%x\n", config_reg);

	error = iicbus_transfer(sc->dev, msgs, nitems(msgs));
	if (error != 0)
		return (error);

	DPRINTF(sc, "HID descriptor: %*D\n",
	    (int)sizeof(struct i2c_hid_desc), hid_desc, " ");

	return (0);
}

static int
iichid_set_power(struct iichid_softc *sc, uint8_t param)
{
	uint8_t *cmdreg = (uint8_t *)&sc->desc.wCommandRegister;
	uint8_t cmd[] = { cmdreg[0], cmdreg[1], param, I2C_HID_CMD_SET_POWER };
	struct iic_msg msgs[] = {
	    { sc->addr, IIC_M_WR, sizeof(cmd), cmd },
	};

	DPRINTF(sc, "HID command I2C_HID_CMD_SET_POWER(%d)\n", param);

	return (iicbus_transfer(sc->dev, msgs, nitems(msgs)));
}

static int
iichid_reset(struct iichid_softc *sc)
{
	uint8_t *cmdreg = (uint8_t *)&sc->desc.wCommandRegister;
	uint8_t cmd[] = { cmdreg[0], cmdreg[1], 0, I2C_HID_CMD_RESET };
	struct iic_msg msgs[] = {
	    { sc->addr, IIC_M_WR, sizeof(cmd), cmd },
	};

	DPRINTF(sc, "HID command I2C_HID_CMD_RESET\n");

	return (iicbus_transfer(sc->dev, msgs, nitems(msgs)));
}

static int
iichid_cmd_get_report_desc(struct iichid_softc* sc, void *buf,
    iichid_size_t len)
{
	uint16_t cmd = sc->desc.wReportDescRegister;
	struct iic_msg msgs[] = {
	    { sc->addr, IIC_M_WR | IIC_M_NOSTOP, 2, (uint8_t *)&cmd },
	    { sc->addr, IIC_M_RD, len, buf },
	};
	int error;

	DPRINTF(sc, "HID command I2C_HID_REPORT_DESCR at 0x%x with size %d\n",
	    le16toh(cmd), len);

	error = iicbus_transfer(sc->dev, msgs, nitems(msgs));
	if (error != 0)
		return (error);

	DPRINTF(sc, "HID report descriptor: %*D\n", len, buf, " ");

	return (0);
}

static int
iichid_cmd_get_report(struct iichid_softc* sc, void *buf, iichid_size_t maxlen,
    iichid_size_t *actual_len, uint8_t type, uint8_t id)
{
	/*
	 * 7.2.2.4 - "The protocol is optimized for Report < 15.  If a
	 * report ID >= 15 is necessary, then the Report ID in the Low Byte
	 * must be set to 1111 and a Third Byte is appended to the protocol.
	 * This Third Byte contains the entire/actual report ID."
	 */
	uint8_t *dtareg = (uint8_t *)&sc->desc.wDataRegister;
	uint8_t *cmdreg = (uint8_t *)&sc->desc.wCommandRegister;
	uint8_t cmd[] =	{   /*________|______id>=15_____|______id<15______*/
						    cmdreg[0]		   ,
						    cmdreg[1]		   ,
			    (id >= 15 ? 15 | (type << 4): id | (type << 4)),
					      I2C_HID_CMD_GET_REPORT	   ,
			    (id >= 15 ?		id	:    dtareg[0]	  ),
			    (id >= 15 ?	   dtareg[0]	:    dtareg[1]	  ),
			    (id >= 15 ?    dtareg[1]	:	0	  ),
			};
	int cmdlen    =	    (id >= 15 ?		7	:	6	  );
	uint8_t actbuf[2] = { 0, 0 };
	uint16_t actlen;
	int d, error;
	struct iic_msg msgs[] = {
	    { sc->addr, IIC_M_WR | IIC_M_NOSTOP, cmdlen, cmd },
	    { sc->addr, IIC_M_RD | IIC_M_NOSTOP, 2, actbuf },
	    { sc->addr, IIC_M_RD | IIC_M_NOSTART, maxlen, buf },
	};

	if (maxlen == 0)
		return (EINVAL);

	DPRINTF(sc, "HID command I2C_HID_CMD_GET_REPORT %d "
	    "(type %d, len %d)\n", id, type, maxlen);

	/*
	 * 7.2.2.2 - Response will be a 2-byte length value, the report
	 * id (1 byte, if defined in Report Descriptor), and then the report.
	 */
	error = iicbus_transfer(sc->dev, msgs, nitems(msgs));
	if (error != 0)
		return (error);

	actlen = actbuf[0] | actbuf[1] << 8;
	if (actlen != maxlen + 2)
		DPRINTF(sc, "response size %d != expected length %d\n",
		    actlen, maxlen + 2);

	if (actlen <= 2 || actlen == 0xFFFF)
		return (ENOMSG);

	d = id != 0 ? *(uint8_t *)buf : 0;
	if (d != id) {
		DPRINTF(sc, "response report id %d != %d\n", d, id);
		return (EBADMSG);
	}

	actlen -= 2;
	if (actlen > maxlen)
		actlen = maxlen;
	if (actual_len != NULL)
		*actual_len = actlen;

	DPRINTF(sc, "response: %*D %*D\n", 2, actbuf, " ", actlen, buf, " ");

	return (0);
}

static int
iichid_cmd_set_report(struct iichid_softc* sc, const void *buf,
    iichid_size_t len, uint8_t type, uint8_t id)
{
	/*
	 * 7.2.2.4 - "The protocol is optimized for Report < 15.  If a
	 * report ID >= 15 is necessary, then the Report ID in the Low Byte
	 * must be set to 1111 and a Third Byte is appended to the protocol.
	 * This Third Byte contains the entire/actual report ID."
	 */
	uint8_t *dtareg = (uint8_t *)&sc->desc.wDataRegister;
	uint8_t *cmdreg = (uint8_t *)&sc->desc.wCommandRegister;
	uint16_t replen = 2 + len;
	uint8_t cmd[] =	{   /*________|______id>=15_____|______id<15______*/
						    cmdreg[0]		   ,
						    cmdreg[1]		   ,
			    (id >= 15 ? 15 | (type << 4): id | (type << 4)),
					      I2C_HID_CMD_SET_REPORT	   ,
			    (id >= 15 ?		id	:    dtareg[0]    ),
			    (id >= 15 ?    dtareg[0]	:    dtareg[1]    ),
			    (id >= 15 ?    dtareg[1]	:   replen & 0xff ),
			    (id >= 15 ?   replen & 0xff	:   replen >> 8   ),
			    (id >= 15 ?   replen >> 8	:	0	  ),
			};
	int cmdlen    =	    (id >= 15 ?		9	:	8	  );
	struct iic_msg msgs[] = {
	    {sc->addr, IIC_M_WR | IIC_M_NOSTOP, cmdlen, cmd},
	    {sc->addr, IIC_M_WR | IIC_M_NOSTART, len, __DECONST(void *, buf)},
	};

	DPRINTF(sc, "HID command I2C_HID_CMD_SET_REPORT %d (type %d, len %d): "
	    "%*D\n", id, type, len, len, buf, " ");

	return (iicbus_transfer(sc->dev, msgs, nitems(msgs)));
}

static void
iichid_event_task(void *context, int pending)
{
	struct iichid_softc *sc = context;
	device_t parent = device_get_parent(sc->dev);
	iichid_size_t maxlen, actual = 0;
	bool locked = false;
	int error;

	if (iicbus_request_bus(parent, sc->dev, IIC_WAIT) != 0)
		goto rearm;

	maxlen = sc->power_on ? sc->intr_bufsize : 0;
	error = iichid_cmd_read(sc, sc->intr_buf, maxlen, &actual);
	iicbus_release_bus(parent, sc->dev);
	if (error != 0) {
		DPRINTF(sc, "read error occured: %d\n", error);
		goto rearm;
	}

	if (!sc->power_on)
		goto rearm;

	mtx_lock(sc->intr_mtx);
	locked = true;
	if (actual > 0) {
		if (sc->open)
			sc->intr_handler(sc->intr_ctx, sc->intr_buf, actual);
#ifdef IICHID_SAMPLING
		sc->missing_samples = 0;
#endif
	} else
#ifdef IICHID_SAMPLING
		++sc->missing_samples;
#else
		DPRINTF(sc, "no data received\n");
#endif

rearm:
#ifdef IICHID_SAMPLING
	if (sc->callout_setup && sc->sampling_rate_slow > 0 && sc->open) {
		if (sc->missing_samples == sc->sampling_hysteresis)
			sc->intr_handler(sc->intr_ctx, sc->intr_buf, 0);
		taskqueue_enqueue_timeout(sc->taskqueue, &sc->periodic_task,
		    hz / MAX(sc->missing_samples >= sc->sampling_hysteresis ?
		      sc->sampling_rate_slow : sc->sampling_rate_fast, 1));
	}
#endif
	if (locked)
		mtx_unlock(sc->intr_mtx);
}

static void
iichid_intr(void *context)
{
	struct iichid_softc *sc = context;
#ifdef HAVE_IG4_POLLING
	device_t parent = device_get_parent(sc->dev);
	iichid_size_t maxlen, actual = 0;
	int error;

	/*
	 * Designware(IG4) driver-specific hack.
	 * Requesting of an I2C bus with IIC_DONTWAIT parameter enables polled
	 * mode in the driver, making possible iicbus_transfer execution from
	 * interrupt handlers and callouts.
	 */
	if (iicbus_request_bus(parent, sc->dev, IIC_DONTWAIT) != 0)
		return;

	/*
	 * Reading of input reports of I2C devices residing in SLEEP state is
	 * not allowed and often returns a garbage. If a HOST needs to
	 * communicate with the DEVICE it MUST issue a SET POWER command
	 * (to ON) before any other command. As some hardware requires reads to
	 * acknoledge interrupts we fetch only length header and discard it.
	 */
	maxlen = sc->power_on ? sc->intr_bufsize : 0;
	error = iichid_cmd_read(sc, sc->intr_buf, maxlen, &actual);
	iicbus_release_bus(parent, sc->dev);
	if (error != 0) {
		DPRINTF(sc, "read error occured: %d\n", error);
		return;
	}

	if (!sc->power_on)
		return;

	if (actual == 0) {
		DPRINTF(sc, "no data received\n");
		return;
	}

	mtx_lock(sc->intr_mtx);
	if (sc->open)
		sc->intr_handler(sc->intr_ctx, sc->intr_buf, actual);
	mtx_unlock(sc->intr_mtx);
#else
	taskqueue_enqueue(sc->taskqueue, &sc->event_task);
#endif
}

static int
iichid_set_power_state(struct iichid_softc *sc, enum iichid_powerstate_how how)
{
	device_t parent = device_get_parent(sc->dev);
	int error = 0;
	bool power_on;

	/*
	 * Request iicbus early as sc->suspend and sc->power_on
	 * are protected by iicbus internal lock.
	 */
	error = iicbus_request_bus(parent, sc->dev, IIC_WAIT);
	if (error != 0)
		return (error);

	switch (how) {
	case IICHID_PS_SUSPEND:
		sc->suspend = true;
		break;
	case IICHID_PS_RESUME:
		sc->suspend = false;
		break;
	case IICHID_PS_NOCHANGE:
	default:
		break;
	}

	mtx_lock(sc->intr_mtx);
again:
	power_on = sc->open & !sc->suspend;
	mtx_unlock(sc->intr_mtx);

	if (power_on != sc->power_on) {
		error = iichid_set_power(sc,
		    power_on ? I2C_HID_POWER_ON : I2C_HID_POWER_OFF);

		sc->power_on = power_on;
		mtx_lock(sc->intr_mtx);
		/* Redo command if sc->open has been changed */
		if (power_on != (sc->open & !sc->suspend))
			goto again;
#ifdef IICHID_SAMPLING
		if (sc->sampling_rate_slow >= 0 && sc->intr_handler != NULL) {
			if (power_on) {
				iichid_setup_callout(sc);
				iichid_reset_callout(sc);
			} else
				iichid_teardown_callout(sc);
		}
#endif
		mtx_unlock(sc->intr_mtx);
	}

	iicbus_release_bus(parent, sc->dev);

	return (error);
}

static void
iichid_power_task(void *context, int pending)
{
	struct iichid_softc *sc = context;

	(void)iichid_set_power_state(sc, IICHID_PS_NOCHANGE);
}

static int
iichid_setup_interrupt(struct iichid_softc *sc)
{
	sc->irq_cookie = 0;

	int error = bus_setup_intr(sc->dev, sc->irq_res,
	    INTR_TYPE_TTY|INTR_MPSAFE, NULL, iichid_intr, sc, &sc->irq_cookie);
	if (error != 0)
		DPRINTF(sc, "Could not setup interrupt handler\n");
	else
		DPRINTF(sc, "successfully setup interrupt\n");

	return (error);
}

static void
iichid_teardown_interrupt(struct iichid_softc *sc)
{
	if (sc->irq_cookie)
		bus_teardown_intr(sc->dev, sc->irq_res, sc->irq_cookie);

	sc->irq_cookie = 0;
}

#ifdef IICHID_SAMPLING
static int
iichid_setup_callout(struct iichid_softc *sc)
{

	mtx_assert(sc->intr_mtx, MA_OWNED);

	if (sc->sampling_rate_slow < 0) {
		DPRINTF(sc, "sampling_rate is below 0, can't setup callout\n");
		return (EINVAL);
	}

	sc->callout_setup = true;
	DPRINTF(sc, "successfully setup callout\n");
	return (0);
}

static int
iichid_reset_callout(struct iichid_softc *sc)
{

	mtx_assert(sc->intr_mtx, MA_OWNED);

	if (sc->sampling_rate_slow <= 0) {
		DPRINTF(sc, "sampling_rate is below or equal to 0, "
		    "can't reset callout\n");
		return (EINVAL);
	}

	if (!sc->callout_setup)
		return (EINVAL);

	/* Start with slow sampling */
	sc->missing_samples = sc->sampling_hysteresis;
	taskqueue_enqueue(sc->taskqueue, &sc->event_task);

	return (0);
}

static void
iichid_teardown_callout(struct iichid_softc *sc)
{

	mtx_assert(sc->intr_mtx, MA_OWNED);

	sc->callout_setup = false;
	taskqueue_cancel_timeout(sc->taskqueue, &sc->periodic_task, NULL);
	DPRINTF(sc, "tore callout down\n");
}

static int
iichid_sysctl_sampling_rate_handler(SYSCTL_HANDLER_ARGS)
{
	struct iichid_softc *sc;
	int error, oldval, value;

	sc = arg1;

	error = sysctl_wire_old_buffer(req, sizeof(int));
	if (error != 0)
		return (error);

	mtx_lock(sc->intr_mtx);

	value = sc->sampling_rate_slow;
	oldval = sc->sampling_rate_slow;
	error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == NULL ||
	    value == sc->sampling_rate_slow) {
		mtx_unlock(sc->intr_mtx);
		return (error);
	}

	/* Can't switch to interrupt mode if it is not supported */
	if (sc->irq_res == NULL && value < 0) {
		mtx_unlock(sc->intr_mtx);
		return (EINVAL);
	}

	sc->sampling_rate_slow = value;

	if (oldval < 0 && value >= 0) {
		iichid_teardown_interrupt(sc);
		if (sc->open)
			iichid_setup_callout(sc);
	} else if (oldval >= 0 && value < 0) {
		if (sc->open)
			iichid_teardown_callout(sc);
		iichid_setup_interrupt(sc);
	}

	if (sc->open && value > 0)
		iichid_reset_callout(sc);

	DPRINTF(sc, "new sampling_rate value: %d\n", value);

	mtx_unlock(sc->intr_mtx);

	return (0);
}
#endif /* IICHID_SAMPLING */

static void
iichid_intr_setup(device_t dev, struct mtx *mtx, hid_intr_t intr,
    void *context, struct hidbus_report_descr *rdesc)
{
	struct iichid_softc* sc = device_get_softc(dev);

	/*
	 * Do not rely on wMaxInputLength, as some devices may set it to
	 * a wrong length. Find the longest input report in report descriptor.
	 */
	rdesc->rdsize = rdesc->isize;
	/* Write and get/set_report sizes are limited by I2C-HID protocol */
	rdesc->wrsize = rdesc->grsize = rdesc->srsize = IICHID_SIZE_MAX;

	sc->intr_handler = intr;
	sc->intr_ctx = context;
	sc->intr_mtx = mtx;
	sc->intr_buf = malloc(rdesc->rdsize, M_DEVBUF, M_WAITOK | M_ZERO);
	sc->intr_bufsize = rdesc->rdsize;
	taskqueue_start_threads(&sc->taskqueue, 1, PI_TTY,
	    "%s taskq", device_get_nameunit(sc->dev));
}

static void
iichid_intr_unsetup(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);

	taskqueue_drain_all(sc->taskqueue);
	free(sc->intr_buf, M_DEVBUF);
}

static int
iichid_intr_start(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);

	mtx_assert(sc->intr_mtx, MA_OWNED);

	DPRINTF(sc, "iichid device open\n");

	sc->open = true;
	taskqueue_enqueue(sc->taskqueue, &sc->power_task);

	return (0);
}

static int
iichid_intr_stop(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);

	mtx_assert(sc->intr_mtx, MA_OWNED);

	DPRINTF(sc, "iichid device close\n");

	/*
	 * 8.2 - The HOST determines that there are no active applications
	 * that are currently using the specific HID DEVICE. The HOST
	 * is recommended to issue a HIPO command to the DEVICE to force
	 * the DEVICE in to a lower power state.
	 */
	sc->open = false;
	taskqueue_enqueue(sc->taskqueue, &sc->power_task);

	return (0);
}

static void
iichid_intr_poll(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);
	iichid_size_t actual = 0;
	int error;

	error = iichid_cmd_read(sc, sc->intr_buf, sc->intr_bufsize, &actual);
	if (error == 0 && actual != 0 && sc->open)
		sc->intr_handler(sc->intr_ctx, sc->intr_buf, actual);
}

/*
 * HID interface
 */
static int
iichid_get_report_desc(device_t dev, void *buf, hid_size_t len)
{
	struct iichid_softc* sc = device_get_softc(dev);
	int error;

	error = iichid_cmd_get_report_desc(sc, buf, len);
	if (error) {
		device_printf(dev, "failed to fetch report descriptor: %d\n",
		    error);
		return (ENXIO);
	}

	return (0);
}

static int
iichid_read(device_t dev, void *buf, hid_size_t maxlen, hid_size_t *actlen)
{
	struct iichid_softc* sc = device_get_softc(dev);
	device_t parent = device_get_parent(sc->dev);
	int error;

	if (maxlen > IICHID_SIZE_MAX)
		return (EMSGSIZE);

	error = iicbus_request_bus(parent, sc->dev, IIC_WAIT);
	if (error == 0) {
		error = iichid_cmd_read(sc, buf, maxlen, actlen);
		iicbus_release_bus(parent, sc->dev);
	}

	return (iic2errno(error));
}

static int
iichid_write(device_t dev, const void *buf, hid_size_t len)
{
	struct iichid_softc* sc = device_get_softc(dev);

	if (len > IICHID_SIZE_MAX)
		return (EMSGSIZE);

	return (iic2errno(iichid_cmd_write(sc, buf, len)));
}

static int
iichid_get_report(device_t dev, void *buf, hid_size_t maxlen,
    hid_size_t *actlen, uint8_t type, uint8_t id)
{
	struct iichid_softc* sc = device_get_softc(dev);

	if (maxlen > IICHID_SIZE_MAX)
		return (EMSGSIZE);

	return (iic2errno(
	    iichid_cmd_get_report(sc, buf, maxlen, actlen, type, id)));
}

static int
iichid_set_report(device_t dev, const void *buf, hid_size_t len, uint8_t type,
    uint8_t id)
{
	struct iichid_softc* sc = device_get_softc(dev);

	if (len > IICHID_SIZE_MAX)
		return (EMSGSIZE);

	return (iic2errno(iichid_cmd_set_report(sc, buf, len, type, id)));
}

static int
iichid_set_idle(device_t dev, uint16_t duration, uint8_t id)
{

	return (ENOTSUP);
}

static int
iichid_set_protocol(device_t dev, uint16_t protocol)
{

	return (ENOTSUP);
}

static void
iichid_init_device_info(struct i2c_hid_desc *desc, ACPI_HANDLE handle,
    struct hid_device_info *hw)
{

	hw->idBus = BUS_I2C;
	hw->idVendor = le16toh(desc->wVendorID);
	hw->idProduct = le16toh(desc->wProductID);
	hw->idVersion = le16toh(desc->wVersionID);
}

static int
iichid_fill_device_info(struct i2c_hid_desc *desc, ACPI_HANDLE handle,
    struct hid_device_info *hw)
{
	ACPI_DEVICE_INFO *device_info;

	/* get ACPI HID. It is a base part of the device name */
	if (ACPI_FAILURE(AcpiGetObjectInfo(handle, &device_info)))
		return (ENXIO);

	snprintf(hw->name, sizeof(hw->name), "%s:%02lX %04X:%04X",
	    (device_info->Valid & ACPI_VALID_HID) ?
	    device_info->HardwareId.String : "Unknown",
	    (device_info->Valid & ACPI_VALID_UID) ?
	    strtoul(device_info->UniqueId.String, NULL, 10) : 0UL,
	    le16toh(desc->wVendorID), le16toh(desc->wProductID));

	AcpiOsFree(device_info);

	strlcpy(hw->serial, "", sizeof(hw->serial));
	hw->rdescsize = le16toh(desc->wReportDescLength);
	if (desc->wOutputRegister == 0 || desc->wMaxOutputLength == 0)
		hid_add_dynamic_quirk(hw, HQ_NOWRITE);

	return (0);
}

static int
iichid_probe(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);
	ACPI_HANDLE handle;
	uint16_t addr = iicbus_get_addr(dev) << 1;
	int error;

	if (sc->probe_done)
		return (sc->probe_result);

	sc->probe_done = true;
	sc->probe_result = ENXIO;

	if (acpi_disabled("iichid"))
		return (ENXIO);
	if (addr == 0)
		return (ENXIO);

	sc->dev = dev;
	sc->addr = addr;

#ifdef HAVE_ACPI_IICBUS
	handle = acpi_get_handle(dev);
#else
	handle = iichid_get_handle(dev);
#endif
	if (handle == NULL)
		return (ENXIO);

	if (!acpi_is_iichid(handle))
		return (ENXIO);

	if (ACPI_FAILURE(iichid_get_config_reg(handle, &sc->config_reg)))
		return (ENXIO);

	error = iichid_cmd_get_hid_desc(sc, sc->config_reg, &sc->desc);
	if (error) {
		device_printf(dev, "could not retrieve HID descriptor from "
		    "the device: %d\n", error);
		return (ENXIO);
	}

	if (le16toh(sc->desc.wHIDDescLength) != 30 ||
	    le16toh(sc->desc.bcdVersion) != 0x100) {
		device_printf(dev, "HID descriptor is broken\n");
		return (ENXIO);
	}

	/*
	 * Setup temporary hid_device_info so that we can figure out some
	 * basic quirks for this device.
	 */
	iichid_init_device_info(&sc->desc, handle, &sc->hw);

	if (hid_test_quirk(&sc->hw, HQ_HID_IGNORE))
		return (ENXIO);

	sc->probe_result = BUS_PROBE_DEFAULT;
	return (sc->probe_result);
}

static int
iichid_attach(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);
	ACPI_HANDLE handle;
	device_t child;
	int error;

	/* Fetch hardware settings from ACPI */
#ifdef HAVE_ACPI_IICBUS
	handle = acpi_get_handle(dev);
#else
	handle = iichid_get_handle(dev);
#endif
	if (handle == NULL)
		return (ENXIO);

	if (iichid_fill_device_info(&sc->desc, handle, &sc->hw) != 0) {
		device_printf(dev, "error evaluating AcpiGetObjectInfo\n");
		return (ENXIO);
	}

	device_printf(dev, "<%s I2C HID device> on %s\n",
	    sc->hw.name, device_get_nameunit(device_get_parent(dev)));
	DPRINTF(sc, "  IICbus addr       : 0x%02X\n", sc->addr >> 1);
	DPRINTF(sc, "  HID descriptor reg: 0x%02X\n", sc->config_reg);

	error = iichid_set_power(sc, I2C_HID_POWER_ON);
	if (error) {
		device_printf(dev, "failed to power on: %d\n", error);
		return (ENXIO);
	}
	/*
	 * Windows driver sleeps for 1ms between the SET_POWER and RESET
	 * commands. So we too as some devices may depend on this.
	 */
	pause("iichid", (hz + 999) / 1000);

	error = iichid_reset(sc);
	if (error) {
		device_printf(dev, "failed to reset hardware: %d\n", error);
		return (ENXIO);
	}

	sc->power_on = false;
	TASK_INIT(&sc->event_task, 0, iichid_event_task, sc);
	TASK_INIT(&sc->power_task, 0, iichid_power_task, sc);
	/* taskqueue_create can't fail with M_WAITOK mflag passed */
	sc->taskqueue = taskqueue_create("imt_tq", M_WAITOK | M_ZERO,
	    taskqueue_thread_enqueue, &sc->taskqueue);
#ifdef IICHID_SAMPLING
	TIMEOUT_TASK_INIT(sc->taskqueue, &sc->periodic_task, 0,
	    iichid_event_task, sc);

	sc->sampling_rate_slow = -1;
	sc->sampling_rate_fast = IICHID_SAMPLING_RATE_FAST;
	sc->sampling_hysteresis = IICHID_SAMPLING_HYSTERESIS;
#endif

	sc->irq_rid = 0;
	sc->irq_res = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ,
	    &sc->irq_rid, RF_ACTIVE);

	if (sc->irq_res != NULL) {
		DPRINTF(sc, "allocated irq at %p and rid %d\n",
		    sc->irq_res, sc->irq_rid);
		error = iichid_setup_interrupt(sc);
	}

	if (sc->irq_res == NULL || error != 0) {
#ifdef IICHID_SAMPLING
		device_printf(sc->dev,
		    "Interrupt setup failed. Fallback to sampling\n");
		sc->sampling_rate_slow = IICHID_SAMPLING_RATE_SLOW;
#else
		device_printf(sc->dev, "Interrupt setup failed\n");
		error = ENXIO;
		goto done;
#endif
	}

#ifdef IICHID_SAMPLING
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(sc->dev),
		SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)),
		OID_AUTO, "sampling_rate_slow", CTLTYPE_INT | CTLFLAG_RWTUN,
		sc, 0, iichid_sysctl_sampling_rate_handler, "I",
		"idle sampling rate in num/second");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(sc->dev),
		SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)),
		OID_AUTO, "sampling_rate_fast", CTLTYPE_INT | CTLFLAG_RWTUN,
		&sc->sampling_rate_fast, 0,
		"active sampling rate in num/second");
	SYSCTL_ADD_INT(device_get_sysctl_ctx(sc->dev),
		SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)),
		OID_AUTO, "sampling_hysteresis", CTLTYPE_INT | CTLFLAG_RWTUN,
		&sc->sampling_hysteresis, 0,
		"number of missing samples before enabling of slow mode");
	hid_add_dynamic_quirk(&sc->hw, HQ_IICHID_SAMPLING);
#endif /* IICHID_SAMPLING */

	child = device_add_child(dev, "hidbus", -1);
	if (child == NULL) {
		device_printf(sc->dev, "Could not add I2C device\n");
		error = ENXIO;
		goto done;
	}

	device_set_ivars(child, &sc->hw);
	error = bus_generic_attach(dev);
	if (error)
		device_printf(dev, "failed to attach child: error %d\n", error);

done:
	(void)iichid_set_power(sc, I2C_HID_POWER_OFF);
	return (error);
}

static int
iichid_detach(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);
	int error;

	error = device_delete_children(dev);
	if (error)
		return (error);

	iichid_teardown_interrupt(sc);

	if (sc->irq_res)
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid,
		    sc->irq_res);

	if (sc->taskqueue)
		taskqueue_free(sc->taskqueue);
	sc->taskqueue = NULL;

	return (0);
}

#ifndef HAVE_ACPI_IICBUS
static ACPI_STATUS
iichid_identify_cb(ACPI_HANDLE handle, UINT32 level, void *context,
    void **status)
{
	devclass_t acpi_iichid_devclass;
	struct resource_list *acpi_iichid_rl;
	device_t iicbus = context;
	device_t child, *children, acpi_iichid;
	int ccount, i;
	uint16_t device_addr;

	if (!acpi_is_iichid(handle))
		 return (AE_OK);

	device_addr = acpi_get_iichid_addr(handle);
	if (device_addr == 0)
		return (AE_OK);

	/* get a list of all children below iicbus */
	if (device_get_children(iicbus, &children, &ccount) != 0)
		return (AE_OK);

	/* scan through to find out if I2C addr is already in use */
	for (i = 0; i < ccount; i++) {
		if (iicbus_get_addr(children[i]) == device_addr) {
			free(children, M_TEMP);
			return (AE_OK);
		}
	}
	free(children, M_TEMP);

	/* No I2C devices tied to the addr found. Add a child */
	child = BUS_ADD_CHILD(iicbus, 0, NULL, -1);
	if (child == NULL) {
		device_printf(iicbus, "add child failed\n");
		return (AE_OK);
	}

	/*
	 * Ensure dummy driver is attached. We are going to remove resources
	 * from the ACPI device so don't let other drivers occupy its place.
	 */
	acpi_iichid = acpi_get_device(handle);
	if (acpi_iichid == NULL)
		return (AE_OK);

	if (!device_is_alive(acpi_iichid))
		device_probe_and_attach(acpi_iichid);

	acpi_iichid_devclass = devclass_find("acpi_iichid");
	if (device_get_devclass(acpi_iichid) != acpi_iichid_devclass)
		return (AE_OK);

	iicbus_set_addr(child, device_addr);

	/* Move all resources including IRQ from ACPI to I2C device */
	acpi_iichid_rl =
	    BUS_GET_RESOURCE_LIST(device_get_parent(acpi_iichid), acpi_iichid);
	resource_list_purge(acpi_iichid_rl);
	acpi_parse_resources(child, handle, &acpi_res_parse_set, NULL);

	return (AE_OK);
}

static void
iichid_identify(driver_t *driver, device_t parent)
{
	ACPI_HANDLE	ctrl_handle;

	ctrl_handle = acpi_get_handle(device_get_parent(parent));
	AcpiWalkNamespace(ACPI_TYPE_DEVICE, ctrl_handle,
	    1, iichid_identify_cb, NULL, parent, NULL);
}
#endif /* HAVE_ACPI_IICBUS */

static int
iichid_suspend(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);
	int error;

	DPRINTF(sc, "Suspend called, setting device to power_state 1\n");

	(void)bus_generic_suspend(dev);

	/*
	 * 8.2 - The HOST is going into a deep power optimized state and wishes
	 * to put all the devices into a low power state also. The HOST
	 * is recommended to issue a HIPO command to the DEVICE to force
	 * the DEVICE in to a lower power state
         */
	error = iichid_set_power_state(sc, IICHID_PS_SUSPEND);
	if (error != 0)
		DPRINTF(sc, "Could not set power_state, error: %d\n", error);
	else
		DPRINTF(sc, "Successfully set power_state\n");

        return (0);
}

static int
iichid_resume(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);
	int error;

	DPRINTF(sc, "Resume called, setting device to power_state 0\n");

	error = iichid_set_power_state(sc, IICHID_PS_RESUME);
	if (error != 0)
		DPRINTF(sc, "Could not set power_state, error: %d\n", error);
	else
		DPRINTF(sc, "Successfully set power_state\n");

	(void)bus_generic_resume(dev);

	return (0);
}

static devclass_t iichid_devclass;

static device_method_t iichid_methods[] = {

#ifndef HAVE_ACPI_IICBUS
	DEVMETHOD(device_identify,      iichid_identify),
#endif
        DEVMETHOD(device_probe,         iichid_probe),
        DEVMETHOD(device_attach,        iichid_attach),
        DEVMETHOD(device_detach,        iichid_detach),
        DEVMETHOD(device_suspend,       iichid_suspend),
        DEVMETHOD(device_resume,        iichid_resume),

	DEVMETHOD(hid_intr_setup,	iichid_intr_setup),
	DEVMETHOD(hid_intr_unsetup,	iichid_intr_unsetup),
	DEVMETHOD(hid_intr_start,	iichid_intr_start),
	DEVMETHOD(hid_intr_stop,	iichid_intr_stop),
	DEVMETHOD(hid_intr_poll,	iichid_intr_poll),

	/* HID interface */
	DEVMETHOD(hid_get_report_descr,	iichid_get_report_desc),
	DEVMETHOD(hid_read,		iichid_read),
	DEVMETHOD(hid_write,		iichid_write),
	DEVMETHOD(hid_get_report,	iichid_get_report),
	DEVMETHOD(hid_set_report,	iichid_set_report),
	DEVMETHOD(hid_set_idle,		iichid_set_idle),
	DEVMETHOD(hid_set_protocol,	iichid_set_protocol),

        DEVMETHOD_END
};

static driver_t iichid_driver = {
        .name = "iichid",
        .methods = iichid_methods,
        .size = sizeof(struct iichid_softc),
};

DRIVER_MODULE(iichid, iicbus, iichid_driver, iichid_devclass, NULL, 0);
MODULE_DEPEND(iichid, iicbus, IICBUS_MINVER, IICBUS_PREFVER, IICBUS_MAXVER);
MODULE_DEPEND(iichid, acpi, 1, 1, 1);
MODULE_DEPEND(iichid, hid, 1, 1, 1);
MODULE_VERSION(iichid, 1);

#ifndef HAVE_ACPI_IICBUS
/*
 * Dummy ACPI driver. Used as bus resources holder for iichid.
 */

static device_probe_t           acpi_iichid_probe;
static device_attach_t          acpi_iichid_attach;
static device_detach_t          acpi_iichid_detach;

static int
acpi_iichid_probe(device_t dev)
{
	if (acpi_disabled("iichid") ||
#if __FreeBSD_version >= 1300000
	    ACPI_ID_PROBE(device_get_parent(dev), dev, iichid_ids, NULL) > 0)
#else
	    ACPI_ID_PROBE(device_get_parent(dev), dev, iichid_ids) == NULL)
#endif
		return (ENXIO);

	device_set_desc(dev, "HID over I2C (ACPI)");

	return (BUS_PROBE_VENDOR);
}

static int
acpi_iichid_attach(device_t dev)
{

	return (0);
}

static int
acpi_iichid_detach(device_t dev)
{

	return (0);
}

static devclass_t acpi_iichid_devclass;

static device_method_t acpi_iichid_methods[] = {
	/* device interface */
	DEVMETHOD(device_probe, acpi_iichid_probe),
	DEVMETHOD(device_attach, acpi_iichid_attach),
	DEVMETHOD(device_detach, acpi_iichid_detach),

	DEVMETHOD_END
};

static driver_t acpi_iichid_driver = {
	.name = "acpi_iichid",
	.methods = acpi_iichid_methods,
	.size = 1,
};

DRIVER_MODULE(acpi_iichid, acpi, acpi_iichid_driver, acpi_iichid_devclass, NULL, 0);
MODULE_DEPEND(acpi_iichid, acpi, 1, 1, 1);
MODULE_VERSION(acpi_iichid, 1);

#endif /* HAVE_ACPI_IICBUS */
