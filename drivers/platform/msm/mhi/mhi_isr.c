/* Copyright (c) 2014-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/interrupt.h>
#include <linux/irqreturn.h>

#include "mhi_sys.h"
#include "mhi_trace.h"

static int mhi_process_event_ring(
		struct mhi_device_ctxt *mhi_dev_ctxt,
		u32 ev_index,
		u32 event_quota)
{
	union mhi_event_pkt *local_rp = NULL;
	union mhi_event_pkt *device_rp = NULL;
	union mhi_event_pkt event_to_process;
	int ret_val = 0;
	struct mhi_event_ctxt *ev_ctxt = NULL;
	struct mhi_ring *local_ev_ctxt =
		&mhi_dev_ctxt->mhi_local_event_ctxt[ev_index];

	read_lock_bh(&mhi_dev_ctxt->pm_xfer_lock);
	if (unlikely(mhi_dev_ctxt->mhi_pm_state == MHI_PM_DISABLE)) {
		mhi_log(MHI_MSG_ERROR, "Invalid MHI PM State\n");
		read_unlock_bh(&mhi_dev_ctxt->pm_xfer_lock);
		return -EIO;
	}
	mhi_assert_device_wake(mhi_dev_ctxt, false);
	read_unlock_bh(&mhi_dev_ctxt->pm_xfer_lock);
	ev_ctxt = &mhi_dev_ctxt->dev_space.ring_ctxt.ec_list[ev_index];

	device_rp = (union mhi_event_pkt *)mhi_p2v_addr(
					mhi_dev_ctxt,
					MHI_RING_TYPE_EVENT_RING,
					ev_index,
					ev_ctxt->mhi_event_read_ptr);

	local_rp = (union mhi_event_pkt *)local_ev_ctxt->rp;

	BUG_ON(validate_ev_el_addr(local_ev_ctxt, (uintptr_t)device_rp));

	while ((local_rp != device_rp) && (event_quota > 0) &&
			(device_rp != NULL) && (local_rp != NULL)) {

		event_to_process = *local_rp;
		read_lock_bh(&mhi_dev_ctxt->pm_xfer_lock);
		recycle_trb_and_ring(mhi_dev_ctxt,
				     local_ev_ctxt,
				     MHI_RING_TYPE_EVENT_RING,
				     ev_index);
		read_unlock_bh(&mhi_dev_ctxt->pm_xfer_lock);

		switch (MHI_TRB_READ_INFO(EV_TRB_TYPE, &event_to_process)) {
		case MHI_PKT_TYPE_CMD_COMPLETION_EVENT:
		{
			union mhi_cmd_pkt *cmd_pkt;
			u32 chan;
			struct mhi_chan_cfg *cfg;
			unsigned long flags;
			struct mhi_ring *cmd_ring = &mhi_dev_ctxt->
				mhi_local_cmd_ctxt[PRIMARY_CMD_RING];
			__pm_stay_awake(&mhi_dev_ctxt->w_lock);
			__pm_relax(&mhi_dev_ctxt->w_lock);
			get_cmd_pkt(mhi_dev_ctxt,
				    &event_to_process,
				    &cmd_pkt, ev_index);
			MHI_TRB_GET_INFO(CMD_TRB_CHID, cmd_pkt, chan);
			cfg = &mhi_dev_ctxt->mhi_chan_cfg[chan];
			mhi_log(MHI_MSG_INFO,
				"MHI CCE received ring 0x%x chan:%u\n",
				ev_index,
				chan);
			spin_lock_irqsave(&cfg->event_lock, flags);
			cfg->cmd_pkt = *cmd_pkt;
			cfg->cmd_event_pkt =
				event_to_process.cmd_complete_event_pkt;
			complete(&cfg->cmd_complete);
			spin_unlock_irqrestore(&cfg->event_lock, flags);
			spin_lock_irqsave(&cmd_ring->ring_lock,
					  flags);
			ctxt_del_element(cmd_ring, NULL);
			spin_unlock_irqrestore(&cmd_ring->ring_lock,
					       flags);
			break;
		}
		case MHI_PKT_TYPE_TX_EVENT:
			__pm_stay_awake(&mhi_dev_ctxt->w_lock);
			parse_xfer_event(mhi_dev_ctxt,
					 &event_to_process,
					 ev_index);
			__pm_relax(&mhi_dev_ctxt->w_lock);
			break;
		case MHI_PKT_TYPE_STATE_CHANGE_EVENT:
		{
			enum STATE_TRANSITION new_state;
			unsigned long flags;
			new_state = MHI_READ_STATE(&event_to_process);
			mhi_log(MHI_MSG_INFO,
				"MHI STE received ring 0x%x State:%s\n",
				ev_index,
				state_transition_str(new_state));

			/* If transitioning to M1 schedule worker thread */
			if (new_state == STATE_TRANSITION_M1) {
				write_lock_irqsave(&mhi_dev_ctxt->pm_xfer_lock,
						   flags);
				mhi_dev_ctxt->mhi_state =
					mhi_get_m_state(mhi_dev_ctxt);
				if (mhi_dev_ctxt->mhi_state == MHI_STATE_M1) {
					mhi_dev_ctxt->mhi_pm_state = MHI_PM_M1;
					mhi_dev_ctxt->counters.m0_m1++;
					schedule_work(&mhi_dev_ctxt->
						      process_m1_worker);
				}
				write_unlock_irqrestore(&mhi_dev_ctxt->
							pm_xfer_lock,
							flags);
			} else {
				mhi_init_state_transition(mhi_dev_ctxt,
							  new_state);
			}
			break;
		}
		case MHI_PKT_TYPE_EE_EVENT:
		{
			enum STATE_TRANSITION new_state;

			mhi_log(MHI_MSG_INFO,
					"MHI EEE received ring 0x%x\n",
					ev_index);
			__pm_stay_awake(&mhi_dev_ctxt->w_lock);
			__pm_relax(&mhi_dev_ctxt->w_lock);
			switch (MHI_READ_EXEC_ENV(&event_to_process)) {
			case MHI_EXEC_ENV_SBL:
				new_state = STATE_TRANSITION_SBL;
				mhi_init_state_transition(mhi_dev_ctxt,
								new_state);
				break;
			case MHI_EXEC_ENV_AMSS:
				new_state = STATE_TRANSITION_AMSS;
				mhi_init_state_transition(mhi_dev_ctxt,
								new_state);
				break;
			}
			break;
		}
		case MHI_PKT_TYPE_SYS_ERR_EVENT:
			mhi_log(MHI_MSG_INFO,
			   "MHI System Error Detected. Triggering Reset\n");
			BUG();
			break;
		default:
			mhi_log(MHI_MSG_ERROR,
				"Unsupported packet type code 0x%x\n",
				MHI_TRB_READ_INFO(EV_TRB_TYPE,
					&event_to_process));
			break;
		}
		local_rp = (union mhi_event_pkt *)local_ev_ctxt->rp;
		device_rp = (union mhi_event_pkt *)mhi_p2v_addr(
						mhi_dev_ctxt,
						MHI_RING_TYPE_EVENT_RING,
						ev_index,
						ev_ctxt->mhi_event_read_ptr);
		ret_val = 0;
		--event_quota;
	}
	read_lock_bh(&mhi_dev_ctxt->pm_xfer_lock);
	mhi_deassert_device_wake(mhi_dev_ctxt);
	read_unlock_bh(&mhi_dev_ctxt->pm_xfer_lock);
	return ret_val;
}

