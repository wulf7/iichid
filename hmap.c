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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*
 * Abstract 1 to 1 HID input usage to evdev event mapper driver.
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
#define DPRINTFN(sc, n, fmt, ...) do {					\
	if ((sc)->debug_var != NULL && *(sc)->debug_var >= (n)) {	\
		device_printf((sc)->dev, "%s: " fmt,			\
		    __FUNCTION__ ,##__VA_ARGS__);			\
	}								\
} while (0)
#define DPRINTF(sc, ...)    DPRINTFN(sc, 1, __VA_ARGS__)
#else
#define DPRINTF(...) do { } while (0)
#define DPRINTFN(...) do { } while (0)
#endif

/* HID report descriptor parser limit hardcoded in usbhid.h */
#define	MAXUSAGE	64

static device_probe_t hmap_probe;

static evdev_open_t hmap_ev_open;
static evdev_close_t hmap_ev_close;

#define HMAP_FOREACH_ITEM(sc, mi, uoff)			\
	for (u_int _map = 0, _item = 0, _uoff_int = -1;	\
	    ((mi) = hmap_get_next_map_item(		\
			(sc), &_map, &_item, &_uoff_int, &(uoff))) != NULL;)

static bool
hmap_get_next_map_index(const struct hmap_item *map, int nmap_items,
    uint32_t *index, uint16_t *usage_offset)
{

	++*usage_offset;
	if ((*index != 0 || *usage_offset != 0) &&
	    *usage_offset >= map[*index].nusages) {
		++*index;
		*usage_offset = 0;
	}

	return (*index < nmap_items);
}

static const struct hmap_item *
hmap_get_next_map_item(struct hmap_softc *sc, u_int *map, u_int *item,
    u_int *uoff_int, uint16_t *uoff)
{

	*uoff = *uoff_int;
	while (!hmap_get_next_map_index(
	   sc->map[*map], sc->nmap_items[*map], item, uoff)) {
		++*map;
		*item = 0;
		*uoff = -1;
		if (*map >= sc->nmaps)
			return (NULL);
	}
	*uoff_int = *uoff;

	return (sc->map[*map] + *item);
}

void
hmap_set_debug_var(device_t dev, int *debug_var)
{
#ifdef HID_DEBUG
	struct hmap_softc *sc = device_get_softc(dev);

	sc->debug_var = debug_var;
#endif
}

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
	int32_t usage;
	int32_t data;
	uint16_t key, uoff;
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

		/*
		 * 5.8. If Logical Minimum and Logical Maximum are both
		 * positive values then the contents of a field can be assumed
		 * to be an unsigned value. Otherwise, all integer values are
		 * signed values represented in 2’s complement format.
		 */
		data = hi->lmin < 0 || hi->lmax < 0
		    ? hid_get_data(buf, len, &hi->loc)
		    : hid_get_udata(buf, len, &hi->loc);

		switch (hi->type) {
		case HMAP_TYPE_CALLBACK:
			if (hi->map->cb(sc, hi, data) != 0)
				continue;
			break;

		case HMAP_TYPE_VAR_NULLST:
			/*
			 * 5.10. If the host or the device receives an
			 * out-of-range value then the current value for the
			 * respective control will not be modified.
			 */
			if (data < hi->lmin || data > hi->lmax)
				continue;
			/* FALLTROUGH */
		case HMAP_TYPE_VARIABLE:
			/*
			 * Ignore reports for absolute data if the data did not
			 * change and for relative data if data is 0.
			 * Evdev layer filters out them anyway.
			 */
			if (data == (hi->evtype == EV_REL ? 0 : hi->last_val))
				continue;
			evdev_push_event(sc->evdev, hi->evtype,
			    hi->code, data);
			hi->last_val = data;
			break;

		case HMAP_TYPE_ARR_LIST:
			key = KEY_RESERVED;
			/*
			 * 6.2.2.5. An out-of range value in an array field
			 * is considered no controls asserted.
			 */
			if (data < hi->lmin || data > hi->lmax)
				goto report_key;
			/*
			 * 6.2.2.5. Rather than returning a single bit for each
			 * button in the group, an array returns an index in
			 * each field that corresponds to the pressed button.
			 */
			key = hi->codes[data - hi->lmin];
			if (key == KEY_RESERVED)
				DPRINTF(sc, "Can not map unknown HID "
				    "array index: %08x\n", data);
			goto report_key;

		case HMAP_TYPE_ARR_RANGE:
			key = KEY_RESERVED;
			/*
			 * 6.2.2.5. An out-of range value in an array field
			 * is considered no controls asserted.
			 */
			if (data < hi->lmin || data > hi->lmax)
				goto report_key;
			/*
			 * When the input field is an array and the usage is
			 * specified with a range instead of an ID, we have to
			 * derive the actual usage by using the item value as
			 * an index in the usage range list.
			 */
			usage = data - hi->lmin + hi->umin;
			HMAP_FOREACH_ITEM(sc, mi, uoff) {
				if (usage == mi->usage + uoff &&
				    mi->type == EV_KEY && !mi->has_cb) {
					key = mi->code;
					break;
				}
			}
			if (key == KEY_RESERVED)
				DPRINTF(sc, "Can not map unknown HID "
				    "usage: %08x\n", usage);
report_key:
			if (key == hi->last_key)
				continue;
			if (hi->last_key != KEY_RESERVED)
				evdev_push_key(sc->evdev, hi->last_key, 0);
			if (key != KEY_RESERVED)
				evdev_push_key(sc->evdev, key, 1);
			hi->last_key = key;
			break;

		default:
			KASSERT(0, ("Unknown map type (%d)", hi->type));
		}
		do_sync = true;
	}

	if (sc->compl_cb && sc->compl_cb(sc, NULL, 0) == 0)
		do_sync = true;

	if (do_sync)
		evdev_sync(sc->evdev);
}

