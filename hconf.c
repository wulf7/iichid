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
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/sx.h>

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

#define	SURFACE_SWITCH	0
#define	BUTTONS_SWITCH	1
#define	SWITCH_COUNT	2

struct hconf_softc {
	device_t		dev;
	struct sx		lock;

	u_int			input_mode;
	struct hid_location	input_mode_loc;
	hid_size_t		input_mode_rlen;
	uint8_t			input_mode_rid;
	struct hid_location     switch_loc[SWITCH_COUNT];
	uint32_t                switch_rlen[SWITCH_COUNT];
	uint8_t                 switch_rid[SWITCH_COUNT];
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
hconf_set_input_mode_impl(struct hconf_softc *sc, enum hconf_input_mode mode)
{
	uint8_t *fbuf;
	int error;

	if (sc->input_mode_rlen <= 1)
		return (ENXIO);

	fbuf = malloc(sc->input_mode_rlen, M_TEMP, M_WAITOK | M_ZERO);
	sx_xlock(&sc->lock);

	/* Input Mode report is not strictly required to be readable */
	error = hid_get_report(sc->dev, fbuf, sc->input_mode_rlen, NULL,
	    HID_FEATURE_REPORT, sc->input_mode_rid);
	if (error != 0)
		bzero(fbuf + 1, sc->input_mode_rlen - 1);

	fbuf[0] = sc->input_mode_rid;
	hid_put_data_unsigned(fbuf + 1, sc->input_mode_rlen - 1,
	    &sc->input_mode_loc, mode);

	error = hid_set_report(sc->dev, fbuf, sc->input_mode_rlen,
	    HID_FEATURE_REPORT, sc->input_mode_rid);
	if (error == 0)
		sc->input_mode = mode;

	sx_unlock(&sc->lock);
	free(fbuf, M_TEMP);

	return (error);
}

static int
hconf_input_mode_handler(SYSCTL_HANDLER_ARGS)
{
	struct hconf_softc *sc = arg1;
	u_int value;
	int error;

	value = sc->input_mode;
	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	error = hconf_set_input_mode_impl(sc, value);
	if (error)
		DPRINTF("Failed to set input mode: %d\n", error);

        return (0);
}

static int
hconf_get_switch(struct hconf_softc *sc, int swtype, u_int *mask)
{
	uint8_t *fbuf;
	int error;

	if (sc->switch_rlen[swtype] <= 1)
		return (ENXIO);

	fbuf = malloc(sc->switch_rlen[swtype], M_TEMP, M_WAITOK | M_ZERO);
	sx_xlock(&sc->lock);

	error = hid_get_report(sc->dev, fbuf, sc->switch_rlen[swtype], NULL,
	    HID_FEATURE_REPORT, sc->switch_rid[swtype]);
	if (error == 0) {
		*mask = hid_get_data_unsigned(fbuf + 1,
		    sc->switch_rlen[swtype] - 1, &sc->switch_loc[swtype]);
	}

	sx_unlock(&sc->lock);
	free(fbuf, M_TEMP);
	return (error);
}

static int
hconf_set_switch(struct hconf_softc *sc, int swtype, u_int mask)
{
	uint8_t *fbuf;
	int error;

	if (sc->switch_rlen[swtype] <= 1)
		return (ENXIO);

	fbuf = malloc(sc->switch_rlen[swtype], M_TEMP, M_WAITOK | M_ZERO);
	sx_xlock(&sc->lock);

	error = hid_get_report(sc->dev, fbuf, sc->switch_rlen[swtype],
	    NULL, HID_FEATURE_REPORT, sc->switch_rid[swtype]);
	if (error != 0)
		goto out;

	hid_put_data_unsigned(fbuf + 1, sc->switch_rlen[swtype] - 1,
	    &sc->switch_loc[swtype], mask);
	error = hid_set_report(sc->dev, fbuf, sc->switch_rlen[swtype],
	    HID_FEATURE_REPORT, sc->switch_rid[swtype]);

out:
	sx_unlock(&sc->lock);
	free(fbuf, M_TEMP);
	return (error);
}

static int
hconf_switch_handler(SYSCTL_HANDLER_ARGS)
{
	struct hconf_softc *sc = arg1;
	u_int value;
	int error;

	error = hconf_get_switch(sc, arg2, &value);
	if (error != 0)
		return (error);

	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	error = hconf_set_switch(sc, arg2, value);
        return (error);
}

static int
hconf_probe(device_t dev)
{
	int error;

	error = hidbus_lookup_driver_info(dev, hconf_devs, sizeof(hconf_devs));
	if (error != 0)
		return (error);

	hidbus_set_desc(dev, "Configuration");

	return (BUS_PROBE_DEFAULT);
}

static int
hconf_attach(device_t dev)
{
	struct hconf_softc *sc = device_get_softc(dev);
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(dev);
	uint32_t flags;
	void *d_ptr;
	hid_size_t d_len;
	uint8_t tlc_index;
	int error;

	error = hid_get_report_descr(dev, &d_ptr, &d_len);
	if (error) {
		device_printf(dev, "could not retrieve report descriptor from "
		    "device: %d\n", error);
		return (ENXIO);
	}

	sc->dev = dev;
	sx_init(&sc->lock, device_get_nameunit(dev));

	tlc_index = hidbus_get_index(dev);

	/* Parse features for input mode switch */
	if (hid_tlc_locate(d_ptr, d_len,
	    HID_USAGE2(HUP_DIGITIZERS, HUD_INPUT_MODE), hid_feature, tlc_index,
	    0, &sc->input_mode_loc, &flags, &sc->input_mode_rid, NULL) &&
	    (flags & (HIO_VARIABLE | HIO_RELATIVE)) == HIO_VARIABLE)
		sc->input_mode_rlen = hid_report_size_1(d_ptr, d_len,
		    hid_feature, sc->input_mode_rid);

	if (sc->input_mode_rlen > 1)
		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "input_mode", CTLTYPE_UINT | CTLFLAG_RW, sc, 0,
		    hconf_input_mode_handler, "I",
		    "HID device input mode: 0 = mouse, 3 = touchpad");

