/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
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

#define	HID_RSIZE_MAX	1024

static hid_intr_t	hidbus_intr;

static device_probe_t	hidbus_probe;
static device_attach_t	hidbus_attach;
static device_detach_t	hidbus_detach;

struct hidbus_softc {
	device_t			dev;
	struct mtx			*lock;
	struct mtx			mtx;

	struct hidbus_report_descr	rdesc;
	int				nest;	/* Child attach nesting lvl */

	STAILQ_HEAD(, hidbus_ivars)	tlcs;
};

devclass_t hidbus_devclass;

static int
hidbus_fill_report_descr(struct hidbus_report_descr *hrd, const void *data,
    hid_size_t len)
{
	int error = 0;

	hrd->data = __DECONST(void *, data);
	hrd->len = len;

	/*
	 * If report descriptor is not available yet, set maximal
	 * report sizes high enough to allow hidraw to work.
	 */
	hrd->isize = len == 0 ? HID_RSIZE_MAX :
	    hid_report_size(data, len, hid_input, &hrd->iid);
	hrd->osize = len == 0 ? HID_RSIZE_MAX :
	    hid_report_size(data, len, hid_output, &hrd->oid);
	hrd->fsize = len == 0 ? HID_RSIZE_MAX :
	    hid_report_size(data, len, hid_feature, &hrd->fid);

	if (hrd->isize > HID_RSIZE_MAX) {
		DPRINTF("input size is too large, %u bytes (truncating)\n",
		    hrd->isize);
		hrd->isize = HID_RSIZE_MAX;
		error = EOVERFLOW;
	}
	if (hrd->osize > HID_RSIZE_MAX) {
		DPRINTF("output size is too large, %u bytes (truncating)\n",
		    hrd->osize);
		hrd->osize = HID_RSIZE_MAX;
		error = EOVERFLOW;
	}
	if (hrd->fsize > HID_RSIZE_MAX) {
		DPRINTF("feature size is too large, %u bytes (truncating)\n",
		    hrd->fsize);
		hrd->fsize = HID_RSIZE_MAX;
		error = EOVERFLOW;
	}

	return (error);
}

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
hidbus_enumerate_children(device_t dev, const void* data, hid_size_t len)
{
	struct hid_data *hd;
	struct hid_item hi;
	device_t child;
	uint8_t index = 0;

	if (data == NULL || len == 0)
		return (ENXIO);

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
		DPRINTF("Add child TLC: 0x%04x:0x%04x\n",
		    HID_GET_USAGE_PAGE(hi.usage), HID_GET_USAGE(hi.usage));
	}
	hid_end_parse(hd);

	if (index == 0)
		return (ENXIO);

	return (0);
}

static int
hidbus_attach_children(device_t dev)
{
	struct hidbus_softc *sc = device_get_softc(dev);
	bool is_sc_kbd;
	int error;

	/* syscons(4)/vt(4) - compatible drivers must be run under Giant */
	is_sc_kbd = hid_is_keyboard(sc->rdesc.data, sc->rdesc.len) != 0;
	sc->lock = is_sc_kbd ? HID_SYSCONS_MTX : &sc->mtx;
	HID_INTR_SETUP(device_get_parent(dev), sc->lock, hidbus_intr, sc,
	    &sc->rdesc);

	error = hidbus_enumerate_children(dev, sc->rdesc.data, sc->rdesc.len);
	if (error != 0)
		DPRINTF("failed to enumerate children: error %d\n", error);

	/*
	 * hidbus_attach_children() can recurse through device_identify->
	 * hid_set_report_descr() call sequence. Do not perform children
	 * attach twice in that case.
	 */
	sc->nest++;
	bus_generic_probe(dev);
	sc->nest--;
	if (sc->nest != 0)
		return (0);

	if (is_sc_kbd)
		error = bus_generic_attach(dev);
	else
#ifdef HAVE_BUS_DELAYED_ATTACH_CHILDREN
		error = bus_delayed_attach_children(dev);
#else
		config_intrhook_oneshot((ich_func_t)bus_generic_attach, dev);
#endif
	if (error != 0)
		device_printf(dev, "failed to attach child: error %d\n", error);

	return (error);
}

