/*-
 * SPDX-License-Identifier: BSD-2-Clause-NetBSD
 *
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 * Copyright (c) 2020 Vladimir Kondratyev <wulf@FreeBSD.org>
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

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

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

#define	HIDRAW_INDEX	0xFF	/* Arbitrary high value */

struct hidraw_softc {
	device_t sc_dev;		/* base device */

	struct mtx *sc_mtx;		/* hidbus private mutex */

	int sc_isize;
	int sc_osize;
	int sc_fsize;
	uint8_t sc_iid;
	uint8_t sc_oid;
	uint8_t sc_fid;

	u_char *sc_buf;			/* user request proxy buffer */
	int sc_buf_size;
	struct sx sc_buf_lock;

	void *sc_repdesc;
	int sc_repdesc_size;

	int sc_rdsize;
	int sc_wrsize;
	uint8_t *sc_q;
	uint16_t *sc_qlen;
	int sc_head;
	int sc_tail;

	struct selinfo sc_rsel;
	struct proc *sc_async;	/* process that wants SIGIO */
	struct {			/* driver state */
		bool	open:1;		/* device is open */
		bool	aslp:1;		/* waiting for device data in read() */
		bool	sel:1;		/* waiting for device data in poll() */
		bool	owfl:1;		/* input queue is about to overflow */
		bool	immed:1;	/* return read data immediately */
		bool	uhid:1;		/* driver switched in to uhid mode */
		u_char	reserved:2;
	} sc_state;
	int sc_fflags;			/* access mode for open lifetime */

	struct cdev *dev;
};

static d_open_t		hidraw_open;
static d_read_t		hidraw_read;
static d_write_t	hidraw_write;
static d_ioctl_t	hidraw_ioctl;
static d_poll_t		hidraw_poll;
static d_kqfilter_t	hidraw_kqfilter;

static d_priv_dtor_t	hidraw_dtor;

static struct cdevsw hidraw_cdevsw = {
	.d_version =	D_VERSION,
	.d_open =	hidraw_open,
	.d_read =	hidraw_read,
	.d_write =	hidraw_write,
	.d_ioctl =	hidraw_ioctl,
	.d_poll =	hidraw_poll,
	.d_kqfilter = 	hidraw_kqfilter,
	.d_name =	"hidraw",
};

static hid_intr_t	hidraw_intr;

static device_identify_t hidraw_identify;
static device_probe_t	hidraw_probe;
static device_attach_t	hidraw_attach;
static device_detach_t	hidraw_detach;

static int		hidraw_kqread(struct knote *, long);
static void		hidraw_kqdetach(struct knote *);
static void		hidraw_notify(struct hidraw_softc *);

static struct filterops hidraw_filterops_read = {
	.f_isfd =	1,
	.f_detach =	hidraw_kqdetach,
	.f_event =	hidraw_kqread,
};

static void
hidraw_identify(driver_t *driver, device_t parent)
{
	device_t child;

	if (device_find_child(parent, "hidraw", -1) == NULL) {
		child = BUS_ADD_CHILD(parent, 0, "hidraw",
		    device_get_unit(parent));
		if (child != NULL)
			hidbus_set_index(child, HIDRAW_INDEX);
	}
}

