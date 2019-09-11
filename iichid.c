/*-
 * Copyright (c) 2018-2019 Marc Priggemeyer <marc.priggemeyer@gmail.com>
 * Copyright (c) 2019 Vladimir Kondratyev <wulf@FreeBSD.org>
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

#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iic.h>
#include <dev/iicbus/iiconf.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbhid.h>

#include "iichid.h"
#include "iichidvar.h"

#define IICHID_DEBUG

#ifdef IICHID_DEBUG
#define	DPRINTF(sc, ...)	device_printf((sc)->dev, __VA_ARGS__)
#else
#define	DPRINTF(sc, ...)
#endif

static char *iichid_ids[] = {
	"PNP0C50",
	"ACPI0C50",
	NULL
};

static int	iichid_setup_callout(struct iichid_softc *);
static int	iichid_reset_callout(struct iichid_softc *);
static void	iichid_teardown_callout(struct iichid_softc *);

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
#endif /* HAVE_ACPI_IICBUS */

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

	AcpiOsFree(result);
	return (status);
}

#ifndef HAVE_ACPI_IICBUS
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
iichid_write_register(device_t dev, void* cmd, int cmdlen)
{
	uint16_t addr = iicbus_get_addr(dev);
	struct iic_msg msgs[] = {
	    { addr << 1, IIC_M_WR, cmdlen, cmd },
	};

	return (iicbus_transfer(dev, msgs, nitems(msgs)));
}

static int
iichid_cmd_get_input_report(struct iichid_softc* sc, void *buf, int len,
    int *actual_len, bool do_poll)
{
	/*
	 * 6.1.3 - Retrieval of Input Reports
	 * DEVICE returns the length (2 Bytes) and the entire Input Report.
	 */
	uint16_t addr = iicbus_get_addr(sc->dev);
	uint8_t actbuf[2] = { 0, 0 };
	/* Read actual input report length */
	struct iic_msg msgs[] = {
	    { addr << 1, IIC_M_RD | IIC_M_NOSTOP, sizeof(actbuf), actbuf },
	};
	device_t parent = device_get_parent(sc->dev);
	/*
	 * Designware(IG4) driver-specific hack.
	 * Requesting of an I2C bus with IIC_DONTWAIT parameter enables polling
	 * mode in the driver, making possible iicbus_transfer execution from
	 * interrupt handlers and callouts.
	 */
	int how = do_poll ? IIC_DONTWAIT : IIC_WAIT;
	int error, actlen;

	if (iicbus_request_bus(parent, sc->dev, how) != 0)
		return (IIC_EBUSBSY);

	error = iicbus_transfer(sc->dev, msgs, nitems(msgs));
	if (error != 0)
		goto out;

	actlen = actbuf[0] | actbuf[1] << 8;
	if (actlen <= 2 || actlen == 0xFFFF) {
		/* Read and discard 1 byte to send I2C STOP condition */
		msgs[0] = (struct iic_msg)
		    { addr << 1, IIC_M_RD | IIC_M_NOSTART, 1, actbuf };
		actlen = 0;
	} else {
		actlen -= 2;
		if (actlen > len) {
			DPRINTF(sc, "input report too big. requested=%d "
			    "received=%d\n", len, actlen);
			actlen = len;
		}
		/* Read input report itself */
		msgs[0] = (struct iic_msg)
		    { addr << 1, IIC_M_RD | IIC_M_NOSTART, actlen, buf };
	}

	error = iicbus_transfer(sc->dev, msgs, 1);
	if (error == 0)
		*actual_len = actlen;

	/* DPRINTF(sc, "%*D - %*D\n", 2, actbuf, " ", actlen, buf, " "); */
out:
	iicbus_release_bus(parent, sc->dev);
	return (error);
}

