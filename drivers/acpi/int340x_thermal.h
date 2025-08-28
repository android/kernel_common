/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * The ACPI INT340X device IDs are shared between the DPTF core
 * and thermal drivers.
 */

#ifndef _ACPI_INT340X_H_
#define _ACPI_INT340X_H_

#define ACPI_INT3400_DEVICE_IDS	\
	{"INT3400"},	\
	{"INTC1040"},	\
	{"INTC1041"},	\
	{"INTC1042"},	\
	{"INTC1068"},	\
	{"INTC10A0"},	\
	{"INTC10D4"}

#define ACPI_INT3401_DEVICE_IDS	\
	{"INT3401"}

#define ACPI_INT3402_DEVICE_IDS	\
	{"INT3402"}

#define ACPI_INT3403_DEVICE_IDS	\
	{"INT3403"},	\
	{"INTC1043"},	\
	{"INTC1046"},	\
	{"INTC1062"},	\
	{"INTC1069"},	\
	{"INTC10A1"},	\
	{"INTC10D5"}

#define ACPI_INT3404_DEVICE_IDS	\
	{"INT3404", }, /* Fan */ \
	{"INTC1044", }, /* Fan for Tiger Lake generation */ \
	{"INTC1048", }, /* Fan for Alder Lake generation */ \
	{"INTC1063", }, /* Fan for Meteor Lake generation */ \
	{"INTC106A", }, /* Fan for Lunar Lake generation */ \
	{"INTC10A2", }, /* Fan for Raptor Lake generation */ \
	{"INTC10D6", } /* Fan for Panther Lake generation */

#define ACPI_INT3406_DEVICE_IDS	\
	{"INT3406"}

#define ACPI_INT3407_DEVICE_IDS	\
	{"INT3407"},	\
	{"INT3532"},	\
	{"INTC1047"},	\
	{"INTC1050"},	\
	{"INTC1060"},	\
	{"INTC1061"},	\
	{"INTC1065"},	\
	{"INTC1066"},	\
	{"INTC106C"},	\
	{"INTC106D"},	\
	{"INTC10A4"},	\
	{"INTC10A5"},	\
	{"INTC10D8"},	\
	{"INTC10D9"}

#endif
