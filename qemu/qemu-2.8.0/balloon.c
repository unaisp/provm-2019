/*
 * Generic Balloon handlers and management
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (C) 2011 Red Hat, Inc.
 * Copyright (C) 2011 Amit Shah <amit.shah@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "exec/cpu-common.h"
#include "sysemu/kvm.h"
#include "sysemu/balloon.h"
#include "trace.h"
#include "qmp-commands.h"
#include "qapi/qmp/qerror.h"
#include "qapi/qmp/qjson.h"

static QEMUBalloonEvent *balloon_event_fn;
/* Added by Bhavesh Singh. 2017.06.02. Begin add */
static QEMUSSDBalloonEvent *ssd_balloon_event_fn;
static void *ssd_balloon_opaque;
/* Added by Bhavesh Singh. 2017.06.02. End add */
static QEMUBalloonStatus *balloon_stat_fn;
static void *balloon_opaque;
static bool balloon_inhibited;

bool qemu_balloon_is_inhibited(void)
{
    return balloon_inhibited;
}

void qemu_balloon_inhibit(bool state)
{
    balloon_inhibited = state;
}

static bool have_balloon(Error **errp)
{
    if (kvm_enabled() && !kvm_has_sync_mmu()) {
        error_set(errp, ERROR_CLASS_KVM_MISSING_CAP,
                  "Using KVM without synchronous MMU, balloon unavailable");
        return false;
    }
    if (!balloon_event_fn) {
        error_set(errp, ERROR_CLASS_DEVICE_NOT_ACTIVE,
                  "No balloon device has been activated");
        return false;
    }
    return true;
}

int qemu_add_balloon_handler(QEMUBalloonEvent *event_func,
                             QEMUBalloonStatus *stat_func, void *opaque)
{
    if (balloon_event_fn || balloon_stat_fn || balloon_opaque) {
        /* We're already registered one balloon handler.  How many can
         * a guest really have?
         */
        return -1;
    }
    balloon_event_fn = event_func;
    balloon_stat_fn = stat_func;
    balloon_opaque = opaque;
    return 0;
}

void qemu_remove_balloon_handler(void *opaque)
{
    if (balloon_opaque != opaque) {
        return;
    }
    balloon_event_fn = NULL;
    balloon_stat_fn = NULL;
    balloon_opaque = NULL;
}

BalloonInfo *qmp_query_balloon(Error **errp)
{
    BalloonInfo *info;

    if (!have_balloon(errp)) {
        return NULL;
    }

    info = g_malloc0(sizeof(*info));
    balloon_stat_fn(balloon_opaque, info);
    return info;
}

void qmp_balloon(int64_t target, Error **errp)
{
    if (!have_balloon(errp)) {
        return;
    }

    if (target <= 0) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "target", "a size");
        return;
    }

    trace_balloon_event(balloon_opaque, target);
    balloon_event_fn(balloon_opaque, target);
}


/* Added by Bhavesh Singh. 2017.06.02. Begin add */
void qmp_ssd_balloon(int64_t target, Error **errp)
{
//    if (!have_balloon(errp)) {
//        return;
//    }
    if(!ssd_balloon_event_fn)
        return;
//    if (target <= 0) {
//        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "target", "a size");
//        return;
//    }

//    trace_balloon_event(balloon_opaque, target);
    ssd_balloon_event_fn(ssd_balloon_opaque, target);
}
int qemu_add_ssd_balloon_handler(QEMUSSDBalloonEvent *event_func, void *opaque)
{
    if (ssd_balloon_event_fn || ssd_balloon_opaque) {
        /* We're already registered one balloon handler.  How many can
         * a guest really have?
         */
        return -1;
    }
    ssd_balloon_event_fn = event_func;
    //balloon_stat_fn = stat_func;
   ssd_balloon_opaque = opaque;
    return 0;
}

void qemu_remove_ssd_balloon_handler(void *opaque)
{
    if (ssd_balloon_opaque != opaque) {
        return;
    }
    ssd_balloon_event_fn = NULL;
    //balloon_stat_fn = NULL;
    ssd_balloon_opaque = NULL;
}
/* Added by Bhavesh Singh. 2017.06.02. End add */