static int
hidbus_detach_children(device_t dev)
{
	device_t *children, bus;
	bool is_bus;
	int i, error;

	error = 0;

	is_bus = device_get_devclass(dev) == hidbus_devclass;
	bus = is_bus ? dev : device_get_parent(dev);

	KASSERT(device_get_class(bus) == hidbus_devclass,
	    ("Device is not hidbus or it's child"));

	if (is_bus) {
		/* If hidbus is passed, delete all children. */
		bus_generic_detach(bus);
		device_delete_children(bus);
	} else {
		/*
		 * If hidbus child is passed, delete all hidbus children
		 * except caller. Deleting the caller may result in deadlock.
		 */
		error = device_get_children(bus, &children, &i);
		if (error != 0)
			return (error);
		while (i-- > 0) {
			if (children[i] == dev)
				continue;
			DPRINTF("Delete child. index=%d (%s)\n",
			    hidbus_get_index(children[i]),
			    device_get_nameunit(children[i]));
			error = device_delete_child(bus, children[i]);
			if (error) {
				DPRINTF("Failed deleting %s\n",
				    device_get_nameunit(children[i]));
				break;
			}
		}
		free(children, M_TEMP);
	}

	HID_INTR_UNSETUP(device_get_parent(bus));

	return (error);
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
	struct hid_device_info *devinfo = device_get_ivars(dev);
	device_t parent = device_get_parent(dev);
	void *d_ptr = NULL;
	hid_size_t d_len;
	int error;

	sc->dev = dev;
	STAILQ_INIT(&sc->tlcs);
	mtx_init(&sc->mtx, "hidbus lock", NULL, MTX_DEF);

	d_len = devinfo->rdescsize;
	if (d_len != 0) {
		d_ptr = malloc(d_len, M_DEVBUF, M_ZERO | M_WAITOK);
		error = HID_GET_REPORT_DESCR(parent, d_ptr, d_len);
		if (error != 0) {
			free(d_ptr, M_DEVBUF);
			d_len = 0;
			d_ptr = NULL;
		}
	}

	hidbus_fill_report_descr(&sc->rdesc, d_ptr, d_len);

	error = hidbus_attach_children(dev);
	if (error != 0) {
		hidbus_detach(dev);
		return (ENXIO);
	}

	return (0);
}

static int
hidbus_detach(device_t dev)
{
	struct hidbus_softc *sc = device_get_softc(dev);

	hidbus_detach_children(dev);
	mtx_destroy(&sc->mtx);
	free(sc->rdesc.data, M_DEVBUF);

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
	    HID_GET_USAGE_PAGE(tlc->usage), HID_GET_USAGE(tlc->usage),
	    devinfo->idBus, devinfo->idVendor, devinfo->idProduct,
	    devinfo->idVersion);
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
hidbus_intr(void *context, void *buf, hid_size_t len)
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

struct hidbus_report_descr *
hidbus_get_report_descr(device_t child)
{
	device_t bus = device_get_parent(child);
	struct hidbus_softc *sc = device_get_softc(bus);

	return (&sc->rdesc);
}

/*
 * HID interface.
 *
 * Hidbus as well as any hidbus child can be passed as first arg.
 */
int
hid_get_report_descr(device_t dev, void **data, hid_size_t *len)
{
	device_t bus;
	struct hidbus_softc *sc;

	bus = device_get_devclass(dev) == hidbus_devclass ?
	    dev : device_get_parent(dev);
	sc = device_get_softc(bus);

	/*
	 * Do not send request to a transport backend.
	 * Use cached report descriptor instead of it.
         */
	if (sc->rdesc.data == NULL || sc->rdesc.len == 0)
		return (ENXIO);

	if (data != NULL)
		*data = sc->rdesc.data;
	if (len != NULL)
		*len = sc->rdesc.len;

	return (0);
}

