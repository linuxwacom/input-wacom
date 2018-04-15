/*
 * drivers/input/tablet/wacom_sys.c
 *
 *  USB Wacom tablet support - system specific code
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

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
#define HID_COLLECTION			0xa0
#define HID_COLLECTION_LOGICAL		0x02
#define HID_COLLECTION_END		0xc0
#define HID_LONGITEM			0xfc


enum {
	WCM_UNDEFINED = 0,
	WCM_DESKTOP,
	WCM_DIGITIZER,
};

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

#define WAC_CMD_LED_CONTROL	0x20
#define WAC_CMD_RETRIES		10

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

	mutex_lock(&wacom->lock);

	wacom->irq->dev = wacom->usbdev;

	if (usb_autopm_get_interface(wacom->intf) < 0) {
		mutex_unlock(&wacom->lock);
		return -EIO;
	}

	if (usb_submit_urb(wacom->irq, GFP_KERNEL)) {
		usb_autopm_put_interface(wacom->intf);
		mutex_unlock(&wacom->lock);
		return -EIO;
	}

	wacom->open = true;
	wacom->intf->needs_remote_wakeup = 1;

	mutex_unlock(&wacom->lock);
	return 0;
}

static void wacom_close(struct input_dev *dev)
{
	struct wacom *wacom = input_get_drvdata(dev);

	mutex_lock(&wacom->lock);
	usb_kill_urb(wacom->irq);
	wacom->open = false;
	wacom->intf->needs_remote_wakeup = 0;
	mutex_unlock(&wacom->lock);
}

static int wacom_parse_hid(struct usb_interface *intf, struct hid_descriptor *hid_desc,
			   struct wacom_features *features)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	char limit = 0;
	/* result has to be defined as int for some devices */
	int result = 0;
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
			/* fall through */
		case 2:
			data |= (report[i+2] << 8);
			/* fall through */
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
				features->device_type = BTN_TOOL_TRIPLETAP;
				if (features->type == INTUOSP2) {
					features->pktlen = WACOM_PKGLEN_INTUOSP2T;
					features->unit = report[i+4];
					features->unitExpo = report[i+6];
					features->x_phy = get_unaligned_le16(&report[i + 10]);
					features->x_max = get_unaligned_le16(&report[i + 15]);
				}
				break;

			case HID_USAGE_WT_Y:
				if (features->type == INTUOSP2) {
					features->y_phy = get_unaligned_le16(&report[i + 4]);
					features->y_max = get_unaligned_le16(&report[i + 7]);
				}
				break;

			case HID_USAGE_X:
				if (finger) {
					features->device_type = BTN_TOOL_DOUBLETAP;
					if (features->type == TABLETPC2FG ||
							 features->type == MTTPC ||
							 features->type == MTTPC_B ||
							 features->type == MTTPC_C ||
							 features->type == MTSCREEN ||
							 features->type == WACOM_24HDT ||
							 features->type == WACOM_MSPROT ||
							 features->type == DTH1152T ||
							 features->type == WACOM_27QHDT ||
							 features->type == DTH2452T) {
						/* need to reset back */
						features->pktlen = WACOM_PKGLEN_TPC2FG;
						if (features->type == MTTPC ||
						    features->type == MTTPC_B ||
						    features->type == MTTPC_C)
							features->pktlen = WACOM_PKGLEN_MTTPC;
						else if (features->type == WACOM_24HDT ||
							 features->type == MTSCREEN)
							features->pktlen = WACOM_PKGLEN_MTOUCH;
						else if (features->type == WACOM_MSPROT ||
							 features->type == DTH2452T)
							features->pktlen = WACOM_PKGLEN_MSPROT;
						else if (features->type == DTH1152T ||
							 features->type == WACOM_27QHDT)
							features->pktlen = WACOM_PKGLEN_27QHDT;
						features->device_type = BTN_TOOL_TRIPLETAP;
					}

					switch (features->type) {
					case BAMBOO_PT:
						/* need to reset back */
						features->pktlen = WACOM_PKGLEN_BBTOUCH;
						features->device_type = BTN_TOOL_TRIPLETAP;
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
					if (features->type == TABLETPC2FG ||
					    features->type == MTTPC ||
					    features->type == MTSCREEN)
						features->pktlen = WACOM_PKGLEN_GRAPHIRE;
					if (features->type == BAMBOO_PT)
						features->pktlen = WACOM_PKGLEN_BBFUN;
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

			case HID_USAGE_WT_STYLUS:
			case HID_USAGE_STYLUS:
				pen = 1;
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
			switch (data) {
			case HID_COLLECTION_LOGICAL:
				if (features->type == BAMBOO_PT) {
					features->pktlen = WACOM_PKGLEN_BBTOUCH3;
					features->device_type = BTN_TOOL_DOUBLETAP;

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
	result = 0;
	kfree(report);
	return result;
}

static int wacom_query_tablet_data(struct usb_interface *intf, struct wacom_features *features)
{
	unsigned char *rep_data;
	int limit = 0, report_id = 2;
	int error = -ENOMEM;

	rep_data = kmalloc(4, GFP_KERNEL);
	if (!rep_data)
		return error;

	/* ask to report Wacom data */
	if (features->device_type != BTN_TOOL_PEN) {
		/* if it is an MT Tablet PC touch */
		if (features->type > TABLETPC) {
			do {
				rep_data[0] = 3;
				rep_data[1] = 4;
				rep_data[2] = 0;
				rep_data[3] = 0;
				report_id = 3;
				error = wacom_set_report(intf, WAC_HID_FEATURE_REPORT,
					report_id, rep_data, 4, 1);
			} while ((error < 0 || rep_data[1] != 4) && limit++ < 5);
		}
		else if (features->type == WACOM_24HDT) {
			do {
				rep_data[0] = 18;
				rep_data[1] = 2;
				report_id = 18;
				error = wacom_set_report(intf, WAC_HID_FEATURE_REPORT,
					report_id, rep_data, 3, 1);
			} while ((error < 0 || rep_data[1] != 2) && limit++ < 5);
		}
		else if (features->type == WACOM_27QHDT) {
			do {
				rep_data[0] = 131;
				rep_data[1] = 2;
				report_id = 131;
				error = wacom_set_report(intf, WAC_HID_FEATURE_REPORT,
					report_id, rep_data, 3, 1);
			} while ((error < 0 || rep_data[1] != 2) && limit++ < 5);
		} else if (features->type == WACOM_MSPROT ||
			   features->type == DTH1152T) {
			do {
				rep_data[0] = 14;
				rep_data[1] = 2;
				report_id = 14;
				error = wacom_set_report(intf, WAC_HID_FEATURE_REPORT,
					report_id, rep_data, 2, 1);
			} while ((error < 0 || rep_data[1] != 2) && limit++ < 5);
		}
	} else if (features->type <= BAMBOO_PT) {
		do {
			rep_data[0] = 2;
			rep_data[1] = 2;
			error = wacom_set_report(intf, WAC_HID_FEATURE_REPORT,
				report_id, rep_data, 2, 1);
		} while ((error < 0 || rep_data[1] != 2) && limit++ < 5);
	}

	kfree(rep_data);

	return error < 0 ? error : 0;
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
	if (error)
		goto out;

	/* touch device found but size is not defined. use default */
	if (features->device_type == BTN_TOOL_DOUBLETAP && !features->x_max) {
		features->x_max = 1023;
		features->y_max = 1023;
	}

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
	int i;

	if (vendor == 0 && product == 0)
		return dev;

	if (dev->parent == NULL)
		return NULL;

	for (i = 0 ; i < dev->parent->maxchild; i++) {
		struct usb_device *sibling = dev->parent->children[i];
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
	}

	retval = wacom_set_report(wacom->intf, 0x03, report_id,
				  buf, buf_size, WAC_CMD_RETRIES);
	kfree(buf);

	return retval;
}

static ssize_t wacom_led_select_store(struct device *dev, int set_id,
				      const char *buf, size_t count)
{
	struct wacom *wacom = dev_get_drvdata(dev);
	unsigned long id;
	int err;

	err = strict_strtoul(buf, 10, &id);
	if (err)
		return err;

	mutex_lock(&wacom->lock);

	wacom->led.select[set_id] = id;
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
static DEVICE_ATTR(status_led##SET_ID##_select, S_IWUSR | S_IRUSR,	\
		    wacom_led##SET_ID##_select_show,			\
		    wacom_led##SET_ID##_select_store)

DEVICE_LED_SELECT_ATTR(0);
DEVICE_LED_SELECT_ATTR(1);

static struct attribute *cintiq_led_attrs[] = {
	&dev_attr_status_led0_select.attr,
	&dev_attr_status_led1_select.attr,
	NULL
};

static struct attribute_group cintiq_led_attr_group = {
	.name = "wacom_led",
	.attrs = cintiq_led_attrs,
};

static struct attribute *intuos4_led_attrs[] = {
	&dev_attr_status_led0_select.attr,
	NULL
};

static struct attribute_group intuos4_led_attr_group = {
	.name = "wacom_led",
	.attrs = intuos4_led_attrs,
};

static struct attribute *intuos5_led_attrs[] = {
	&dev_attr_status_led0_select.attr,
	NULL
};

static struct attribute_group intuos5_led_attr_group = {
	.name = "wacom_led",
	.attrs = intuos5_led_attrs,
};

struct wacom_sysfs_group_devres {
	struct attribute_group *group;
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
					   struct attribute_group *group)
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
	if (error)
		return error;

	devres_add(&wacom->intf->dev, devres);

	return 0;
}

static int wacom_devm_sysfs_create_group(struct wacom *wacom,
					 struct attribute_group *group)
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

		error = wacom_devm_sysfs_create_group(wacom,
						      &intuos4_led_attr_group);
		break;

	case WACOM_24HD:
	case WACOM_21UX2:
		wacom->led.select[0] = 0;
		wacom->led.select[1] = 0;
		wacom->led.llv = 0;
		wacom->led.hlv = 0;

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

static int wacom_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *endpoint;
	struct wacom *wacom;
	struct wacom_wac *wacom_wac;
	struct wacom_features *features;
	struct input_dev *input_dev;
	int error;
	struct usb_device *other_dev;

	if (!id->driver_info)
		return -EINVAL;

	/* Verify that a device really has an endpoint */
	if (intf->cur_altsetting->desc.bNumEndpoints < 1)
		return -EINVAL;

	wacom = devm_kzalloc(&dev->dev, sizeof(struct wacom), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!wacom || !input_dev) {
		error = -ENOMEM;
		goto fail1;
	}

	wacom_wac = &wacom->wacom_wac;
	wacom_wac->features = *((struct wacom_features *)id->driver_info);
	features = &wacom_wac->features;
	if (features->pktlen > WACOM_PKGLEN_MAX) {
		error = -EINVAL;
		goto fail1;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
	wacom_wac->data = usb_buffer_alloc(dev, WACOM_PKGLEN_MAX,
					   GFP_KERNEL, &wacom->data_dma);
#else
	wacom_wac->data = usb_alloc_coherent(dev, WACOM_PKGLEN_MAX,
					     GFP_KERNEL, &wacom->data_dma);
#endif
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
	usb_make_path(dev, wacom->phys, sizeof(wacom->phys));
	strlcat(wacom->phys, "/input0", sizeof(wacom->phys));

	wacom_wac->input = input_dev;

	endpoint = &intf->cur_altsetting->endpoint[0].desc;

	/* Retrieve the physical and logical size for OEM devices */
	error = wacom_retrieve_hid_descriptor(intf, features);
	if (error)
		goto fail2;

	wacom_setup_device_quirks(wacom);

	strlcpy(wacom_wac->name, features->name, sizeof(wacom_wac->name));

	/* Append the device type to the name */
	strlcat(wacom_wac->name,
		features->device_type == BTN_TOOL_PEN ? " Pen" : " Finger",
		sizeof(wacom_wac->name));

	other_dev = wacom_get_sibling(dev, features->oVid, features->oPid);
	if (other_dev == NULL || wacom_get_usbdev_data(other_dev) == NULL)
		other_dev = dev;
	error = wacom_add_shared_data(wacom_wac, other_dev);
	if (error)
		goto fail3;

	input_dev->name = wacom_wac->name;
	input_dev->dev.parent = &intf->dev;
	input_dev->open = wacom_open;
	input_dev->close = wacom_close;
	usb_to_input_id(dev, &input_dev->id);
	input_set_drvdata(input_dev, wacom);

	wacom_setup_input_capabilities(input_dev, wacom_wac);

	usb_fill_int_urb(wacom->irq, dev,
			 usb_rcvintpipe(dev, endpoint->bEndpointAddress),
			 wacom_wac->data, features->pktlen,
			 wacom_sys_irq, wacom, endpoint->bInterval);
	wacom->irq->transfer_dma = wacom->data_dma;
	wacom->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	error = wacom_initialize_leds(wacom);
	if (error)
		goto fail4;

	error = input_register_device(input_dev);
	if (error)
		goto fail4;

	if (wacom_wac->features.touch_max && wacom_wac->shared) {
		if (wacom_wac->features.device_type == BTN_TOOL_DOUBLETAP ||
		    wacom_wac->features.device_type == BTN_TOOL_TRIPLETAP) {
			wacom_wac->shared->type = wacom_wac->features.type;
			wacom_wac->shared->touch_input = wacom_wac->input;
		}
	}

	/* Note that if query fails it is not a hard failure */
	wacom_query_tablet_data(intf, features);

	usb_set_intfdata(intf, wacom);
	return 0;

 fail4:	wacom_remove_shared_data(wacom_wac);
 fail3:	usb_free_urb(wacom->irq);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
 fail2:	usb_buffer_free(dev, WACOM_PKGLEN_MAX, wacom_wac->data, wacom->data_dma);
#else
 fail2: usb_free_coherent(dev, WACOM_PKGLEN_MAX, wacom_wac->data, wacom->data_dma);
#endif
 fail1:	input_free_device(input_dev);
	return error;
}

static void wacom_disconnect(struct usb_interface *intf)
{
	struct wacom *wacom = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);

	usb_kill_urb(wacom->irq);
	input_unregister_device(wacom->wacom_wac.input);
	usb_free_urb(wacom->irq);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
	usb_buffer_free(interface_to_usbdev(intf), WACOM_PKGLEN_MAX,
			wacom->wacom_wac.data, wacom->data_dma);
#else
	usb_free_coherent(interface_to_usbdev(intf), WACOM_PKGLEN_MAX,
			wacom->wacom_wac.data, wacom->data_dma);
#endif
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
	int rv;

	mutex_lock(&wacom->lock);

	/* switch to wacom mode first */
	wacom_query_tablet_data(intf, features);
	wacom_led_control(wacom);

	if (wacom->open)
		rv = usb_submit_urb(wacom->irq, GFP_NOIO);
	else
		rv = 0;
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

static int __init wacom_init(void)
{
	int result;

	result = usb_register(&wacom_driver);
	if (result == 0)
		printk(KERN_INFO KBUILD_MODNAME ": " DRIVER_VERSION ":"
		       DRIVER_DESC "\n");
	return result;
}

static void __exit wacom_exit(void)
{
	usb_deregister(&wacom_driver);
}

module_init(wacom_init);
module_exit(wacom_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE(DRIVER_LICENSE);
