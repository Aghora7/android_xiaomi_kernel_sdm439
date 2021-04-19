/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2014 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/msm_drm_notify.h>
#include <linux/notifier.h>

#include "msm_drv.h"
#include "msm_kms.h"
#include "msm_gem.h"
#include "msm_fence.h"
#include "sde_trace.h"

#define MULTIPLE_CONN_DETECTED(x) (x > 1)

struct msm_commit {
	struct drm_device *dev;
	struct drm_atomic_state *state;
	uint32_t crtc_mask;
	bool nonblock;
	struct kthread_work commit_work;
};

static BLOCKING_NOTIFIER_HEAD(msm_drm_notifier_list);

/**
 * msm_drm_register_client - register a client notifier
 * @nb: notifier block to callback on events
 *
 * This function registers a notifier callback function
 * to msm_drm_notifier_list, which would be called when
 * received unblank/power down event.
 */
int msm_drm_register_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&msm_drm_notifier_list,
						nb);
}
EXPORT_SYMBOL(msm_drm_register_client);

/**
 * msm_drm_unregister_client - unregister a client notifier
 * @nb: notifier block to callback on events
 *
 * This function unregisters the callback function from
 * msm_drm_notifier_list.
 */
int msm_drm_unregister_client(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&msm_drm_notifier_list,
						  nb);
}
EXPORT_SYMBOL(msm_drm_unregister_client);

/**
 * msm_drm_notifier_call_chain - notify clients of drm_events
 * @val: event MSM_DRM_EARLY_EVENT_BLANK or MSM_DRM_EVENT_BLANK
 * @v: notifier data, inculde display id and display blank
 *     event(unblank or power down).
 */
static int msm_drm_notifier_call_chain(unsigned long val, void *v)
{
	return blocking_notifier_call_chain(&msm_drm_notifier_list, val,
					    v);
}

/* block until specified crtcs are no longer pending update, and
 * atomically mark them as pending update
 */
static int start_atomic(struct msm_drm_private *priv, uint32_t crtc_mask)
{
	int ret;

	spin_lock(&priv->pending_crtcs_event.lock);
	ret = wait_event_interruptible_locked(priv->pending_crtcs_event,
			!(priv->pending_crtcs & crtc_mask));
	if (ret == 0) {
		DBG("start: %08x", crtc_mask);
		priv->pending_crtcs |= crtc_mask;
	}
	spin_unlock(&priv->pending_crtcs_event.lock);

	return ret;
}

/* clear specified crtcs (no longer pending update)
 */
static void end_atomic(struct msm_drm_private *priv, uint32_t crtc_mask)
{
	spin_lock(&priv->pending_crtcs_event.lock);
	DBG("end: %08x", crtc_mask);
	priv->pending_crtcs &= ~crtc_mask;
	wake_up_all_locked(&priv->pending_crtcs_event);
	spin_unlock(&priv->pending_crtcs_event.lock);
}

static void commit_destroy(struct msm_commit *c)
{
	end_atomic(c->dev->dev_private, c->crtc_mask);
	if (c->nonblock)
		kfree(c);
}

static inline bool _msm_seamless_for_crtc(struct drm_atomic_state *state,
			struct drm_crtc_state *crtc_state, bool enable)
{
	struct drm_connector *connector = NULL;
	struct drm_connector_state  *conn_state = NULL;
	int i = 0;
	int conn_cnt = 0;

	if (msm_is_mode_seamless(&crtc_state->mode) ||
		msm_is_mode_seamless_vrr(&crtc_state->adjusted_mode) ||
		msm_is_mode_seamless_dyn_clk(&crtc_state->adjusted_mode))
		return true;

	if (msm_is_mode_seamless_dms(&crtc_state->adjusted_mode) && !enable)
		return true;

	if (!crtc_state->mode_changed && crtc_state->connectors_changed) {
		for_each_connector_in_state(state, connector, conn_state, i) {
			if ((conn_state->crtc == crtc_state->crtc) ||
					(connector->state->crtc ==
					 crtc_state->crtc))
				conn_cnt++;

			if (MULTIPLE_CONN_DETECTED(conn_cnt))
				return true;
		}
	}

	return false;
}

static inline bool _msm_seamless_for_conn(struct drm_connector *connector,
		struct drm_connector_state *old_conn_state, bool enable)
{
	if (!old_conn_state || !old_conn_state->crtc)
		return false;

	if (!old_conn_state->crtc->state->mode_changed &&
			!old_conn_state->crtc->state->active_changed &&
			old_conn_state->crtc->state->connectors_changed) {
		if (old_conn_state->crtc == connector->state->crtc)
			return true;
	}

