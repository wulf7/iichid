/*	$NetBSD: uhid.c,v 1.46 2001/11/13 06:24:55 lukem Exp $	*/

/* Also already merged from NetBSD:
 *	$NetBSD: uhid.c,v 1.54 2002/09/23 05:51:21 simonb Exp $
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

/*-
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
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

/*
 * HID spec: http://www.usb.org/developers/devclass_docs/HID1_11.pdf
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/signalvar.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/filio.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/ioccom.h>
#include <sys/conf.h>
#include <sys/tty.h>
#include <sys/selinfo.h>
#include <sys/proc.h>
#include <sys/poll.h>
#include <sys/sysctl.h>
#include <sys/uio.h>

#include "clist.h"

#include "hid.h"
#include "hidbus.h"
#include "hidraw.h"
#include <dev/usb/usb_ioctl.h>

#define HID_DEBUG_VAR	hidraw_debug
#include "hid_debug.h"

#ifdef HID_DEBUG
static int hidraw_debug = 0;
static SYSCTL_NODE(_hw_hid, OID_AUTO, hidraw, CTLFLAG_RW, 0,
    "HID raw interface");
SYSCTL_INT(_hw_hid_hidraw, OID_AUTO, debug, CTLFLAG_RWTUN,
    &hidraw_debug, 0, "Debug level");
#endif

struct hidraw_softc {
	device_t sc_dev;		/* base device */

	struct mtx *sc_mtx;		/* hidbus private mutex */

	int sc_isize;
	int sc_osize;
	int sc_fsize;
	u_int8_t sc_iid;
	u_int8_t sc_oid;
	u_int8_t sc_fid;

	u_char *sc_buf;			/* user request proxy buffer */
	int sc_buf_size;
	struct sx sc_buf_lock;

	void *sc_repdesc;
	int sc_repdesc_size;

	struct clist sc_q;
	struct selinfo sc_rsel;
	struct proc *sc_async;	/* process that wants SIGIO */
	struct {			/* driver state */
		bool	open:1;		/* device is open */
		bool	aslp:1;		/* waiting for device data in read() */
		bool	sel:1;		/* waiting for device data in poll() */
		bool	immed:1;	/* return read data immediately */
		u_char	reserved:4;
	} sc_state;

	struct cdev *dev;
};

#define	UHID_CHUNK	128	/* chunk size for read */
#define	UHID_BSIZE	1020	/* buffer size */
#define	UHID_INDEX	0xFF	/* Arbitrary high value */

static d_open_t		hidraw_open;
static d_read_t		hidraw_read;
static d_write_t	hidraw_write;
static d_ioctl_t	hidraw_ioctl;
static d_poll_t		hidraw_poll;

static d_priv_dtor_t	hidraw_dtor;

static struct cdevsw hidraw_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	hidraw_open,
	.d_read =	hidraw_read,
	.d_write =	hidraw_write,
	.d_ioctl =	hidraw_ioctl,
	.d_poll =	hidraw_poll,
	.d_name =	"hidraw",
};

static hid_intr_t	hidraw_intr;

static device_identify_t hidraw_identify;
static device_probe_t	hidraw_probe;
static device_attach_t	hidraw_attach;
static device_detach_t	hidraw_detach;

static void		hidraw_notify(struct hidraw_softc *);

static void
hidraw_identify(driver_t *driver, device_t parent)
{
	device_t child;

	if (device_find_child(parent, "hidraw", -1) == NULL) {
		child = BUS_ADD_CHILD(parent, 0, "hidraw",
		    device_get_unit(parent));
		hidbus_set_index(child, UHID_INDEX);
	}
}

static int
hidraw_probe(device_t self)
{

	if (hidbus_get_index(self) != UHID_INDEX)
		return (ENXIO);

#ifdef NOT_YET
	if (usbd_get_quirks(uaa->device)->uq_flags & UQ_HID_IGNORE)
		return (ENXIO);
#endif

	return (BUS_PROBE_GENERIC);
}