int parse_event_thread(void *ctxt)
{
	struct mhi_device_ctxt *mhi_dev_ctxt = ctxt;
	u32 i = 0;
	int ret_val = 0;
	int ret_val_process_event = 0;
	atomic_t *ev_pen_ptr = &mhi_dev_ctxt->counters.events_pending;

	/* Go through all event rings */
	for (;;) {
		ret_val =
			wait_event_interruptible(
				*mhi_dev_ctxt->mhi_ev_wq.mhi_event_wq,
				((atomic_read(
				&mhi_dev_ctxt->counters.events_pending) > 0) &&
					!mhi_dev_ctxt->flags.stop_threads) ||
				mhi_dev_ctxt->flags.kill_threads ||
				(mhi_dev_ctxt->flags.stop_threads &&
				!mhi_dev_ctxt->flags.ev_thread_stopped));

		switch (ret_val) {
		case -ERESTARTSYS:
			return 0;
		default:
			if (mhi_dev_ctxt->flags.kill_threads) {
				mhi_log(MHI_MSG_INFO,
					"Caught exit signal, quitting\n");
				return 0;
			}
			if (mhi_dev_ctxt->flags.stop_threads) {
				mhi_dev_ctxt->flags.ev_thread_stopped = 1;
				continue;
			}
			break;
		}
		mhi_dev_ctxt->flags.ev_thread_stopped = 0;
		atomic_dec(&mhi_dev_ctxt->counters.events_pending);
		for (i = 1; i < mhi_dev_ctxt->mmio_info.nr_event_rings; ++i) {
			if (mhi_dev_ctxt->mhi_state == MHI_STATE_SYS_ERR) {
				mhi_log(MHI_MSG_INFO,
				"SYS_ERR detected, not processing events\n");
				atomic_set(&mhi_dev_ctxt->
					   counters.events_pending,
					   0);
				break;
			}
			if (GET_EV_PROPS(EV_MANAGED,
					mhi_dev_ctxt->ev_ring_props[i].flags)) {
				ret_val_process_event =
				    mhi_process_event_ring(mhi_dev_ctxt,
						  i,
						  mhi_dev_ctxt->
						  ev_ring_props[i].nr_desc);
				if (ret_val_process_event == -EINPROGRESS)
					atomic_inc(ev_pen_ptr);
			}
		}
	}
}

