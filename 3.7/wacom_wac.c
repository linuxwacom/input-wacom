// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * drivers/input/tablet/wacom_wac.c
 *
 *  USB Wacom tablet support - Wacom specific code
 */

/*
 */

#include "wacom_wac.h"
#include "wacom.h"
#include <linux/input/mt.h>
#include <linux/hid.h>

#ifndef SW_MUTE_DEVICE
#define SW_MUTE_DEVICE		0x0e  /* set = device disabled */
#endif

#ifndef KEY_ONSCREEN_KEYBOARD
#define KEY_ONSCREEN_KEYBOARD	0x278
#endif

#ifndef KEY_BUTTONCONFIG
#define KEY_BUTTONCONFIG		0x240
#endif

#ifndef KEY_CONTROLPANEL
#define KEY_CONTROLPANEL		0x243
#endif

/* resolution for penabled devices */
#define WACOM_PL_RES		20
#define WACOM_PENPRTN_RES	40
#define WACOM_VOLITO_RES	50
#define WACOM_GRAPHIRE_RES	80
#define WACOM_INTUOS_RES	100
#define WACOM_INTUOS3_RES	200

/* Newer Cintiq and DTU have an offset between tablet and screen areas */
#define WACOM_DTU_OFFSET	200
#define WACOM_CINTIQ_OFFSET	400

/*
 * Scale factor relating reported contact size to logical contact area.
 * 2^14/pi is a good approximation on Intuos5 and 3rd-gen Bamboo
 */
#define WACOM_CONTACT_AREA_SCALE 2607

static bool touch_arbitration = 1;
module_param(touch_arbitration, bool, 0644);
MODULE_PARM_DESC(touch_arbitration, " on (Y) off (N)");

static void wacom_report_numbered_buttons(struct input_dev *input_dev,
				int button_count, int mask);

static void __wacom_notify_battery(struct wacom_battery *battery,
				   int bat_status, int bat_capacity,
				   bool bat_charging, bool bat_connected,
				   bool ps_connected)
{
	bool changed = battery->bat_status       != bat_status    ||
		       battery->battery_capacity != bat_capacity  ||
		       battery->bat_charging     != bat_charging  ||
		       battery->bat_connected    != bat_connected ||
		       battery->ps_connected     != ps_connected;

	if (changed) {
		battery->bat_status = bat_status;
		battery->battery_capacity = bat_capacity;
		battery->bat_charging = bat_charging;
		battery->bat_connected = bat_connected;
		battery->ps_connected = ps_connected;

		if (battery->battery.dev)
			power_supply_changed(&battery->battery);
	}
}

static void wacom_notify_battery(struct wacom_wac *wacom_wac,
	int bat_status, int bat_capacity, bool bat_charging,
	bool bat_connected, bool ps_connected)
{
	struct wacom *wacom = container_of(wacom_wac, struct wacom, wacom_wac);

	__wacom_notify_battery(&wacom->battery, bat_status, bat_capacity,
			       bat_charging, bat_connected, ps_connected);
}

static int wacom_penpartner_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;

	switch (data[0]) {
	case 1:
		if (data[5] & 0x80) {
			wacom->tool[0] = (data[5] & 0x20) ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
			wacom->id[0] = (data[5] & 0x20) ? ERASER_DEVICE_ID : STYLUS_DEVICE_ID;
			input_report_key(input, wacom->tool[0], 1);
			input_report_abs(input, ABS_MISC, wacom->id[0]); /* report tool id */
			input_report_abs(input, ABS_X, get_unaligned_le16(&data[1]));
			input_report_abs(input, ABS_Y, get_unaligned_le16(&data[3]));
			input_report_abs(input, ABS_PRESSURE, (signed char)data[6] + 127);
			input_report_key(input, BTN_TOUCH, ((signed char)data[6] > -127));
			input_report_key(input, BTN_STYLUS, (data[5] & 0x40));
		} else {
			input_report_key(input, wacom->tool[0], 0);
			input_report_abs(input, ABS_MISC, 0); /* report tool id */
			input_report_abs(input, ABS_PRESSURE, -1);
			input_report_key(input, BTN_TOUCH, 0);
		}
		break;

	case 2:
		input_report_key(input, BTN_TOOL_PEN, 1);
		input_report_abs(input, ABS_MISC, STYLUS_DEVICE_ID); /* report tool id */
		input_report_abs(input, ABS_X, get_unaligned_le16(&data[1]));
		input_report_abs(input, ABS_Y, get_unaligned_le16(&data[3]));
		input_report_abs(input, ABS_PRESSURE, (signed char)data[6] + 127);
		input_report_key(input, BTN_TOUCH, ((signed char)data[6] > -80) && !(data[5] & 0x20));
		input_report_key(input, BTN_STYLUS, (data[5] & 0x40));
		break;

	default:
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d\n", __func__, data[0]);
		return 0;
        }

	return 1;
}

static int wacom_pl_irq(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int prox, pressure;

	if (data[0] != WACOM_REPORT_PENABLED) {
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d\n", __func__, data[0]);
		return 0;
	}

	prox = data[1] & 0x40;

	if (prox) {
		wacom->id[0] = ERASER_DEVICE_ID;
		pressure = (signed char)((data[7] << 1) | ((data[4] >> 2) & 1));
		if (features->pressure_max > 255)
			pressure = (pressure << 1) | ((data[4] >> 6) & 1);
		pressure += (features->pressure_max + 1) / 2;

		/*
		 * if going from out of proximity into proximity select between the eraser
		 * and the pen based on the state of the stylus2 button, choose eraser if
		 * pressed else choose pen. if not a proximity change from out to in, send
		 * an out of proximity for previous tool then a in for new tool.
		 */
		if (!wacom->tool[0]) {
			/* Eraser bit set for DTF */
			if (data[1] & 0x10)
				wacom->tool[1] = BTN_TOOL_RUBBER;
			else
				/* Going into proximity select tool */
				wacom->tool[1] = (data[4] & 0x20) ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
		} else {
			/* was entered with stylus2 pressed */
			if (wacom->tool[1] == BTN_TOOL_RUBBER && !(data[4] & 0x20)) {
				/* report out proximity for previous tool */
				input_report_key(input, wacom->tool[1], 0);
				input_sync(input);
				wacom->tool[1] = BTN_TOOL_PEN;
				return 0;
			}
		}
		if (wacom->tool[1] != BTN_TOOL_RUBBER) {
			/* Unknown tool selected default to pen tool */
			wacom->tool[1] = BTN_TOOL_PEN;
			wacom->id[0] = STYLUS_DEVICE_ID;
		}
		input_report_key(input, wacom->tool[1], prox); /* report in proximity for tool */
		input_report_abs(input, ABS_MISC, wacom->id[0]); /* report tool id */
		input_report_abs(input, ABS_X, data[3] | (data[2] << 7) | ((data[1] & 0x03) << 14));
		input_report_abs(input, ABS_Y, data[6] | (data[5] << 7) | ((data[4] & 0x03) << 14));
		input_report_abs(input, ABS_PRESSURE, pressure);

		input_report_key(input, BTN_TOUCH, data[4] & 0x08);
		input_report_key(input, BTN_STYLUS, data[4] & 0x10);
		/* Only allow the stylus2 button to be reported for the pen tool. */
		input_report_key(input, BTN_STYLUS2, (wacom->tool[1] == BTN_TOOL_PEN) && (data[4] & 0x20));
	} else {
		/* report proximity-out of a (valid) tool */
		if (wacom->tool[1] != BTN_TOOL_RUBBER) {
			/* Unknown tool selected default to pen tool */
			wacom->tool[1] = BTN_TOOL_PEN;
		}
		input_report_key(input, wacom->tool[1], prox);
	}

	wacom->tool[0] = prox; /* Save proximity state */
	return 1;
}

static int wacom_ptu_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;

	if (data[0] != WACOM_REPORT_PENABLED) {
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d\n", __func__, data[0]);
		return 0;
	}

	if (data[1] & 0x04) {
		input_report_key(input, BTN_TOOL_RUBBER, data[1] & 0x20);
		input_report_key(input, BTN_TOUCH, data[1] & 0x08);
		wacom->id[0] = ERASER_DEVICE_ID;
	} else {
		input_report_key(input, BTN_TOOL_PEN, data[1] & 0x20);
		input_report_key(input, BTN_TOUCH, data[1] & 0x01);
		wacom->id[0] = STYLUS_DEVICE_ID;
	}
	input_report_abs(input, ABS_MISC, wacom->id[0]); /* report tool id */
	input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
	input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
	input_report_abs(input, ABS_PRESSURE, le16_to_cpup((__le16 *)&data[6]));
	input_report_key(input, BTN_STYLUS, data[1] & 0x02);
	input_report_key(input, BTN_STYLUS2, data[1] & 0x10);
	return 1;
}

static int wacom_dtu_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int prox = data[1] & 0x20;

	dev_dbg(input->dev.parent,
		"%s: received report #%d", __func__, data[0]);

	if (prox) {
		/* Going into proximity select tool */
		wacom->tool[0] = (data[1] & 0x0c) ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
		if (wacom->tool[0] == BTN_TOOL_PEN)
			wacom->id[0] = STYLUS_DEVICE_ID;
		else
			wacom->id[0] = ERASER_DEVICE_ID;
	}
	input_report_key(input, BTN_STYLUS, data[1] & 0x02);
	input_report_key(input, BTN_STYLUS2, data[1] & 0x10);
	input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
	input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
	input_report_abs(input, ABS_PRESSURE, ((data[7] & 0x01) << 8) | data[6]);
	input_report_key(input, BTN_TOUCH, data[1] & 0x05);
	if (!prox) /* out-prox */
		wacom->id[0] = 0;
	input_report_key(input, wacom->tool[0], prox);
	input_report_abs(input, ABS_MISC, wacom->id[0]);
	return 1;
}

static int wacom_dtus_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	struct wacom_features *features = &wacom->features;
	unsigned short prox, pressure = 0;

	if (data[0] != WACOM_REPORT_DTUS && data[0] != WACOM_REPORT_DTUSPAD
			&& data[0] != WACOM_REPORT_DTK2451PAD) {
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d", __func__, data[0]);
		return 0;
	} else if (data[0] == WACOM_REPORT_DTUSPAD
			|| data[0] == WACOM_REPORT_DTK2451PAD) {
		input_report_key(input, BTN_0, (data[1] & 0x01));
		input_report_key(input, BTN_1, (data[1] & 0x02));
		input_report_key(input, BTN_2, (data[1] & 0x04));
		input_report_key(input, BTN_3, (data[1] & 0x08));
		input_report_abs(input, ABS_MISC,
				 data[1] & 0x0f ? PAD_DEVICE_ID : 0);
		/*
		 * Serial number is required when expresskeys are
		 * reported through pen interface.
		 */
		input_event(input, EV_MSC, MSC_SERIAL, 0xf0);
		return 1;
	} else {
		prox = data[1] & 0x80;
		if (prox) {
			switch ((data[1] >> 3) & 3) {
			case 1: /* Rubber */
				wacom->tool[0] = BTN_TOOL_RUBBER;
				wacom->id[0] = ERASER_DEVICE_ID;
				break;

			case 2: /* Pen */
				wacom->tool[0] = BTN_TOOL_PEN;
				wacom->id[0] = STYLUS_DEVICE_ID;
				break;
			}
		}

		input_report_key(input, BTN_STYLUS, data[1] & 0x20);
		input_report_key(input, BTN_STYLUS2, data[1] & 0x40);
		if (features->type  == DTK2451) {
			pressure = get_unaligned_le16(&data[2]);
			input_report_abs(input, ABS_X, get_unaligned_le16(&data[4]));
			input_report_abs(input, ABS_Y, get_unaligned_le16(&data[6]));
		} else {
			pressure = ((data[1] & 0x03) << 8) | (data[2] & 0xff);
			input_report_abs(input, ABS_X, get_unaligned_be16(&data[3]));
			input_report_abs(input, ABS_Y, get_unaligned_be16(&data[5]));
		}
		input_report_abs(input, ABS_PRESSURE, pressure);
		input_report_key(input, BTN_TOUCH, pressure > 10);

		if (!prox) /* out-prox */
			wacom->id[0] = 0;
		input_report_key(input, wacom->tool[0], prox);
		input_report_abs(input, ABS_MISC, wacom->id[0]);
		input_event(input, EV_MSC, MSC_SERIAL, 1);
		return 1;
	}
}

static int wacom_dth1152_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	unsigned short prox, pressure = 0;

	if (data[0] != WACOM_REPORT_DTUS) {
		if (data[0] == WACOM_REPORT_DTUSPAD) {
			input_report_key(input, BTN_0, (data[1] & 0x01));
			input_report_key(input, BTN_1, (data[1] & 0x02));
			input_report_key(input, BTN_2, (data[1] & 0x04));
			input_report_key(input, BTN_3, (data[1] & 0x08));
			input_report_abs(input, ABS_MISC,
				 data[1] & 0x0f ? PAD_DEVICE_ID : 0);
			/*
			 * Serial number is required when expresskeys are
			 * reported through pen interface.
			 */
			input_event(input, EV_MSC, MSC_SERIAL, 0xf0);
			return 1;
		}
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d", __func__, data[0]);
		return 0;
	} else {
		prox = data[1] & 0x80;
		if (prox) {
			wacom->tool[0] = BTN_TOOL_PEN;
			wacom->id[0] = STYLUS_DEVICE_ID;
		}
		input_report_key(input, BTN_STYLUS, data[1] & 0x20);
		input_report_abs(input, ABS_X, get_unaligned_le16(&data[4]));
		input_report_abs(input, ABS_Y, get_unaligned_le16(&data[6]));
		pressure = data[2] | (data[3] << 8);
		input_report_abs(input, ABS_PRESSURE, pressure);
		input_report_key(input, BTN_TOUCH, data[1] & 0x10);

		if (!prox)
			wacom->id[0] = 0;
		input_report_key(input, wacom->tool[0], prox);
		input_report_abs(input, ABS_MISC, wacom->id[0]);
		return 1;
	}
}

static int wacom_graphire_irq(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int prox;
	int rw = 0;
	int retval = 0;

	if (data[0] != WACOM_REPORT_PENABLED) {
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d\n", __func__, data[0]);
		goto exit;
	}

	prox = data[1] & 0x80;
	if (prox || wacom->id[0]) {
		if (prox) {
			switch ((data[1] >> 5) & 3) {

			case 0:	/* Pen */
				wacom->tool[0] = BTN_TOOL_PEN;
				wacom->id[0] = STYLUS_DEVICE_ID;
				break;

			case 1: /* Rubber */
				wacom->tool[0] = BTN_TOOL_RUBBER;
				wacom->id[0] = ERASER_DEVICE_ID;
				break;

			case 2: /* Mouse with wheel */
				input_report_key(input, BTN_MIDDLE, data[1] & 0x04);
				/* fall through */

			case 3: /* Mouse without wheel */
				wacom->tool[0] = BTN_TOOL_MOUSE;
				wacom->id[0] = CURSOR_DEVICE_ID;
				break;
			}
		}
		input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
		input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
		if (wacom->tool[0] != BTN_TOOL_MOUSE) {
			input_report_abs(input, ABS_PRESSURE, data[6] | ((data[7] & 0x03) << 8));
			input_report_key(input, BTN_TOUCH, data[1] & 0x01);
			input_report_key(input, BTN_STYLUS, data[1] & 0x02);
			input_report_key(input, BTN_STYLUS2, data[1] & 0x04);
		} else {
			input_report_key(input, BTN_LEFT, data[1] & 0x01);
			input_report_key(input, BTN_RIGHT, data[1] & 0x02);
			if (features->type == WACOM_G4 ||
					features->type == WACOM_MO) {
				input_report_abs(input, ABS_DISTANCE, data[6] & 0x3f);
				rw = (data[7] & 0x04) - (data[7] & 0x03);
			} else {
				input_report_abs(input, ABS_DISTANCE, data[7] & 0x3f);
				rw = -(signed char)data[6];
			}
			input_report_rel(input, REL_WHEEL, rw);
		}

		if (!prox)
			wacom->id[0] = 0;
		input_report_abs(input, ABS_MISC, wacom->id[0]); /* report tool id */
		input_report_key(input, wacom->tool[0], prox);
		input_event(input, EV_MSC, MSC_SERIAL, 1);
		input_sync(input); /* sync last event */
	}

	/* send pad data */
	switch (features->type) {
	case WACOM_G4:
		prox = data[7] & 0xf8;
		if (prox || wacom->id[1]) {
			wacom->id[1] = PAD_DEVICE_ID;
			input_report_key(input, BTN_BACK, (data[7] & 0x40));
			input_report_key(input, BTN_FORWARD, (data[7] & 0x80));
			rw = ((data[7] & 0x18) >> 3) - ((data[7] & 0x20) >> 3);
			input_report_rel(input, REL_WHEEL, rw);
			if (!prox)
				wacom->id[1] = 0;
			input_report_abs(input, ABS_MISC, wacom->id[1]);
			input_event(input, EV_MSC, MSC_SERIAL, 0xf0);
			retval = 1;
		}
		break;

	case WACOM_MO:
		prox = (data[7] & 0xf8) || data[8];
		if (prox || wacom->id[1]) {
			wacom->id[1] = PAD_DEVICE_ID;
			input_report_key(input, BTN_BACK, (data[7] & 0x08));
			input_report_key(input, BTN_LEFT, (data[7] & 0x20));
			input_report_key(input, BTN_FORWARD, (data[7] & 0x10));
			input_report_key(input, BTN_RIGHT, (data[7] & 0x40));
			input_report_abs(input, ABS_WHEEL, (data[8] & 0x7f));
			if (!prox)
				wacom->id[1] = 0;
			input_report_abs(input, ABS_MISC, wacom->id[1]);
			input_event(input, EV_MSC, MSC_SERIAL, 0xf0);
			retval = 1;
		}
		break;
	}
exit:
	return retval;
}