static int
hidraw_attach(device_t self)
{
	struct hidraw_softc *sc = device_get_softc(self);
	struct make_dev_args mda;
	uint16_t size;
	void *desc;
	int error;

	hidbus_set_desc(self, "Raw HID Device");

	sc->sc_dev = self;
	sc->sc_mtx = hidbus_get_lock(self);

	error = hid_get_report_descr(sc->sc_dev, &desc, &size);
	if (error) {
		device_printf(self, "no report descriptor\n");
		return ENXIO;
	}

	sx_init(&sc->sc_buf_lock, "hidraw sx");

	sc->sc_isize = hid_report_size(desc, size, hid_input,   &sc->sc_iid);
	sc->sc_osize = hid_report_size(desc, size, hid_output,  &sc->sc_oid);
	sc->sc_fsize = hid_report_size(desc, size, hid_feature, &sc->sc_fid);
	sc->sc_buf_size = imax(sc->sc_isize, imax(sc->sc_osize, sc->sc_fsize));

	sc->sc_repdesc = desc;
	sc->sc_repdesc_size = size;

	make_dev_args_init(&mda);
	mda.mda_flags = MAKEDEV_WAITOK;
	mda.mda_devsw = &hidraw_cdevsw;
	mda.mda_uid = UID_ROOT;
	mda.mda_gid = GID_OPERATOR;
	mda.mda_mode = 0600;
	mda.mda_si_drv1 = sc;

	error = make_dev_s(&mda, &sc->dev, "hidraw%d", device_get_unit(self));
	if (error) {
		device_printf(self, "Can not create character device\n");
		sx_destroy(&sc->sc_buf_lock);
		return (error);
	}

	hidbus_set_intr(sc->sc_dev, hidraw_intr);

	return 0;
}

static int
hidraw_detach(device_t self)
{
	struct hidraw_softc *sc = device_get_softc(self);

	DPRINTF("sc=%p\n", sc);

	mtx_lock(sc->sc_mtx);
	sc->dev->si_drv1 = NULL;
	/* Wake everyone */
	if (sc->sc_state.open)
		hidraw_notify(sc);
	mtx_unlock(sc->sc_mtx);
	destroy_dev(sc->dev);
	sx_destroy(&sc->sc_buf_lock);

	return (0);
}

void
hidraw_intr(void *context, void *buf, uint16_t len)
{
	device_t dev = context;
	struct hidraw_softc *sc = device_get_softc(dev);

	DPRINTFN(5, "len=%d\n", len);
	DPRINTFN(5, "data = %*D\n", len, buf, " ");

	(void) b_to_q(buf, sc->sc_isize, &sc->sc_q);

	hidraw_notify(sc);
}

static int
hidraw_open(struct cdev *dev, int flag, int mode, struct thread *td)
{
	struct hidraw_softc *sc;
	int error;

	sc = dev->si_drv1;
	if (sc == NULL)
		return (ENXIO);

	DPRINTF("sc=%p\n", sc);

	mtx_lock(sc->sc_mtx);
	if (sc->sc_state.open) {
		mtx_unlock(sc->sc_mtx);
		return (EBUSY);
	}
	sc->sc_state.open = true;
	mtx_unlock(sc->sc_mtx);

	error = devfs_set_cdevpriv(sc, hidraw_dtor);
	if (error != 0) {
		mtx_lock(sc->sc_mtx);
		sc->sc_state.open = false;
		mtx_unlock(sc->sc_mtx);
		return (error);
	}

	clist_alloc_cblocks(&sc->sc_q, UHID_BSIZE, UHID_BSIZE);
	sc->sc_buf = malloc(sc->sc_buf_size, M_DEVBUF, M_WAITOK);

	/* Set up interrupt pipe. */
	mtx_lock(sc->sc_mtx);
	hidbus_intr_start(sc->sc_dev);
	sc->sc_state.immed = false;
	sc->sc_async = 0;
	mtx_unlock(sc->sc_mtx);

	return (0);
}

static void
hidraw_dtor(void *data)
{
	struct hidraw_softc *sc = data;

	DPRINTF("sc=%p\n", sc);

	/* Disable interrupts. */
	mtx_lock(sc->sc_mtx);
	hidbus_intr_stop(sc->sc_dev);
	mtx_unlock(sc->sc_mtx);

	ndflush(&sc->sc_q, sc->sc_q.c_cc);
	clist_free_cblocks(&sc->sc_q);

	free(sc->sc_buf, M_DEVBUF);
	sc->sc_buf = NULL;

	mtx_lock(sc->sc_mtx);
	sc->sc_state.open = false;
	sc->sc_async = 0;
	mtx_unlock(sc->sc_mtx);
}

