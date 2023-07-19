// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  USB Wacom tablet support - system specific code
 */

#include <linux/spinlock.h>
#include "wacom_wac.h"
#include "wacom.h"

/* defines to get HID report descriptor */
#define HID_DEVICET_HID		(USB_TYPE_CLASS | 0x01)
#define HID_DEVICET_REPORT	(USB_TYPE_CLASS | 0x02)
#define HID_USAGE_UNDEFINED		0x00
#define HID_USAGE_PAGE			0x04
#define HID_USAGE_PAGE_DIGITIZER	0x0d
#define HID_USAGE_PAGE_DESKTOP		0x01
#define HID_USAGE_PAGE_WACOMTOUCH	0xff00
#define HID_USAGE			0x08
#define HID_USAGE_X			((HID_USAGE_PAGE_DESKTOP << 16) | 0x30)
#define HID_USAGE_Y			((HID_USAGE_PAGE_DESKTOP << 16) | 0x31)
#define HID_USAGE_PRESSURE		((HID_USAGE_PAGE_DIGITIZER << 16) | 0x30)
#define HID_USAGE_X_TILT		((HID_USAGE_PAGE_DIGITIZER << 16) | 0x3d)
#define HID_USAGE_Y_TILT		((HID_USAGE_PAGE_DIGITIZER << 16) | 0x3e)
#define HID_USAGE_FINGER		((HID_USAGE_PAGE_DIGITIZER << 16) | 0x22)
#define HID_USAGE_STYLUS		((HID_USAGE_PAGE_DIGITIZER << 16) | 0x20)
#define HID_USAGE_WT_X			((HID_USAGE_PAGE_WACOMTOUCH << 16) | 0x130)
#define HID_USAGE_WT_Y			((HID_USAGE_PAGE_WACOMTOUCH << 16) | 0x131)
#define HID_USAGE_WT_FINGER		((HID_USAGE_PAGE_WACOMTOUCH << 16) | 0x22)
#define HID_USAGE_WT_STYLUS		((HID_USAGE_PAGE_WACOMTOUCH << 16) | 0x20)
#define HID_USAGE_CONTACTMAX		((HID_USAGE_PAGE_DIGITIZER << 16) | 0x55)
#define HID_COLLECTION			0xa0
#define HID_COLLECTION_LOGICAL		0x02
#define HID_COLLECTION_END		0xc0
#define HID_LONGITEM			0xfc

struct hid_descriptor {
	struct usb_descriptor_header header;
	__le16   bcdHID;
	u8       bCountryCode;
	u8       bNumDescriptors;
	u8       bDescriptorType;
	__le16   wDescriptorLength;
} __attribute__ ((packed));

/* defines to get/set USB message */
#define USB_REQ_GET_REPORT	0x01
#define USB_REQ_SET_REPORT	0x09

#define WAC_HID_FEATURE_REPORT	0x03
#define WAC_MSG_RETRIES		5
#define WAC_HID_OUTPUT_REPORT	1

#define WAC_CMD_WL_LED_CONTROL	0x03
#define WAC_CMD_LED_CONTROL	0x20
#define WAC_CMD_ICON_START	0x21
#define WAC_CMD_ICON_XFER	0x23
#define WAC_CMD_RETRIES		10
#define WAC_CMD_DELETE_PAIRING	0x20
#define WAC_CMD_UNPAIR_ALL	0xFF
#define WAC_REMOTE_SERIAL_MAX_STRLEN	9

#define DEV_ATTR_RW_PERM (S_IRUGO | S_IWUSR | S_IWGRP)
#define DEV_ATTR_WO_PERM (S_IWUSR | S_IWGRP)
#define DEV_ATTR_RO_PERM (S_IRUSR | S_IRGRP)

static int wacom_get_report(struct usb_interface *intf, u8 type, u8 id,
			    void *buf, size_t size, unsigned int retries)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	int retval;

	do {
		retval = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
				USB_REQ_GET_REPORT,
				USB_DIR_IN | USB_TYPE_CLASS |
				USB_RECIP_INTERFACE,
				(type << 8) + id,
				intf->altsetting[0].desc.bInterfaceNumber,
				buf, size, 100);
	} while ((retval == -ETIMEDOUT || retval == -EAGAIN) && --retries);

	if (retval < 0)
		dev_err(&intf->dev, "%s - ran out of retries (last error = %d)\n",
			__func__, retval);
	return retval;
}

static int wacom_set_report(struct usb_interface *intf, u8 type, u8 id,
			    void *buf, size_t size, unsigned int retries)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	int retval;

	do {
		retval = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
				USB_REQ_SET_REPORT,
				USB_TYPE_CLASS | USB_RECIP_INTERFACE,
				(type << 8) + id,
				intf->altsetting[0].desc.bInterfaceNumber,
				buf, size, 1000);
	} while ((retval == -ETIMEDOUT || retval == -EAGAIN) && --retries);

	if (retval < 0)
		dev_err(&intf->dev, "%s - ran out of retries (last error = %d)\n",
			__func__, retval);
	return retval;
}

static void wacom_sys_irq(struct urb *urb)
{
	struct wacom *wacom = urb->context;
	struct device *dev = &wacom->intf->dev;
	int retval;

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dev_dbg(dev, "%s - urb shutting down with status: %d\n",
			__func__, urb->status);
		return;
	default:
		dev_dbg(dev, "%s - nonzero urb status received: %d\n",
			__func__, urb->status);
		goto exit;
	}

	wacom_wac_irq(&wacom->wacom_wac, urb->actual_length);

 exit:
	usb_mark_last_busy(wacom->usbdev);
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(dev, "%s - usb_submit_urb failed with result %d\n",
			__func__, retval);
}

static int wacom_open(struct input_dev *dev)
{
	struct wacom *wacom = input_get_drvdata(dev);
	int retval = 0;

	if (usb_autopm_get_interface(wacom->intf) < 0)
		return -EIO;

	mutex_lock(&wacom->lock);

	if (usb_submit_urb(wacom->irq, GFP_KERNEL)) {
		retval = -EIO;
		goto out;
	}

	wacom->open = true;
	wacom->intf->needs_remote_wakeup = 1;

out:
	mutex_unlock(&wacom->lock);
	usb_autopm_put_interface(wacom->intf);
	return retval;
}

static void wacom_close(struct input_dev *dev)
{
	struct wacom *wacom = input_get_drvdata(dev);
	int autopm_error;

	autopm_error = usb_autopm_get_interface(wacom->intf);

	mutex_lock(&wacom->lock);
	usb_kill_urb(wacom->irq);
	wacom->open = false;
	wacom->intf->needs_remote_wakeup = 0;
	mutex_unlock(&wacom->lock);

	if (!autopm_error)
		usb_autopm_put_interface(wacom->intf);
}

/*
 * Calculate the resolution of the X or Y axis, given appropriate HID data.
 * This function is little more than hidinput_calc_abs_res stripped down.
 */
static int wacom_calc_hid_res(int logical_extents, int physical_extents,
                              unsigned char unit, unsigned char exponent)
{
	int prev, unit_exponent;

	/* Check if the extents are sane */
	if (logical_extents <= 0 || physical_extents <= 0)
		return 0;

	/* Get signed value of nybble-sized twos-compliment exponent */
	unit_exponent = exponent;
	if (unit_exponent > 7)
		unit_exponent -= 16;

	/* Convert physical_extents to millimeters */
	if (unit == 0x11) {		/* If centimeters */
		unit_exponent += 1;
	} else if (unit == 0x13) {	/* If inches */
		prev = physical_extents;
		physical_extents *= 254;
		if (physical_extents < prev)
			return 0;
		unit_exponent -= 1;
	} else {
		return 0;
	}

	/* Apply negative unit exponent */
	for (; unit_exponent < 0; unit_exponent++) {
		prev = logical_extents;
		logical_extents *= 10;
		if (logical_extents < prev)
			return 0;
	}
	/* Apply positive unit exponent */
	for (; unit_exponent > 0; unit_exponent--) {
		prev = physical_extents;
		physical_extents *= 10;
		if (physical_extents < prev)
			return 0;
	}

	/* Calculate resolution */
	return logical_extents / physical_extents;
}

static void wacom_retrieve_report_data(struct usb_interface *intf,
				       struct wacom_features *features)
{
	int result = 0;
	unsigned char *rep_data;

