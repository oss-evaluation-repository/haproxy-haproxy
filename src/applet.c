/*
 * Functions managing applets
 *
 * Copyright 2000-2015 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <haproxy/api.h>
#include <haproxy/applet.h>
#include <haproxy/channel.h>
#include <haproxy/list.h>
#include <haproxy/stream.h>
#include <haproxy/stream_interface.h>
#include <haproxy/task.h>

unsigned int nb_applets = 0;

DECLARE_POOL(pool_head_appctx,  "appctx",  sizeof(struct appctx));

/* Initializes all required fields for a new appctx. Note that it does the
 * minimum acceptable initialization for an appctx. This means only the
 * 3 integer states st0, st1, st2 and the chunk used to gather unfinished
 * commands are zeroed
 */
static inline void appctx_init(struct appctx *appctx)
{
	appctx->st0 = appctx->st1 = appctx->st2 = 0;
	appctx->chunk = NULL;
	appctx->io_release = NULL;
	appctx->call_rate.curr_tick = 0;
	appctx->call_rate.curr_ctr = 0;
	appctx->call_rate.prev_ctr = 0;
	appctx->state = 0;
	LIST_INIT(&appctx->wait_entry);
}

/* Tries to allocate a new appctx and initialize its main fields. The appctx
 * is returned on success, NULL on failure. The appctx must be released using
 * appctx_free(). <applet> is assigned as the applet, but it can be NULL. The
 * applet's task is always created on the current thread.
 */
struct appctx *appctx_new(struct applet *applet, struct cs_endpoint *endp)
{
	struct appctx *appctx;

	appctx = pool_alloc(pool_head_appctx);
	if (unlikely(!appctx))
		goto fail_appctx;

	appctx_init(appctx);
	appctx->obj_type = OBJ_TYPE_APPCTX;
	appctx->applet = applet;

	if (!endp) {
		endp = cs_endpoint_new();
		if (!endp)
			goto fail_endp;
		endp->target = appctx;
		endp->ctx = appctx;
		endp->flags |= (CS_EP_T_APPLET|CS_EP_ORPHAN);
	}
	appctx->endp = endp;

	appctx->t = task_new_here();
	if (unlikely(!appctx->t))
		goto fail_task;
	appctx->t->process = task_run_applet;
	appctx->t->context = appctx;

	LIST_INIT(&appctx->buffer_wait.list);
	appctx->buffer_wait.target = appctx;
	appctx->buffer_wait.wakeup_cb = appctx_buf_available;

	_HA_ATOMIC_INC(&nb_applets);
	return appctx;

  fail_task:
	cs_endpoint_free(appctx->endp);
  fail_endp:
	pool_free(pool_head_appctx, appctx);
  fail_appctx:
	return NULL;
}

/* Callback used to wake up an applet when a buffer is available. The applet
 * <appctx> is woken up if an input buffer was requested for the associated
 * stream interface. In this case the buffer is immediately allocated and the
 * function returns 1. Otherwise it returns 0. Note that this automatically
 * covers multiple wake-up attempts by ensuring that the same buffer will not
 * be accounted for multiple times.
 */
int appctx_buf_available(void *arg)
{
	struct appctx *appctx = arg;
	struct stream_interface *si = cs_si(appctx->owner);

	/* allocation requested ? */
	if (!(si->flags & SI_FL_RXBLK_BUFF))
		return 0;

	si_rx_buff_rdy(si);

	/* was already allocated another way ? if so, don't take this one */
	if (c_size(si_ic(si)) || si_ic(si)->pipe)
		return 0;

	/* allocation possible now ? */
	if (!b_alloc(&si_ic(si)->buf)) {
		si_rx_buff_blk(si);
		return 0;
	}

	task_wakeup(appctx->t, TASK_WOKEN_RES);
	return 1;
}

/* Default applet handler */
struct task *task_run_applet(struct task *t, void *context, unsigned int state)
{
	struct appctx *app = context;
	struct stream_interface *si;
	unsigned int rate;
	size_t count;

	if (app->state & APPLET_WANT_DIE) {
		__appctx_free(app);
		return NULL;
	}

	si = cs_si(app->owner);

	/* We always pretend the applet can't get and doesn't want to
	 * put, it's up to it to change this if needed. This ensures
	 * that one applet which ignores any event will not spin.
	 */
	si_cant_get(si);
	si_rx_endp_done(si);

	/* Now we'll try to allocate the input buffer. We wake up the applet in
	 * all cases. So this is the applet's responsibility to check if this
	 * buffer was allocated or not. This leaves a chance for applets to do
	 * some other processing if needed. The applet doesn't have anything to
	 * do if it needs the buffer, it will be called again upon readiness.
	 */
	if (!si_alloc_ibuf(si, &app->buffer_wait))
		si_rx_endp_more(si);

	count = co_data(si_oc(si));
	app->applet->fct(app);

	/* now check if the applet has released some room and forgot to
	 * notify the other side about it.
	 */
	if (count != co_data(si_oc(si))) {
		si_oc(si)->flags |= CF_WRITE_PARTIAL | CF_WROTE_DATA;
		si_rx_room_rdy(si_opposite(si));
	}

	/* measure the call rate and check for anomalies when too high */
	rate = update_freq_ctr(&app->call_rate, 1);
	if (rate >= 100000 && app->call_rate.prev_ctr && // looped more than 100k times over last second
	    ((b_size(si_ib(si)) && si->flags & SI_FL_RXBLK_BUFF) || // asks for a buffer which is present
	     (b_size(si_ib(si)) && !b_data(si_ib(si)) && si->flags & SI_FL_RXBLK_ROOM) || // asks for room in an empty buffer
	     (b_data(si_ob(si)) && si_tx_endp_ready(si) && !si_tx_blocked(si)) || // asks for data already present
	     (!b_data(si_ib(si)) && b_data(si_ob(si)) && // didn't return anything ...
	      (si_oc(si)->flags & (CF_WRITE_PARTIAL|CF_SHUTW_NOW)) == CF_SHUTW_NOW))) { // ... and left data pending after a shut
		stream_dump_and_crash(&app->obj_type, read_freq_ctr(&app->call_rate));
	}

	si_applet_wake_cb(si);
	channel_release_buffer(si_ic(si), &app->buffer_wait);
	return t;
}
