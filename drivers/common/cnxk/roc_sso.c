/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#include "roc_api.h"
#include "roc_priv.h"

/* Private functions. */
static int
sso_lf_alloc(struct roc_sso *roc_sso, enum sso_lf_type lf_type, uint16_t nb_lf,
	     void **rsp)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	int rc = -ENOSPC;

	switch (lf_type) {
	case SSO_LF_TYPE_HWS: {
		struct ssow_lf_alloc_req *req;

		req = mbox_alloc_msg_ssow_lf_alloc(dev->mbox);
		if (req == NULL)
			return rc;
		req->hws = nb_lf;
	} break;
	case SSO_LF_TYPE_HWGRP: {
		struct sso_lf_alloc_req *req;

		req = mbox_alloc_msg_sso_lf_alloc(dev->mbox);
		if (req == NULL)
			return rc;
		req->hwgrps = nb_lf;
	} break;
	default:
		break;
	}

	rc = mbox_process_msg(dev->mbox, rsp);
	if (rc < 0)
		return rc;

	return 0;
}

static int
sso_lf_free(struct roc_sso *roc_sso, enum sso_lf_type lf_type, uint16_t nb_lf)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	int rc = -ENOSPC;

	switch (lf_type) {
	case SSO_LF_TYPE_HWS: {
		struct ssow_lf_free_req *req;

		req = mbox_alloc_msg_ssow_lf_free(dev->mbox);
		if (req == NULL)
			return rc;
		req->hws = nb_lf;
	} break;
	case SSO_LF_TYPE_HWGRP: {
		struct sso_lf_free_req *req;

		req = mbox_alloc_msg_sso_lf_free(dev->mbox);
		if (req == NULL)
			return rc;
		req->hwgrps = nb_lf;
	} break;
	default:
		break;
	}

	rc = mbox_process(dev->mbox);
	if (rc < 0)
		return rc;

	return 0;
}

static int
sso_rsrc_attach(struct roc_sso *roc_sso, enum sso_lf_type lf_type,
		uint16_t nb_lf)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct rsrc_attach_req *req;
	int rc = -ENOSPC;

	req = mbox_alloc_msg_attach_resources(dev->mbox);
	if (req == NULL)
		return rc;
	switch (lf_type) {
	case SSO_LF_TYPE_HWS:
		req->ssow = nb_lf;
		break;
	case SSO_LF_TYPE_HWGRP:
		req->sso = nb_lf;
		break;
	default:
		return SSO_ERR_PARAM;
	}

	req->modify = true;
	if (mbox_process(dev->mbox) < 0)
		return -EIO;

	return 0;
}

static int
sso_rsrc_detach(struct roc_sso *roc_sso, enum sso_lf_type lf_type)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct rsrc_detach_req *req;
	int rc = -ENOSPC;

	req = mbox_alloc_msg_detach_resources(dev->mbox);
	if (req == NULL)
		return rc;
	switch (lf_type) {
	case SSO_LF_TYPE_HWS:
		req->ssow = true;
		break;
	case SSO_LF_TYPE_HWGRP:
		req->sso = true;
		break;
	default:
		return SSO_ERR_PARAM;
	}

	req->partial = true;
	if (mbox_process(dev->mbox) < 0)
		return -EIO;

	return 0;
}

static int
sso_rsrc_get(struct roc_sso *roc_sso)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct free_rsrcs_rsp *rsrc_cnt;
	int rc;

	mbox_alloc_msg_free_rsrc_cnt(dev->mbox);
	rc = mbox_process_msg(dev->mbox, (void **)&rsrc_cnt);
	if (rc < 0) {
		plt_err("Failed to get free resource count\n");
		return rc;
	}

	roc_sso->max_hwgrp = rsrc_cnt->sso;
	roc_sso->max_hws = rsrc_cnt->ssow;

	return 0;
}

static void
sso_hws_link_modify(uint8_t hws, uintptr_t base, struct plt_bitmap *bmp,
		    uint16_t hwgrp[], uint16_t n, uint16_t enable)
{
	uint64_t reg;
	int i, j, k;

	i = 0;
	while (n) {
		uint64_t mask[4] = {
			0x8000,
			0x8000,
			0x8000,
			0x8000,
		};

		k = n % 4;
		k = k ? k : 4;
		for (j = 0; j < k; j++) {
			mask[j] = hwgrp[i + j] | enable << 14;
			enable ? plt_bitmap_set(bmp, hwgrp[i + j]) :
				 plt_bitmap_clear(bmp, hwgrp[i + j]);
			plt_sso_dbg("HWS %d Linked to HWGRP %d", hws,
				    hwgrp[i + j]);
		}

		n -= j;
		i += j;
		reg = mask[0] | mask[1] << 16 | mask[2] << 32 | mask[3] << 48;
		plt_write64(reg, base + SSOW_LF_GWS_GRPMSK_CHG);
	}
}

