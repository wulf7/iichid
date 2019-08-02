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

static int
iichid_get_hw(ACPI_HANDLE handle, struct iichid_hw *hw)
{
	ACPI_OBJECT *result;
	ACPI_OBJECT_LIST acpi_arg;
	ACPI_BUFFER acpi_buf;
	ACPI_STATUS status;
	ACPI_DEVICE_INFO *device_info;

	/*
	 * function (_DSM) to be evaluated to retrieve the address of
	 * the configuration register of the HID device
	 */
	/* 3cdff6f7-4267-4555-ad05-b30a3d8938de */
	static uint8_t dsm_guid[ACPI_UUID_LENGTH] = {
		0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
		0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE,
	};

	/* prepare 4 arguments */
	static ACPI_OBJECT args[] = {{
		.Buffer.Type = ACPI_TYPE_BUFFER,
		.Buffer.Length = ACPI_UUID_LENGTH,
		.Buffer.Pointer = dsm_guid,
	}, {
		.Integer.Type = ACPI_TYPE_INTEGER,
		.Integer.Value = 1,
	}, {
		.Integer.Type = ACPI_TYPE_INTEGER,
		.Integer.Value = 1,
	}, {
		.Package.Type = ACPI_TYPE_PACKAGE,
		.Package.Count = 0,
	}};

	/* Evaluate _DSM method to obtain HID Descriptor address */
	acpi_arg.Pointer = args;
	acpi_arg.Count = nitems(args);

	acpi_buf.Pointer = NULL;
	acpi_buf.Length = ACPI_ALLOCATE_BUFFER;

	status = AcpiEvaluateObject(handle, "_DSM", &acpi_arg, &acpi_buf);
	if (ACPI_FAILURE(status)) {
		printf("%s: error evaluating _DSM\n", __func__);
		if (acpi_buf.Pointer != NULL)
			AcpiOsFree(acpi_buf.Pointer);
		return (status);
	}

	/* the result will contain the register address (int type) */
	result = (ACPI_OBJECT *) acpi_buf.Pointer;
	if (result->Type != ACPI_TYPE_INTEGER) {
		printf("%s: _DSM should return descriptor register address "
		    "as integer\n", __func__);
		AcpiOsFree(result);
		return (AE_TYPE);
	}

	/* take it (much work done for one byte -.-) */
	hw->config_reg = result->Integer.Value;

	AcpiOsFree(result);

	/* get ACPI HID. It is a base part of the evdev device name */
	status = AcpiGetObjectInfo(handle, &device_info);
	if (ACPI_FAILURE(status)) {
		printf("%s: error evaluating AcpiGetObjectInfo\n", __func__);
		return (status);
	}

	if (device_info->Valid & ACPI_VALID_HID)
		strlcpy(hw->hid, device_info->HardwareId.String,
		    sizeof(hw->hid));

	AcpiOsFree(device_info);

	return (AE_OK);
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

static int
iichid_fetch_buffer(device_t dev, void* cmd, int cmdlen, void *buf, int buflen)
{
	uint16_t addr = iicbus_get_addr(dev);
	struct iic_msg msgs[] = {
	    { addr << 1, IIC_M_WR | IIC_M_NOSTOP, cmdlen, cmd },
	    { addr << 1, IIC_M_RD, buflen, buf },
	};

	return (iicbus_transfer(dev, msgs, nitems(msgs)));
}

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
iichid_fetch_input_report(struct iichid_softc* sc, uint8_t *data, int len,
    int *actual_len)
{
	uint16_t cmd = sc->desc.wInputRegister;
	int cmdlen = sizeof(cmd);
	uint8_t buf[len];

	int error = iichid_fetch_buffer(sc->dev, &cmd, cmdlen, buf, len);
	if (error != 0) {
		device_printf(sc->dev, "could not retrieve input report (%d)\n", error);
		return (error);
	}

	memcpy(data, buf, len);
	*actual_len = data[0] | data[1] << 8;

	return (0);
}

static int
iichid_fetch_hid_descriptor(device_t dev, uint16_t cmd, struct i2c_hid_desc *hid_desc)
{

	return (iichid_fetch_buffer(dev, &cmd, sizeof(cmd), hid_desc, sizeof(struct i2c_hid_desc)));
}

int
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
	int error;

	DPRINTF(sc, "HID command I2C_HID_CMD_SET_POWER(%d)\n", power_state);

	error = iichid_write_register(dev, cmd, sizeof(cmd));

	return (error);
}

