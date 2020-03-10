/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
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
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/systm.h>

#include "hid.h"
#include "hidbus.h"

#include "hid_if.h"

#include "strcasestr.h"

#define	HID_DEBUG_VAR	hid_debug
#include "hid_debug.h"

#if __FreeBSD_version >= 1300067
#define HAVE_BUS_DELAYED_ATTACH_CHILDREN
#endif

static hid_intr_t	hidbus_intr;

static device_probe_t	hidbus_probe;
static device_attach_t	hidbus_attach;
static device_detach_t	hidbus_detach;

struct hidbus_softc {
	device_t			dev;
	struct mtx			*lock;
	struct mtx			mtx;

	STAILQ_HEAD(, hidbus_ivars)	tlcs;
};

static device_t
hidbus_add_child(device_t dev, u_int order, const char *name, int unit)
{
	struct hidbus_softc *sc = device_get_softc(dev);
	struct hidbus_ivars *tlc;
	device_t child;

	child = device_add_child_ordered(dev, order, name, unit);
	if (child == NULL)
			return (child);

	tlc = malloc(sizeof(struct hidbus_ivars), M_DEVBUF, M_WAITOK | M_ZERO);
	tlc->child = child;
	device_set_ivars(child, tlc);
	mtx_lock(sc->lock);
	STAILQ_INSERT_TAIL(&sc->tlcs, tlc, link);
	mtx_unlock(sc->lock);

	return (child);
}

static int
hidbus_enumerate_children(device_t dev, void* data, uint16_t len)
{
	struct hid_data *hd;
	struct hid_item hi;
	device_t child;
	uint8_t index = 0;

	/* Add a child for each top level collection */
	hd = hid_start_parse(data, len, 1 << hid_input);
	while (hid_get_item(hd, &hi)) {
		if (hi.kind != hid_collection || hi.collevel != 1)
			continue;
		child = BUS_ADD_CHILD(dev, 0, NULL, -1);
		if (child == NULL) {
			device_printf(dev, "Could not add HID device\n");
			continue;
		}
		hidbus_set_index(child, index);
		hidbus_set_usage(child, hi.usage);
		index++;
		DPRINTF("Add child TLC: 0x%04hx:0x%04hx\n",
		    (uint16_t)(hi.usage >> 16), (uint16_t)(hi.usage & 0xFFFF));
	}
	hid_end_parse(hd);

	if (index == 0)
		return (ENXIO);

	return (0);
}

static int
hidbus_probe(device_t dev)
{

	device_set_desc(dev, "HID bus");

	/* Allow other subclasses to override this driver. */
	return (BUS_PROBE_GENERIC);
}

static int
hidbus_attach(device_t dev)
{
	struct hidbus_softc *sc = device_get_softc(dev);
	void *d_ptr;
	uint16_t d_len;
	int error;
	bool is_keyboard;

	sc->dev = dev;
	STAILQ_INIT(&sc->tlcs);

	if (HID_GET_REPORT_DESCR(device_get_parent(dev), &d_ptr, &d_len) != 0)
		return (ENXIO);
	is_keyboard = hid_is_keyboard(d_ptr, d_len) != 0;

	mtx_init(&sc->mtx, "hidbus lock", NULL, MTX_DEF);
	sc->lock = is_keyboard ? HID_SYSCONS_MTX : &sc->mtx;

	HID_INTR_SETUP(device_get_parent(dev), sc->lock, hidbus_intr, sc);

	error = hidbus_enumerate_children(dev, d_ptr, d_len);
	if (error != 0) {
		hidbus_detach(dev);
		return (ENXIO);
	}

	if (is_keyboard)
		error = bus_generic_attach(dev);
	else
#ifdef HAVE_BUS_DELAYED_ATTACH_CHILDREN
		error = bus_delayed_attach_children(dev);
#else
		config_intrhook_oneshot((ich_func_t)bus_generic_attach, dev);
#endif
	if (error != 0)
		device_printf(dev, "failed to attach child: error %d\n", error);

	return (0);
}

static int
hidbus_detach(device_t dev)
{
	struct hidbus_softc *sc = device_get_softc(dev);

	bus_generic_detach(dev);
	device_delete_children(dev);

	HID_INTR_UNSETUP(device_get_parent(dev));
	mtx_destroy(&sc->mtx);

	return (0);
}

