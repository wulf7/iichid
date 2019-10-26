/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2008 Hans Petter Selasky
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
 *
 * $FreeBSD$
 */

#include <sys/types.h>

#include "hid.h"
#include "hidbus.h"

/*------------------------------------------------------------------------*
 *	hid_lookup_id
 *
 * This functions takes an array of "struct hid_device_id" and tries
 * to match the entries with the information in "struct hid_tlc_info".
 *
 * NOTE: The "sizeof_id" parameter must be a multiple of the
 * hid_device_id structure size. Else the behaviour of this function
 * is undefined.
 *
 * Return values:
 * NULL: No match found.
 * Else: Pointer to matching entry.
 *------------------------------------------------------------------------*/
const struct hid_device_id *
hid_lookup_id(const struct hid_device_id *id, size_t sizeof_id,
    const struct hid_tlc_info *tlc)
{
	const struct hid_device_id *id_end;
	const struct hid_device_info *info = tlc->device_info;

	if (id == NULL) {
		goto done;
	}
	id_end = (const void *)(((const uint8_t *)id) + sizeof_id);

	/*
	 * Keep on matching array entries until we find a match or
	 * until we reach the end of the matching array:
	 */
	for (; id != id_end; id++) {

		if ((id->match_flag_usage) &&
		    (id->usage != tlc->usage)) {
			continue;
		}
		if ((id->match_flag_bus) &&
		    (id->idBus != info->idBus)) {
			continue;
		}
		if ((id->match_flag_vendor) &&
		    (id->idVendor != info->idVendor)) {
			continue;
		}
		if ((id->match_flag_product) &&
		    (id->idProduct != info->idProduct)) {
			continue;
		}
		if ((id->match_flag_ver_lo) &&
		    (id->idVersion_lo > info->idVersion)) {
			continue;
		}
		if ((id->match_flag_ver_hi) &&
		    (id->idVersion_hi < info->idVersion)) {
			continue;
		}
		/* We found a match! */
		return (id);
	}

done:
	return (NULL);
}

/*------------------------------------------------------------------------*
 *	hid_lookup_driver_info - factored out code
 *
 * Return values:
 *    0: Success
 * Else: Failure
 *------------------------------------------------------------------------*/
int
hid_lookup_driver_info(const struct hid_device_id *id, size_t sizeof_id,
    struct hid_tlc_info *tlc)
{
	id = hid_lookup_id(id, sizeof_id, tlc);
	if (id) {
		/* copy driver info */
		tlc->driver_info = id->driver_info;
		return (0);
	}
	return (ENXIO);
}