static int
sso_msix_fill(struct roc_sso *roc_sso, uint16_t nb_hws, uint16_t nb_hwgrp)
{
	struct sso *sso = roc_sso_to_sso_priv(roc_sso);
	struct msix_offset_rsp *rsp;
	struct dev *dev = &sso->dev;
	int i, rc;

	mbox_alloc_msg_msix_offset(dev->mbox);
	rc = mbox_process_msg(dev->mbox, (void **)&rsp);
	if (rc < 0)
		return rc;

	for (i = 0; i < nb_hws; i++)
		sso->hws_msix_offset[i] = rsp->ssow_msixoff[i];
	for (i = 0; i < nb_hwgrp; i++)
		sso->hwgrp_msix_offset[i] = rsp->sso_msixoff[i];

	return 0;
}

/* Public Functions. */
uintptr_t
roc_sso_hws_base_get(struct roc_sso *roc_sso, uint8_t hws)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;

	return dev->bar2 + (RVU_BLOCK_ADDR_SSOW << 20 | hws << 12);
}

uintptr_t
roc_sso_hwgrp_base_get(struct roc_sso *roc_sso, uint16_t hwgrp)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;

	return dev->bar2 + (RVU_BLOCK_ADDR_SSO << 20 | hwgrp << 12);
}

uint64_t
roc_sso_ns_to_gw(struct roc_sso *roc_sso, uint64_t ns)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	uint64_t current_us, current_ns, new_ns;
	uintptr_t base;

	base = dev->bar2 + (RVU_BLOCK_ADDR_SSOW << 20);
	current_us = plt_read64(base + SSOW_LF_GWS_NW_TIM);
	/* From HRM, table 14-19:
	 * The SSOW_LF_GWS_NW_TIM[NW_TIM] period is specified in n-1 notation.
	 */
	current_us += 1;

	/* From HRM, table 14-1:
	 * SSOW_LF_GWS_NW_TIM[NW_TIM] specifies the minimum timeout. The SSO
	 * hardware times out a GET_WORK request within 2 usec of the minimum
	 * timeout specified by SSOW_LF_GWS_NW_TIM[NW_TIM].
	 */
	current_us += 2;
	current_ns = current_us * 1E3;
	new_ns = (ns - PLT_MIN(ns, current_ns));
	new_ns = !new_ns ? 1 : new_ns;
	return (new_ns * plt_tsc_hz()) / 1E9;
}

int
roc_sso_hws_link(struct roc_sso *roc_sso, uint8_t hws, uint16_t hwgrp[],
		 uint16_t nb_hwgrp)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct sso *sso;
	uintptr_t base;

	sso = roc_sso_to_sso_priv(roc_sso);
	base = dev->bar2 + (RVU_BLOCK_ADDR_SSOW << 20 | hws << 12);
	sso_hws_link_modify(hws, base, sso->link_map[hws], hwgrp, nb_hwgrp, 1);

	return nb_hwgrp;
}

int
roc_sso_hws_unlink(struct roc_sso *roc_sso, uint8_t hws, uint16_t hwgrp[],
		   uint16_t nb_hwgrp)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct sso *sso;
	uintptr_t base;

	sso = roc_sso_to_sso_priv(roc_sso);
	base = dev->bar2 + (RVU_BLOCK_ADDR_SSOW << 20 | hws << 12);
	sso_hws_link_modify(hws, base, sso->link_map[hws], hwgrp, nb_hwgrp, 0);

	return nb_hwgrp;
}

int
roc_sso_hwgrp_hws_link_status(struct roc_sso *roc_sso, uint8_t hws,
			      uint16_t hwgrp)
{
	struct sso *sso;

	sso = roc_sso_to_sso_priv(roc_sso);
	return plt_bitmap_get(sso->link_map[hws], hwgrp);
}