static int
hidraw_read(struct cdev *dev, struct uio *uio, int flag)
{
	struct hidraw_softc *sc;
	int error = 0;
	size_t length;
	u_char buffer[UHID_CHUNK];

	DPRINTFN(1, "\n");

	sc = dev->si_drv1;
	if (sc == NULL)
		return (ENXIO);

	mtx_lock(sc->sc_mtx);
	if (sc->sc_state.immed) {
		mtx_unlock(sc->sc_mtx);
		DPRINTFN(1, "immed\n");

		sx_xlock(&sc->sc_buf_lock);
		error = hid_get_report(sc->sc_dev, sc->sc_buf, sc->sc_isize,
		    NULL, HID_INPUT_REPORT, sc->sc_iid);
		if (error == 0)
			error = uiomove(sc->sc_buf, sc->sc_isize, uio);
		sx_unlock(&sc->sc_buf_lock);
		return (error);
	}

	while (sc->sc_q.c_cc == 0) {
		if (flag & O_NONBLOCK) {
			error = EWOULDBLOCK;
			break;
		}
		sc->sc_state.aslp = true;
		DPRINTFN(5, "sleep on %p\n", &sc->sc_q);
		error = mtx_sleep(&sc->sc_q, sc->sc_mtx, PZERO | PCATCH,
		    "hidrawrd", 0);
		DPRINTFN(5, "woke, error=%d\n", error);
		if (dev->si_drv1 == NULL)
			error = EIO;
		if (error) {
			sc->sc_state.aslp = false;
			break;
		}
	}

	/* Transfer as many chunks as possible. */
	while (sc->sc_q.c_cc > 0 && uio->uio_resid > 0 && !error) {
		length = min(sc->sc_q.c_cc, uio->uio_resid);
		if (length > sizeof(buffer))
			length = sizeof(buffer);

		/* Remove a small chunk from the input queue. */
		(void) q_to_b(&sc->sc_q, buffer, length);
		DPRINTFN(5, "got %lu chars\n", (u_long)length);

		/* Copy the data to the user process. */
		mtx_unlock(sc->sc_mtx);
		error = uiomove(buffer, length, uio);
		mtx_lock(sc->sc_mtx);
	}
	mtx_unlock(sc->sc_mtx);

	return (error);
}

static int
hidraw_write(struct cdev *dev, struct uio *uio, int flag)
{
	struct hidraw_softc *sc;
	int error;
	int size;

	DPRINTFN(1, "\n");

	sc = dev->si_drv1;
	if (sc == NULL)
		return (EIO);

	size = sc->sc_osize;
	if (uio->uio_resid != size)
		return (EINVAL);
	sx_xlock(&sc->sc_buf_lock);
	error = uiomove(sc->sc_buf, size, uio);
	if (error == 0)
		error = hid_write(sc->sc_dev, sc->sc_buf, size);
	sx_unlock(&sc->sc_buf_lock);

	return (error);
}

static int
hidraw_ioctl(struct cdev *dev, u_long cmd, caddr_t addr, int flag,
    struct thread *td)
{
	struct hidraw_softc *sc;
	const struct hid_device_info *hw;
	struct usb_gen_descriptor *ugd;
	struct hidraw_report_descriptor *hrd;
	struct hidraw_devinfo *hdi;
	uint32_t size;
	int id, len;
	int error = 0;

	DPRINTFN(2, "cmd=%lx\n", cmd);

	sc = dev->si_drv1;
	if (sc == NULL)
		return (EIO);