static int wacom_intuos_pad(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int i;
	int buttons = 0, nbuttons = features->numbered_buttons;
	int keys = 0, nkeys = 0;
	int ring1 = 0, ring2 = 0;
	int strip1 = 0, strip2 = 0;
	bool prox = false;
	bool wrench = false, keyboard = false, mute_touch = false, menu = false,
	     info = false;

	/* pad packets. Works as a second tool and is always in prox */
	if (!(data[0] == WACOM_REPORT_INTUOSPAD || data[0] == WACOM_REPORT_INTUOS5PAD ||
	      data[0] == WACOM_REPORT_CINTIQPAD))
		return 0;

	if (features->type >= INTUOS4S && features->type <= INTUOS4L) {
		buttons = (data[3] << 1) | (data[2] & 0x01);
		ring1 = data[1];
	} else if (features->type == DTK) {
		buttons = data[6];
	} else if (features->type == WACOM_13HD) {
		buttons = (data[4] << 1) | (data[3] & 0x01);
	} else if (features->type == WACOM_24HD) {
		buttons = (data[8] << 8) | data[6];
		ring1 = data[1];
		ring2 = data[2];

		/*
		 * Three "buttons" are available on the 24HD which are
		 * physically implemented as a touchstrip. Each button
		 * is approximately 3 bits wide with a 2 bit spacing.
		 * The raw touchstrip bits are stored at:
		 *    ((data[3] & 0x1f) << 8) | data[4])
		 */
		nkeys = 3;
		keys = ((data[3] & 0x1C) ? 1<<2 : 0) |
		       ((data[4] & 0xE0) ? 1<<1 : 0) |
		       ((data[4] & 0x07) ? 1<<0 : 0);
		keyboard = !!(data[4] & 0xE0);
		info = !!(data[3] & 0x1C);

		if (features->oPid) {
			mute_touch = !!(data[4] & 0x07);
			if (mute_touch)
				wacom->shared->is_touch_on =
					!wacom->shared->is_touch_on;
		} else {
			wrench = !!(data[4] & 0x07);
		}
	} else if (features->type == WACOM_27QHD) {
		nkeys = 3;
		keys = data[2] & 0x07;

		wrench = !!(data[2] & 0x01);
		keyboard = !!(data[2] & 0x02);

		if (features->oPid) {
			mute_touch = !!(data[2] & 0x04);
			if (mute_touch)
				wacom->shared->is_touch_on =
					!wacom->shared->is_touch_on;
		} else {
			menu = !!(data[2] & 0x04);
		}
	} else if (features->type == CINTIQ_HYBRID) {
		/*
		 * Do not send hardware buttons under Android. They
		 * are already sent to the system through GPIO (and
		 * have different meaning).
		 *
		 * d-pad right  -> data[4] & 0x10
		 * d-pad up     -> data[4] & 0x20
		 * d-pad left   -> data[4] & 0x40
		 * d-pad down   -> data[4] & 0x80
		 * d-pad center -> data[3] & 0x01
		 */
		buttons = (data[4] << 1) | (data[3] & 0x01);
	} else if (features->type == CINTIQ_COMPANION_2) {
		/* d-pad right  -> data[2] & 0x10
		 * d-pad up     -> data[2] & 0x20
		 * d-pad left   -> data[2] & 0x40
		 * d-pad down   -> data[2] & 0x80
		 * d-pad center -> data[1] & 0x01
		 */
		buttons = ((data[2] >> 4) << 7) |
		          ((data[1] & 0x04) << 4) |
		          ((data[2] & 0x0F) << 2) |
		          (data[1] & 0x03);
	} else if (features->type >= INTUOS5S && features->type <= INTUOSPL) {
		/*
		 * ExpressKeys on Intuos5/Intuos Pro have a capacitive sensor in
		 * addition to the mechanical switch. Switch data is
		 * stored in data[4], capacitive data in data[5].
		 *
		 * Touch ring mode switch (data[3]) has no capacitive sensor
		 */
		buttons = (data[4] << 1) | (data[3] & 0x01);
		ring1 = data[2];
	} else {
		if (features->type == WACOM_21UX2 || features->type == WACOM_22HD) {
			buttons = (data[8] << 10) | ((data[7] & 0x01) << 9) |
			          (data[6] << 1) | (data[5] & 0x01);

			if (features->type == WACOM_22HD) {
				nkeys = 3;
				keys = data[9] & 0x07;

				info = !!(data[9] & 0x01);
				wrench = !!(data[9] & 0x02);
			}
		} else {
			buttons = ((data[6] & 0x10) << 5)  |
			          ((data[5] & 0x10) << 4)  |
			          ((data[6] & 0x0F) << 4)  |
			          (data[5] & 0x0F);
		}
		strip1 = ((data[1] & 0x1f) << 8) | data[2];
		strip2 = ((data[3] & 0x1f) << 8) | data[4];
	}

	prox = (buttons & ~(~0U << nbuttons)) | (keys & ~(~0U << nkeys)) |
	       (ring1 & 0x80) | (ring2 & 0x80) | strip1 | strip2;

	wacom_report_numbered_buttons(input, nbuttons, buttons);

	for (i = 0; i < nkeys; i++)
		input_report_key(input, KEY_PROG1 + i, keys & (1 << i));

	input_report_key(input, KEY_BUTTONCONFIG, wrench);
	input_report_key(input, KEY_ONSCREEN_KEYBOARD, keyboard);
	input_report_key(input, KEY_CONTROLPANEL, menu);
	input_report_key(input, KEY_INFO, info);

	if (wacom->shared && wacom->shared->touch_input) {
		input_report_switch(wacom->shared->touch_input,
				    SW_MUTE_DEVICE,
				    !wacom->shared->is_touch_on);
		input_sync(wacom->shared->touch_input);
	}

	input_report_abs(input, ABS_RX, strip1);
	input_report_abs(input, ABS_RY, strip2);

	input_report_abs(input, ABS_WHEEL,    (ring1 & 0x80) ? (ring1 & 0x7f) : 0);
	input_report_abs(input, ABS_THROTTLE, (ring2 & 0x80) ? (ring2 & 0x7f) : 0);

	input_report_key(input, wacom->tool[1], prox ? 1 : 0);
	input_report_abs(input, ABS_MISC, prox ? PAD_DEVICE_ID : 0);

	input_event(input, EV_MSC, MSC_SERIAL, 0xffffffff);

	return 1;
}

static int wacom_intuos_id_mangle(int tool_id)
{
	return (tool_id & ~0xFFF) << 4 | (tool_id & 0xFFF);
}

static int wacom_intuos_get_tool_type(int tool_id)
{
	int tool_type;

	switch (tool_id) {
	case 0x812: /* Inking pen */
	case 0x801: /* Intuos3 Inking pen */
	case 0x12802: /* Intuos4/5 Inking Pen */
	case 0x012:
		tool_type = BTN_TOOL_PENCIL;
		break;

	case 0x822: /* Pen */
	case 0x842:
	case 0x852:
	case 0x823: /* Intuos3 Grip Pen */
	case 0x813: /* Intuos3 Classic Pen */
	case 0x885: /* Intuos3 Marker Pen */
	case 0x802: /* Intuos4/5 13HD/24HD General Pen */
	case 0x804: /* Intuos4/5 13HD/24HD Marker Pen */
	case 0x8e2: /* IntuosHT2 pen */
	case 0x022:
	case 0x10804: /* Intuos4/5 13HD/24HD Art Pen */
	case 0x10842: /* MobileStudio Pro Pro Pen slim */
	case 0x14802: /* Intuos4/5 13HD/24HD Classic Pen */
	case 0x16802: /* Cintiq 13HD Pro Pen */
	case 0x18802: /* DTH2242 Pen */
	case 0x10802: /* Intuos4/5 13HD/24HD General Pen */
		tool_type = BTN_TOOL_PEN;
		break;

	case 0x832: /* Stroke pen */
	case 0x032:
		tool_type = BTN_TOOL_BRUSH;
		break;

	case 0x007: /* Mouse 4D and 2D */
	case 0x09c:
	case 0x094:
	case 0x017: /* Intuos3 2D Mouse */
	case 0x806: /* Intuos4 Mouse */
		tool_type = BTN_TOOL_MOUSE;
		break;

	case 0x096: /* Lens cursor */
	case 0x097: /* Intuos3 Lens cursor */
	case 0x006: /* Intuos4 Lens cursor */
		tool_type = BTN_TOOL_LENS;
		break;

	case 0x82a: /* Eraser */
	case 0x84a:
	case 0x85a:
	case 0x91a:
	case 0xd1a:
	case 0x0fa:
	case 0x82b: /* Intuos3 Grip Pen Eraser */
	case 0x81b: /* Intuos3 Classic Pen Eraser */
	case 0x91b: /* Intuos3 Airbrush Eraser */
	case 0x80c: /* Intuos4/5 13HD/24HD Marker Pen Eraser */
	case 0x80a: /* Intuos4/5 13HD/24HD General Pen Eraser */
	case 0x90a: /* Intuos4/5 13HD/24HD Airbrush Eraser */
	case 0x1480a: /* Intuos4/5 13HD/24HD Classic Pen Eraser */
	case 0x1090a: /* Intuos4/5 13HD/24HD Airbrush Eraser */
	case 0x1080c: /* Intuos4/5 13HD/24HD Art Pen Eraser */
	case 0x1084a: /* MobileStudio Pro Pro Pen slim Eraser */
	case 0x1680a: /* Cintiq 13HD Pro Pen Eraser */
	case 0x1880a: /* DTH2242 Eraser */
	case 0x1080a: /* Intuos4/5 13HD/24HD General Pen Eraser */
		tool_type = BTN_TOOL_RUBBER;
		break;

	case 0xd12:
	case 0x912:
	case 0x112:
	case 0x913: /* Intuos3 Airbrush */
	case 0x902: /* Intuos4/5 13HD/24HD Airbrush */
	case 0x10902: /* Intuos4/5 13HD/24HD Airbrush */
		tool_type = BTN_TOOL_AIRBRUSH;
		break;

	default: /* Unknown tool */
		tool_type = BTN_TOOL_PEN;
		break;
	}
	return tool_type;
}

static int wacom_intuos_inout(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int idx = (features->type == INTUOS) ? (data[1] & 0x01) : 0;

	if (!(((data[1] & 0xfc) == 0xc0) ||  /* in prox */
	    ((data[1] & 0xfe) == 0x20) ||    /* in range */
	    ((data[1] & 0xfe) == 0x80)))     /* out prox */
		return 0;

	/* Enter report */
	if ((data[1] & 0xfc) == 0xc0) {
		/* serial number of the tool */
		wacom->serial[idx] = ((data[3] & 0x0f) << 28) +
			(data[4] << 20) + (data[5] << 12) +
			(data[6] << 4) + (data[7] >> 4);

		wacom->id[idx] = (data[2] << 4) | (data[3] >> 4) |
		     ((data[7] & 0x0f) << 16) | ((data[8] & 0xf0) << 8);

		wacom->tool[idx] = wacom_intuos_get_tool_type(wacom->id[idx]);

		wacom->shared->stylus_in_proximity = true;
		return 1;
	}

	/* in Range */
	if ((data[1] & 0xfe) == 0x20) {
		if (features->type != INTUOSHT2)
			wacom->shared->stylus_in_proximity = true;

		/* in Range while exiting */
		if (wacom->reporting_data) {
			input_report_key(input, BTN_TOUCH, 0);
			input_report_abs(input, ABS_PRESSURE, 0);
			input_report_abs(input, ABS_DISTANCE, wacom->features.distance_max);
			return 2;
		}
		return 1;
	}

	/* Exit report */
	if ((data[1] & 0xfe) == 0x80) {
		wacom->shared->stylus_in_proximity = false;
		wacom->reporting_data = false;

		/* don't report exit if we don't know the ID */
		if (!wacom->id[idx])
			return 1;

		/*
		 * Reset all states otherwise we lose the initial states
		 * when in-prox next time
		 */
		input_report_abs(input, ABS_X, 0);
		input_report_abs(input, ABS_Y, 0);
		input_report_abs(input, ABS_DISTANCE, 0);
		input_report_abs(input, ABS_TILT_X, 0);
		input_report_abs(input, ABS_TILT_Y, 0);
		if (wacom->tool[idx] >= BTN_TOOL_MOUSE) {
			input_report_key(input, BTN_LEFT, 0);
			input_report_key(input, BTN_MIDDLE, 0);
			input_report_key(input, BTN_RIGHT, 0);
			input_report_key(input, BTN_SIDE, 0);
			input_report_key(input, BTN_EXTRA, 0);
			input_report_abs(input, ABS_THROTTLE, 0);
			input_report_abs(input, ABS_RZ, 0);
		} else {
			input_report_abs(input, ABS_PRESSURE, 0);
			input_report_key(input, BTN_STYLUS, 0);
			input_report_key(input, BTN_STYLUS2, 0);
			input_report_key(input, BTN_TOUCH, 0);
			input_report_abs(input, ABS_WHEEL, 0);
			if (features->type >= INTUOS3S)
				input_report_abs(input, ABS_Z, 0);
		}
		input_report_key(input, wacom->tool[idx], 0);
		input_report_abs(input, ABS_MISC, 0); /* reset tool id */
		input_event(input, EV_MSC, MSC_SERIAL, wacom->serial[idx]);
		wacom->id[idx] = 0;
		return 2;
	}

	/* don't report other events if we don't know the ID */
	if (!wacom->id[idx])
		return 1;

	return 0;
}

static inline bool report_touch_events(struct wacom_wac *wacom)
{
	return (touch_arbitration ? !wacom->shared->stylus_in_proximity : 1);
}

static inline bool delay_pen_events(struct wacom_wac *wacom)
{
	return (wacom->shared->touch_down && touch_arbitration);
}

static int wacom_intuos_general(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int idx = (features->type == INTUOS) ? (data[1] & 0x01) : 0;
	unsigned char type = (data[1] >> 1) & 0x0F;
	unsigned int x, y, distance, t;

	if (data[0] != WACOM_REPORT_PENABLED && data[0] != WACOM_REPORT_CINTIQ &&
		data[0] != WACOM_REPORT_INTUOS_PEN)
		return 0;

	if (delay_pen_events(wacom))
		return 1;

	/*
	 * don't report events for invalid data
	 */
	/* older I4 styli don't work with new Cintiqs */
	if ((!((wacom->id[idx] >> 16) & 0x01) &&
			(features->type == WACOM_21UX2)) ||
	    /* Only large Intuos support Lense Cursor */
	    (wacom->tool[idx] == BTN_TOOL_LENS &&
		(features->type == INTUOS3 ||
		 features->type == INTUOS3S ||
		 features->type == INTUOS4 ||
		 features->type == INTUOS4S ||
		 features->type == INTUOS5 ||
		 features->type == INTUOS5S ||
		 features->type == INTUOSPM ||
		 features->type == INTUOSPS)) ||
	   /* Cintiq doesn't send data when RDY bit isn't set */
	   (features->type == CINTIQ && !(data[1] & 0x40)))
		return 1;

	x = (be16_to_cpup((__be16 *)&data[2]) << 1) | ((data[9] >> 1) & 1);
	y = (be16_to_cpup((__be16 *)&data[4]) << 1) | (data[9] & 1);
	distance = data[9] >> 2;
	if (features->type < INTUOS3S) {
		x >>= 1;
		y >>= 1;
		distance >>= 1;
	}
	if (features->type == INTUOSHT2)
		distance = features->distance_max - distance;
	input_report_abs(input, ABS_X, x);
	input_report_abs(input, ABS_Y, y);
	input_report_abs(input, ABS_DISTANCE, distance);

	switch (type) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
		/* general pen packet */
		t = (data[6] << 3) | ((data[7] & 0xC0) >> 5) | (data[1] & 1);
		if (features->pressure_max < 2047)
			t >>= 1;
		input_report_abs(input, ABS_PRESSURE, t);
		if (features->type != INTUOSHT2) {
		    input_report_abs(input, ABS_TILT_X,
				((data[7] << 1) & 0x7e) | (data[8] >> 7));
		    input_report_abs(input, ABS_TILT_Y, data[8] & 0x7f);
		}
		input_report_key(input, BTN_STYLUS, data[1] & 2);
		input_report_key(input, BTN_STYLUS2, data[1] & 4);
		input_report_key(input, BTN_TOUCH, t > 10);
		break;

	case 0x0a:
		/* airbrush second packet */
		input_report_abs(input, ABS_WHEEL,
				(data[6] << 2) | ((data[7] >> 6) & 3));
		input_report_abs(input, ABS_TILT_X,
				((data[7] << 1) & 0x7e) | (data[8] >> 7));
		input_report_abs(input, ABS_TILT_Y, data[8] & 0x7f);
		break;

	case 0x05:
		/* Rotation packet */
		if (features->type >= INTUOS3S) {
			/* I3 marker pen rotation */
			t = (data[6] << 3) | ((data[7] >> 5) & 7);
			t = (data[7] & 0x20) ? ((t > 900) ? ((t-1) / 2 - 1350) :
				((t-1) / 2 + 450)) : (450 - t / 2) ;
			input_report_abs(input, ABS_Z, t);
		} else {
			/* 4D mouse 2nd packet */
			t = (data[6] << 3) | ((data[7] >> 5) & 7);
			input_report_abs(input, ABS_RZ, (data[7] & 0x20) ?
				((t - 1) / 2) : -t / 2);
		}
		break;

	case 0x04:
		/* 4D mouse 1st packet */
		input_report_key(input, BTN_LEFT,   data[8] & 0x01);
		input_report_key(input, BTN_MIDDLE, data[8] & 0x02);
		input_report_key(input, BTN_RIGHT,  data[8] & 0x04);

		input_report_key(input, BTN_SIDE,   data[8] & 0x20);
		input_report_key(input, BTN_EXTRA,  data[8] & 0x10);
		t = (data[6] << 2) | ((data[7] >> 6) & 3);
		input_report_abs(input, ABS_THROTTLE, (data[8] & 0x08) ? -t : t);
		break;

	case 0x06:
		/* I4 mouse */
		input_report_key(input, BTN_LEFT,   data[6] & 0x01);
		input_report_key(input, BTN_MIDDLE, data[6] & 0x02);
		input_report_key(input, BTN_RIGHT,  data[6] & 0x04);
		input_report_rel(input, REL_WHEEL, ((data[7] & 0x80) >> 7)
				 - ((data[7] & 0x40) >> 6));
		input_report_key(input, BTN_SIDE,   data[6] & 0x08);
		input_report_key(input, BTN_EXTRA,  data[6] & 0x10);

		input_report_abs(input, ABS_TILT_X,
			((data[7] << 1) & 0x7e) | (data[8] >> 7));
		input_report_abs(input, ABS_TILT_Y, data[8] & 0x7f);
		break;

	case 0x08:
		if (wacom->tool[idx] == BTN_TOOL_MOUSE) {
			/* 2D mouse packet */
			input_report_key(input, BTN_LEFT,   data[8] & 0x04);
			input_report_key(input, BTN_MIDDLE, data[8] & 0x08);
			input_report_key(input, BTN_RIGHT,  data[8] & 0x10);
			input_report_rel(input, REL_WHEEL, (data[8] & 0x01)
					 - ((data[8] & 0x02) >> 1));

			/* I3 2D mouse side buttons */
			if (features->type >= INTUOS3S && features->type <= INTUOS3L) {
				input_report_key(input, BTN_SIDE,   data[8] & 0x40);
				input_report_key(input, BTN_EXTRA,  data[8] & 0x20);
			}
		}
		else if (wacom->tool[idx] == BTN_TOOL_LENS) {
			/* Lens cursor packets */
			input_report_key(input, BTN_LEFT,   data[8] & 0x01);
			input_report_key(input, BTN_MIDDLE, data[8] & 0x02);
			input_report_key(input, BTN_RIGHT,  data[8] & 0x04);
			input_report_key(input, BTN_SIDE,   data[8] & 0x10);
			input_report_key(input, BTN_EXTRA,  data[8] & 0x08);
		}
		break;

	case 0x07:
	case 0x09:
	case 0x0b:
	case 0x0c:
	case 0x0d:
	case 0x0e:
	case 0x0f:
		/* unhandled */
		break;
	}

	input_report_abs(input, ABS_MISC,
			 wacom_intuos_id_mangle(wacom->id[idx])); /* report tool id */
	input_report_key(input, wacom->tool[idx], 1);
	input_event(input, EV_MSC, MSC_SERIAL, wacom->serial[idx]);
	wacom->reporting_data = true;
	return 2;
}

