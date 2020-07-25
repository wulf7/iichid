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
 * General Desktop/System Controls usage page driver
 * https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf
 */

#include <sys/param.h>
#include <sys/bitstring.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#include "hid.h"
#include "hidbus.h"
#include "hmap.h"

#define	HID_DEBUG_VAR	hsctrl_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hsctrl_debug = 1;

static SYSCTL_NODE(_hw_hid, OID_AUTO, hsctrl, CTLFLAG_RW, 0,
		"System Controls");
SYSCTL_INT(_hw_hid_hsctrl, OID_AUTO, debug, CTLFLAG_RWTUN,
		&hsctrl_debug, 0, "Debug level");
#endif

#define HUG_SYSTEM_POWER_UP	0x008e
#define HUG_SYSTEM_RESTART	0x008f

#define	HSCTRL_MAP(usage, code)	\
	{ HMAP_KEY(HUP_GENERIC_DESKTOP, HUG_SYSTEM_##usage, code) }

static const struct hmap_item hsctrl_map[] = {
	HSCTRL_MAP(POWER_DOWN,		KEY_POWER),
	HSCTRL_MAP(SLEEP,		KEY_SLEEP),
	HSCTRL_MAP(WAKEUP,		KEY_WAKEUP),
	HSCTRL_MAP(CONTEXT_MENU,	KEY_CONTEXT_MENU),
	HSCTRL_MAP(MAIN_MENU,		KEY_MENU),
	HSCTRL_MAP(APP_MENU,		KEY_PROG1),
	HSCTRL_MAP(MENU_HELP,		KEY_HELP),
	HSCTRL_MAP(MENU_EXIT,		KEY_EXIT),
	HSCTRL_MAP(MENU_SELECT,		KEY_SELECT),
	HSCTRL_MAP(MENU_RIGHT,		KEY_RIGHT),
	HSCTRL_MAP(MENU_LEFT,		KEY_LEFT),
	HSCTRL_MAP(MENU_UP,		KEY_UP),
	HSCTRL_MAP(MENU_DOWN,		KEY_DOWN),
	HSCTRL_MAP(POWER_UP,		KEY_POWER2),
	HSCTRL_MAP(RESTART,		KEY_RESTART),
};

static const struct hid_device_id hsctrl_devs[] = {
	{ HID_TLC(HUP_GENERIC_DESKTOP, HUG_SYSTEM_CONTROL) },
};

static int
hsctrl_probe(device_t dev)
{
	struct hmap *hm = device_get_softc(dev);
	int error;

	error = HIDBUS_LOOKUP_DRIVER_INFO(dev, hsctrl_devs);
	if (error != 0)
		return (error);

	hmap_set_dev(hm, dev);
	hmap_set_debug_var(hm, &HID_DEBUG_VAR);

	/* Check if report descriptor belongs to a System control TLC */
	error = hmap_add_map(hm, hsctrl_map, nitems(hsctrl_map), NULL);
	if (error != 0)
		return (error);

	hidbus_set_desc(dev, "System Control");

	return (BUS_PROBE_DEFAULT);
}

static int
hsctrl_attach(device_t dev)
{
	return (hmap_attach(device_get_softc(dev)));
}

static int
hsctrl_detach(device_t dev)
{
	return (hmap_detach(device_get_softc(dev)));
}

static devclass_t hsctrl_devclass;
static device_method_t hsctrl_methods[] = {
	DEVMETHOD(device_probe,		hsctrl_probe),
	DEVMETHOD(device_attach,	hsctrl_attach),
	DEVMETHOD(device_detach,	hsctrl_detach),

	DEVMETHOD_END
};

DEFINE_CLASS_0(hsctrl, hsctrl_driver, hsctrl_methods, sizeof(struct hmap));
DRIVER_MODULE(hsctrl, hidbus, hsctrl_driver, hsctrl_devclass, NULL, 0);
MODULE_DEPEND(hsctrl, hid, 1, 1, 1);
MODULE_DEPEND(hsctrl, hmap, 1, 1, 1);
MODULE_DEPEND(hsctrl, evdev, 1, 1, 1);
MODULE_VERSION(hsctrl, 1);
