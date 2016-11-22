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

#include <linux/msm_mhi.h>
#include <linux/workqueue.h>
#include <linux/pm.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/pm_runtime.h>

#include "mhi_sys.h"
#include "mhi.h"
#include "mhi_hwio.h"

/* Write only sysfs attributes */
static DEVICE_ATTR(MHI_M0, S_IWUSR, NULL, sysfs_init_m0);
static DEVICE_ATTR(MHI_M3, S_IWUSR, NULL, sysfs_init_m3);

/* Read only sysfs attributes */

static struct attribute *mhi_attributes[] = {
	&dev_attr_MHI_M0.attr,
	&dev_attr_MHI_M3.attr,
	NULL,
};

static struct attribute_group mhi_attribute_group = {
	.attrs = mhi_attributes,
};

int mhi_pci_suspend(struct device *dev)
{
	int r = 0;
	struct mhi_device_ctxt *mhi_dev_ctxt = dev_get_drvdata(dev);

	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Entered\n");
	/* if rpm status still active then force suspend */
	if (!pm_runtime_status_suspended(dev)) {
		r = mhi_runtime_suspend(dev);
		if (r)
			return r;
	}

	pm_runtime_set_suspended(dev);
	pm_runtime_disable(dev);

	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Exit\n");
	return r;
}

int mhi_runtime_suspend(struct device *dev)
{
	int r = 0;
	struct mhi_device_ctxt *mhi_dev_ctxt = dev_get_drvdata(dev);

	mutex_lock(&mhi_dev_ctxt->pm_lock);
	read_lock_bh(&mhi_dev_ctxt->pm_xfer_lock);

	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO,
		"Entered with State:0x%x %s\n",
		mhi_dev_ctxt->mhi_pm_state,
		TO_MHI_STATE_STR(mhi_dev_ctxt->mhi_state));

	/* Link is already disabled */
	if (mhi_dev_ctxt->mhi_pm_state == MHI_PM_DISABLE ||
	   mhi_dev_ctxt->mhi_pm_state == MHI_PM_M3) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_INFO,
			"Already in active state, exiting\n");
		read_unlock_bh(&mhi_dev_ctxt->pm_xfer_lock);
		mutex_unlock(&mhi_dev_ctxt->pm_lock);
		return 0;
	}

	if (unlikely(atomic_read(&mhi_dev_ctxt->counters.device_wake))) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_INFO,
			"Busy, Aborting Runtime Suspend\n");
		read_unlock_bh(&mhi_dev_ctxt->pm_xfer_lock);
		mutex_unlock(&mhi_dev_ctxt->pm_lock);
		return -EBUSY;
	}

	mhi_dev_ctxt->assert_wake(mhi_dev_ctxt, false);
	read_unlock_bh(&mhi_dev_ctxt->pm_xfer_lock);
	r = wait_event_timeout(*mhi_dev_ctxt->mhi_ev_wq.m0_event,
			       mhi_dev_ctxt->mhi_state == MHI_STATE_M0 ||
			       mhi_dev_ctxt->mhi_state == MHI_STATE_M1,
			       msecs_to_jiffies(MHI_MAX_RESUME_TIMEOUT));
	if (!r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Failed to get M0||M1 event, timeout, current state:%s\n",
			TO_MHI_STATE_STR(mhi_dev_ctxt->mhi_state));
		r = -EIO;
		goto rpm_suspend_exit;
	}

	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Allowing M3 State\n");
	write_lock_irq(&mhi_dev_ctxt->pm_xfer_lock);
	mhi_dev_ctxt->deassert_wake(mhi_dev_ctxt);
	mhi_dev_ctxt->mhi_pm_state = MHI_PM_M3_ENTER;
	mhi_set_m_state(mhi_dev_ctxt, MHI_STATE_M3);
	write_unlock_irq(&mhi_dev_ctxt->pm_xfer_lock);
	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Waiting for M3 completion.\n");
	r = wait_event_timeout(*mhi_dev_ctxt->mhi_ev_wq.m3_event,
			       mhi_dev_ctxt->mhi_state == MHI_STATE_M3,
			       msecs_to_jiffies(MHI_MAX_SUSPEND_TIMEOUT));
	if (!r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Failed to get M3 event, timeout, current state:%s\n",
			TO_MHI_STATE_STR(mhi_dev_ctxt->mhi_state));
		r = -EIO;
		goto rpm_suspend_exit;
	}

	r = mhi_turn_off_pcie_link(mhi_dev_ctxt);
	if (r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_ERROR,
			"Failed to Turn off link ret:%d\n",
			r);
	}

