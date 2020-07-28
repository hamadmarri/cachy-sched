// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, Intel Corporation. */

#include "ice.h"
#include "ice_base.h"
#include "ice_lib.h"

/**
 * ice_validate_vf_id - helper to check if VF ID is valid
 * @pf: pointer to the PF structure
 * @vf_id: the ID of the VF to check
 */
static int ice_validate_vf_id(struct ice_pf *pf, u16 vf_id)
{
	/* vf_id range is only valid for 0-255, and should always be unsigned */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(ice_pf_to_dev(pf), "Invalid VF ID: %u\n", vf_id);
		return -EINVAL;
	}
	return 0;
}

/**
 * ice_check_vf_init - helper to check if VF init complete
 * @pf: pointer to the PF structure
 * @vf: the pointer to the VF to check
 */
static int ice_check_vf_init(struct ice_pf *pf, struct ice_vf *vf)
{
	if (!test_bit(ICE_VF_STATE_INIT, vf->vf_states)) {
		dev_err(ice_pf_to_dev(pf), "VF ID: %u in reset. Try again.\n",
			vf->vf_id);
		return -EBUSY;
	}
	return 0;
}

/**
 * ice_vc_vf_broadcast - Broadcast a message to all VFs on PF
 * @pf: pointer to the PF structure
 * @v_opcode: operation code
 * @v_retval: return value
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 */
static void
ice_vc_vf_broadcast(struct ice_pf *pf, enum virtchnl_ops v_opcode,
		    enum virtchnl_status_code v_retval, u8 *msg, u16 msglen)
{
	struct ice_hw *hw = &pf->hw;
	int i;

	ice_for_each_vf(pf, i) {
		struct ice_vf *vf = &pf->vf[i];

		/* Not all vfs are enabled so skip the ones that are not */
		if (!test_bit(ICE_VF_STATE_INIT, vf->vf_states) &&
		    !test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states))
			continue;

		/* Ignore return value on purpose - a given VF may fail, but
		 * we need to keep going and send to all of them
		 */
		ice_aq_send_msg_to_vf(hw, vf->vf_id, v_opcode, v_retval, msg,
				      msglen, NULL);
	}
}

/**
 * ice_set_pfe_link - Set the link speed/status of the virtchnl_pf_event
 * @vf: pointer to the VF structure
 * @pfe: pointer to the virtchnl_pf_event to set link speed/status for
 * @ice_link_speed: link speed specified by ICE_AQ_LINK_SPEED_*
 * @link_up: whether or not to set the link up/down
 */
static void
ice_set_pfe_link(struct ice_vf *vf, struct virtchnl_pf_event *pfe,
		 int ice_link_speed, bool link_up)
{
	if (vf->driver_caps & VIRTCHNL_VF_CAP_ADV_LINK_SPEED) {
		pfe->event_data.link_event_adv.link_status = link_up;
		/* Speed in Mbps */
		pfe->event_data.link_event_adv.link_speed =
			ice_conv_link_speed_to_virtchnl(true, ice_link_speed);
	} else {
		pfe->event_data.link_event.link_status = link_up;
		/* Legacy method for virtchnl link speeds */
		pfe->event_data.link_event.link_speed =
			(enum virtchnl_link_speed)
			ice_conv_link_speed_to_virtchnl(false, ice_link_speed);
	}
}

/**
 * ice_vf_has_no_qs_ena - check if the VF has any Rx or Tx queues enabled
 * @vf: the VF to check
 *
 * Returns true if the VF has no Rx and no Tx queues enabled and returns false
 * otherwise
 */
static bool ice_vf_has_no_qs_ena(struct ice_vf *vf)
{
	return (!bitmap_weight(vf->rxq_ena, ICE_MAX_RSS_QS_PER_VF) &&
		!bitmap_weight(vf->txq_ena, ICE_MAX_RSS_QS_PER_VF));
}

/**
 * ice_is_vf_link_up - check if the VF's link is up
 * @vf: VF to check if link is up
 */
static bool ice_is_vf_link_up(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;

	if (ice_check_vf_init(pf, vf))
		return false;

	if (ice_vf_has_no_qs_ena(vf))
		return false;
	else if (vf->link_forced)
		return vf->link_up;
	else
		return pf->hw.port_info->phy.link_info.link_info &
			ICE_AQ_LINK_UP;
}

/**
 * ice_vc_notify_vf_link_state - Inform a VF of link status
 * @vf: pointer to the VF structure
 *
 * send a link status message to a single VF
 */
static void ice_vc_notify_vf_link_state(struct ice_vf *vf)
{
	struct virtchnl_pf_event pfe = { 0 };
	struct ice_hw *hw = &vf->pf->hw;

	pfe.event = VIRTCHNL_EVENT_LINK_CHANGE;
	pfe.severity = PF_EVENT_SEVERITY_INFO;

	if (ice_is_vf_link_up(vf))
		ice_set_pfe_link(vf, &pfe,
				 hw->port_info->phy.link_info.link_speed, true);
	else
		ice_set_pfe_link(vf, &pfe, ICE_AQ_LINK_SPEED_UNKNOWN, false);

	ice_aq_send_msg_to_vf(hw, vf->vf_id, VIRTCHNL_OP_EVENT,
			      VIRTCHNL_STATUS_SUCCESS, (u8 *)&pfe,
			      sizeof(pfe), NULL);
}

/**
 * ice_free_vf_res - Free a VF's resources
 * @vf: pointer to the VF info
 */
static void ice_free_vf_res(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;
	int i, last_vector_idx;

	/* First, disable VF's configuration API to prevent OS from
	 * accessing the VF's VSI after it's freed or invalidated.
	 */
	clear_bit(ICE_VF_STATE_INIT, vf->vf_states);

	/* free VSI and disconnect it from the parent uplink */
	if (vf->lan_vsi_idx) {
		ice_vsi_release(pf->vsi[vf->lan_vsi_idx]);
		vf->lan_vsi_idx = 0;
		vf->lan_vsi_num = 0;
		vf->num_mac = 0;
	}

	last_vector_idx = vf->first_vector_idx + pf->num_msix_per_vf - 1;

	/* clear VF MDD event information */
	memset(&vf->mdd_tx_events, 0, sizeof(vf->mdd_tx_events));
	memset(&vf->mdd_rx_events, 0, sizeof(vf->mdd_rx_events));

	/* Disable interrupts so that VF starts in a known state */
	for (i = vf->first_vector_idx; i <= last_vector_idx; i++) {
		wr32(&pf->hw, GLINT_DYN_CTL(i), GLINT_DYN_CTL_CLEARPBA_M);
		ice_flush(&pf->hw);
	}
	/* reset some of the state variables keeping track of the resources */
	clear_bit(ICE_VF_STATE_MC_PROMISC, vf->vf_states);
	clear_bit(ICE_VF_STATE_UC_PROMISC, vf->vf_states);
}

/**
 * ice_dis_vf_mappings
 * @vf: pointer to the VF structure
 */
static void ice_dis_vf_mappings(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;
	struct device *dev;
	int first, last, v;
	struct ice_hw *hw;

	hw = &pf->hw;
	vsi = pf->vsi[vf->lan_vsi_idx];

	dev = ice_pf_to_dev(pf);
	wr32(hw, VPINT_ALLOC(vf->vf_id), 0);
	wr32(hw, VPINT_ALLOC_PCI(vf->vf_id), 0);

	first = vf->first_vector_idx;
	last = first + pf->num_msix_per_vf - 1;
	for (v = first; v <= last; v++) {
		u32 reg;

		reg = (((1 << GLINT_VECT2FUNC_IS_PF_S) &
			GLINT_VECT2FUNC_IS_PF_M) |
		       ((hw->pf_id << GLINT_VECT2FUNC_PF_NUM_S) &
			GLINT_VECT2FUNC_PF_NUM_M));
		wr32(hw, GLINT_VECT2FUNC(v), reg);
	}

	if (vsi->tx_mapping_mode == ICE_VSI_MAP_CONTIG)
		wr32(hw, VPLAN_TX_QBASE(vf->vf_id), 0);
	else
		dev_err(dev, "Scattered mode for VF Tx queues is not yet implemented\n");

	if (vsi->rx_mapping_mode == ICE_VSI_MAP_CONTIG)
		wr32(hw, VPLAN_RX_QBASE(vf->vf_id), 0);
	else
		dev_err(dev, "Scattered mode for VF Rx queues is not yet implemented\n");
}

/**
 * ice_sriov_free_msix_res - Reset/free any used MSIX resources
 * @pf: pointer to the PF structure
 *
 * Since no MSIX entries are taken from the pf->irq_tracker then just clear
 * the pf->sriov_base_vector.
 *
 * Returns 0 on success, and -EINVAL on error.
 */
static int ice_sriov_free_msix_res(struct ice_pf *pf)
{
	struct ice_res_tracker *res;

	if (!pf)
		return -EINVAL;

	res = pf->irq_tracker;
	if (!res)
		return -EINVAL;

	/* give back irq_tracker resources used */
	WARN_ON(pf->sriov_base_vector < res->num_entries);

	pf->sriov_base_vector = 0;

	return 0;
}

/**
 * ice_set_vf_state_qs_dis - Set VF queues state to disabled
 * @vf: pointer to the VF structure
 */
void ice_set_vf_state_qs_dis(struct ice_vf *vf)
{
	/* Clear Rx/Tx enabled queues flag */
	bitmap_zero(vf->txq_ena, ICE_MAX_RSS_QS_PER_VF);
	bitmap_zero(vf->rxq_ena, ICE_MAX_RSS_QS_PER_VF);
	clear_bit(ICE_VF_STATE_QS_ENA, vf->vf_states);
}

/**
 * ice_dis_vf_qs - Disable the VF queues
 * @vf: pointer to the VF structure
 */
static void ice_dis_vf_qs(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;

	vsi = pf->vsi[vf->lan_vsi_idx];

	ice_vsi_stop_lan_tx_rings(vsi, ICE_NO_RESET, vf->vf_id);
	ice_vsi_stop_all_rx_rings(vsi);
	ice_set_vf_state_qs_dis(vf);
}

/**
 * ice_free_vfs - Free all VFs
 * @pf: pointer to the PF structure
 */
void ice_free_vfs(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	struct ice_hw *hw = &pf->hw;
	int tmp, i;

	if (!pf->vf)
		return;

	while (test_and_set_bit(__ICE_VF_DIS, pf->state))
		usleep_range(1000, 2000);

	/* Disable IOV before freeing resources. This lets any VF drivers
	 * running in the host get themselves cleaned up before we yank
	 * the carpet out from underneath their feet.
	 */
	if (!pci_vfs_assigned(pf->pdev))
		pci_disable_sriov(pf->pdev);
	else
		dev_warn(dev, "VFs are assigned - not disabling SR-IOV\n");

	/* Avoid wait time by stopping all VFs at the same time */
	ice_for_each_vf(pf, i)
		if (test_bit(ICE_VF_STATE_QS_ENA, pf->vf[i].vf_states))
			ice_dis_vf_qs(&pf->vf[i]);

	tmp = pf->num_alloc_vfs;
	pf->num_qps_per_vf = 0;
	pf->num_alloc_vfs = 0;
	for (i = 0; i < tmp; i++) {
		if (test_bit(ICE_VF_STATE_INIT, pf->vf[i].vf_states)) {
			/* disable VF qp mappings and set VF disable state */
			ice_dis_vf_mappings(&pf->vf[i]);
			set_bit(ICE_VF_STATE_DIS, pf->vf[i].vf_states);
			ice_free_vf_res(&pf->vf[i]);
		}
	}

	if (ice_sriov_free_msix_res(pf))
		dev_err(dev, "Failed to free MSIX resources used by SR-IOV\n");

	devm_kfree(dev, pf->vf);
	pf->vf = NULL;

	/* This check is for when the driver is unloaded while VFs are
	 * assigned. Setting the number of VFs to 0 through sysfs is caught
	 * before this function ever gets called.
	 */
	if (!pci_vfs_assigned(pf->pdev)) {
		unsigned int vf_id;

		/* Acknowledge VFLR for all VFs. Without this, VFs will fail to
		 * work correctly when SR-IOV gets re-enabled.
		 */
		for (vf_id = 0; vf_id < tmp; vf_id++) {
			u32 reg_idx, bit_idx;

			reg_idx = (hw->func_caps.vf_base_id + vf_id) / 32;
			bit_idx = (hw->func_caps.vf_base_id + vf_id) % 32;
			wr32(hw, GLGEN_VFLRSTAT(reg_idx), BIT(bit_idx));
		}
	}
	clear_bit(__ICE_VF_DIS, pf->state);
	clear_bit(ICE_FLAG_SRIOV_ENA, pf->flags);
}

/**
 * ice_trigger_vf_reset - Reset a VF on HW
 * @vf: pointer to the VF structure
 * @is_vflr: true if VFLR was issued, false if not
 * @is_pfr: true if the reset was triggered due to a previous PFR
 *
 * Trigger hardware to start a reset for a particular VF. Expects the caller
 * to wait the proper amount of time to allow hardware to reset the VF before
 * it cleans up and restores VF functionality.
 */
static void ice_trigger_vf_reset(struct ice_vf *vf, bool is_vflr, bool is_pfr)
{
	struct ice_pf *pf = vf->pf;
	u32 reg, reg_idx, bit_idx;
	unsigned int vf_abs_id, i;
	struct device *dev;
	struct ice_hw *hw;

	dev = ice_pf_to_dev(pf);
	hw = &pf->hw;
	vf_abs_id = vf->vf_id + hw->func_caps.vf_base_id;

	/* Inform VF that it is no longer active, as a warning */
	clear_bit(ICE_VF_STATE_ACTIVE, vf->vf_states);

	/* Disable VF's configuration API during reset. The flag is re-enabled
	 * in ice_alloc_vf_res(), when it's safe again to access VF's VSI.
	 * It's normally disabled in ice_free_vf_res(), but it's safer
	 * to do it earlier to give some time to finish to any VF config
	 * functions that may still be running at this point.
	 */
	clear_bit(ICE_VF_STATE_INIT, vf->vf_states);

	/* VF_MBX_ARQLEN is cleared by PFR, so the driver needs to clear it
	 * in the case of VFR. If this is done for PFR, it can mess up VF
	 * resets because the VF driver may already have started cleanup
	 * by the time we get here.
	 */
	if (!is_pfr)
		wr32(hw, VF_MBX_ARQLEN(vf->vf_id), 0);

	/* In the case of a VFLR, the HW has already reset the VF and we
	 * just need to clean up, so don't hit the VFRTRIG register.
	 */
	if (!is_vflr) {
		/* reset VF using VPGEN_VFRTRIG reg */
		reg = rd32(hw, VPGEN_VFRTRIG(vf->vf_id));
		reg |= VPGEN_VFRTRIG_VFSWR_M;
		wr32(hw, VPGEN_VFRTRIG(vf->vf_id), reg);
	}
	/* clear the VFLR bit in GLGEN_VFLRSTAT */
	reg_idx = (vf_abs_id) / 32;
	bit_idx = (vf_abs_id) % 32;
	wr32(hw, GLGEN_VFLRSTAT(reg_idx), BIT(bit_idx));
	ice_flush(hw);

	wr32(hw, PF_PCI_CIAA,
	     VF_DEVICE_STATUS | (vf_abs_id << PF_PCI_CIAA_VF_NUM_S));
	for (i = 0; i < ICE_PCI_CIAD_WAIT_COUNT; i++) {
		reg = rd32(hw, PF_PCI_CIAD);
		/* no transactions pending so stop polling */
		if ((reg & VF_TRANS_PENDING_M) == 0)
			break;

		dev_err(dev, "VF %u PCI transactions stuck\n", vf->vf_id);
		udelay(ICE_PCI_CIAD_WAIT_DELAY_US);
	}
}