	rep_data = kmalloc(2, GFP_KERNEL);
	if (rep_data) {

		rep_data[0] = 12;
		result = wacom_get_report(intf, WAC_HID_FEATURE_REPORT,
					  rep_data[0], rep_data, 2,
					  WAC_MSG_RETRIES);

		if (result >= 0 && rep_data[1] > 2)
			features->touch_max = rep_data[1];

		kfree(rep_data);
	}
}

/*
 * Interface Descriptor of wacom devices can be incomplete and
 * inconsistent so wacom_features table is used to store stylus
 * device's packet lengths, various maximum values, and tablet
 * resolution based on product ID's.
 *
 * For devices that contain 2 interfaces, wacom_features table is
 * inaccurate for the touch interface.  Since the Interface Descriptor
 * for touch interfaces has pretty complete data, this function exists
 * to query tablet for this missing information instead of hard coding in
 * an additional table.
 *
 * A typical Interface Descriptor for a stylus will contain a
 * boot mouse application collection that is not of interest and this
 * function will ignore it.
 *
 * It also contains a digitizer application collection that also is not
 * of interest since any information it contains would be duplicate
 * of what is in wacom_features. Usually it defines a report of an array
 * of bytes that could be used as max length of the stylus packet returned.
 * If it happens to define a Digitizer-Stylus Physical Collection then
 * the X and Y logical values contain valid data but it is ignored.
 *
 * A typical Interface Descriptor for a touch interface will contain a
 * Digitizer-Finger Physical Collection which will define both logical
 * X/Y maximum as well as the physical size of tablet. Since touch
 * interfaces haven't supported pressure or distance, this is enough
 * information to override invalid values in the wacom_features table.
 *
 * 3rd gen Bamboo Touch no longer define a Digitizer-Finger Pysical
 * Collection. Instead they define a Logical Collection with a single
 * Logical Maximum for both X and Y.
 *
 * Intuos5 touch interface does not contain useful data. We deal with
 * this after returning from this function.
 */
static int wacom_parse_hid(struct usb_interface *intf,
			   struct hid_descriptor *hid_desc,
			   struct wacom_features *features)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	char limit = 0;
	/* result has to be defined as int for some devices */
	int result = 0, touch_max = 0;
	int i = 0, page = 0, finger = 0, pen = 0;
	unsigned char *report;

	report = kzalloc(hid_desc->wDescriptorLength, GFP_KERNEL);
	if (!report)
		return -ENOMEM;

	/* retrive report descriptors */
	do {
		result = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
			USB_REQ_GET_DESCRIPTOR,
			USB_RECIP_INTERFACE | USB_DIR_IN,
			HID_DEVICET_REPORT << 8,
			intf->altsetting[0].desc.bInterfaceNumber, /* interface */
			report,
			hid_desc->wDescriptorLength,
			5000); /* 5 secs */
	} while (result < 0 && limit++ < WAC_MSG_RETRIES);

	/* No need to parse the Descriptor. It isn't an error though */
	if (result < 0)
		goto out;

	for (i = 0; i < hid_desc->wDescriptorLength; i++) {
		int item = report[i] & 0xFC;
		int len = report[i] & 0x03;
		int data = 0;

		switch (len) {
		case 3:
			len = 4;
			data |= (report[i+4] << 24);
			data |= (report[i+3] << 16);
			fallthrough;
		case 2:
			data |= (report[i+2] << 8);
			fallthrough;
		case 1:
			data |= (report[i+1]);
			break;
		}

		switch (item) {
		case HID_USAGE_PAGE:
			page = data;
			break;

		case HID_USAGE:
			if (len < 4) {
				data |= (page << 16);
			}

			switch (data) {
			case HID_USAGE_WT_X:
				if (finger)
					features->device_type = BTN_TOOL_FINGER;
				if (features->type == INTUOSP2 ||
				    features->type == INTUOSP2S) {
					features->touch_max = 10;
					features->pktlen = WACOM_PKGLEN_INTUOSP2T;
					features->unit = report[i+4];
					features->unitExpo = report[i+6];
					features->x_phy = get_unaligned_le16(&report[i + 10]);
					features->x_max = get_unaligned_le16(&report[i + 15]);
				}
				break;

			case HID_USAGE_WT_Y:
				if (features->type == INTUOSP2 ||
				    features->type == INTUOSP2S) {
					features->y_phy = get_unaligned_le16(&report[i + 4]);
					features->y_max = get_unaligned_le16(&report[i + 7]);
				}
				break;

			case HID_USAGE_X:
				if (finger) {
					features->device_type = BTN_TOOL_FINGER;

					/*
					 * Bamboo Pen only descriptor contains touch
					 * but it should not...
					 */
					if (features->type != BAMBOO_PT)
						/*
						 * ISDv4 touch devices at least 
						 * supports one touch point
						 */
						touch_max = 1;
					switch (features->type) {
					case TABLETPC2FG:
						features->pktlen = WACOM_PKGLEN_TPC2FG;
						break;

					case MTSCREEN:
					case WACOM_24HDT:
						features->pktlen = WACOM_PKGLEN_MTOUCH;
						break;

					case DTH1152T:
					case WACOM_27QHDT:
						features->pktlen = WACOM_PKGLEN_27QHDT;
						break;

					case MTTPC:
					case MTTPC_B:
					case MTTPC_C:
						features->pktlen = WACOM_PKGLEN_MTTPC;
						break;

					case BAMBOO_PT:
						features->pktlen = WACOM_PKGLEN_BBTOUCH;
						break;

					case WACOM_MSPROT:
					case DTH2452T:
						features->pktlen = WACOM_PKGLEN_MSPROT;
						break;

					default:
						features->pktlen = WACOM_PKGLEN_GRAPHIRE;
						break;
					}

					switch (features->type) {
					case BAMBOO_PT:
						features->x_phy =
							get_unaligned_le16(&report[i + 5]);
						features->x_max =
							get_unaligned_le16(&report[i + 8]);
						break;

					case DTH1152T:
					case WACOM_24HDT:
						features->x_max =
							get_unaligned_le16(&report[i + 3]);
						features->x_phy =
							get_unaligned_le16(&report[i + 8]);
						features->unit = report[i - 1];
						features->unitExpo = report[i - 3];
						break;

					case WACOM_27QHDT:
						if (!features->x_max) {
							features->x_max =
								get_unaligned_le16(&report[i - 4]);
							features->x_phy =
								get_unaligned_le16(&report[i - 7]);
							features->unit = report[i - 13];
							features->unitExpo = report[i - 11];
						}
						break;

					case WACOM_MSPROT:
					case MTTPC_B:
					case DTH2452T:
						features->x_max =
							get_unaligned_le16(&report[i + 3]);
						features->x_phy =
							get_unaligned_le16(&report[i + 6]);
						features->unit = report[i - 5];
						features->unitExpo = report[i - 3];
						break;

					case MTTPC_C:
						features->x_max =
							get_unaligned_le16(&report[i + 3]);
						features->x_phy =
							get_unaligned_le16(&report[i + 8]);
						features->unit = report[i - 1];
						features->unitExpo = report[i - 3];
						break;

					default:
						features->x_max =
							get_unaligned_le16(&report[i + 3]);
						features->x_phy =
							get_unaligned_le16(&report[i + 6]);
						features->unit = report[i + 9];
						features->unitExpo = report[i + 11];
						break;
					}
				} else if (pen) {
					/* penabled only accepts exact bytes of data */
					if (features->type >= TABLETPC)
						features->pktlen = WACOM_PKGLEN_GRAPHIRE;
					features->device_type = BTN_TOOL_PEN;
					features->x_max =
						get_unaligned_le16(&report[i + 3]);
				}
				break;

			case HID_USAGE_Y:
				if (finger) {
					switch (features->type) {
					case TABLETPC2FG:
					case MTSCREEN:
					case MTTPC:
						features->y_max =
							get_unaligned_le16(&report[i + 3]);
						features->y_phy =
							get_unaligned_le16(&report[i + 6]);
						break;

					case DTH1152T:
					case WACOM_24HDT:
					case MTTPC_C:
						features->y_max =
							get_unaligned_le16(&report[i + 3]);
						features->y_phy =
							get_unaligned_le16(&report[i - 2]);
						break;

					case WACOM_27QHDT:
						if (!features->y_max) {
							features->y_max =
								get_unaligned_le16(&report[i - 2]);
							features->y_phy =
								get_unaligned_le16(&report[i - 5]);
						}
						break;

					case BAMBOO_PT:
						features->y_phy =
							get_unaligned_le16(&report[i + 3]);
						features->y_max =
							get_unaligned_le16(&report[i + 6]);
						break;

					case WACOM_MSPROT:
					case MTTPC_B:
					case DTH2452T:
						features->y_max =
							get_unaligned_le16(&report[i + 3]);
						features->y_phy =
							get_unaligned_le16(&report[i + 6]);
						break;

					default:
						features->y_max =
							features->x_max;
						features->y_phy =
							get_unaligned_le16(&report[i + 3]);
						break;
					}
				} else if (pen) {
					features->y_max =
						get_unaligned_le16(&report[i + 3]);
				}
				break;

			case HID_USAGE_WT_FINGER:
			case HID_USAGE_FINGER:
				finger = 1;
				break;

			/*
			 * Requiring Stylus Usage will ignore boot mouse
			 * X/Y values and some cases of invalid Digitizer X/Y
			 * values commonly reported.
			 */
			case HID_USAGE_WT_STYLUS:
			case HID_USAGE_STYLUS:
				pen = 1;
				break;

			case HID_USAGE_CONTACTMAX:
				/* leave touch_max as is if predefined */
				if (!features->touch_max)
					wacom_retrieve_report_data(intf, features);
				break;

			case HID_USAGE_PRESSURE:
				if (pen) {
					features->pressure_max =
						get_unaligned_le16(&report[i + 3]);
				}
				break;
			}
			break;

		case HID_COLLECTION_END:
			/* reset UsagePage and Finger */
			finger = page = 0;
			break;

		case HID_COLLECTION:
			switch (report[i]) {
			case HID_COLLECTION_LOGICAL:
				if (features->type == BAMBOO_PT) {
					features->pktlen = WACOM_PKGLEN_BBTOUCH3;
					features->device_type = BTN_TOOL_FINGER;

					features->x_max = features->y_max =
						get_unaligned_le16(&report[10]);
				}
				break;
			}
			break;

		case HID_LONGITEM:
			/*
			 * HID "Long Items" can contain up to 255 bytes
			 * of data. We don't use long items, so just
			 * update the length to skip over it entirely.
			 */
			len += data & 0x00FF;
			break;
		}

		i += len;
	}

 out:
	if (!features->touch_max && touch_max)
		features->touch_max = touch_max;
	result = 0;
	kfree(report);
	return result;
}