void mhi_ctrl_ev_task(unsigned long data)
{
	struct mhi_device_ctxt *mhi_dev_ctxt =
		(struct mhi_device_ctxt *)data;
	const unsigned CTRL_EV_RING = 0;
	struct mhi_event_ring_cfg *ring_props =
		&mhi_dev_ctxt->ev_ring_props[CTRL_EV_RING];

	mhi_log(MHI_MSG_VERBOSE, "Enter\n");
	/* Process control event ring */
	mhi_process_event_ring(mhi_dev_ctxt,
			       CTRL_EV_RING,
			       ring_props->nr_desc);
	enable_irq(MSI_TO_IRQ(mhi_dev_ctxt, CTRL_EV_RING));
	mhi_log(MHI_MSG_VERBOSE, "Exit\n");
}

struct mhi_result *mhi_poll(struct mhi_client_handle *client_handle)
{
	int ret_val;

	client_handle->result.buf_addr = NULL;
	client_handle->result.bytes_xferd = 0;
	client_handle->result.transaction_status = 0;
	ret_val = mhi_process_event_ring(client_handle->mhi_dev_ctxt,
					 client_handle->event_ring_index,
					 1);
	if (ret_val)
		mhi_log(MHI_MSG_INFO, "NAPI failed to process event ring\n");
	return &(client_handle->result);
}

void mhi_mask_irq(struct mhi_client_handle *client_handle)
{
	struct mhi_device_ctxt *mhi_dev_ctxt =
		client_handle->mhi_dev_ctxt;
	struct mhi_ring *ev_ring = &mhi_dev_ctxt->
		mhi_local_event_ctxt[client_handle->event_ring_index];

	disable_irq_nosync(MSI_TO_IRQ(mhi_dev_ctxt, client_handle->msi_vec));
	ev_ring->msi_disable_cntr++;
}

void mhi_unmask_irq(struct mhi_client_handle *client_handle)
{
	struct mhi_device_ctxt *mhi_dev_ctxt =
		client_handle->mhi_dev_ctxt;
	struct mhi_ring *ev_ring = &mhi_dev_ctxt->
		mhi_local_event_ctxt[client_handle->event_ring_index];

	ev_ring->msi_enable_cntr++;
	enable_irq(MSI_TO_IRQ(mhi_dev_ctxt, client_handle->msi_vec));
}

irqreturn_t mhi_msi_handlr(int irq_number, void *dev_id)
{
	struct device *mhi_device = dev_id;
	struct mhi_device_ctxt *mhi_dev_ctxt = mhi_device->platform_data;
	int msi = IRQ_TO_MSI(mhi_dev_ctxt, irq_number);

	if (!mhi_dev_ctxt) {
		mhi_log(MHI_MSG_ERROR, "Failed to get a proper context\n");
		return IRQ_HANDLED;
	}
	mhi_dev_ctxt->counters.msi_counter[
			IRQ_TO_MSI(mhi_dev_ctxt, irq_number)]++;
	mhi_log(MHI_MSG_VERBOSE, "Got MSI 0x%x\n", msi);
	trace_mhi_msi(IRQ_TO_MSI(mhi_dev_ctxt, irq_number));

	if (msi) {
		atomic_inc(&mhi_dev_ctxt->counters.events_pending);
		wake_up_interruptible(mhi_dev_ctxt->mhi_ev_wq.mhi_event_wq);
	} else  {
		disable_irq_nosync(irq_number);
		tasklet_schedule(&mhi_dev_ctxt->ev_task);
	}

	return IRQ_HANDLED;
}

irqreturn_t mhi_msi_ipa_handlr(int irq_number, void *dev_id)
{
	struct device *mhi_device = dev_id;
	u32 client_index;
	struct mhi_device_ctxt *mhi_dev_ctxt = mhi_device->platform_data;
	struct mhi_client_handle *client_handle;
	struct mhi_client_info_t *client_info;
	struct mhi_cb_info cb_info;
	int msi_num = (IRQ_TO_MSI(mhi_dev_ctxt, irq_number));

	mhi_dev_ctxt->counters.msi_counter[msi_num]++;
	mhi_log(MHI_MSG_VERBOSE, "Got MSI 0x%x\n", msi_num);
	trace_mhi_msi(msi_num);
	client_index = MHI_MAX_CHANNELS -
			(mhi_dev_ctxt->mmio_info.nr_event_rings - msi_num);
	client_handle = mhi_dev_ctxt->client_handle_list[client_index];
	client_info = &client_handle->client_info;
	if (likely(NULL != client_handle)) {
		client_handle->result.user_data =
				client_handle->user_data;
	if (likely(NULL != &client_info->mhi_client_cb)) {
			cb_info.result = &client_handle->result;
			cb_info.cb_reason = MHI_CB_XFER;
			cb_info.chan = client_handle->chan_info.chan_nr;
			cb_info.result->transaction_status = 0;
			client_info->mhi_client_cb(&cb_info);
		}
	}
	return IRQ_HANDLED;
}
