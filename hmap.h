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

#ifndef _HMAP_H_
#define _HMAP_H_

struct hmap_item {
	char		*name;
	int32_t 	usage;		/* HID usage */
	uint16_t	type;		/* Evdev event type */
	uint32_t	code;		/* Evdev event code */
	struct {
		u_int	required:1;	/* Required for System Controls */
		u_int	relative:1;
		u_int	reserved:30;
	};
};

struct hmap_hid_item {
	union {
		const struct hmap_item	*map;
		struct {
			uint32_t	offset;
			int32_t		last_key;
		};
	};
	uint8_t			id;
	struct hid_location	loc;
	uint32_t		flags;
	bool			is_signed;
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
	bitstr_t		bit_decl(evdev_props, INPUT_PROP_CNT);
};

uint32_t	hmap_hid_probe(device_t dev, const struct hmap_item *map,
		    int nmap_items, bitstr_t *caps);

device_attach_t	hmap_attach;
device_detach_t	hmap_detach;

extern driver_t hmap_driver;

#endif	/* _HMAP_H_ */