static int wacom_set_device_mode(struct usb_interface *intf, int report_id, int length, int mode)
{
	unsigned char *rep_data;
	int error = -ENOMEM, limit = 0;

	rep_data = kzalloc(length, GFP_KERNEL);
	if (!rep_data)
		return error;

	do {
		rep_data[0] = report_id;
		rep_data[1] = mode;

		error = wacom_set_report(intf, WAC_HID_FEATURE_REPORT,
		                         report_id, rep_data, length, 1);
	} while ((error < 0 || rep_data[1] != mode) && limit++ < WAC_MSG_RETRIES);

	kfree(rep_data);

	return error < 0 ? error : 0;
}

/*
 * Switch the tablet into its most-capable mode. Wacom tablets are
 * typically configured to power-up in a mode which sends mouse-like
 * reports to the OS. To get absolute position, pressure data, etc.
 * from the tablet, it is necessary to switch the tablet out of this
 * mode and into one which sends the full range of tablet data.
 */
static int wacom_query_tablet_data(struct usb_interface *intf, struct wacom_features *features)
{
	if (features->device_type == BTN_TOOL_FINGER) {
		if (features->type > TABLETPC) {
			/* MT Tablet PC touch */
			return wacom_set_device_mode(intf, 3, 4, 4);
		}
		else if (features->type == WACOM_24HDT) {
			return wacom_set_device_mode(intf, 18, 3, 2);
		}
		else if (features->type == WACOM_27QHDT) {
			return wacom_set_device_mode(intf, 131, 3, 2);
		}
		else if (features->type == WACOM_MSPROT ||
			 features->type == DTH1152T) {
			return wacom_set_device_mode(intf, 14, 2, 2);
		}
	} else if (features->device_type == BTN_TOOL_PEN) {
		if (features->type <= BAMBOO_PT) {
			return wacom_set_device_mode(intf, 2, 2, 2);
		}
	}

	return 0;
}

static int wacom_retrieve_hid_descriptor(struct usb_interface *intf,
					 struct wacom_features *features)
{
	int error = 0;
	struct usb_host_interface *interface = intf->cur_altsetting;
	struct hid_descriptor *hid_desc;

	/* default features */
	features->device_type = BTN_TOOL_PEN;
	features->x_fuzz = 4;
	features->y_fuzz = 4;
	features->pressure_fuzz = 0;
	features->distance_fuzz = 1;
	features->tilt_fuzz = 1;

	/*
	 * The wireless device HID is basic and layout conflicts with
	 * other tablets (monitor and touch interface can look like pen).
	 * Skip the query for this type and modify defaults based on
	 * interface number.
	 */
	if (features->type == WIRELESS) {
		if (intf->cur_altsetting->desc.bInterfaceNumber == 0) {
			features->device_type = 0;
		} else if (intf->cur_altsetting->desc.bInterfaceNumber == 2) {
			features->device_type = BTN_TOOL_FINGER;
			features->pktlen = WACOM_PKGLEN_BBTOUCH3;
		}
	}

	/* only devices that support touch need to retrieve the info */
	if (features->type < BAMBOO_PT) {
		goto out;
	}

	error = usb_get_extra_descriptor(interface, HID_DEVICET_HID, &hid_desc);
	if (error) {
		error = usb_get_extra_descriptor(&interface->endpoint[0],
						 HID_DEVICET_REPORT, &hid_desc);
		if (error) {
			dev_err(&intf->dev,
				"can not retrieve extra class descriptor\n");
			goto out;
		}
	}
	error = wacom_parse_hid(intf, hid_desc, features);

 out:
	return error;
}

struct wacom_usbdev_data {
	struct list_head list;
	struct kref kref;
	struct usb_device *dev;
	struct wacom_shared shared;
};

static LIST_HEAD(wacom_udev_list);
static DEFINE_MUTEX(wacom_udev_list_lock);

static struct usb_device *wacom_get_sibling(struct usb_device *dev, int vendor, int product)
{
	int port1;
	struct usb_device *sibling;

	if (vendor == 0 && product == 0)
		return dev;

	if (dev->parent == NULL)
		return NULL;

	usb_hub_for_each_child(dev->parent, port1, sibling) {
		struct usb_device_descriptor *d;
		if (sibling == NULL)
			continue;

		d = &sibling->descriptor;
		if (d->idVendor == vendor && d->idProduct == product)
			return sibling;
	}

	return NULL;
}

static struct wacom_usbdev_data *wacom_get_usbdev_data(struct usb_device *dev)
{
	struct wacom_usbdev_data *data;

	list_for_each_entry(data, &wacom_udev_list, list) {
		if (data->dev == dev) {
			kref_get(&data->kref);
			return data;
		}
	}

	return NULL;
}

static void wacom_release_shared_data(struct kref *kref)
{
	struct wacom_usbdev_data *data =
		container_of(kref, struct wacom_usbdev_data, kref);

	mutex_lock(&wacom_udev_list_lock);
	list_del(&data->list);
	mutex_unlock(&wacom_udev_list_lock);

	kfree(data);
}

static void wacom_remove_shared_data(struct wacom_wac *wacom)
{
	struct wacom_usbdev_data *data;

	if (wacom->shared) {
		data = container_of(wacom->shared, struct wacom_usbdev_data, shared);
		kref_put(&data->kref, wacom_release_shared_data);
		wacom->shared = NULL;
	}
}

static int wacom_add_shared_data(struct wacom_wac *wacom,
				 struct usb_device *dev)
{
	struct wacom_usbdev_data *data;
	int retval = 0;

	mutex_lock(&wacom_udev_list_lock);

	data = wacom_get_usbdev_data(dev);
	if (!data) {
		data = kzalloc(sizeof(struct wacom_usbdev_data), GFP_KERNEL);
		if (!data) {
			retval = -ENOMEM;
			goto out;
		}

		kref_init(&data->kref);
		data->dev = dev;
		list_add_tail(&data->list, &wacom_udev_list);
	}

	wacom->shared = &data->shared;

out:
	mutex_unlock(&wacom_udev_list_lock);
	return retval;
}