static int wacom_intuos_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int result;

	if (data[0] != WACOM_REPORT_PENABLED &&
	    data[0] != WACOM_REPORT_INTUOS_ID1 &&
	    data[0] != WACOM_REPORT_INTUOS_ID2 &&
	    data[0] != WACOM_REPORT_INTUOSPAD &&
	    data[0] != WACOM_REPORT_INTUOS_PEN &&
	    data[0] != WACOM_REPORT_CINTIQ &&
	    data[0] != WACOM_REPORT_CINTIQPAD &&
	    data[0] != WACOM_REPORT_INTUOS5PAD) {
		dev_dbg(input->dev.parent,
			"%s: received unknown report #%d\n", __func__, data[0]);
                return 0;
	}

	/* process pad events */
	result = wacom_intuos_pad(wacom);
	if (result)
		return result;

	/* process in/out prox events */
	result = wacom_intuos_inout(wacom);
	if (result)
		return result - 1;

	/* process general packets */
	result = wacom_intuos_general(wacom);
	if (result)
		return result - 1;

	return 0;
}

static int wacom_remote_irq(struct wacom_wac *wacom_wac, size_t len)
{
	unsigned char *data = wacom_wac->data;
	struct wacom *wacom = container_of(wacom_wac, struct wacom, wacom_wac);
	struct wacom_remote *remote = wacom->remote;
	struct input_dev *input;
	int bat_charging, bat_percent, touch_ring_mode;
	__u32 serial;
	int i, index = -1;
	unsigned long flags;

	if (data[0] != WACOM_REPORT_REMOTE) {
		dev_dbg(&wacom->intf->dev,
			"%s: received unknown report #%d", __func__, data[0]);
		return 0;
	}

	serial = data[3] + (data[4] << 8) + (data[5] << 16);
	wacom_wac->id[0] = PAD_DEVICE_ID;

	spin_lock_irqsave(&remote->remote_lock, flags);

	for (i = 0; i < WACOM_MAX_REMOTES; i++) {
		if (remote->remotes[i].serial == serial) {
			index = i;
			break;
		}
	}

	input = remote->remotes[index].input;

	if (index < 0 || !remote->remotes[index].registered)
		goto out;

	input_report_key(input, BTN_0, (data[9] & 0x01));
	input_report_key(input, BTN_1, (data[9] & 0x02));
	input_report_key(input, BTN_2, (data[9] & 0x04));
	input_report_key(input, BTN_3, (data[9] & 0x08));
	input_report_key(input, BTN_4, (data[9] & 0x10));
	input_report_key(input, BTN_5, (data[9] & 0x20));
	input_report_key(input, BTN_6, (data[9] & 0x40));
	input_report_key(input, BTN_7, (data[9] & 0x80));

	input_report_key(input, BTN_8, (data[10] & 0x01));
	input_report_key(input, BTN_9, (data[10] & 0x02));
	input_report_key(input, BTN_A, (data[10] & 0x04));
	input_report_key(input, BTN_B, (data[10] & 0x08));
	input_report_key(input, BTN_C, (data[10] & 0x10));
	input_report_key(input, BTN_X, (data[10] & 0x20));
	input_report_key(input, BTN_Y, (data[10] & 0x40));
	input_report_key(input, BTN_Z, (data[10] & 0x80));

	input_report_key(input, BTN_BASE, (data[11] & 0x01));
	input_report_key(input, BTN_BASE2, (data[11] & 0x02));

	if (data[12] & 0x80)
		input_report_abs(input, ABS_WHEEL, (data[12] & 0x7f) - 1);
	else
		input_report_abs(input, ABS_WHEEL, 0);

	bat_percent = data[7] & 0x7f;
	bat_charging = !!(data[7] & 0x80);

	if (data[9] | data[10] | (data[11] & 0x03) | data[12])
		input_report_abs(input, ABS_MISC, PAD_DEVICE_ID);
	else
		input_report_abs(input, ABS_MISC, 0);

	input_event(input, EV_MSC, MSC_SERIAL, serial);

	input_sync(input);

	/*Which mode select (LED light) is currently on?*/
	touch_ring_mode = (data[11] & 0xC0) >> 6;

	for (i = 0; i < WACOM_MAX_REMOTES; i++) {
		if (remote->remotes[i].serial == serial)
			wacom->led.select[i] = touch_ring_mode;
	}

	__wacom_notify_battery(&remote->remotes[index].battery,
				WACOM_POWER_SUPPLY_STATUS_AUTO, bat_percent,
				bat_charging, 1, bat_charging);

out:
	spin_unlock_irqrestore(&remote->remote_lock, flags);
	return 0;
}

static void wacom_remote_status_irq(struct wacom_wac *wacom_wac, size_t len)
{
	struct wacom *wacom = container_of(wacom_wac, struct wacom, wacom_wac);
	struct device *dev = &wacom->intf->dev;
	unsigned char *data = wacom_wac->data;
	struct wacom_remote *remote = wacom->remote;
	struct wacom_remote_data remote_data;
	unsigned long flags;
	int i, ret;

	if (data[0] != WACOM_REPORT_DEVICE_LIST)
		return;

	memset(&remote_data, 0, sizeof(struct wacom_remote_data));

	for (i = 0; i < WACOM_MAX_REMOTES; i++) {
		int j = i * 6;
		int serial = (data[j+6] << 16) + (data[j+5] << 8) + data[j+4];
		bool connected = data[j+2];

		remote_data.remote[i].serial = serial;
		remote_data.remote[i].connected = connected;
	}

	spin_lock_irqsave(&remote->remote_lock, flags);

	ret = kfifo_in(&remote->remote_fifo, &remote_data, sizeof(remote_data));
	if (ret != sizeof(remote_data)) {
		spin_unlock_irqrestore(&remote->remote_lock, flags);
		dev_err(dev, "Can't queue Remote status event.\n");
		return;
	}

	spin_unlock_irqrestore(&remote->remote_lock, flags);

	wacom_schedule_work(wacom_wac, WACOM_WORKER_REMOTE);
}

static int int_dist(int x1, int y1, int x2, int y2)
{
	int x = x2 - x1;
	int y = y2 - y1;

	return int_sqrt(x*x + y*y);
}

static int wacom_wac_finger_count_touches(struct wacom_wac *wacom)
{
	struct input_dev *input = wacom->input;
	unsigned touch_max = wacom->features.touch_max;
	int count = 0;
	int i;

	if (!touch_max)
		return 0;

	if (touch_max == 1)
		return test_bit(BTN_TOUCH, input->key) &&
			report_touch_events(wacom);

	for (i = 0; i < input->mt->num_slots; i++) {
		struct input_mt_slot *ps = &input->mt->slots[i];
		int id = input_mt_get_value(ps, ABS_MT_TRACKING_ID);
		if (id >= 0)
			count++;
	}

	return count;
}

static void wacom_multitouch_generic_finger(struct wacom_wac *wacom,
					    int contact_id, bool prox,
					    int x, int y, int w, int h,
					    int c_x, int c_y)
{
	struct input_dev *input = wacom->input;
	int slot = input_mt_get_slot_by_key(input, contact_id);

	if (wacom->shared->has_mute_touch_switch &&
	    !wacom->shared->is_touch_on) {
		if (!wacom->shared->touch_down)
			return;
		prox = 0;
	}

	if (slot < 0)
		return;

	prox = prox && report_touch_events(wacom);

	input_mt_slot(input, slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, prox);

	if (prox) {
		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);

		if (w >= 0 && h >= 0) {
			input_report_abs(input, ABS_MT_TOUCH_MAJOR, max(w, h));
			input_report_abs(input, ABS_MT_TOUCH_MINOR, min(w, h));
			input_report_abs(input, ABS_MT_ORIENTATION, w > h);

			if (c_x >= 0 && c_y >= 0) {
				input_report_abs(input, ABS_MT_WIDTH_MAJOR,
						 min(w, h) + int_dist(x, y, c_x, c_y));
				input_report_abs(input, ABS_MT_WIDTH_MINOR, min(w, h));
			}
		}
	}
}

static int wacom_multitouch_generic(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int current_num_contacts, contacts_to_send, contacts_per_packet;
	int bytes_per_packet, bytes_header;
	int i;

	switch (features->type) {
	case WACOM_24HDT:
		current_num_contacts = data[61];
		contacts_per_packet = 4;
		bytes_per_packet = WACOM_BYTES_PER_24HDT_PACKET;
		bytes_header = 1;
		break;
	case WACOM_27QHDT:
		if (wacom->shared->has_mute_touch_switch &&
		    !wacom->shared->is_touch_on) {
			if (!wacom->shared->touch_down)
				return 0;
		}

		current_num_contacts = data[63];
		contacts_per_packet = 10;
		bytes_per_packet = WACOM_BYTES_PER_QHDTHID_PACKET;
		bytes_header = 1;
		break;
	case WACOM_MSPROT:
	case DTH1152T:
	case DTH2452T:
		current_num_contacts = data[2];
		contacts_per_packet = 5;
		bytes_per_packet = WACOM_BYTES_PER_MSPROT_PACKET;
		bytes_header = 3;
		break;
	case INTUOSP2:
	case INTUOSP2S:
		current_num_contacts = data[1];
		contacts_per_packet = 5;
		bytes_per_packet = WACOM_BYTES_PER_INTUOSP2_PACKET;
		bytes_header = 2;
		break;
	default:
		return 0;
	}

	if (current_num_contacts)
		wacom->num_contacts_left = current_num_contacts;

	contacts_to_send = min(contacts_per_packet, wacom->num_contacts_left);

	for (i = 0; i < contacts_to_send; i++) {
		int contact_id = -1;
		int x = -1;
		int y = -1;
		int w = -1;
		int h = -1;
		int c_x = -1;
		int c_y = -1;
		int prox = -1;
		int offset = bytes_per_packet * i + bytes_header;

		switch (features->type) {
		case WACOM_24HDT:
			prox = data[offset] & 0x01;
			contact_id = data[offset + 1];
			x   = get_unaligned_le16(&data[offset + 2]);
			c_x = get_unaligned_le16(&data[offset + 4]);
			y   = get_unaligned_le16(&data[offset + 6]);
			c_y = get_unaligned_le16(&data[offset + 8]);
			w   = get_unaligned_le16(&data[offset + 10]);
			h   = get_unaligned_le16(&data[offset + 12]);
			break;

		case WACOM_27QHDT:
			prox = data[offset] & 0x01;
			contact_id = data[offset + 1];
			x = get_unaligned_le16(&data[offset + 2]);
			y = get_unaligned_le16(&data[offset + 4]);
			break;

		case WACOM_MSPROT:
		case DTH1152T:
		case DTH2452T:
			prox = data[offset] & 0x1;
			contact_id = get_unaligned_le16(&data[offset + 1]);
			x = get_unaligned_le16(&data[offset + 3]);
			y = get_unaligned_le16(&data[offset + 5]);
			w = data[offset + 7] * 133;
			h = data[offset + 8] * 133;
			break;

		case INTUOSP2:
		case INTUOSP2S:
			contact_id = data[offset];
			prox = data[offset + 1] & 0x01;
			x = get_unaligned_le16(&data[offset + 2]);
			y = get_unaligned_le16(&data[offset + 4]);
			w = data[offset + 6];
			h = data[offset + 7];
			break;

		default:
			continue;
		}

		wacom_multitouch_generic_finger(wacom, contact_id, prox, x, y, w, h, c_x, c_y);
	}

	wacom->num_contacts_left -= contacts_to_send;
	if (wacom->num_contacts_left <= 0) {
		wacom->num_contacts_left = 0;
		wacom->shared->touch_down = wacom_wac_finger_count_touches(wacom);
	}

	input_mt_sync_frame(input);
	return 1;
}

static int wacom_mt_touch(struct wacom_wac *wacom)
{
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int i;
	int current_num_contacts = data[2];
	int contacts_to_send = 0;
	int x_offset = 0;

	/* MTTPC does not support Height and Width */
	if (wacom->features.type == MTTPC ||
	    wacom->features.type == MTTPC_B ||
	    wacom->features.type == MTTPC_C)
		x_offset = -4;

	/*
	 * First packet resets the counter since only the first
	 * packet in series will have non-zero current_num_contacts.
	 */
	if (current_num_contacts)
		wacom->num_contacts_left = current_num_contacts;

	/* There are at most 5 contacts per packet */
	contacts_to_send = min(5, wacom->num_contacts_left);

	for (i = 0; i < contacts_to_send; i++) {
		int offset = (WACOM_BYTES_PER_MT_PACKET + x_offset) * i + 3;
		bool touch = (data[offset] & 0x1) && report_touch_events(wacom);
		int id = get_unaligned_le16(&data[offset + 1]);
		int slot = input_mt_get_slot_by_key(input, id);

		if (slot < 0)
			continue;

		input_mt_slot(input, slot);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, touch);
		if (touch) {
			int x = get_unaligned_le16(&data[offset + x_offset + 7]);
			int y = get_unaligned_le16(&data[offset + x_offset + 9]);
			input_report_abs(input, ABS_MT_POSITION_X, x);
			input_report_abs(input, ABS_MT_POSITION_Y, y);
		}
	}
	input_mt_report_pointer_emulation(input, true);

	wacom->num_contacts_left -= contacts_to_send;
	if (wacom->num_contacts_left <= 0) {
		wacom->num_contacts_left = 0;
		wacom->shared->touch_down = wacom_wac_finger_count_touches(wacom);
	}
	return 1;
}

static int wacom_tpc_mt_touch(struct wacom_wac *wacom)
{
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int i;

	for (i = 0; i < 2; i++) {
		int p = data[1] & (1 << i);
		bool touch = p && report_touch_events(wacom);

		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, touch);
		if (touch) {
			int x = le16_to_cpup((__le16 *)&data[i * 2 + 2]) & 0x7fff;
			int y = le16_to_cpup((__le16 *)&data[i * 2 + 6]) & 0x7fff;

			input_report_abs(input, ABS_MT_POSITION_X, x);
			input_report_abs(input, ABS_MT_POSITION_Y, y);
		}
	}
	input_mt_report_pointer_emulation(input, true);

	/* keep touch state for pen event */
	wacom->shared->touch_down = wacom_wac_finger_count_touches(wacom);

	return 1;
}

static int wacom_tpc_single_touch(struct wacom_wac *wacom, size_t len)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	bool prox = report_touch_events(wacom);
	int x = 0, y = 0;

	if (wacom->features.touch_max > 1 || len > WACOM_PKGLEN_TPC2FG)
		return 0;

	if (len == WACOM_PKGLEN_TPC1FG) {
		prox = prox && (data[0] & 0x01);
		x = get_unaligned_le16(&data[1]);
		y = get_unaligned_le16(&data[3]);
	} else if (len == WACOM_PKGLEN_TPC1FG_B) {
		prox = prox && (data[2] & 0x01);
		x = get_unaligned_le16(&data[3]);
		y = get_unaligned_le16(&data[5]);
	} else {
		prox = prox && (data[1] & 0x01);
		x = le16_to_cpup((__le16 *)&data[2]);
		y = le16_to_cpup((__le16 *)&data[4]);
	}

	if (prox) {
		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);
	}
	input_report_key(input, BTN_TOUCH, prox);

	/* keep touch state for pen events */
	wacom->shared->touch_down = prox;

	return 1;
}

static int wacom_tpc_pen(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	bool prox = data[1] & 0x20;

	if (!wacom->shared->stylus_in_proximity) /* first in prox */
		/* Going into proximity select tool */
		wacom->tool[0] = (data[1] & 0x0c) ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;

	/* keep pen state for touch events */
	wacom->shared->stylus_in_proximity = prox;

	/* send pen events only when touch is up or forced out
	 * or touch arbitration is off
	 */
	if (!delay_pen_events(wacom)) {
		input_report_key(input, BTN_STYLUS, data[1] & 0x02);
		input_report_key(input, BTN_STYLUS2, data[1] & 0x10);
		input_report_abs(input, ABS_X, le16_to_cpup((__le16 *)&data[2]));
		input_report_abs(input, ABS_Y, le16_to_cpup((__le16 *)&data[4]));
		input_report_abs(input, ABS_PRESSURE, ((data[7] & 0x07) << 8) | data[6]);
		input_report_key(input, BTN_TOUCH, data[1] & 0x05);
		input_report_key(input, wacom->tool[0], prox);
		return 1;
	}

	return 0;
}