static inline bool
can_map_callback(struct hid_item *hi, const struct hmap_item *mi,
    uint16_t usage_offset)
{

	return (mi->has_cb && !mi->compl_cb &&
	    hi->usage == mi->usage + usage_offset &&
	    (mi->relabs == HMAP_RELABS_ANY ||
	    !(hi->flags & HIO_RELATIVE) == !(mi->relabs == HMAP_RELATIVE)));
}

static inline bool
can_map_variable(struct hid_item *hi, const struct hmap_item *mi,
    uint16_t usage_offset)
{

	return ((hi->flags & HIO_VARIABLE) != 0 && !mi->has_cb &&
	    hi->usage == mi->usage + usage_offset &&
	    (mi->relabs == HMAP_RELABS_ANY ||
	    !(hi->flags & HIO_RELATIVE) == !(mi->relabs == HMAP_RELATIVE)));
}

static inline bool
can_map_arr_range(struct hid_item *hi, const struct hmap_item *mi,
    uint16_t usage_offset)
{

	return ((hi->flags & HIO_VARIABLE) == 0 && !mi->has_cb &&
	    hi->usage_minimum <= mi->usage + usage_offset &&
	    hi->usage_maximum >= mi->usage + usage_offset &&
	    (hi->flags & HIO_RELATIVE) == 0 &&
	    mi->type == EV_KEY);
}

static inline bool
can_map_arr_list(struct hid_item *hi, const struct hmap_item *mi,
    uint32_t usage, uint16_t usage_offset)
{

	return ((hi->flags & HIO_VARIABLE) == 0 && !mi->has_cb &&
	    usage == mi->usage + usage_offset &&
	    (hi->flags & HIO_RELATIVE) == 0 &&
	    mi->type == EV_KEY);
}