/*
 * Replace cached report descriptor with top level driver provided one.
 *
 * It deletes all hidbus children except caller and enumerates them again after
 * new descriptor has been registered. Currently it can not be called from
 * autoenumerated (by report's TLC) child device context as it results in child
 * duplication. To overcome this limitation hid_set_report_descr() should be
 * called from device_identify driver's handler with hidbus itself passed as
 * 'device_t dev' parameter.
 */
int
hid_set_report_descr(device_t dev, const void *data, hid_size_t len)
{
	struct hidbus_report_descr rdesc;
	device_t bus;
	struct hidbus_softc *sc;
	bool is_bus;
	int error;

	GIANT_REQUIRED;

	is_bus = device_get_devclass(dev) == hidbus_devclass;
	bus = is_bus ? dev : device_get_parent(dev);
	sc = device_get_softc(bus);

	/*
	 * Do not overload already overloaded report descriptor in
	 * device_identify handler. It causes infinite recursion loop.
	 */
	if (is_bus && sc->rdesc.overloaded)
		return(0);

	DPRINTFN(5, "len=%d\n", len);
	DPRINTFN(5, "data = %*D\n", len, data, " ");

	error = hidbus_fill_report_descr(&rdesc, data, len);
	if (error != 0)
		return (error);

	error = hidbus_detach_children(dev);
	if (error != 0)
		return(error);

	/* Make private copy to handle a case of dynamicaly allocated data. */
	rdesc.data = malloc(len, M_DEVBUF, M_ZERO | M_WAITOK);
	bcopy(data, rdesc.data, len);
	rdesc.overloaded = true;
	free(sc->rdesc.data, M_DEVBUF);
	bcopy(&rdesc, &sc->rdesc, sizeof(struct hidbus_report_descr));

	error = hidbus_attach_children(bus);

	return (error);
}

int
hid_read(device_t dev, void *data, hid_size_t maxlen, hid_size_t *actlen)
{

	return (HID_READ(device_get_parent(dev), data, maxlen, actlen));
}

int
hid_write(device_t dev, const void *data, hid_size_t len)
{
	struct hidbus_softc *sc;
	struct hid_device_info *devinfo;
	uint8_t id;

	if (device_get_devclass(dev) == hidbus_devclass) {
		devinfo = device_get_ivars(dev);
		/*
		 * Output interrupt endpoint is often optional. If HID device
		 * do not provide it, send reports via control pipe.
		 */
		if (devinfo->noWriteEp) {
			sc = device_get_softc(dev);
			/* try to extract the ID byte */
			id = (sc->rdesc.oid & (len > 0)) ?
			    ((const uint8_t*)data)[0] : 0;
			return (HID_SET_REPORT(device_get_parent(dev),
			   data, len, UHID_OUTPUT_REPORT, id));
		}
	}

	return (HID_WRITE(device_get_parent(dev), data, len));
}

int
hid_get_report(device_t dev, void *data, hid_size_t maxlen, hid_size_t *actlen,
    uint8_t type, uint8_t id)
{

	return (HID_GET_REPORT(device_get_parent(dev),
	    data, maxlen, actlen, type, id));
}

int
hid_set_report(device_t dev, const void *data, hid_size_t len, uint8_t type,
    uint8_t id)
{

	return (HID_SET_REPORT(device_get_parent(dev), data, len, type, id));
}

int
hid_set_idle(device_t dev, uint16_t duration, uint8_t id)
{

	return (HID_SET_IDLE(device_get_parent(dev), duration, id));
}

int
hid_set_protocol(device_t dev, uint16_t protocol)
{

	return (HID_SET_PROTOCOL(device_get_parent(dev), protocol));
}

const struct hid_device_info *
hid_get_device_info(device_t dev)
{
	device_t bus;

	bus = device_get_devclass(dev) == hidbus_devclass ?
	    dev : device_get_parent(dev);

	return (device_get_ivars(bus));
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

MODULE_VERSION(hidbus, 1);
DRIVER_MODULE(hidbus, usbhid, hidbus_driver, hidbus_devclass, 0, 0);
DRIVER_MODULE(hidbus, iichid, hidbus_driver, hidbus_devclass, 0, 0);