static int wacom_led_control(struct wacom *wacom)
{
	unsigned char *buf;
	int retval;
	unsigned char report_id = WAC_CMD_LED_CONTROL;
	int buf_size = 9;

	if (wacom->wacom_wac.pid) { /* wireless connected */
		report_id = WAC_CMD_WL_LED_CONTROL;
		buf_size = 13;
	}
	buf = kzalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (wacom->wacom_wac.features.type == INTUOSP2) {

		buf[0] = WAC_CMD_LED_CONTROL_GENERIC;
		buf[1] = wacom->led.llv;
		buf[2] = wacom->led.select[0] & 0x03;

	} else if (wacom->wacom_wac.features.type >= INTUOS5S &&
	    wacom->wacom_wac.features.type <= INTUOSPL) {
		/*
		 * Touch Ring and crop mark LED luminance may take on
		 * one of four values:
		 *    0 = Low; 1 = Medium; 2 = High; 3 = Off
		 */
		int ring_led = wacom->led.select[0] & 0x03;
		int ring_lum = (((wacom->led.llv & 0x60) >> 5) - 1) & 0x03;
		int crop_lum = 0;

		unsigned char led_bits = (crop_lum << 4) | (ring_lum << 2) | (ring_led);

		buf[0] = report_id;
		if (wacom->wacom_wac.pid) {
			wacom_get_report(wacom->intf, WAC_HID_FEATURE_REPORT,
					 buf[0], buf, buf_size, WAC_CMD_RETRIES);
			buf[0] = report_id;
			buf[4] = led_bits;
		} else
			buf[1] = led_bits;
	}
	else {
		int led = wacom->led.select[0] | 0x4;

		if (wacom->wacom_wac.features.type == WACOM_21UX2 ||
		    wacom->wacom_wac.features.type == WACOM_24HD)
			led |= (wacom->led.select[1] << 4) | 0x40;

		buf[0] = report_id;
		buf[1] = led;
		buf[2] = wacom->led.llv;
		buf[3] = wacom->led.hlv;
		buf[4] = wacom->led.img_lum;
	}

	retval = wacom_set_report(wacom->intf, 0x03, report_id,
				  buf, buf_size, WAC_CMD_RETRIES);
	kfree(buf);

	return retval;
}