static bool
hmap_probe_hid_item(struct hid_item *hi, const struct hmap_item *map,
    int nmap_items, bitstr_t *caps)
{
	struct hmap_hid_item hi_temp;
	int32_t arr_size, usage;
	u_int i, j;
	uint16_t uoff;
	bool found = false;

#define	HMAP_FOREACH_INDEX(map, nitems, idx, uoff)	\
	for ((idx) = 0, (uoff) = -1;			\
	     hmap_get_next_map_index((map), (nitems), &(idx), &(uoff));)

	HMAP_FOREACH_INDEX(map, nmap_items, i, uoff) {
		if (can_map_callback(hi, map + i, uoff)) {
			bzero(&hi_temp, sizeof(hi_temp));
			hi_temp.map = map + i;
			hi_temp.type = HMAP_TYPE_CALLBACK;
			if (hi_temp.map->cb(NULL, &hi_temp, (intptr_t)hi) != 0)
				break;
			bit_set(caps, i);
			return (true);
		}
	}

	if (hi->flags & HIO_VARIABLE) {
		HMAP_FOREACH_INDEX(map, nmap_items, i, uoff) {
			if (can_map_variable(hi, map + i, uoff)) {
				KASSERT(map[i].type == EV_KEY ||
					map[i].type == EV_REL ||
					map[i].type == EV_ABS,
				    ("Unsupported event type"));
				bit_set(caps, i);
				return (true);
			}
		}
		return (false);
	}

	if (hi->usage_minimum != 0 || hi->usage_maximum != 0) {
		HMAP_FOREACH_INDEX(map, nmap_items, i, uoff) {
			if (can_map_arr_range(hi, map + i, uoff)) {
				bit_set(caps, i);
				found = true;
			}
		}
		return (found);
	}

	arr_size = hi->logical_maximum - hi->logical_minimum + 1;
	if (arr_size < 1 || arr_size > MAXUSAGE)
		return (false);
	for (j = 0; j < arr_size; j++) {
		/*
		 * Due to deficiencies in HID report descriptor parser
		 * only first usage in array is returned to caller.
		 * For now bail out instead of processing second one.
		 */
		if (j != 0)
			break;
		usage = hi->usage;
		HMAP_FOREACH_INDEX(map, nmap_items, i, uoff) {
			if (can_map_arr_list(hi, map + i, usage, uoff)) {
				bit_set(caps, i);
				found = true;
			}
		}
	}

	return (found);
}

static uint32_t
hmap_probe_hid_descr(void *d_ptr, uint16_t d_len, uint8_t tlc_index,
    const struct hmap_item *map, int nmap_items, bitstr_t *caps)
{
	struct hid_item hi;
	struct hid_data *hd;
	uint32_t i, items = 0;
	bool do_free = false;

	if (caps == NULL) {
		caps = bit_alloc(nmap_items, M_DEVBUF, M_WAITOK);
		do_free = true;
	} else
		bzero (caps, bitstr_size(nmap_items));

	/* Parse inputs */
	hd = hid_start_parse(d_ptr, d_len, 1 << hid_input);
	HID_TLC_FOREACH_ITEM(hd, &hi, tlc_index) {
		if (hi.kind != hid_input)
			continue;
		if (hi.flags & HIO_CONST)
			continue;
		if (hmap_probe_hid_item(&hi, map, nmap_items, caps))
			items++;
	}
	hid_end_parse(hd);

	/* Check that all mandatory usages are present in report descriptor */
	if (items != 0) {
		for (i = 0; i < nmap_items; i++) {
			if (map[i].required && !bit_test(caps, i)) {
//				DPRINTF(dev, "required usage %s not found\n",
//				    map[i].name);
				items = 0;
				break;
			}
		}
	}

	if (do_free)
		free(caps, M_DEVBUF);

	return (items);
}

uint32_t
hmap_add_map(device_t dev, const struct hmap_item *map, int nmap_items,
    bitstr_t *caps)
{
	struct hmap_softc *sc = device_get_softc(dev);
	uint8_t tlc_index = hidbus_get_index(dev);
	uint32_t items;
	void *d_ptr;
	uint16_t d_len;
	int error;

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error != 0) {
		DPRINTF(sc, "could not retrieve report descriptor from "
		     "device: %d\n", error);
		return (error);
	}

	sc->cb_state = HMAP_CB_IS_PROBING;
	items = hmap_probe_hid_descr(d_ptr, d_len, tlc_index, map, nmap_items,
	    caps);
	if (items == 0)
		return (ENXIO);

	KASSERT(sc->nmaps < HMAP_MAX_MAPS,
	    ("Not more than %d maps is supported", HMAP_MAX_MAPS));
	sc->nhid_items += items;
	sc->map[sc->nmaps] = map;
	sc->nmap_items[sc->nmaps] = nmap_items;
	sc->nmaps++;

	return (0);
}

