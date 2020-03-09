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

#ifndef _HMAP_H_
#define _HMAP_H_

#include <sys/param.h>
#include <sys/bitstring.h>

struct hmap_hid_item;
struct hmap_item;
struct hmap_softc;

enum hmap_cb_state {
	HMAP_CB_IS_PROBING,
	HMAP_CB_IS_ATTACHING,
	HMAP_CB_IS_RUNNING,
	HMAP_CB_IS_DETACHING,
};

#define	HMAP_CB_ARGS	\
    struct hmap_softc *super_sc, struct hmap_hid_item *hi, intptr_t ctx
#define	HMAP_CB_GET_STATE(...)	\
    ((super_sc == NULL) ? HMAP_CB_IS_PROBING : super_sc->cb_state)
#define	HMAP_CB_GET_SOFTC	((void *)super_sc)
#define	HMAP_CB_GET_EVDEV	(super_sc->evdev)
#define	HMAP_CB_GET_MAP_ITEM	(hi->map)
#define	HMAP_CB_UDATA		(hi->udata)
typedef int hmap_cb_t(HMAP_CB_ARGS);

enum hmap_relabs {
	HMAP_RELABS_ANY = 0,
	HMAP_RELATIVE,
	HMAP_ABSOLUTE,
};

struct hmap_item {
	char			*name;
	int32_t 		usage;		/* HID usage */
	union {
		struct {
			uint16_t	type;	/* Evdev event type */
			uint32_t	code;	/* Evdev event code */
		};
		hmap_cb_t		*cb;	/* Reporting callback */
	};
	bool			required:1;	/* Required by driver */
	enum hmap_relabs	relabs:2;
	bool			has_cb:1;
	u_int			reserved:4;
};

#define	HMAP_ANY(_name, _usage, _type, _code)				\
    .name = (_name), .usage = (_usage), .type = (_type), .code = (_code)
#define	HMAP_KEY(_name, _usage, _code)					\
    HMAP_ANY((_name), (_usage), EV_KEY, (_code)), .relabs = HMAP_RELABS_ANY
#define	HMAP_REL(_name, _usage, _code)					\
    HMAP_ANY((_name), (_usage), EV_REL, (_code)), .relabs = HMAP_RELATIVE
#define	HMAP_ABS(_name, _usage, _code)					\
    HMAP_ANY((_name), (_usage), EV_ABS, (_code)), .relabs = HMAP_ABSOLUTE
#define	HMAP_ANY_CB(_name, _usage, _callback)				\
    .name = (_name), .usage = (_usage), .cb = (_callback), .has_cb = true
#define	HMAP_REL_CB(_name, _usage, _callback)				\
    HMAP_ANY_CB((_name), (_usage), (_callback)), .relabs = HMAP_RELATIVE
#define	HMAP_ABS_CB(_name, _usage, _callback)				\
    HMAP_ANY_CB((_name), (_usage), (_callback)), .relabs = HMAP_ABSOLUTE

#define	HMAP_FOREACH_ITEM(sc, mi)	\
    for ((mi) = (sc)->map; (mi) < (sc)->map + (sc)->nmap_items; (mi)++)

enum hmap_type {
	HMAP_TYPE_CALLBACK = 0,	/* HID item is reported with user callback */
	HMAP_TYPE_VARIABLE,	/* HID item is variable (single usage) */
	HMAP_TYPE_VAR_NULLST,	/* HID item is null state variable */
	HMAP_TYPE_ARR_LIST,	/* HID item is array with list of usages */
	HMAP_TYPE_ARR_RANGE,	/* Array with range (min;max) of usages */
};

struct hmap_hid_item {
	enum hmap_type		type;
	union {
		const struct hmap_item	*map;	/* Callback & variable */
		const struct hmap_item	**list;	/* Array list map type */
		uint32_t		base;	/* Array range map type */
	};
	uint8_t			id;		/* Report ID */
	struct hid_location	loc;		/* HID item location */
	int32_t			lmin;		/* HID item logical minimum */
	int32_t			lmax;		/* HID item logical maximum */
	union {
		intptr_t	udata;		/* Callback private context */
		int32_t		last_val;	/* Last reported value (var) */
		int32_t		last_key;	/* Last reported key (array) */
	};
};

struct hmap_softc {
	device_t		dev;

	struct evdev_dev	*evdev;

	uint32_t		nmap_items;
	const struct hmap_item	*map;
	uint32_t		nhid_items;
	struct hmap_hid_item	*hid_items;
	uint32_t		isize;
	int			*debug_var;
	enum hmap_cb_state	cb_state;
	bitstr_t		bit_decl(evdev_props, INPUT_PROP_CNT);
};

#define	HMAP_CAPS(name, map)	bitstr_t bit_decl((name), nitems(map));
static inline bool
hmap_test_cap(bitstr_t *caps, int cap)
{

	return (bit_test(caps, cap));
}

static inline int
hmap_count_caps(bitstr_t *caps, int first, int last)
{
	int count;

	bit_count(caps, first, last + 1, &count);
	return (count);
}

/*
 * It is safe to call any of following procedures in device_probe context
 * that makes possible to write probe-only drivers with attach/detach handlers
 * inherited from hmap. See hcons and hsctrl drivers for example.
 */

static inline void
hmap_set_evdev_prop(device_t dev, uint16_t prop)
{
	/* Assume struct hmap_softc is a first member of sc */
	struct hmap_softc *sc = device_get_softc(dev);

	bit_set(sc->evdev_props, prop);
}
void		hmap_set_debug_var(device_t dev, int *debug_var);
uint32_t	hmap_add_map(device_t dev, const struct hmap_item *map,
		    int nmap_items, bitstr_t *caps);

device_attach_t	hmap_attach;
device_detach_t	hmap_detach;

extern driver_t hmap_driver;

#endif	/* _HMAP_H_ */
