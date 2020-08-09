# iichid - Generic HID layer for FreeBSD

iichid is a set of abstract HID drivers for FreeBSD. All the drivers use evdev
protocol to communicate with userland applications like libinput and
xf86-input-evdev. hidbus is used to connect HID drivers with transport
backends (usbhid for USB and iichid for I2C) and allows multiple HID drivers
to attach to a single physical device.

Current drivers and respective HID devices supported by them are following:

* **hms** - HID mouses. Physical relative devices with roller or optical sensor
  are supported as well as virtual absolute devices like VBox, Bhyve and VMWare
  mices.
* **hmt** - MS Windows 7+ compatible Multi-touch devices like touchscreens
  and precission touchpads.
* **hconf** - Mouse/touchpad mode switches on precission touchpads.
* **hkbd** - HID keyboards. AT-key subset.
* **hskbd** - Simple evdev-only HID keyboard driver.
* **hpen**  - Generic / MS Windows compatible HID pen tablets.
* **hcons** - Consumer page AKA Multimedia keys.
* **hsctrl** - System Controls page (Power/Sleep keys).
* **hgame** - Game controllers including Xbox360-compatible and joysticks.
* **hidraw** - Export raw HID data in uhid(4) and Linux hidraw-compatible way.

## System requirements

FreeBSD 12.1+. Recent (any from 2020) CURRENT or 12-STABLE are preferred.

## Downloading

This project does not have a special home page. The source code and the
issue tracker are hosted on the Github:

https://github.com/wulf7/iichid

## Building

To build driver, cd in to extracted archive directory and type

```
$ make
```

You need the sources of the running operating system under **/usr/src**

## Installing

To install file already built just type:

```
$ sudo make install
```

and you will get the compiled module installed in **/boot/modules**

To load the module at a boot time, add both I2C driver module (it is usually
an **ig4**) and HID module **iichid** to **kld_list** variable in
**/etc/rc.conf**.

To handle keyboards in single user mode and at the early stages
of booting process, iichid should be loaded at kernel startup.
Add following lines to **/boot/loader.conf** in that case:

```
ig4_load="YES"
iicbus_load="YES"
iichid_load="YES"
```

## Configure Xorg

Generally speaking, it is a bad idea to use static Xorg configuration for
evdev devices due to dynamic unit number assignment. The simplest way
to get new devices recognized by Xorg-server is to rebuild it with DEVD
(since 1.20.7) or UDEV autoconfiguration backend enabled.

Static configuration is still possible. Add following snippet to a file under
**/usr/local/etc/X11/xorg.conf.d/**

```
Section "InputDevice"
	Identifier "HID device"
	Driver "libinput"
	Option "Device" "/dev/input/event6"
	Option "AutoServerLayout" "true"
EndSection
```

You may need to run **sudo libinput list-devices** to find out unit number
belonging to given device. Note that it can change across reboots.

## usbhid

USB transport backend can (and usually does) interfere with OS built-in USB
HID drivers like **uhid**, **ukbd**, **ums** and **wmt**. Which one will be
active is depend on the order of loading, so genarally you should load
**iichid.ko** from bootloader. In the case if the module is loaded
with **kldload** it is necessary to issue

```
$ sudo usbconfig -d ugenX.X reset
```

command to force ugenX.X device reprobe at operating system run time. ugenX.X
to reprobe can be found with issuing of simple **usbconfig** command:

```
$ sudo usbconfig
```

It is possible to build **iichid** with USB support disabled with following
command:

```
$ make -DDISABLE_USBHID
```

## I2C transport backend sampling (polling) mode

Currently **iichid** is unable to utilize GPIO interrupts on i386 and amd64
platforms due to absence of INTRNG support in PIC drivers and can not use any
interrupts on 12.1-RELEASE due to inability of **ig4** driver to work in
ithread context in 12.1. In this case it fallbacks to so called sampling
mode with periodic polling of hardware by driver. It is possible to check
mode with following command:
```
$ sysctl dev.iichid.0.sampling_rate_slow
```
Any positive number returned means that sampling mode is enabled.

Unfortunatelly, using of sampling mode leads to lose of some data and
often results in glitches. Most known are "drift of inactive mouse pointer"
and "stuck of single finger touch". **iichid** has some internal hacks to
workaround them but they are not reliable.

## Choosing of optimal sampling rate for I2C transport.

Sampling rate is set to 60Hz by default. Although mostly it just works, in
some cases e.g. if device internal scan rate lower than polling frequence
it results in reading of empty reports, doubling of them or other unwanted
effects.

Typically driver polling frequency should be set to about 0.9 of device
internal scan rate. The simplest way to measure it is to run a Linux live
distro from USB flash than execute evemu-record utility and choose your I2C
device. Than you should convert inter-report interval duration to Hz and
set 0.9 of that value as sampling_rate_fast parameter with following command
```
$ sudo sysctl dev.iichid.0.sampling_rate_fast=<poll freq>
```

If your device is supported by **hmt** driver, scan rate can be obtained by
analyze of hardware timestamps. To enable them HQ_MT_TIMESTAMP quirk should
be set for HID device. At first get vendor and product IDs with following
command:
```
$ devinfo -rv | grep 'hmt.* bus=0x18'
```
It will return something like:
   hmt0 pnpinfo page=0x000d usage=0x0004 bus=0x18 vendor=0x06cb product=0x1941 ...

Then add following line to **/boot/loader.conf** replacing 0x6cb and 0x1941
with vendor and product values from previous command output.
```
hw.hid.quirk.0="0x18 **0x6cb** **0x1941** 0 0xffff HQ_MT_TIMESTAMP"
```
That will enable output of hardware timestamps in hmt driver after reboot.
Reboot than run any evdev client e.g. libinput XOrg driver, evemu-record and
so on and touch device surface. Scan rate is available as r/o sysctl now.
```
$ sysctl dev.hmt.0.scan_rate
```

It is possible to use double of scan_rate as sampling rate with increasing
of dev.iichid.0.sampling_hysteresis to 3 or 4 to filter out missing samples.
Tuning of this value requires enabling of debug output in I2C transport
and lies out of scope of this README. Generally, sampling_hysteresis should
be left as 1 if sampling frequency lower or equal 0.9 of device scan rate.

## Bug reporting

You can report bugs at 'Project Issues' page
https://github.com/wulf7/iichid/issues
It is recommended to enclose console output of 'kldload iichid.ko' command
with your report.

Some additional information that can be helpful especially if device has not
been detected at all can be obtained with following commands:

```
$ devinfo -rv	# Very verbose
$ pciconf -lv
$ sudo usbconfig
$ dmesg
```