static int wacom_tpc_irq(struct wacom_wac *wacom, size_t len)
{
	unsigned char *data = wacom->data;

	dev_dbg(wacom->input->dev.parent,
		"%s: received report #%d\n", __func__, data[0]);

	switch (len) {
	case WACOM_PKGLEN_TPC1FG:
		return wacom_tpc_single_touch(wacom, len);

	case WACOM_PKGLEN_TPC2FG:
		return wacom_tpc_mt_touch(wacom);

	case WACOM_PKGLEN_PENABLED:
		return wacom_tpc_pen(wacom);

	default:
		switch (data[0]) {
		case WACOM_REPORT_TPC1FG:
		case WACOM_REPORT_TPCHID:
		case WACOM_REPORT_TPCST:
		case WACOM_REPORT_TPC1FGE:
			return wacom_tpc_single_touch(wacom, len);

		case WACOM_REPORT_TPCMT:
		case WACOM_REPORT_TPCMT2:
			return wacom_mt_touch(wacom);

		case WACOM_REPORT_PENABLED:
			return wacom_tpc_pen(wacom);
		}
	}

	return 0;
}

static int wacom_bpt_touch(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int i;

	if (data[0] != 0x02)
	    return 0;

	for (i = 0; i < 2; i++) {
		int offset = (data[1] & 0x80) ? (8 * i) : (9 * i);
		bool touch = report_touch_events(wacom)
			   && (data[offset + 3] & 0x80);

		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, touch);
		if (touch) {
			int x = get_unaligned_be16(&data[offset + 3]) & 0x7ff;
			int y = get_unaligned_be16(&data[offset + 5]) & 0x7ff;
			if (features->quirks & WACOM_QUIRK_BBTOUCH_LOWRES) {
				x <<= 5;
				y <<= 5;
			}
			input_report_abs(input, ABS_MT_POSITION_X, x);
			input_report_abs(input, ABS_MT_POSITION_Y, y);
		}
	}

	input_mt_report_pointer_emulation(input, true);

	input_report_key(input, BTN_LEFT, (data[1] & 0x08) != 0);
	input_report_key(input, BTN_FORWARD, (data[1] & 0x04) != 0);
	input_report_key(input, BTN_BACK, (data[1] & 0x02) != 0);
	input_report_key(input, BTN_RIGHT, (data[1] & 0x01) != 0);
	wacom->shared->touch_down = wacom_wac_finger_count_touches(wacom);

	return 1;
}

static void wacom_bpt3_touch_msg(struct wacom_wac *wacom, unsigned char *data)
{
	struct wacom_features *features = &wacom->features;
	struct input_dev *input = wacom->input;
	bool touch = data[1] & 0x80;
	int slot = input_mt_get_slot_by_key(input, data[0]);

	if (slot < 0)
		return;

	touch = touch && report_touch_events(wacom);

	input_mt_slot(input, slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, touch);

	if (touch) {
		int x = (data[2] << 4) | (data[4] >> 4);
		int y = (data[3] << 4) | (data[4] & 0x0f);
		int width, height;

		if (features->type >= INTUOSPS && features->type <= INTUOSHT2) {
			width  = data[5] * 100;
			height = data[6] * 100;
		} else {
			/*
			 * "a" is a scaled-down area which we assume is
			 * roughly circular and which can be described as:
			 * a=(pi*r^2)/C.
			 */
			int a = data[5];
			int x_res = input_abs_get_res(input, ABS_MT_POSITION_X);
			int y_res = input_abs_get_res(input, ABS_MT_POSITION_Y);
			width = 2 * int_sqrt(a * WACOM_CONTACT_AREA_SCALE);
			height = width * y_res / x_res;
		}

		input_report_abs(input, ABS_MT_POSITION_X, x);
		input_report_abs(input, ABS_MT_POSITION_Y, y);
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, width);
		input_report_abs(input, ABS_MT_TOUCH_MINOR, height);
	}
}

static void wacom_bpt3_button_msg(struct wacom_wac *wacom, unsigned char *data)
{
	struct input_dev *input = wacom->input;
	struct wacom_features *features = &wacom->features;

	if (features->type == INTUOSHT || features->type == INTUOSHT2) {
		input_report_key(input, BTN_LEFT, (data[1] & 0x02) != 0);
		input_report_key(input, BTN_BACK, (data[1] & 0x08) != 0);
	} else {
		input_report_key(input, BTN_BACK, (data[1] & 0x02) != 0);
		input_report_key(input, BTN_LEFT, (data[1] & 0x08) != 0);
	}
	input_report_key(input, BTN_FORWARD, (data[1] & 0x04) != 0);
	input_report_key(input, BTN_RIGHT, (data[1] & 0x01) != 0);
}

static int wacom_bpt3_touch(struct wacom_wac *wacom)
{
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int count = data[1] & 0x07;
	int touch_changed = 0, i;

	if (data[0] != 0x02)
	    return 0;

	/* data has up to 7 fixed sized 8-byte messages starting at data[2] */
	for (i = 0; i < count; i++) {
		int offset = (8 * i) + 2;
		int msg_id = data[offset];

		if (msg_id >= 2 && msg_id <= 17) {
			wacom_bpt3_touch_msg(wacom, data + offset);
			touch_changed++;
		} else if (msg_id == 128)
			wacom_bpt3_button_msg(wacom, data + offset);

	}

	if (touch_changed) {
		input_mt_report_pointer_emulation(input, true);
		wacom->shared->touch_down = wacom_wac_finger_count_touches(wacom);
	}
	return 1;
}

static int wacom_bpt_pen(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	struct input_dev *input = wacom->input;
	unsigned char *data = wacom->data;
	int x = 0, y = 0, p = 0, d = 0;
	bool pen = false, btn1 = false, btn2 = false;
	bool range, prox, rdy;

	if (data[0] != WACOM_REPORT_PENABLED)
	    return 0;

	range = (data[1] & 0x80) == 0x80;
	prox = (data[1] & 0x40) == 0x40;
	rdy = (data[1] & 0x20) == 0x20;

	wacom->shared->stylus_in_proximity = range;
	if (delay_pen_events(wacom))
		return 0;

	if (rdy) {
		p = le16_to_cpup((__le16 *)&data[6]);
		pen = data[1] & 0x01;
		btn1 = data[1] & 0x02;
		btn2 = data[1] & 0x04;
	}
	if (prox) {
		x = le16_to_cpup((__le16 *)&data[2]);
		y = le16_to_cpup((__le16 *)&data[4]);

		if (data[1] & 0x08) {
			wacom->tool[0] = BTN_TOOL_RUBBER;
			wacom->id[0] = ERASER_DEVICE_ID;
		} else {
			wacom->tool[0] = BTN_TOOL_PEN;
			wacom->id[0] = STYLUS_DEVICE_ID;
		}
		wacom->reporting_data = true;
	}
	if (range) {
		/*
		 * Convert distance from out prox to distance from tablet.
		 * distance will be greater than distance_max once
		 * touching and applying pressure; do not report negative
		 * distance.
		 */
		if (data[8] <= features->distance_max)
			d = features->distance_max - data[8];
	} else {
		wacom->id[0] = 0;
	}

	if (wacom->reporting_data) {
		input_report_key(input, BTN_TOUCH, pen);
		input_report_key(input, BTN_STYLUS, btn1);
		input_report_key(input, BTN_STYLUS2, btn2);

		if (prox || !range) {
			input_report_abs(input, ABS_X, x);
			input_report_abs(input, ABS_Y, y);
		}
		input_report_abs(input, ABS_PRESSURE, p);
		input_report_abs(input, ABS_DISTANCE, d);

		input_report_key(input, wacom->tool[0], range); /* PEN or RUBBER */
		input_report_abs(input, ABS_MISC, wacom->id[0]); /* TOOL ID */
	}

	if (!range) {
		wacom->reporting_data = false;
	}

	return 1;
}

static int wacom_bpt_irq(struct wacom_wac *wacom, size_t len)
{
	struct wacom_features *features = &wacom->features;

	if ((features->type == INTUOSHT2) &&
	    (features->device_type == BTN_TOOL_PEN))
		return wacom_intuos_irq(wacom);
	else if (len == WACOM_PKGLEN_BBTOUCH)
		return wacom_bpt_touch(wacom);
	else if (len == WACOM_PKGLEN_BBTOUCH3)
		return wacom_bpt3_touch(wacom);
	else if (len == WACOM_PKGLEN_BBFUN || len == WACOM_PKGLEN_BBPEN)
		return wacom_bpt_pen(wacom);

	return 0;
}

static int wacom_wireless_irq(struct wacom_wac *wacom, size_t len)
{
	unsigned char *data = wacom->data;
	int connected;

	if (len != WACOM_PKGLEN_WIRELESS || data[0] != WACOM_REPORT_WL)
		return 0;

	connected = data[1] & 0x01;
	if (connected) {
		int pid, battery, charging;

		if ((wacom->shared->type == INTUOSHT ||
		    wacom->shared->type == INTUOSHT2) &&
		    wacom->shared->touch_input &&
		    wacom->shared->touch_max) {
			wacom->shared->is_touch_on = !(data[5] & 0x40);
			input_report_switch(wacom->shared->touch_input,
					SW_MUTE_DEVICE, !wacom->shared->is_touch_on);
			input_sync(wacom->shared->touch_input);
		}

		pid = get_unaligned_be16(&data[6]);
		battery = (data[5] & 0x3f) * 100 / 31;
		charging = !!(data[5] & 0x80);
		if (wacom->pid != pid) {
			wacom->pid = pid;
			wacom_schedule_work(wacom, WACOM_WORKER_WIRELESS);
		}

		wacom_notify_battery(wacom, WACOM_POWER_SUPPLY_STATUS_AUTO,
				     battery, charging, 1, 0);

	} else if (wacom->pid != 0) {
		/* disconnected while previously connected */
		wacom->pid = 0;
		wacom_schedule_work(wacom, WACOM_WORKER_WIRELESS);
		wacom_notify_battery(wacom, POWER_SUPPLY_STATUS_UNKNOWN, 0, 0, 0, 0);
	}

	return 0;
}

static int wacom_status_irq(struct wacom_wac *wacom_wac, size_t len)
{
	struct wacom *wacom = container_of(wacom_wac, struct wacom, wacom_wac);
	struct wacom_features *features = &wacom_wac->features;
	unsigned char *data = wacom_wac->data;

	if (data[0] != WACOM_REPORT_USB)
		return 0;

	if ((features->type == INTUOSHT ||
	    features->type == INTUOSHT2) &&
	    wacom_wac->shared->touch_input &&
	    features->touch_max) {
		wacom_wac->shared->is_touch_on = !(data[8] & 0x40);
		input_report_switch(wacom_wac->shared->touch_input,
				    SW_MUTE_DEVICE, !wacom_wac->shared->is_touch_on);
		input_sync(wacom_wac->shared->touch_input);
	}

	if (data[9] & 0x02) { /* wireless module is attached */
		int battery = (data[8] & 0x3f) * 100 / 31;
		bool charging = !!(data[8] & 0x80);

		wacom_notify_battery(wacom_wac, WACOM_POWER_SUPPLY_STATUS_AUTO,
				     battery, charging, battery || charging, 1);

		if (!wacom->battery.battery.dev &&
		    !(features->quirks & WACOM_QUIRK_BATTERY)) {
			features->quirks |= WACOM_QUIRK_BATTERY;
			wacom_schedule_work(wacom_wac, WACOM_WORKER_BATTERY);
		}
	}
	else if ((features->quirks & WACOM_QUIRK_BATTERY) &&
		 wacom->battery.battery.dev) {
		features->quirks &= ~WACOM_QUIRK_BATTERY;
		wacom_schedule_work(wacom_wac, WACOM_WORKER_BATTERY);
		wacom_notify_battery(wacom_wac, POWER_SUPPLY_STATUS_UNKNOWN, 0, 0, 0, 0);
	}
	return 0;
}

static void wacom_mspro_touch_switch(struct wacom_wac *wacom, bool enable_touch)
{
	if (!wacom->shared->touch_input)
		return;

	wacom->shared->is_touch_on = enable_touch;
	input_report_switch(wacom->shared->touch_input,
			    SW_MUTE_DEVICE, !enable_touch);
	input_sync(wacom->shared->touch_input);
}

static void wacom_mspro_touch_toggle(struct wacom_wac *wacom)
{
	if (!wacom->shared->touch_input)
		return;

	wacom_mspro_touch_switch(wacom, !wacom->shared->is_touch_on);
}

static int wacom_mspro_device_irq(struct wacom_wac *wacom)
{
	struct wacom *w = container_of(wacom, struct wacom, wacom_wac);
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	bool bat_charging;
	int battery_level;

	battery_level = data[1] & 0x7F;
	bat_charging = data[1] & 0x80;

	if (!w->battery.battery.dev &&
	    !(features->quirks & WACOM_QUIRK_BATTERY)) {
		features->quirks |= WACOM_QUIRK_BATTERY;
		wacom_schedule_work(wacom, WACOM_WORKER_BATTERY);
	}

	wacom_notify_battery(wacom, WACOM_POWER_SUPPLY_STATUS_AUTO,
			     battery_level, bat_charging, 1, bat_charging);

	wacom_mspro_touch_switch(wacom, (data[2] & 0x80));

	return 0;
}

int wacom_mask_with_numbered_buttons(int nbuttons, int buttons)
{
	int mask = 0;
	int i;

	for (i = 0; i < nbuttons; i++)
		mask |= buttons & (1 << i);

	return mask;
}

static int wacom_mspro_pad_irq(struct wacom_wac *wacom)
{
	struct wacom_features *features = &wacom->features;
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	int nbuttons = features->numbered_buttons;
	bool prox;
	int buttons, ring, ringvalue, keys;
	bool active = false;

	switch (nbuttons) {
		case 11:
			buttons = (data[1] >> 1) | (data[3] << 6);
			ring = le16_to_cpup((__le16 *)&data[4]);
			keys = 0;
			break;
		case 13:
			buttons = data[1] | (data[3] << 8);
			ring = le16_to_cpup((__le16 *)&data[4]);
			keys = 0;
			break;
		case 4:
			buttons = data[1];
			keys = 0;
			ring = 0;
			break;
		case 9:
			buttons = (data[1]) | (data[3] << 8);
			ring = le16_to_cpup((__le16 *)&data[4]);
			keys = 0;
			break;
		case 7:
			buttons = (data[1]) | (data[3] << 6);
			ring = le16_to_cpup((__le16 *)&data[4]);
			keys = 0;
			break;
		case 0:
			buttons = 0;
			ring = WACOM_INTUOSP2_RING_UNTOUCHED; /* No ring */
			keys = data[1] & 0x0E; /* 0x01 shouldn't make the pad active */

			if (data[1] & 0x01)
				wacom_mspro_touch_toggle(wacom);

			input_report_key(input, KEY_CONTROLPANEL, (data[1] & 0x02) != 0);
			input_report_key(input, KEY_ONSCREEN_KEYBOARD, (data[1] & 0x04) != 0);
			input_report_key(input, KEY_BUTTONCONFIG, (data[1] & 0x08) != 0);
			break;
		default:
			dev_warn(input->dev.parent, "%s: unsupported device #%d\n", __func__, data[0]);
			return 0;
	}

	/* Fix touchring data: userspace expects 0 at left and increasing clockwise */
	if (input->id.product == 0x357 || input->id.product == 0x358) {
		/* 2nd-gen Intuos Pro */
		ringvalue = 71 - (ring & 0x7F);
		ringvalue += 3*72/16;
		if (ringvalue > 71)
			ringvalue -= 72;
	} else if (input->id.product == 0x34d || input->id.product == 0x34e ||
		 input->id.product == 0x398 || input->id.product == 0x399) {
		/* MobileStudio Pro */
		ringvalue = 35 - (ring & 0x7F);
		ringvalue += 36/2;
		if (ringvalue > 35)
			ringvalue -= 36;
	}
	else {
		/* "Standard" devices */
		ringvalue = 71 - (ring & 0x7F);
		ringvalue += 72/4;
		if (ringvalue > 71)
			ringvalue -= 72;
	}

	/* Mask off buttons greater than nbuttons to avoid having them set prox */
	buttons = wacom_mask_with_numbered_buttons(nbuttons, buttons);

	if (ring != WACOM_INTUOSP2_RING_UNTOUCHED)
		prox = buttons || ring;
	else
		prox = buttons;

	wacom_report_numbered_buttons(input, nbuttons, buttons);
	input_report_abs(input, ABS_WHEEL, (ring & 0x80) ? ringvalue : 0);

	input_report_key(input, wacom->tool[1], prox ? 1 : 0);

	active = (ring ^ wacom->previous_ring) || (buttons ^ wacom->previous_buttons) || (keys ^ wacom->previous_keys);

	input_report_abs(input, ABS_MISC, prox ? PAD_DEVICE_ID : 0);

	wacom->previous_buttons = buttons;
	wacom->previous_ring = ring;
	wacom->previous_keys = keys;

	if (active)
		input_event(input, EV_MSC, MSC_SERIAL, 0xffffffff);
	else
		return 0;

	return 1;
}

