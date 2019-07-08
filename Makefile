# $FreeBSD$

#.PATH:		${SRCTOP}/sys/dev/iicbus/input

KMOD	= iichid
SRCS	= iichid.c imt.c
SRCS	+= device_if.h acpi_if.h bus_if.h iicbus_if.h vnode_if.h
SRCS	+= opt_acpi.h opt_usb.h

.include <bsd.kmod.mk>
