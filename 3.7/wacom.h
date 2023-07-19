/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  USB Wacom tablet support
 *
 *  Copyright (c) 2000-2004 Vojtech Pavlik	<vojtech@ucw.cz>
 *  Copyright (c) 2000 Andreas Bach Aaen	<abach@stofanet.dk>
 *  Copyright (c) 2000 Clifford Wolf		<clifford@clifford.at>
 *  Copyright (c) 2000 Sam Mosel		<sam.mosel@computer.org>
 *  Copyright (c) 2000 James E. Blair		<corvus@gnu.org>
 *  Copyright (c) 2000 Daniel Egger		<egger@suse.de>
 *  Copyright (c) 2001 Frederic Lepied		<flepied@mandrakesoft.com>
 *  Copyright (c) 2004 Panagiotis Issaris	<panagiotis.issaris@mech.kuleuven.ac.be>
 *  Copyright (c) 2002-2011 Ping Cheng		<pingc@wacom.com>
 *
 *  ChangeLog:
 *      v0.1 (vp)  - Initial release
 *      v0.2 (aba) - Support for all buttons / combinations
 *      v0.3 (vp)  - Support for Intuos added
 *	v0.4 (sm)  - Support for more Intuos models, menustrip
 *			relative mode, proximity.
 *	v0.5 (vp)  - Big cleanup, nifty features removed,
 *			they belong in userspace
 *	v1.8 (vp)  - Submit URB only when operating, moved to CVS,
 *			use input_report_key instead of report_btn and
 *			other cleanups
 *	v1.11 (vp) - Add URB ->dev setting for new kernels
 *	v1.11 (jb) - Add support for the 4D Mouse & Lens
 *	v1.12 (de) - Add support for two more inking pen IDs
 *	v1.14 (vp) - Use new USB device id probing scheme.
 *		     Fix Wacom Graphire mouse wheel
 *	v1.18 (vp) - Fix mouse wheel direction
 *		     Make mouse relative
 *      v1.20 (fl) - Report tool id for Intuos devices
 *                 - Multi tools support
 *                 - Corrected Intuos protocol decoding (airbrush, 4D mouse, lens cursor...)
 *                 - Add PL models support
 *		   - Fix Wacom Graphire mouse wheel again
 *	v1.21 (vp) - Removed protocol descriptions
 *		   - Added MISC_SERIAL for tool serial numbers
 *	      (gb) - Identify version on module load.
 *    v1.21.1 (fl) - added Graphire2 support
 *    v1.21.2 (fl) - added Intuos2 support
 *                 - added all the PL ids
 *    v1.21.3 (fl) - added another eraser id from Neil Okamoto
 *                 - added smooth filter for Graphire from Peri Hankey
 *                 - added PenPartner support from Olaf van Es
 *                 - new tool ids from Ole Martin Bjoerndalen
 *	v1.29 (pc) - Add support for more tablets
 *		   - Fix pressure reporting
 *	v1.30 (vp) - Merge 2.4 and 2.5 drivers
 *		   - Since 2.5 now has input_sync(), remove MSC_SERIAL abuse
 *		   - Cleanups here and there
 *    v1.30.1 (pi) - Added Graphire3 support
 *	v1.40 (pc) - Add support for several new devices, fix eraser reporting, ...
 *	v1.43 (pc) - Added support for Cintiq 21UX
 *		   - Fixed a Graphire bug
 *		   - Merged wacom_intuos3_irq into wacom_intuos_irq
 *	v1.44 (pc) - Added support for Graphire4, Cintiq 710, Intuos3 6x11, etc.
 *		   - Report Device IDs
 *      v1.45 (pc) - Added support for DTF 521, Intuos3 12x12 and 12x19
 *                 - Minor data report fix
 *      v1.46 (pc) - Split wacom.c into wacom_sys.c and wacom_wac.c,
 *		   - where wacom_sys.c deals with system specific code,
 *		   - and wacom_wac.c deals with Wacom specific code
 *		   - Support Intuos3 4x6
 *      v1.47 (pc) - Added support for Bamboo
 *      v1.48 (pc) - Added support for Bamboo1, BambooFun, and Cintiq 12WX
 *      v1.49 (pc) - Added support for USB Tablet PC (0x90, 0x93, and 0x9A)
 *      v1.50 (pc) - Fixed a TabletPC touch bug in 2.6.28
 *      v1.51 (pc) - Added support for Intuos4
 *      v1.52 (pc) - Query Wacom data upon system resume
 *                 - add defines for features->type
 *                 - add new devices (0x9F, 0xE2, and 0XE3)
 */