int
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
	int error;

	DPRINTF(sc, "HID command I2C_HID_CMD_RESET\n");

	error = iichid_write_register(dev, cmd, sizeof(cmd));

	return (error);
}

int
iichid_get_report_desc(device_t dev, void **buf, int *len)
{
	struct iichid_softc* sc = device_get_softc(dev);
	int error;
	uint16_t cmd = sc->desc.wReportDescRegister;
	uint8_t *tmpbuf;

	DPRINTF(sc, "HID command I2C_HID_REPORT_DESCR at 0x%x with size %d\n",
	    le16toh(cmd), le16toh(sc->desc.wReportDescLength));

	tmpbuf = malloc(sc->desc.wReportDescLength, M_TEMP, M_WAITOK | M_ZERO);
	error = iichid_fetch_buffer(dev, &cmd, sizeof(cmd), tmpbuf,
	    le16toh(sc->desc.wReportDescLength));
	if (error) {
		free (tmpbuf, M_TEMP);
		device_printf(dev, "could not retrieve report descriptor "
		    "(%d)\n", error);
		return (error);
	}

	*buf = tmpbuf;
	*len = le16toh(sc->desc.wReportDescLength);

	DPRINTF(sc, "HID report descriptor: %*D\n", *len, tmpbuf, " ");

	/*
	 * Do not rely on wMaxInputLength, as some devices may set it to
	 * a wrong length. So traverse report descriptor and find the longest.
	 */
	sc->isize = hid_report_size(tmpbuf, *len, hid_input, &sc->iid) + 2;
	if (sc->isize != le16toh(sc->desc.wMaxInputLength))
		DPRINTF(sc, "determined (len=%d) and described (len=%d)"
		    " input report lengths mismatch\n",
		    sc->isize, le16toh(sc->desc.wMaxInputLength));

	return (0);
}

int
iichid_get_report(device_t dev, void *buf, int len, uint8_t type, uint8_t id)
{
	/*
	 * 7.2.2.4 - "The protocol is optimized for Report < 15.  If a
	 * report ID >= 15 is necessary, then the Report ID in the Low Byte
	 * must be set to 1111 and a Third Byte is appended to the protocol.
	 * This Third Byte contains the entire/actual report ID."
	 */
	struct iichid_softc* sc = device_get_softc(dev);
	uint16_t dtareg = htole16(sc->desc.wDataRegister);
	uint16_t cmdreg = htole16(sc->desc.wCommandRegister);
	uint8_t cmd[] =	{   /*________|______id>=15_____|______id<15______*/
						  cmdreg & 0xff		   ,
						   cmdreg >> 8		   ,
			    (id >= 15 ? 15 | (type << 4): id | (type << 4)),
					      I2C_HID_CMD_GET_REPORT	   ,
			    (id >= 15 ?		id	:   dtareg & 0xff ),
			    (id >= 15 ?   dtareg & 0xff	:   dtareg >> 8   ),
			    (id >= 15 ?   dtareg >> 8	:	0	  ),
			};
	int cmdlen    =	    (id >= 15 ?		7	:	6	  );
	int report_id_len = (id >= 15 ?		2	:	1	  );
	int report_len = len + 2;
	int d, err;
	uint8_t *tmprep;

	DPRINTF(sc, "HID command I2C_HID_CMD_GET_REPORT %d "
	    "(type %d, len %d)\n", id, type, len);

	/*
	 * 7.2.2.2 - Response will be a 2-byte length value, the report
	 * id with length determined above, and then the report.
	 * Allocate len + 2 + 2 bytes, read into that temporary
	 * buffer, and then copy only the report back out to buf.
	 */
	report_len += report_id_len;
	tmprep = malloc(report_len, M_DEVBUF, M_WAITOK | M_ZERO);

	/* type 3 id 8: 22 00 38 02 23 00 */
	err = iichid_fetch_buffer(dev, &cmd, cmdlen, tmprep, report_len);
	if (err != 0) {
		free(tmprep, M_DEVBUF);
		return (EIO);
	}

	d = tmprep[0] | tmprep[1] << 8;
	if (d != report_len)
		DPRINTF(sc, "response size %d != expected length %d\n",
		    d, report_len);

	if (report_id_len == 2)
		d = tmprep[2] | tmprep[3] << 8;
	else
		d = tmprep[2];

	if (d != id) {
		DPRINTF(sc, "response report id %d != %d\n", d, id);
		free(tmprep, M_DEVBUF);
		return (EBADMSG);
	}

	DPRINTF(sc, "response: %*D\n", report_len, tmprep, " ");

	memcpy(buf, tmprep + 2 + report_id_len, len);
	free(tmprep, M_DEVBUF);

	return (0);
}