static int wacom_led_putimage(struct wacom *wacom, int button_id, const void *img)
{
	unsigned char *buf;
	int i, retval;

	buf = kzalloc(259, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Send 'start' command */
	buf[0] = WAC_CMD_ICON_START;
	buf[1] = 1;
	retval = wacom_set_report(wacom->intf, 0x03, WAC_CMD_ICON_START,
				  buf, 2, WAC_CMD_RETRIES);
	if (retval < 0)
		goto out;

	buf[0] = WAC_CMD_ICON_XFER;
	buf[1] = button_id & 0x07;
	for (i = 0; i < 4; i++) {
		buf[2] = i;
		memcpy(buf + 3, img + i * 256, 256);

		retval = wacom_set_report(wacom->intf, 0x03, WAC_CMD_ICON_XFER,
					  buf, 259, WAC_CMD_RETRIES);
		if (retval < 0)
			break;
	}

	/* Send 'stop' */
	buf[0] = WAC_CMD_ICON_START;
	buf[1] = 0;
	wacom_set_report(wacom->intf, 0x03, WAC_CMD_ICON_START,
			 buf, 2, WAC_CMD_RETRIES);

out:
	kfree(buf);
	return retval;
}

static ssize_t wacom_led_select_store(struct device *dev, int set_id,
				      const char *buf, size_t count)
{
	struct wacom *wacom = dev_get_drvdata(dev);
	unsigned int id;
	int err;

	err = kstrtouint(buf, 10, &id);
	if (err)
		return err;

	mutex_lock(&wacom->lock);

	wacom->led.select[set_id] = id & 0x3;
	err = wacom_led_control(wacom);

	mutex_unlock(&wacom->lock);

	return err < 0 ? err : count;
}

#define DEVICE_LED_SELECT_ATTR(SET_ID)					\
static ssize_t wacom_led##SET_ID##_select_store(struct device *dev,	\
	struct device_attribute *attr, const char *buf, size_t count)	\
{									\
	return wacom_led_select_store(dev, SET_ID, buf, count);		\
}									\
static ssize_t wacom_led##SET_ID##_select_show(struct device *dev,	\
	struct device_attribute *attr, char *buf)			\
{									\
	struct wacom *wacom = dev_get_drvdata(dev);			\
	return snprintf(buf, PAGE_SIZE, "%d\n",				\
			wacom->led.select[SET_ID]);			\
}									\
static DEVICE_ATTR(status_led##SET_ID##_select, DEV_ATTR_RW_PERM,	\
		    wacom_led##SET_ID##_select_show,			\
		    wacom_led##SET_ID##_select_store)

DEVICE_LED_SELECT_ATTR(0);
DEVICE_LED_SELECT_ATTR(1);

static ssize_t wacom_luminance_store(struct wacom *wacom, u8 *dest,
				     const char *buf, size_t count)
{
	unsigned int value;
	int err;

	err = kstrtouint(buf, 10, &value);
	if (err)
		return err;

	mutex_lock(&wacom->lock);

	*dest = value & 0x7f;
	err = wacom_led_control(wacom);

	mutex_unlock(&wacom->lock);

	return err < 0 ? err : count;
}

#define DEVICE_LUMINANCE_ATTR(name, field)				\
static ssize_t wacom_##name##_luminance_store(struct device *dev,	\
	struct device_attribute *attr, const char *buf, size_t count)	\
{									\
	struct wacom *wacom = dev_get_drvdata(dev);			\
									\
	return wacom_luminance_store(wacom, &wacom->led.field,		\
				     buf, count);			\
}									\
static ssize_t wacom_##name##_luminance_show(struct device *dev,	\
	struct device_attribute *attr, char *buf)			\
{									\
	struct wacom *wacom = dev_get_drvdata(dev);			\
	return scnprintf(buf, PAGE_SIZE, "%d\n", wacom->led.field);	\
}									\
static DEVICE_ATTR(name##_luminance, DEV_ATTR_RW_PERM,			\
		   wacom_##name##_luminance_show,			\
		   wacom_##name##_luminance_store)

DEVICE_LUMINANCE_ATTR(status0, llv);
DEVICE_LUMINANCE_ATTR(status1, hlv);
DEVICE_LUMINANCE_ATTR(buttons, img_lum);

static ssize_t wacom_button_image_store(struct device *dev, int button_id,
					const char *buf, size_t count)
{
	struct wacom *wacom = dev_get_drvdata(dev);
	int err;

	if (count != 1024)
		return -EINVAL;

	mutex_lock(&wacom->lock);

	err = wacom_led_putimage(wacom, button_id, buf);

	mutex_unlock(&wacom->lock);

	return err < 0 ? err : count;
}

#define DEVICE_BTNIMG_ATTR(BUTTON_ID)					\
static ssize_t wacom_btnimg##BUTTON_ID##_store(struct device *dev,	\
	struct device_attribute *attr, const char *buf, size_t count)	\
{									\
	return wacom_button_image_store(dev, BUTTON_ID, buf, count);	\
}									\
static DEVICE_ATTR(button##BUTTON_ID##_rawimg, DEV_ATTR_WO_PERM,	\
		   NULL, wacom_btnimg##BUTTON_ID##_store)

DEVICE_BTNIMG_ATTR(0);
DEVICE_BTNIMG_ATTR(1);
DEVICE_BTNIMG_ATTR(2);
DEVICE_BTNIMG_ATTR(3);
DEVICE_BTNIMG_ATTR(4);
DEVICE_BTNIMG_ATTR(5);
DEVICE_BTNIMG_ATTR(6);
DEVICE_BTNIMG_ATTR(7);

static struct attribute *cintiq_led_attrs[] = {
	&dev_attr_status_led0_select.attr,
	&dev_attr_status_led1_select.attr,
	NULL
};

static const struct attribute_group cintiq_led_attr_group = {
	.name = "wacom_led",
	.attrs = cintiq_led_attrs,
};

static struct attribute *intuos4_led_attrs[] = {
	&dev_attr_status0_luminance.attr,
	&dev_attr_status1_luminance.attr,
	&dev_attr_status_led0_select.attr,
	&dev_attr_buttons_luminance.attr,
	&dev_attr_button0_rawimg.attr,
	&dev_attr_button1_rawimg.attr,
	&dev_attr_button2_rawimg.attr,
	&dev_attr_button3_rawimg.attr,
	&dev_attr_button4_rawimg.attr,
	&dev_attr_button5_rawimg.attr,
	&dev_attr_button6_rawimg.attr,
	&dev_attr_button7_rawimg.attr,
	NULL
};

static const struct attribute_group intuos4_led_attr_group = {
	.name = "wacom_led",
	.attrs = intuos4_led_attrs,
};

static struct attribute *intuos5_led_attrs[] = {
	&dev_attr_status0_luminance.attr,
	&dev_attr_status_led0_select.attr,
	NULL
};

static const struct attribute_group intuos5_led_attr_group = {
	.name = "wacom_led",
	.attrs = intuos5_led_attrs,
};

struct wacom_sysfs_group_devres {
	const struct attribute_group *group;
	struct kobject *root;
};

static void wacom_devm_sysfs_group_release(struct device *dev, void *res)
{
	struct wacom_sysfs_group_devres *devres = res;
	struct kobject *kobj = devres->root;

	dev_dbg(dev, "%s: dropping reference to %s\n",
		__func__, devres->group->name);
	sysfs_remove_group(kobj, devres->group);
}

static int __wacom_devm_sysfs_create_group(struct wacom *wacom,
					   struct kobject *root,
					   const struct attribute_group *group)
{
	struct wacom_sysfs_group_devres *devres;
	int error;

	devres = devres_alloc(wacom_devm_sysfs_group_release,
			      sizeof(struct wacom_sysfs_group_devres),
			      GFP_KERNEL);
	if (!devres)
		return -ENOMEM;

	devres->group = group;
	devres->root = root;

	error = sysfs_create_group(devres->root, group);
	if (error) {
		devres_free(devres);
		return error;
	}

	devres_add(&wacom->intf->dev, devres);

	return 0;
}

static int wacom_devm_sysfs_create_group(struct wacom *wacom,
					 const struct attribute_group *group)
{
	return __wacom_devm_sysfs_create_group(wacom, &wacom->intf->dev.kobj,
					       group);
}

static int wacom_initialize_leds(struct wacom *wacom)
{
	int error;

	if (wacom->wacom_wac.features.device_type != BTN_TOOL_PEN)
		return 0;

	/* Initialize default values */
	switch (wacom->wacom_wac.features.type) {
	case INTUOS4S:
	case INTUOS4:
	case INTUOS4L:
		wacom->led.select[0] = 0;
		wacom->led.select[1] = 0;
		wacom->led.llv = 10;
		wacom->led.hlv = 20;
		wacom->led.img_lum = 10;

		error = wacom_devm_sysfs_create_group(wacom,
						      &intuos4_led_attr_group);
		break;

	case WACOM_24HD:
	case WACOM_21UX2:
		wacom->led.select[0] = 0;
		wacom->led.select[1] = 0;
		wacom->led.llv = 0;
		wacom->led.hlv = 0;
		wacom->led.img_lum = 0;

		error = wacom_devm_sysfs_create_group(wacom,
						      &cintiq_led_attr_group);
		break;

	case INTUOS5S:
	case INTUOS5:
	case INTUOS5L:
	case INTUOSPS:
	case INTUOSPM:
	case INTUOSPL:
	case INTUOSP2:
		wacom->led.select[0] = 0;
		wacom->led.select[1] = 0;
		wacom->led.llv = 32;
		wacom->led.hlv = 0;
		wacom->led.img_lum = 0;

		error = wacom_devm_sysfs_create_group(wacom,
						      &intuos5_led_attr_group);
		break;

	default:
		return 0;
	}

	if (error) {
		dev_err(&wacom->intf->dev,
			"cannot create sysfs group err: %d\n", error);
		return error;
	}
	wacom_led_control(wacom);

	return 0;
}

static enum power_supply_property wacom_battery_props[] = {
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_CAPACITY
};

static int wacom_battery_get_property(struct power_supply *psy,
				      enum power_supply_property psp,
				      union power_supply_propval *val)
{
#ifdef WACOM_POWERSUPPLY_41
	struct wacom_battery *battery = power_supply_get_drvdata(psy);
#else
	struct wacom_battery *battery = container_of(psy, struct wacom_battery, battery);
#endif
	int ret = 0;

	switch (psp) {
		case POWER_SUPPLY_PROP_MODEL_NAME:
			val->strval = battery->wacom->wacom_wac.name;
			break;
		case POWER_SUPPLY_PROP_PRESENT:
			val->intval = battery->bat_connected;
			break;
		case POWER_SUPPLY_PROP_SCOPE:
			val->intval = POWER_SUPPLY_SCOPE_DEVICE;
			break;
		case POWER_SUPPLY_PROP_CAPACITY:
			val->intval = battery->battery_capacity;
			break;
		case POWER_SUPPLY_PROP_STATUS:
			if (battery->bat_status != WACOM_POWER_SUPPLY_STATUS_AUTO)
				val->intval = battery->bat_status;
			else if (battery->bat_charging)
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			else if (battery->battery_capacity == 100 &&
				    battery->ps_connected)
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else if (battery->ps_connected)
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		default:
			ret = -EINVAL;
			break;
	}

	return ret;
}

#ifndef WACOM_POWERSUPPLY_41
static int __wacom_initialize_battery(struct wacom *wacom,
				      struct wacom_battery *battery)
{
	static atomic_t battery_no = ATOMIC_INIT(0);
	struct device *dev = &wacom->intf->dev;
	int error;
	unsigned long n;

	n = atomic_inc_return(&battery_no) - 1;

	battery->battery.properties = wacom_battery_props;
	battery->battery.num_properties = ARRAY_SIZE(wacom_battery_props);
	battery->battery.get_property = wacom_battery_get_property;
	sprintf(wacom->battery.bat_name, "wacom_battery_%ld", n);
	battery->battery.name = wacom->battery.bat_name;
	battery->battery.type = POWER_SUPPLY_TYPE_USB;
	battery->battery.use_for_apm = 0;

	error = power_supply_register(dev, &battery->battery);

	if (error)
		return error;

	power_supply_powers(WACOM_POWERSUPPLY_REF(battery->battery), dev);

	return 0;
}

#else
static int __wacom_initialize_battery(struct wacom *wacom,
				      struct wacom_battery *battery)
{
	static atomic_t battery_no = ATOMIC_INIT(0);
	struct device *dev = &wacom->intf->dev;
	struct power_supply_config psy_cfg = { .drv_data = battery, };
	struct power_supply *ps_bat;
	struct power_supply_desc *bat_desc = &battery->bat_desc;
	unsigned long n;
	int error;

	if (!devres_open_group(dev, bat_desc, GFP_KERNEL))
		return -ENOMEM;

	battery->wacom = wacom;

	n = atomic_inc_return(&battery_no) - 1;

	bat_desc->properties = wacom_battery_props;
	bat_desc->num_properties = ARRAY_SIZE(wacom_battery_props);
	bat_desc->get_property = wacom_battery_get_property;
	sprintf(battery->bat_name, "wacom_battery_0%ld", n);
	bat_desc->name = battery->bat_name;
	bat_desc->type = POWER_SUPPLY_TYPE_USB;
	bat_desc->use_for_apm = 0;

	ps_bat = devm_power_supply_register(dev, bat_desc, &psy_cfg);
	if (IS_ERR(ps_bat)) {
		error = PTR_ERR(ps_bat);
		goto err;
	}

	power_supply_powers(ps_bat, &wacom->intf->dev);

	battery->battery = ps_bat;

	devres_close_group(dev, bat_desc);
	return 0;

err:
	devres_release_group(dev, bat_desc);
	return error;
}
#endif

static int wacom_initialize_battery(struct wacom *wacom)
{
	if (wacom->wacom_wac.features.quirks & WACOM_QUIRK_BATTERY)
		return __wacom_initialize_battery(wacom, &wacom->battery);

	return 0;
}

static void wacom_destroy_battery(struct wacom *wacom)
{
	if (WACOM_POWERSUPPLY_DEVICE(wacom->battery.battery)) {
#ifdef WACOM_POWERSUPPLY_41
		devres_release_group(&wacom->intf->dev,
				     &wacom->battery.bat_desc);
#else
		power_supply_unregister(&wacom->battery.battery);
#endif
		WACOM_POWERSUPPLY_DEVICE(wacom->battery.battery) = NULL;
	}
}

static void wacom_destroy_remote_battery(struct wacom_battery *battery)
{
	if (WACOM_POWERSUPPLY_DEVICE(battery->battery)) {
		power_supply_unregister(WACOM_POWERSUPPLY_REF(battery->battery));
		WACOM_POWERSUPPLY_DEVICE(battery->battery) = NULL;
	}
}

static ssize_t wacom_show_remote_mode(struct kobject *kobj,
				      struct kobj_attribute *kattr,
				      char *buf, int index)
{
	struct device *dev = kobj_to_dev(kobj->parent);
	struct wacom *wacom = dev_get_drvdata(dev);
	u8 mode;

	mode = wacom->led.select[index];
	return sprintf(buf, "%d\n", mode < 3 ? mode : -1);
}

#define DEVICE_EKR_ATTR_GROUP(SET_ID)					\
static ssize_t wacom_show_remote##SET_ID##_mode(struct kobject *kobj,	\
			       struct kobj_attribute *kattr, char *buf)	\
{									\
	return wacom_show_remote_mode(kobj, kattr, buf, SET_ID);	\
}									\
static struct kobj_attribute remote##SET_ID##_mode_attr = {		\
	.attr = {.name = "remote_mode",					\
		.mode = DEV_ATTR_RO_PERM},				\
	.show = wacom_show_remote##SET_ID##_mode,			\
};									\
static struct attribute *remote##SET_ID##_serial_attrs[] = {		\
	&remote##SET_ID##_mode_attr.attr,				\
	NULL								\
};									\
static const struct attribute_group remote##SET_ID##_serial_group = {		\
	.name = NULL,							\
	.attrs = remote##SET_ID##_serial_attrs,				\
}

