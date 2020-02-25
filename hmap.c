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
 * Abstract 1 to 1 HID usage to evdev event mapper driver.
 */

#include <sys/param.h>
#include <sys/bitstring.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/sbuf.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "hid.h"
#include "hidbus.h"
#include "hmap.h"

#ifdef HID_DEBUG
#define DPRINTFN(sc, n, fmt, ...) do {			\
	if ((*(sc)->debug_var) >= (n)) {		\
		device_printf((sc)->dev, "%s: " fmt,	\
		    __FUNCTION__ ,##__VA_ARGS__);	\
  }							\
} while (0)
#define DPRINTF(dev, fmt, ...)				\
		device_printf((dev), "%s: " fmt,	\
		    __FUNCTION__ ,##__VA_ARGS__)
#else
#define DPRINTF(...) do { } while (0)
#define DPRINTFN(...) do { } while (0)
#endif


static device_probe_t hmap_probe;

static evdev_open_t hmap_ev_open;
static evdev_close_t hmap_ev_close;

static const struct evdev_methods hmap_evdev_methods = {
	.ev_open = &hmap_ev_open,
	.ev_close = &hmap_ev_close,
};

static int
hmap_ev_close(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	return (hidbus_intr_stop(dev));
}

static int
hmap_ev_open(struct evdev_dev *evdev)
{
	device_t dev = evdev_get_softc(evdev);

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	return (hidbus_intr_start(dev));
}

static void
hmap_intr(void *context, void *buf, uint16_t len)
{
	device_t dev = context;
	struct hmap_softc *sc = device_get_softc(dev);
	struct hmap_hid_item *hi;
	const struct hmap_item *mi;
	uint32_t usage;
	int32_t data;
	uint8_t id = 0;
	bool do_sync = false;

	mtx_assert(hidbus_get_lock(dev), MA_OWNED);

	/* Make sure we don't process old data */
	if (len < sc->isize)
		bzero((uint8_t *)buf + len, sc->isize - len);

	/* Strip leading "report ID" byte */
	if (sc->hid_items[0].id) {
		id = *(uint8_t *)buf;
		len--;
		buf = (uint8_t *)buf + 1;
	}

	for (hi = sc->hid_items; hi < sc->hid_items + sc->nhid_items; hi++) {
		/* Ignore irrelevant reports */
		if (id != hi->id)
			continue;

		/* Try to avoid sign extension effects */
		data = hi->is_signed
		    ? hid_get_data(buf, len, &hi->loc)
		    : hid_get_udata(buf, len, &hi->loc);

		if (hi->flags & HIO_VARIABLE) {
			evdev_push_event(sc->evdev, hi->map->type,
			    hi->map->code, data);
		} else {
			if (hi->last_key != 0)
				evdev_push_key(sc->evdev, hi->last_key, 0);
			hi->last_key = 0;
			if (data == 0)
				continue;
			/*
			 * When the input field is an array and the usage is
			 * specified with a range instead of an ID, we have to
			 * derive the actual usage by using the item value as
			 * an index in the usage range list.
			 */
			usage = hi->offset + data;
			for (mi = sc->map; mi < sc->map+sc->nmap_items; mi++) {
				if (usage == mi->usage && mi->type == EV_KEY) {
					hi->last_key = mi->code;
					evdev_push_key(sc->evdev,
					    hi->last_key, 1);
					break;
				}
			}
		}
		do_sync = true;
	}

	if (do_sync)
		evdev_sync(sc->evdev);
}

static inline bool
can_map_variable(struct hid_item *hi, const struct hmap_item *mi)
{

	return (hi->usage == mi->usage && (mi->type == EV_KEY ||
	    !(hi->flags & HIO_RELATIVE) == !mi->relative));
}

static inline bool
can_map_array(struct hid_item *hi, const struct hmap_item *mi)
{

	return (hi->usage_minimum <= mi->usage &&
	    hi->usage_maximum >= mi->usage &&
	    (hi->flags & HIO_RELATIVE) == 0 &&
	    mi->type == EV_KEY);
}

uint32_t
hmap_hid_probe(device_t dev, const struct hmap_item *map, int nmap_items,
    bitstr_t *caps)
{
	uint8_t tlc_index = hidbus_get_index(dev);
	struct hid_item hi;
	struct hid_data *hd;
	uint32_t i, items = 0;
	void *d_ptr;
	uint16_t d_len;
	bool found;
	int error;

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error != 0) {
		DPRINTF(dev, "could not retrieve report descriptor from "
		     "device: %d\n", error);
		return (0);
	}

	/* Parse inputs */
	hd = hid_start_parse(d_ptr, d_len, 1 << hid_input);
	HID_TLC_FOREACH_ITEM(hd, &hi, tlc_index) {
		if (hi.kind != hid_input)
			continue;
		if (hi.flags & HIO_CONST)
			continue;
		if (hi.flags & HIO_VARIABLE) {
			for (i = 0; i < nmap_items; i++) {
				if (can_map_variable(&hi, map + i)) {
					KASSERT(map[i].type == EV_KEY ||
					    map[i].type == EV_REL ||
					    map[i].type == EV_ABS,
					    ("Unsupported event type"));
					bit_set(caps, i);
					items++;
					break;
				}
			}
		} else {
			for (i = 0; i < nmap_items; i++) {
				found = false;
				if (can_map_array(&hi, map + i)) {
					bit_set(caps, i);
					found = true;
				}
				if (found)
					items++;
			}
		}
	}
	hid_end_parse(hd);

	if (items == 0)
		return (0);

	for (i = 0; i < nmap_items; i++) {
		if (map[i].required && !bit_test(caps, i)) {
			DPRINTF(dev, "required usage %s not found\n",
			    map[i].name);
			return (0);
		}
	}

	return (items);
}