static int
iichid_cmd_set_output_report(struct iichid_softc *sc, void *buf, int len)
{
	/* 6.2.3 - Sending Output Reports */
	uint8_t *cmdreg = (uint8_t *)&sc->desc.wOutputRegister;
	uint16_t addr = iicbus_get_addr(sc->dev);
	uint16_t replen = 2 + len;
	uint8_t cmd[4] = { cmdreg[0], cmdreg[1], replen & 0xFF, replen >> 8 };
	struct iic_msg msgs[] = {
	    { addr << 1, IIC_M_WR | IIC_M_NOSTOP, sizeof(cmd), cmd },
	    { addr << 1, IIC_M_WR | IIC_M_NOSTART, len, buf },
	};

	if (le16toh(sc->desc.wMaxOutputLength) == 0)
		return (IIC_ENOTSUPP);
	if (len < (sc->iid != 0 ? 3 : 2))
		return (IIC_ENOTSUPP);

	DPRINTF(sc, "HID command I2C_HID_CMD_SET_OUTPUT_REPORT %d (len %d): "
	    "%*D\n", sc->iid != 0 ? *(uint8_t *)buf : 0, len, len, buf, " ");

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
	uint16_t addr = iicbus_get_addr(sc->dev);
	uint16_t cmd = htole16(config_reg);
	struct iic_msg msgs[] = {
	    { addr << 1, IIC_M_WR | IIC_M_NOSTOP, 2, (uint8_t *)&cmd },
	    { addr << 1, IIC_M_RD, sizeof(*hid_desc), (uint8_t *)hid_desc },
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
iichid_set_power(device_t dev, bool enable)
{
	struct iichid_softc *sc = device_get_softc(dev);
	uint16_t cmdreg = htole16(sc->desc.wCommandRegister);
	uint8_t power_state = enable ? 0 : 1;
	/* 0x08, 4 reserved bits plus opcode (4 bit) */
	uint8_t cmd[] = {
		cmdreg & 0xff,
		cmdreg >> 8,
		power_state,
		I2C_HID_CMD_SET_POWER,
	};

	DPRINTF(sc, "HID command I2C_HID_CMD_SET_POWER(%d)\n", power_state);

	return (iichid_write_register(dev, cmd, sizeof(cmd)));
}

static int
iichid_reset(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);
	uint16_t cmdreg = htole16(sc->desc.wCommandRegister);
	uint8_t cmd[] = {
		cmdreg & 0xff,
		cmdreg >> 8,
		0,
		I2C_HID_CMD_RESET,
	};

	DPRINTF(sc, "HID command I2C_HID_CMD_RESET\n");

	return (iichid_write_register(dev, cmd, sizeof(cmd)));
}

static int
iichid_cmd_get_report_desc(struct iichid_softc* sc, void *buf, int len)
{
	uint16_t cmd = sc->desc.wReportDescRegister;
	uint16_t addr = iicbus_get_addr(sc->dev);
	struct iic_msg msgs[] = {
	    { addr << 1, IIC_M_WR | IIC_M_NOSTOP, 2, (uint8_t *)&cmd },
	    { addr << 1, IIC_M_RD, len, buf },
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
iichid_cmd_get_report(struct iichid_softc* sc, void *buf, int len,
    uint8_t type, uint8_t id)
{
	/*
	 * 7.2.2.4 - "The protocol is optimized for Report < 15.  If a
	 * report ID >= 15 is necessary, then the Report ID in the Low Byte
	 * must be set to 1111 and a Third Byte is appended to the protocol.
	 * This Third Byte contains the entire/actual report ID."
	 */
	uint16_t addr = iicbus_get_addr(sc->dev);
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
	uint8_t actlen[2] = { 0, 0 };
	int d, error;
	struct iic_msg msgs[] = {
	    { addr << 1, IIC_M_WR | IIC_M_NOSTOP, cmdlen, cmd },
	    { addr << 1, IIC_M_RD | IIC_M_NOSTOP, 2, actlen },
	    { addr << 1, IIC_M_RD | IIC_M_NOSTART, len, buf },
	};

	if (len < 1)
		return (EINVAL);

	DPRINTF(sc, "HID command I2C_HID_CMD_GET_REPORT %d "
	    "(type %d, len %d)\n", id, type, len);

	/*
	 * 7.2.2.2 - Response will be a 2-byte length value, the report
	 * id (1 byte, if defined in Report Descriptor), and then the report.
	 */
	error = iicbus_transfer(sc->dev, msgs, nitems(msgs));
	if (error != 0)
		return (error);

	d = actlen[0] | actlen[1] << 8;
	if (d != len + 2)
		DPRINTF(sc, "response size %d != expected length %d\n",
		    d, len + 2);

	d = id != 0 ? *(uint8_t *)buf : 0;
	if (d != id) {
		DPRINTF(sc, "response report id %d != %d\n", d, id);
		return (EBADMSG);
	}

	DPRINTF(sc, "response: %*D %*D\n", 2, actlen, " ", len, buf, " ");

	return (0);
}

static int
iichid_cmd_set_report(struct iichid_softc* sc, void *buf, int len,
    uint8_t type, uint8_t id)
{
	/*
	 * 7.2.2.4 - "The protocol is optimized for Report < 15.  If a
	 * report ID >= 15 is necessary, then the Report ID in the Low Byte
	 * must be set to 1111 and a Third Byte is appended to the protocol.
	 * This Third Byte contains the entire/actual report ID."
	 */
	uint16_t addr = iicbus_get_addr(sc->dev);
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
	    { addr << 1, IIC_M_WR | IIC_M_NOSTOP, cmdlen, cmd },
	    { addr << 1, IIC_M_WR | IIC_M_NOSTART, len, buf },
	};

	DPRINTF(sc, "HID command I2C_HID_CMD_SET_REPORT %d (type %d, len %d): "
	    "%*D\n", id, type, len, len, buf, " ");

	return (iicbus_transfer(sc->dev, msgs, nitems(msgs)));
}

static void
iichid_event_task(void *context, int pending)
{
	struct iichid_softc *sc = context;
	int actual = 0;
	int error;

	error = iichid_cmd_get_input_report(
	    sc, sc->ibuf, sc->isize, &actual, false);
	if (error != 0) {
		device_printf(sc->dev, "an error occured\n");
		return;
	}

	if (actual <= (sc->iid != 0 ? 1 : 0)) {
//		device_printf(sc->dev, "no data received\n");
		return;
	}

	mtx_lock(sc->intr_mtx);
	if (sc->open)
		sc->intr_handler(sc->intr_context, sc->ibuf, actual);
	mtx_unlock(sc->intr_mtx);
}

static void
iichid_intr(void *context)
{
	struct iichid_softc *sc = context;
#ifdef HAVE_IG4_POLLING
	int actual = 0;
	int error;

	if (taskqueue_poll_is_busy(sc->taskqueue, &sc->event_task) == 0 &&
	    (error = iichid_cmd_get_input_report(
	      sc, sc->ibuf, sc->isize, &actual, true)) != IIC_EBUSBSY) {
		if (error != 0) {
			device_printf(sc->dev, "an error occured\n");
			goto out;
		}
		if (actual <= (sc->iid != 0 ? 1 : 0))
			goto out;

		if (!sc->callout_setup)
			mtx_lock(sc->intr_mtx);
		if (sc->open)
			sc->intr_handler(sc->intr_context, sc->ibuf, actual);
		if (!sc->callout_setup)
			mtx_unlock(sc->intr_mtx);
	} else
#endif
		taskqueue_enqueue(sc->taskqueue, &sc->event_task);
#ifdef HAVE_IG4_POLLING
out:
#endif
	if (sc->callout_setup && sc->sampling_rate > 0 && sc->open)
		callout_reset(&sc->periodic_callout, hz / sc->sampling_rate,
		    iichid_intr, sc);
}

static int
iichid_set_power_state(struct iichid_softc *sc)
{
	int error = 0;
	bool power_on;

	sx_assert(&sc->lock, SA_XLOCKED);

	power_on = sc->open & !sc->suspend;
	if (power_on != sc->power_on) {
		error = iichid_set_power(sc->dev, power_on);
		mtx_lock(sc->intr_mtx);
		if (sc->sampling_rate >= 0 && sc->intr_handler != NULL) {
			if (power_on) {
				iichid_setup_callout(sc);
				iichid_reset_callout(sc);
			} else
				iichid_teardown_callout(sc);
		}
		mtx_unlock(sc->intr_mtx);
		sc->power_on = power_on;
	}

	return (error);
}

static void
iichid_power_task(void *context, int pending)
{
	struct iichid_softc *sc = context;

	sx_xlock(&sc->lock);
	(void)iichid_set_power_state(sc);
	sx_unlock(&sc->lock);
}

static int
iichid_setup_callout(struct iichid_softc *sc)
{

	if (sc->sampling_rate < 0) {
		DPRINTF(sc, "sampling_rate is below 0, can't setup callout\n");
		return (EINVAL);
	}

	sc->callout_setup=true;
	DPRINTF(sc, "successfully setup callout\n");
	return (0);
}

static int
iichid_reset_callout(struct iichid_softc *sc)
{

	if (sc->sampling_rate <= 0) {
		DPRINTF(sc, "sampling_rate is below or equal to 0, "
		    "can't reset callout\n");
		return (EINVAL);
	}

	if (sc->callout_setup)
		callout_reset(&sc->periodic_callout, hz / sc->sampling_rate, iichid_intr, sc);
	else
		return (EINVAL);
	return (0);
}

static void
iichid_teardown_callout(struct iichid_softc *sc)
{
	callout_stop(&sc->periodic_callout);
	sc->callout_setup=false;
	DPRINTF(sc, "tore callout down\n");
}

static int
iichid_setup_interrupt(struct iichid_softc *sc)
{
	sc->irq_cookie = 0;

	int error = bus_setup_intr(sc->dev, sc->irq_res,
	    INTR_TYPE_TTY | INTR_MPSAFE, NULL, iichid_intr, sc, &sc->irq_cookie);
	if (error != 0) {
		DPRINTF(sc, "Could not setup interrupt handler\n");
		return error;
	} else
		DPRINTF(sc, "successfully setup interrupt\n");

	return (0);
}

static void
iichid_teardown_interrupt(struct iichid_softc *sc)
{
	if (sc->irq_cookie)
		bus_teardown_intr(sc->dev, sc->irq_res, sc->irq_cookie);

	sc->irq_cookie = 0;
}

static int
iichid_sysctl_sampling_rate_handler(SYSCTL_HANDLER_ARGS)
{
	int err, value, oldval;
	struct iichid_softc *sc;

	sc = arg1;      

	mtx_lock(sc->intr_mtx);

	value = sc->sampling_rate;
	oldval = sc->sampling_rate;
	err = sysctl_handle_int(oidp, &value, 0, req);

	if (err != 0 || req->newptr == NULL || value == sc->sampling_rate) {
		mtx_unlock(sc->intr_mtx);
		return (err);
	}

	/* Can't switch to interrupt mode if it is not supported */
	if (sc->irq_res == NULL && value < 0) {
		mtx_unlock(sc->intr_mtx);
		return (EINVAL);
	}

	sc->sampling_rate = value;

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

void
iichid_intr_setup(device_t dev, struct mtx *mtx, iichid_intr_t intr,
    void *context)
{
	struct iichid_softc* sc = device_get_softc(dev);

	sc->intr_handler = intr;
	sc->intr_context = context;
	sc->intr_mtx = mtx;
	callout_init_mtx(&sc->periodic_callout, sc->intr_mtx, 0);
	taskqueue_start_threads(&sc->taskqueue, 1, PI_TTY,
	    "%s taskq", device_get_nameunit(sc->dev));
}

void
iichid_intr_unsetup(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);

	taskqueue_drain_all(sc->taskqueue);
}

int
iichid_intr_start(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);

	mtx_assert(sc->intr_mtx, MA_OWNED);

	DPRINTF(sc, "iichid device open\n");

	sc->open = true;
	taskqueue_enqueue(sc->taskqueue, &sc->power_task);

	return (0);
}

int
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

/*
 * HID interface
 */
int
iichid_get_report_desc(device_t dev, void **buf, int *len)
{
	struct iichid_softc* sc = device_get_softc(dev);

	*buf = sc->rep_desc;
	*len = le16toh(sc->desc.wReportDescLength);

	return (0);
}

int
iichid_get_input_report(device_t dev, void *buf, int len)
{
	struct iichid_softc* sc = device_get_softc(dev);
	int actlen;

	return (iic2errno(iichid_cmd_get_input_report(sc,
	    buf, len, &actlen, false)));
}

int
iichid_set_output_report(device_t dev, void *buf, int len)
{
	struct iichid_softc* sc = device_get_softc(dev);

	return (iic2errno(iichid_cmd_set_output_report(sc, buf, len)));
}

int
iichid_get_report(device_t dev, void *buf, int len, uint8_t type, uint8_t id)
{
	struct iichid_softc* sc = device_get_softc(dev);

	return (iic2errno(iichid_cmd_get_report(sc, buf, len, type, id)));
}

int
iichid_set_report(device_t dev, void *buf, int len, uint8_t type, uint8_t id)
{
	struct iichid_softc* sc = device_get_softc(dev);

	return (iic2errno(iichid_cmd_set_report(sc, buf, len, type, id)));
}

static int
iichid_attach(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);
	ACPI_HANDLE handle;
	ACPI_STATUS status;
	ACPI_DEVICE_INFO *device_info;
	uint16_t addr = iicbus_get_addr(dev);
	int error;

	/* Fetch hardware settings from ACPI */
#ifdef HAVE_ACPI_IICBUS
	handle = acpi_get_handle(dev);
#else
	handle = iichid_get_handle(dev);
#endif
	if (handle == NULL)
		return (ENXIO);

	/* get ACPI HID. It is a base part of the evdev device name */
	status = AcpiGetObjectInfo(handle, &device_info);
	if (ACPI_FAILURE(status)) {
		device_printf(dev, "error evaluating AcpiGetObjectInfo\n");
		return (ENXIO);
	}

	if (device_info->Valid & ACPI_VALID_HID)
		strlcpy(sc->hw.hid, device_info->HardwareId.String,
		    sizeof(sc->hw.hid));

	AcpiOsFree(device_info);

	DPRINTF(sc, "  ACPI Hardware ID  : %s\n", sc->hw.hid);
	DPRINTF(sc, "  IICbus addr       : 0x%02X\n", addr);
	DPRINTF(sc, "  HID descriptor reg: 0x%02X\n", sc->config_reg);

	sc->hw.idVendor = le16toh(sc->desc.wVendorID);
	sc->hw.idProduct = le16toh(sc->desc.wProductID);
	sc->hw.idVersion = le16toh(sc->desc.wVersionID);

	error = iichid_set_power(dev, true);
	if (error) {
		device_printf(dev, "failed to power on: %d\n", error);
		return (ENXIO);
	}
	/*
	 * Windows driver sleeps for 1ms between the SET_POWER and RESET
	 * commands. So we too as some devices may depend on this.
	 */
	if (cold)
		DELAY(1000);
	else
		tsleep(sc, 0, "iichid", (hz + 999) / 1000);

	error = iichid_reset(dev);
	if (error) {
		device_printf(dev, "failed to reset hardware: %d\n", error);
		return (ENXIO);
	}

	sc->rep_desc = malloc(le16toh(sc->desc.wReportDescLength), M_DEVBUF,
	    M_WAITOK | M_ZERO);
	error = iichid_cmd_get_report_desc(sc, sc->rep_desc,
	    le16toh(sc->desc.wReportDescLength));
	if (error) {
		device_printf(dev, "failed to fetch report descriptor: %d\n",
		    error);
		free (sc->rep_desc, M_TEMP);
		return (ENXIO);
	}

	/*
	 * Do not rely on wMaxInputLength, as some devices may set it to
	 * a wrong length. Traverse report descriptor and find the longest.
	 */
	sc->isize = hid_report_size(sc->rep_desc,
	     le16toh(sc->desc.wReportDescLength), hid_input, &sc->iid);
	if (sc->isize != le16toh(sc->desc.wMaxInputLength) - 2)
		DPRINTF(sc, "determined (len=%d) and described (len=%d)"
		    " input report lengths mismatch\n",
		    sc->isize, le16toh(sc->desc.wMaxInputLength) - 2);
	sc->ibuf = malloc(sc->isize, M_DEVBUF, M_WAITOK | M_ZERO);

	sx_init(&sc->lock, device_get_nameunit(dev));

	sc->power_on = false;
	TASK_INIT(&sc->event_task, 0, iichid_event_task, sc);
	TASK_INIT(&sc->power_task, 0, iichid_power_task, sc);
	/* taskqueue_create can't fail with M_WAITOK mflag passed */
	sc->taskqueue = taskqueue_create("imt_tq", M_WAITOK | M_ZERO,
	    taskqueue_thread_enqueue, &sc->taskqueue);

	sc->irq_rid = 0;
	sc->sampling_rate = -1;
	sc->irq_res = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ, &sc->irq_rid, RF_ACTIVE);

	if (sc->irq_res != NULL) {
		DPRINTF(sc, "allocated irq at 0x%lx and rid %d\n",
		    (uint64_t)sc->irq_res, sc->irq_rid);
	} else {
		DPRINTF(sc, "IRQ allocation failed. Fallback to sampling.\n");
		sc->sampling_rate = IICHID_DEFAULT_SAMPLING_RATE;
	}

	if (sc->sampling_rate < 0) {
		error = iichid_setup_interrupt(sc);
		if (error != 0) {
			device_printf(sc->dev,
			    "Interrupt setup failed. Fallback to sampling.\n");
			sc->sampling_rate = IICHID_DEFAULT_SAMPLING_RATE;
		}
	}
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(sc->dev),
		SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)),
		OID_AUTO, "sampling_rate", CTLTYPE_INT | CTLFLAG_RWTUN,
		sc, 0,
		iichid_sysctl_sampling_rate_handler, "I", "sampling rate in num/second");

	sc->child = device_add_child(dev, NULL, -1);
	if (sc->child == NULL) {
		device_printf(sc->dev, "Could not add I2C device\n");
		error = ENXIO;
		goto done;
	}

	device_set_ivars(sc->child, &sc->hw);
	error = bus_generic_attach(dev);
	if (error)
		device_printf(dev, "failed to attach child: error %d\n", error);