static bool
hmap_parse_hid_item(struct hmap_softc *sc, struct hid_item *hi,
    struct hmap_hid_item *item)
{
	const struct hmap_item *mi;
	struct hmap_hid_item hi_temp;
	int32_t arr_size, usage;
	uint32_t i;
	uint16_t uoff;
	bool found = false;

	HMAP_FOREACH_ITEM(sc, mi, uoff) {
		if (can_map_callback(hi, mi, uoff)) {
			bzero(&hi_temp, sizeof(hi_temp));
			hi_temp.map = mi;
			hi_temp.type = HMAP_TYPE_CALLBACK;
			/*
			 * Values returned by probe- and attach-stage
			 * callbacks MUST be identical.
			 */
			if (mi->cb(sc, &hi_temp, (intptr_t)hi) != 0)
				break;
			bcopy(&hi_temp, item, sizeof(hi_temp));
			goto mapped;
		}
	}

	if (hi->flags & HIO_VARIABLE) {
		HMAP_FOREACH_ITEM(sc, mi, uoff) {
			if (can_map_variable(hi, mi, uoff)) {
				item->evtype = mi->type;
				item->code = mi->code + uoff;
				item->type = hi->flags & HIO_NULLSTATE ?
				    HMAP_TYPE_VAR_NULLST : HMAP_TYPE_VARIABLE;
				item->last_val = 0;
				switch (mi->type) {
				case EV_KEY:
					evdev_support_event(sc->evdev, EV_KEY);
					evdev_support_key(sc->evdev, item->code);
					break;
				case EV_REL:
					evdev_support_event(sc->evdev, EV_REL);
					evdev_support_rel(sc->evdev, item->code);
					break;
				case EV_ABS:
					evdev_support_event(sc->evdev, EV_ABS);
					evdev_support_abs(sc->evdev, item->code,
					    0, hi->logical_minimum,
					    hi->logical_maximum, 0, 0,
					    hid_item_resolution(hi));
					break;
				default:
					KASSERT(0, ("Unsupported event type"));
				}
				goto mapped;
			}
		}
		return (false);
	}

	if (hi->usage_minimum != 0 || hi->usage_maximum != 0) {
		HMAP_FOREACH_ITEM(sc, mi, uoff) {
			if (can_map_arr_range(hi, mi, uoff)) {
				evdev_support_key(sc->evdev, mi->code + uoff);
				found = true;
			}
		}
		if (!found)
			return (false);
		item->umin = hi->usage_minimum;
		item->type = HMAP_TYPE_ARR_RANGE;
		item->last_key = KEY_RESERVED;
		evdev_support_event(sc->evdev, EV_KEY);
		goto mapped;
	}

	arr_size = hi->logical_maximum - hi->logical_minimum + 1;
	if (arr_size < 1 || arr_size > MAXUSAGE)
		return (false);
	for (i = 0; i < arr_size; i++) {
		/*
		 * Due to deficiencies in HID report descriptor parser
		 * only first usage in array is returned to caller.
		 * For now bail out instead of processing second one.
		 */
		if (i != 0)
			break;
		usage = hi->usage;
		HMAP_FOREACH_ITEM(sc, mi, uoff) {
			if (can_map_arr_list(hi, mi, usage, uoff)) {
				evdev_support_key(sc->evdev, mi->code + uoff);
				if (item->codes == NULL)
					item->codes = malloc(
					    arr_size * sizeof(uint16_t),
					    M_DEVBUF, M_WAITOK | M_ZERO);
				item->codes[i] = mi->code + uoff;
				found = true;
				break;
			}
		}
	}
	if (!found)
		return (false);
	item->type = HMAP_TYPE_ARR_LIST;
	item->last_key = KEY_RESERVED;
	evdev_support_event(sc->evdev, EV_KEY);

mapped:
	item->id = hi->report_ID;
	item->loc = hi->loc;
	item->lmin = hi->logical_minimum;
	item->lmax = hi->logical_maximum;