static int
hidraw_probe(device_t self)
{

	if (hidbus_get_index(self) != HIDRAW_INDEX)
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
	const struct hid_device_info *hw = hid_get_device_info(self);
	struct make_dev_args mda;
	uint16_t size;
	void *desc;
	int error;

	hidbus_set_desc(self, "Raw HID Device");

	sc->sc_dev = self;
	sc->sc_mtx = hidbus_get_lock(self);

	/* Hidraw mode does not require report descriptor to work */
	if (hid_get_report_descr(sc->sc_dev, &desc, &size) == 0) {
		sc->sc_repdesc = desc;
		sc->sc_repdesc_size = size;
	} else
		device_printf(self, "no report descriptor\n");

	sx_init(&sc->sc_buf_lock, "hidraw sx");
	knlist_init_mtx(&sc->sc_rsel.si_note, sc->sc_mtx);

	sc->sc_isize = hid_report_size(desc, size, hid_input,   &sc->sc_iid);
	sc->sc_osize = hid_report_size(desc, size, hid_output,  &sc->sc_oid);
	sc->sc_fsize = hid_report_size(desc, size, hid_feature, &sc->sc_fid);

	sc->sc_rdsize = hw->rdsize;
	sc->sc_wrsize = hw->wrsize;
	sc->sc_buf_size = MAX(sc->sc_isize, MAX(sc->sc_osize, sc->sc_fsize));

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
		hidraw_detach(self);
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

	if (sc->dev != NULL) {
		sc->dev->si_drv1 = NULL;
		destroy_dev(sc->dev);
	}
	sx_destroy(&sc->sc_buf_lock);
	/* Avoid knlist_clear KASSERTion when hidbus lock is a newbus lock */
	mtx_lock(sc->sc_mtx);
	knlist_clear(&sc->sc_rsel.si_note, 1);
	mtx_unlock(sc->sc_mtx);
	knlist_destroy(&sc->sc_rsel.si_note);
	seldrain(&sc->sc_rsel);

	return (0);
}

void
hidraw_intr(void *context, void *buf, uint16_t len)
{
	device_t dev = context;
	struct hidraw_softc *sc = device_get_softc(dev);
	int next;

	DPRINTFN(5, "len=%d\n", len);
	DPRINTFN(5, "data = %*D\n", len, buf, " ");

	next = (sc->sc_tail + 1) % HIDRAW_BUFFER_SIZE;
	if (next == sc->sc_head)
		return;

	bcopy(buf, sc->sc_q + sc->sc_tail * sc->sc_rdsize, len);
	sc->sc_qlen[sc->sc_tail] = len;
	sc->sc_tail = next;

	if ((next + 1) % HIDRAW_BUFFER_SIZE == sc->sc_head) {
		DPRINTFN(3, "queue overflown. Stop intr");
		sc->sc_state.owfl = true;
		hidbus_intr_stop(sc->sc_dev);
	}

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

	sx_xlock(&sc->sc_buf_lock);
	sc->sc_q = malloc(sc->sc_rdsize * HIDRAW_BUFFER_SIZE, M_DEVBUF,
	    M_ZERO | M_WAITOK);
	sc->sc_qlen = malloc(sizeof(uint16_t) * HIDRAW_BUFFER_SIZE, M_DEVBUF,
	    M_ZERO | M_WAITOK);
	sc->sc_buf = malloc(sc->sc_buf_size, M_DEVBUF, M_WAITOK);
	sx_unlock(&sc->sc_buf_lock);

	/* Set up interrupt pipe. */
	mtx_lock(sc->sc_mtx);
	hidbus_intr_start(sc->sc_dev);
	sc->sc_state.immed = false;
	sc->sc_async = 0;
	sc->sc_state.uhid = false;	/* hidraw mode is default */
	sc->sc_state.owfl = false;
	sc->sc_head = sc->sc_tail = 0;
	sc->sc_fflags = flag;
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
	if (!sc->sc_state.owfl)
		hidbus_intr_stop(sc->sc_dev);
	sc->sc_tail = sc->sc_head = 0;
	mtx_unlock(sc->sc_mtx);

	sx_xlock(&sc->sc_buf_lock);
	free(sc->sc_q, M_DEVBUF);
	free(sc->sc_qlen, M_DEVBUF);
	free(sc->sc_buf, M_DEVBUF);
	sc->sc_q = sc->sc_buf = NULL;
	sx_unlock(&sc->sc_buf_lock);

	mtx_lock(sc->sc_mtx);
	sc->sc_state.open = false;
	/* Wake everyone */
	hidraw_notify(sc);
	sc->sc_async = 0;
	mtx_unlock(sc->sc_mtx);
}