DEVICE_EKR_ATTR_GROUP(0);
DEVICE_EKR_ATTR_GROUP(1);
DEVICE_EKR_ATTR_GROUP(2);
DEVICE_EKR_ATTR_GROUP(3);
DEVICE_EKR_ATTR_GROUP(4);

static int wacom_remote_create_attr_group(struct wacom *wacom, __u32 serial,
					  int index)
{
	int error = 0;
	struct wacom_remote *remote = wacom->remote;

	remote->remotes[index].group.name = kasprintf(GFP_KERNEL, "%d", serial);
	if (!remote->remotes[index].group.name)
		return -ENOMEM;

	error = __wacom_devm_sysfs_create_group(wacom, remote->remote_dir,
						&remote->remotes[index].group);
	if (error) {
		remote->remotes[index].group.name = NULL;
		dev_err(&wacom->intf->dev,
			"cannot create sysfs group err: %d\n", error);
		return error;
	}

	return 0;
}

static int wacom_cmd_unpair_remote(struct wacom *wacom, unsigned char selector)
{
	const size_t buf_size = 2;
	unsigned char *buf;
	int retval;

	buf = kzalloc(buf_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = WAC_CMD_DELETE_PAIRING;
	buf[1] = selector;

	retval = wacom_set_report(wacom->intf, WAC_HID_OUTPUT_REPORT,
				  WAC_CMD_DELETE_PAIRING, buf,
				  buf_size, WAC_CMD_RETRIES);
	kfree(buf);

	return retval;
}

static void wacom_remote_destroy_one(struct wacom *wacom, unsigned int index)
{
	struct wacom_remote *remote = wacom->remote;
	u32 serial = remote->remotes[index].serial;
	int i;
	unsigned long flags;

	spin_lock_irqsave(&remote->remote_lock, flags);
	remote->remotes[index].registered = false;
	spin_unlock_irqrestore(&remote->remote_lock, flags);

	if (remote->remotes[index].group.name)
		devres_release_group(&wacom->usbdev->dev,
				     &remote->remotes[index]);

	if (remote->remotes[index].input)
		input_unregister_device(remote->remotes[index].input);

	if (WACOM_POWERSUPPLY_DEVICE(remote->remotes[index].battery.battery))
		wacom_destroy_remote_battery(&remote->remotes[index].battery);

	for (i = 0; i < WACOM_MAX_REMOTES; i++) {
		if (remote->remotes[i].serial == serial) {
			remote->remotes[i].serial = 0;

			/* Destroy the attribute group parts not
			 * covered by devres for this kernel.
			 */
			kfree((char *)remote->remotes[i].group.name);
			remote->remotes[i].group.name = NULL;
			remote->remotes[i].registered = false;
			WACOM_POWERSUPPLY_DEVICE(remote->remotes[i].battery.battery) = NULL;
			wacom->led.select[i] = WACOM_STATUS_UNKNOWN;
		}
	}
}

static ssize_t wacom_store_unpair_remote(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	unsigned char selector = 0;
	struct device *dev = kobj_to_dev(kobj->parent);
	struct wacom *wacom = dev_get_drvdata(dev);
	int err;

	if (!strncmp(buf, "*\n", 2)) {
		selector = WAC_CMD_UNPAIR_ALL;
	} else {
		dev_info(&wacom->intf->dev, "remote: unrecognized unpair code: %s\n",
			 buf);
		return -1;
	}

	mutex_lock(&wacom->lock);

	err = wacom_cmd_unpair_remote(wacom, selector);
	mutex_unlock(&wacom->lock);

	return err < 0 ? err : count;
}

static struct kobj_attribute unpair_remote_attr = {
	.attr = {.name = "unpair_remote", .mode = 0200},
	.store = wacom_store_unpair_remote,
};

static const struct attribute *remote_unpair_attrs[] = {
	&unpair_remote_attr.attr,
	NULL
};

static void wacom_remotes_destroy(struct wacom *wacom)
{
	struct wacom_remote *remote = wacom->remote;
	int i;

	if (!remote)
		return;

	for (i = 0; i < WACOM_MAX_REMOTES; i++) {
		if (remote->remotes[i].registered) {
			wacom_remote_destroy_one(wacom, i);
		}
	}
	kobject_put(remote->remote_dir);
	kfifo_free(&remote->remote_fifo);
	wacom->remote = NULL;
}

static int wacom_initialize_remotes(struct wacom *wacom)
{
	int error = 0;
	struct wacom_remote *remote;
	int i;

	if (wacom->wacom_wac.features.type != REMOTE)
		return 0;

	remote = devm_kzalloc(&wacom->usbdev->dev, sizeof(*wacom->remote),
			      GFP_KERNEL);
	if (!remote)
		return -ENOMEM;

	wacom->remote = remote;

	spin_lock_init(&remote->remote_lock);

	error = kfifo_alloc(&remote->remote_fifo,
			5 * sizeof(struct wacom_remote_data),
			GFP_KERNEL);
	if (error) {
		dev_err(&wacom->intf->dev, "failed allocating remote_fifo\n");
		return -ENOMEM;
	}

	remote->remotes[0].group = remote0_serial_group;
	remote->remotes[1].group = remote1_serial_group;
	remote->remotes[2].group = remote2_serial_group;
	remote->remotes[3].group = remote3_serial_group;
	remote->remotes[4].group = remote4_serial_group;

	remote->remote_dir = kobject_create_and_add("wacom_remote",
						    &wacom->intf->dev.kobj);

	if (!remote->remote_dir)
		return -ENOMEM;

	error = sysfs_create_files(remote->remote_dir, remote_unpair_attrs);

	if (error) {
		dev_err(&wacom->intf->dev,
			"cannot create sysfs group err: %d\n", error);
		return error;
	}

	for (i = 0; i < WACOM_MAX_REMOTES; i++) {
		wacom->led.select[i] = WACOM_STATUS_UNKNOWN;
		remote->remotes[i].serial = 0;
	}

	return 0;
}

static void wacom_unregister_inputs(struct wacom *wacom)
{
	if (wacom->wacom_wac.input)
		input_unregister_device(wacom->wacom_wac.input);
	wacom->wacom_wac.input = NULL;
}

static int wacom_register_remote_input(struct wacom *wacom, int index)
{
	struct input_dev *input_dev;
	struct wacom_remote *remote = wacom->remote;
	struct usb_interface *intf = wacom->intf;
	struct usb_device *dev = interface_to_usbdev(intf);
	struct wacom_wac *wacom_wac = &(wacom->wacom_wac);
	int error;

	input_dev = input_allocate_device();
	if (!input_dev) {
		error = -ENOMEM;
		goto fail1;
	}

	input_dev->name = wacom_wac->name;
	input_dev->dev.parent = &intf->dev;
	usb_to_input_id(dev, &input_dev->id);
	if (wacom_wac->pid != 0) {
		/* copy PID of wireless device */
		input_dev->id.product = wacom_wac->pid;
	}
	input_set_drvdata(input_dev, wacom);

	remote->remotes[index].input = input_dev;

	return 0;

fail1:
	return error;
}

static int wacom_register_input(struct wacom *wacom)
{
	struct input_dev *input_dev;
	struct usb_interface *intf = wacom->intf;
	struct usb_device *dev = interface_to_usbdev(intf);
	struct wacom_wac *wacom_wac = &(wacom->wacom_wac);
	struct wacom_features *features = &wacom_wac->features;
	int error;

	if (features->type == REMOTE)
		return 0;

	input_dev = input_allocate_device();
	if (!input_dev) {
		error = -ENOMEM;
		goto fail1;
	}

	input_dev->name = wacom_wac->name;
	input_dev->dev.parent = &intf->dev;
	input_dev->open = wacom_open;
	input_dev->close = wacom_close;
	usb_to_input_id(dev, &input_dev->id);
	if (wacom_wac->pid != 0) {
		/* copy PID of wireless device */
		input_dev->id.product = wacom_wac->pid;
	}
	input_set_drvdata(input_dev, wacom);

	wacom_wac->input = input_dev;

	if (wacom_wac->features.touch_max && wacom_wac->shared) {
		if (wacom_wac->features.device_type == BTN_TOOL_FINGER) {
			wacom_wac->shared->type = wacom_wac->features.type;
			wacom_wac->shared->touch_input = wacom_wac->input;
		}
	}

	error = wacom_setup_input_capabilities(input_dev, wacom_wac);
	if (error)
		goto fail1;

	error = input_register_device(input_dev);
	if (error)
		goto fail2;

	return 0;

fail2:
	if (input_dev)
		input_free_device(input_dev);
	wacom_wac->input = NULL;
fail1:
	return error;
}

static int wacom_remote_create_one(struct wacom *wacom, u32 serial,
				   unsigned int index)
{
	struct wacom_remote *remote = wacom->remote;
	struct wacom_wac *wacom_wac = &wacom->wacom_wac;
	struct device *dev = &wacom->usbdev->dev;
	int error, k;

	/* A remote can pair more than once with an EKR,
	 * check to make sure this serial isn't already paired.
	 */
	for (k = 0; k < WACOM_MAX_REMOTES; k++) {
		if (remote->remotes[k].serial == serial)
			break;
	}

	if (k < WACOM_MAX_REMOTES) {
		remote->remotes[index].serial = serial;
		return 0;
	}

	if (!devres_open_group(dev, &remote->remotes[index], GFP_KERNEL))
		return -ENOMEM;

	error = wacom_remote_create_attr_group(wacom, serial, index);
	if (error)
		goto fail;

	error = wacom_register_remote_input(wacom, index);
	if (error)
		goto fail;

	if (!remote->remotes[index].input) {
		error = -ENOMEM;
		goto fail;
	}

	remote->remotes[index].input->uniq = remote->remotes[index].group.name;
	remote->remotes[index].input->name = wacom_wac->name;

	if (!remote->remotes[index].input->name) {
		error = -EINVAL;
		goto fail;
	}

	error = wacom_setup_input_capabilities(remote->remotes[index].input,
					       wacom_wac);
	if (error)
		goto fail;

	remote->remotes[index].serial = serial;

	error = input_register_device(remote->remotes[index].input);
	if (error)
		goto fail2;

	remote->remotes[index].registered = true;

	devres_close_group(dev, &remote->remotes[index]);

	return 0;
fail2:
	if (remote->remotes[index].input)
		input_free_device(remote->remotes[index].input);
	remote->remotes[index].input = NULL;
fail:
	devres_release_group(dev, &remote->remotes[index]);
	remote->remotes[index].serial = 0;
	return error;
}

static int wacom_remote_attach_battery(struct wacom *wacom, int index)
{
	struct wacom_remote *remote = wacom->remote;

	if (!remote->remotes[index].registered)
		return 0;

	if (WACOM_POWERSUPPLY_DEVICE(remote->remotes[index].battery.battery))
		return 0;

	return 0;
}

/*
 * Not all devices report physical dimensions from HID.
 * Compute the default from hardcoded logical dimension
 * and resolution before driver overwrites them.
 */
static void wacom_set_default_phy(struct wacom_features *features)
{
	if (features->x_resolution) {
		features->x_phy = (features->x_max * 100) /
					features->x_resolution;
		features->y_phy = (features->y_max * 100) /
					features->y_resolution;
	}
}

static void wacom_calculate_res(struct wacom_features *features)
{
	/* set unit to "100th of a mm" for devices not reported by HID */
	if (!features->unit) {
		features->unit = 0x11;
		features->unitExpo = 16-3;
	}

	features->x_resolution = wacom_calc_hid_res(features->x_max,
						    features->x_phy,
						    features->unit,
						    features->unitExpo);
	features->y_resolution = wacom_calc_hid_res(features->y_max,
						    features->y_phy,
						    features->unit,
						    features->unitExpo);
}

static void wacom_wireless_work(struct work_struct *work)
{
	struct wacom *wacom = container_of(work, struct wacom, wireless_work);
	struct usb_device *usbdev = wacom->usbdev;
	struct wacom_wac *wacom_wac = &wacom->wacom_wac;
	struct wacom *wacom1, *wacom2;
	struct wacom_wac *wacom_wac1, *wacom_wac2;
	int error;

	/*
	 * Regardless if this is a disconnect or a new tablet,
	 * remove any existing input and battery devices.
	 */

	wacom_destroy_battery(wacom);

	/* Stylus interface */
	wacom1 = usb_get_intfdata(usbdev->config->interface[1]);
	wacom_wac1 = &(wacom1->wacom_wac);
	wacom_unregister_inputs(wacom1);

	/* Touch interface */
	wacom2 = usb_get_intfdata(usbdev->config->interface[2]);
	wacom_wac2 = &(wacom2->wacom_wac);
	wacom_unregister_inputs(wacom2);

	if (wacom_wac->pid == 0) {
		dev_info(&wacom->intf->dev, "wireless tablet disconnected\n");
		wacom_wac1->shared->type = 0;
	} else {
		const struct usb_device_id *id = wacom_ids;

		dev_info(&wacom->intf->dev,
			 "wireless tablet connected with PID %x\n",
			 wacom_wac->pid);

		while (id->match_flags) {
			if (id->idVendor == USB_VENDOR_ID_WACOM &&
			    id->idProduct == wacom_wac->pid)
				break;
			id++;
		}

		if (!id->match_flags) {
			dev_info(&wacom->intf->dev,
				 "ignoring unknown PID.\n");
			return;
		}

		/* Stylus interface */
		wacom_wac1->features =
			*((struct wacom_features *)id->driver_info);
		wacom_wac1->features.device_type = BTN_TOOL_PEN;
		snprintf(wacom_wac1->name, WACOM_NAME_MAX, "%s (WL) Pen",
			 wacom_wac1->features.name);
		wacom_set_default_phy(&wacom_wac1->features);
		wacom_calculate_res(&wacom_wac1->features);
		wacom_wac1->shared->touch_max = wacom_wac1->features.touch_max;
		wacom_wac1->shared->type = wacom_wac1->features.type;
		wacom_wac1->pid = wacom_wac->pid;
		error = wacom_register_input(wacom1);
		if (error)
			goto fail;

		/* Touch interface */
		if (wacom_wac1->features.touch_max ||
		    (wacom_wac1->features.type >= INTUOSHT &&
		    wacom_wac1->features.type <= BAMBOO_PT)) {
			wacom_wac2->features =
				*((struct wacom_features *)id->driver_info);
			wacom_wac2->features.pktlen = WACOM_PKGLEN_BBTOUCH3;
			wacom_wac2->features.device_type = BTN_TOOL_FINGER;
			wacom_set_default_phy(&wacom_wac1->features);
			wacom_wac2->features.x_max = wacom_wac2->features.y_max = 4096;
			wacom_calculate_res(&wacom_wac1->features);
			if (wacom_wac1->features.touch_max)
				snprintf(wacom_wac2->name, WACOM_NAME_MAX,
					 "%s (WL) Finger",wacom_wac2->features.name);
			else
				snprintf(wacom_wac2->name, WACOM_NAME_MAX,
					 "%s (WL) Pad",wacom_wac2->features.name);
			wacom_wac2->pid = wacom_wac->pid;
			error = wacom_register_input(wacom2);
			if (error)
				goto fail;

			if ((wacom_wac1->features.type == INTUOSHT ||
			     wacom_wac1->features.type == INTUOSHT2) &&
			     wacom_wac1->features.touch_max) {
				wacom_wac->shared->type = wacom_wac->features.type;
				wacom_wac->shared->touch_input = wacom_wac2->input;
			}
		}
	}

	return;

fail:
	wacom_unregister_inputs(wacom1);
	wacom_unregister_inputs(wacom2);
	return;
}

void wacom_battery_work(struct work_struct *work)
{
	struct wacom *wacom = container_of(work, struct wacom, battery_work);

	if ((wacom->wacom_wac.features.quirks & WACOM_QUIRK_BATTERY) &&
	     !WACOM_POWERSUPPLY_DEVICE(wacom->battery.battery)) {
		wacom_initialize_battery(wacom);
	}
	else if (!(wacom->wacom_wac.features.quirks & WACOM_QUIRK_BATTERY) &&
		 WACOM_POWERSUPPLY_DEVICE(wacom->battery.battery)) {
		wacom_destroy_battery(wacom);
	}
}

static void wacom_remote_work(struct work_struct *work)
{
	struct wacom *wacom = container_of(work, struct wacom, remote_work);
	struct device *dev = &wacom->intf->dev;
	struct wacom_remote *remote = wacom->remote;
	struct wacom_remote_data data;
	unsigned long flags;
	unsigned int count;
	u32 serial;
	int i;

	spin_lock_irqsave(&remote->remote_lock, flags);

	count = kfifo_out(&remote->remote_fifo, &data, sizeof(data));

	if (count != sizeof(data)) {
		dev_err(dev,
			"workitem triggered without status available\n");
		spin_unlock_irqrestore(&remote->remote_lock, flags);
		return;
	}

	if (!kfifo_is_empty(&remote->remote_fifo))
		wacom_schedule_work(&wacom->wacom_wac, WACOM_WORKER_REMOTE);

	spin_unlock_irqrestore(&remote->remote_lock, flags);

	for (i = 0; i < WACOM_MAX_REMOTES; i++) {
		serial = data.remote[i].serial;
		if (data.remote[i].connected) {

			if (remote->remotes[i].serial == serial) {
				wacom_remote_attach_battery(wacom, i);
				continue;
			}

			if (remote->remotes[i].serial)
				wacom_remote_destroy_one(wacom, i);

			wacom_remote_create_one(wacom, serial, i);

		} else if (remote->remotes[i].serial) {
			wacom_remote_destroy_one(wacom, i);
		}
	}
}

static int wacom_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *endpoint;
	struct wacom *wacom;
	struct wacom_wac *wacom_wac;
	struct wacom_features *features;
	int error;
	struct usb_device *other_dev;

	if (!id->driver_info)
		return -EINVAL;

	/* Verify that a device really has an endpoint */
	if (intf->cur_altsetting->desc.bNumEndpoints < 1)
		return -EINVAL;

	wacom = devm_kzalloc(&dev->dev, sizeof(struct wacom), GFP_KERNEL);
	if (!wacom)
		return -ENOMEM;

	wacom_wac = &wacom->wacom_wac;
	wacom_wac->features = *((struct wacom_features *)id->driver_info);
	features = &wacom_wac->features;
	if (features->pktlen > WACOM_PKGLEN_MAX) {
		error = -EINVAL;
		goto fail1;
	}

	if ((features->type == WACOM_ONE || features->type == CINTIQ_16) &&
	    intf->cur_altsetting->desc.bInterfaceNumber != 0) {
		error = -EINVAL;
		goto fail1;
	}

	wacom_wac->data = usb_alloc_coherent(dev, WACOM_PKGLEN_MAX,
					     GFP_KERNEL, &wacom->data_dma);
	if (!wacom_wac->data) {
		error = -ENOMEM;
		goto fail1;
	}

	wacom->irq = usb_alloc_urb(0, GFP_KERNEL);
	if (!wacom->irq) {
		error = -ENOMEM;
		goto fail2;
	}

	wacom->usbdev = dev;
	wacom->intf = intf;
	mutex_init(&wacom->lock);
	INIT_WORK(&wacom->wireless_work, wacom_wireless_work);
	INIT_WORK(&wacom->battery_work, wacom_battery_work);
	INIT_WORK(&wacom->remote_work, wacom_remote_work);

	usb_make_path(dev, wacom->phys, sizeof(wacom->phys));
	strlcat(wacom->phys, "/input0", sizeof(wacom->phys));

	endpoint = &intf->cur_altsetting->endpoint[0].desc;

	/* set the default size in case we do not get them from hid */
	wacom_set_default_phy(features);

	/* Retrieve the physical and logical size for touch devices */
	error = wacom_retrieve_hid_descriptor(intf, features);
	if (error)
		goto fail3;

	wacom_setup_device_quirks(wacom);
	wacom_calculate_res(features);

	strlcpy(wacom_wac->name, features->name, sizeof(wacom_wac->name));

	/* Append the device type to the name */
	if (features->device_type != BTN_TOOL_FINGER)
		strlcat(wacom_wac->name, " Pen", WACOM_NAME_MAX);
	else if (features->touch_max)
		strlcat(wacom_wac->name, " Finger", WACOM_NAME_MAX);
	else
		strlcat(wacom_wac->name, " Pad", WACOM_NAME_MAX);

	other_dev = wacom_get_sibling(dev, features->oVid, features->oPid);
	if (other_dev == NULL || wacom_get_usbdev_data(other_dev) == NULL)
		other_dev = dev;
	error = wacom_add_shared_data(wacom_wac, other_dev);
	if (error)
		goto fail3;

	usb_fill_int_urb(wacom->irq, dev,
			 usb_rcvintpipe(dev, endpoint->bEndpointAddress),
			 wacom_wac->data, features->pktlen,
			 wacom_sys_irq, wacom, endpoint->bInterval);
	wacom->irq->transfer_dma = wacom->data_dma;
	wacom->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	if (!(features->quirks & WACOM_QUIRK_NO_INPUT)) {
		error = wacom_register_input(wacom);
		if (error)
			goto fail4;
	}

	/* Note that if query fails it is not a hard failure */
	wacom_query_tablet_data(intf, features);

	usb_set_intfdata(intf, wacom);

	if (features->quirks & WACOM_QUIRK_MONITOR) {
		if (usb_submit_urb(wacom->irq, GFP_KERNEL)) {
			error = -EIO;
			goto fail4;
		}
	}

	error = wacom_initialize_leds(wacom);
	if (error)
		goto fail4;

	if (wacom->wacom_wac.features.type == REMOTE) {
		error = wacom_initialize_remotes(wacom);
		if (error)
			goto fail4;
	}

	return 0;

 fail4:	wacom_remove_shared_data(wacom_wac);
 fail3:	usb_free_urb(wacom->irq);
	wacom_destroy_battery(wacom);
 fail2:	usb_free_coherent(dev, WACOM_PKGLEN_MAX, wacom_wac->data, wacom->data_dma);
 fail1:
	return error;
}