/**
 * ice_vsi_manage_pvid - Enable or disable port VLAN for VSI
 * @vsi: the VSI to update
 * @pvid_info: VLAN ID and QoS used to set the PVID VSI context field
 * @enable: true for enable PVID false for disable
 */
static int ice_vsi_manage_pvid(struct ice_vsi *vsi, u16 pvid_info, bool enable)
{
	struct ice_hw *hw = &vsi->back->hw;
	struct ice_aqc_vsi_props *info;
	struct ice_vsi_ctx *ctxt;
	enum ice_status status;
	int ret = 0;

	ctxt = kzalloc(sizeof(*ctxt), GFP_KERNEL);
	if (!ctxt)
		return -ENOMEM;

	ctxt->info = vsi->info;
	info = &ctxt->info;
	if (enable) {
		info->vlan_flags = ICE_AQ_VSI_VLAN_MODE_UNTAGGED |
			ICE_AQ_VSI_PVLAN_INSERT_PVID |
			ICE_AQ_VSI_VLAN_EMOD_STR;
		info->sw_flags2 |= ICE_AQ_VSI_SW_FLAG_RX_VLAN_PRUNE_ENA;
	} else {
		info->vlan_flags = ICE_AQ_VSI_VLAN_EMOD_NOTHING |
			ICE_AQ_VSI_VLAN_MODE_ALL;
		info->sw_flags2 &= ~ICE_AQ_VSI_SW_FLAG_RX_VLAN_PRUNE_ENA;
	}

	info->pvid = cpu_to_le16(pvid_info);
	info->valid_sections = cpu_to_le16(ICE_AQ_VSI_PROP_VLAN_VALID |
					   ICE_AQ_VSI_PROP_SW_VALID);

	status = ice_update_vsi(hw, vsi->idx, ctxt, NULL);
	if (status) {
		dev_info(ice_hw_to_dev(hw), "update VSI for port VLAN failed, err %d aq_err %d\n",
			 status, hw->adminq.sq_last_status);
		ret = -EIO;
		goto out;
	}

	vsi->info.vlan_flags = info->vlan_flags;
	vsi->info.sw_flags2 = info->sw_flags2;
	vsi->info.pvid = info->pvid;
out:
	kfree(ctxt);
	return ret;
}

/**
 * ice_vf_vsi_setup - Set up a VF VSI
 * @pf: board private structure
 * @pi: pointer to the port_info instance
 * @vf_id: defines VF ID to which this VSI connects.
 *
 * Returns pointer to the successfully allocated VSI struct on success,
 * otherwise returns NULL on failure.
 */
static struct ice_vsi *
ice_vf_vsi_setup(struct ice_pf *pf, struct ice_port_info *pi, u16 vf_id)
{
	return ice_vsi_setup(pf, pi, ICE_VSI_VF, vf_id);
}

/**
 * ice_calc_vf_first_vector_idx - Calculate MSIX vector index in the PF space
 * @pf: pointer to PF structure
 * @vf: pointer to VF that the first MSIX vector index is being calculated for
 *
 * This returns the first MSIX vector index in PF space that is used by this VF.
 * This index is used when accessing PF relative registers such as
 * GLINT_VECT2FUNC and GLINT_DYN_CTL.
 * This will always be the OICR index in the AVF driver so any functionality
 * using vf->first_vector_idx for queue configuration will have to increment by
 * 1 to avoid meddling with the OICR index.
 */
static int ice_calc_vf_first_vector_idx(struct ice_pf *pf, struct ice_vf *vf)
{
	return pf->sriov_base_vector + vf->vf_id * pf->num_msix_per_vf;
}

/**
 * ice_alloc_vsi_res - Setup VF VSI and its resources
 * @vf: pointer to the VF structure
 *
 * Returns 0 on success, negative value on failure
 */
static int ice_alloc_vsi_res(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;
	LIST_HEAD(tmp_add_list);
	u8 broadcast[ETH_ALEN];
	struct ice_vsi *vsi;
	struct device *dev;
	int status = 0;

	dev = ice_pf_to_dev(pf);
	/* first vector index is the VFs OICR index */
	vf->first_vector_idx = ice_calc_vf_first_vector_idx(pf, vf);

	vsi = ice_vf_vsi_setup(pf, pf->hw.port_info, vf->vf_id);
	if (!vsi) {
		dev_err(dev, "Failed to create VF VSI\n");
		return -ENOMEM;
	}

	vf->lan_vsi_idx = vsi->idx;
	vf->lan_vsi_num = vsi->vsi_num;

	/* Check if port VLAN exist before, and restore it accordingly */
	if (vf->port_vlan_info) {
		ice_vsi_manage_pvid(vsi, vf->port_vlan_info, true);
		if (ice_vsi_add_vlan(vsi, vf->port_vlan_info & VLAN_VID_MASK))
			dev_warn(ice_pf_to_dev(pf), "Failed to add Port VLAN %d filter for VF %d\n",
				 vf->port_vlan_info & VLAN_VID_MASK, vf->vf_id);
	} else {
		/* set VLAN 0 filter by default when no port VLAN is
		 * enabled. If a port VLAN is enabled we don't want
		 * untagged broadcast/multicast traffic seen on the VF
		 * interface.
		 */
		if (ice_vsi_add_vlan(vsi, 0))
			dev_warn(ice_pf_to_dev(pf), "Failed to add VLAN 0 filter for VF %d, MDD events will trigger. Reset the VF, disable spoofchk, or enable 8021q module on the guest\n",
				 vf->vf_id);
	}

	eth_broadcast_addr(broadcast);

	status = ice_add_mac_to_list(vsi, &tmp_add_list, broadcast);
	if (status)
		goto ice_alloc_vsi_res_exit;

	if (is_valid_ether_addr(vf->dflt_lan_addr.addr)) {
		status = ice_add_mac_to_list(vsi, &tmp_add_list,
					     vf->dflt_lan_addr.addr);
		if (status)
			goto ice_alloc_vsi_res_exit;
	}

	status = ice_add_mac(&pf->hw, &tmp_add_list);
	if (status)
		dev_err(dev, "could not add mac filters error %d\n", status);
	else
		vf->num_mac = 1;

	/* Clear this bit after VF initialization since we shouldn't reclaim
	 * and reassign interrupts for synchronous or asynchronous VFR events.
	 * We don't want to reconfigure interrupts since AVF driver doesn't
	 * expect vector assignment to be changed unless there is a request for
	 * more vectors.
	 */
ice_alloc_vsi_res_exit:
	ice_free_fltr_list(dev, &tmp_add_list);
	return status;
}

/**
 * ice_alloc_vf_res - Allocate VF resources
 * @vf: pointer to the VF structure
 */
static int ice_alloc_vf_res(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;
	int tx_rx_queue_left;
	int status;

	/* Update number of VF queues, in case VF had requested for queue
	 * changes
	 */
	tx_rx_queue_left = min_t(int, ice_get_avail_txq_count(pf),
				 ice_get_avail_rxq_count(pf));
	tx_rx_queue_left += pf->num_qps_per_vf;
	if (vf->num_req_qs && vf->num_req_qs <= tx_rx_queue_left &&
	    vf->num_req_qs != vf->num_vf_qs)
		vf->num_vf_qs = vf->num_req_qs;

	/* setup VF VSI and necessary resources */
	status = ice_alloc_vsi_res(vf);
	if (status)
		goto ice_alloc_vf_res_exit;

	if (vf->trusted)
		set_bit(ICE_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps);
	else
		clear_bit(ICE_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps);

	/* VF is now completely initialized */
	set_bit(ICE_VF_STATE_INIT, vf->vf_states);

	return status;

ice_alloc_vf_res_exit:
	ice_free_vf_res(vf);
	return status;
}

/**
 * ice_ena_vf_mappings
 * @vf: pointer to the VF structure
 *
 * Enable VF vectors and queues allocation by writing the details into
 * respective registers.
 */
static void ice_ena_vf_mappings(struct ice_vf *vf)
{
	int abs_vf_id, abs_first, abs_last;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;
	struct device *dev;
	int first, last, v;
	struct ice_hw *hw;
	u32 reg;

	dev = ice_pf_to_dev(pf);
	hw = &pf->hw;
	vsi = pf->vsi[vf->lan_vsi_idx];
	first = vf->first_vector_idx;
	last = (first + pf->num_msix_per_vf) - 1;
	abs_first = first + pf->hw.func_caps.common_cap.msix_vector_first_id;
	abs_last = (abs_first + pf->num_msix_per_vf) - 1;
	abs_vf_id = vf->vf_id + hw->func_caps.vf_base_id;

	/* VF Vector allocation */
	reg = (((abs_first << VPINT_ALLOC_FIRST_S) & VPINT_ALLOC_FIRST_M) |
	       ((abs_last << VPINT_ALLOC_LAST_S) & VPINT_ALLOC_LAST_M) |
	       VPINT_ALLOC_VALID_M);
	wr32(hw, VPINT_ALLOC(vf->vf_id), reg);

	reg = (((abs_first << VPINT_ALLOC_PCI_FIRST_S)
		 & VPINT_ALLOC_PCI_FIRST_M) |
	       ((abs_last << VPINT_ALLOC_PCI_LAST_S) & VPINT_ALLOC_PCI_LAST_M) |
	       VPINT_ALLOC_PCI_VALID_M);
	wr32(hw, VPINT_ALLOC_PCI(vf->vf_id), reg);
	/* map the interrupts to its functions */
	for (v = first; v <= last; v++) {
		reg = (((abs_vf_id << GLINT_VECT2FUNC_VF_NUM_S) &
			GLINT_VECT2FUNC_VF_NUM_M) |
		       ((hw->pf_id << GLINT_VECT2FUNC_PF_NUM_S) &
			GLINT_VECT2FUNC_PF_NUM_M));
		wr32(hw, GLINT_VECT2FUNC(v), reg);
	}

	/* Map mailbox interrupt. We put an explicit 0 here to remind us that
	 * VF admin queue interrupts will go to VF MSI-X vector 0.
	 */
	wr32(hw, VPINT_MBX_CTL(abs_vf_id), VPINT_MBX_CTL_CAUSE_ENA_M | 0);
	/* set regardless of mapping mode */
	wr32(hw, VPLAN_TXQ_MAPENA(vf->vf_id), VPLAN_TXQ_MAPENA_TX_ENA_M);

	/* VF Tx queues allocation */
	if (vsi->tx_mapping_mode == ICE_VSI_MAP_CONTIG) {
		/* set the VF PF Tx queue range
		 * VFNUMQ value should be set to (number of queues - 1). A value
		 * of 0 means 1 queue and a value of 255 means 256 queues
		 */
		reg = (((vsi->txq_map[0] << VPLAN_TX_QBASE_VFFIRSTQ_S) &
			VPLAN_TX_QBASE_VFFIRSTQ_M) |
		       (((vsi->alloc_txq - 1) << VPLAN_TX_QBASE_VFNUMQ_S) &
			VPLAN_TX_QBASE_VFNUMQ_M));
		wr32(hw, VPLAN_TX_QBASE(vf->vf_id), reg);
	} else {
		dev_err(dev, "Scattered mode for VF Tx queues is not yet implemented\n");
	}

	/* set regardless of mapping mode */
	wr32(hw, VPLAN_RXQ_MAPENA(vf->vf_id), VPLAN_RXQ_MAPENA_RX_ENA_M);

	/* VF Rx queues allocation */
	if (vsi->rx_mapping_mode == ICE_VSI_MAP_CONTIG) {
		/* set the VF PF Rx queue range
		 * VFNUMQ value should be set to (number of queues - 1). A value
		 * of 0 means 1 queue and a value of 255 means 256 queues
		 */
		reg = (((vsi->rxq_map[0] << VPLAN_RX_QBASE_VFFIRSTQ_S) &
			VPLAN_RX_QBASE_VFFIRSTQ_M) |
		       (((vsi->alloc_txq - 1) << VPLAN_RX_QBASE_VFNUMQ_S) &
			VPLAN_RX_QBASE_VFNUMQ_M));
		wr32(hw, VPLAN_RX_QBASE(vf->vf_id), reg);
	} else {
		dev_err(dev, "Scattered mode for VF Rx queues is not yet implemented\n");
	}
}

/**
 * ice_determine_res
 * @pf: pointer to the PF structure
 * @avail_res: available resources in the PF structure
 * @max_res: maximum resources that can be given per VF
 * @min_res: minimum resources that can be given per VF
 *
 * Returns non-zero value if resources (queues/vectors) are available or
 * returns zero if PF cannot accommodate for all num_alloc_vfs.
 */
static int
ice_determine_res(struct ice_pf *pf, u16 avail_res, u16 max_res, u16 min_res)
{
	bool checked_min_res = false;
	int res;

	/* start by checking if PF can assign max number of resources for
	 * all num_alloc_vfs.
	 * if yes, return number per VF
	 * If no, divide by 2 and roundup, check again
	 * repeat the loop till we reach a point where even minimum resources
	 * are not available, in that case return 0
	 */
	res = max_res;
	while ((res >= min_res) && !checked_min_res) {
		int num_all_res;

		num_all_res = pf->num_alloc_vfs * res;
		if (num_all_res <= avail_res)
			return res;

		if (res == min_res)
			checked_min_res = true;

		res = DIV_ROUND_UP(res, 2);
	}
	return 0;
}

/**
 * ice_calc_vf_reg_idx - Calculate the VF's register index in the PF space
 * @vf: VF to calculate the register index for
 * @q_vector: a q_vector associated to the VF
 */
int ice_calc_vf_reg_idx(struct ice_vf *vf, struct ice_q_vector *q_vector)
{
	struct ice_pf *pf;

	if (!vf || !q_vector)
		return -EINVAL;

	pf = vf->pf;

	/* always add one to account for the OICR being the first MSIX */
	return pf->sriov_base_vector + pf->num_msix_per_vf * vf->vf_id +
		q_vector->v_idx + 1;
}

/**
 * ice_get_max_valid_res_idx - Get the max valid resource index
 * @res: pointer to the resource to find the max valid index for
 *
 * Start from the end of the ice_res_tracker and return right when we find the
 * first res->list entry with the ICE_RES_VALID_BIT set. This function is only
 * valid for SR-IOV because it is the only consumer that manipulates the
 * res->end and this is always called when res->end is set to res->num_entries.
 */
static int ice_get_max_valid_res_idx(struct ice_res_tracker *res)
{
	int i;

	if (!res)
		return -EINVAL;

	for (i = res->num_entries - 1; i >= 0; i--)
		if (res->list[i] & ICE_RES_VALID_BIT)
			return i;

	return 0;
}

/**
 * ice_sriov_set_msix_res - Set any used MSIX resources
 * @pf: pointer to PF structure
 * @num_msix_needed: number of MSIX vectors needed for all SR-IOV VFs
 *
 * This function allows SR-IOV resources to be taken from the end of the PF's
 * allowed HW MSIX vectors so that the irq_tracker will not be affected. We
 * just set the pf->sriov_base_vector and return success.
 *
 * If there are not enough resources available, return an error. This should
 * always be caught by ice_set_per_vf_res().
 *
 * Return 0 on success, and -EINVAL when there are not enough MSIX vectors in
 * in the PF's space available for SR-IOV.
 */
