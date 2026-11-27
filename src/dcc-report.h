// SPDX-License-Identifier: GPL-2.0-only
/*
 * dcc-report.h
 * System report (1.45.0): what Dudes Control Center detected on this
 * computer, for bug reports from laptops it was not tested on.
 * Used by dcc-fan-cli --report and the GUI's Help tab.
 *
 * The report contains no user names, host names, serial numbers or home
 * folder paths: DMI serials and UUIDs are never read, and the user name,
 * home folder and host name are replaced in the finished text.
 */

#ifndef DUDES_CONTROL_CENTER_REPORT_H
#define DUDES_CONTROL_CENTER_REPORT_H

/* Root prefix for /sys, /proc and /etc (tests point it at a fake tree) */
#ifndef DCC_SYSROOT
#define DCC_SYSROOT ""
#endif

/* The report as text (malloc'd, the caller frees it); NULL if out of memory */
char *dcc_report_build(const char *version);

/* Replaces the user name, home folder and host name in text (in place
 * where possible; returns text or a new malloc'd string, freeing text) */
char *dcc_report_scrub(char *text, const char *user, const char *home, const char *host);

#endif /* DUDES_CONTROL_CENTER_REPORT_H */
