/*
 * drivers/input/tablet/wacom_wac.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef WACOM_WAC_H
#define WACOM_WAC_H

#include <linux/types.h>

/* maximum packet length for USB devices */
#define WACOM_PKGLEN_MAX	192

#define WACOM_NAME_MAX		64

/* packet length for individual models */
#define WACOM_PKGLEN_PENPRTN	 7
#define WACOM_PKGLEN_GRAPHIRE	 8
#define WACOM_PKGLEN_BBFUN	 9
#define WACOM_PKGLEN_INTUOS	10
#define WACOM_PKGLEN_TPC1FG	 5
#define WACOM_PKGLEN_TPC1FG_B	10
#define WACOM_PKGLEN_TPC2FG	14
#define WACOM_PKGLEN_BBTOUCH	20
#define WACOM_PKGLEN_MTOUCH	62
#define WACOM_PKGLEN_BBTOUCH3	64
#define WACOM_PKGLEN_BBPEN	10
#define WACOM_PKGLEN_MTTPC	40
#define WACOM_PKGLEN_DTUS	68
#define WACOM_PKGLEN_PENABLED	 8
#define WACOM_PKGLEN_27QHDT	64
#define WACOM_PKGLEN_MSPRO	64
#define WACOM_PKGLEN_MSPROT	50
#define WACOM_PKGLEN_INTUOSP2	64
#define WACOM_PKGLEN_INTUOSP2T	44
#define WACOM_PKGLEN_DTH1152	12

/* wacom data size per MT contact */
#define WACOM_BYTES_PER_MT_PACKET	11
#define WACOM_BYTES_PER_24HDT_PACKET	14
#define WACOM_BYTES_PER_QHDTHID_PACKET	 6
#define WACOM_BYTES_PER_MSPROT_PACKET	 9
#define WACOM_BYTES_PER_INTUOSP2_PACKET  8

/* device IDs */
#define STYLUS_DEVICE_ID	0x02
#define TOUCH_DEVICE_ID		0x03
#define CURSOR_DEVICE_ID	0x06
#define ERASER_DEVICE_ID	0x0A
#define PAD_DEVICE_ID		0x0F

/* wacom data packet report IDs */
#define WACOM_REPORT_PENABLED		2
#define WACOM_REPORT_INTUOS_ID1		5
#define WACOM_REPORT_INTUOS_ID2		6
#define WACOM_REPORT_INTUOSPAD		12
#define WACOM_REPORT_INTUOS5PAD		3
#define WACOM_REPORT_DTUSPAD		21
#define WACOM_REPORT_TPC1FG		6
#define WACOM_REPORT_TPC2FG		13
#define WACOM_REPORT_TPCMT2		3
#define WACOM_REPORT_CINTIQ		16
#define WACOM_REPORT_MSPRO		16
#define WACOM_REPORT_INTUOS_PEN		16
#define WACOM_REPORT_CINTIQPAD		17
#define WACOM_REPORT_DTUS		17
#define WACOM_REPORT_MSPROPAD		17
#define WACOM_REPORT_MSPRODEVICE	19
#define WACOM_REPORT_VENDOR_DEF_TOUCH	33
#define WAC_CMD_LED_CONTROL_GENERIC	50

/* device quirks */
#define WACOM_QUIRK_BBTOUCH_LOWRES	0x0001
#define WACOM_QUIRK_NO_INPUT		0x0002
#define WACOM_QUIRK_MONITOR		0x0004

#ifndef BTN_STYLUS3
#define BTN_STYLUS3                     0x149
#endif

#define WACOM_INTUOSP2_RING_UNTOUCHED	0x7f
enum {
	PENPARTNER = 0,
	GRAPHIRE,
	WACOM_G4,
	PTU,
	PL,
	DTU,
	DTUS,
	DTUSX,
	DTH1152,
	INTUOS,
	INTUOS3S,
	INTUOS3,
	INTUOS3L,
	INTUOS4S,
	INTUOS4,
	INTUOS4L,
	INTUOS5S,
	INTUOS5,
	INTUOS5L,
	INTUOSPS,
	INTUOSPM,
	INTUOSPL,
	WACOM_21UX2,
	WACOM_22HD,
	WACOM_27QHD,
	DTK,
	WACOM_24HD,
	WACOM_MSPRO,
	CINTIQ,
	WACOM_BEE,
	WACOM_13HD,
	WACOM_MO,
	WIRELESS,
	INTUOSHT,
	INTUOSHT2,
	BAMBOO_PT,
	WACOM_24HDT,
	WACOM_27QHDT,
	WACOM_MSPROT,
	DTH1152T,
	INTUOSP2,
	TABLETPC,
	TABLETPC2FG,
	MTTPC,
	MTTPC_B,
	MTTPC_C,
	MAX_TYPE
};

struct wacom_features {
	const char *name;
	int pktlen;
	int x_max;
	int y_max;
	int pressure_max;
	int distance_max;
	int type;
	int numbered_buttons;
	int offset_left;
	int offset_right;
	int offset_top;
	int offset_bottom;
	int device_type;
	int x_phy;
	int y_phy;
	unsigned char unit;
	unsigned char unitExpo;
	int x_fuzz;
	int y_fuzz;
	int pressure_fuzz;
	int distance_fuzz;
	int tilt_fuzz;
	unsigned quirks;
	unsigned touch_max;
	int oVid;
	int oPid;
};

struct wacom_shared {
	bool stylus_in_proximity;
	bool touch_down;
};

struct wacom_wac {
	char name[WACOM_NAME_MAX];
	unsigned char *data;
	int tool[3];
	int id[3];
	__u32 serial[2];
	bool reporting_data;
	int last_finger;
	struct wacom_features features;
	struct wacom_shared *shared;
	struct input_dev *input;
	int num_contacts_left;
	int contacts_to_send;
	int slots[10];
	int previous_buttons;
	int previous_ring;
	int previous_keys;
};

#endif