int
roc_sso_hwgrp_qos_config(struct roc_sso *roc_sso, struct roc_sso_hwgrp_qos *qos,
			 uint8_t nb_qos, uint32_t nb_xaq)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct sso_grp_qos_cfg *req;
	int i, rc;

	for (i = 0; i < nb_qos; i++) {
		uint8_t xaq_prcnt = qos[i].xaq_prcnt;
		uint8_t iaq_prcnt = qos[i].iaq_prcnt;
		uint8_t taq_prcnt = qos[i].taq_prcnt;

		req = mbox_alloc_msg_sso_grp_qos_config(dev->mbox);
		if (req == NULL) {
			rc = mbox_process(dev->mbox);
			if (rc < 0)
				return rc;
			req = mbox_alloc_msg_sso_grp_qos_config(dev->mbox);
			if (req == NULL)
				return -ENOSPC;
		}
		req->grp = qos[i].hwgrp;
		req->xaq_limit = (nb_xaq * (xaq_prcnt ? xaq_prcnt : 100)) / 100;
		req->taq_thr = (SSO_HWGRP_IAQ_MAX_THR_MASK *
				(iaq_prcnt ? iaq_prcnt : 100)) /
			       100;
		req->iaq_thr = (SSO_HWGRP_TAQ_MAX_THR_MASK *
				(taq_prcnt ? taq_prcnt : 100)) /
			       100;
	}

	return mbox_process(dev->mbox);
}

int
roc_sso_hwgrp_alloc_xaq(struct roc_sso *roc_sso, uint32_t npa_aura_id,
			uint16_t hwgrps)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct sso_hw_setconfig *req;
	int rc = -ENOSPC;

	req = mbox_alloc_msg_sso_hw_setconfig(dev->mbox);
	if (req == NULL)
		return rc;
	req->npa_pf_func = idev_npa_pffunc_get();
	req->npa_aura_id = npa_aura_id;
	req->hwgrps = hwgrps;

	return mbox_process(dev->mbox);
}

int
roc_sso_hwgrp_release_xaq(struct roc_sso *roc_sso, uint16_t hwgrps)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct sso_hw_xaq_release *req;

	req = mbox_alloc_msg_sso_hw_release_xaq_aura(dev->mbox);
	if (req == NULL)
		return -EINVAL;
	req->hwgrps = hwgrps;

	return mbox_process(dev->mbox);
}

int
roc_sso_hwgrp_set_priority(struct roc_sso *roc_sso, uint16_t hwgrp,
			   uint8_t weight, uint8_t affinity, uint8_t priority)
{
	struct dev *dev = &roc_sso_to_sso_priv(roc_sso)->dev;
	struct sso_grp_priority *req;
	int rc = -ENOSPC;

	req = mbox_alloc_msg_sso_grp_set_priority(dev->mbox);
	if (req == NULL)
		return rc;
	req->grp = hwgrp;
	req->weight = weight;
	req->affinity = affinity;
	req->priority = priority;

	rc = mbox_process(dev->mbox);
	if (rc < 0)
		return rc;
	plt_sso_dbg("HWGRP %d weight %d affinity %d priority %d", hwgrp, weight,
		    affinity, priority);

	return 0;
}

int
roc_sso_rsrc_init(struct roc_sso *roc_sso, uint8_t nb_hws, uint16_t nb_hwgrp)
{
	struct sso *sso = roc_sso_to_sso_priv(roc_sso);
	struct sso_lf_alloc_rsp *rsp_hwgrp;
	int rc;

	if (roc_sso->max_hwgrp < nb_hwgrp)
		return -ENOENT;
	if (roc_sso->max_hws < nb_hws)
		return -ENOENT;

	rc = sso_rsrc_attach(roc_sso, SSO_LF_TYPE_HWS, nb_hws);
	if (rc < 0) {
		plt_err("Unable to attach SSO HWS LFs");
		return rc;
	}

	rc = sso_rsrc_attach(roc_sso, SSO_LF_TYPE_HWGRP, nb_hwgrp);
	if (rc < 0) {
		plt_err("Unable to attach SSO HWGRP LFs");
		goto hwgrp_atch_fail;
	}

	rc = sso_lf_alloc(roc_sso, SSO_LF_TYPE_HWS, nb_hws, NULL);
	if (rc < 0) {
		plt_err("Unable to alloc SSO HWS LFs");
		goto hws_alloc_fail;
	}

	rc = sso_lf_alloc(roc_sso, SSO_LF_TYPE_HWGRP, nb_hwgrp,
			  (void **)&rsp_hwgrp);
	if (rc < 0) {
		plt_err("Unable to alloc SSO HWGRP Lfs");
		goto hwgrp_alloc_fail;
	}

	roc_sso->xaq_buf_size = rsp_hwgrp->xaq_buf_size;
	roc_sso->xae_waes = rsp_hwgrp->xaq_wq_entries;
	roc_sso->iue = rsp_hwgrp->in_unit_entries;

	rc = sso_msix_fill(roc_sso, nb_hws, nb_hwgrp);
	if (rc < 0) {
		plt_err("Unable to get MSIX offsets for SSO LFs");
		goto sso_msix_fail;
	}

	rc = sso_register_irqs_priv(roc_sso, &sso->pci_dev->intr_handle, nb_hws,
				    nb_hwgrp);
	if (rc < 0) {
		plt_err("Failed to register SSO LF IRQs");
		goto sso_msix_fail;
	}

	roc_sso->nb_hwgrp = nb_hwgrp;
	roc_sso->nb_hws = nb_hws;

	return 0;
sso_msix_fail:
	sso_lf_free(roc_sso, SSO_LF_TYPE_HWGRP, nb_hwgrp);
hwgrp_alloc_fail:
	sso_lf_free(roc_sso, SSO_LF_TYPE_HWS, nb_hws);
hws_alloc_fail:
	sso_rsrc_detach(roc_sso, SSO_LF_TYPE_HWGRP);
hwgrp_atch_fail:
	sso_rsrc_detach(roc_sso, SSO_LF_TYPE_HWS);
	return rc;
}