static int wacom_mspro_pen_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;
	bool tip, sw1, sw2, range, proximity;
	unsigned int x, y;
	unsigned int pressure;
	int tilt_x, tilt_y;
	int rotation;
	unsigned int fingerwheel;
	unsigned int height;
	u64 tool_uid;
	unsigned int tool_type;

	if (delay_pen_events(wacom))
		return 1;

	tip         = data[1] & 0x01;
	sw1         = data[1] & 0x02;
	sw2         = data[1] & 0x04;
	/* eraser   = data[1] & 0x08; */
	/* invert   = data[1] & 0x10; */
	range       = data[1] & 0x20;
	proximity   = data[1] & 0x40;
	x           = le32_to_cpup((__le32 *)&data[2]) & 0xFFFFFF;
	y           = le32_to_cpup((__le32 *)&data[5]) & 0xFFFFFF;
	pressure    = le16_to_cpup((__le16 *)&data[8]);
	tilt_x      = (char)data[10];
	tilt_y      = (char)data[11];
	rotation    = (int16_t)le16_to_cpup((__le16 *)&data[12]);
	fingerwheel = le16_to_cpup((__le16 *)&data[14]);
	height      = data[16];
	tool_uid    = le64_to_cpup((__le64 *)&data[17]);
	tool_type   = le16_to_cpup((__le16 *)&data[25]);

	if (range) {
		wacom->serial[0] = (tool_uid & 0xFFFFFFFF);
		wacom->id[0]     = ((tool_uid >> 32) & 0xFFFFF) | tool_type;
		wacom->tool[0] = wacom_intuos_get_tool_type(wacom->id[0] & 0xFFFFF);
	}

	/* pointer going from fully "in range" to merely "in proximity" */
	if (!range && wacom->tool[0]) {
		height = wacom->features.distance_max;
	}

	/*
	 * only report data if there's a tool for userspace to associate
	 * the events with.
	 */
	if (wacom->tool[0]) {
		unsigned int sw_state = sw1 | (sw2 << 1);

		/* Fix rotation alignment: userspace expects zero at left */
		rotation += 1800/4;
		if (rotation > 899)
			rotation -= 1800;

		/* Fix tilt zero-point: wacom_setup_cintiq declares 0..127, not -63..+64 */
		tilt_x += 64;
		tilt_y += 64;

		input_report_key(input, BTN_TOUCH,    proximity ? tip           : 0);
		input_report_key(input, BTN_STYLUS,   proximity ? sw_state == 1 : 0);
		input_report_key(input, BTN_STYLUS2,  proximity ? sw_state == 2 : 0);
		input_report_key(input, BTN_STYLUS3,  proximity ? sw_state == 3 : 0);
		input_report_abs(input, ABS_X,        proximity ? x             : 0);
		input_report_abs(input, ABS_Y,        proximity ? y             : 0);
		input_report_abs(input, ABS_PRESSURE, proximity ? pressure      : 0);
		input_report_abs(input, ABS_TILT_X,   proximity ? tilt_x        : 0);
		input_report_abs(input, ABS_TILT_Y,   proximity ? tilt_y        : 0);
		input_report_abs(input, ABS_Z,        proximity ? rotation      : 0);
		input_report_abs(input, ABS_WHEEL,    proximity ? fingerwheel   : 0);
		input_report_abs(input, ABS_DISTANCE, proximity ? height        : 0);

		input_event(input, EV_MSC, MSC_SERIAL, wacom->serial[0]);
		input_report_abs(input, ABS_MISC, proximity ? wacom_intuos_id_mangle(wacom->id[0]) : 0);
		input_report_key(input, wacom->tool[0], proximity ? 1 : 0);

		if (!proximity)
			wacom->tool[0] = 0;
	}

	wacom->shared->stylus_in_proximity = proximity;

	return 1;
}

static int wacom_mspro_irq(struct wacom_wac *wacom)
{
	unsigned char *data = wacom->data;
	struct input_dev *input = wacom->input;

	switch (data[0]) {
		case WACOM_REPORT_MSPRO:
			return wacom_mspro_pen_irq(wacom);
		case WACOM_REPORT_MSPROPAD:
			return wacom_mspro_pad_irq(wacom);
		case WACOM_REPORT_MSPRODEVICE:
			return wacom_mspro_device_irq(wacom);
		default:
			dev_dbg(input->dev.parent,
				"%s: received unknown report #%d\n", __func__, data[0]);
			break;
	}
	return 0;
}

void wacom_wac_irq(struct wacom_wac *wacom_wac, size_t len)
{
	bool sync;

	switch (wacom_wac->features.type) {
	case PENPARTNER:
		sync = wacom_penpartner_irq(wacom_wac);
		break;

	case PL:
		sync = wacom_pl_irq(wacom_wac);
		break;

	case WACOM_G4:
	case GRAPHIRE:
	case WACOM_MO:
		sync = wacom_graphire_irq(wacom_wac);
		break;

	case PTU:
		sync = wacom_ptu_irq(wacom_wac);
		break;

	case DTU:
		sync = wacom_dtu_irq(wacom_wac);
		break;

	case DTUS:
	case DTUSX:
	case DTK2451:
		sync = wacom_dtus_irq(wacom_wac);
		break;

	case DTUS2:
	case DTH1152:
		sync = wacom_dth1152_irq(wacom_wac);
		break;

	case INTUOS:
	case INTUOS3S:
	case INTUOS3:
	case INTUOS3L:
	case INTUOS4S:
	case INTUOS4:
	case INTUOS4L:
	case CINTIQ:
	case WACOM_BEE:
	case WACOM_13HD:
	case WACOM_21UX2:
	case WACOM_22HD:
	case WACOM_24HD:
	case WACOM_27QHD:
	case DTK:
	case CINTIQ_HYBRID:
	case CINTIQ_COMPANION_2:
		sync = wacom_intuos_irq(wacom_wac);
		break;

	case WACOM_MSPRO:
	case INTUOSP2:
	case INTUOSP2S:
	case CINTIQ_16:
		if (len == WACOM_PKGLEN_INTUOSP2T &&
		    wacom_wac->data[0] == WACOM_REPORT_VENDOR_DEF_TOUCH)
			sync = wacom_multitouch_generic(wacom_wac);
		else
			sync = wacom_mspro_irq(wacom_wac);
		break;

	case WACOM_24HDT:
	case WACOM_27QHDT:
	case DTH1152T:
	case DTH2452T:
	case WACOM_MSPROT:
		sync = wacom_multitouch_generic(wacom_wac);
		break;

	case INTUOS5S:
	case INTUOS5:
	case INTUOS5L:
	case INTUOSPS:
	case INTUOSPM:
	case INTUOSPL:
		if (len == WACOM_PKGLEN_BBTOUCH3)
			sync = wacom_bpt3_touch(wacom_wac);
		else if (wacom_wac->data[0] == WACOM_REPORT_USB)
			sync = wacom_status_irq(wacom_wac, len);
		else
			sync = wacom_intuos_irq(wacom_wac);
		break;

	case TABLETPC:
	case TABLETPCE:
	case TABLETPC2FG:
	case MTSCREEN:
	case MTTPC:
	case MTTPC_B:
	case MTTPC_C:
		sync = wacom_tpc_irq(wacom_wac, len);
		break;

	case BAMBOO_PT:
	case INTUOSHT:
	case INTUOSHT2:
		if (wacom_wac->data[0] == WACOM_REPORT_USB)
			sync = wacom_status_irq(wacom_wac, len);
		else
			sync = wacom_bpt_irq(wacom_wac, len);
		break;

	case INTUOSHT3:
		sync = wacom_mspro_irq(wacom_wac);
		break;

	case WIRELESS:
		sync = wacom_wireless_irq(wacom_wac, len);
		break;

	case REMOTE:
		sync = false;
		if (wacom_wac->data[0] == WACOM_REPORT_DEVICE_LIST)
			wacom_remote_status_irq(wacom_wac, len);
		else
			sync = wacom_remote_irq(wacom_wac, len);
		break;

	default:
		sync = false;
		break;
	}

	if (sync)
		input_sync(wacom_wac->input);
}

static void wacom_setup_basic_pro_pen(struct wacom_wac *wacom_wac)
{
	struct input_dev *input_dev = wacom_wac->input;

	input_set_capability(input_dev, EV_MSC, MSC_SERIAL);

	__set_bit(BTN_TOOL_PEN, input_dev->keybit);
	__set_bit(BTN_STYLUS, input_dev->keybit);
	__set_bit(BTN_STYLUS2, input_dev->keybit);

	input_set_abs_params(input_dev, ABS_DISTANCE,
			     0, wacom_wac->features.distance_max, wacom_wac->features.distance_fuzz, 0);
}

static void wacom_setup_cintiq(struct wacom_wac *wacom_wac)
{
	struct input_dev *input_dev = wacom_wac->input;
	struct wacom_features *features = &wacom_wac->features;

	wacom_setup_basic_pro_pen(wacom_wac);

	__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
	__set_bit(BTN_TOOL_BRUSH, input_dev->keybit);
	__set_bit(BTN_TOOL_PENCIL, input_dev->keybit);
	__set_bit(BTN_TOOL_AIRBRUSH, input_dev->keybit);

	input_set_abs_params(input_dev, ABS_WHEEL, 0, 1023, 0, 0);
	input_set_abs_params(input_dev, ABS_TILT_X, 0, 127, features->tilt_fuzz, 0);
	input_set_abs_params(input_dev, ABS_TILT_Y, 0, 127, features->tilt_fuzz, 0);
}

static void wacom_setup_intuos(struct wacom_wac *wacom_wac)
{
	struct input_dev *input_dev = wacom_wac->input;

	input_set_capability(input_dev, EV_REL, REL_WHEEL);

	wacom_setup_cintiq(wacom_wac);

	__set_bit(BTN_LEFT, input_dev->keybit);
	__set_bit(BTN_RIGHT, input_dev->keybit);
	__set_bit(BTN_MIDDLE, input_dev->keybit);
	__set_bit(BTN_SIDE, input_dev->keybit);
	__set_bit(BTN_EXTRA, input_dev->keybit);
	__set_bit(BTN_TOOL_MOUSE, input_dev->keybit);
	__set_bit(BTN_TOOL_LENS, input_dev->keybit);

	input_set_abs_params(input_dev, ABS_RZ, -900, 899, 0, 0);
	input_set_abs_params(input_dev, ABS_THROTTLE, -1023, 1023, 0, 0);
}

void wacom_setup_device_quirks(struct wacom *wacom)
{
	struct wacom_features *features = &wacom->wacom_wac.features;
	struct usb_endpoint_descriptor *endpoint =
			&(wacom->intf)->cur_altsetting->endpoint[0].desc;

	/* touch device found but size is not defined. use default */
	if (features->device_type == BTN_TOOL_FINGER && !features->x_max) {
		features->x_max = 1023;
		features->y_max = 1023;
	}

	/*
	 * Intuos5/Pro and Bamboo 3rd gen have no useful data about its
	 * touch interface in its HID descriptor. If this is the touch
	 * interface (wMaxPacketSize of WACOM_PKGLEN_BBTOUCH3), override
	 * the tablet values.
	 */
	if ((features->type >= INTUOS5S && features->type <= INTUOSPL) ||
		(features->type >= INTUOSHT && features->type <= BAMBOO_PT)) {
		if (endpoint->wMaxPacketSize == WACOM_PKGLEN_BBTOUCH3) {
			features->device_type = BTN_TOOL_FINGER;
			features->pktlen = WACOM_PKGLEN_BBTOUCH3;

			if (features->type == INTUOSHT2) {
				features->x_max = features->x_max / 10;
				features->y_max = features->y_max / 10;
			}
			else {
				features->x_max = 4096;
				features->y_max = 4096;
			}
		} else {
			features->device_type = BTN_TOOL_PEN;
		}
	}

	/* quirk for bamboo touch with 2 low res touches */
	if (features->type == BAMBOO_PT &&
	    features->pktlen == WACOM_PKGLEN_BBTOUCH) {
		features->x_max <<= 5;
		features->y_max <<= 5;
		features->x_fuzz <<= 5;
		features->y_fuzz <<= 5;
		features->quirks |= WACOM_QUIRK_BBTOUCH_LOWRES;
	}

	if (features->type == REMOTE)
		features->device_type = BTN_TOOL_FINGER;

	if (features->type == WIRELESS) {

		/* monitor never has input and pen/touch have delayed create */
		features->quirks |= WACOM_QUIRK_NO_INPUT;

		/* must be monitor interface if no device_type set */
		if (!features->device_type) {
			features->quirks |= WACOM_QUIRK_MONITOR;
			features->quirks |= WACOM_QUIRK_BATTERY;
		}
	}

	if (features->type == REMOTE)
		features->quirks |= WACOM_QUIRK_MONITOR;
}

static void wacom_abs_set_axis(struct input_dev *input_dev,
			       struct wacom_wac *wacom_wac)
{
	struct wacom_features *features = &wacom_wac->features;

	if (features->device_type == BTN_TOOL_PEN) {
		input_set_abs_params(input_dev, ABS_X, 0 + features->offset_left,
				     features->x_max - features->offset_right,
				     features->x_fuzz, 0);
		input_set_abs_params(input_dev, ABS_Y, 0 + features->offset_top,
				     features->y_max - features->offset_bottom,
				     features->y_fuzz, 0);
		input_set_abs_params(input_dev, ABS_PRESSURE, 0,
			features->pressure_max, features->pressure_fuzz, 0);

		/* penabled devices have fixed resolution for each model */
		input_abs_set_res(input_dev, ABS_X, features->x_resolution);
		input_abs_set_res(input_dev, ABS_Y, features->y_resolution);
	} else {
		if (features->touch_max == 1) {
			input_set_abs_params(input_dev, ABS_X, 0,
				features->x_max, features->x_fuzz, 0);
			input_set_abs_params(input_dev, ABS_Y, 0,
				features->y_max, features->y_fuzz, 0);
			input_abs_set_res(input_dev, ABS_X,
					  features->x_resolution);
			input_abs_set_res(input_dev, ABS_Y,
					  features->y_resolution);
		}

		if (features->touch_max > 1) {
			input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0,
				features->x_max, features->x_fuzz, 0);
			input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0,
				features->y_max, features->y_fuzz, 0);
			input_abs_set_res(input_dev, ABS_MT_POSITION_X,
					  features->x_resolution);
			input_abs_set_res(input_dev, ABS_MT_POSITION_Y,
					  features->y_resolution);
		}
	}
}

static int wacom_numbered_button_to_key(int n)
{
	if (n < 10)
		return BTN_0 + n;
	else if (n < 16)
		return BTN_A + (n-10);
	else if (n < 18)
		return BTN_BASE + (n-16);
	else
		return 0;
}

static void wacom_setup_numbered_buttons(struct input_dev *input_dev,
				int button_count)
{
	int i;

	for (i = 0; i < button_count; i++) {
		int key = wacom_numbered_button_to_key(i);

		if (key)
			__set_bit(key, input_dev->keybit);
	}
}

static void wacom_report_numbered_buttons(struct input_dev *input_dev,
				int button_count, int mask)
{
	int i;

	for (i = 0; i < button_count; i++) {
		int key = wacom_numbered_button_to_key(i);

		if (key)
			input_report_key(input_dev, key, mask & (1 << i));
	}
}

int wacom_setup_input_capabilities(struct input_dev *input_dev,
				   struct wacom_wac *wacom_wac)
{
	struct wacom_features *features = &wacom_wac->features;
	int numbered_buttons = features->numbered_buttons;
	if (features->type == REMOTE && input_dev == wacom_wac->input)
		return -ENODEV;

	input_dev->evbit[0] |= BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(ABS_MISC, input_dev->absbit);

	wacom_abs_set_axis(input_dev, wacom_wac);

	switch (features->type) {
	case REMOTE:
		/* kept for making legacy xf86-input-wacom accepting the pad */
		input_set_abs_params(input_dev, ABS_X, 0, 1, 0, 0);
		input_set_abs_params(input_dev, ABS_Y, 0, 1, 0, 0);

		/* kept for making udev and libwacom accepting the pad */
		__set_bit(BTN_STYLUS, input_dev->keybit);
		__clear_bit(BTN_TOUCH, input_dev->keybit);
		input_set_capability(input_dev, EV_MSC, MSC_SERIAL);
		input_set_abs_params(input_dev, ABS_WHEEL, 0, 71, 0, 0);
		break;

	case WACOM_MO:
		input_set_abs_params(input_dev, ABS_WHEEL, 0, 71, 0, 0);
		/* fall through */

	case WACOM_G4:
		input_set_capability(input_dev, EV_MSC, MSC_SERIAL);
		input_set_abs_params(input_dev, ABS_DISTANCE, 0,
			features->distance_max, features->distance_fuzz, 0);

		__set_bit(BTN_BACK, input_dev->keybit);
		__set_bit(BTN_FORWARD, input_dev->keybit);
		/* fall through */

	case GRAPHIRE:
		input_set_capability(input_dev, EV_REL, REL_WHEEL);

		__set_bit(BTN_LEFT, input_dev->keybit);
		__set_bit(BTN_RIGHT, input_dev->keybit);
		__set_bit(BTN_MIDDLE, input_dev->keybit);

		__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
		__set_bit(BTN_TOOL_PEN, input_dev->keybit);
		__set_bit(BTN_TOOL_MOUSE, input_dev->keybit);
		__set_bit(BTN_STYLUS, input_dev->keybit);
		__set_bit(BTN_STYLUS2, input_dev->keybit);

		__set_bit(INPUT_PROP_POINTER, input_dev->propbit);
		break;

	case WACOM_MSPRO:
	case CINTIQ_16:
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		__set_bit(BTN_STYLUS3, input_dev->keybit);
		__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

		if (features->type == WACOM_MSPRO &&
		    features->numbered_buttons == 0) { /* Cintiq Pro */
			__set_bit(KEY_CONTROLPANEL, input_dev->keybit);
			__set_bit(KEY_ONSCREEN_KEYBOARD, input_dev->keybit);
			__set_bit(KEY_BUTTONCONFIG, input_dev->keybit);

			wacom_wac->previous_ring = WACOM_INTUOSP2_RING_UNTOUCHED;
		}

		wacom_setup_cintiq(wacom_wac);
		break;

	case WACOM_24HD:
		__set_bit(KEY_PROG1, input_dev->keybit);
		__set_bit(KEY_PROG2, input_dev->keybit);
		__set_bit(KEY_PROG3, input_dev->keybit);

		__set_bit(KEY_ONSCREEN_KEYBOARD, input_dev->keybit);
		__set_bit(KEY_INFO, input_dev->keybit);

		if (!features->oPid)
			__set_bit(KEY_BUTTONCONFIG, input_dev->keybit);

		input_set_abs_params(input_dev, ABS_THROTTLE, 0, 71, 0, 0);
		/* fall through */

	case WACOM_13HD:
	case CINTIQ_HYBRID:
	case CINTIQ_COMPANION_2:
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		/* fall through */

	case DTK:
		__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

		wacom_setup_cintiq(wacom_wac);
		break;

	case WACOM_27QHD:
		__set_bit(KEY_PROG1, input_dev->keybit);
		__set_bit(KEY_PROG2, input_dev->keybit);
		__set_bit(KEY_PROG3, input_dev->keybit);

		__set_bit(KEY_ONSCREEN_KEYBOARD, input_dev->keybit);
		__set_bit(KEY_BUTTONCONFIG, input_dev->keybit);

		if (!features->oPid)
			__set_bit(KEY_CONTROLPANEL, input_dev->keybit);

		__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);

		wacom_setup_cintiq(wacom_wac);
		break;

	case WACOM_22HD:
		__set_bit(KEY_PROG1, input_dev->keybit);
		__set_bit(KEY_PROG2, input_dev->keybit);
		__set_bit(KEY_PROG3, input_dev->keybit);

		__set_bit(KEY_BUTTONCONFIG, input_dev->keybit);
		__set_bit(KEY_INFO, input_dev->keybit);
		/* fall through */

	case WACOM_21UX2:
	case WACOM_BEE:
	case CINTIQ:
		input_set_abs_params(input_dev, ABS_RX, 0, 4096, 0, 0);
		input_set_abs_params(input_dev, ABS_RY, 0, 4096, 0, 0);
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

