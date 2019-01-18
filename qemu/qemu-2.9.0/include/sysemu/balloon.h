/*
 * Balloon
 *
 * Copyright IBM, Corp. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_BALLOON_H
#define QEMU_BALLOON_H

#include "qapi-types.h"

typedef void (QEMUBalloonEvent)(void *opaque, ram_addr_t target);

/* Added by Bhavesh Singh. 2017.06.02. Begin add */
typedef void (QEMUSSDBalloonEvent)(void *opaque, int64_t target);
int qemu_add_ssd_balloon_handler(QEMUSSDBalloonEvent *event_func, void *opaque);
void qemu_remove_ssd_balloon_handler(void *opaque);
/* Added by Bhavesh Singh. 2017.06.02. End add */

typedef void (QEMUBalloonStatus)(void *opaque, BalloonInfo *info);

int qemu_add_balloon_handler(QEMUBalloonEvent *event_func,
			     QEMUBalloonStatus *stat_func, void *opaque);
void qemu_remove_balloon_handler(void *opaque);
bool qemu_balloon_is_inhibited(void);
void qemu_balloon_inhibit(bool state);

#endif