	return (true);
}

static int
hmap_parse_hid_descr(struct hmap_softc *sc, uint8_t tlc_index)
{
	struct hid_item hi;
	struct hid_data *hd;
	const struct hmap_item *mi;
	struct hmap_hid_item *item = sc->hid_items;
	void *d_ptr;
	uint16_t d_len, uoff;
	int error;

	error = hid_get_report_descr(sc->dev, &d_ptr, &d_len);
	if (error != 0) {
		DPRINTF(sc, "could not retrieve report descriptor from "
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
		if (hmap_parse_hid_item(sc, &hi, item))
			item++;
		KASSERT(item <= sc->hid_items + sc->nhid_items,
		    ("Parsed HID item array overflow"));
	}
	hid_end_parse(hd);

	/*
	 * Resulting number of parsed HID items can be less than expected as
	 * map items might be duplicated in different maps. Save real number.
	 */
	if (sc->nhid_items != item - sc->hid_items)
		DPRINTF(sc, "Parsed HID item number mismatch: expected=%u "
		    "result=%ld\n", sc->nhid_items, item - sc->hid_items);
	sc->nhid_items = item - sc->hid_items;

	sc->isize = hid_report_size(d_ptr, d_len, hid_input, NULL);

	/*
	 * If completion callback returned success at attach stage, run it
	 * in interrupt handler and at the device detach too.
	 */
	HMAP_FOREACH_ITEM(sc, mi, uoff) {
		if (mi->compl_cb) {
			if (mi->cb(sc, NULL, 0) == 0)
				sc->compl_cb = mi->cb;
			break;
		}
	}

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
	int error;

	sc->cb_state = HMAP_CB_IS_ATTACHING;

	sc->dev = dev;
	sc->hid_items = malloc(sc->nhid_items * sizeof(struct hid_item),
	    M_DEVBUF, M_WAITOK | M_ZERO);

	hidbus_set_intr(dev, hmap_intr);
	sc->evdev_methods = (struct evdev_methods) {
		.ev_open = &hmap_ev_open,
		.ev_close = &hmap_ev_close,
	};

	sc->evdev = evdev_alloc();
	evdev_set_name(sc->evdev, device_get_desc(dev));
	evdev_set_phys(sc->evdev, device_get_nameunit(dev));
	evdev_set_id(sc->evdev, hw->idBus, hw->idVendor, hw->idProduct,
	    hw->idVersion);
	evdev_set_serial(sc->evdev, hw->serial);
	evdev_support_event(sc->evdev, EV_SYN);
	error = hmap_parse_hid_descr(sc, hidbus_get_index(dev));
	if (error) {
		hmap_detach(dev);
		return (ENXIO);
	}

	evdev_set_methods(sc->evdev, dev, &sc->evdev_methods);
	sc->cb_state = HMAP_CB_IS_RUNNING;

	error = evdev_register_mtx(sc->evdev, hidbus_get_lock(dev));
	if (error) {
		hmap_detach(dev);
		return (ENXIO);
	}

	return (0);
}

int
hmap_detach(device_t dev)
{
	struct hmap_softc *sc = device_get_softc(dev);
	struct hmap_hid_item *hi;

	sc->cb_state = HMAP_CB_IS_DETACHING;

	evdev_free(sc->evdev);
	if (sc->hid_items != NULL) {
		for (hi = sc->hid_items; hi < sc->hid_items + sc->nhid_items;
		    hi++)
			if (hi->type == HMAP_TYPE_CALLBACK)
				hi->map->cb(sc, hi, 0);
			else if (hi->type == HMAP_TYPE_ARR_LIST)
				free(hi->codes, M_DEVBUF);
		free(sc->hid_items, M_DEVBUF);
	}

	if (sc->compl_cb)
		sc->compl_cb(sc, NULL, 0);

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