	/* fixed-length ioctls handling */
	switch (cmd) {
	case FIONBIO:
		/* All handled in the upper FS layer. */
		return (0);

	case FIOASYNC:
		mtx_lock(sc->sc_mtx);
		if (*(int *)addr) {
			if (sc->sc_async == NULL) {
				sc->sc_async = td->td_proc;
				DPRINTF("FIOASYNC %p\n", sc->sc_async);
			} else
				error = EBUSY;
		} else
			sc->sc_async = NULL;
		mtx_unlock(sc->sc_mtx);
		return (error);

	/* XXX this is not the most general solution. */
	case TIOCSPGRP:
		mtx_lock(sc->sc_mtx);
		if (sc->sc_async == NULL)
			error = EINVAL;
		else if (*(int *)addr != sc->sc_async->p_pgid)
			error = EPERM;
		mtx_unlock(sc->sc_mtx);
		return (error);

	case USB_GET_REPORT_DESC:
		ugd = (struct usb_gen_descriptor *)addr;
		if (sc->sc_repdesc_size > ugd->ugd_maxlen) {
			size = ugd->ugd_maxlen;
		} else {
			size = sc->sc_repdesc_size;
		}
		ugd->ugd_actlen = size;
		if (ugd->ugd_data == NULL)
			return (0);		/* descriptor length only */
		return (copyout(sc->sc_repdesc, ugd->ugd_data, size));

	case USB_SET_IMMED:
		if (!(flag & FREAD))
			return (EPERM);
		if (*(int *)addr) {
			/* XXX should read into ibuf, but does it matter? */
			sx_xlock(&sc->sc_buf_lock);
			error = hid_get_report(sc->sc_dev, sc->sc_buf,
			    sc->sc_isize, NULL, UHID_INPUT_REPORT, sc->sc_iid);
			sx_unlock(&sc->sc_buf_lock);
			if (error)
				return (EOPNOTSUPP);

			mtx_lock(sc->sc_mtx);
			sc->sc_state.immed = true;
			mtx_unlock(sc->sc_mtx);
		} else {
			mtx_lock(sc->sc_mtx);
			sc->sc_state.immed = false;
			mtx_unlock(sc->sc_mtx);
		}
		return (0);

	case USB_GET_REPORT:
		if (!(flag & FREAD))
			return (EPERM);
		ugd = (struct usb_gen_descriptor *)addr;
		switch (ugd->ugd_report_type) {
		case UHID_INPUT_REPORT:
			size = sc->sc_isize;
			id = sc->sc_iid;
			break;
		case UHID_OUTPUT_REPORT:
			size = sc->sc_osize;
			id = sc->sc_oid;
			break;
		case UHID_FEATURE_REPORT:
			size = sc->sc_fsize;
			id = sc->sc_fid;
			break;
		default:
			return (EINVAL);
		}
		if (id != 0)
			copyin(ugd->ugd_data, &id, 1);
		size = MIN(ugd->ugd_maxlen, size);
		sx_xlock(&sc->sc_buf_lock);
		error = hid_get_report(sc->sc_dev, sc->sc_buf, size, NULL,
		    ugd->ugd_report_type, id);
		if (!error)
			error = copyout(sc->sc_buf, ugd->ugd_data, size);
		sx_unlock(&sc->sc_buf_lock);
		return (error);

	case USB_SET_REPORT:
		if (!(flag & FWRITE))
			return (EPERM);
		ugd = (struct usb_gen_descriptor *)addr;
		switch (ugd->ugd_report_type) {
		case UHID_INPUT_REPORT:
			size = sc->sc_isize;
			id = sc->sc_iid;
			break;
		case UHID_OUTPUT_REPORT:
			size = sc->sc_osize;
			id = sc->sc_oid;
			break;
		case UHID_FEATURE_REPORT:
			size = sc->sc_fsize;
			id = sc->sc_fid;
			break;
		default:
			return (EINVAL);
		}
		size = MIN(ugd->ugd_maxlen, size);
		sx_xlock(&sc->sc_buf_lock);
		copyin(ugd->ugd_data, sc->sc_buf, size);
		if (id != 0)
			id = sc->sc_buf[0];
		error = hid_set_report(sc->sc_dev, sc->sc_buf, size,
		    ugd->ugd_report_type, id);
		sx_unlock(&sc->sc_buf_lock);
		return (error);

	case USB_GET_REPORT_ID:
		*(int *)addr = 0;	/* XXX: we only support reportid 0? */
		return (0);

	case HIDIOCGRDESCSIZE:
		*(int *)addr = sc->sc_repdesc_size;
		return (0);

	case HIDIOCGRDESC:
		hrd = *(struct hidraw_report_descriptor **)addr;
		error = copyin(&hrd->size, &size, sizeof(uint32_t));
		if (error)
			return (error);
		/*
		 * HID_MAX_DESCRIPTOR_SIZE-1 is a limit of report descriptor
		 * size in current Linux implementation.
		 */
		if (size >= HID_MAX_DESCRIPTOR_SIZE)
			return (EINVAL);
		size = MIN(size, sc->sc_repdesc_size);
		return (copyout(sc->sc_repdesc, hrd->value, size));

	case HIDIOCGRAWINFO:
		hw = hid_get_device_info(sc->sc_dev);
		hdi = (struct hidraw_devinfo *)addr;
		hdi->bustype = hw->idBus;
		hdi->vendor = hw->idVendor;
		hdi->product = hw->idProduct;
		return (0);
	}