#ifndef WACOM_H
#define WACOM_H

#include "../config.h"
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/kfifo.h>
#include <linux/usb/input.h>
#include <linux/power_supply.h>
#include <asm/unaligned.h>

/*
 * Version Information
 */
#ifndef WACOM_VERSION_SUFFIX
#define WACOM_VERSION_SUFFIX ""
#endif
#define DRIVER_VERSION "v1.53"WACOM_VERSION_SUFFIX
#define DRIVER_AUTHOR "Vojtech Pavlik <vojtech@ucw.cz>"
#define DRIVER_DESC "USB Wacom tablet driver"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

#define USB_VENDOR_ID_WACOM	0x056a
#define USB_VENDOR_ID_LENOVO	0x17ef

#ifndef fallthrough
#  if defined __has_attribute
#    if __has_attribute(__fallthrough__)
#      define fallthrough                    __attribute__((__fallthrough__))
#    endif
#  endif
#endif
#ifndef fallthrough
#  define fallthrough                    do {} while (0)  /* fallthrough */
#endif

#ifdef WACOM_POWERSUPPLY_41
#define WACOM_POWERSUPPLY_DEVICE(ps) (ps)
#define WACOM_POWERSUPPLY_REF(ps) (ps)
#define WACOM_POWERSUPPLY_DESC(ps) (ps##_desc)
#else
#define WACOM_POWERSUPPLY_DEVICE(ps) ((ps).dev)
#define WACOM_POWERSUPPLY_REF(ps) (&(ps))
#define WACOM_POWERSUPPLY_DESC(ps) (ps)
#endif

enum wacom_worker {
	WACOM_WORKER_WIRELESS,
	WACOM_WORKER_BATTERY,
	WACOM_WORKER_REMOTE,
};

struct wacom_battery {
	struct wacom *wacom;
#ifdef WACOM_POWERSUPPLY_41
	struct power_supply_desc bat_desc;
	struct power_supply *battery;
#else
	struct power_supply battery;
#endif
	char bat_name[WACOM_NAME_MAX];
	int bat_status;
	int battery_capacity;
	int bat_charging;
	int bat_connected;
	int ps_connected;
};

struct wacom_remote {
	spinlock_t remote_lock;
	struct kfifo remote_fifo;
	struct kobject *remote_dir;
	struct {
		struct attribute_group group;
		u32 serial;
		struct input_dev *input;
		bool registered;
		struct wacom_battery battery;
	} remotes[WACOM_MAX_REMOTES];
};

struct wacom {
	dma_addr_t data_dma;
	struct usb_device *usbdev;
	struct usb_interface *intf;
	struct urb *irq;
	struct wacom_wac wacom_wac;
	struct mutex lock;
	struct work_struct wireless_work;
	struct work_struct battery_work;
	struct work_struct remote_work;
	struct wacom_remote *remote;
	bool open;
	char phys[32];
	struct wacom_led {
		u8 select[5]; /* status led selector (0..3) */
		u8 llv;       /* status led brightness no button (1..127) */
		u8 hlv;       /* status led brightness button pressed (1..127) */
		u8 img_lum;   /* OLED matrix display brightness */
	} led;
	struct wacom_battery battery;
};

static inline void wacom_schedule_work(struct wacom_wac *wacom_wac,
				       enum wacom_worker which)
{
	struct wacom *wacom = container_of(wacom_wac, struct wacom, wacom_wac);

	switch (which) {
	case WACOM_WORKER_WIRELESS:
		schedule_work(&wacom->wireless_work);
		break;
	case WACOM_WORKER_BATTERY:
		schedule_work(&wacom->battery_work);
		break;
	case WACOM_WORKER_REMOTE:
		schedule_work(&wacom->remote_work);
		break;
	}
}

extern const struct usb_device_id wacom_ids[];

void wacom_wac_irq(struct wacom_wac *wacom_wac, size_t len);
void wacom_setup_device_quirks(struct wacom *wacom);
int wacom_setup_input_capabilities(struct input_dev *input_dev,
				   struct wacom_wac *wacom_wac);
void wacom_battery_work(struct work_struct *work);
#endif