static int ice_sriov_set_msix_res(struct ice_pf *pf, u16 num_msix_needed)
{
	u16 total_vectors = pf->hw.func_caps.common_cap.num_msix_vectors;
	int vectors_used = pf->irq_tracker->num_entries;
	int sriov_base_vector;

	sriov_base_vector = total_vectors - num_msix_needed;

	/* make sure we only grab irq_tracker entries from the list end and
	 * that we have enough available MSIX vectors
	 */
	if (sriov_base_vector < vectors_used)
		return -EINVAL;

	pf->sriov_base_vector = sriov_base_vector;

	return 0;
}

/**
 * ice_set_per_vf_res - check if vectors and queues are available
 * @pf: pointer to the PF structure
 *
 * First, determine HW interrupts from common pool. If we allocate fewer VFs, we
 * get more vectors and can enable more queues per VF. Note that this does not
 * grab any vectors from the SW pool already allocated. Also note, that all
 * vector counts include one for each VF's miscellaneous interrupt vector
 * (i.e. OICR).
 *
 * Minimum VFs - 2 vectors, 1 queue pair
 * Small VFs - 5 vectors, 4 queue pairs
 * Medium VFs - 17 vectors, 16 queue pairs
 *
 * Second, determine number of queue pairs per VF by starting with a pre-defined
 * maximum each VF supports. If this is not possible, then we adjust based on
 * queue pairs available on the device.
 *
 * Lastly, set queue and MSI-X VF variables tracked by the PF so it can be used
 * by each VF during VF initialization and reset.
 */
static int ice_set_per_vf_res(struct ice_pf *pf)
{
	int max_valid_res_idx = ice_get_max_valid_res_idx(pf->irq_tracker);
	int msix_avail_per_vf, msix_avail_for_sriov;
	struct device *dev = ice_pf_to_dev(pf);
	u16 num_msix_per_vf, num_txq, num_rxq;

	if (!pf->num_alloc_vfs || max_valid_res_idx < 0)
		return -EINVAL;

	/* determine MSI-X resources per VF */
	msix_avail_for_sriov = pf->hw.func_caps.common_cap.num_msix_vectors -
		pf->irq_tracker->num_entries;
	msix_avail_per_vf = msix_avail_for_sriov / pf->num_alloc_vfs;
	if (msix_avail_per_vf >= ICE_NUM_VF_MSIX_MED) {
		num_msix_per_vf = ICE_NUM_VF_MSIX_MED;
	} else if (msix_avail_per_vf >= ICE_NUM_VF_MSIX_SMALL) {
		num_msix_per_vf = ICE_NUM_VF_MSIX_SMALL;
	} else if (msix_avail_per_vf >= ICE_MIN_INTR_PER_VF) {
		num_msix_per_vf = ICE_MIN_INTR_PER_VF;
	} else {
		dev_err(dev, "Only %d MSI-X interrupts available for SR-IOV. Not enough to support minimum of %d MSI-X interrupts per VF for %d VFs\n",
			msix_avail_for_sriov, ICE_MIN_INTR_PER_VF,
			pf->num_alloc_vfs);
		return -EIO;
	}

	/* determine queue resources per VF */
	num_txq = ice_determine_res(pf, ice_get_avail_txq_count(pf),
				    min_t(u16,
					  num_msix_per_vf - ICE_NONQ_VECS_VF,
					  ICE_MAX_RSS_QS_PER_VF),
				    ICE_MIN_QS_PER_VF);

	num_rxq = ice_determine_res(pf, ice_get_avail_rxq_count(pf),
				    min_t(u16,
					  num_msix_per_vf - ICE_NONQ_VECS_VF,
					  ICE_MAX_RSS_QS_PER_VF),
				    ICE_MIN_QS_PER_VF);

	if (!num_txq || !num_rxq) {
		dev_err(dev, "Not enough queues to support minimum of %d queue pairs per VF for %d VFs\n",
			ICE_MIN_QS_PER_VF, pf->num_alloc_vfs);
		return -EIO;
	}

	if (ice_sriov_set_msix_res(pf, num_msix_per_vf * pf->num_alloc_vfs)) {
		dev_err(dev, "Unable to set MSI-X resources for %d VFs\n",
			pf->num_alloc_vfs);
		return -EINVAL;
	}

	/* only allow equal Tx/Rx queue count (i.e. queue pairs) */
	pf->num_qps_per_vf = min_t(int, num_txq, num_rxq);
	pf->num_msix_per_vf = num_msix_per_vf;
	dev_info(dev, "Enabling %d VFs with %d vectors and %d queues per VF\n",
		 pf->num_alloc_vfs, pf->num_msix_per_vf, pf->num_qps_per_vf);

	return 0;
}

/**
 * ice_cleanup_and_realloc_vf - Clean up VF and reallocate resources after reset
 * @vf: pointer to the VF structure
 *
 * Cleanup a VF after the hardware reset is finished. Expects the caller to
 * have verified whether the reset is finished properly, and ensure the
 * minimum amount of wait time has passed. Reallocate VF resources back to make
 * VF state active
 */
static void ice_cleanup_and_realloc_vf(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;
	struct ice_hw *hw;
	u32 reg;

	hw = &pf->hw;

	/* PF software completes the flow by notifying VF that reset flow is
	 * completed. This is done by enabling hardware by clearing the reset
	 * bit in the VPGEN_VFRTRIG reg and setting VFR_STATE in the VFGEN_RSTAT
	 * register to VFR completed (done at the end of this function)
	 * By doing this we allow HW to access VF memory at any point. If we
	 * did it any sooner, HW could access memory while it was being freed
	 * in ice_free_vf_res(), causing an IOMMU fault.
	 *
	 * On the other hand, this needs to be done ASAP, because the VF driver
	 * is waiting for this to happen and may report a timeout. It's
	 * harmless, but it gets logged into Guest OS kernel log, so best avoid
	 * it.
	 */
	reg = rd32(hw, VPGEN_VFRTRIG(vf->vf_id));
	reg &= ~VPGEN_VFRTRIG_VFSWR_M;
	wr32(hw, VPGEN_VFRTRIG(vf->vf_id), reg);

	/* reallocate VF resources to finish resetting the VSI state */
	if (!ice_alloc_vf_res(vf)) {
		ice_ena_vf_mappings(vf);
		set_bit(ICE_VF_STATE_ACTIVE, vf->vf_states);
		clear_bit(ICE_VF_STATE_DIS, vf->vf_states);
	}

	/* Tell the VF driver the reset is done. This needs to be done only
	 * after VF has been fully initialized, because the VF driver may
	 * request resources immediately after setting this flag.
	 */
	wr32(hw, VFGEN_RSTAT(vf->vf_id), VIRTCHNL_VFR_VFACTIVE);
}

/**
 * ice_vf_set_vsi_promisc - set given VF VSI to given promiscuous mode(s)
 * @vf: pointer to the VF info
 * @vsi: the VSI being configured
 * @promisc_m: mask of promiscuous config bits
 * @rm_promisc: promisc flag request from the VF to remove or add filter
 *
 * This function configures VF VSI promiscuous mode, based on the VF requests,
 * for Unicast, Multicast and VLAN
 */
static enum ice_status
ice_vf_set_vsi_promisc(struct ice_vf *vf, struct ice_vsi *vsi, u8 promisc_m,
		       bool rm_promisc)
{
	struct ice_pf *pf = vf->pf;
	enum ice_status status = 0;
	struct ice_hw *hw;

	hw = &pf->hw;
	if (vsi->num_vlan) {
		status = ice_set_vlan_vsi_promisc(hw, vsi->idx, promisc_m,
						  rm_promisc);
	} else if (vf->port_vlan_info) {
		if (rm_promisc)
			status = ice_clear_vsi_promisc(hw, vsi->idx, promisc_m,
						       vf->port_vlan_info);
		else
			status = ice_set_vsi_promisc(hw, vsi->idx, promisc_m,
						     vf->port_vlan_info);
	} else {
		if (rm_promisc)
			status = ice_clear_vsi_promisc(hw, vsi->idx, promisc_m,
						       0);
		else
			status = ice_set_vsi_promisc(hw, vsi->idx, promisc_m,
						     0);
	}

	return status;
}

/**
 * ice_config_res_vfs - Finalize allocation of VFs resources in one go
 * @pf: pointer to the PF structure
 *
 * This function is being called as last part of resetting all VFs, or when
 * configuring VFs for the first time, where there is no resource to be freed
 * Returns true if resources were properly allocated for all VFs, and false
 * otherwise.
 */
static bool ice_config_res_vfs(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	struct ice_hw *hw = &pf->hw;
	int v;

	if (ice_set_per_vf_res(pf)) {
		dev_err(dev, "Cannot allocate VF resources, try with fewer number of VFs\n");
		return false;
	}

	/* rearm global interrupts */
	if (test_and_clear_bit(__ICE_OICR_INTR_DIS, pf->state))
		ice_irq_dynamic_ena(hw, NULL, NULL);

	/* Finish resetting each VF and allocate resources */
	ice_for_each_vf(pf, v) {
		struct ice_vf *vf = &pf->vf[v];

		vf->num_vf_qs = pf->num_qps_per_vf;
		dev_dbg(dev, "VF-id %d has %d queues configured\n", vf->vf_id,
			vf->num_vf_qs);
		ice_cleanup_and_realloc_vf(vf);
	}

	ice_flush(hw);
	clear_bit(__ICE_VF_DIS, pf->state);

	return true;
}

/**
 * ice_reset_all_vfs - reset all allocated VFs in one go
 * @pf: pointer to the PF structure
 * @is_vflr: true if VFLR was issued, false if not
 *
 * First, tell the hardware to reset each VF, then do all the waiting in one
 * chunk, and finally finish restoring each VF after the wait. This is useful
 * during PF routines which need to reset all VFs, as otherwise it must perform
 * these resets in a serialized fashion.
 *
 * Returns true if any VFs were reset, and false otherwise.
 */
bool ice_reset_all_vfs(struct ice_pf *pf, bool is_vflr)
{
	struct device *dev = ice_pf_to_dev(pf);
	struct ice_hw *hw = &pf->hw;
	struct ice_vf *vf;
	int v, i;

	/* If we don't have any VFs, then there is nothing to reset */
	if (!pf->num_alloc_vfs)
		return false;

	/* If VFs have been disabled, there is no need to reset */
	if (test_and_set_bit(__ICE_VF_DIS, pf->state))
		return false;

	/* Begin reset on all VFs at once */
	ice_for_each_vf(pf, v)
		ice_trigger_vf_reset(&pf->vf[v], is_vflr, true);

	ice_for_each_vf(pf, v) {
		struct ice_vsi *vsi;

		vf = &pf->vf[v];
		vsi = pf->vsi[vf->lan_vsi_idx];
		if (test_bit(ICE_VF_STATE_QS_ENA, vf->vf_states))
			ice_dis_vf_qs(vf);
		ice_dis_vsi_txq(vsi->port_info, vsi->idx, 0, 0, NULL, NULL,
				NULL, ICE_VF_RESET, vf->vf_id, NULL);
	}

	/* HW requires some time to make sure it can flush the FIFO for a VF
	 * when it resets it. Poll the VPGEN_VFRSTAT register for each VF in
	 * sequence to make sure that it has completed. We'll keep track of
	 * the VFs using a simple iterator that increments once that VF has
	 * finished resetting.
	 */
	for (i = 0, v = 0; i < 10 && v < pf->num_alloc_vfs; i++) {
		/* Check each VF in sequence */
		while (v < pf->num_alloc_vfs) {
			u32 reg;

			vf = &pf->vf[v];
			reg = rd32(hw, VPGEN_VFRSTAT(vf->vf_id));
			if (!(reg & VPGEN_VFRSTAT_VFRD_M)) {
				/* only delay if the check failed */
				usleep_range(10, 20);
				break;
			}

			/* If the current VF has finished resetting, move on
			 * to the next VF in sequence.
			 */
			v++;
		}
	}

	/* Display a warning if at least one VF didn't manage to reset in
	 * time, but continue on with the operation.
	 */
	if (v < pf->num_alloc_vfs)
		dev_warn(dev, "VF reset check timeout\n");

	/* free VF resources to begin resetting the VSI state */
	ice_for_each_vf(pf, v) {
		vf = &pf->vf[v];

		ice_free_vf_res(vf);

		/* Free VF queues as well, and reallocate later.
		 * If a given VF has different number of queues
		 * configured, the request for update will come
		 * via mailbox communication.
		 */
		vf->num_vf_qs = 0;
	}

	if (ice_sriov_free_msix_res(pf))
		dev_err(dev, "Failed to free MSIX resources used by SR-IOV\n");

	if (!ice_config_res_vfs(pf))
		return false;

	return true;
}

/**
 * ice_is_vf_disabled
 * @vf: pointer to the VF info
 *
 * Returns true if the PF or VF is disabled, false otherwise.
 */
static bool ice_is_vf_disabled(struct ice_vf *vf)
{
	struct ice_pf *pf = vf->pf;

	/* If the PF has been disabled, there is no need resetting VF until
	 * PF is active again. Similarly, if the VF has been disabled, this
	 * means something else is resetting the VF, so we shouldn't continue.
	 * Otherwise, set disable VF state bit for actual reset, and continue.
	 */
	return (test_bit(__ICE_VF_DIS, pf->state) ||
		test_bit(ICE_VF_STATE_DIS, vf->vf_states));
}

/**
 * ice_reset_vf - Reset a particular VF
 * @vf: pointer to the VF structure
 * @is_vflr: true if VFLR was issued, false if not
 *
 * Returns true if the VF is currently in reset, resets successfully, or resets
 * are disabled and false otherwise.
 */
bool ice_reset_vf(struct ice_vf *vf, bool is_vflr)
{
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;
	struct device *dev;
	struct ice_hw *hw;
	bool rsd = false;
	u8 promisc_m;
	u32 reg;
	int i;

	dev = ice_pf_to_dev(pf);

	if (test_bit(__ICE_VF_RESETS_DISABLED, pf->state)) {
		dev_dbg(dev, "Trying to reset VF %d, but all VF resets are disabled\n",
			vf->vf_id);
		return true;
	}

	if (ice_is_vf_disabled(vf)) {
		dev_dbg(dev, "VF is already disabled, there is no need for resetting it, telling VM, all is fine %d\n",
			vf->vf_id);
		return true;
	}

	/* Set VF disable bit state here, before triggering reset */
	set_bit(ICE_VF_STATE_DIS, vf->vf_states);
	ice_trigger_vf_reset(vf, is_vflr, false);

	vsi = pf->vsi[vf->lan_vsi_idx];

	if (test_bit(ICE_VF_STATE_QS_ENA, vf->vf_states))
		ice_dis_vf_qs(vf);

	/* Call Disable LAN Tx queue AQ whether or not queues are
	 * enabled. This is needed for successful completion of VFR.
	 */
	ice_dis_vsi_txq(vsi->port_info, vsi->idx, 0, 0, NULL, NULL,
			NULL, ICE_VF_RESET, vf->vf_id, NULL);

	hw = &pf->hw;
	/* poll VPGEN_VFRSTAT reg to make sure
	 * that reset is complete
	 */
	for (i = 0; i < 10; i++) {
		/* VF reset requires driver to first reset the VF and then
		 * poll the status register to make sure that the reset
		 * completed successfully.
		 */
		reg = rd32(hw, VPGEN_VFRSTAT(vf->vf_id));
		if (reg & VPGEN_VFRSTAT_VFRD_M) {
			rsd = true;
			break;
		}

		/* only sleep if the reset is not done */
		usleep_range(10, 20);
	}

	/* Display a warning if VF didn't manage to reset in time, but need to
	 * continue on with the operation.
	 */
	if (!rsd)
		dev_warn(dev, "VF reset check timeout on VF %d\n", vf->vf_id);

	/* disable promiscuous modes in case they were enabled
	 * ignore any error if disabling process failed
	 */
	if (test_bit(ICE_VF_STATE_UC_PROMISC, vf->vf_states) ||
	    test_bit(ICE_VF_STATE_MC_PROMISC, vf->vf_states)) {
		if (vf->port_vlan_info || vsi->num_vlan)
			promisc_m = ICE_UCAST_VLAN_PROMISC_BITS;
		else
			promisc_m = ICE_UCAST_PROMISC_BITS;

		vsi = pf->vsi[vf->lan_vsi_idx];
		if (ice_vf_set_vsi_promisc(vf, vsi, promisc_m, true))
			dev_err(dev, "disabling promiscuous mode failed\n");
	}

	/* free VF resources to begin resetting the VSI state */
	ice_free_vf_res(vf);

	ice_cleanup_and_realloc_vf(vf);

	ice_flush(hw);

	return true;
}