static int
hmap_hid_parse(struct hmap_softc *sc, uint8_t tlc_index)
{
	struct hid_item hi;
	struct hid_data *hd;
	size_t i, item = 0;
	void *d_ptr;
	uint16_t d_len;
	bool found;
	int error;

	error = hid_get_report_descr(sc->dev, &d_ptr, &d_len);
	if (error != 0) {
		DPRINTF(sc->dev, "could not retrieve report descriptor from "
		     "device: %d\n", error);
		return (error);
	}

	/* Parse inputs */
	hd = hid_start_parse(d_ptr, d_len, 1 << hid_input);
	HID_TLC_FOREACH_ITEM(hd, &hi, tlc_index) {
		if (hi.kind != hid_input)
			continue;
		if (hi.flags & HIO_CONST)
			continue;
		found = false;
		if (hi.flags & HIO_VARIABLE) {
			for (i = 0; i < sc->nmap_items; i++) {
				if (!can_map_variable(&hi, sc->map + i))
					continue;
				sc->hid_items[item].map = sc->map + i;
				switch (sc->map[i].type) {
				case EV_KEY:
					evdev_support_event(sc->evdev, EV_KEY);
					evdev_support_key(sc->evdev,
					    sc->map[i].code);
					break;
				case EV_REL:
					evdev_support_event(sc->evdev, EV_REL);
					evdev_support_rel(sc->evdev,
					    sc->map[i].code);
					break;
				case EV_ABS:
					evdev_support_event(sc->evdev, EV_ABS);
					evdev_support_abs(sc->evdev,
					    sc->map[i].code, 0,
					    hi.logical_minimum,
					    hi.logical_maximum, 0, 0,
					    hid_item_resolution(&hi));
					break;
				default:
					KASSERT(0, ("Unsupported event type"));
				}
				found = true;
				break;
			}
		} else {
			for (i = 0; i < sc->nmap_items; i++) {
				if (!can_map_array(&hi, sc->map + i))
					continue;
				evdev_support_key(sc->evdev, sc->map[i].code);
				sc->hid_items[item].last_key = 0;
				sc->hid_items[item].offset =
				    hi.usage_minimum - hi.logical_minimum;
				found = true;
			}
			if (found)
				evdev_support_event(sc->evdev, EV_KEY);
		}
		if (!found)
			continue;
		sc->hid_items[item].id = hi.report_ID;
		sc->hid_items[item].loc = hi.loc;
		sc->hid_items[item].flags = hi.flags;
		sc->hid_items[item].is_signed = hi.logical_minimum < 0;
		item++;
		KASSERT(item > sc->nitems, ("Parsed HID item array overflow"));
	}
	hid_end_parse(hd);

	return (0);
}

static int
hmap_probe(device_t dev)
{

	/* It is an abstract driver */
	return (ENXIO);
}

int
hmap_attach(device_t dev)
{
	struct hmap_softc *sc = device_get_softc(dev);
	const struct hid_device_info *hw = hid_get_device_info(dev);
	uint16_t i;
	int error;

	sc->dev = dev;
	sc->hid_items = malloc(sc->nhid_items * sizeof(struct hid_item),
	    M_DEVBUF, M_WAITOK | M_ZERO);

	device_set_desc(dev, hw->name);

	hidbus_set_intr(dev, hmap_intr);

	sc->evdev = evdev_alloc();
	evdev_set_name(sc->evdev, device_get_desc(dev));
	evdev_set_phys(sc->evdev, device_get_nameunit(dev));
	evdev_set_id(sc->evdev, hw->idBus, hw->idVendor,
			hw->idProduct, hw->idVersion);
	evdev_set_serial(sc->evdev, hw->serial);
	evdev_support_event(sc->evdev, EV_SYN);
	for (i = 0; i < INPUT_PROP_CNT; i++)
		if (bit_test(sc->evdev_props, i))
			evdev_set_flag(sc->evdev, i);
	evdev_set_methods(sc->evdev, dev, &hmap_evdev_methods);
	error = hmap_hid_parse(sc, hidbus_get_index(dev));
	if (error) {
		hmap_detach(dev);
		return (ENXIO);
	}

	error = evdev_register_mtx(sc->evdev, hidbus_get_lock(dev));
	if (error) {
		hmap_detach(dev);
		return (ENXIO);
	}

	return (0);
}

int
hmap_detach(device_t self)
{
	struct hmap_softc *sc = device_get_softc(self);

	evdev_free(sc->evdev);
	free(sc->hid_items, M_DEVBUF);

	return (0);
}

static devclass_t hmap_devclass;

static device_method_t hmap_methods[] = {
	DEVMETHOD(device_probe, hmap_probe),
	DEVMETHOD(device_attach, hmap_attach),
	DEVMETHOD(device_detach, hmap_detach),

	DEVMETHOD_END
};

driver_t hmap_driver = {
	.name = "hmap",
	.methods = hmap_methods,
	.size = sizeof(struct hmap_softc),
};

DRIVER_MODULE(hmap, hidbus, hmap_driver, hmap_devclass, NULL, 0);
MODULE_DEPEND(hmap, hid, 1, 1, 1);
MODULE_DEPEND(hmap, evdev, 1, 1, 1);
MODULE_VERSION(hmap, 1);