void
roc_sso_rsrc_fini(struct roc_sso *roc_sso)
{
	struct sso *sso = roc_sso_to_sso_priv(roc_sso);

	if (!roc_sso->nb_hws && !roc_sso->nb_hwgrp)
		return;

	sso_unregister_irqs_priv(roc_sso, &sso->pci_dev->intr_handle,
				 roc_sso->nb_hws, roc_sso->nb_hwgrp);
	sso_lf_free(roc_sso, SSO_LF_TYPE_HWS, roc_sso->nb_hws);
	sso_lf_free(roc_sso, SSO_LF_TYPE_HWGRP, roc_sso->nb_hwgrp);

	sso_rsrc_detach(roc_sso, SSO_LF_TYPE_HWS);
	sso_rsrc_detach(roc_sso, SSO_LF_TYPE_HWGRP);

	roc_sso->nb_hwgrp = 0;
	roc_sso->nb_hws = 0;
}

int
roc_sso_dev_init(struct roc_sso *roc_sso)
{
	struct plt_pci_device *pci_dev;
	uint32_t link_map_sz;
	struct sso *sso;
	void *link_mem;
	int i, rc;

	if (roc_sso == NULL || roc_sso->pci_dev == NULL)
		return SSO_ERR_PARAM;

	PLT_STATIC_ASSERT(sizeof(struct sso) <= ROC_SSO_MEM_SZ);
	sso = roc_sso_to_sso_priv(roc_sso);
	memset(sso, 0, sizeof(*sso));
	pci_dev = roc_sso->pci_dev;

	rc = dev_init(&sso->dev, pci_dev);
	if (rc < 0) {
		plt_err("Failed to init roc device");
		goto fail;
	}

	rc = sso_rsrc_get(roc_sso);
	if (rc < 0) {
		plt_err("Failed to get SSO resources");
		goto rsrc_fail;
	}
	rc = -ENOMEM;

	sso->link_map =
		plt_zmalloc(sizeof(struct plt_bitmap *) * roc_sso->max_hws, 0);
	if (sso->link_map == NULL) {
		plt_err("Failed to allocate memory for link_map array");
		goto rsrc_fail;
	}

	link_map_sz = plt_bitmap_get_memory_footprint(roc_sso->max_hwgrp);
	sso->link_map_mem = plt_zmalloc(link_map_sz * roc_sso->max_hws, 0);
	if (sso->link_map_mem == NULL) {
		plt_err("Failed to get link_map memory");
		goto rsrc_fail;
	}

	link_mem = sso->link_map_mem;
	for (i = 0; i < roc_sso->max_hws; i++) {
		sso->link_map[i] = plt_bitmap_init(roc_sso->max_hwgrp, link_mem,
						   link_map_sz);
		if (sso->link_map[i] == NULL) {
			plt_err("Failed to allocate link map");
			goto link_mem_free;
		}
		link_mem = PLT_PTR_ADD(link_mem, link_map_sz);
	}
	idev_sso_pffunc_set(sso->dev.pf_func);
	sso->pci_dev = pci_dev;
	sso->dev.drv_inited = true;
	roc_sso->lmt_base = sso->dev.lmt_base;

	return 0;
link_mem_free:
	plt_free(sso->link_map_mem);
rsrc_fail:
	rc |= dev_fini(&sso->dev, pci_dev);
fail:
	return rc;
}

int
roc_sso_dev_fini(struct roc_sso *roc_sso)
{
	struct sso *sso;

	sso = roc_sso_to_sso_priv(roc_sso);
	sso->dev.drv_inited = false;

	return dev_fini(&sso->dev, sso->pci_dev);
}