/**
 * ice_vc_notify_link_state - Inform all VFs on a PF of link status
 * @pf: pointer to the PF structure
 */
void ice_vc_notify_link_state(struct ice_pf *pf)
{
	int i;

	ice_for_each_vf(pf, i)
		ice_vc_notify_vf_link_state(&pf->vf[i]);
}

/**
 * ice_vc_notify_reset - Send pending reset message to all VFs
 * @pf: pointer to the PF structure
 *
 * indicate a pending reset to all VFs on a given PF
 */
void ice_vc_notify_reset(struct ice_pf *pf)
{
	struct virtchnl_pf_event pfe;

	if (!pf->num_alloc_vfs)
		return;

	pfe.event = VIRTCHNL_EVENT_RESET_IMPENDING;
	pfe.severity = PF_EVENT_SEVERITY_CERTAIN_DOOM;
	ice_vc_vf_broadcast(pf, VIRTCHNL_OP_EVENT, VIRTCHNL_STATUS_SUCCESS,
			    (u8 *)&pfe, sizeof(struct virtchnl_pf_event));
}

/**
 * ice_vc_notify_vf_reset - Notify VF of a reset event
 * @vf: pointer to the VF structure
 */
static void ice_vc_notify_vf_reset(struct ice_vf *vf)
{
	struct virtchnl_pf_event pfe;
	struct ice_pf *pf;

	if (!vf)
		return;

	pf = vf->pf;
	if (ice_validate_vf_id(pf, vf->vf_id))
		return;

	/* Bail out if VF is in disabled state, neither initialized, nor active
	 * state - otherwise proceed with notifications
	 */
	if ((!test_bit(ICE_VF_STATE_INIT, vf->vf_states) &&
	     !test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) ||
	    test_bit(ICE_VF_STATE_DIS, vf->vf_states))
		return;

	pfe.event = VIRTCHNL_EVENT_RESET_IMPENDING;
	pfe.severity = PF_EVENT_SEVERITY_CERTAIN_DOOM;
	ice_aq_send_msg_to_vf(&pf->hw, vf->vf_id, VIRTCHNL_OP_EVENT,
			      VIRTCHNL_STATUS_SUCCESS, (u8 *)&pfe, sizeof(pfe),
			      NULL);
}

/**
 * ice_alloc_vfs - Allocate and set up VFs resources
 * @pf: pointer to the PF structure
 * @num_alloc_vfs: number of VFs to allocate
 */
static int ice_alloc_vfs(struct ice_pf *pf, u16 num_alloc_vfs)
{
	struct device *dev = ice_pf_to_dev(pf);
	struct ice_hw *hw = &pf->hw;
	struct ice_vf *vfs;
	int i, ret;

	/* Disable global interrupt 0 so we don't try to handle the VFLR. */
	wr32(hw, GLINT_DYN_CTL(pf->oicr_idx),
	     ICE_ITR_NONE << GLINT_DYN_CTL_ITR_INDX_S);
	set_bit(__ICE_OICR_INTR_DIS, pf->state);
	ice_flush(hw);

	ret = pci_enable_sriov(pf->pdev, num_alloc_vfs);
	if (ret) {
		pf->num_alloc_vfs = 0;
		goto err_unroll_intr;
	}
	/* allocate memory */
	vfs = devm_kcalloc(dev, num_alloc_vfs, sizeof(*vfs), GFP_KERNEL);
	if (!vfs) {
		ret = -ENOMEM;
		goto err_pci_disable_sriov;
	}
	pf->vf = vfs;
	pf->num_alloc_vfs = num_alloc_vfs;

	/* apply default profile */
	ice_for_each_vf(pf, i) {
		vfs[i].pf = pf;
		vfs[i].vf_sw_id = pf->first_sw;
		vfs[i].vf_id = i;

		/* assign default capabilities */
		set_bit(ICE_VIRTCHNL_VF_CAP_L2, &vfs[i].vf_caps);
		vfs[i].spoofchk = true;
	}

	/* VF resources get allocated with initialization */
	if (!ice_config_res_vfs(pf)) {
		ret = -EIO;
		goto err_unroll_sriov;
	}

	return ret;

err_unroll_sriov:
	pf->vf = NULL;
	devm_kfree(dev, vfs);
	vfs = NULL;
	pf->num_alloc_vfs = 0;
err_pci_disable_sriov:
	pci_disable_sriov(pf->pdev);
err_unroll_intr:
	/* rearm interrupts here */
	ice_irq_dynamic_ena(hw, NULL, NULL);
	clear_bit(__ICE_OICR_INTR_DIS, pf->state);
	return ret;
}

/**
 * ice_pf_state_is_nominal - checks the PF for nominal state
 * @pf: pointer to PF to check
 *
 * Check the PF's state for a collection of bits that would indicate
 * the PF is in a state that would inhibit normal operation for
 * driver functionality.
 *
 * Returns true if PF is in a nominal state.
 * Returns false otherwise
 */
static bool ice_pf_state_is_nominal(struct ice_pf *pf)
{
	DECLARE_BITMAP(check_bits, __ICE_STATE_NBITS) = { 0 };

	if (!pf)
		return false;

	bitmap_set(check_bits, 0, __ICE_STATE_NOMINAL_CHECK_BITS);
	if (bitmap_intersects(pf->state, check_bits, __ICE_STATE_NBITS))
		return false;

	return true;
}

/**
 * ice_pci_sriov_ena - Enable or change number of VFs
 * @pf: pointer to the PF structure
 * @num_vfs: number of VFs to allocate
 */
static int ice_pci_sriov_ena(struct ice_pf *pf, int num_vfs)
{
	int pre_existing_vfs = pci_num_vf(pf->pdev);
	struct device *dev = ice_pf_to_dev(pf);
	int err;

	if (!ice_pf_state_is_nominal(pf)) {
		dev_err(dev, "Cannot enable SR-IOV, device not ready\n");
		return -EBUSY;
	}

	if (!test_bit(ICE_FLAG_SRIOV_CAPABLE, pf->flags)) {
		dev_err(dev, "This device is not capable of SR-IOV\n");
		return -EOPNOTSUPP;
	}

	if (pre_existing_vfs && pre_existing_vfs != num_vfs)
		ice_free_vfs(pf);
	else if (pre_existing_vfs && pre_existing_vfs == num_vfs)
		return num_vfs;

	if (num_vfs > pf->num_vfs_supported) {
		dev_err(dev, "Can't enable %d VFs, max VFs supported is %d\n",
			num_vfs, pf->num_vfs_supported);
		return -EOPNOTSUPP;
	}

	dev_info(dev, "Allocating %d VFs\n", num_vfs);
	err = ice_alloc_vfs(pf, num_vfs);
	if (err) {
		dev_err(dev, "Failed to enable SR-IOV: %d\n", err);
		return err;
	}

	set_bit(ICE_FLAG_SRIOV_ENA, pf->flags);
	return num_vfs;
}

/**
 * ice_sriov_configure - Enable or change number of VFs via sysfs
 * @pdev: pointer to a pci_dev structure
 * @num_vfs: number of VFs to allocate
 *
 * This function is called when the user updates the number of VFs in sysfs.
 */
int ice_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	struct ice_pf *pf = pci_get_drvdata(pdev);
	struct device *dev = ice_pf_to_dev(pf);

	if (ice_is_safe_mode(pf)) {
		dev_err(dev, "SR-IOV cannot be configured - Device is in Safe Mode\n");
		return -EOPNOTSUPP;
	}

	if (num_vfs)
		return ice_pci_sriov_ena(pf, num_vfs);

	if (!pci_vfs_assigned(pdev)) {
		ice_free_vfs(pf);
	} else {
		dev_err(dev, "can't free VFs because some are assigned to VMs.\n");
		return -EBUSY;
	}

	return 0;
}

/**
 * ice_process_vflr_event - Free VF resources via IRQ calls
 * @pf: pointer to the PF structure
 *
 * called from the VFLR IRQ handler to
 * free up VF resources and state variables
 */
void ice_process_vflr_event(struct ice_pf *pf)
{
	struct ice_hw *hw = &pf->hw;
	unsigned int vf_id;
	u32 reg;

	if (!test_and_clear_bit(__ICE_VFLR_EVENT_PENDING, pf->state) ||
	    !pf->num_alloc_vfs)
		return;

	ice_for_each_vf(pf, vf_id) {
		struct ice_vf *vf = &pf->vf[vf_id];
		u32 reg_idx, bit_idx;

		reg_idx = (hw->func_caps.vf_base_id + vf_id) / 32;
		bit_idx = (hw->func_caps.vf_base_id + vf_id) % 32;
		/* read GLGEN_VFLRSTAT register to find out the flr VFs */
		reg = rd32(hw, GLGEN_VFLRSTAT(reg_idx));
		if (reg & BIT(bit_idx))
			/* GLGEN_VFLRSTAT bit will be cleared in ice_reset_vf */
			ice_reset_vf(vf, true);
	}
}

/**
 * ice_vc_reset_vf - Perform software reset on the VF after informing the AVF
 * @vf: pointer to the VF info
 */
static void ice_vc_reset_vf(struct ice_vf *vf)
{
	ice_vc_notify_vf_reset(vf);
	ice_reset_vf(vf, false);
}

/**
 * ice_get_vf_from_pfq - get the VF who owns the PF space queue passed in
 * @pf: PF used to index all VFs
 * @pfq: queue index relative to the PF's function space
 *
 * If no VF is found who owns the pfq then return NULL, otherwise return a
 * pointer to the VF who owns the pfq
 */
static struct ice_vf *ice_get_vf_from_pfq(struct ice_pf *pf, u16 pfq)
{
	unsigned int vf_id;

	ice_for_each_vf(pf, vf_id) {
		struct ice_vf *vf = &pf->vf[vf_id];
		struct ice_vsi *vsi;
		u16 rxq_idx;

		vsi = pf->vsi[vf->lan_vsi_idx];

		ice_for_each_rxq(vsi, rxq_idx)
			if (vsi->rxq_map[rxq_idx] == pfq)
				return vf;
	}

	return NULL;
}

/**
 * ice_globalq_to_pfq - convert from global queue index to PF space queue index
 * @pf: PF used for conversion
 * @globalq: global queue index used to convert to PF space queue index
 */
static u32 ice_globalq_to_pfq(struct ice_pf *pf, u32 globalq)
{
	return globalq - pf->hw.func_caps.common_cap.rxq_first_id;
}

/**
 * ice_vf_lan_overflow_event - handle LAN overflow event for a VF
 * @pf: PF that the LAN overflow event happened on
 * @event: structure holding the event information for the LAN overflow event
 *
 * Determine if the LAN overflow event was caused by a VF queue. If it was not
 * caused by a VF, do nothing. If a VF caused this LAN overflow event trigger a
 * reset on the offending VF.
 */
void
ice_vf_lan_overflow_event(struct ice_pf *pf, struct ice_rq_event_info *event)
{
	u32 gldcb_rtctq, queue;
	struct ice_vf *vf;

	gldcb_rtctq = le32_to_cpu(event->desc.params.lan_overflow.prtdcb_ruptq);
	dev_dbg(ice_pf_to_dev(pf), "GLDCB_RTCTQ: 0x%08x\n", gldcb_rtctq);

	/* event returns device global Rx queue number */
	queue = (gldcb_rtctq & GLDCB_RTCTQ_RXQNUM_M) >>
		GLDCB_RTCTQ_RXQNUM_S;

	vf = ice_get_vf_from_pfq(pf, ice_globalq_to_pfq(pf, queue));
	if (!vf)
		return;

	ice_vc_reset_vf(vf);
}

/**
 * ice_vc_send_msg_to_vf - Send message to VF
 * @vf: pointer to the VF info
 * @v_opcode: virtual channel opcode
 * @v_retval: virtual channel return value
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * send msg to VF
 */
static int
ice_vc_send_msg_to_vf(struct ice_vf *vf, u32 v_opcode,
		      enum virtchnl_status_code v_retval, u8 *msg, u16 msglen)
{
	enum ice_status aq_ret;
	struct device *dev;
	struct ice_pf *pf;

	if (!vf)
		return -EINVAL;

	pf = vf->pf;
	if (ice_validate_vf_id(pf, vf->vf_id))
		return -EINVAL;

	dev = ice_pf_to_dev(pf);

	/* single place to detect unsuccessful return values */
	if (v_retval) {
		vf->num_inval_msgs++;
		dev_info(dev, "VF %d failed opcode %d, retval: %d\n", vf->vf_id,
			 v_opcode, v_retval);
		if (vf->num_inval_msgs > ICE_DFLT_NUM_INVAL_MSGS_ALLOWED) {
			dev_err(dev, "Number of invalid messages exceeded for VF %d\n",
				vf->vf_id);
			dev_err(dev, "Use PF Control I/F to enable the VF\n");
			set_bit(ICE_VF_STATE_DIS, vf->vf_states);
			return -EIO;
		}
	} else {
		vf->num_valid_msgs++;
		/* reset the invalid counter, if a valid message is received. */
		vf->num_inval_msgs = 0;
	}

	aq_ret = ice_aq_send_msg_to_vf(&pf->hw, vf->vf_id, v_opcode, v_retval,
				       msg, msglen, NULL);
	if (aq_ret && pf->hw.mailboxq.sq_last_status != ICE_AQ_RC_ENOSYS) {
		dev_info(dev, "Unable to send the message to VF %d ret %d aq_err %d\n",
			 vf->vf_id, aq_ret, pf->hw.mailboxq.sq_last_status);
		return -EIO;
	}

	return 0;
}

/**
 * ice_vc_get_ver_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to request the API version used by the PF
 */
static int ice_vc_get_ver_msg(struct ice_vf *vf, u8 *msg)
{
	struct virtchnl_version_info info = {
		VIRTCHNL_VERSION_MAJOR, VIRTCHNL_VERSION_MINOR
	};

	vf->vf_ver = *(struct virtchnl_version_info *)msg;
	/* VFs running the 1.0 API expect to get 1.0 back or they will cry. */
	if (VF_IS_V10(&vf->vf_ver))
		info.minor = VIRTCHNL_VERSION_MINOR_NO_VF_CAPS;

	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_VERSION,
				     VIRTCHNL_STATUS_SUCCESS, (u8 *)&info,
				     sizeof(struct virtchnl_version_info));
}