rpm_suspend_exit:
	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Exited\n");
	mutex_unlock(&mhi_dev_ctxt->pm_lock);
	return r;
}

int mhi_runtime_idle(struct device *dev)
{
	struct mhi_device_ctxt *mhi_dev_ctxt = dev_get_drvdata(dev);

	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO,
		"Entered returning -EBUSY\n");

	/*
	 * RPM framework during runtime resume always calls
	 * rpm_idle to see if device ready to suspend.
	 * If dev.power usage_count count is 0, rpm fw will call
	 * rpm_idle cb to see if device is ready to suspend.
	 * if cb return 0, or cb not defined the framework will
	 * assume device driver is ready to suspend;
	 * therefore, fw will schedule runtime suspend.
	 * In MHI power management, MHI host shall go to
	 * runtime suspend only after entering MHI State M2, even if
	 * usage count is 0.  Return -EBUSY to disable automatic suspend.
	 */
	return -EBUSY;
}

int mhi_runtime_resume(struct device *dev)
{
	int r = 0;
	struct mhi_device_ctxt *mhi_dev_ctxt = dev_get_drvdata(dev);

	mutex_lock(&mhi_dev_ctxt->pm_lock);
	read_lock_bh(&mhi_dev_ctxt->pm_xfer_lock);
	BUG_ON(mhi_dev_ctxt->mhi_pm_state != MHI_PM_M3);
	read_unlock_bh(&mhi_dev_ctxt->pm_xfer_lock);

	/* turn on link */
	r = mhi_turn_on_pcie_link(mhi_dev_ctxt);
	if (r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Failed to resume link\n");
		goto rpm_resume_exit;
	}

	write_lock_irq(&mhi_dev_ctxt->pm_xfer_lock);
	mhi_dev_ctxt->mhi_pm_state = MHI_PM_M3_EXIT;
	write_unlock_irq(&mhi_dev_ctxt->pm_xfer_lock);

	/* Set and wait for M0 Event */
	write_lock_irq(&mhi_dev_ctxt->pm_xfer_lock);
	mhi_set_m_state(mhi_dev_ctxt, MHI_STATE_M0);
	write_unlock_irq(&mhi_dev_ctxt->pm_xfer_lock);
	r = wait_event_timeout(*mhi_dev_ctxt->mhi_ev_wq.m0_event,
			       mhi_dev_ctxt->mhi_state == MHI_STATE_M0 ||
			       mhi_dev_ctxt->mhi_state == MHI_STATE_M1,
			       msecs_to_jiffies(MHI_MAX_RESUME_TIMEOUT));
	if (!r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_ERROR,
			"Failed to get M0 event, timeout\n");
		r = -EIO;
		goto rpm_resume_exit;
	}
	r = 0; /* no errors */

rpm_resume_exit:
	mutex_unlock(&mhi_dev_ctxt->pm_lock);
	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Exited with :%d\n", r);
	return r;
}

int mhi_pci_resume(struct device *dev)
{
	int r = 0;
	struct mhi_device_ctxt *mhi_dev_ctxt = dev_get_drvdata(dev);

	r = mhi_runtime_resume(dev);
	if (r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Failed to resume link\n");
	} else {
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);
	}

	return r;
}

int mhi_init_pm_sysfs(struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &mhi_attribute_group);
}