		wacom_setup_cintiq(wacom_wac);
		break;

	case INTUOS3:
	case INTUOS3L:
		input_set_abs_params(input_dev, ABS_RY, 0, 4096, 0, 0);
		/* fall through */

	case INTUOS3S:
		input_set_abs_params(input_dev, ABS_RX, 0, 4096, 0, 0);
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		/* fall through */

	case INTUOS:
		__set_bit(INPUT_PROP_POINTER, input_dev->propbit);

		wacom_setup_intuos(wacom_wac);
		break;

	case INTUOSP2:
	case INTUOSP2S:
		if (features->device_type == BTN_TOOL_PEN) {
			__set_bit(BTN_STYLUS3, input_dev->keybit);
			wacom_wac->previous_ring = WACOM_INTUOSP2_RING_UNTOUCHED;
			wacom_wac->shared->has_mute_touch_switch = true;
		}
		else {
			input_dev->evbit[0] |= BIT_MASK(EV_SW);
			__set_bit(SW_MUTE_DEVICE, input_dev->swbit);
			wacom_wac->shared->has_mute_touch_switch = true;
		}
		/* fall through */

	case INTUOS5:
	case INTUOS5L:
	case INTUOSPM:
	case INTUOSPL:
	case INTUOS5S:
	case INTUOSPS:
		__set_bit(INPUT_PROP_POINTER, input_dev->propbit);

		if (features->device_type == BTN_TOOL_PEN) {
			input_set_abs_params(input_dev, ABS_DISTANCE, 0,
					      features->distance_max,
					      features->distance_fuzz, 0);

			input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);

			wacom_setup_intuos(wacom_wac);
		} else if (features->device_type == BTN_TOOL_FINGER) {
			__clear_bit(ABS_MISC, input_dev->absbit);

			/* pad is on pen interface */
			numbered_buttons = 0;
			input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR,
			                     0, features->x_max, 0, 0);
			input_set_abs_params(input_dev, ABS_MT_TOUCH_MINOR,
			                     0, features->y_max, 0, 0);
			input_mt_init_slots(input_dev, features->touch_max, INPUT_MT_POINTER);
		}
		break;

	case INTUOS4:
	case INTUOS4L:
	case INTUOS4S:
		input_set_abs_params(input_dev, ABS_Z, -900, 899, 0, 0);
		wacom_setup_intuos(wacom_wac);

		__set_bit(INPUT_PROP_POINTER, input_dev->propbit);
		break;

	case WACOM_24HDT:
		if (features->device_type == BTN_TOOL_FINGER) {
			input_set_abs_params(input_dev, ABS_MT_WIDTH_MAJOR, 0, features->x_max, 0, 0);
			input_set_abs_params(input_dev, ABS_MT_WIDTH_MINOR, 0, features->y_max, 0, 0);
		}
		/* fall through */

	case DTH1152T:
	case DTH2452T:
	case WACOM_MSPROT:
		if (features->device_type == BTN_TOOL_FINGER) {
			input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, features->x_max, 0, 0);
			if (features->type != WACOM_24HDT)
				input_set_abs_params(input_dev, ABS_MT_TOUCH_MINOR,
						     0, features->y_max, 0, 0);
			input_set_abs_params(input_dev, ABS_MT_ORIENTATION, 0, 1, 0, 0);
		}
		/* fall through */

	case WACOM_27QHDT:
		if (wacom_wac->shared->touch_input->id.product == 0x32C ||
		    wacom_wac->shared->touch_input->id.product == 0xF6) {
			input_dev->evbit[0] |= BIT_MASK(EV_SW);
			__set_bit(SW_MUTE_DEVICE, input_dev->swbit);
			wacom_wac->shared->has_mute_touch_switch = true;
		}
		/* fall through */

	case MTSCREEN:
	case MTTPC:
	case MTTPC_B:
	case MTTPC_C:
	case TABLETPC2FG:
		if ((features->device_type == BTN_TOOL_FINGER) && features->touch_max > 1)
			input_mt_init_slots(input_dev, features->touch_max, INPUT_MT_DIRECT);
		/* fall through */

	case TABLETPC:
	case TABLETPCE:
		__clear_bit(ABS_MISC, input_dev->absbit);

		__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

		if ((features->device_type == BTN_TOOL_FINGER) &&
		    (input_dev->id.product >= 0x353 && input_dev->id.product <= 0x356)) {
			input_dev->evbit[0] |= BIT_MASK(EV_SW);
			__set_bit(SW_MUTE_DEVICE, input_dev->swbit);
			wacom_wac->shared->has_mute_touch_switch = true;
			wacom_wac->shared->is_touch_on = true;
		}

		if (features->device_type != BTN_TOOL_PEN)
			break;  /* no need to process stylus stuff */

		/* fall through */

	case DTUS:
	case DTUS2:
	case DTK2451:
		input_set_capability(input_dev, EV_MSC, MSC_SERIAL);
		/* fall through */

	case DTUSX:
	case PL:
	case DTU:
		if (features->type != DTUS2) {
			__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
			__set_bit(BTN_STYLUS2, input_dev->keybit);
		}
		/* fall through */

	case DTH1152:
		__set_bit(BTN_TOOL_PEN, input_dev->keybit);
		__set_bit(BTN_STYLUS, input_dev->keybit);

		__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
		break;

	case PTU:
		__set_bit(BTN_STYLUS2, input_dev->keybit);
		/* fall through */

	case PENPARTNER:
		__set_bit(BTN_TOOL_PEN, input_dev->keybit);
		__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
		__set_bit(BTN_STYLUS, input_dev->keybit);

		__set_bit(INPUT_PROP_POINTER, input_dev->propbit);
		break;

	case INTUOSHT:
	case INTUOSHT2:
		if (features->touch_max &&
		    features->device_type == BTN_TOOL_FINGER) {
			input_dev->evbit[0] |= BIT_MASK(EV_SW);
			__set_bit(SW_MUTE_DEVICE, input_dev->swbit);
			wacom_wac->shared->has_mute_touch_switch = true;
		}
		/* fall through */

	case INTUOSHT3:
	case BAMBOO_PT:
		__clear_bit(ABS_MISC, input_dev->absbit);

		if (features->device_type == BTN_TOOL_FINGER) {

			__set_bit(BTN_LEFT, input_dev->keybit);
			__set_bit(BTN_FORWARD, input_dev->keybit);
			__set_bit(BTN_BACK, input_dev->keybit);
			__set_bit(BTN_RIGHT, input_dev->keybit);

			if (features->touch_max) {
				if (features->pktlen == WACOM_PKGLEN_BBTOUCH3) {
					input_set_abs_params(input_dev,
						     ABS_MT_TOUCH_MAJOR,
						     0, features->x_max, 0, 0);
					input_set_abs_params(input_dev,
						     ABS_MT_TOUCH_MINOR,
						     0, features->y_max, 0, 0);
				}
				input_mt_init_slots(input_dev, features->touch_max, INPUT_MT_POINTER);
			} else {
				/* buttons/keys only interface */
				__clear_bit(ABS_X, input_dev->absbit);
				__clear_bit(ABS_Y, input_dev->absbit);
				__clear_bit(BTN_TOUCH, input_dev->keybit);

				/* For Bamboo, buttons are supported only when touch is supported */
				if (features->type == BAMBOO_PT) {
					__clear_bit(BTN_LEFT, input_dev->keybit);
					__clear_bit(BTN_FORWARD, input_dev->keybit);
					__clear_bit(BTN_BACK, input_dev->keybit);
					__clear_bit(BTN_RIGHT, input_dev->keybit);
				}
			}
		} else if (features->device_type == BTN_TOOL_PEN) {
			__set_bit(INPUT_PROP_POINTER, input_dev->propbit);
			if (features->type == INTUOSHT2 || features->type == INTUOSHT3) {
				__set_bit(ABS_MISC, input_dev->absbit);
				wacom_setup_basic_pro_pen(wacom_wac);
			} else {
				__set_bit(BTN_TOOL_RUBBER, input_dev->keybit);
				__set_bit(BTN_TOOL_PEN, input_dev->keybit);
				__set_bit(BTN_STYLUS, input_dev->keybit);
				__set_bit(BTN_STYLUS2, input_dev->keybit);
				input_set_abs_params(input_dev, ABS_DISTANCE, 0,
					      features->distance_max,
					      features->distance_fuzz, 0);
			}
		}
		break;
	}

	/*
	 * Because all devices with numbered buttons have the pen and pad on
	 * the same interface we can rely on this check to avoid creating
	 * extra pads. Future devices may require creating a more involved
	 * check.
	 */
	if (features->device_type == BTN_TOOL_PEN || features->type == REMOTE)
		wacom_setup_numbered_buttons(input_dev, numbered_buttons);

	return 0;
}

static const struct wacom_features wacom_features_0x00 =
	{ "Wacom Penpartner",     WACOM_PKGLEN_PENPRTN,    5040, 3780, 255, 0,
	  PENPARTNER, WACOM_PENPRTN_RES, WACOM_PENPRTN_RES };
static const struct wacom_features wacom_features_0x10 =
	{ "Wacom Graphire",       WACOM_PKGLEN_GRAPHIRE,  10206, 7422, 511, 63,
	  GRAPHIRE, WACOM_GRAPHIRE_RES, WACOM_GRAPHIRE_RES };
static const struct wacom_features wacom_features_0x11 =
	{ "Wacom Graphire2 4x5",  WACOM_PKGLEN_GRAPHIRE,  10206, 7422, 511, 63,
	  GRAPHIRE, WACOM_GRAPHIRE_RES, WACOM_GRAPHIRE_RES };
static const struct wacom_features wacom_features_0x12 =
	{ "Wacom Graphire2 5x7",  WACOM_PKGLEN_GRAPHIRE,  13918, 10206, 511, 63,
	  GRAPHIRE, WACOM_GRAPHIRE_RES, WACOM_GRAPHIRE_RES };
static const struct wacom_features wacom_features_0x13 =
	{ "Wacom Graphire3",      WACOM_PKGLEN_GRAPHIRE,  10208, 7424, 511, 63,
	  GRAPHIRE, WACOM_GRAPHIRE_RES, WACOM_GRAPHIRE_RES };
static const struct wacom_features wacom_features_0x14 =
	{ "Wacom Graphire3 6x8",  WACOM_PKGLEN_GRAPHIRE,  16704, 12064, 511, 63,
	  GRAPHIRE, WACOM_GRAPHIRE_RES, WACOM_GRAPHIRE_RES };
static const struct wacom_features wacom_features_0x15 =
	{ "Wacom Graphire4 4x5",  WACOM_PKGLEN_GRAPHIRE,  10208, 7424, 511, 63,
	  WACOM_G4, WACOM_GRAPHIRE_RES, WACOM_GRAPHIRE_RES };
static const struct wacom_features wacom_features_0x16 =
	{ "Wacom Graphire4 6x8",  WACOM_PKGLEN_GRAPHIRE,  16704, 12064, 511, 63,
	  WACOM_G4, WACOM_GRAPHIRE_RES, WACOM_GRAPHIRE_RES };