	if (enable)
		return false;

	if (msm_is_mode_seamless(&connector->encoder->crtc->state->mode))
		return true;

	if (msm_is_mode_seamless_vrr(
			&connector->encoder->crtc->state->adjusted_mode))
		return true;

	if (msm_is_mode_seamless_dyn_clk(
			 &connector->encoder->crtc->state->adjusted_mode))
		return true;

	if (msm_is_mode_seamless_dms(
			&connector->encoder->crtc->state->adjusted_mode))
		return true;

	return false;
}

static void msm_atomic_wait_for_commit_done(
		struct drm_device *dev,
		struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct msm_drm_private *priv = old_state->dev->dev_private;
	struct msm_kms *kms = priv->kms;
	int i;

	for_each_crtc_in_state(old_state, crtc, crtc_state, i) {
		if (!crtc->state->enable)
			continue;

		/* Legacy cursor ioctls are completely unsynced, and userspace
		 * relies on that (by doing tons of cursor updates). */
		if (old_state->legacy_cursor_update)
			continue;

		if (drm_crtc_vblank_get(crtc))
			continue;

		kms->funcs->wait_for_crtc_commit_done(kms, crtc);

		drm_crtc_vblank_put(crtc);
	}

	for_each_connector_in_state(old_state, connector, old_conn_state, i) {
		struct drm_encoder *encoder;

		if (!connector->state->best_encoder)
			continue;

		if (!connector->state->crtc->state->active ||
		    !drm_atomic_crtc_needs_modeset(
				    connector->state->crtc->state))
			continue;

		if (_msm_seamless_for_conn(connector, old_conn_state, true))
			continue;

		encoder = connector->state->best_encoder;

		DRM_DEBUG_ATOMIC("bridge enable enabling [ENCODER:%d:%s]\n",
				 encoder->base.id, encoder->name);

		drm_bridge_enable(encoder->bridge);
		if (connector->state->crtc->state->active_changed) {
			DRM_DEBUG_ATOMIC("Notify unblank\n");
			msm_drm_notifier_call_chain(MSM_DRM_EVENT_BLANK,
					    &notifier_data);
		}
	}
	SDE_ATRACE_END("msm_enable");
}

/* The (potentially) asynchronous part of the commit.  At this point
 * nothing can fail short of armageddon.
 */
static void complete_commit(struct msm_commit *c)
{
	struct drm_atomic_state *state = c->state;
	struct drm_device *dev = state->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;

	drm_atomic_helper_wait_for_fences(dev, state, false);

	kms->funcs->prepare_commit(kms, state);

	msm_atomic_helper_commit_modeset_disables(dev, state);

	drm_atomic_helper_commit_planes(dev, state, 0);

	msm_atomic_helper_commit_modeset_enables(dev, state);

	/* NOTE: _wait_for_vblanks() only waits for vblank on
	 * enabled CRTCs.  So we end up faulting when disabling
	 * due to (potentially) unref'ing the outgoing fb's
	 * before the vblank when the disable has latched.
	 *
	 * But if it did wait on disabled (or newly disabled)
	 * CRTCs, that would be racy (ie. we could have missed
	 * the irq.  We need some way to poll for pipe shut
	 * down.  Or just live with occasionally hitting the
	 * timeout in the CRTC disable path (which really should
	 * not be critical path)
	 */

	msm_atomic_wait_for_commit_done(dev, state);

	drm_atomic_helper_cleanup_planes(dev, state);

	kms->funcs->complete_commit(kms, state);

	drm_atomic_state_free(state);

	commit_destroy(c);
}

static void _msm_drm_commit_work_cb(struct kthread_work *work)
{
	struct msm_commit *commit =  NULL;

	if (!work) {
		DRM_ERROR("%s: Invalid commit work data!\n", __func__);
		return;
	}

	commit = container_of(work, struct msm_commit, commit_work);

	SDE_ATRACE_BEGIN("complete_commit");
	complete_commit(commit);
	SDE_ATRACE_END("complete_commit");
}

static struct msm_commit *commit_init(struct drm_atomic_state *state,
		bool nonblock)
{
	struct msm_commit *c = kzalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return NULL;

	c->dev = state->dev;
	c->state = state;
	c->nonblock = nonblock;

	kthread_init_work(&c->commit_work, _msm_drm_commit_work_cb);

	return c;
}