static void wacom_disconnect(struct usb_interface *intf)
{
	struct wacom *wacom = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);

	usb_kill_urb(wacom->irq);
	cancel_work_sync(&wacom->wireless_work);
	cancel_work_sync(&wacom->battery_work);
	cancel_work_sync(&wacom->remote_work);
	wacom_remotes_destroy(wacom);
	wacom_unregister_inputs(wacom);
	wacom_destroy_battery(wacom);
	usb_free_urb(wacom->irq);
	usb_free_coherent(interface_to_usbdev(intf), WACOM_PKGLEN_MAX,
			wacom->wacom_wac.data, wacom->data_dma);
	wacom_remove_shared_data(&wacom->wacom_wac);
}

static int wacom_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct wacom *wacom = usb_get_intfdata(intf);

	mutex_lock(&wacom->lock);
	usb_kill_urb(wacom->irq);
	mutex_unlock(&wacom->lock);

	return 0;
}

static int wacom_resume(struct usb_interface *intf)
{
	struct wacom *wacom = usb_get_intfdata(intf);
	struct wacom_features *features = &wacom->wacom_wac.features;
	int rv = 0;

	mutex_lock(&wacom->lock);

	/* switch to wacom mode first */
	wacom_query_tablet_data(intf, features);
	wacom_led_control(wacom);

	if ((wacom->open || (features->quirks & WACOM_QUIRK_MONITOR)) &&
	    usb_submit_urb(wacom->irq, GFP_NOIO) < 0)
		rv = -EIO;

	mutex_unlock(&wacom->lock);

	return rv;
}

static int wacom_reset_resume(struct usb_interface *intf)
{
	return wacom_resume(intf);
}

static struct usb_driver wacom_driver = {
	.name =		"wacom",
	.id_table =	wacom_ids,
	.probe =	wacom_probe,
	.disconnect =	wacom_disconnect,
	.suspend =	wacom_suspend,
	.resume =	wacom_resume,
	.reset_resume =	wacom_reset_resume,
	.supports_autosuspend = 1,
};

module_usb_driver(wacom_driver);

MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