void mhi_rem_pm_sysfs(struct device *dev)
{
	return sysfs_remove_group(&dev->kobj, &mhi_attribute_group);
}

ssize_t sysfs_init_m0(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct mhi_device_ctxt *mhi_dev_ctxt = dev_get_drvdata(dev);

	pm_runtime_get(&mhi_dev_ctxt->pcie_device->dev);
	pm_runtime_mark_last_busy(&mhi_dev_ctxt->pcie_device->dev);
	pm_runtime_put_noidle(&mhi_dev_ctxt->pcie_device->dev);

	return count;
}

ssize_t sysfs_init_m3(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct mhi_device_ctxt *mhi_dev_ctxt = dev_get_drvdata(dev);

	if (atomic_read(&mhi_dev_ctxt->counters.device_wake) == 0) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_INFO,
			"Schedule RPM suspend");
		pm_runtime_mark_last_busy(&mhi_dev_ctxt->
					  pcie_device->dev);
		pm_request_autosuspend(&mhi_dev_ctxt->
				       pcie_device->dev);
	}

	return count;
}

int mhi_turn_off_pcie_link(struct mhi_device_ctxt *mhi_dev_ctxt)
{
	struct pci_dev *pcie_dev;
	int r = 0;

	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Entered...\n");
	pcie_dev = mhi_dev_ctxt->pcie_device;

	if (0 == mhi_dev_ctxt->flags.link_up) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Link already marked as down, nothing to do\n");
		goto exit;
	}

	r = pci_save_state(pcie_dev);
	if (r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Failed to save pcie state ret: %d\n", r);
	}
	mhi_dev_ctxt->core.pcie_state = pci_store_saved_state(pcie_dev);
	pci_disable_device(pcie_dev);
	r = pci_set_power_state(pcie_dev, PCI_D3hot);
	if (r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_INFO,
			"Failed to set pcie power state to D3hot ret:%d\n", r);
	}

	r = msm_pcie_pm_control(MSM_PCIE_SUSPEND,
				pcie_dev->bus->number,
				pcie_dev,
				NULL,
				0);
	if (r)
		mhi_log(mhi_dev_ctxt, MHI_MSG_INFO,
			"Failed to suspend pcie bus ret 0x%x\n", r);

	r = mhi_set_bus_request(mhi_dev_ctxt, 0);
	if (r)
		mhi_log(mhi_dev_ctxt, MHI_MSG_INFO,
			"Failed to set bus freq ret %d\n", r);
	mhi_dev_ctxt->flags.link_up = 0;
exit:
	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Exited...\n");

	return 0;
}

int mhi_turn_on_pcie_link(struct mhi_device_ctxt *mhi_dev_ctxt)
{
	int r = 0;
	struct pci_dev *pcie_dev;

	pcie_dev = mhi_dev_ctxt->pcie_device;

	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Entered...\n");
	if (mhi_dev_ctxt->flags.link_up)
		goto exit;

	r  = mhi_set_bus_request(mhi_dev_ctxt, 1);
	if (r)
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Could not set bus frequency ret: %d\n", r);

	r = msm_pcie_pm_control(MSM_PCIE_RESUME,
				pcie_dev->bus->number,
				pcie_dev,
				NULL,
				0);
	if (r) {
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Failed to resume pcie bus ret %d\n", r);
		goto exit;
	}

	r = pci_enable_device(pcie_dev);
	if (r)
		mhi_log(mhi_dev_ctxt, MHI_MSG_CRITICAL,
			"Failed to enable device ret:%d\n", r);

	pci_load_and_free_saved_state(pcie_dev,
				      &mhi_dev_ctxt->core.pcie_state);
	pci_restore_state(pcie_dev);
	pci_set_master(pcie_dev);

	mhi_dev_ctxt->flags.link_up = 1;
exit:
	mhi_log(mhi_dev_ctxt, MHI_MSG_INFO, "Exited...\n");
	return r;
}