/* Start display thread function */
static void msm_atomic_commit_dispatch(struct drm_device *dev,
		struct drm_atomic_state *state, struct msm_commit *commit)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_crtc *crtc = NULL;
	struct drm_crtc_state *crtc_state = NULL;
	int ret = -EINVAL, i = 0, j = 0;
	bool nonblock;

	/* cache since work will kfree commit in non-blocking case */
	nonblock = commit->nonblock;

	for_each_crtc_in_state(state, crtc, crtc_state, i) {
		for (j = 0; j < priv->num_crtcs; j++) {
			if (priv->disp_thread[j].crtc_id ==
						crtc->base.id) {
				if (priv->disp_thread[j].thread) {
					kthread_queue_work(
						&priv->disp_thread[j].worker,
							&commit->commit_work);
					/* only return zero if work is
					 * queued successfully.
					 */
					ret = 0;
				} else {
					DRM_ERROR(" Error for crtc_id: %d\n",
						priv->disp_thread[j].crtc_id);
				}
				break;
			}
		}
		/*
		 * TODO: handle cases where there will be more than
		 * one crtc per commit cycle. Remove this check then.
		 * Current assumption is there will be only one crtc
		 * per commit cycle.
		 */
		if (j < priv->num_crtcs)
			break;
	}

	if (ret) {
		/**
		 * this is not expected to happen, but at this point the state
		 * has been swapped, but we couldn't dispatch to a crtc thread.
		 * fallback now to a synchronous complete_commit to try and
		 * ensure that SW and HW state don't get out of sync.
		 */
		DRM_ERROR("failed to dispatch commit to any CRTC\n");
		complete_commit(commit);
	} else if (!nonblock) {
		kthread_flush_work(&commit->commit_work);
	}

	/* free nonblocking commits in this context, after processing */
	if (!nonblock)
		kfree(commit);
}

/**
 * drm_atomic_helper_commit - commit validated state object
 * @dev: DRM device
 * @state: the driver state object
 * @nonblock: nonblocking commit
 *
 * This function commits a with drm_atomic_helper_check() pre-validated state
 * object. This can still fail when e.g. the framebuffer reservation fails.
 *
 * RETURNS
 * Zero for success or -errno.
 */
int msm_atomic_commit(struct drm_device *dev,
		struct drm_atomic_state *state, bool nonblock)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_commit *c;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	int i, ret;

	if (!priv || priv->shutdown_in_progress) {
		DRM_ERROR("priv is null or shutdwon is in-progress\n");
		return -EINVAL;
	}

	SDE_ATRACE_BEGIN("atomic_commit");
	ret = drm_atomic_helper_prepare_planes(dev, state);
	if (ret) {
		SDE_ATRACE_END("atomic_commit");
		return ret;
	}

	c = commit_init(state, nonblock);
	if (!c) {
		ret = -ENOMEM;
		goto error;
	}

	/*
	 * Figure out what crtcs we have:
	 */
	for_each_crtc_in_state(state, crtc, crtc_state, i)
		c->crtc_mask |= drm_crtc_mask(crtc);

	/*
	 * Figure out what fence to wait for:
	 */
	for_each_plane_in_state(state, plane, plane_state, i) {
		if ((plane->state->fb != plane_state->fb) && plane_state->fb) {
			struct drm_gem_object *obj = msm_framebuffer_bo(plane_state->fb, 0);
			struct msm_gem_object *msm_obj = to_msm_bo(obj);

			plane_state->fence = reservation_object_get_excl_rcu(msm_obj->resv);
		}
	}

	/*
	 * Wait for pending updates on any of the same crtc's and then
	 * mark our set of crtc's as busy:
	 */
	ret = start_atomic(dev->dev_private, c->crtc_mask);
	if (ret) {
		kfree(c);
		goto error;
	}

	/*
	 * This is the point of no return - everything below never fails except
	 * when the hw goes bonghits. Which means we can commit the new state on
	 * the software side now.
	 */

	drm_atomic_helper_swap_state(state, true);

	/*
	 * Provide the driver a chance to prepare for output fences. This is
	 * done after the point of no return, but before asynchronous commits
	 * are dispatched to work queues, so that the fence preparation is
	 * finished before the .atomic_commit returns.
	 */
	if (priv->kms && priv->kms->funcs && priv->kms->funcs->prepare_fence)
		priv->kms->funcs->prepare_fence(priv->kms, state);

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one conditions: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout.
	 */

	msm_atomic_commit_dispatch(dev, state, c);
	SDE_ATRACE_END("atomic_commit");
	return 0;

error:
	drm_atomic_helper_cleanup_planes(dev, state);
	SDE_ATRACE_END("atomic_commit");
	return ret;
}