static void
hidbus_child_deleted(device_t bus, device_t child)
{
	struct hidbus_softc *sc = device_get_softc(bus);
	struct hidbus_ivars *tlc = device_get_ivars(child);

	KASSERT(!tlc->open, ("Child device is running"));

	mtx_lock(sc->lock);
	STAILQ_REMOVE(&sc->tlcs, tlc, hidbus_ivars, link);
	mtx_unlock(sc->lock);
	free(tlc, M_DEVBUF);
}

static int
hidbus_read_ivar(device_t bus, device_t child, int which, uintptr_t *result)
{
	struct hidbus_ivars *tlc = device_get_ivars(child);

	switch (which) {
	case HIDBUS_IVAR_INDEX:
		*result = tlc->index;
		break;
	case HIDBUS_IVAR_USAGE:
		*result = tlc->usage;
		break;
	case HIDBUS_IVAR_INTR:
		*result = (uintptr_t)tlc->intr;
		break;
	case HIDBUS_IVAR_DRIVER_INFO:
		*result = tlc->driver_info;
		break;
	default:
		return (EINVAL);
	}
        return (0);
}

static int
hidbus_write_ivar(device_t bus, device_t child, int which, uintptr_t value)
{
	struct hidbus_ivars *tlc = device_get_ivars(child);

	switch (which) {
	case HIDBUS_IVAR_INDEX:
		tlc->index = value;
		break;
	case HIDBUS_IVAR_USAGE:
		tlc->usage = value;
		break;
	case HIDBUS_IVAR_INTR:
		tlc->intr = (hid_intr_t *)value;
		break;
	case HIDBUS_IVAR_DRIVER_INFO:
		tlc->driver_info = value;
		break;
	default:
		return (EINVAL);
	}
        return (0);
}

/* Location hint for devctl(8) */
static int
hidbus_child_location_str(device_t bus, device_t child, char *buf,
    size_t buflen)
{
	struct hidbus_ivars *tlc = device_get_ivars(child);

	snprintf(buf, buflen, "index=%hhu", tlc->index);
        return (0);
}

/* PnP information for devctl(8) */
static int
hidbus_child_pnpinfo_str(device_t bus, device_t child, char *buf,
    size_t buflen)
{
	struct hidbus_ivars *tlc = device_get_ivars(child);
	struct hid_device_info *devinfo = device_get_ivars(bus);

	snprintf(buf, buflen, "page=0x%04x usage=0x%04x bus=0x%02hx "
	    "vendor=0x%04hx product=0x%04hx version=0x%04hx",
	    tlc->usage >> 16, tlc->usage & 0xFFFF, devinfo->idBus,
	    devinfo->idVendor, devinfo->idProduct, devinfo->idVersion);
	return (0);
}

struct mtx *
hidbus_get_lock(device_t child)
{
	struct hidbus_softc *sc = device_get_softc(device_get_parent(child));

	return (sc->lock);
}

void
hidbus_set_desc(device_t child, const char *suffix)
{
	device_t bus = device_get_parent(child);
	struct hid_device_info *devinfo = device_get_ivars(bus);
	char buf[80];

	/* Do not add NULL suffix or if device name already contains it. */
	if (suffix != NULL && strcasestr(devinfo->name, suffix) == NULL) {
		snprintf(buf, sizeof(buf), "%s %s", devinfo->name, suffix);
		device_set_desc_copy(child, buf);
	} else
		device_set_desc(child, devinfo->name);
}

void
hidbus_intr(void *context, void *buf, uint16_t len)
{
	struct hidbus_softc *sc = context;
	struct hidbus_ivars *tlc;

	mtx_assert(sc->lock, MA_OWNED);

	/*
	 * Broadcast input report to all subscribers.
	 * TODO: Add check for input report ID.
	 */
	 STAILQ_FOREACH(tlc, &sc->tlcs, link) {
		if (tlc->open) {
			KASSERT(tlc->intr != NULL,
			    ("hidbus: interrupt handler is NULL"));
			tlc->intr(tlc->child, buf, len);
		}
	}
}