/**
 * ice_vc_get_vf_res_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to request its resources
 */
static int ice_vc_get_vf_res_msg(struct ice_vf *vf, u8 *msg)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_vf_resource *vfres = NULL;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;
	int len = 0;
	int ret;

	if (ice_check_vf_init(pf, vf)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto err;
	}

	len = sizeof(struct virtchnl_vf_resource);

	vfres = kzalloc(len, GFP_KERNEL);
	if (!vfres) {
		v_ret = VIRTCHNL_STATUS_ERR_NO_MEMORY;
		len = 0;
		goto err;
	}
	if (VF_IS_V11(&vf->vf_ver))
		vf->driver_caps = *(u32 *)msg;
	else
		vf->driver_caps = VIRTCHNL_VF_OFFLOAD_L2 |
				  VIRTCHNL_VF_OFFLOAD_RSS_REG |
				  VIRTCHNL_VF_OFFLOAD_VLAN;

	vfres->vf_cap_flags = VIRTCHNL_VF_OFFLOAD_L2;
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto err;
	}

	if (!vsi->info.pvid)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_VLAN;

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_PF) {
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_PF;
	} else {
		if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_AQ)
			vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_AQ;
		else
			vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_REG;
	}

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2;

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_ENCAP)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_ENCAP;

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM;

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RX_POLLING)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RX_POLLING;

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_WB_ON_ITR)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_WB_ON_ITR;

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_REQ_QUEUES)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_REQ_QUEUES;

	if (vf->driver_caps & VIRTCHNL_VF_CAP_ADV_LINK_SPEED)
		vfres->vf_cap_flags |= VIRTCHNL_VF_CAP_ADV_LINK_SPEED;

	vfres->num_vsis = 1;
	/* Tx and Rx queue are equal for VF */
	vfres->num_queue_pairs = vsi->num_txq;
	vfres->max_vectors = pf->num_msix_per_vf;
	vfres->rss_key_size = ICE_VSIQF_HKEY_ARRAY_SIZE;
	vfres->rss_lut_size = ICE_VSIQF_HLUT_ARRAY_SIZE;

	vfres->vsi_res[0].vsi_id = vf->lan_vsi_num;
	vfres->vsi_res[0].vsi_type = VIRTCHNL_VSI_SRIOV;
	vfres->vsi_res[0].num_queue_pairs = vsi->num_txq;
	ether_addr_copy(vfres->vsi_res[0].default_mac_addr,
			vf->dflt_lan_addr.addr);

	/* match guest capabilities */
	vf->driver_caps = vfres->vf_cap_flags;

	set_bit(ICE_VF_STATE_ACTIVE, vf->vf_states);

err:
	/* send the response back to the VF */
	ret = ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_GET_VF_RESOURCES, v_ret,
				    (u8 *)vfres, len);

	kfree(vfres);
	return ret;
}

/**
 * ice_vc_reset_vf_msg
 * @vf: pointer to the VF info
 *
 * called from the VF to reset itself,
 * unlike other virtchnl messages, PF driver
 * doesn't send the response back to the VF
 */
static void ice_vc_reset_vf_msg(struct ice_vf *vf)
{
	if (test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states))
		ice_reset_vf(vf, false);
}

/**
 * ice_find_vsi_from_id
 * @pf: the PF structure to search for the VSI
 * @id: ID of the VSI it is searching for
 *
 * searches for the VSI with the given ID
 */
static struct ice_vsi *ice_find_vsi_from_id(struct ice_pf *pf, u16 id)
{
	int i;

	ice_for_each_vsi(pf, i)
		if (pf->vsi[i] && pf->vsi[i]->vsi_num == id)
			return pf->vsi[i];

	return NULL;
}

/**
 * ice_vc_isvalid_vsi_id
 * @vf: pointer to the VF info
 * @vsi_id: VF relative VSI ID
 *
 * check for the valid VSI ID
 */
static bool ice_vc_isvalid_vsi_id(struct ice_vf *vf, u16 vsi_id)
{
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;

	vsi = ice_find_vsi_from_id(pf, vsi_id);

	return (vsi && (vsi->vf_id == vf->vf_id));
}

/**
 * ice_vc_isvalid_q_id
 * @vf: pointer to the VF info
 * @vsi_id: VSI ID
 * @qid: VSI relative queue ID
 *
 * check for the valid queue ID
 */
static bool ice_vc_isvalid_q_id(struct ice_vf *vf, u16 vsi_id, u8 qid)
{
	struct ice_vsi *vsi = ice_find_vsi_from_id(vf->pf, vsi_id);
	/* allocated Tx and Rx queues should be always equal for VF VSI */
	return (vsi && (qid < vsi->alloc_txq));
}

/**
 * ice_vc_isvalid_ring_len
 * @ring_len: length of ring
 *
 * check for the valid ring count, should be multiple of ICE_REQ_DESC_MULTIPLE
 * or zero
 */
static bool ice_vc_isvalid_ring_len(u16 ring_len)
{
	return ring_len == 0 ||
	       (ring_len >= ICE_MIN_NUM_DESC &&
		ring_len <= ICE_MAX_NUM_DESC &&
		!(ring_len % ICE_REQ_DESC_MULTIPLE));
}

/**
 * ice_vc_config_rss_key
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Configure the VF's RSS key
 */
static int ice_vc_config_rss_key(struct ice_vf *vf, u8 *msg)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_rss_key *vrk =
		(struct virtchnl_rss_key *)msg;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_isvalid_vsi_id(vf, vrk->vsi_id)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (vrk->key_len != ICE_VSIQF_HKEY_ARRAY_SIZE) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!test_bit(ICE_FLAG_RSS_ENA, vf->pf->flags)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (ice_set_rss(vsi, vrk->key, NULL, 0))
		v_ret = VIRTCHNL_STATUS_ERR_ADMIN_QUEUE_ERROR;
error_param:
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_CONFIG_RSS_KEY, v_ret,
				     NULL, 0);
}

/**
 * ice_vc_config_rss_lut
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Configure the VF's RSS LUT
 */
static int ice_vc_config_rss_lut(struct ice_vf *vf, u8 *msg)
{
	struct virtchnl_rss_lut *vrl = (struct virtchnl_rss_lut *)msg;
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_isvalid_vsi_id(vf, vrl->vsi_id)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (vrl->lut_entries != ICE_VSIQF_HLUT_ARRAY_SIZE) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!test_bit(ICE_FLAG_RSS_ENA, vf->pf->flags)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (ice_set_rss(vsi, NULL, vrl->lut, ICE_VSIQF_HLUT_ARRAY_SIZE))
		v_ret = VIRTCHNL_STATUS_ERR_ADMIN_QUEUE_ERROR;
error_param:
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_CONFIG_RSS_LUT, v_ret,
				     NULL, 0);
}

/**
 * ice_wait_on_vf_reset - poll to make sure a given VF is ready after reset
 * @vf: The VF being resseting
 *
 * The max poll time is about ~800ms, which is about the maximum time it takes
 * for a VF to be reset and/or a VF driver to be removed.
 */
static void ice_wait_on_vf_reset(struct ice_vf *vf)
{
	int i;

	for (i = 0; i < ICE_MAX_VF_RESET_TRIES; i++) {
		if (test_bit(ICE_VF_STATE_INIT, vf->vf_states))
			break;
		msleep(ICE_MAX_VF_RESET_SLEEP_MS);
	}
}

/**
 * ice_check_vf_ready_for_cfg - check if VF is ready to be configured/queried
 * @vf: VF to check if it's ready to be configured/queried
 *
 * The purpose of this function is to make sure the VF is not in reset, not
 * disabled, and initialized so it can be configured and/or queried by a host
 * administrator.
 */
static int ice_check_vf_ready_for_cfg(struct ice_vf *vf)
{
	struct ice_pf *pf;

	ice_wait_on_vf_reset(vf);

	if (ice_is_vf_disabled(vf))
		return -EINVAL;

	pf = vf->pf;
	if (ice_check_vf_init(pf, vf))
		return -EBUSY;

	return 0;
}

/**
 * ice_set_vf_spoofchk
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @ena: flag to enable or disable feature
 *
 * Enable or disable VF spoof checking
 */
int ice_set_vf_spoofchk(struct net_device *netdev, int vf_id, bool ena)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;
	struct ice_vsi_ctx *ctx;
	struct ice_vsi *vf_vsi;
	enum ice_status status;
	struct device *dev;
	struct ice_vf *vf;
	int ret;

	dev = ice_pf_to_dev(pf);
	if (ice_validate_vf_id(pf, vf_id))
		return -EINVAL;

	vf = &pf->vf[vf_id];
	ret = ice_check_vf_ready_for_cfg(vf);
	if (ret)
		return ret;

	vf_vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vf_vsi) {
		netdev_err(netdev, "VSI %d for VF %d is null\n",
			   vf->lan_vsi_idx, vf->vf_id);
		return -EINVAL;
	}

	if (vf_vsi->type != ICE_VSI_VF) {
		netdev_err(netdev, "Type %d of VSI %d for VF %d is no ICE_VSI_VF\n",
			   vf_vsi->type, vf_vsi->vsi_num, vf->vf_id);
		return -ENODEV;
	}

	if (ena == vf->spoofchk) {
		dev_dbg(dev, "VF spoofchk already %s\n", ena ? "ON" : "OFF");
		return 0;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->info.sec_flags = vf_vsi->info.sec_flags;
	ctx->info.valid_sections = cpu_to_le16(ICE_AQ_VSI_PROP_SECURITY_VALID);
	if (ena) {
		ctx->info.sec_flags |=
			ICE_AQ_VSI_SEC_FLAG_ENA_MAC_ANTI_SPOOF |
			(ICE_AQ_VSI_SEC_TX_VLAN_PRUNE_ENA <<
			 ICE_AQ_VSI_SEC_TX_PRUNE_ENA_S);
	} else {
		ctx->info.sec_flags &=
			~(ICE_AQ_VSI_SEC_FLAG_ENA_MAC_ANTI_SPOOF |
			  (ICE_AQ_VSI_SEC_TX_VLAN_PRUNE_ENA <<
			   ICE_AQ_VSI_SEC_TX_PRUNE_ENA_S));
	}

	status = ice_update_vsi(&pf->hw, vf_vsi->idx, ctx, NULL);
	if (status) {
		dev_err(dev, "Failed to %sable spoofchk on VF %d VSI %d\n error %d\n",
			ena ? "en" : "dis", vf->vf_id, vf_vsi->vsi_num, status);
		ret = -EIO;
		goto out;
	}

	/* only update spoofchk state and VSI context on success */
	vf_vsi->info.sec_flags = ctx->info.sec_flags;
	vf->spoofchk = ena;

out:
	kfree(ctx);
	return ret;
}

/**
 * ice_vc_get_stats_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to get VSI stats
 */
static int ice_vc_get_stats_msg(struct ice_vf *vf, u8 *msg)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_queue_select *vqs =
		(struct virtchnl_queue_select *)msg;
	struct ice_eth_stats stats = { 0 };
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_isvalid_vsi_id(vf, vqs->vsi_id)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	ice_update_eth_stats(vsi);

	stats = vsi->eth_stats;

error_param:
	/* send the response to the VF */
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_GET_STATS, v_ret,
				     (u8 *)&stats, sizeof(stats));
}

/**
 * ice_vc_validate_vqs_bitmaps - validate Rx/Tx queue bitmaps from VIRTCHNL
 * @vqs: virtchnl_queue_select structure containing bitmaps to validate
 *
 * Return true on successful validation, else false
 */
static bool ice_vc_validate_vqs_bitmaps(struct virtchnl_queue_select *vqs)
{
	if ((!vqs->rx_queues && !vqs->tx_queues) ||
	    vqs->rx_queues >= BIT(ICE_MAX_RSS_QS_PER_VF) ||
	    vqs->tx_queues >= BIT(ICE_MAX_RSS_QS_PER_VF))
		return false;

	return true;
}

/**
 * ice_vf_ena_txq_interrupt - enable Tx queue interrupt via QINT_TQCTL
 * @vsi: VSI of the VF to configure
 * @q_idx: VF queue index used to determine the queue in the PF's space
 */
static void ice_vf_ena_txq_interrupt(struct ice_vsi *vsi, u32 q_idx)
{
	struct ice_hw *hw = &vsi->back->hw;
	u32 pfq = vsi->txq_map[q_idx];
	u32 reg;

	reg = rd32(hw, QINT_TQCTL(pfq));

	/* MSI-X index 0 in the VF's space is always for the OICR, which means
	 * this is most likely a poll mode VF driver, so don't enable an
	 * interrupt that was never configured via VIRTCHNL_OP_CONFIG_IRQ_MAP
	 */
	if (!(reg & QINT_TQCTL_MSIX_INDX_M))
		return;

	wr32(hw, QINT_TQCTL(pfq), reg | QINT_TQCTL_CAUSE_ENA_M);
}

/**
 * ice_vf_ena_rxq_interrupt - enable Tx queue interrupt via QINT_RQCTL
 * @vsi: VSI of the VF to configure
 * @q_idx: VF queue index used to determine the queue in the PF's space
 */
static void ice_vf_ena_rxq_interrupt(struct ice_vsi *vsi, u32 q_idx)
{
	struct ice_hw *hw = &vsi->back->hw;
	u32 pfq = vsi->rxq_map[q_idx];
	u32 reg;

	reg = rd32(hw, QINT_RQCTL(pfq));

	/* MSI-X index 0 in the VF's space is always for the OICR, which means
	 * this is most likely a poll mode VF driver, so don't enable an
	 * interrupt that was never configured via VIRTCHNL_OP_CONFIG_IRQ_MAP
	 */
	if (!(reg & QINT_RQCTL_MSIX_INDX_M))
		return;

	wr32(hw, QINT_RQCTL(pfq), reg | QINT_RQCTL_CAUSE_ENA_M);
}

/**
 * ice_vc_ena_qs_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to enable all or specific queue(s)
 */
static int ice_vc_ena_qs_msg(struct ice_vf *vf, u8 *msg)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_queue_select *vqs =
	    (struct virtchnl_queue_select *)msg;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;
	unsigned long q_map;
	u16 vf_q_id;

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_isvalid_vsi_id(vf, vqs->vsi_id)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_validate_vqs_bitmaps(vqs)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	/* Enable only Rx rings, Tx rings were enabled by the FW when the
	 * Tx queue group list was configured and the context bits were
	 * programmed using ice_vsi_cfg_txqs
	 */
	q_map = vqs->rx_queues;
	for_each_set_bit(vf_q_id, &q_map, ICE_MAX_RSS_QS_PER_VF) {
		if (!ice_vc_isvalid_q_id(vf, vqs->vsi_id, vf_q_id)) {
			v_ret = VIRTCHNL_STATUS_ERR_PARAM;
			goto error_param;
		}

		/* Skip queue if enabled */
		if (test_bit(vf_q_id, vf->rxq_ena))
			continue;

		if (ice_vsi_ctrl_one_rx_ring(vsi, true, vf_q_id, true)) {
			dev_err(ice_pf_to_dev(vsi->back), "Failed to enable Rx ring %d on VSI %d\n",
				vf_q_id, vsi->vsi_num);
			v_ret = VIRTCHNL_STATUS_ERR_PARAM;
			goto error_param;
		}

		ice_vf_ena_rxq_interrupt(vsi, vf_q_id);
		set_bit(vf_q_id, vf->rxq_ena);
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	q_map = vqs->tx_queues;
	for_each_set_bit(vf_q_id, &q_map, ICE_MAX_RSS_QS_PER_VF) {
		if (!ice_vc_isvalid_q_id(vf, vqs->vsi_id, vf_q_id)) {
			v_ret = VIRTCHNL_STATUS_ERR_PARAM;
			goto error_param;
		}

		/* Skip queue if enabled */
		if (test_bit(vf_q_id, vf->txq_ena))
			continue;

		ice_vf_ena_txq_interrupt(vsi, vf_q_id);
		set_bit(vf_q_id, vf->txq_ena);
	}

	/* Set flag to indicate that queues are enabled */
	if (v_ret == VIRTCHNL_STATUS_SUCCESS)
		set_bit(ICE_VF_STATE_QS_ENA, vf->vf_states);

error_param:
	/* send the response to the VF */
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_ENABLE_QUEUES, v_ret,
				     NULL, 0);
}