int
iichid_set_report(device_t dev, void *buf, int len, uint8_t type, uint8_t id)
{
	/*
	 * 7.2.2.4 - "The protocol is optimized for Report < 15.  If a
	 * report ID >= 15 is necessary, then the Report ID in the Low Byte
	 * must be set to 1111 and a Third Byte is appended to the protocol.
	 * This Third Byte contains the entire/actual report ID."
	 */
	struct iichid_softc* sc = device_get_softc(dev);
	uint16_t dtareg = htole16(type == I2C_HID_REPORT_TYPE_FEATURE ?
	    sc->desc.wDataRegister : sc->desc.wOutputRegister);
	uint16_t cmdreg = htole16(sc->desc.wCommandRegister);
	uint16_t replen = htole16(2 + (id != 0 ? 1 : 0) + len);
	uint8_t cmd[] =	{   /*________|______id>=15_____|______id<15______*/
						  cmdreg & 0xff		   ,
						   cmdreg >> 8		   ,
			    (id >= 15 ? 15 | (type << 4): id | (type << 4)),
					      I2C_HID_CMD_SET_REPORT	   ,
			    (id >= 15 ?		id	:   dtareg & 0xff ),
			    (id >= 15 ?   dtareg & 0xff	:   dtareg >> 8   ),
			    (id >= 15 ?   dtareg >> 8	:   replen & 0xff ),
			    (id >= 15 ?   replen & 0xff	:   replen >> 8   ),
			    (id >= 15 ?   replen >> 8	:	id	  ),
			    (id >= 15 ?		id	:	0	  ),
			};
	int cmdlen    =	    (id >= 15 ?		10	:	9	  );
	int error;
	uint8_t *finalcmd;

	DPRINTF(sc, "HID command I2C_HID_CMD_SET_REPORT %d (type %d, len %d): "
	    "%*D\n", id, type, len, len, buf, " ");

	finalcmd = malloc(cmdlen + len, M_DEVBUF, M_WAITOK | M_ZERO);

	memcpy(finalcmd, cmd, cmdlen);
	memcpy(finalcmd + cmdlen, buf, len);

	/* type 3 id 4: 22 00 34 03 23 00 04 00 04 03 */
	error = iichid_write_register(dev, finalcmd, cmdlen + len);

	free(finalcmd, M_DEVBUF);

	return (error);
}

static void
iichid_intr(void *context)
{
	struct iichid_softc *sc = context;

	taskqueue_enqueue(sc->taskqueue, &sc->event_task);

	if (sc->callout_setup && sc->sampling_rate > 0 && sc->open)
		callout_reset(&sc->periodic_callout, hz / sc->sampling_rate,
		    iichid_intr, sc);
}

static void
iichid_event_task(void *context, int pending)
{
	struct iichid_softc *sc = context;
	int actual = 0;
	int error;
	int report_id;
	int header_len = sc->iid != 0 ? 3 : 2;

	error = iichid_fetch_input_report(sc, sc->ibuf, sc->isize, &actual);
	if (error != 0) {
		device_printf(sc->dev, "an error occured\n");
		return;
	}

	if (actual <= header_len) {
//		device_printf(sc->dev, "no data received\n");
		return;
	}

	report_id = sc->iid > 0 ? ((uint8_t *)sc->ibuf)[2] : 0;

	/* DPRINTF(sc, "%*D\n", actual, sc->ibuf, " "); */
	mtx_lock(sc->intr_mtx);
	if (sc->open)
		sc->intr_handler(sc->intr_context, (uint8_t *)sc->ibuf +
		    header_len, actual - header_len, report_id);
	mtx_unlock(sc->intr_mtx);
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
iichid_set_intr(device_t dev, struct mtx *mtx, iichid_intr_t intr,
    void *context)
{
	struct iichid_softc* sc = device_get_softc(dev);

	sc->intr_handler = intr;
	sc->intr_context = context;
	sc->intr_mtx = mtx;
}

int
iichid_open(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);

	mtx_assert(sc->intr_mtx, MA_OWNED);

	DPRINTF(sc, "iichid device open\n");

	sc->open = true;
	taskqueue_enqueue(sc->taskqueue, &sc->power_task);

	return (0);
}

