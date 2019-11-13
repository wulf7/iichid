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

/*
 * Digitizer configuration top-level collection support.
 * https://docs.microsoft.com/en-us/windows-hardware/design/component-guidelines/windows-precision-touchpad-required-hid-top-level-collections
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include "hid.h"
#include "hidbus.h"

#include "hconf.h"

#define	HID_DEBUG_VAR	hconf_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hconf_debug = 0;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hconf, CTLFLAG_RW, 0,
    "Digitizer configuration top-level collection");
SYSCTL_INT(_hw_hid_hconf, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hconf_debug, 1, "Debug level");
#endif

struct hconf_softc {
	device_t		dev;
	uint32_t		input_mode;
	struct hid_location	input_mode_loc;
	uint32_t		input_mode_rlen;
	uint8_t			input_mode_rid;
};

static device_probe_t		hconf_probe;
static device_attach_t		hconf_attach;
static device_detach_t		hconf_detach;
static device_resume_t		hconf_resume;

static devclass_t hconf_devclass;

static device_method_t hconf_methods[] = {

	DEVMETHOD(device_probe,		hconf_probe),
	DEVMETHOD(device_attach,	hconf_attach),
	DEVMETHOD(device_detach,	hconf_detach),
	DEVMETHOD(device_resume,	hconf_resume),

	DEVMETHOD_END
};

static driver_t hconf_driver = {
	.name = "hconf",
	.methods = hconf_methods,
	.size = sizeof(struct hconf_softc),
};

static const struct hid_device_id hconf_devs[] = {
	{ HID_TLC(HUP_DIGITIZERS, HUD_CONFIG) },
};

static int
hconf_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, hconf_devs, sizeof(hconf_devs));
	if (error != 0)
		return (error);

	return (BUS_PROBE_DEFAULT);
}

static int
hconf_attach(device_t dev)
{
	struct hconf_softc *sc = device_get_softc(dev);
	const struct hid_device_info *hw = hid_get_device_info(dev);
	uint32_t flags;
	void *d_ptr;
	uint16_t d_len;
	uint8_t tlc_index;
	int error;

	device_set_desc(dev, hw->name);

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error) {
		device_printf(dev, "could not retrieve report descriptor from "
		    "device: %d\n", error);
		return (ENXIO);
	}

	sc->dev = dev;

	tlc_index = hidbus_get_index(dev);

	/* Parse features for input mode switch */
	if (hid_tlc_locate(d_ptr, d_len,
	    HID_USAGE2(HUP_DIGITIZERS, HUD_CONFIG), hid_feature, tlc_index,
	    0, &sc->input_mode_loc, &flags, &sc->input_mode_rid, NULL) &&
	    (flags & (HIO_VARIABLE | HIO_RELATIVE)) == HIO_VARIABLE)
		sc->input_mode_rlen = hid_report_size_1(d_ptr, d_len,
		    hid_feature, sc->input_mode_rid);

	return (0);
}

static int
hconf_detach(device_t dev)
{

	return (0);
}

static int
hconf_resume(device_t dev)
{
	struct hconf_softc *sc = device_get_softc(dev);
	int error;

	if (sc->input_mode_rlen > 1) {
		error = hconf_set_input_mode(sc, sc->input_mode);
		if (error)
			DPRINTF("Failed to set input mode: %d\n", error);
	}

	return (0);
}

int
hconf_set_input_mode(struct hconf_softc *sc, enum hconf_input_mode mode)
{
	uint8_t *fbuf;
	int error;

	if (sc->input_mode_rlen <= 1)
		return (ENXIO);

	fbuf = malloc(sc->input_mode_rlen, M_TEMP, M_WAITOK | M_ZERO);

	/* Input Mode report is not strictly required to be readable */
	error = hid_get_report(sc->dev, fbuf, sc->input_mode_rlen,
	    HID_FEATURE_REPORT, sc->input_mode_rid);
	if (error)
		bzero(fbuf + 1, sc->input_mode_rlen - 1);

	fbuf[0] = sc->input_mode_rid;
	hid_put_data_unsigned(fbuf + 1, sc->input_mode_rlen - 1,
	    &sc->input_mode_loc, mode);

	error = hid_set_report(sc->dev, fbuf, sc->input_mode_rlen,
	    HID_FEATURE_REPORT, sc->input_mode_rid);

	free(fbuf, M_TEMP);

	if (error == 0)
		sc->input_mode = mode;

	return (error);
}

DRIVER_MODULE(hconf, hidbus, hconf_driver, hconf_devclass, NULL, 0);
MODULE_DEPEND(hconf, hidbus, 1, 1, 1);
MODULE_DEPEND(hconf, hid, 1, 1, 1);
MODULE_VERSION(hconf, 1);