	/* Parse features for enable / disable switches. */
	if (hid_tlc_locate(d_ptr, d_len,
	    HID_USAGE2(HUP_DIGITIZERS, 0x57), hid_feature, tlc_index,
	    0, &sc->switch_loc[SURFACE_SWITCH], &flags,
	    &sc->switch_rid[SURFACE_SWITCH], NULL) &&
		(flags & (HIO_VARIABLE | HIO_RELATIVE)) == HIO_VARIABLE) {
		sc->switch_rlen[SURFACE_SWITCH] = hid_report_size_1(d_ptr,
		    d_len, hid_feature, sc->switch_rid[SURFACE_SWITCH]);
	}
	if (sc->switch_rlen[SURFACE_SWITCH] > 1) {
		struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(dev);
		struct sysctl_oid *tree = device_get_sysctl_tree(dev);

		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "surface_switch", CTLTYPE_UINT | CTLFLAG_RW, sc,
		    SURFACE_SWITCH, hconf_switch_handler, "I",
		    "Enable / disable switch for surface");
	}
	if (hid_tlc_locate(d_ptr, d_len,
	    HID_USAGE2(HUP_DIGITIZERS, 0x58), hid_feature, tlc_index,
	    0, &sc->switch_loc[BUTTONS_SWITCH], &flags,
	    &sc->switch_rid[BUTTONS_SWITCH], NULL) &&
		(flags & (HIO_VARIABLE | HIO_RELATIVE)) == HIO_VARIABLE) {
		sc->switch_rlen[BUTTONS_SWITCH] = hid_report_size_1(d_ptr,
		    d_len, hid_feature, sc->switch_rid[BUTTONS_SWITCH]);
	}
	if (sc->switch_rlen[BUTTONS_SWITCH] > 1) {
		struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(dev);
		struct sysctl_oid *tree = device_get_sysctl_tree(dev);

		SYSCTL_ADD_PROC(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
		    "buttons_switch", CTLTYPE_UINT | CTLFLAG_RW, sc,
		    BUTTONS_SWITCH, hconf_switch_handler, "I",
		    "Enable / disable switch for buttons");
	}

	return (0);
}

static int
hconf_detach(device_t dev)
{
	struct hconf_softc *sc = device_get_softc(dev);

	sx_destroy(&sc->lock);

	return (0);
}

static int
hconf_resume(device_t dev)
{
	struct hconf_softc *sc = device_get_softc(dev);
	int error;

	if (sc->input_mode_rlen > 1) {
		error = hconf_set_input_mode_impl(sc, sc->input_mode);
		if (error)
			DPRINTF("Failed to set input mode: %d\n", error);
	}

	return (0);
}

int
hconf_set_input_mode(device_t dev, enum hconf_input_mode mode)
{
	struct hconf_softc *sc = device_get_softc(dev);

	return (hconf_set_input_mode_impl(sc, mode));
}

DRIVER_MODULE(hconf, hidbus, hconf_driver, hconf_devclass, NULL, 0);
MODULE_DEPEND(hconf, hidbus, 1, 1, 1);
MODULE_DEPEND(hconf, hid, 1, 1, 1);
MODULE_VERSION(hconf, 1);