int
iichid_close(device_t dev)
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

int
iichid_attach(device_t dev)
{
	struct iichid_softc* sc = device_get_softc(dev);
	int error;

	sx_init(&sc->lock, device_get_nameunit(dev));

	sc->power_on = false;
	(void)iichid_set_power(dev, false);

	if (sc->intr_handler == NULL)
		return (0);

	TASK_INIT(&sc->event_task, 0, iichid_event_task, sc);
	TASK_INIT(&sc->power_task, 0, iichid_power_task, sc);
	/* taskqueue_create can't fail with M_WAITOK mflag passed */
	sc->taskqueue = taskqueue_create("imt_tq", M_WAITOK | M_ZERO,
	    taskqueue_thread_enqueue, &sc->taskqueue);

	callout_init_mtx(&sc->periodic_callout, sc->intr_mtx, 0);

	/*
	 * Fallback to HID descriptor input length
	 * if report descriptor has not been fetched yet
	 */
	if (sc->isize == 0)
		sc->isize = le16toh(sc->desc.wMaxInputLength);

	sc->ibuf = malloc(sc->isize, M_DEVBUF, M_WAITOK | M_ZERO);
	taskqueue_start_threads(&sc->taskqueue, 1, PI_TTY, "%s taskq", device_get_nameunit(sc->dev));

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

	return (0);
}

int
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

	if (addr == 0)
		return (ENXIO);

	sc->dev = dev;
	sc->ibuf = NULL;

	/* Fetch hardware settings from ACPI */
	handle = iichid_get_handle(dev);
	if (handle == NULL)
		return (ENXIO);

	if (ACPI_FAILURE(iichid_get_hw(handle, &sc->hw)))
		return (ENXIO);

	DPRINTF(sc, "  ACPI Hardware ID  : %s\n", sc->hw.hid);
	DPRINTF(sc, "  IICbus addr       : 0x%02X\n", sc->hw.device_addr);
	DPRINTF(sc, "  HID descriptor reg: 0x%02X\n", sc->hw.config_reg);

	error = iichid_fetch_hid_descriptor(dev, sc->hw.config_reg, &sc->desc);
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
		(void)iichid_set_power(dev, false);
		return (ENXIO);
	}

	sc->probe_result = BUS_PROBE_DEFAULT;
	return (sc->probe_result);
}

int
iichid_detach(device_t dev)
{
	struct iichid_softc *sc = device_get_softc(dev);

	if (sc->intr_handler == NULL)
		return(0);

	if (sc->ibuf)
		free(sc->ibuf, M_DEVBUF);

	iichid_teardown_interrupt(sc);

	if (sc->irq_res)
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid,
		    sc->irq_res);

	if (sc->taskqueue) {
		taskqueue_block(sc->taskqueue);
		taskqueue_drain_all(sc->taskqueue);
		taskqueue_free(sc->taskqueue);
	}

	sx_destroy(&sc->lock);

	return (0);
}

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

void
iichid_identify(driver_t *driver, device_t parent)
{
	ACPI_HANDLE	ctrl_handle;

	ctrl_handle = acpi_get_handle(device_get_parent(parent));
	AcpiWalkNamespace(ACPI_TYPE_DEVICE, ctrl_handle,
	    1, iichid_identify_cb, NULL, parent, NULL);
}

int
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

int
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

MODULE_DEPEND(iichid, acpi, 1, 1, 1);
MODULE_DEPEND(iichid, usb, 1, 1, 1);
MODULE_VERSION(iichid, 1);

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