done:
	(void)iichid_set_power(dev, false);
	return (error);
}

static int
iichid_probe(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);
	ACPI_HANDLE handle;
	uint16_t addr = iicbus_get_addr(dev);
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
	sc->ibuf = NULL;

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

	sc->probe_result = BUS_PROBE_DEFAULT;
	return (sc->probe_result);
}

static int
iichid_detach(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);

	if (device_is_attached(dev))
		bus_generic_detach(dev);
	if (sc->child)
		device_delete_child(dev, sc->child);

	iichid_teardown_interrupt(sc);

	if (sc->irq_res)
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid,
		    sc->irq_res);

	if (sc->taskqueue)
		taskqueue_free(sc->taskqueue);
	sc->taskqueue = NULL;

	free(sc->rep_desc, M_DEVBUF);
	free(sc->ibuf, M_DEVBUF);

	sx_destroy(&sc->lock);

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

	/*
	 * 8.2 - The HOST is going into a deep power optimized state and wishes
	 * to put all the devices into a low power state also. The HOST
	 * is recommended to issue a HIPO command to the DEVICE to force
	 * the DEVICE in to a lower power state
         */
	sx_xlock(&sc->lock);
	sc->suspend = true;
	error = iichid_set_power_state(sc);
	sx_unlock(&sc->lock);

	if (error != 0)
		DPRINTF(sc, "Could not set power_state, error: %d\n", error);
	else
		DPRINTF(sc, "Successfully set power_state\n");

        return (error);
}

static int
iichid_resume(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);
	int error;

	DPRINTF(sc, "Resume called, setting device to power_state 0\n");

	sx_xlock(&sc->lock);
	sc->suspend = false;
	error = iichid_set_power_state(sc);
	sx_unlock(&sc->lock);

	if (error != 0)
		DPRINTF(sc, "Could not set power_state, error: %d\n", error);
	else
		DPRINTF(sc, "Successfully set power_state\n");

	return (error);
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
MODULE_DEPEND(iichid, usb, 1, 1, 1);
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
