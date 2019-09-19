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
#include <sys/module.h>

#include "hidbus.h"
#include "iichid.h"

#include "hid_if.h"

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
	device_t child;
	int error;
	void *ivars = device_get_ivars(dev);

	child = device_add_child(dev, NULL, -1);
	if (child == NULL) {
		device_printf(dev, "Could not add HID device\n");
		return (ENXIO);
	}

	device_set_ivars(child, ivars);
	error = bus_generic_attach(dev);
	if (error)
		device_printf(dev, "failed to attach child: error %d\n", error);

	return (0);
}

static int
hidbus_detach(device_t dev)
{

	bus_generic_detach(dev);
	device_delete_children(dev);
	return (0);
}

static device_method_t hidbus_methods[] = {
	/* device interface */
	DEVMETHOD(device_probe,         hidbus_probe),
	DEVMETHOD(device_attach,        hidbus_attach),
	DEVMETHOD(device_detach,        hidbus_detach),
	DEVMETHOD(device_suspend,       bus_generic_suspend),
	DEVMETHOD(device_resume,        bus_generic_resume),

	/* hidbus interface */

        DEVMETHOD_END
};


driver_t hidbus_driver = {
	"hidbus",
	hidbus_methods,
	0 /*sizeof(struct hidbus_softc)*/,
};

devclass_t hidbus_devclass;

MODULE_VERSION(hidbus, 1);
DRIVER_MODULE(hidbus, iichid, hidbus_driver, hidbus_devclass, 0, 0);