static int
hidraw_read(struct cdev *dev, struct uio *uio, int flag)
{
	struct hidraw_softc *sc;
	int head, error = 0;
	size_t length;

	DPRINTFN(1, "\n");

	sc = dev->si_drv1;
	if (sc == NULL)
		return (ENXIO);

	mtx_lock(sc->sc_mtx);
	if (!sc->sc_state.open) {
		mtx_unlock(sc->sc_mtx);
		return (EIO);
	}
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

	while (sc->sc_tail == sc->sc_head) {
		if (flag & O_NONBLOCK) {
			error = EWOULDBLOCK;
			break;
		}
		sc->sc_state.aslp = true;
		DPRINTFN(5, "sleep on %p\n", &sc->sc_q);
		error = mtx_sleep(&sc->sc_q, sc->sc_mtx, PZERO | PCATCH,
		    "hidrawrd", 0);
		DPRINTFN(5, "woke, error=%d\n", error);
		if (!sc->sc_state.open)
			error = EIO;
		if (error) {
			sc->sc_state.aslp = false;
			break;
		}
	}

	while (sc->sc_tail != sc->sc_head && uio->uio_resid > 0 && !error) {
		head = sc->sc_head;
		length = min(uio->uio_resid,
		    sc->sc_state.uhid ? sc->sc_isize : sc->sc_qlen[head]);
		DPRINTFN(5, "got %lu chars\n", (u_long)length);
		/* Remove a small chunk from the input queue. */
		sc->sc_head = (head + 1) % HIDRAW_BUFFER_SIZE;
		mtx_unlock(sc->sc_mtx);

		/* Copy the data to the user process. */
		sx_slock(&sc->sc_buf_lock);
		if (sc->sc_q == NULL) {
			sx_unlock(&sc->sc_buf_lock);
			return (0);
		}
		error = uiomove(sc->sc_q + head * sc->sc_rdsize, length, uio);
		sx_unlock(&sc->sc_buf_lock);

		mtx_lock(sc->sc_mtx);
		if (sc->sc_state.owfl) {
			DPRINTFN(3, "queue freed. Start intr");
			sc->sc_state.owfl = false;
			hidbus_intr_start(sc->sc_dev);
		}
		/*
		 * In uhid mode transfer as many chunks as possible. Hidraw
		 * packets are transferred one by one due to different length.
		 */
		if (!sc->sc_state.uhid)
			break;
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
	size_t buf_offset;
	uint8_t id = 0;

	DPRINTFN(1, "\n");

	sc = dev->si_drv1;
	if (sc == NULL)
		return (EIO);

	if (sc->sc_osize == 0)
		return (EOPNOTSUPP);

	buf_offset = 0;
	if (sc->sc_state.uhid) {
		size = sc->sc_osize;
		if (uio->uio_resid != size)
			return (EINVAL);
	} else {
		size = uio->uio_resid;
		if (size < 2)
			return (EINVAL);
		/* Strip leading 0 if the device doesnt use numbered reports */
		error = uiomove(&id, 1, uio);
		if (error)
			return (error);
		if (id != 0)
			buf_offset++;
		else
			size--;
		/* Check if underlying driver could process this request */
		if (size > sc->sc_wrsize)
			return (ENOBUFS);
	}
	sx_xlock(&sc->sc_buf_lock);
	if (sc->sc_buf != NULL) {
		/* Expand buf if needed as hidraw allows writes of any size. */
		if (size > sc->sc_buf_size) {
			free(sc->sc_buf, M_DEVBUF);
			sc->sc_buf = malloc(sc->sc_wrsize, M_DEVBUF, M_WAITOK);
			sc->sc_buf_size = sc->sc_wrsize;
		}
		sc->sc_buf[0] = id;
		error = uiomove(sc->sc_buf + buf_offset, uio->uio_resid, uio);
	} else
		error = EIO;
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
		if (sc->sc_repdesc_size == 0)
			return (EOPNOTSUPP);
		mtx_lock(sc->sc_mtx);
		sc->sc_state.uhid = true;
		mtx_unlock(sc->sc_mtx);
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
		if (!(sc->sc_fflags & FREAD))
			return (EPERM);
		if (*(int *)addr) {
			/* XXX should read into ibuf, but does it matter? */
			sx_xlock(&sc->sc_buf_lock);
			if (sc->sc_buf == NULL) {
				sx_unlock(&sc->sc_buf_lock);
				return (EIO);
			}
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
		if (!(sc->sc_fflags & FREAD))
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
		if (sc->sc_buf == NULL) {
			sx_unlock(&sc->sc_buf_lock);
			return (EIO);
		}
		error = hid_get_report(sc->sc_dev, sc->sc_buf, size, NULL,
		    ugd->ugd_report_type, id);
		if (!error)
			error = copyout(sc->sc_buf, ugd->ugd_data, size);
		sx_unlock(&sc->sc_buf_lock);
		return (error);

	case USB_SET_REPORT:
		if (!(sc->sc_fflags & FWRITE))
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
		if (sc->sc_buf == NULL) {
			sx_unlock(&sc->sc_buf_lock);
			return (EIO);
		}
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
		if (!(sc->sc_fflags & FWRITE))
			return (EPERM);
		if (len < 2)
			return (EINVAL);
		id = *(uint8_t *)addr;
		if (id == 0) {
			addr = (uint8_t *)addr + 1;
			len--;
		}
		return (hid_set_report(sc->sc_dev, addr, len,
		    HID_FEATURE_REPORT, id));

	case HIDIOCGFEATURE(0):
		if (!(sc->sc_fflags & FREAD))
			return (EPERM);
		if (len < 2)
			return (EINVAL);
		id = *(uint8_t *)addr;
		if (id == 0) {
			addr = (uint8_t *)addr + 1;
			len--;
		}
		return (hid_get_report(sc->sc_dev, addr, len, NULL,
		    HID_FEATURE_REPORT, id));

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
		return (POLLERR);

	mtx_lock(sc->sc_mtx);
	if (!sc->sc_state.open) {
		mtx_unlock(sc->sc_mtx);
		return (POLLHUP);
	}
	if (events & (POLLOUT | POLLWRNORM) && (sc->sc_fflags & FWRITE))
		revents |= events & (POLLOUT | POLLWRNORM);
	if (events & (POLLIN | POLLRDNORM) && (sc->sc_fflags & FREAD)) {
		if (sc->sc_head != sc->sc_tail)
			revents |= events & (POLLIN | POLLRDNORM);
		else {
			sc->sc_state.sel = true;
			selrecord(td, &sc->sc_rsel);
		}
	}
	mtx_unlock(sc->sc_mtx);

	return (revents);
}

static int
hidraw_kqfilter(struct cdev *dev, struct knote *kn)
{
	struct hidraw_softc *sc;

	sc = dev->si_drv1;
	if (sc == NULL)
		return (ENXIO);

	switch(kn->kn_filter) {
	case EVFILT_READ:
		if (sc->sc_fflags & FREAD) {
			kn->kn_fop = &hidraw_filterops_read;
			break;
		}
		/* FALLTHROUGH */
	default:
		return(EINVAL);
	}
	kn->kn_hook = sc;

	knlist_add(&sc->sc_rsel.si_note, kn, 0);
	return (0);
}

static int
hidraw_kqread(struct knote *kn, long hint)
{
	struct hidraw_softc *sc;
	int ret;

	sc = kn->kn_hook;

	mtx_assert(sc->sc_mtx, MA_OWNED);

	if (!sc->sc_state.open) {
		kn->kn_flags |= EV_EOF;
		ret = 1;
	} else
		ret = (sc->sc_head != sc->sc_tail) ? 1 : 0;

	return (ret);
}

static void
hidraw_kqdetach(struct knote *kn)
{
	struct hidraw_softc *sc;

	sc = kn->kn_hook;
	knlist_remove(&sc->sc_rsel.si_note, kn, 0);
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
	KNOTE_LOCKED(&sc->sc_rsel.si_note, 0);
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
MODULE_DEPEND(hidraw, usb, 1, 1, 1);
MODULE_VERSION(hidraw, 1);
