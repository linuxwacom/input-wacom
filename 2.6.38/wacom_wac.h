/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * drivers/input/tablet/wacom_wac.h
 */
#ifndef WACOM_WAC_H
#define WACOM_WAC_H

#include <linux/types.h>

/* maximum packet length for USB devices */
#define WACOM_PKGLEN_MAX	192

#define WACOM_NAME_MAX		64
#define WACOM_MAX_REMOTES	5
#define WACOM_STATUS_UNKNOWN	255

/* packet length for individual models */
#define WACOM_PKGLEN_PENPRTN	 7
#define WACOM_PKGLEN_GRAPHIRE	 8
#define WACOM_PKGLEN_BBFUN	 9
#define WACOM_PKGLEN_INTUOS	10
#define WACOM_PKGLEN_TPC1FG	 5
#define WACOM_PKGLEN_TPC1FG_B	10
#define WACOM_PKGLEN_TPC2FG	14
#define WACOM_PKGLEN_BBTOUCH	20
#define WACOM_PKGLEN_BBTOUCH3	64
#define WACOM_PKGLEN_BBPEN	10
#define WACOM_PKGLEN_WIRELESS	32
#define WACOM_PKGLEN_MTOUCH	62
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
#define WACOM_REPORT_TPCMT		13
#define WACOM_REPORT_TPCMT2		3
#define WACOM_REPORT_TPCHID		15
#define WACOM_REPORT_TPCST		16
#define WACOM_REPORT_CINTIQ		16
#define WACOM_REPORT_MSPRO		16
#define WACOM_REPORT_INTUOS_PEN		16
#define WACOM_REPORT_CINTIQPAD		17
#define WACOM_REPORT_DTUS		17
#define WACOM_REPORT_MSPROPAD		17
#define WACOM_REPORT_TPC1FGE		18
#define WACOM_REPORT_MSPRODEVICE	19
#define WACOM_REPORT_DTK2451PAD		21
#define WACOM_REPORT_24HDT		1
#define WACOM_REPORT_WL			128
#define WACOM_REPORT_USB		192
#define WACOM_REPORT_DEVICE_LIST	16
#define WACOM_REPORT_REMOTE		17
#define WACOM_REPORT_VENDOR_DEF_TOUCH	33
#define WAC_CMD_LED_CONTROL_GENERIC	50

/* device quirks */
#define WACOM_QUIRK_BBTOUCH_LOWRES	0x0001
#define WACOM_QUIRK_NO_INPUT		0x0002
#define WACOM_QUIRK_MONITOR		0x0004
#define WACOM_QUIRK_BATTERY		0x0008

#ifndef BTN_STYLUS3
#define BTN_STYLUS3                     0x149
#endif

#define WACOM_INTUOSP2_RING_UNTOUCHED	0x7f
#define WACOM_POWER_SUPPLY_STATUS_AUTO  -1
enum {
	PENPARTNER = 0,
	GRAPHIRE,
	WACOM_G4,
	PTU,
	PL,
	DTU,
	DTUS,
	DTUS2,
	DTUSX,
	DTH1152,
	DTK2451,
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
	DTK,
	WACOM_24HD,
	WACOM_27QHD,
	CINTIQ_HYBRID,
	CINTIQ_COMPANION_2,
	WACOM_MSPRO,
	CINTIQ_16,
	WACOM_ONE,
	CINTIQ,
	WACOM_BEE,
	WACOM_13HD,
	WACOM_MO,
	INTUOSHT,
	INTUOSHT2,
	BAMBOO_PT,
	WACOM_24HDT,
	WACOM_27QHDT,
	WACOM_MSPROT,
	DTH1152T,
	INTUOSP2,
	INTUOSP2S,
	INTUOSHT3,
	WIRELESS,
	REMOTE,
	TABLETPC,   /* add new TPC below */
	TABLETPCE,
	TABLETPC2FG,
	DTH2452T,
	MTSCREEN,
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
	int x_resolution;
	int y_resolution;
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
	/* for wireless device to access USB interfaces */
	unsigned touch_max;
	int type;
	struct input_dev *touch_input;
	bool has_mute_touch_switch;
	bool is_touch_on;
};

struct wacom_remote_data {
	struct {
		u32 serial;
		bool connected;
	} remote[WACOM_MAX_REMOTES];
};

struct wacom_wac {
	char name[WACOM_NAME_MAX];
	unsigned char *data;
	int tool[2];
	int id[2];
	__u32 serial[2];
	bool reporting_data;
	struct wacom_features features;
	struct wacom_shared *shared;
	struct input_dev *input;
	int pid;
	int num_contacts_left;
	int *slots;
	int previous_buttons;
	int previous_ring;
	int previous_keys;
};

#endif