/**
 * ice_vc_dis_qs_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to disable all or specific
 * queue(s)
 */
static int ice_vc_dis_qs_msg(struct ice_vf *vf, u8 *msg)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_queue_select *vqs =
	    (struct virtchnl_queue_select *)msg;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;
	unsigned long q_map;
	u16 vf_q_id;

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states) &&
	    !test_bit(ICE_VF_STATE_QS_ENA, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_isvalid_vsi_id(vf, vqs->vsi_id)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_validate_vqs_bitmaps(vqs)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (vqs->tx_queues) {
		q_map = vqs->tx_queues;

		for_each_set_bit(vf_q_id, &q_map, ICE_MAX_RSS_QS_PER_VF) {
			struct ice_ring *ring = vsi->tx_rings[vf_q_id];
			struct ice_txq_meta txq_meta = { 0 };

			if (!ice_vc_isvalid_q_id(vf, vqs->vsi_id, vf_q_id)) {
				v_ret = VIRTCHNL_STATUS_ERR_PARAM;
				goto error_param;
			}

			/* Skip queue if not enabled */
			if (!test_bit(vf_q_id, vf->txq_ena))
				continue;

			ice_fill_txq_meta(vsi, ring, &txq_meta);

			if (ice_vsi_stop_tx_ring(vsi, ICE_NO_RESET, vf->vf_id,
						 ring, &txq_meta)) {
				dev_err(ice_pf_to_dev(vsi->back), "Failed to stop Tx ring %d on VSI %d\n",
					vf_q_id, vsi->vsi_num);
				v_ret = VIRTCHNL_STATUS_ERR_PARAM;
				goto error_param;
			}

			/* Clear enabled queues flag */
			clear_bit(vf_q_id, vf->txq_ena);
		}
	}

	q_map = vqs->rx_queues;
	/* speed up Rx queue disable by batching them if possible */
	if (q_map &&
	    bitmap_equal(&q_map, vf->rxq_ena, ICE_MAX_RSS_QS_PER_VF)) {
		if (ice_vsi_stop_all_rx_rings(vsi)) {
			dev_err(ice_pf_to_dev(vsi->back), "Failed to stop all Rx rings on VSI %d\n",
				vsi->vsi_num);
			v_ret = VIRTCHNL_STATUS_ERR_PARAM;
			goto error_param;
		}

		bitmap_zero(vf->rxq_ena, ICE_MAX_RSS_QS_PER_VF);
	} else if (q_map) {
		for_each_set_bit(vf_q_id, &q_map, ICE_MAX_RSS_QS_PER_VF) {
			if (!ice_vc_isvalid_q_id(vf, vqs->vsi_id, vf_q_id)) {
				v_ret = VIRTCHNL_STATUS_ERR_PARAM;
				goto error_param;
			}

			/* Skip queue if not enabled */
			if (!test_bit(vf_q_id, vf->rxq_ena))
				continue;

			if (ice_vsi_ctrl_one_rx_ring(vsi, false, vf_q_id,
						     true)) {
				dev_err(ice_pf_to_dev(vsi->back), "Failed to stop Rx ring %d on VSI %d\n",
					vf_q_id, vsi->vsi_num);
				v_ret = VIRTCHNL_STATUS_ERR_PARAM;
				goto error_param;
			}

			/* Clear enabled queues flag */
			clear_bit(vf_q_id, vf->rxq_ena);
		}
	}

	/* Clear enabled queues flag */
	if (v_ret == VIRTCHNL_STATUS_SUCCESS && ice_vf_has_no_qs_ena(vf))
		clear_bit(ICE_VF_STATE_QS_ENA, vf->vf_states);

error_param:
	/* send the response to the VF */
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_DISABLE_QUEUES, v_ret,
				     NULL, 0);
}

/**
 * ice_cfg_interrupt
 * @vf: pointer to the VF info
 * @vsi: the VSI being configured
 * @vector_id: vector ID
 * @map: vector map for mapping vectors to queues
 * @q_vector: structure for interrupt vector
 * configure the IRQ to queue map
 */
static int
ice_cfg_interrupt(struct ice_vf *vf, struct ice_vsi *vsi, u16 vector_id,
		  struct virtchnl_vector_map *map,
		  struct ice_q_vector *q_vector)
{
	u16 vsi_q_id, vsi_q_id_idx;
	unsigned long qmap;

	q_vector->num_ring_rx = 0;
	q_vector->num_ring_tx = 0;

	qmap = map->rxq_map;
	for_each_set_bit(vsi_q_id_idx, &qmap, ICE_MAX_RSS_QS_PER_VF) {
		vsi_q_id = vsi_q_id_idx;

		if (!ice_vc_isvalid_q_id(vf, vsi->vsi_num, vsi_q_id))
			return VIRTCHNL_STATUS_ERR_PARAM;

		q_vector->num_ring_rx++;
		q_vector->rx.itr_idx = map->rxitr_idx;
		vsi->rx_rings[vsi_q_id]->q_vector = q_vector;
		ice_cfg_rxq_interrupt(vsi, vsi_q_id, vector_id,
				      q_vector->rx.itr_idx);
	}

	qmap = map->txq_map;
	for_each_set_bit(vsi_q_id_idx, &qmap, ICE_MAX_RSS_QS_PER_VF) {
		vsi_q_id = vsi_q_id_idx;

		if (!ice_vc_isvalid_q_id(vf, vsi->vsi_num, vsi_q_id))
			return VIRTCHNL_STATUS_ERR_PARAM;

		q_vector->num_ring_tx++;
		q_vector->tx.itr_idx = map->txitr_idx;
		vsi->tx_rings[vsi_q_id]->q_vector = q_vector;
		ice_cfg_txq_interrupt(vsi, vsi_q_id, vector_id,
				      q_vector->tx.itr_idx);
	}

	return VIRTCHNL_STATUS_SUCCESS;
}

/**
 * ice_vc_cfg_irq_map_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to configure the IRQ to queue map
 */
static int ice_vc_cfg_irq_map_msg(struct ice_vf *vf, u8 *msg)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	u16 num_q_vectors_mapped, vsi_id, vector_id;
	struct virtchnl_irq_map_info *irqmap_info;
	struct virtchnl_vector_map *map;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;
	int i;

	irqmap_info = (struct virtchnl_irq_map_info *)msg;
	num_q_vectors_mapped = irqmap_info->num_vectors;

	/* Check to make sure number of VF vectors mapped is not greater than
	 * number of VF vectors originally allocated, and check that
	 * there is actually at least a single VF queue vector mapped
	 */
	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states) ||
	    pf->num_msix_per_vf < num_q_vectors_mapped ||
	    !num_q_vectors_mapped) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < num_q_vectors_mapped; i++) {
		struct ice_q_vector *q_vector;

		map = &irqmap_info->vecmap[i];

		vector_id = map->vector_id;
		vsi_id = map->vsi_id;
		/* vector_id is always 0-based for each VF, and can never be
		 * larger than or equal to the max allowed interrupts per VF
		 */
		if (!(vector_id < pf->num_msix_per_vf) ||
		    !ice_vc_isvalid_vsi_id(vf, vsi_id) ||
		    (!vector_id && (map->rxq_map || map->txq_map))) {
			v_ret = VIRTCHNL_STATUS_ERR_PARAM;
			goto error_param;
		}

		/* No need to map VF miscellaneous or rogue vector */
		if (!vector_id)
			continue;

		/* Subtract non queue vector from vector_id passed by VF
		 * to get actual number of VSI queue vector array index
		 */
		q_vector = vsi->q_vectors[vector_id - ICE_NONQ_VECS_VF];
		if (!q_vector) {
			v_ret = VIRTCHNL_STATUS_ERR_PARAM;
			goto error_param;
		}

		/* lookout for the invalid queue index */
		v_ret = (enum virtchnl_status_code)
			ice_cfg_interrupt(vf, vsi, vector_id, map, q_vector);
		if (v_ret)
			goto error_param;
	}

error_param:
	/* send the response to the VF */
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_CONFIG_IRQ_MAP, v_ret,
				     NULL, 0);
}

/**
 * ice_vc_cfg_qs_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * called from the VF to configure the Rx/Tx queues
 */
static int ice_vc_cfg_qs_msg(struct ice_vf *vf, u8 *msg)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_vsi_queue_config_info *qci =
	    (struct virtchnl_vsi_queue_config_info *)msg;
	struct virtchnl_queue_pair_info *qpi;
	u16 num_rxq = 0, num_txq = 0;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;
	int i;

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_isvalid_vsi_id(vf, qci->vsi_id)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (qci->num_queue_pairs > ICE_MAX_RSS_QS_PER_VF ||
	    qci->num_queue_pairs > min_t(u16, vsi->alloc_txq, vsi->alloc_rxq)) {
		dev_err(ice_pf_to_dev(pf), "VF-%d requesting more than supported number of queues: %d\n",
			vf->vf_id, min_t(u16, vsi->alloc_txq, vsi->alloc_rxq));
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < qci->num_queue_pairs; i++) {
		qpi = &qci->qpair[i];
		if (qpi->txq.vsi_id != qci->vsi_id ||
		    qpi->rxq.vsi_id != qci->vsi_id ||
		    qpi->rxq.queue_id != qpi->txq.queue_id ||
		    qpi->txq.headwb_enabled ||
		    !ice_vc_isvalid_ring_len(qpi->txq.ring_len) ||
		    !ice_vc_isvalid_ring_len(qpi->rxq.ring_len) ||
		    !ice_vc_isvalid_q_id(vf, qci->vsi_id, qpi->txq.queue_id)) {
			v_ret = VIRTCHNL_STATUS_ERR_PARAM;
			goto error_param;
		}
		/* copy Tx queue info from VF into VSI */
		if (qpi->txq.ring_len > 0) {
			num_txq++;
			vsi->tx_rings[i]->dma = qpi->txq.dma_ring_addr;
			vsi->tx_rings[i]->count = qpi->txq.ring_len;
		}

		/* copy Rx queue info from VF into VSI */
		if (qpi->rxq.ring_len > 0) {
			num_rxq++;
			vsi->rx_rings[i]->dma = qpi->rxq.dma_ring_addr;
			vsi->rx_rings[i]->count = qpi->rxq.ring_len;

			if (qpi->rxq.databuffer_size != 0 &&
			    (qpi->rxq.databuffer_size > ((16 * 1024) - 128) ||
			     qpi->rxq.databuffer_size < 1024)) {
				v_ret = VIRTCHNL_STATUS_ERR_PARAM;
				goto error_param;
			}
			vsi->rx_buf_len = qpi->rxq.databuffer_size;
			vsi->rx_rings[i]->rx_buf_len = vsi->rx_buf_len;
			if (qpi->rxq.max_pkt_size >= (16 * 1024) ||
			    qpi->rxq.max_pkt_size < 64) {
				v_ret = VIRTCHNL_STATUS_ERR_PARAM;
				goto error_param;
			}
		}

		vsi->max_frame = qpi->rxq.max_pkt_size;
	}

	/* VF can request to configure less than allocated queues
	 * or default allocated queues. So update the VSI with new number
	 */
	vsi->num_txq = num_txq;
	vsi->num_rxq = num_rxq;
	/* All queues of VF VSI are in TC 0 */
	vsi->tc_cfg.tc_info[0].qcount_tx = num_txq;
	vsi->tc_cfg.tc_info[0].qcount_rx = num_rxq;

	if (ice_vsi_cfg_lan_txqs(vsi) || ice_vsi_cfg_rxqs(vsi))
		v_ret = VIRTCHNL_STATUS_ERR_ADMIN_QUEUE_ERROR;

error_param:
	/* send the response to the VF */
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_CONFIG_VSI_QUEUES, v_ret,
				     NULL, 0);
}

/**
 * ice_is_vf_trusted
 * @vf: pointer to the VF info
 */
static bool ice_is_vf_trusted(struct ice_vf *vf)
{
	return test_bit(ICE_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps);
}

/**
 * ice_can_vf_change_mac
 * @vf: pointer to the VF info
 *
 * Return true if the VF is allowed to change its MAC filters, false otherwise
 */
static bool ice_can_vf_change_mac(struct ice_vf *vf)
{
	/* If the VF MAC address has been set administratively (via the
	 * ndo_set_vf_mac command), then deny permission to the VF to
	 * add/delete unicast MAC addresses, unless the VF is trusted
	 */
	if (vf->pf_set_mac && !ice_is_vf_trusted(vf))
		return false;

	return true;
}

/**
 * ice_vc_add_mac_addr - attempt to add the MAC address passed in
 * @vf: pointer to the VF info
 * @vsi: pointer to the VF's VSI
 * @mac_addr: MAC address to add
 */
static int
ice_vc_add_mac_addr(struct ice_vf *vf, struct ice_vsi *vsi, u8 *mac_addr)
{
	struct device *dev = ice_pf_to_dev(vf->pf);
	enum ice_status status;

	/* default unicast MAC already added */
	if (ether_addr_equal(mac_addr, vf->dflt_lan_addr.addr))
		return 0;

	if (is_unicast_ether_addr(mac_addr) && !ice_can_vf_change_mac(vf)) {
		dev_err(dev, "VF attempting to override administratively set MAC address, bring down and up the VF interface to resume normal operation\n");
		return -EPERM;
	}

	status = ice_vsi_cfg_mac_fltr(vsi, mac_addr, true);
	if (status == ICE_ERR_ALREADY_EXISTS) {
		dev_err(dev, "MAC %pM already exists for VF %d\n", mac_addr,
			vf->vf_id);
		return -EEXIST;
	} else if (status) {
		dev_err(dev, "Failed to add MAC %pM for VF %d\n, error %d\n",
			mac_addr, vf->vf_id, status);
		return -EIO;
	}

	/* only set dflt_lan_addr once */
	if (is_zero_ether_addr(vf->dflt_lan_addr.addr) &&
	    is_unicast_ether_addr(mac_addr))
		ether_addr_copy(vf->dflt_lan_addr.addr, mac_addr);

	vf->num_mac++;

	return 0;
}

/**
 * ice_vc_del_mac_addr - attempt to delete the MAC address passed in
 * @vf: pointer to the VF info
 * @vsi: pointer to the VF's VSI
 * @mac_addr: MAC address to delete
 */