int
hidbus_intr_start(device_t child)
{
	device_t bus = device_get_parent(child);
	struct hidbus_softc *sc = device_get_softc(bus);
	struct hidbus_ivars *tlc;
	bool open = false;

	mtx_assert(sc->lock, MA_OWNED);

	STAILQ_FOREACH(tlc, &sc->tlcs, link) {
		open = open || tlc->open;
		if (tlc->child == child)
			tlc->open = true;
	}

	if (open)
		return (0);

	return (HID_INTR_START(device_get_parent(bus)));
}

int
hidbus_intr_stop(device_t child)
{
	device_t bus = device_get_parent(child);
	struct hidbus_softc *sc = device_get_softc(bus);
	struct hidbus_ivars *tlc;
	bool open = false;

	mtx_assert(sc->lock, MA_OWNED);

	STAILQ_FOREACH(tlc, &sc->tlcs, link) {
		if (tlc->child == child)
			tlc->open = false;
		open = open || tlc->open;
	}

	if (open)
		return (0);

	return (HID_INTR_STOP(device_get_parent(bus)));
}

void
hidbus_intr_poll(device_t child)
{
	device_t bus = device_get_parent(child);

	HID_INTR_POLL(device_get_parent(bus));
}

/*
 * HID interface
 */
int
hid_get_report_descr(device_t bus, void **data, uint16_t *len)
{

	return (HID_GET_REPORT_DESCR(device_get_parent(bus), data, len));
}

int
hid_read(device_t bus, void *data, uint16_t maxlen, uint16_t *actlen)
{

	return (HID_READ(device_get_parent(bus), data, maxlen, actlen));
}

int
hid_write(device_t bus, void *data, uint16_t len)
{

	return (HID_WRITE(device_get_parent(bus), data, len));
}

int
hid_get_report(device_t bus, void *data, uint16_t maxlen, uint16_t *actlen,
    uint8_t type, uint8_t id)
{

	return (HID_GET_REPORT(device_get_parent(bus),
	    data, maxlen, actlen, type, id));
}

int
hid_set_report(device_t bus, void *data, uint16_t len, uint8_t type,
    uint8_t id)
{

	return (HID_SET_REPORT(device_get_parent(bus), data, len, type, id));
}

int
hid_set_idle(device_t bus, uint16_t duration, uint8_t id)
{

	return (HID_SET_IDLE(device_get_parent(bus), duration, id));
}

int
hid_set_protocol(device_t bus, uint16_t protocol)
{

	return (HID_SET_PROTOCOL(device_get_parent(bus), protocol));
}

static device_method_t hidbus_methods[] = {
	/* device interface */
	DEVMETHOD(device_probe,         hidbus_probe),
	DEVMETHOD(device_attach,        hidbus_attach),
	DEVMETHOD(device_detach,        hidbus_detach),
	DEVMETHOD(device_suspend,       bus_generic_suspend),
	DEVMETHOD(device_resume,        bus_generic_resume),

	/* bus interface */
	DEVMETHOD(bus_add_child,	hidbus_add_child),
	DEVMETHOD(bus_child_deleted,	hidbus_child_deleted),
	DEVMETHOD(bus_read_ivar,	hidbus_read_ivar),
	DEVMETHOD(bus_write_ivar,	hidbus_write_ivar),
	DEVMETHOD(bus_child_pnpinfo_str,hidbus_child_pnpinfo_str),
	DEVMETHOD(bus_child_location_str,hidbus_child_location_str),

	/* hid interface */
	DEVMETHOD(hid_get_report_descr,	hid_get_report_descr),
	DEVMETHOD(hid_read,		hid_read),
	DEVMETHOD(hid_write,		hid_write),
	DEVMETHOD(hid_get_report,       hid_get_report),
	DEVMETHOD(hid_set_report,       hid_set_report),
	DEVMETHOD(hid_set_idle,		hid_set_idle),
	DEVMETHOD(hid_set_protocol,	hid_set_protocol),

        DEVMETHOD_END
};


driver_t hidbus_driver = {
	"hidbus",
	hidbus_methods,
	sizeof(struct hidbus_softc),
};

devclass_t hidbus_devclass;

MODULE_VERSION(hidbus, 1);
DRIVER_MODULE(hidbus, usbhid, hidbus_driver, hidbus_devclass, 0, 0);
DRIVER_MODULE(hidbus, iichid, hidbus_driver, hidbus_devclass, 0, 0);