static const struct wacom_features wacom_features_0x17 =
	{ "Wacom BambooFun 4x5",  WACOM_PKGLEN_BBFUN,     14760, 9225, 511, 63,
	  WACOM_MO, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x18 =
	{ "Wacom BambooFun 6x8",  WACOM_PKGLEN_BBFUN,     21648, 13530, 511, 63,
	  WACOM_MO, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x19 =
	{ "Wacom Bamboo1 Medium", WACOM_PKGLEN_GRAPHIRE,  16704, 12064, 511, 63,
	  GRAPHIRE, WACOM_GRAPHIRE_RES, WACOM_GRAPHIRE_RES };
static const struct wacom_features wacom_features_0x60 =
	{ "Wacom Volito",         WACOM_PKGLEN_GRAPHIRE,   5104, 3712, 511, 63,
	  GRAPHIRE, WACOM_VOLITO_RES, WACOM_VOLITO_RES };
static const struct wacom_features wacom_features_0x61 =
	{ "Wacom PenStation2",    WACOM_PKGLEN_GRAPHIRE,   3250, 2320, 255, 63,
	  GRAPHIRE, WACOM_VOLITO_RES, WACOM_VOLITO_RES };
static const struct wacom_features wacom_features_0x62 =
	{ "Wacom Volito2 4x5",    WACOM_PKGLEN_GRAPHIRE,   5104, 3712, 511, 63,
	  GRAPHIRE, WACOM_VOLITO_RES, WACOM_VOLITO_RES };
static const struct wacom_features wacom_features_0x63 =
	{ "Wacom Volito2 2x3",    WACOM_PKGLEN_GRAPHIRE,   3248, 2320, 511, 63,
	  GRAPHIRE, WACOM_VOLITO_RES, WACOM_VOLITO_RES };
static const struct wacom_features wacom_features_0x64 =
	{ "Wacom PenPartner2",    WACOM_PKGLEN_GRAPHIRE,   3250, 2320, 511, 63,
	  GRAPHIRE, WACOM_VOLITO_RES, WACOM_VOLITO_RES };
static const struct wacom_features wacom_features_0x65 =
	{ "Wacom Bamboo",         WACOM_PKGLEN_BBFUN,     14760, 9225, 511, 63,
	  WACOM_MO, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x69 =
	{ "Wacom Bamboo1",        WACOM_PKGLEN_GRAPHIRE,   5104, 3712, 511, 63,
	  GRAPHIRE, WACOM_PENPRTN_RES, WACOM_PENPRTN_RES };
static const struct wacom_features wacom_features_0x6A =
	{ "Wacom Bamboo1 4x6",    WACOM_PKGLEN_GRAPHIRE,  14760, 9225, 1023, 63,
	  GRAPHIRE, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x6B =
	{ "Wacom Bamboo1 5x8",    WACOM_PKGLEN_GRAPHIRE,  21648, 13530, 1023, 63,
	  GRAPHIRE, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x20 =
	{ "Wacom Intuos 4x5",     WACOM_PKGLEN_INTUOS,    12700, 10600, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x21 =
	{ "Wacom Intuos 6x8",     WACOM_PKGLEN_INTUOS,    20320, 16240, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x22 =
	{ "Wacom Intuos 9x12",    WACOM_PKGLEN_INTUOS,    30480, 24060, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x23 =
	{ "Wacom Intuos 12x12",   WACOM_PKGLEN_INTUOS,    30480, 31680, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x24 =
	{ "Wacom Intuos 12x18",   WACOM_PKGLEN_INTUOS,    45720, 31680, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x30 =
	{ "Wacom PL400",          WACOM_PKGLEN_GRAPHIRE,   5408, 4056, 255, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x31 =
	{ "Wacom PL500",          WACOM_PKGLEN_GRAPHIRE,   6144, 4608, 255, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x32 =
	{ "Wacom PL600",          WACOM_PKGLEN_GRAPHIRE,   6126, 4604, 255, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x33 =
	{ "Wacom PL600SX",        WACOM_PKGLEN_GRAPHIRE,   6260, 5016, 255, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x34 =
	{ "Wacom PL550",          WACOM_PKGLEN_GRAPHIRE,   6144, 4608, 511, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x35 =
	{ "Wacom PL800",          WACOM_PKGLEN_GRAPHIRE,   7220, 5780, 511, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x37 =
	{ "Wacom PL700",          WACOM_PKGLEN_GRAPHIRE,   6758, 5406, 511, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x38 =
	{ "Wacom PL510",          WACOM_PKGLEN_GRAPHIRE,   6282, 4762, 511, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x39 =
	{ "Wacom DTU710",         WACOM_PKGLEN_GRAPHIRE,  34080, 27660, 511, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0xC4 =
	{ "Wacom DTF521",         WACOM_PKGLEN_GRAPHIRE,   6282, 4762, 511, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0xC0 =
	{ "Wacom DTF720",         WACOM_PKGLEN_GRAPHIRE,   6858, 5506, 511, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0xC2 =
	{ "Wacom DTF720a",        WACOM_PKGLEN_GRAPHIRE,   6858, 5506, 511, 0,
	  PL, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x03 =
	{ "Wacom Cintiq Partner", WACOM_PKGLEN_GRAPHIRE,  20480, 15360, 511, 0,
	  PTU, WACOM_PL_RES, WACOM_PL_RES };
static const struct wacom_features wacom_features_0x41 =
	{ "Wacom Intuos2 4x5",    WACOM_PKGLEN_INTUOS,    12700, 10600, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x42 =
	{ "Wacom Intuos2 6x8",    WACOM_PKGLEN_INTUOS,    20320, 16240, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x43 =
	{ "Wacom Intuos2 9x12",   WACOM_PKGLEN_INTUOS,    30480, 24060, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x44 =
	{ "Wacom Intuos2 12x12",  WACOM_PKGLEN_INTUOS,    30480, 31680, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x45 =
	{ "Wacom Intuos2 12x18",  WACOM_PKGLEN_INTUOS,    45720, 31680, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xB0 =
	{ "Wacom Intuos3 4x5",    WACOM_PKGLEN_INTUOS,    25400, 20320, 1023, 63,
	  INTUOS3S, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 4 };
static const struct wacom_features wacom_features_0xB1 =
	{ "Wacom Intuos3 6x8",    WACOM_PKGLEN_INTUOS,    40640, 30480, 1023, 63,
	  INTUOS3, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 8 };
static const struct wacom_features wacom_features_0xB2 =
	{ "Wacom Intuos3 9x12",   WACOM_PKGLEN_INTUOS,    60960, 45720, 1023, 63,
	  INTUOS3, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 8 };
static const struct wacom_features wacom_features_0xB3 =
	{ "Wacom Intuos3 12x12",  WACOM_PKGLEN_INTUOS,    60960, 60960, 1023, 63,
	  INTUOS3L, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 8 };
static const struct wacom_features wacom_features_0xB4 =
	{ "Wacom Intuos3 12x19",  WACOM_PKGLEN_INTUOS,    97536, 60960, 1023, 63,
	  INTUOS3L, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 8 };
static const struct wacom_features wacom_features_0xB5 =
	{ "Wacom Intuos3 6x11",   WACOM_PKGLEN_INTUOS,    54204, 31750, 1023, 63,
	  INTUOS3, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 8 };
static const struct wacom_features wacom_features_0xB7 =
	{ "Wacom Intuos3 4x6",    WACOM_PKGLEN_INTUOS,    31496, 19685, 1023, 63,
	  INTUOS3S, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 4 };
static const struct wacom_features wacom_features_0xB8 =
	{ "Wacom Intuos4 4x6",    WACOM_PKGLEN_INTUOS,    31496, 19685, 2047, 63,
	  INTUOS4S, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 7 };
static const struct wacom_features wacom_features_0xB9 =
	{ "Wacom Intuos4 6x9",    WACOM_PKGLEN_INTUOS,    44704, 27940, 2047, 63,
	  INTUOS4, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9 };
static const struct wacom_features wacom_features_0xBA =
	{ "Wacom Intuos4 8x13",   WACOM_PKGLEN_INTUOS,    65024, 40640, 2047, 63,
	  INTUOS4L, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9 };
static const struct wacom_features wacom_features_0xBB =
	{ "Wacom Intuos4 12x19",  WACOM_PKGLEN_INTUOS,    97536, 60960, 2047, 63,
	  INTUOS4L, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9 };
static const struct wacom_features wacom_features_0xBC =
	{ "Wacom Intuos4 WL",     WACOM_PKGLEN_INTUOS,    40640, 25400, 2047, 63,
	  INTUOS4, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9 };
static const struct wacom_features wacom_features_0x26 =
	{ "Wacom Intuos5 touch S", WACOM_PKGLEN_INTUOS,  31496, 19685, 2047, 63,
	  INTUOS5S, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 7, .touch_max = 16 };
static const struct wacom_features wacom_features_0x27 =
	{ "Wacom Intuos5 touch M", WACOM_PKGLEN_INTUOS,  44704, 27940, 2047, 63,
	  INTUOS5, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9, .touch_max = 16 };
static const struct wacom_features wacom_features_0x28 =
	{ "Wacom Intuos5 touch L", WACOM_PKGLEN_INTUOS, 65024, 40640, 2047, 63,
	  INTUOS5L, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9, .touch_max = 16 };
static const struct wacom_features wacom_features_0x29 =
	{ "Wacom Intuos5 S", WACOM_PKGLEN_INTUOS,  31496, 19685, 2047, 63,
	  INTUOS5S, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 7 };
static const struct wacom_features wacom_features_0x2A =
	{ "Wacom Intuos5 M", WACOM_PKGLEN_INTUOS,  44704, 27940, 2047, 63,
	  INTUOS5, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9 };
static const struct wacom_features wacom_features_0x314 =
	{ "Wacom Intuos Pro S", WACOM_PKGLEN_INTUOS,  31496, 19685, 2047, 63,
	  INTUOSPS, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 7, .touch_max = 16 };
static const struct wacom_features wacom_features_0x315 =
	{ "Wacom Intuos Pro M", WACOM_PKGLEN_INTUOS,  44704, 27940, 2047, 63,
	  INTUOSPM, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9, .touch_max = 16 };
static const struct wacom_features wacom_features_0x317 =
	{ "Wacom Intuos Pro L", WACOM_PKGLEN_INTUOS,  65024, 40640, 2047, 63,
	  INTUOSPL, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9, .touch_max = 16 };
static const struct wacom_features wacom_features_0xF4 =
	{ "Wacom Cintiq 24HD",       WACOM_PKGLEN_INTUOS,   104480, 65600, 2047, 63,
	  WACOM_24HD, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 16,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0xF8 =
	{ "Wacom Cintiq 24HD touch", WACOM_PKGLEN_INTUOS,   104480, 65600, 2047, 63, /* Pen */
	  WACOM_24HD, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 16,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0xf6 };
static const struct wacom_features wacom_features_0xF6 =
	{ "Wacom Cintiq 24HD touch", .type = WACOM_24HDT, /* Touch */
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0xf8, .touch_max = 10 };
static const struct wacom_features wacom_features_0x32A =
	{ "Wacom Cintiq 27QHD", WACOM_PKGLEN_INTUOS, 120140, 67920, 2047, 63,
	  WACOM_27QHD, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x32B =
	{ "Wacom Cintiq 27QHD touch", WACOM_PKGLEN_INTUOS, 120140, 67920, 2047, 63,
	  WACOM_27QHD, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x32C };
static const struct wacom_features wacom_features_0x32C =
	{ "Wacom Cintiq 27QHD touch", .type = WACOM_27QHDT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x32B, .touch_max = 10 };
static const struct wacom_features wacom_features_0x3F =
	{ "Wacom Cintiq 21UX",    WACOM_PKGLEN_INTUOS,    87200, 65600, 1023, 63,
	  CINTIQ, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 8 };
static const struct wacom_features wacom_features_0xC5 =
	{ "Wacom Cintiq 20WSX",   WACOM_PKGLEN_INTUOS,    86680, 54180, 1023, 63,
	  WACOM_BEE, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 10 };
static const struct wacom_features wacom_features_0xC6 =
	{ "Wacom Cintiq 12WX",    WACOM_PKGLEN_INTUOS,    53020, 33440, 1023, 63,
	  WACOM_BEE, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 10 };
static const struct wacom_features wacom_features_0x304 =
	{ "Wacom Cintiq 13HD",    WACOM_PKGLEN_INTUOS,    59552, 33848, 1023, 63,
	  WACOM_13HD, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x333 =
	{ "Wacom Cintiq 13HD touch", WACOM_PKGLEN_INTUOS, 59552, 33848, 2047, 63,
	  WACOM_13HD, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x335 };
static const struct wacom_features wacom_features_0x335 =
	{ "Wacom Cintiq 13HD touch", .type = WACOM_24HDT, /* Touch */
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x333, .touch_max = 10 };
static const struct wacom_features wacom_features_0xC7 =
	{ "Wacom DTU1931",        WACOM_PKGLEN_GRAPHIRE,  37832, 30305, 511, 0,
	  PL, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xCE =
	{ "Wacom DTU2231",        WACOM_PKGLEN_GRAPHIRE,  47864, 27011, 511, 0,
	  DTU, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xF0 =
	{ "Wacom DTU1631",        WACOM_PKGLEN_GRAPHIRE,  34623, 19553, 511, 0,
	  DTU, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xFB =
	{ "Wacom DTU1031",        WACOM_PKGLEN_DTUS,      22096, 13960, 511, 0,
	  DTUS, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x32F =
	{ "Wacom DTU1031X",       WACOM_PKGLEN_DTUS,      22672, 12928, 511, 0,
	  DTUSX, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 0,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x336 =
	{ "Wacom DTU1141",        WACOM_PKGLEN_DTUS,      23672, 13403, 1023, 0,
	  DTUS, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x57 =
	{ "Wacom DTK2241",        WACOM_PKGLEN_INTUOS,    95840, 54260, 2047, 63,
	  DTK, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 6,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x59 = /* Pen */
	{ "Wacom DTH2242",        WACOM_PKGLEN_INTUOS,    95840, 54260, 2047, 63,
	  DTK, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 6,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x5D };
static const struct wacom_features wacom_features_0x5D = /* Touch */
	{ "Wacom DTH2242",       .type = WACOM_24HDT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x59, .touch_max = 10 };
static const struct wacom_features wacom_features_0xCC =
	{ "Wacom Cintiq 21UX2",   WACOM_PKGLEN_INTUOS,    87200, 65600, 2047, 63,
	  WACOM_21UX2, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 18,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0xFA =
	{ "Wacom Cintiq 22HD",    WACOM_PKGLEN_INTUOS,    95840, 54260, 2047, 63,
	  WACOM_22HD, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 18,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x5B =
	{ "Wacom Cintiq 22HDT", WACOM_PKGLEN_INTUOS,      95840, 54260, 2047, 63,
	  WACOM_22HD, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 18,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x5e };
static const struct wacom_features wacom_features_0x5E =
	{ "Wacom Cintiq 22HDT", .type = WACOM_24HDT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x5b, .touch_max = 10 };
static const struct wacom_features wacom_features_0x90 =
	{ "Wacom ISDv4 90",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325, 255, 0,
	  TABLETPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x93 =
	{ "Wacom ISDv4 93",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325, 255, 0,
	  TABLETPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x97 =
	{ "Wacom ISDv4 97",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325, 511, 0,
	  TABLETPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x9A =
	{ "Wacom ISDv4 9A",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325, 255, 0,
	  TABLETPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x9F =
	{ "Wacom ISDv4 9F",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325, 255, 0,
	  TABLETPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xE2 =
	{ "Wacom ISDv4 E2",       WACOM_PKGLEN_TPC2FG,    26202, 16325, 255, 0,
	  TABLETPC2FG, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xE3 =
	{ "Wacom ISDv4 E3",       WACOM_PKGLEN_TPC2FG,    26202, 16325, 255, 0,
	  TABLETPC2FG, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xE5 =
	{ "Wacom ISDv4 E5",       WACOM_PKGLEN_MTOUCH,    26202, 16325, 255, 0,
	  MTSCREEN, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xE6 =
	{ "Wacom ISDv4 E6",       WACOM_PKGLEN_TPC2FG,    27760, 15694, 255, 0,
	  TABLETPC2FG, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xEC =
	{ "Wacom ISDv4 EC",       WACOM_PKGLEN_GRAPHIRE,  25710, 14500, 255, 0,
	  TABLETPC,    WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xED =
	{ "Wacom ISDv4 ED",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325, 255, 0,
	  TABLETPCE, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xEF =
	{ "Wacom ISDv4 EF",       WACOM_PKGLEN_GRAPHIRE,  26202, 16325, 255, 0,
	  TABLETPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x100 =
	{ "Wacom ISDv4 100",      WACOM_PKGLEN_MTTPC,     26202, 16325, 255, 0,
	  MTTPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x101 =
	{ "Wacom ISDv4 101",      WACOM_PKGLEN_MTTPC,     26202, 16325, 255, 0,
	  MTTPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x10D =
	{ "Wacom ISDv4 10D",      WACOM_PKGLEN_MTTPC,     26202, 16325, 255, 0,
	  MTTPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x10E =
	{ "Wacom ISDv4 10E",      WACOM_PKGLEN_MTTPC,     27760, 15694, 255, 0,
	  MTTPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x10F =
	{ "Wacom ISDv4 10F",      WACOM_PKGLEN_MTTPC,     27760, 15694, 255, 0,
	  MTTPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x116 =
	{ "Wacom ISDv4 116",      WACOM_PKGLEN_GRAPHIRE,  26202, 16325, 255, 0,
	  TABLETPCE, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x12C =
	{ "Wacom ISDv4 12C",      WACOM_PKGLEN_GRAPHIRE,  27848, 15752, 2047, 0,
	  TABLETPCE, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x4001 =
	{ "Wacom ISDv4 4001",      WACOM_PKGLEN_MTTPC,     26202, 16325, 255, 0,
	  MTTPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x4004 =
	{ "Wacom ISDv4 4004",      WACOM_PKGLEN_MTTPC,     11060, 6220, 255, 0,
	  MTTPC_B, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x5000 =
	{ "Wacom ISDv4 5000",      WACOM_PKGLEN_MTTPC,     27848, 15752, 1023, 0,
	  MTTPC_B, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x5002 =
	{ "Wacom ISDv4 5002",      WACOM_PKGLEN_MTTPC,     29576, 16724,  1023, 0,
	  MTTPC_B, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x5010 =
	{ "Wacom ISDv4 5010",      WACOM_PKGLEN_MTTPC,     13756, 7736,  1023, 0,
	  MTTPC_B, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x5013 =
	{ "Wacom ISDv4 5013",      WACOM_PKGLEN_MTTPC,     11752, 6612,  1023, 0,
	  MTTPC_B, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x5044 =
	{ "Wacom ISDv4 5044",      WACOM_PKGLEN_MTTPC,     27648, 15552, 2047, 0,
	  MTTPC_C, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x5048 =
	{ "Wacom ISDv4 5048",      WACOM_PKGLEN_MTTPC,     27648, 15552, 2047, 0,
	  MTTPC_C, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x5090 =
	{ "Wacom ISDv4 5090",      WACOM_PKGLEN_MTTPC,     27648, 15552, 2047, 0,
	  MTTPC_C, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x47 =
	{ "Wacom Intuos2 6x8",    WACOM_PKGLEN_INTUOS,    20320, 16240, 1023, 31,
	  INTUOS, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x84 =
	{ "Wacom Wireless Receiver", WACOM_PKGLEN_WIRELESS, .type = WIRELESS, .touch_max = 16 };
static const struct wacom_features wacom_features_0xD0 =
	{ "Wacom Bamboo 2FG",     WACOM_PKGLEN_BBFUN,     14720, 9200, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xD1 =
	{ "Wacom Bamboo 2FG 4x5", WACOM_PKGLEN_BBFUN,     14720, 9200, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xD2 =
	{ "Wacom Bamboo Craft",   WACOM_PKGLEN_BBFUN,     14720, 9200, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xD3 =
	{ "Wacom Bamboo 2FG 6x8", WACOM_PKGLEN_BBFUN,     21648, 13700, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xD4 =
	{ "Wacom Bamboo Pen",     WACOM_PKGLEN_BBFUN,     14720, 9200, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xD5 =
	{ "Wacom Bamboo Pen 6x8",     WACOM_PKGLEN_BBFUN, 21648, 13700, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xD6 =
	{ "Wacom BambooPT 2FG 4x5", WACOM_PKGLEN_BBFUN,   14720, 9200, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xD7 =
	{ "Wacom BambooPT 2FG Small", WACOM_PKGLEN_BBFUN, 14720, 9200, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xD8 =
	{ "Wacom Bamboo Comic 2FG", WACOM_PKGLEN_BBFUN,   21648, 13700, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xDA =
	{ "Wacom Bamboo 2FG 4x5 SE", WACOM_PKGLEN_BBFUN,  14720, 9200, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xDB =
	{ "Wacom Bamboo 2FG 6x8 SE", WACOM_PKGLEN_BBFUN,  21648, 13700, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 2 };
static const struct wacom_features wacom_features_0xDD =
        { "Wacom Bamboo Connect", WACOM_PKGLEN_BBPEN,     14720, 9200, 1023, 31,
          BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0xDE =
        { "Wacom Bamboo 16FG 4x5", WACOM_PKGLEN_BBPEN,    14720, 9200, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 16 };
static const struct wacom_features wacom_features_0xDF =
        { "Wacom Bamboo 16FG 6x8", WACOM_PKGLEN_BBPEN,    21648, 13700, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 16 };
static const struct wacom_features wacom_features_0x300 =
	{ "Wacom Bamboo One S",    WACOM_PKGLEN_BBPEN,    14720, 9225, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x301 =
	{ "Wacom Bamboo One M",    WACOM_PKGLEN_BBPEN,    21648, 13530, 1023, 31,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x302 =
	{ "Wacom Intuos PT S",     WACOM_PKGLEN_BBPEN,    15200, 9500, 1023, 31,
	  INTUOSHT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 16 };
static const struct wacom_features wacom_features_0x303 =
	{ "Wacom Intuos PT M",     WACOM_PKGLEN_BBPEN,    21600, 13500, 1023, 31,
	  INTUOSHT, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 16 };
static const struct wacom_features wacom_features_0x30E =
	{ "Wacom Intuos S",        WACOM_PKGLEN_BBPEN,    15200, 9500, 1023, 31,
	  INTUOSHT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x6004 =
	{ "ISD-V4",               WACOM_PKGLEN_GRAPHIRE,  12800, 8000, 255, 0,
	  TABLETPC, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x307 =
	{ "Wacom ISDv5 307", WACOM_PKGLEN_INTUOS,  59552, 33848, 2047, 63,
	  CINTIQ_HYBRID, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x309 };
static const struct wacom_features wacom_features_0x309 =
	{ "Wacom ISDv5 309", .type = WACOM_24HDT, /* Touch */
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x0307, .touch_max = 10 };
static const struct wacom_features wacom_features_0x30A =
	{ "Wacom ISDv5 30A", WACOM_PKGLEN_INTUOS, 59552, 33848, 2047, 63,
	  CINTIQ_HYBRID, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x30C };
static const struct wacom_features wacom_features_0x30C =
	{ "Wacom ISDv5 30C", .type = WACOM_24HDT, /* Touch */
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x30A, .touch_max = 10 };
static const struct wacom_features wacom_features_0x323 =
	{ "Wacom Intuos P M", WACOM_PKGLEN_BBPEN,    21600, 13500, 1023, 31,
	  INTUOSHT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x325 =
	{ "Wacom ISDv5 325", WACOM_PKGLEN_INTUOS,    59552, 33848, 2047, 63,
	  CINTIQ_COMPANION_2, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 11,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x326 };
static const struct wacom_features wacom_features_0x326 = /* Touch */
	{ "Wacom ISDv5 326", .type = WACOM_24HDT, .oVid = USB_VENDOR_ID_WACOM,
	  .oPid = 0x325, .touch_max = 10 };
static const struct wacom_features wacom_features_0x331 =
	{ "Wacom Express Key Remote", WACOM_PKGLEN_WIRELESS, .type = REMOTE,
	  .numbered_buttons = 18 };
static const struct wacom_features wacom_features_0x33B =
	{ "Wacom Intuos S 2", WACOM_PKGLEN_INTUOS, 15200, 9500, 2047, 63,
	  INTUOSHT2, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x33C =
	{ "Wacom Intuos PT S 2", WACOM_PKGLEN_INTUOS, 15200, 9500, 2047, 63,
	  INTUOSHT2, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 16 };
static const struct wacom_features wacom_features_0x33D =
	{ "Wacom Intuos P M 2",  WACOM_PKGLEN_INTUOS, 21600, 13500, 2047, 63,
	  INTUOSHT2, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x33E =
	{ "Wacom Intuos PT M 2", WACOM_PKGLEN_INTUOS, 21600, 13500, 2047, 63,
	  INTUOSHT2, WACOM_INTUOS_RES, WACOM_INTUOS_RES, .touch_max = 16 };
static const struct wacom_features wacom_features_0x343 =
	{ "Wacom DTK1651", WACOM_PKGLEN_DTUS, 34816, 19759, 1023, 0,
	  DTUS, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x34A =
	{ "Wacom MobileStudio Pro 13 Touch", WACOM_PKGLEN_MSPROT, .type = WACOM_MSPROT, /* Touch */
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34D };
static const struct wacom_features wacom_features_0x34B =
	{ "Wacom MobileStudio Pro 16 Touch", WACOM_PKGLEN_MSPROT, .type = WACOM_MSPROT, /* Touch */
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34E };
static const struct wacom_features wacom_features_0x34D =
	{ "Wacom MobileStudio Pro 13", WACOM_PKGLEN_MSPRO, 59552, 33848, 8191, 63,
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 11,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34A };
static const struct wacom_features wacom_features_0x34E =
	{ "Wacom MobileStudio Pro 16", WACOM_PKGLEN_MSPRO, 69920, 39680, 8191, 63,
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 13,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34B };
static const struct wacom_features wacom_features_0x34F =
	{ "Wacom Cintiq Pro 13 FHD", WACOM_PKGLEN_MSPRO, 59552, 33848, 8191, 63,
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x353 };
static const struct wacom_features wacom_features_0x350 =
	{ "Wacom Cintiq Pro 16UHD", WACOM_PKGLEN_MSPRO, 69920, 39680, 8191, 63,
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x354 };
static const struct wacom_features wacom_features_0x351 =
	{ "Wacom Cintiq Pro 24", WACOM_PKGLEN_MSPRO, 105286, 59574, 8191, 63, /* Pen & Touch */
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x355 };
static const struct wacom_features wacom_features_0x352 =
	{ "Wacom Cintiq Pro 32", WACOM_PKGLEN_MSPRO, 140384, 79316, 8191, 63,
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x356 };
static const struct wacom_features wacom_features_0x353 =
	{ "Wacom Cintiq Pro 13FHD Touch", WACOM_PKGLEN_MSPROT, .type = WACOM_MSPROT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x34f }; /* Touch */
static const struct wacom_features wacom_features_0x354 =
	{ "Wacom Cintiq Pro 16UHD Touch", WACOM_PKGLEN_MSPROT, .type = WACOM_MSPROT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x350 }; /* Touch */
static const struct wacom_features wacom_features_0x355 =
	{ "Wacom Cintiq Pro 24 Touch", WACOM_PKGLEN_27QHDT, .type = WACOM_27QHDT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x351, .touch_max = 10 }; /* Touch */
static const struct wacom_features wacom_features_0x356 =
	{ "Wacom Cintiq Pro 32 Touch", WACOM_PKGLEN_27QHDT, .type = WACOM_27QHDT,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x352, .touch_max = 10 }; /* Touch */
static const struct wacom_features wacom_features_0x357 =
	{ "Wacom Intuos Pro M", WACOM_PKGLEN_INTUOSP2, 44800, 29600, 8191, 63,
	  INTUOSP2, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9, .touch_max = 10 };
static const struct wacom_features wacom_features_0x358 =
	{ "Wacom Intuos Pro L", WACOM_PKGLEN_INTUOSP2, 62200, 43200, 8191, 63,
	  INTUOSP2, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 9, .touch_max = 10 };
static const struct wacom_features wacom_features_0x359 =
	{ "Wacom DTU-1141B", WACOM_PKGLEN_DTH1152, 22320, 12555, 1023, 0,
	  DTUS2, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4 };
static const struct wacom_features wacom_features_0x35A =
	{ "Wacom DTH-1152", WACOM_PKGLEN_DTH1152, 22320, 12555, 1023, 0,
	  DTH1152, WACOM_INTUOS_RES, WACOM_INTUOS_RES,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x368 };
static const struct wacom_features wacom_features_0x368 =
	{ "Wacom DTH-1152 Touch", WACOM_PKGLEN_27QHDT,
	  .type = DTH1152T, .touch_max = 10, .oVid = USB_VENDOR_ID_WACOM,
	  .oPid = 0x35A }; /* Touch */
static const struct wacom_features wacom_features_0x374 =
	{ "Intuos S", WACOM_PKGLEN_INTUOSP2, 15200, 9500, 4095,
	  63, INTUOSHT3, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4 };
static const struct wacom_features wacom_features_0x375 =
	{ "Intuos M", WACOM_PKGLEN_INTUOSP2, 21600, 13500, 4095,
	  63, INTUOSHT3, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4 };
static const struct wacom_features wacom_features_0x376 =
	{ "Intuos BT S", WACOM_PKGLEN_INTUOSP2, 15200, 9500, 4095,
	  63, INTUOSHT3, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4 };
static const struct wacom_features wacom_features_0x378 =
	{ "Intuos BT M", WACOM_PKGLEN_INTUOSP2, 21600, 13500, 4095,
	  63, INTUOSHT3, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4 };
static const struct wacom_features wacom_features_0x37A =
	{ "Wacom One by Wacom S", WACOM_PKGLEN_BBPEN, 15200, 9500, 2047, 63,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x37B =
	{ "Wacom One by Wacom M", WACOM_PKGLEN_BBPEN, 21600, 13500, 2047, 63,
	  BAMBOO_PT, WACOM_INTUOS_RES, WACOM_INTUOS_RES };
static const struct wacom_features wacom_features_0x37C =
	{ "Wacom Cintiq Pro 24", WACOM_PKGLEN_MSPRO, 105286, 59574, 8191, 63, /* Pen-only */
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x37D =
	{ "Wacom DTH-2452", WACOM_PKGLEN_DTH1152, 53104, 30046, 2047,
	  0, DTK2451, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x37E };
static const struct wacom_features wacom_features_0x37E =
	{ "Wacom DTH-2452 Touch", WACOM_PKGLEN_MSPROT,
	  .type = DTH2452T, .touch_max = 10, .oVid = USB_VENDOR_ID_WACOM,
	  .oPid = 0x37D }; /* Touch */
static const struct wacom_features wacom_features_0x382 =
	{ "Wacom DTK-2451", WACOM_PKGLEN_DTH1152, 53104, 30046, 2047,
	  0, DTK2451, WACOM_INTUOS_RES, WACOM_INTUOS_RES, 4,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET,
	  WACOM_DTU_OFFSET, WACOM_DTU_OFFSET };
static const struct wacom_features wacom_features_0x390 =
	{ "Wacom Cintiq 16", WACOM_PKGLEN_MSPRO, 69632, 39518, 8191, 63,
	  CINTIQ_16, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x391 =
	{ "Wacom Cintiq 22", WACOM_PKGLEN_MSPRO, 96012, 54358, 8191, 63,
	  CINTIQ_16, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x392 =
	{ "Wacom Intuos Pro S", WACOM_PKGLEN_INTUOSP2, 31920, 19950, 8191, 63,
	   INTUOSP2S, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 7, .touch_max = 10 };
static const struct wacom_features wacom_features_0x396 =
	{ "Wacom DTK-1660E", WACOM_PKGLEN_MSPRO, 69632, 39518, 8191, 63,
	  CINTIQ_16, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 0,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET };
static const struct wacom_features wacom_features_0x398 =
	{ "Wacom MobileStudio Pro 13", WACOM_PKGLEN_MSPRO, 59552, 33848, 8191, 63,
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 11,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x39A };
static const struct wacom_features wacom_features_0x399 =
	{ "Wacom MobileStudio Pro 16", WACOM_PKGLEN_MSPRO, 69920, 39680, 8191, 63,
	  WACOM_MSPRO, WACOM_INTUOS3_RES, WACOM_INTUOS3_RES, 13,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  WACOM_CINTIQ_OFFSET, WACOM_CINTIQ_OFFSET,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x39B };
static const struct wacom_features wacom_features_0x39A =
	{ "Wacom MobileStudio Pro 13 Touch", WACOM_PKGLEN_MSPROT, /* Touch */
	  .type = WACOM_MSPROT, .touch_max = 10,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x398 };
static const struct wacom_features wacom_features_0x39B =
	{ "Wacom MobileStudio Pro 16 Touch", WACOM_PKGLEN_MSPROT, /* Touch */
	  .type = WACOM_MSPROT, .touch_max = 10,
	  .oVid = USB_VENDOR_ID_WACOM, .oPid = 0x399 };

#define USB_DEVICE_WACOM(prod)					\
	USB_DEVICE(USB_VENDOR_ID_WACOM, prod),			\
	.driver_info = (kernel_ulong_t)&wacom_features_##prod

#define USB_DEVICE_DETAILED(prod, class, sub, proto)			\
	USB_DEVICE_AND_INTERFACE_INFO(USB_VENDOR_ID_WACOM, prod, class,	\
				      sub, proto),			\
	.driver_info = (kernel_ulong_t)&wacom_features_##prod

#define USB_DEVICE_LENOVO(prod)					\
	USB_DEVICE(USB_VENDOR_ID_LENOVO, prod),			\
	.driver_info = (kernel_ulong_t)&wacom_features_##prod

const struct usb_device_id wacom_ids[] = {
	{ USB_DEVICE_WACOM(0x00) },
	{ USB_DEVICE_WACOM(0x03) },
	{ USB_DEVICE_WACOM(0x10) },
	{ USB_DEVICE_WACOM(0x11) },
	{ USB_DEVICE_WACOM(0x12) },
	{ USB_DEVICE_WACOM(0x13) },
	{ USB_DEVICE_WACOM(0x14) },
	{ USB_DEVICE_WACOM(0x15) },
	{ USB_DEVICE_WACOM(0x16) },
	{ USB_DEVICE_WACOM(0x17) },
	{ USB_DEVICE_WACOM(0x18) },
	{ USB_DEVICE_WACOM(0x19) },
	{ USB_DEVICE_WACOM(0x20) },
	{ USB_DEVICE_WACOM(0x21) },
	{ USB_DEVICE_WACOM(0x22) },
	{ USB_DEVICE_WACOM(0x23) },
	{ USB_DEVICE_WACOM(0x24) },
	{ USB_DEVICE_WACOM(0x26) },
	{ USB_DEVICE_WACOM(0x27) },
	{ USB_DEVICE_WACOM(0x28) },
	{ USB_DEVICE_WACOM(0x29) },
	{ USB_DEVICE_WACOM(0x2A) },
	{ USB_DEVICE_WACOM(0x30) },
	{ USB_DEVICE_WACOM(0x31) },
	{ USB_DEVICE_WACOM(0x32) },
	{ USB_DEVICE_WACOM(0x33) },
	{ USB_DEVICE_WACOM(0x34) },
	{ USB_DEVICE_WACOM(0x35) },
	{ USB_DEVICE_WACOM(0x37) },
	{ USB_DEVICE_WACOM(0x38) },
	{ USB_DEVICE_WACOM(0x39) },
	{ USB_DEVICE_WACOM(0x3F) },
	{ USB_DEVICE_WACOM(0x41) },
	{ USB_DEVICE_WACOM(0x42) },
	{ USB_DEVICE_WACOM(0x43) },
	{ USB_DEVICE_WACOM(0x44) },
	{ USB_DEVICE_WACOM(0x45) },
	{ USB_DEVICE_WACOM(0x47) },
	{ USB_DEVICE_WACOM(0x57) },
	{ USB_DEVICE_WACOM(0x59) },
	{ USB_DEVICE_WACOM(0x5B) },
	{ USB_DEVICE_DETAILED(0x5D, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x5E, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x60) },
	{ USB_DEVICE_WACOM(0x61) },
	{ USB_DEVICE_WACOM(0x62) },
	{ USB_DEVICE_WACOM(0x63) },
	{ USB_DEVICE_WACOM(0x64) },
	{ USB_DEVICE_WACOM(0x65) },
	{ USB_DEVICE_WACOM(0x69) },
	{ USB_DEVICE_WACOM(0x6A) },
	{ USB_DEVICE_WACOM(0x6B) },
	{ USB_DEVICE_WACOM(0x84) },
	{ USB_DEVICE_WACOM(0x90) },
	{ USB_DEVICE_WACOM(0x93) },
	{ USB_DEVICE_WACOM(0x97) },
	{ USB_DEVICE_WACOM(0x9A) },
	{ USB_DEVICE_WACOM(0x9F) },
	{ USB_DEVICE_WACOM(0xB0) },
	{ USB_DEVICE_WACOM(0xB1) },
	{ USB_DEVICE_WACOM(0xB2) },
	{ USB_DEVICE_WACOM(0xB3) },
	{ USB_DEVICE_WACOM(0xB4) },
	{ USB_DEVICE_WACOM(0xB5) },
	{ USB_DEVICE_WACOM(0xB7) },
	{ USB_DEVICE_WACOM(0xB8) },
	{ USB_DEVICE_WACOM(0xB9) },
	{ USB_DEVICE_WACOM(0xBA) },
	{ USB_DEVICE_WACOM(0xBB) },
	{ USB_DEVICE_WACOM(0xBC) },
	{ USB_DEVICE_WACOM(0xC0) },
	{ USB_DEVICE_WACOM(0xC2) },
	{ USB_DEVICE_WACOM(0xC4) },
	{ USB_DEVICE_WACOM(0xC5) },
	{ USB_DEVICE_WACOM(0xC6) },
	{ USB_DEVICE_WACOM(0xC7) },
	{ USB_DEVICE_WACOM(0xCC) },
	/*
	 * DTU-2231 has two interfaces on the same configuration,
	 * only one is used.
	 */
	{ USB_DEVICE_DETAILED(0xCE, USB_CLASS_HID,
			      USB_INTERFACE_SUBCLASS_BOOT,
			      USB_INTERFACE_PROTOCOL_MOUSE) },
	{ USB_DEVICE_WACOM(0xD0) },
	{ USB_DEVICE_WACOM(0xD1) },
	{ USB_DEVICE_WACOM(0xD2) },
	{ USB_DEVICE_WACOM(0xD3) },
	{ USB_DEVICE_WACOM(0xD4) },
	{ USB_DEVICE_WACOM(0xD5) },
	{ USB_DEVICE_WACOM(0xD6) },
	{ USB_DEVICE_WACOM(0xD7) },
	{ USB_DEVICE_WACOM(0xD8) },
	{ USB_DEVICE_WACOM(0xDA) },
	{ USB_DEVICE_WACOM(0xDB) },
	{ USB_DEVICE_WACOM(0xDD) },
	{ USB_DEVICE_WACOM(0xDE) },
	{ USB_DEVICE_WACOM(0xDF) },
	{ USB_DEVICE_WACOM(0xE2) },
	{ USB_DEVICE_WACOM(0xE3) },
	{ USB_DEVICE_WACOM(0xE5) },
	{ USB_DEVICE_WACOM(0xE6) },
	{ USB_DEVICE_WACOM(0xEC) },
	{ USB_DEVICE_WACOM(0xED) },
	{ USB_DEVICE_WACOM(0xEF) },
	{ USB_DEVICE_WACOM(0xF0) },
	{ USB_DEVICE_WACOM(0xF4) },
	{ USB_DEVICE_DETAILED(0xF6, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0xF8) },
	{ USB_DEVICE_WACOM(0xFA) },
	{ USB_DEVICE_WACOM(0xFB) },
	{ USB_DEVICE_WACOM(0x100) },
	{ USB_DEVICE_WACOM(0x101) },
	{ USB_DEVICE_WACOM(0x10D) },
	{ USB_DEVICE_WACOM(0x10E) },
	{ USB_DEVICE_WACOM(0x10F) },
	{ USB_DEVICE_WACOM(0x116) },
	{ USB_DEVICE_WACOM(0x12C) },
	{ USB_DEVICE_WACOM(0x300) },
	{ USB_DEVICE_WACOM(0x301) },
	{ USB_DEVICE_DETAILED(0x302, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x303, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x304) },
	{ USB_DEVICE_WACOM(0x307) },
	{ USB_DEVICE_DETAILED(0x309, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x30A) },
	{ USB_DEVICE_WACOM(0x30C) },
	{ USB_DEVICE_DETAILED(0x30E, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x314, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x315, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x317, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x323, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x325) },
	{ USB_DEVICE_WACOM(0x326) },
	{ USB_DEVICE_WACOM(0x32A) },
	{ USB_DEVICE_WACOM(0x32B) },
	{ USB_DEVICE_WACOM(0x32C) },
	{ USB_DEVICE_WACOM(0x32F) },
	{ USB_DEVICE_DETAILED(0x331, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x333) },
	{ USB_DEVICE_WACOM(0x335) },
	{ USB_DEVICE_WACOM(0x336) },
	{ USB_DEVICE_DETAILED(0x33B, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x33C, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x33D, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x33E, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x343) },
	{ USB_DEVICE_WACOM(0x34A) },
	{ USB_DEVICE_WACOM(0x34B) },
	{ USB_DEVICE_WACOM(0x34D) },
	{ USB_DEVICE_WACOM(0x34E) },
	{ USB_DEVICE_WACOM(0x34F) },
	{ USB_DEVICE_WACOM(0x350) },
	{ USB_DEVICE_WACOM(0x351) },
	{ USB_DEVICE_WACOM(0x352) },
	{ USB_DEVICE_WACOM(0x353) },
	{ USB_DEVICE_WACOM(0x354) },
	{ USB_DEVICE_WACOM(0x355) },
	{ USB_DEVICE_WACOM(0x356) },
	{ USB_DEVICE_DETAILED(0x357, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x358, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x359) },
	{ USB_DEVICE_WACOM(0x35A) },
	{ USB_DEVICE_WACOM(0x368) },
	{ USB_DEVICE_WACOM(0x374) },
	{ USB_DEVICE_WACOM(0x375) },
	{ USB_DEVICE_WACOM(0x376) },
	{ USB_DEVICE_WACOM(0x378) },
	{ USB_DEVICE_WACOM(0x37A) },
	{ USB_DEVICE_WACOM(0x37B) },
	{ USB_DEVICE_WACOM(0x37C) },
	{ USB_DEVICE_WACOM(0x37D) },
	{ USB_DEVICE_WACOM(0x37E) },
	{ USB_DEVICE_WACOM(0x382) },
	{ USB_DEVICE_DETAILED(0x390, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x391, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x392, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_DETAILED(0x396, USB_CLASS_HID, 0, 0) },
	{ USB_DEVICE_WACOM(0x398) },
	{ USB_DEVICE_WACOM(0x399) },
	{ USB_DEVICE_WACOM(0x39A) },
	{ USB_DEVICE_WACOM(0x39B) },
	{ USB_DEVICE_WACOM(0x4001) },
	{ USB_DEVICE_WACOM(0x4004) },
	{ USB_DEVICE_WACOM(0x5000) },
	{ USB_DEVICE_WACOM(0x5002) },
	{ USB_DEVICE_WACOM(0x5010) },
	{ USB_DEVICE_WACOM(0x5013) },
	{ USB_DEVICE_WACOM(0x5044) },
	{ USB_DEVICE_WACOM(0x5048) },
	{ USB_DEVICE_WACOM(0x5090) },
	{ USB_DEVICE_LENOVO(0x6004) },
	{ }
};
MODULE_DEVICE_TABLE(usb, wacom_ids);
