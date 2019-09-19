# $FreeBSD$

#.PATH:		${SRCTOP}/sys/dev/iicbus/input

KMOD	= iichid
SRCS	= iichid.c imt.c hidbus.c hidbus.h hid_if.c hid_if.h
SRCS	+= acpi_if.h bus_if.h device_if.h iicbus_if.h vnode_if.h
SRCS	+= opt_acpi.h opt_usb.h
#CFLAGS	+= -DHAVE_ACPI_IICBUS -DHAVE_IG4_POLLING

.include <bsd.kmod.mk>