static int
ice_vc_del_mac_addr(struct ice_vf *vf, struct ice_vsi *vsi, u8 *mac_addr)
{
	struct device *dev = ice_pf_to_dev(vf->pf);
	enum ice_status status;

	if (!ice_can_vf_change_mac(vf) &&
	    ether_addr_equal(mac_addr, vf->dflt_lan_addr.addr))
		return 0;

	status = ice_vsi_cfg_mac_fltr(vsi, mac_addr, false);
	if (status == ICE_ERR_DOES_NOT_EXIST) {
		dev_err(dev, "MAC %pM does not exist for VF %d\n", mac_addr,
			vf->vf_id);
		return -ENOENT;
	} else if (status) {
		dev_err(dev, "Failed to delete MAC %pM for VF %d, error %d\n",
			mac_addr, vf->vf_id, status);
		return -EIO;
	}

	if (ether_addr_equal(mac_addr, vf->dflt_lan_addr.addr))
		eth_zero_addr(vf->dflt_lan_addr.addr);

	vf->num_mac--;

	return 0;
}

/**
 * ice_vc_handle_mac_addr_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @set: true if MAC filters are being set, false otherwise
 *
 * add guest MAC address filter
 */
static int
ice_vc_handle_mac_addr_msg(struct ice_vf *vf, u8 *msg, bool set)
{
	int (*ice_vc_cfg_mac)
		(struct ice_vf *vf, struct ice_vsi *vsi, u8 *mac_addr);
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_ether_addr_list *al =
	    (struct virtchnl_ether_addr_list *)msg;
	struct ice_pf *pf = vf->pf;
	enum virtchnl_ops vc_op;
	struct ice_vsi *vsi;
	int i;

	if (set) {
		vc_op = VIRTCHNL_OP_ADD_ETH_ADDR;
		ice_vc_cfg_mac = ice_vc_add_mac_addr;
	} else {
		vc_op = VIRTCHNL_OP_DEL_ETH_ADDR;
		ice_vc_cfg_mac = ice_vc_del_mac_addr;
	}

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states) ||
	    !ice_vc_isvalid_vsi_id(vf, al->vsi_id)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto handle_mac_exit;
	}

	/* If this VF is not privileged, then we can't add more than a
	 * limited number of addresses. Check to make sure that the
	 * additions do not push us over the limit.
	 */
	if (set && !ice_is_vf_trusted(vf) &&
	    (vf->num_mac + al->num_elements) > ICE_MAX_MACADDR_PER_VF) {
		dev_err(ice_pf_to_dev(pf), "Can't add more MAC addresses, because VF-%d is not trusted, switch the VF to trusted mode in order to add more functionalities\n",
			vf->vf_id);
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto handle_mac_exit;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto handle_mac_exit;
	}

	for (i = 0; i < al->num_elements; i++) {
		u8 *mac_addr = al->list[i].addr;
		int result;

		if (is_broadcast_ether_addr(mac_addr) ||
		    is_zero_ether_addr(mac_addr))
			continue;

		result = ice_vc_cfg_mac(vf, vsi, mac_addr);
		if (result == -EEXIST || result == -ENOENT) {
			continue;
		} else if (result) {
			v_ret = VIRTCHNL_STATUS_ERR_ADMIN_QUEUE_ERROR;
			goto handle_mac_exit;
		}
	}

handle_mac_exit:
	/* send the response to the VF */
	return ice_vc_send_msg_to_vf(vf, vc_op, v_ret, NULL, 0);
}

/**
 * ice_vc_add_mac_addr_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * add guest MAC address filter
 */
static int ice_vc_add_mac_addr_msg(struct ice_vf *vf, u8 *msg)
{
	return ice_vc_handle_mac_addr_msg(vf, msg, true);
}

/**
 * ice_vc_del_mac_addr_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * remove guest MAC address filter
 */
static int ice_vc_del_mac_addr_msg(struct ice_vf *vf, u8 *msg)
{
	return ice_vc_handle_mac_addr_msg(vf, msg, false);
}

/**
 * ice_vc_request_qs_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * VFs get a default number of queues but can use this message to request a
 * different number. If the request is successful, PF will reset the VF and
 * return 0. If unsuccessful, PF will send message informing VF of number of
 * available queue pairs via virtchnl message response to VF.
 */
static int ice_vc_request_qs_msg(struct ice_vf *vf, u8 *msg)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_vf_res_request *vfres =
		(struct virtchnl_vf_res_request *)msg;
	u16 req_queues = vfres->num_queue_pairs;
	struct ice_pf *pf = vf->pf;
	u16 max_allowed_vf_queues;
	u16 tx_rx_queue_left;
	struct device *dev;
	u16 cur_queues;

	dev = ice_pf_to_dev(pf);
	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	cur_queues = vf->num_vf_qs;
	tx_rx_queue_left = min_t(u16, ice_get_avail_txq_count(pf),
				 ice_get_avail_rxq_count(pf));
	max_allowed_vf_queues = tx_rx_queue_left + cur_queues;
	if (!req_queues) {
		dev_err(dev, "VF %d tried to request 0 queues. Ignoring.\n",
			vf->vf_id);
	} else if (req_queues > ICE_MAX_RSS_QS_PER_VF) {
		dev_err(dev, "VF %d tried to request more than %d queues.\n",
			vf->vf_id, ICE_MAX_RSS_QS_PER_VF);
		vfres->num_queue_pairs = ICE_MAX_RSS_QS_PER_VF;
	} else if (req_queues > cur_queues &&
		   req_queues - cur_queues > tx_rx_queue_left) {
		dev_warn(dev, "VF %d requested %u more queues, but only %u left.\n",
			 vf->vf_id, req_queues - cur_queues, tx_rx_queue_left);
		vfres->num_queue_pairs = min_t(u16, max_allowed_vf_queues,
					       ICE_MAX_RSS_QS_PER_VF);
	} else {
		/* request is successful, then reset VF */
		vf->num_req_qs = req_queues;
		ice_vc_reset_vf(vf);
		dev_info(dev, "VF %d granted request of %u queues.\n",
			 vf->vf_id, req_queues);
		return 0;
	}

error_param:
	/* send the response to the VF */
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_REQUEST_QUEUES,
				     v_ret, (u8 *)vfres, sizeof(*vfres));
}

/**
 * ice_set_vf_port_vlan
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @vlan_id: VLAN ID being set
 * @qos: priority setting
 * @vlan_proto: VLAN protocol
 *
 * program VF Port VLAN ID and/or QoS
 */
int
ice_set_vf_port_vlan(struct net_device *netdev, int vf_id, u16 vlan_id, u8 qos,
		     __be16 vlan_proto)
{
	struct ice_pf *pf = ice_netdev_to_pf(netdev);
	struct ice_vsi *vsi;
	struct device *dev;
	struct ice_vf *vf;
	u16 vlanprio;
	int ret;

	dev = ice_pf_to_dev(pf);
	if (ice_validate_vf_id(pf, vf_id))
		return -EINVAL;

	if (vlan_id >= VLAN_N_VID || qos > 7) {
		dev_err(dev, "Invalid Port VLAN parameters for VF %d, ID %d, QoS %d\n",
			vf_id, vlan_id, qos);
		return -EINVAL;
	}

	if (vlan_proto != htons(ETH_P_8021Q)) {
		dev_err(dev, "VF VLAN protocol is not supported\n");
		return -EPROTONOSUPPORT;
	}

	vf = &pf->vf[vf_id];
	vsi = pf->vsi[vf->lan_vsi_idx];

	ret = ice_check_vf_ready_for_cfg(vf);
	if (ret)
		return ret;

	vlanprio = vlan_id | (qos << VLAN_PRIO_SHIFT);

	if (vf->port_vlan_info == vlanprio) {
		/* duplicate request, so just return success */
		dev_dbg(dev, "Duplicate pvid %d request\n", vlanprio);
		return 0;
	}

	if (vlan_id || qos) {
		/* remove VLAN 0 filter set by default when transitioning from
		 * no port VLAN to a port VLAN. No change to old port VLAN on
		 * failure.
		 */
		ret = ice_vsi_kill_vlan(vsi, 0);
		if (ret)
			return ret;
		ret = ice_vsi_manage_pvid(vsi, vlanprio, true);
		if (ret)
			return ret;
	} else {
		/* add VLAN 0 filter back when transitioning from port VLAN to
		 * no port VLAN. No change to old port VLAN on failure.
		 */
		ret = ice_vsi_add_vlan(vsi, 0);
		if (ret)
			return ret;
		ret = ice_vsi_manage_pvid(vsi, 0, false);
		if (ret)
			return ret;
	}

	if (vlan_id) {
		dev_info(dev, "Setting VLAN %d, QoS 0x%x on VF %d\n",
			 vlan_id, qos, vf_id);

		/* add VLAN filter for the port VLAN */
		ret = ice_vsi_add_vlan(vsi, vlan_id);
		if (ret)
			return ret;
	}
	/* remove old port VLAN filter with valid VLAN ID or QoS fields */
	if (vf->port_vlan_info)
		ice_vsi_kill_vlan(vsi, vf->port_vlan_info & VLAN_VID_MASK);

	/* keep port VLAN information persistent on resets */
	vf->port_vlan_info = le16_to_cpu(vsi->info.pvid);

	return 0;
}

/**
 * ice_vf_vlan_offload_ena - determine if capabilities support VLAN offloads
 * @caps: VF driver negotiated capabilities
 *
 * Return true if VIRTCHNL_VF_OFFLOAD_VLAN capability is set, else return false
 */
static bool ice_vf_vlan_offload_ena(u32 caps)
{
	return !!(caps & VIRTCHNL_VF_OFFLOAD_VLAN);
}

/**
 * ice_vc_process_vlan_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @add_v: Add VLAN if true, otherwise delete VLAN
 *
 * Process virtchnl op to add or remove programmed guest VLAN ID
 */
static int ice_vc_process_vlan_msg(struct ice_vf *vf, u8 *msg, bool add_v)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct virtchnl_vlan_filter_list *vfl =
	    (struct virtchnl_vlan_filter_list *)msg;
	struct ice_pf *pf = vf->pf;
	bool vlan_promisc = false;
	struct ice_vsi *vsi;
	struct device *dev;
	struct ice_hw *hw;
	int status = 0;
	u8 promisc_m;
	int i;

	dev = ice_pf_to_dev(pf);
	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vf_vlan_offload_ena(vf->driver_caps)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vc_isvalid_vsi_id(vf, vfl->vsi_id)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < vfl->num_elements; i++) {
		if (vfl->vlan_id[i] >= VLAN_N_VID) {
			v_ret = VIRTCHNL_STATUS_ERR_PARAM;
			dev_err(dev, "invalid VF VLAN id %d\n",
				vfl->vlan_id[i]);
			goto error_param;
		}
	}

	hw = &pf->hw;
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (add_v && !ice_is_vf_trusted(vf) &&
	    vsi->num_vlan >= ICE_MAX_VLAN_PER_VF) {
		dev_info(dev, "VF-%d is not trusted, switch the VF to trusted mode, in order to add more VLAN addresses\n",
			 vf->vf_id);
		/* There is no need to let VF know about being not trusted,
		 * so we can just return success message here
		 */
		goto error_param;
	}

	if (vsi->info.pvid) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (test_bit(ICE_VF_STATE_UC_PROMISC, vf->vf_states) ||
	    test_bit(ICE_VF_STATE_MC_PROMISC, vf->vf_states))
		vlan_promisc = true;

	if (add_v) {
		for (i = 0; i < vfl->num_elements; i++) {
			u16 vid = vfl->vlan_id[i];

			if (!ice_is_vf_trusted(vf) &&
			    vsi->num_vlan >= ICE_MAX_VLAN_PER_VF) {
				dev_info(dev, "VF-%d is not trusted, switch the VF to trusted mode, in order to add more VLAN addresses\n",
					 vf->vf_id);
				/* There is no need to let VF know about being
				 * not trusted, so we can just return success
				 * message here as well.
				 */
				goto error_param;
			}

			/* we add VLAN 0 by default for each VF so we can enable
			 * Tx VLAN anti-spoof without triggering MDD events so
			 * we don't need to add it again here
			 */
			if (!vid)
				continue;

			status = ice_vsi_add_vlan(vsi, vid);
			if (status) {
				v_ret = VIRTCHNL_STATUS_ERR_PARAM;
				goto error_param;
			}

			/* Enable VLAN pruning when non-zero VLAN is added */
			if (!vlan_promisc && vid &&
			    !ice_vsi_is_vlan_pruning_ena(vsi)) {
				status = ice_cfg_vlan_pruning(vsi, true, false);
				if (status) {
					v_ret = VIRTCHNL_STATUS_ERR_PARAM;
					dev_err(dev, "Enable VLAN pruning on VLAN ID: %d failed error-%d\n",
						vid, status);
					goto error_param;
				}
			} else if (vlan_promisc) {
				/* Enable Ucast/Mcast VLAN promiscuous mode */
				promisc_m = ICE_PROMISC_VLAN_TX |
					    ICE_PROMISC_VLAN_RX;

				status = ice_set_vsi_promisc(hw, vsi->idx,
							     promisc_m, vid);
				if (status) {
					v_ret = VIRTCHNL_STATUS_ERR_PARAM;
					dev_err(dev, "Enable Unicast/multicast promiscuous mode on VLAN ID:%d failed error-%d\n",
						vid, status);
				}
			}
		}
	} else {
		/* In case of non_trusted VF, number of VLAN elements passed
		 * to PF for removal might be greater than number of VLANs
		 * filter programmed for that VF - So, use actual number of
		 * VLANS added earlier with add VLAN opcode. In order to avoid
		 * removing VLAN that doesn't exist, which result to sending
		 * erroneous failed message back to the VF
		 */
		int num_vf_vlan;

		num_vf_vlan = vsi->num_vlan;
		for (i = 0; i < vfl->num_elements && i < num_vf_vlan; i++) {
			u16 vid = vfl->vlan_id[i];

			/* we add VLAN 0 by default for each VF so we can enable
			 * Tx VLAN anti-spoof without triggering MDD events so
			 * we don't want a VIRTCHNL request to remove it
			 */
			if (!vid)
				continue;

			/* Make sure ice_vsi_kill_vlan is successful before
			 * updating VLAN information
			 */
			status = ice_vsi_kill_vlan(vsi, vid);
			if (status) {
				v_ret = VIRTCHNL_STATUS_ERR_PARAM;
				goto error_param;
			}

			/* Disable VLAN pruning when only VLAN 0 is left */
			if (vsi->num_vlan == 1 &&
			    ice_vsi_is_vlan_pruning_ena(vsi))
				ice_cfg_vlan_pruning(vsi, false, false);

			/* Disable Unicast/Multicast VLAN promiscuous mode */
			if (vlan_promisc) {
				promisc_m = ICE_PROMISC_VLAN_TX |
					    ICE_PROMISC_VLAN_RX;

				ice_clear_vsi_promisc(hw, vsi->idx,
						      promisc_m, vid);
			}
		}
	}

error_param:
	/* send the response to the VF */
	if (add_v)
		return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_ADD_VLAN, v_ret,
					     NULL, 0);
	else
		return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_DEL_VLAN, v_ret,
					     NULL, 0);
}

/**
 * ice_vc_add_vlan_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * Add and program guest VLAN ID
 */
static int ice_vc_add_vlan_msg(struct ice_vf *vf, u8 *msg)
{
	return ice_vc_process_vlan_msg(vf, msg, true);
}

/**
 * ice_vc_remove_vlan_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 *
 * remove programmed guest VLAN ID
 */