	/* variable-length ioctls handling */
	len = IOCPARM_LEN(cmd);
	switch (IOCBASECMD(cmd)) {
	case HIDIOCGRAWNAME(0):
		hw = hid_get_device_info(sc->sc_dev);
		strlcpy(addr, hw->name, len);
		return (0);

	case HIDIOCGRAWPHYS(0):
		strlcpy(addr, device_get_nameunit(sc->sc_dev), len);
		return (0);

	case HIDIOCSFEATURE(0):
	case HIDIOCGFEATURE(0):
		return (EOPNOTSUPP);

	case HIDIOCGRAWUNIQ(0):
		hw = hid_get_device_info(sc->sc_dev);
		strlcpy(addr, hw->serial, len);
		return (0);
	}

	return (EINVAL);
}

static int
hidraw_poll(struct cdev *dev, int events, struct thread *td)
{
	struct hidraw_softc *sc;
	int revents = 0;

	sc = dev->si_drv1;
	if (sc == NULL)
		return (POLLHUP);

	if (events & (POLLOUT | POLLWRNORM))
		revents |= events & (POLLOUT | POLLWRNORM);
	if (events & (POLLIN | POLLRDNORM)) {
		mtx_lock(sc->sc_mtx);
		if (sc->sc_q.c_cc > 0)
			revents |= events & (POLLIN | POLLRDNORM);
		else {
			sc->sc_state.sel = true;
			selrecord(td, &sc->sc_rsel);
		}
		mtx_unlock(sc->sc_mtx);
	}

	return (revents);
}

static void
hidraw_notify(struct hidraw_softc *sc)
{

	mtx_assert(sc->sc_mtx, MA_OWNED);

	if (sc->sc_state.aslp) {
		sc->sc_state.aslp = false;
		DPRINTFN(5, "waking %p\n", &sc->sc_q);
		wakeup(&sc->sc_q);
	}
	if (sc->sc_state.sel) {
		sc->sc_state.sel = false;
		selwakeuppri(&sc->sc_rsel, PZERO);
	}
	if (sc->sc_async != NULL) {
		DPRINTFN(3, "sending SIGIO %p\n", sc->sc_async);
		PROC_LOCK(sc->sc_async);
		kern_psignal(sc->sc_async, SIGIO);
		PROC_UNLOCK(sc->sc_async);
	}
}

static device_method_t hidraw_methods[] = {
	/* Device interface */
	DEVMETHOD(device_identify,	hidraw_identify),
	DEVMETHOD(device_probe,		hidraw_probe),
	DEVMETHOD(device_attach,	hidraw_attach),
	DEVMETHOD(device_detach,	hidraw_detach),

	{ 0, 0 }
};

static driver_t hidraw_driver = {
	"hidraw",
	hidraw_methods,
	sizeof(struct hidraw_softc)
};

static devclass_t hidraw_devclass;

DRIVER_MODULE(hidraw, hidbus, hidraw_driver, hidraw_devclass, NULL, 0);
MODULE_DEPEND(hidraw, hidbus, 1, 1, 1);
MODULE_DEPEND(hidraw, hid, 1, 1, 1);
MODULE_VERSION(hidraw, 1);