static int ice_vc_remove_vlan_msg(struct ice_vf *vf, u8 *msg)
{
	return ice_vc_process_vlan_msg(vf, msg, false);
}

/**
 * ice_vc_ena_vlan_stripping
 * @vf: pointer to the VF info
 *
 * Enable VLAN header stripping for a given VF
 */
static int ice_vc_ena_vlan_stripping(struct ice_vf *vf)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vf_vlan_offload_ena(vf->driver_caps)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (ice_vsi_manage_vlan_stripping(vsi, true))
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;

error_param:
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_ENABLE_VLAN_STRIPPING,
				     v_ret, NULL, 0);
}

/**
 * ice_vc_dis_vlan_stripping
 * @vf: pointer to the VF info
 *
 * Disable VLAN header stripping for a given VF
 */
static int ice_vc_dis_vlan_stripping(struct ice_vf *vf)
{
	enum virtchnl_status_code v_ret = VIRTCHNL_STATUS_SUCCESS;
	struct ice_pf *pf = vf->pf;
	struct ice_vsi *vsi;

	if (!test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (!ice_vf_vlan_offload_ena(vf->driver_caps)) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;
		goto error_param;
	}

	if (ice_vsi_manage_vlan_stripping(vsi, false))
		v_ret = VIRTCHNL_STATUS_ERR_PARAM;

error_param:
	return ice_vc_send_msg_to_vf(vf, VIRTCHNL_OP_DISABLE_VLAN_STRIPPING,
				     v_ret, NULL, 0);
}

/**
 * ice_vf_init_vlan_stripping - enable/disable VLAN stripping on initialization
 * @vf: VF to enable/disable VLAN stripping for on initialization
 *
 * If the VIRTCHNL_VF_OFFLOAD_VLAN flag is set enable VLAN stripping, else if
 * the flag is cleared then we want to disable stripping. For example, the flag
 * will be cleared when port VLANs are configured by the administrator before
 * passing the VF to the guest or if the AVF driver doesn't support VLAN
 * offloads.
 */
static int ice_vf_init_vlan_stripping(struct ice_vf *vf)
{
	struct ice_vsi *vsi = vf->pf->vsi[vf->lan_vsi_idx];

	if (!vsi)
		return -EINVAL;

	/* don't modify stripping if port VLAN is configured */
	if (vsi->info.pvid)
		return 0;

	if (ice_vf_vlan_offload_ena(vf->driver_caps))
		return ice_vsi_manage_vlan_stripping(vsi, true);
	else
		return ice_vsi_manage_vlan_stripping(vsi, false);
}

/**
 * ice_vc_process_vf_msg - Process request from VF
 * @pf: pointer to the PF structure
 * @event: pointer to the AQ event
 *
 * called from the common asq/arq handler to
 * process request from VF
 */
void ice_vc_process_vf_msg(struct ice_pf *pf, struct ice_rq_event_info *event)
{
	u32 v_opcode = le32_to_cpu(event->desc.cookie_high);
	s16 vf_id = le16_to_cpu(event->desc.retval);
	u16 msglen = event->msg_len;
	u8 *msg = event->msg_buf;
	struct ice_vf *vf = NULL;
	struct device *dev;
	int err = 0;

	dev = ice_pf_to_dev(pf);
	if (ice_validate_vf_id(pf, vf_id)) {
		err = -EINVAL;
		goto error_handler;
	}

	vf = &pf->vf[vf_id];

	/* Check if VF is disabled. */
	if (test_bit(ICE_VF_STATE_DIS, vf->vf_states)) {
		err = -EPERM;
		goto error_handler;
	}

	/* Perform basic checks on the msg */
	err = virtchnl_vc_validate_vf_msg(&vf->vf_ver, v_opcode, msg, msglen);
	if (err) {
		if (err == VIRTCHNL_STATUS_ERR_PARAM)
			err = -EPERM;
		else
			err = -EINVAL;
	}

error_handler:
	if (err) {
		ice_vc_send_msg_to_vf(vf, v_opcode, VIRTCHNL_STATUS_ERR_PARAM,
				      NULL, 0);
		dev_err(dev, "Invalid message from VF %d, opcode %d, len %d, error %d\n",
			vf_id, v_opcode, msglen, err);
		return;
	}

	switch (v_opcode) {
	case VIRTCHNL_OP_VERSION:
		err = ice_vc_get_ver_msg(vf, msg);
		break;
	case VIRTCHNL_OP_GET_VF_RESOURCES:
		err = ice_vc_get_vf_res_msg(vf, msg);
		if (ice_vf_init_vlan_stripping(vf))
			dev_err(dev, "Failed to initialize VLAN stripping for VF %d\n",
				vf->vf_id);
		ice_vc_notify_vf_link_state(vf);
		break;
	case VIRTCHNL_OP_RESET_VF:
		ice_vc_reset_vf_msg(vf);
		break;
	case VIRTCHNL_OP_ADD_ETH_ADDR:
		err = ice_vc_add_mac_addr_msg(vf, msg);
		break;
	case VIRTCHNL_OP_DEL_ETH_ADDR:
		err = ice_vc_del_mac_addr_msg(vf, msg);
		break;
	case VIRTCHNL_OP_CONFIG_VSI_QUEUES:
		err = ice_vc_cfg_qs_msg(vf, msg);
		break;
	case VIRTCHNL_OP_ENABLE_QUEUES:
		err = ice_vc_ena_qs_msg(vf, msg);
		ice_vc_notify_vf_link_state(vf);
		break;
	case VIRTCHNL_OP_DISABLE_QUEUES:
		err = ice_vc_dis_qs_msg(vf, msg);
		break;
	case VIRTCHNL_OP_REQUEST_QUEUES:
		err = ice_vc_request_qs_msg(vf, msg);
		break;
	case VIRTCHNL_OP_CONFIG_IRQ_MAP:
		err = ice_vc_cfg_irq_map_msg(vf, msg);
		break;
	case VIRTCHNL_OP_CONFIG_RSS_KEY:
		err = ice_vc_config_rss_key(vf, msg);
		break;
	case VIRTCHNL_OP_CONFIG_RSS_LUT:
		err = ice_vc_config_rss_lut(vf, msg);
		break;
	case VIRTCHNL_OP_GET_STATS:
		err = ice_vc_get_stats_msg(vf, msg);
		break;
	case VIRTCHNL_OP_ADD_VLAN:
		err = ice_vc_add_vlan_msg(vf, msg);
		break;
	case VIRTCHNL_OP_DEL_VLAN:
		err = ice_vc_remove_vlan_msg(vf, msg);
		break;
	case VIRTCHNL_OP_ENABLE_VLAN_STRIPPING:
		err = ice_vc_ena_vlan_stripping(vf);
		break;
	case VIRTCHNL_OP_DISABLE_VLAN_STRIPPING:
		err = ice_vc_dis_vlan_stripping(vf);
		break;
	case VIRTCHNL_OP_UNKNOWN:
	default:
		dev_err(dev, "Unsupported opcode %d from VF %d\n", v_opcode,
			vf_id);
		err = ice_vc_send_msg_to_vf(vf, v_opcode,
					    VIRTCHNL_STATUS_ERR_NOT_SUPPORTED,
					    NULL, 0);
		break;
	}
	if (err) {
		/* Helper function cares less about error return values here
		 * as it is busy with pending work.
		 */
		dev_info(dev, "PF failed to honor VF %d, opcode %d, error %d\n",
			 vf_id, v_opcode, err);
	}
}

/**
 * ice_get_vf_cfg
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @ivi: VF configuration structure
 *
 * return VF configuration
 */
int
ice_get_vf_cfg(struct net_device *netdev, int vf_id, struct ifla_vf_info *ivi)
{
	struct ice_pf *pf = ice_netdev_to_pf(netdev);
	struct ice_vf *vf;

	if (ice_validate_vf_id(pf, vf_id))
		return -EINVAL;

	vf = &pf->vf[vf_id];

	if (ice_check_vf_init(pf, vf))
		return -EBUSY;

	ivi->vf = vf_id;
	ether_addr_copy(ivi->mac, vf->dflt_lan_addr.addr);

	/* VF configuration for VLAN and applicable QoS */
	ivi->vlan = vf->port_vlan_info & VLAN_VID_MASK;
	ivi->qos = (vf->port_vlan_info & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;

	ivi->trusted = vf->trusted;
	ivi->spoofchk = vf->spoofchk;
	if (!vf->link_forced)
		ivi->linkstate = IFLA_VF_LINK_STATE_AUTO;
	else if (vf->link_up)
		ivi->linkstate = IFLA_VF_LINK_STATE_ENABLE;
	else
		ivi->linkstate = IFLA_VF_LINK_STATE_DISABLE;
	ivi->max_tx_rate = vf->tx_rate;
	ivi->min_tx_rate = 0;
	return 0;
}

/**
 * ice_set_vf_mac
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @mac: MAC address
 *
 * program VF MAC address
 */
int ice_set_vf_mac(struct net_device *netdev, int vf_id, u8 *mac)
{
	struct ice_pf *pf = ice_netdev_to_pf(netdev);
	struct ice_vf *vf;
	int ret;

	if (ice_validate_vf_id(pf, vf_id))
		return -EINVAL;

	if (is_zero_ether_addr(mac) || is_multicast_ether_addr(mac)) {
		netdev_err(netdev, "%pM not a valid unicast address\n", mac);
		return -EINVAL;
	}

	vf = &pf->vf[vf_id];
	ret = ice_check_vf_ready_for_cfg(vf);
	if (ret)
		return ret;

	/* copy MAC into dflt_lan_addr and trigger a VF reset. The reset
	 * flow will use the updated dflt_lan_addr and add a MAC filter
	 * using ice_add_mac. Also set pf_set_mac to indicate that the PF has
	 * set the MAC address for this VF.
	 */
	ether_addr_copy(vf->dflt_lan_addr.addr, mac);
	vf->pf_set_mac = true;
	netdev_info(netdev, "MAC on VF %d set to %pM. VF driver will be reinitialized\n",
		    vf_id, mac);

	ice_vc_reset_vf(vf);
	return 0;
}

/**
 * ice_set_vf_trust
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @trusted: Boolean value to enable/disable trusted VF
 *
 * Enable or disable a given VF as trusted
 */
int ice_set_vf_trust(struct net_device *netdev, int vf_id, bool trusted)
{
	struct ice_pf *pf = ice_netdev_to_pf(netdev);
	struct ice_vf *vf;
	int ret;

	if (ice_validate_vf_id(pf, vf_id))
		return -EINVAL;

	vf = &pf->vf[vf_id];
	ret = ice_check_vf_ready_for_cfg(vf);
	if (ret)
		return ret;

	/* Check if already trusted */
	if (trusted == vf->trusted)
		return 0;

	vf->trusted = trusted;
	ice_vc_reset_vf(vf);
	dev_info(ice_pf_to_dev(pf), "VF %u is now %strusted\n",
		 vf_id, trusted ? "" : "un");

	return 0;
}

/**
 * ice_set_vf_link_state
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @link_state: required link state
 *
 * Set VF's link state, irrespective of physical link state status
 */
int ice_set_vf_link_state(struct net_device *netdev, int vf_id, int link_state)
{
	struct ice_pf *pf = ice_netdev_to_pf(netdev);
	struct ice_vf *vf;
	int ret;

	if (ice_validate_vf_id(pf, vf_id))
		return -EINVAL;

	vf = &pf->vf[vf_id];
	ret = ice_check_vf_ready_for_cfg(vf);
	if (ret)
		return ret;

	switch (link_state) {
	case IFLA_VF_LINK_STATE_AUTO:
		vf->link_forced = false;
		break;
	case IFLA_VF_LINK_STATE_ENABLE:
		vf->link_forced = true;
		vf->link_up = true;
		break;
	case IFLA_VF_LINK_STATE_DISABLE:
		vf->link_forced = true;
		vf->link_up = false;
		break;
	default:
		return -EINVAL;
	}

	ice_vc_notify_vf_link_state(vf);

	return 0;
}

/**
 * ice_get_vf_stats - populate some stats for the VF
 * @netdev: the netdev of the PF
 * @vf_id: the host OS identifier (0-255)
 * @vf_stats: pointer to the OS memory to be initialized
 */
int ice_get_vf_stats(struct net_device *netdev, int vf_id,
		     struct ifla_vf_stats *vf_stats)
{
	struct ice_pf *pf = ice_netdev_to_pf(netdev);
	struct ice_eth_stats *stats;
	struct ice_vsi *vsi;
	struct ice_vf *vf;
	int ret;

	if (ice_validate_vf_id(pf, vf_id))
		return -EINVAL;

	vf = &pf->vf[vf_id];
	ret = ice_check_vf_ready_for_cfg(vf);
	if (ret)
		return ret;

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi)
		return -EINVAL;

	ice_update_eth_stats(vsi);
	stats = &vsi->eth_stats;

	memset(vf_stats, 0, sizeof(*vf_stats));

	vf_stats->rx_packets = stats->rx_unicast + stats->rx_broadcast +
		stats->rx_multicast;
	vf_stats->tx_packets = stats->tx_unicast + stats->tx_broadcast +
		stats->tx_multicast;
	vf_stats->rx_bytes   = stats->rx_bytes;
	vf_stats->tx_bytes   = stats->tx_bytes;
	vf_stats->broadcast  = stats->rx_broadcast;
	vf_stats->multicast  = stats->rx_multicast;
	vf_stats->rx_dropped = stats->rx_discards;
	vf_stats->tx_dropped = stats->tx_discards;

	return 0;
}

/**
 * ice_print_vfs_mdd_event - print VFs malicious driver detect event
 * @pf: pointer to the PF structure
 *
 * Called from ice_handle_mdd_event to rate limit and print VFs MDD events.
 */
void ice_print_vfs_mdd_events(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	struct ice_hw *hw = &pf->hw;
	int i;

	/* check that there are pending MDD events to print */
	if (!test_and_clear_bit(__ICE_MDD_VF_PRINT_PENDING, pf->state))
		return;

	/* VF MDD event logs are rate limited to one second intervals */
	if (time_is_after_jiffies(pf->last_printed_mdd_jiffies + HZ * 1))
		return;

	pf->last_printed_mdd_jiffies = jiffies;

	ice_for_each_vf(pf, i) {
		struct ice_vf *vf = &pf->vf[i];

		/* only print Rx MDD event message if there are new events */
		if (vf->mdd_rx_events.count != vf->mdd_rx_events.last_printed) {
			vf->mdd_rx_events.last_printed =
							vf->mdd_rx_events.count;

			dev_info(dev, "%d Rx Malicious Driver Detection events detected on PF %d VF %d MAC %pM. mdd-auto-reset-vfs=%s\n",
				 vf->mdd_rx_events.count, hw->pf_id, i,
				 vf->dflt_lan_addr.addr,
				 test_bit(ICE_FLAG_MDD_AUTO_RESET_VF, pf->flags)
					  ? "on" : "off");
		}

		/* only print Tx MDD event message if there are new events */
		if (vf->mdd_tx_events.count != vf->mdd_tx_events.last_printed) {
			vf->mdd_tx_events.last_printed =
							vf->mdd_tx_events.count;

			dev_info(dev, "%d Tx Malicious Driver Detection events detected on PF %d VF %d MAC %pM.\n",
				 vf->mdd_tx_events.count, hw->pf_id, i,
				 vf->dflt_lan_addr.addr);
		}
	}
}