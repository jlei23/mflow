// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2007 - 2012 Realtek Corporation. */

#define _RTW_XMIT_C_

#include "../include/osdep_service.h"
#include "../include/drv_types.h"
#include "../include/wifi.h"
#include "../include/osdep_intf.h"
#include "../include/usb_ops.h"
#include "../include/usb_osintf.h"
#include "../include/rtl8188e_xmit.h"

static u8 P802_1H_OUI[P80211_OUI_LEN] = { 0x00, 0x00, 0xf8 };
static u8 RFC1042_OUI[P80211_OUI_LEN] = { 0x00, 0x00, 0x00 };

static void _init_txservq(struct tx_servq *ptxservq)
{
	INIT_LIST_HEAD(&ptxservq->tx_pending);
	INIT_LIST_HEAD(&ptxservq->sta_pending);
	ptxservq->qcnt = 0;
}

void	_rtw_init_sta_xmit_priv(struct sta_xmit_priv *psta_xmitpriv)
{
	memset((unsigned char *)psta_xmitpriv, 0, sizeof(struct sta_xmit_priv));
	spin_lock_init(&psta_xmitpriv->lock);
	_init_txservq(&psta_xmitpriv->be_q);
	_init_txservq(&psta_xmitpriv->bk_q);
	_init_txservq(&psta_xmitpriv->vi_q);
	_init_txservq(&psta_xmitpriv->vo_q);
}

static int rtw_xmit_resource_alloc(struct adapter *padapter, struct xmit_buf *pxmitbuf,
				   u32 alloc_sz)
{
	pxmitbuf->pallocated_buf = kzalloc(alloc_sz, GFP_KERNEL);
	if (!pxmitbuf->pallocated_buf)
		return -ENOMEM;

	pxmitbuf->pbuf = (u8 *)ALIGN((size_t)(pxmitbuf->pallocated_buf), XMITBUF_ALIGN_SZ);

	pxmitbuf->pxmit_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!pxmitbuf->pxmit_urb) {
		kfree(pxmitbuf->pallocated_buf);
		return -ENOMEM;
	}

	return 0;
}

static void rtw_xmit_resource_free(struct adapter *padapter, struct xmit_buf *pxmitbuf,
				   u32 free_sz)
{
	usb_free_urb(pxmitbuf->pxmit_urb);
	kfree(pxmitbuf->pallocated_buf);
}

int _rtw_init_xmit_priv(struct xmit_priv *pxmitpriv, struct adapter *padapter)
{
	int i;
	struct xmit_buf *pxmitbuf;
	struct xmit_frame *pxframe;
	u32 max_xmit_extbuf_size = MAX_XMIT_EXTBUF_SZ;
	u32 num_xmit_extbuf = NR_XMIT_EXTBUFF;

	/*  We don't need to memset padapter->XXX to zero, because adapter is allocated by vzalloc(). */

	spin_lock_init(&pxmitpriv->lock);

	/*
	 * Please insert all the queue initializaiton using rtw_init_queue below
	 */

	pxmitpriv->adapter = padapter;

	INIT_LIST_HEAD(&pxmitpriv->be_pending);
	INIT_LIST_HEAD(&pxmitpriv->bk_pending);
	INIT_LIST_HEAD(&pxmitpriv->vi_pending);
	INIT_LIST_HEAD(&pxmitpriv->vo_pending);

	rtw_init_queue(&pxmitpriv->free_xmit_queue);

	/*
	 * Please allocate memory with the sz = (struct xmit_frame) * NR_XMITFRAME,
	 * and initialize free_xmit_frame below.
	 * Please also apply  free_txobj to link_up all the xmit_frames...
	 */

	pxmitpriv->pallocated_frame_buf = vzalloc(NR_XMITFRAME * sizeof(struct xmit_frame) + 4);

	if (!pxmitpriv->pallocated_frame_buf) {
		pxmitpriv->pxmit_frame_buf = NULL;
		goto exit;
	}
	pxmitpriv->pxmit_frame_buf = (u8 *)ALIGN((size_t)(pxmitpriv->pallocated_frame_buf), 4);
	/* pxmitpriv->pxmit_frame_buf = pxmitpriv->pallocated_frame_buf + 4 - */
	/* 						((size_t) (pxmitpriv->pallocated_frame_buf) &3); */

	pxframe = (struct xmit_frame *)pxmitpriv->pxmit_frame_buf;

	for (i = 0; i < NR_XMITFRAME; i++) {
		INIT_LIST_HEAD(&pxframe->list);

		pxframe->padapter = padapter;
		pxframe->frame_tag = NULL_FRAMETAG;

		pxframe->pkt = NULL;

		pxframe->buf_addr = NULL;
		pxframe->pxmitbuf = NULL;

		list_add_tail(&pxframe->list, &pxmitpriv->free_xmit_queue.queue);

		pxframe++;
	}

	pxmitpriv->free_xmitframe_cnt = NR_XMITFRAME;

	pxmitpriv->frag_len = MAX_FRAG_THRESHOLD;

	/* init xmit_buf */
	rtw_init_queue(&pxmitpriv->free_xmitbuf_queue);
	rtw_init_queue(&pxmitpriv->pending_xmitbuf_queue);

	pxmitpriv->pallocated_xmitbuf = vzalloc(NR_XMITBUFF * sizeof(struct xmit_buf) + 4);

	if (!pxmitpriv->pallocated_xmitbuf)
		goto free_frame_buf;

	pxmitpriv->pxmitbuf = (u8 *)ALIGN((size_t)(pxmitpriv->pallocated_xmitbuf), 4);
	/* pxmitpriv->pxmitbuf = pxmitpriv->pallocated_xmitbuf + 4 - */
	/* 						((size_t) (pxmitpriv->pallocated_xmitbuf) &3); */

	pxmitbuf = (struct xmit_buf *)pxmitpriv->pxmitbuf;

	for (i = 0; i < NR_XMITBUFF; i++) {
		INIT_LIST_HEAD(&pxmitbuf->list);

		pxmitbuf->priv_data = NULL;
		pxmitbuf->padapter = padapter;
		pxmitbuf->ext_tag = false;

		/* Tx buf allocation may fail sometimes, so sleep and retry. */
		if (rtw_xmit_resource_alloc(padapter, pxmitbuf, (MAX_XMITBUF_SZ + XMITBUF_ALIGN_SZ))) {
			msleep(10);
			if (rtw_xmit_resource_alloc(padapter, pxmitbuf, (MAX_XMITBUF_SZ + XMITBUF_ALIGN_SZ)))
				goto free_xmitbuf;
		}

		pxmitbuf->high_queue = false;

		list_add_tail(&pxmitbuf->list, &pxmitpriv->free_xmitbuf_queue.queue);
		pxmitbuf++;
	}

	pxmitpriv->free_xmitbuf_cnt = NR_XMITBUFF;

	/*  Init xmit extension buff */
	rtw_init_queue(&pxmitpriv->free_xmit_extbuf_queue);

	pxmitpriv->pallocated_xmit_extbuf = vzalloc(num_xmit_extbuf * sizeof(struct xmit_buf) + 4);

	if (!pxmitpriv->pallocated_xmit_extbuf)
		goto free_xmitbuf;

	pxmitpriv->pxmit_extbuf = (u8 *)ALIGN((size_t)(pxmitpriv->pallocated_xmit_extbuf), 4);

	pxmitbuf = (struct xmit_buf *)pxmitpriv->pxmit_extbuf;

	for (i = 0; i < num_xmit_extbuf; i++) {
		INIT_LIST_HEAD(&pxmitbuf->list);

		pxmitbuf->priv_data = NULL;
		pxmitbuf->padapter = padapter;
		pxmitbuf->ext_tag = true;

		if (rtw_xmit_resource_alloc(padapter, pxmitbuf, max_xmit_extbuf_size + XMITBUF_ALIGN_SZ))
			goto free_xmit_extbuf;

		list_add_tail(&pxmitbuf->list, &pxmitpriv->free_xmit_extbuf_queue.queue);
		pxmitbuf++;
	}

	pxmitpriv->free_xmit_extbuf_cnt = num_xmit_extbuf;

	if (rtw_alloc_hwxmits(padapter))
		goto free_xmit_extbuf;

	for (i = 0; i < 4; i++)
		pxmitpriv->wmm_para_seq[i] = i;

	pxmitpriv->ack_tx = false;
	mutex_init(&pxmitpriv->ack_tx_mutex);
	rtw_sctx_init(&pxmitpriv->ack_tx_ops, 0);

	tasklet_init(&pxmitpriv->xmit_tasklet, rtl8188eu_xmit_tasklet, (unsigned long)padapter);

	return 0;

free_xmit_extbuf:
	pxmitbuf = (struct xmit_buf *)pxmitpriv->pxmit_extbuf;
	while (i--) {
		rtw_xmit_resource_free(padapter, pxmitbuf, (max_xmit_extbuf_size + XMITBUF_ALIGN_SZ));
		pxmitbuf++;
	}
	vfree(pxmitpriv->pallocated_xmit_extbuf);
	i = NR_XMITBUFF;
free_xmitbuf:
	pxmitbuf = (struct xmit_buf *)pxmitpriv->pxmitbuf;
	while (i--) {
		rtw_xmit_resource_free(padapter, pxmitbuf, (MAX_XMITBUF_SZ + XMITBUF_ALIGN_SZ));
		pxmitbuf++;
	}
	vfree(pxmitpriv->pallocated_xmitbuf);
free_frame_buf:
	vfree(pxmitpriv->pallocated_frame_buf);
exit:
	return -ENOMEM;
}

static void rtw_pkt_complete(struct adapter *padapter, struct sk_buff *pkt)
{
	u16 queue;
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;

	queue = skb_get_queue_mapping(pkt);
	if (padapter->registrypriv.wifi_spec) {
		if (__netif_subqueue_stopped(padapter->pnetdev, queue) &&
		    (pxmitpriv->hwxmits[queue].accnt < WMM_XMIT_THRESHOLD))
			netif_wake_subqueue(padapter->pnetdev, queue);
	} else {
		if (__netif_subqueue_stopped(padapter->pnetdev, queue))
			netif_wake_subqueue(padapter->pnetdev, queue);
	}

	dev_kfree_skb_any(pkt);
}

void rtw_xmit_complete(struct adapter *padapter, struct xmit_frame *pxframe)
{
	if (pxframe->pkt)
		rtw_pkt_complete(padapter, pxframe->pkt);
	pxframe->pkt = NULL;
}

void _rtw_free_xmit_priv(struct xmit_priv *pxmitpriv)
{
	int i;
	struct adapter *padapter = pxmitpriv->adapter;
	struct xmit_frame *pxmitframe = (struct xmit_frame *)pxmitpriv->pxmit_frame_buf;
	struct xmit_buf *pxmitbuf = (struct xmit_buf *)pxmitpriv->pxmitbuf;
	u32 max_xmit_extbuf_size = MAX_XMIT_EXTBUF_SZ;
	u32 num_xmit_extbuf = NR_XMIT_EXTBUFF;

	if (!pxmitpriv->pxmit_frame_buf)
		return;

	for (i = 0; i < NR_XMITFRAME; i++) {
		rtw_xmit_complete(padapter, pxmitframe);

		pxmitframe++;
	}

	for (i = 0; i < NR_XMITBUFF; i++) {
		rtw_xmit_resource_free(padapter, pxmitbuf, (MAX_XMITBUF_SZ + XMITBUF_ALIGN_SZ));
		pxmitbuf++;
	}

	vfree(pxmitpriv->pallocated_frame_buf);

	vfree(pxmitpriv->pallocated_xmitbuf);

	pxmitbuf = (struct xmit_buf *)pxmitpriv->pxmit_extbuf;
	for (i = 0; i < num_xmit_extbuf; i++) {
		rtw_xmit_resource_free(padapter, pxmitbuf, (max_xmit_extbuf_size + XMITBUF_ALIGN_SZ));
		pxmitbuf++;
	}

	vfree(pxmitpriv->pallocated_xmit_extbuf);

	kfree(pxmitpriv->hwxmits);

	mutex_destroy(&pxmitpriv->ack_tx_mutex);
}

static void update_attrib_vcs_info(struct adapter *padapter, struct xmit_frame *pxmitframe)
{
	u32	sz;
	struct pkt_attrib	*pattrib = &pxmitframe->attrib;
	struct sta_info	*psta = pattrib->psta;
	struct mlme_ext_priv	*pmlmeext = &padapter->mlmeextpriv;
	struct mlme_ext_info	*pmlmeinfo = &pmlmeext->mlmext_info;

	if (pattrib->nr_frags != 1)
		sz = padapter->xmitpriv.frag_len;
	else /* no frag */
		sz = pattrib->last_txcmdsz;

	/*  (1) RTS_Threshold is compared to the MPDU, not MSDU. */
	/*  (2) If there are more than one frag in  this MSDU, only the first frag uses protection frame. */
	/* 		Other fragments are protected by previous fragment. */
	/* 		So we only need to check the length of first fragment. */
	if (pmlmeext->cur_wireless_mode < WIRELESS_11_24N  || padapter->registrypriv.wifi_spec) {
		if (sz > padapter->registrypriv.rts_thresh) {
			pattrib->vcs_mode = RTS_CTS;
		} else {
			if (psta->rtsen)
				pattrib->vcs_mode = RTS_CTS;
			else if (psta->cts2self)
				pattrib->vcs_mode = CTS_TO_SELF;
			else
				pattrib->vcs_mode = NONE_VCS;
		}
	} else {
		while (true) {
			/* IOT action */
			if ((pmlmeinfo->assoc_AP_vendor == HT_IOT_PEER_ATHEROS) && pattrib->ampdu_en &&
			    (padapter->securitypriv.dot11PrivacyAlgrthm == _AES_)) {
				pattrib->vcs_mode = CTS_TO_SELF;
				break;
			}

			/* check ERP protection */
			if (psta->rtsen || psta->cts2self) {
				if (psta->rtsen)
					pattrib->vcs_mode = RTS_CTS;
				else if (psta->cts2self)
					pattrib->vcs_mode = CTS_TO_SELF;

				break;
			}

			/* check HT op mode */
			if (pattrib->ht_en) {
				u8 htopmode = pmlmeinfo->HT_protection;

				if ((pmlmeext->cur_bwmode && (htopmode == 2 || htopmode == 3)) ||
				    (!pmlmeext->cur_bwmode && htopmode == 3)) {
					pattrib->vcs_mode = RTS_CTS;
					break;
				}
			}

			/* check rts */
			if (sz > padapter->registrypriv.rts_thresh) {
				pattrib->vcs_mode = RTS_CTS;
				break;
			}

			/* to do list: check MIMO power save condition. */

			/* check AMPDU aggregation for TXOP */
			if (pattrib->ampdu_en) {
				pattrib->vcs_mode = RTS_CTS;
				break;
			}

			pattrib->vcs_mode = NONE_VCS;
			break;
		}
	}
}

static void update_attrib_phy_info(struct pkt_attrib *pattrib, struct sta_info *psta)
{
	/*if (psta->rtsen)
		pattrib->vcs_mode = RTS_CTS;
	else if (psta->cts2self)
		pattrib->vcs_mode = CTS_TO_SELF;
	else
		pattrib->vcs_mode = NONE_VCS;*/

	pattrib->mdata = 0;
	pattrib->eosp = 0;
	pattrib->triggered = 0;

	/* qos_en, ht_en, init rate, , bw, ch_offset, sgi */
	pattrib->qos_en = psta->qos_option;

	pattrib->raid = psta->raid;
	pattrib->ht_en = psta->htpriv.ht_option;
	pattrib->bwmode = psta->htpriv.bwmode;
	pattrib->ch_offset = psta->htpriv.ch_offset;
	pattrib->sgi = psta->htpriv.sgi;
	pattrib->ampdu_en = false;
	pattrib->retry_ctrl = false;
}

u8	qos_acm(u8 acm_mask, u8 priority)
{
	u8	change_priority = priority;

	switch (priority) {
	case 0:
	case 3:
		if (acm_mask & BIT(1))
			change_priority = 1;
		break;
	case 1:
	case 2:
		break;
	case 4:
	case 5:
		if (acm_mask & BIT(2))
			change_priority = 0;
		break;
	case 6:
	case 7:
		if (acm_mask & BIT(3))
			change_priority = 5;
		break;
	default:
		break;
	}

	return change_priority;
}

static void rtw_open_pktfile(struct sk_buff *pktptr, struct pkt_file *pfile)
{
	if (!pktptr) {
		pr_err("8188eu: pktptr is NULL\n");
		return;
	}
	if (!pfile) {
		pr_err("8188eu: pfile is NULL\n");
		return;
	}
	pfile->pkt = pktptr;
	pfile->cur_addr = pktptr->data;
	pfile->buf_start = pktptr->data;
	pfile->pkt_len = pktptr->len;
	pfile->buf_len = pktptr->len;

	pfile->cur_buffer = pfile->buf_start;
}

static uint rtw_remainder_len(struct pkt_file *pfile)
{
	return pfile->buf_len - ((size_t)(pfile->cur_addr) -
	       (size_t)(pfile->buf_start));
}

static uint rtw_pktfile_read(struct pkt_file *pfile, u8 *rmem, uint rlen)
{
	uint len;

	len = min(rtw_remainder_len(pfile), rlen);

	if (rmem)
		skb_copy_bits(pfile->pkt, pfile->buf_len - pfile->pkt_len, rmem, len);

	pfile->cur_addr += len;
	pfile->pkt_len -= len;

	return len;
}

static void set_qos(struct pkt_file *ppktfile, struct pkt_attrib *pattrib)
{
	struct ethhdr etherhdr;
	struct iphdr ip_hdr;
	s32 user_prio = 0;

	rtw_open_pktfile(ppktfile->pkt, ppktfile);
	rtw_pktfile_read(ppktfile, (unsigned char *)&etherhdr, ETH_HLEN);

	/*  get user_prio from IP hdr */
	if (pattrib->ether_type == 0x0800) {
		rtw_pktfile_read(ppktfile, (u8 *)&ip_hdr, sizeof(ip_hdr));
/* 		user_prio = (ntohs(ip_hdr.tos) >> 5) & 0x3; */
		user_prio = ip_hdr.tos >> 5;
	} else if (pattrib->ether_type == 0x888e) {
		/*  "When priority processing of data frames is supported, */
		/*  a STA's SME should send EAPOL-Key frames at the highest priority." */
		user_prio = 7;
	}

	pattrib->priority = user_prio;
	pattrib->hdrlen = WLAN_HDR_A3_QOS_LEN;
	pattrib->subtype = IEEE80211_STYPE_QOS_DATA | IEEE80211_FTYPE_DATA;
}

static s32 update_attrib(struct adapter *padapter, struct sk_buff *pkt, struct pkt_attrib *pattrib)
{
	struct pkt_file pktfile;
	struct sta_info *psta = NULL;
	struct ethhdr etherhdr;

	bool bmcast;
	struct sta_priv		*pstapriv = &padapter->stapriv;
	struct security_priv	*psecuritypriv = &padapter->securitypriv;
	struct mlme_priv	*pmlmepriv = &padapter->mlmepriv;
	struct qos_priv		*pqospriv = &pmlmepriv->qospriv;
	int res = _SUCCESS;



	rtw_open_pktfile(pkt, &pktfile);
	rtw_pktfile_read(&pktfile, (u8 *)&etherhdr, ETH_HLEN);

	pattrib->ether_type = ntohs(etherhdr.h_proto);

	memcpy(pattrib->dst, &etherhdr.h_dest, ETH_ALEN);
	memcpy(pattrib->src, &etherhdr.h_source, ETH_ALEN);

	pattrib->pctrl = 0;

	if (check_fwstate(pmlmepriv, WIFI_ADHOC_STATE) ||
	    check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE)) {
		memcpy(pattrib->ra, pattrib->dst, ETH_ALEN);
		memcpy(pattrib->ta, pattrib->src, ETH_ALEN);
	} else if (check_fwstate(pmlmepriv, WIFI_STATION_STATE)) {
		memcpy(pattrib->ra, get_bssid(pmlmepriv), ETH_ALEN);
		memcpy(pattrib->ta, pattrib->src, ETH_ALEN);
	} else if (check_fwstate(pmlmepriv, WIFI_AP_STATE)) {
		memcpy(pattrib->ra, pattrib->dst, ETH_ALEN);
		memcpy(pattrib->ta, get_bssid(pmlmepriv), ETH_ALEN);
	}

	pattrib->pktlen = pktfile.pkt_len;

	if (pattrib->ether_type == ETH_P_IP) {
		/*  The following is for DHCP and ARP packet, we use cck1M to tx these packets and let LPS awake some time */
		/*  to prevent DHCP protocol fail */
		u8 tmp[24];

		rtw_pktfile_read(&pktfile, &tmp[0], 24);
		pattrib->dhcp_pkt = 0;
		if (pktfile.pkt_len > 282) {/* MINIMUM_DHCP_PACKET_SIZE) { */
			if (((tmp[21] == 68) && (tmp[23] == 67)) ||
			    ((tmp[21] == 67) && (tmp[23] == 68))) {
				/*  68 : UDP BOOTP client */
				/*  67 : UDP BOOTP server */
				/*  Use low rate to send DHCP packet. */
				pattrib->dhcp_pkt = 1;
			}
		}
	}

	/*  If EAPOL , ARP , OR DHCP packet, driver must be in active mode. */
	if ((pattrib->ether_type == 0x0806) || (pattrib->ether_type == 0x888e) || (pattrib->dhcp_pkt == 1))
		rtw_lps_ctrl_wk_cmd(padapter, LPS_CTRL_SPECIAL_PACKET, 1);

	bmcast = is_multicast_ether_addr(pattrib->ra);

	/*  get sta_info */
	if (bmcast) {
		psta = rtw_get_bcmc_stainfo(padapter);
	} else {
		psta = rtw_get_stainfo(pstapriv, pattrib->ra);
		if (!psta) { /*  if we cannot get psta => drrp the pkt */
			res = _FAIL;
			goto exit;
		} else if (check_fwstate(pmlmepriv, WIFI_AP_STATE) && !(psta->state & _FW_LINKED)) {
			res = _FAIL;
			goto exit;
		}
	}

	if (psta) {
		pattrib->mac_id = psta->mac_id;
		pattrib->psta = psta;
	} else {
		/*  if we cannot get psta => drop the pkt */
		res = _FAIL;
		goto exit;
	}

	pattrib->ack_policy = 0;
	/*  get ether_hdr_len */
	pattrib->pkt_hdrlen = ETH_HLEN;/* pattrib->ether_type == 0x8100) ? (14 + 4): 14; vlan tag */

	pattrib->hdrlen = WLAN_HDR_A3_LEN;
	pattrib->subtype = IEEE80211_FTYPE_DATA;
	pattrib->priority = 0;

	if (check_fwstate(pmlmepriv, WIFI_AP_STATE | WIFI_ADHOC_STATE | WIFI_ADHOC_MASTER_STATE)) {
		if (psta->qos_option)
			set_qos(&pktfile, pattrib);
	} else {
		if (pqospriv->qos_option) {
			set_qos(&pktfile, pattrib);

			if (pmlmepriv->acm_mask != 0)
				pattrib->priority = qos_acm(pmlmepriv->acm_mask, pattrib->priority);
		}
	}

	if (psta->ieee8021x_blocked) {
		pattrib->encrypt = 0;

		if ((pattrib->ether_type != 0x888e) && !check_fwstate(pmlmepriv, WIFI_MP_STATE)) {
			res = _FAIL;
			goto exit;
		}
	} else {
		GET_ENCRY_ALGO(psecuritypriv, psta, pattrib->encrypt, bmcast);

		switch (psecuritypriv->dot11AuthAlgrthm) {
		case dot11AuthAlgrthm_Open:
		case dot11AuthAlgrthm_Shared:
		case dot11AuthAlgrthm_Auto:
			pattrib->key_idx = (u8)psecuritypriv->dot11PrivacyKeyIndex;
			break;
		case dot11AuthAlgrthm_8021X:
			if (bmcast)
				pattrib->key_idx = (u8)psecuritypriv->dot118021XGrpKeyid;
			else
				pattrib->key_idx = 0;
			break;
		default:
			pattrib->key_idx = 0;
			break;
		}
	}

	switch (pattrib->encrypt) {
	case _WEP40_:
	case _WEP104_:
		pattrib->iv_len = 4;
		pattrib->icv_len = 4;
		break;
	case _TKIP_:
		pattrib->iv_len = 8;
		pattrib->icv_len = 4;

		if (padapter->securitypriv.busetkipkey == _FAIL) {
			res = _FAIL;
			goto exit;
		}
		break;
	case _AES_:
		pattrib->iv_len = 8;
		pattrib->icv_len = 8;
		break;
	default:
		pattrib->iv_len = 0;
		pattrib->icv_len = 0;
		break;
	}

	if (pattrib->encrypt &&
	    (padapter->securitypriv.sw_encrypt || !psecuritypriv->hw_decrypted))
		pattrib->bswenc = true;
	else
		pattrib->bswenc = false;

	update_attrib_phy_info(pattrib, psta);

exit:

	return res;
}

static s32 xmitframe_addmic(struct adapter *padapter, struct xmit_frame *pxmitframe)
{
	int curfragnum, length;
	u8	*pframe, *payload, mic[8];
	struct	mic_data micdata;
	struct	sta_info *stainfo;
	struct	pkt_attrib *pattrib = &pxmitframe->attrib;
	struct	security_priv	*psecuritypriv = &padapter->securitypriv;
	struct	xmit_priv *pxmitpriv = &padapter->xmitpriv;
	u8 priority[4] = {0x0, 0x0, 0x0, 0x0};
	u8 hw_hdr_offset = 0;

	if (pattrib->psta)
		stainfo = pattrib->psta;
	else
		stainfo = rtw_get_stainfo(&padapter->stapriv, &pattrib->ra[0]);

	hw_hdr_offset = TXDESC_SIZE + (pxmitframe->pkt_offset * PACKET_OFFSET_SZ);

	if (pattrib->encrypt == _TKIP_) {/* if (psecuritypriv->dot11PrivacyAlgrthm == _TKIP_PRIVACY_) */
		/* encode mic code */
		if (stainfo) {
			u8 null_key[16] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
					   0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
					   0x0, 0x0};

			pframe = pxmitframe->buf_addr + hw_hdr_offset;

			if (is_multicast_ether_addr(pattrib->ra)) {
				if (!memcmp(psecuritypriv->dot118021XGrptxmickey[psecuritypriv->dot118021XGrpKeyid].skey, null_key, 16))
					return _FAIL;
				/* start to calculate the mic code */
				rtw_secmicsetkey(&micdata, psecuritypriv->dot118021XGrptxmickey[psecuritypriv->dot118021XGrpKeyid].skey);
			} else {
				if (!memcmp(&stainfo->dot11tkiptxmickey.skey[0], null_key, 16)) {
					/* msleep(10); */
					return _FAIL;
				}
				/* start to calculate the mic code */
				rtw_secmicsetkey(&micdata, &stainfo->dot11tkiptxmickey.skey[0]);
			}

			if (pframe[1] & 1) {   /* ToDS == 1 */
				rtw_secmicappend(&micdata, &pframe[16], 6);  /* DA */
				if (pframe[1] & 2)  /* From Ds == 1 */
					rtw_secmicappend(&micdata, &pframe[24], 6);
				else
					rtw_secmicappend(&micdata, &pframe[10], 6);
			} else {	/* ToDS == 0 */
				rtw_secmicappend(&micdata, &pframe[4], 6);   /* DA */
				if (pframe[1] & 2)  /* From Ds == 1 */
					rtw_secmicappend(&micdata, &pframe[16], 6);
				else
					rtw_secmicappend(&micdata, &pframe[10], 6);
			}

			if (pattrib->qos_en)
				priority[0] = (u8)pxmitframe->attrib.priority;

			rtw_secmicappend(&micdata, &priority[0], 4);

			payload = pframe;

			for (curfragnum = 0; curfragnum < pattrib->nr_frags; curfragnum++) {
				payload = PTR_ALIGN(payload, 4);

				payload = payload + pattrib->hdrlen + pattrib->iv_len;
				if ((curfragnum + 1) == pattrib->nr_frags) {
					length = pattrib->last_txcmdsz - pattrib->hdrlen - pattrib->iv_len - ((pattrib->bswenc) ? pattrib->icv_len : 0);
					rtw_secmicappend(&micdata, payload, length);
					payload = payload + length;
				} else {
					length = pxmitpriv->frag_len - pattrib->hdrlen - pattrib->iv_len - ((pattrib->bswenc) ? pattrib->icv_len : 0);
					rtw_secmicappend(&micdata, payload, length);
					payload = payload + length + pattrib->icv_len;
				}
			}
			rtw_secgetmic(&micdata, &mic[0]);
			/* add mic code  and add the mic code length in last_txcmdsz */

			memcpy(payload, &mic[0], 8);
			pattrib->last_txcmdsz += 8;

			payload = payload - pattrib->last_txcmdsz + 8;
		}
	}

	return _SUCCESS;
}

static void xmitframe_swencrypt(struct adapter *padapter, struct xmit_frame *pxmitframe)
{
	struct	pkt_attrib	 *pattrib = &pxmitframe->attrib;

	if (!pattrib->bswenc)
		return;

	switch (pattrib->encrypt) {
	case _WEP40_:
	case _WEP104_:
		rtw_wep_encrypt(padapter, pxmitframe);
		break;
	case _TKIP_:
		rtw_tkip_encrypt(padapter, pxmitframe);
		break;
	case _AES_:
		rtw_aes_encrypt(padapter, pxmitframe);
		break;
	default:
		break;
	}
}

s32 rtw_make_wlanhdr(struct adapter *padapter, u8 *hdr, struct pkt_attrib *pattrib)
{
	u16 *qc;

	struct ieee80211_hdr *pwlanhdr = (struct ieee80211_hdr *)hdr;
	struct mlme_priv *pmlmepriv = &padapter->mlmepriv;
	struct qos_priv *pqospriv = &pmlmepriv->qospriv;
	bool qos_option;
	__le16 *fctrl = &pwlanhdr->frame_control;

	struct sta_info *psta;

	if (pattrib->psta)
		psta = pattrib->psta;
	else if (is_multicast_ether_addr(pattrib->ra))
		psta = rtw_get_bcmc_stainfo(padapter);
	else
		psta = rtw_get_stainfo(&padapter->stapriv, pattrib->ra);

	memset(hdr, 0, WLANHDR_OFFSET);

	SetFrameSubType(fctrl, pattrib->subtype);

	if (!(pattrib->subtype & IEEE80211_FTYPE_DATA))
		return _SUCCESS;

	if (check_fwstate(pmlmepriv, WIFI_STATION_STATE)) {
		/* to_ds = 1, fr_ds = 0; */
		/* Data transfer to AP */
		SetToDs(fctrl);
		memcpy(pwlanhdr->addr1, get_bssid(pmlmepriv), ETH_ALEN);
		memcpy(pwlanhdr->addr2, pattrib->src, ETH_ALEN);
		memcpy(pwlanhdr->addr3, pattrib->dst, ETH_ALEN);
		qos_option = pqospriv->qos_option;
	} else if (check_fwstate(pmlmepriv,  WIFI_AP_STATE)) {
		/* to_ds = 0, fr_ds = 1; */
		SetFrDs(fctrl);
		memcpy(pwlanhdr->addr1, pattrib->dst, ETH_ALEN);
		memcpy(pwlanhdr->addr2, get_bssid(pmlmepriv), ETH_ALEN);
		memcpy(pwlanhdr->addr3, pattrib->src, ETH_ALEN);
		qos_option = psta->qos_option;
	} else if (check_fwstate(pmlmepriv, WIFI_ADHOC_STATE) ||
		   check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE)) {
		memcpy(pwlanhdr->addr1, pattrib->dst, ETH_ALEN);
		memcpy(pwlanhdr->addr2, pattrib->src, ETH_ALEN);
		memcpy(pwlanhdr->addr3, get_bssid(pmlmepriv), ETH_ALEN);
		qos_option = psta->qos_option;
	} else {
		return _FAIL;
	}

	if (pattrib->mdata)
		SetMData(fctrl);

	if (pattrib->encrypt)
		SetPrivacy(fctrl);

	if (qos_option) {
		qc = (unsigned short *)(hdr + pattrib->hdrlen - 2);

		if (pattrib->priority)
			SetPriority(qc, pattrib->priority);

		SetEOSP(qc, pattrib->eosp);

		SetAckpolicy(qc, pattrib->ack_policy);
	}

	/* TODO: fill HT Control Field */

	/* Update Seq Num will be handled by f/w */
	if (psta) {
		psta->sta_xmitpriv.txseq_tid[pattrib->priority]++;
		psta->sta_xmitpriv.txseq_tid[pattrib->priority] &= 0xFFF;

		pattrib->seqnum = psta->sta_xmitpriv.txseq_tid[pattrib->priority];

		SetSeqNum(hdr, pattrib->seqnum);

		/* check if enable ampdu */
		if (pattrib->ht_en && psta->htpriv.ampdu_enable) {
			if (psta->htpriv.agg_enable_bitmap & BIT(pattrib->priority))
				pattrib->ampdu_en = true;
		}

		/* re-check if enable ampdu by BA_starting_seqctrl */
		if (pattrib->ampdu_en) {
			u16 tx_seq;

			tx_seq = psta->BA_starting_seqctrl[pattrib->priority & 0x0f];

			/* check BA_starting_seqctrl */
			if (SN_LESS(pattrib->seqnum, tx_seq)) {
				pattrib->ampdu_en = false;/* AGG BK */
			} else if (SN_EQUAL(pattrib->seqnum, tx_seq)) {
				psta->BA_starting_seqctrl[pattrib->priority & 0x0f] = (tx_seq + 1) & 0xfff;

				pattrib->ampdu_en = true;/* AGG EN */
			} else {
				psta->BA_starting_seqctrl[pattrib->priority & 0x0f] = (pattrib->seqnum + 1) & 0xfff;
				pattrib->ampdu_en = true;/* AGG EN */
			}
		}
	}

	return _SUCCESS;
}

s32 rtw_txframes_pending(struct adapter *padapter)
{
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;

	return (!list_empty(&pxmitpriv->be_pending) ||
		!list_empty(&pxmitpriv->bk_pending) ||
		!list_empty(&pxmitpriv->vi_pending) ||
		!list_empty(&pxmitpriv->vo_pending));
}

s32 rtw_txframes_sta_ac_pending(struct adapter *padapter, struct pkt_attrib *pattrib)
{
	struct sta_info *psta;
	struct tx_servq *ptxservq;
	int priority = pattrib->priority;

	psta = pattrib->psta;

	switch (priority) {
	case 1:
	case 2:
		ptxservq = &psta->sta_xmitpriv.bk_q;
		break;
	case 4:
	case 5:
		ptxservq = &psta->sta_xmitpriv.vi_q;
		break;
	case 6:
	case 7:
		ptxservq = &psta->sta_xmitpriv.vo_q;
		break;
	case 0:
	case 3:
	default:
		ptxservq = &psta->sta_xmitpriv.be_q;
		break;
	}

	if (ptxservq)
		return ptxservq->qcnt;
	return 0;
}

/*
 * This sub-routine will perform all the following:
 *
 * 1. remove 802.3 header.
 * 2. create wlan_header, based on the info in pxmitframe
 * 3. append sta's iv/ext-iv
 * 4. append LLC
 * 5. move frag chunk from pframe to pxmitframe->mem
 * 6. apply sw-encrypt, if necessary.
 */
s32 rtw_xmitframe_coalesce(struct adapter *padapter, struct sk_buff *pkt, struct xmit_frame *pxmitframe)
{
	struct pkt_file pktfile;
	s32 frg_inx, frg_len, mpdu_len, llc_sz, mem_sz;
	u8 *pframe, *mem_start;
	u8 hw_hdr_offset;
	struct sta_info		*psta;
	struct xmit_priv	*pxmitpriv = &padapter->xmitpriv;
	struct pkt_attrib	*pattrib = &pxmitframe->attrib;
	u8 *pbuf_start;
	bool bmcst = is_multicast_ether_addr(pattrib->ra);
	s32 res = _SUCCESS;

	if (!pkt)
		return _FAIL;

	psta = rtw_get_stainfo(&padapter->stapriv, pattrib->ra);

	if (!psta)
		return _FAIL;

	if (!pxmitframe->buf_addr)
		return _FAIL;

	pbuf_start = pxmitframe->buf_addr;

	hw_hdr_offset =  TXDESC_SIZE + (pxmitframe->pkt_offset * PACKET_OFFSET_SZ);

	mem_start = pbuf_start +	hw_hdr_offset;

	if (rtw_make_wlanhdr(padapter, mem_start, pattrib) == _FAIL) {
		res = _FAIL;
		goto exit;
	}

	rtw_open_pktfile(pkt, &pktfile);
	rtw_pktfile_read(&pktfile, NULL, pattrib->pkt_hdrlen);

	frg_inx = 0;
	frg_len = pxmitpriv->frag_len - 4;/* 2346-4 = 2342 */

	while (1) {
		llc_sz = 0;

		mpdu_len = frg_len;

		pframe = mem_start;

		SetMFrag(mem_start);

		pframe += pattrib->hdrlen;
		mpdu_len -= pattrib->hdrlen;

		/* adding icv, if necessary... */
		if (pattrib->iv_len) {
			switch (pattrib->encrypt) {
			case _WEP40_:
			case _WEP104_:
				WEP_IV(pattrib->iv, psta->dot11txpn, pattrib->key_idx);
				break;
			case _TKIP_:
				if (bmcst)
					TKIP_IV(pattrib->iv, psta->dot11txpn, pattrib->key_idx);
				else
					TKIP_IV(pattrib->iv, psta->dot11txpn, 0);
				break;
			case _AES_:
				if (bmcst)
					AES_IV(pattrib->iv, psta->dot11txpn, pattrib->key_idx);
				else
					AES_IV(pattrib->iv, psta->dot11txpn, 0);
				break;
			}

			memcpy(pframe, pattrib->iv, pattrib->iv_len);

			pframe += pattrib->iv_len;

			mpdu_len -= pattrib->iv_len;
		}

		if (frg_inx == 0) {
			llc_sz = rtw_put_snap(pframe, pattrib->ether_type);
			pframe += llc_sz;
			mpdu_len -= llc_sz;
		}

		if ((pattrib->icv_len > 0) && (pattrib->bswenc))
			mpdu_len -= pattrib->icv_len;

		if (bmcst) {
			/*  don't do fragment to broadcast/multicast packets */
			mem_sz = rtw_pktfile_read(&pktfile, pframe, pattrib->pktlen);
		} else {
			mem_sz = rtw_pktfile_read(&pktfile, pframe, mpdu_len);
		}

		pframe += mem_sz;

		if ((pattrib->icv_len > 0) && (pattrib->bswenc)) {
			memcpy(pframe, pattrib->icv, pattrib->icv_len);
			pframe += pattrib->icv_len;
		}

		frg_inx++;

		if (bmcst || pktfile.pkt_len == 0) {
			pattrib->nr_frags = frg_inx;

			pattrib->last_txcmdsz = pattrib->hdrlen + pattrib->iv_len + ((pattrib->nr_frags == 1) ? llc_sz : 0) +
						((pattrib->bswenc) ? pattrib->icv_len : 0) + mem_sz;

			ClearMFrag(mem_start);

			break;
		}

		mem_start = PTR_ALIGN(pframe, 4) + hw_hdr_offset;
		memcpy(mem_start, pbuf_start + hw_hdr_offset, pattrib->hdrlen);
	}

	if (xmitframe_addmic(padapter, pxmitframe) == _FAIL) {
		res = _FAIL;
		goto exit;
	}

	xmitframe_swencrypt(padapter, pxmitframe);

	if (!bmcst)
		update_attrib_vcs_info(padapter, pxmitframe);
	else
		pattrib->vcs_mode = NONE_VCS;

exit:

	return res;
}

/* Logical Link Control(LLC) SubNetwork Attachment Point(SNAP) header
 * IEEE LLC/SNAP header contains 8 octets
 * First 3 octets comprise the LLC portion
 * SNAP portion, 5 octets, is divided into two fields:
 *	Organizationally Unique Identifier(OUI), 3 octets,
 *	type, defined by that organization, 2 octets.
 */
s32 rtw_put_snap(u8 *data, u16 h_proto)
{
	struct ieee80211_snap_hdr *snap;
	u8 *oui;

	snap = (struct ieee80211_snap_hdr *)data;
	snap->dsap = 0xaa;
	snap->ssap = 0xaa;
	snap->ctrl = 0x03;

	if (h_proto == 0x8137 || h_proto == 0x80f3)
		oui = P802_1H_OUI;
	else
		oui = RFC1042_OUI;

	snap->oui[0] = oui[0];
	snap->oui[1] = oui[1];
	snap->oui[2] = oui[2];

	*(__be16 *)(data + SNAP_SIZE) = htons(h_proto);

	return SNAP_SIZE + sizeof(u16);
}

void rtw_count_tx_stats(struct adapter *padapter, struct xmit_frame *pxmitframe, int sz)
{
	struct sta_info *psta = NULL;
	struct stainfo_stats *pstats = NULL;
	struct xmit_priv	*pxmitpriv = &padapter->xmitpriv;
	struct mlme_priv	*pmlmepriv = &padapter->mlmepriv;

	if ((pxmitframe->frame_tag & 0x0f) == DATA_FRAMETAG) {
		pxmitpriv->tx_bytes += sz;
		pmlmepriv->LinkDetectInfo.NumTxOkInPeriod += pxmitframe->agg_num;

		psta = pxmitframe->attrib.psta;
		if (psta) {
			pstats = &psta->sta_stats;
			pstats->tx_pkts += pxmitframe->agg_num;
			pstats->tx_bytes += sz;
		}
	}
}

struct xmit_buf *rtw_alloc_xmitbuf_ext(struct xmit_priv *pxmitpriv)
{
	struct xmit_buf *pxmitbuf =  NULL;
	struct list_head *plist, *phead;
	struct __queue *pfree_queue = &pxmitpriv->free_xmit_extbuf_queue;
	unsigned long flags;

	spin_lock_irqsave(&pfree_queue->lock, flags);

	if (list_empty(&pfree_queue->queue)) {
		pxmitbuf = NULL;
	} else {
		phead = get_list_head(pfree_queue);

		plist = phead->next;

		pxmitbuf = container_of(plist, struct xmit_buf, list);

		list_del_init(&pxmitbuf->list);
	}

	if (pxmitbuf) {
		pxmitpriv->free_xmit_extbuf_cnt--;

		pxmitbuf->priv_data = NULL;
		/* pxmitbuf->ext_tag = true; */

		if (pxmitbuf->sctx)
			rtw_sctx_done_err(&pxmitbuf->sctx, RTW_SCTX_DONE_BUF_ALLOC);
	}

	spin_unlock_irqrestore(&pfree_queue->lock, flags);

	return pxmitbuf;
}

s32 rtw_free_xmitbuf_ext(struct xmit_priv *pxmitpriv, struct xmit_buf *pxmitbuf)
{
	struct __queue *pfree_queue = &pxmitpriv->free_xmit_extbuf_queue;
	unsigned long flags;

	if (!pxmitbuf)
		return _FAIL;

	spin_lock_irqsave(&pfree_queue->lock, flags);

	list_del_init(&pxmitbuf->list);

	list_add_tail(&pxmitbuf->list, get_list_head(pfree_queue));
	pxmitpriv->free_xmit_extbuf_cnt++;

	spin_unlock_irqrestore(&pfree_queue->lock, flags);

	return _SUCCESS;
}

struct xmit_buf *rtw_alloc_xmitbuf(struct xmit_priv *pxmitpriv)
{
	struct xmit_buf *pxmitbuf =  NULL;
	struct list_head *plist, *phead;
	struct __queue *pfree_xmitbuf_queue = &pxmitpriv->free_xmitbuf_queue;
	unsigned long flags;

	spin_lock_irqsave(&pfree_xmitbuf_queue->lock, flags);

	if (list_empty(&pfree_xmitbuf_queue->queue)) {
		pxmitbuf = NULL;
	} else {
		phead = get_list_head(pfree_xmitbuf_queue);

		plist = phead->next;

		pxmitbuf = container_of(plist, struct xmit_buf, list);

		list_del_init(&pxmitbuf->list);
	}

	if (pxmitbuf) {
		pxmitpriv->free_xmitbuf_cnt--;
		pxmitbuf->priv_data = NULL;
		if (pxmitbuf->sctx)
			rtw_sctx_done_err(&pxmitbuf->sctx, RTW_SCTX_DONE_BUF_ALLOC);
	}
	spin_unlock_irqrestore(&pfree_xmitbuf_queue->lock, flags);

	return pxmitbuf;
}

s32 rtw_free_xmitbuf(struct xmit_priv *pxmitpriv, struct xmit_buf *pxmitbuf)
{
	struct __queue *pfree_xmitbuf_queue = &pxmitpriv->free_xmitbuf_queue;
	unsigned long flags;

	if (!pxmitbuf)
		return _FAIL;

	if (pxmitbuf->sctx)
		rtw_sctx_done_err(&pxmitbuf->sctx, RTW_SCTX_DONE_BUF_FREE);

	if (pxmitbuf->ext_tag) {
		rtw_free_xmitbuf_ext(pxmitpriv, pxmitbuf);
	} else {
		spin_lock_irqsave(&pfree_xmitbuf_queue->lock, flags);

		list_del_init(&pxmitbuf->list);

		list_add_tail(&pxmitbuf->list, get_list_head(pfree_xmitbuf_queue));

		pxmitpriv->free_xmitbuf_cnt++;
		spin_unlock_irqrestore(&pfree_xmitbuf_queue->lock, flags);
	}

	return _SUCCESS;
}

/*
 * Calling context:
 * 1. OS_TXENTRY
 * 2. RXENTRY (rx_thread or RX_ISR/RX_CallBack)
 *
 * If we turn on USE_RXTHREAD, then, no need for critical section.
 * Otherwise, we must use _enter/_exit critical to protect free_xmit_queue...
 *
 * Must be very very cautious...
 */
struct xmit_frame *rtw_alloc_xmitframe(struct xmit_priv *pxmitpriv)/* _queue *pfree_xmit_queue) */
{
	/*
	 * Please remember to use all the osdep_service api,
	 * and lock/unlock or _enter/_exit critical to protect
	 * pfree_xmit_queue
	 */

	struct xmit_frame *pxframe = NULL;
	struct list_head *plist, *phead;
	struct __queue *pfree_xmit_queue = &pxmitpriv->free_xmit_queue;

	spin_lock_bh(&pfree_xmit_queue->lock);

	if (list_empty(&pfree_xmit_queue->queue))
		goto out;

	phead = get_list_head(pfree_xmit_queue);
	plist = phead->next;
	pxframe = container_of(plist, struct xmit_frame, list);
	list_del_init(&pxframe->list);

	pxmitpriv->free_xmitframe_cnt--;

	pxframe->buf_addr = NULL;
	pxframe->pxmitbuf = NULL;

	memset(&pxframe->attrib, 0, sizeof(struct pkt_attrib));
	/* pxframe->attrib.psta = NULL; */

	pxframe->frame_tag = DATA_FRAMETAG;

	pxframe->pkt = NULL;
	pxframe->pkt_offset = 1;/* default use pkt_offset to fill tx desc */

	pxframe->agg_num = 1;
	pxframe->ack_report = 0;

out:
	spin_unlock_bh(&pfree_xmit_queue->lock);
	return pxframe;
}

s32 rtw_free_xmitframe(struct xmit_priv *pxmitpriv, struct xmit_frame *pxmitframe)
{
	struct __queue *pfree_xmit_queue = &pxmitpriv->free_xmit_queue;
	struct adapter *padapter = pxmitpriv->adapter;
	struct sk_buff *pndis_pkt = NULL;

	if (!pxmitframe)
		goto exit;

	spin_lock_bh(&pfree_xmit_queue->lock);

	list_del_init(&pxmitframe->list);

	if (pxmitframe->pkt) {
		pndis_pkt = pxmitframe->pkt;
		pxmitframe->pkt = NULL;
	}

	list_add_tail(&pxmitframe->list, get_list_head(pfree_xmit_queue));

	pxmitpriv->free_xmitframe_cnt++;

	spin_unlock_bh(&pfree_xmit_queue->lock);

	if (pndis_pkt)
		rtw_pkt_complete(padapter, pndis_pkt);

exit:

	return _SUCCESS;
}

void rtw_free_xmitframe_list(struct xmit_priv *pxmitpriv, struct list_head *xframe_list)
{
	struct	xmit_frame *pxmitframe, *tmp_xmitframe;

	list_for_each_entry_safe(pxmitframe, tmp_xmitframe, xframe_list, list)
		rtw_free_xmitframe(pxmitpriv, pxmitframe);
}

struct xmit_frame *rtw_dequeue_xframe(struct xmit_priv *pxmitpriv, struct hw_xmit *phwxmit_i)
{
	struct hw_xmit *phwxmit;
	struct tx_servq *ptxservq, *tmp_txservq;
	struct list_head *xframe_list;
	struct xmit_frame *pxmitframe = NULL;
	struct adapter *padapter = pxmitpriv->adapter;
	struct registry_priv	*pregpriv = &padapter->registrypriv;
	int i, inx[] = { 0, 1, 2, 3 };

	if (pregpriv->wifi_spec == 1) {
		for (i = 0; i < ARRAY_SIZE(inx); i++)
			inx[i] = pxmitpriv->wmm_para_seq[i];
	}

	spin_lock_bh(&pxmitpriv->lock);

	for (i = 0; i < HWXMIT_ENTRY; i++) {
		phwxmit = phwxmit_i + inx[i];
		list_for_each_entry_safe(ptxservq, tmp_txservq, phwxmit->sta_list, tx_pending) {
			xframe_list = &ptxservq->sta_pending;
			if (list_empty(xframe_list))
				continue;

			pxmitframe = container_of(xframe_list->next, struct xmit_frame, list);
			list_del_init(&pxmitframe->list);

			phwxmit->accnt--;
			ptxservq->qcnt--;

			/* Remove sta node when there are no pending packets. */
			if (list_empty(xframe_list))
				list_del_init(&ptxservq->tx_pending);

			goto exit;
		}
	}
exit:
	spin_unlock_bh(&pxmitpriv->lock);

	return pxmitframe;
}

struct tx_servq *rtw_get_sta_pending(struct adapter *padapter, struct sta_info *psta, int up, u8 *ac)
{
	struct tx_servq *ptxservq;

	switch (up) {
	case 1:
	case 2:
		ptxservq = &psta->sta_xmitpriv.bk_q;
		*(ac) = 3;
		break;
	case 4:
	case 5:
		ptxservq = &psta->sta_xmitpriv.vi_q;
		*(ac) = 1;
		break;
	case 6:
	case 7:
		ptxservq = &psta->sta_xmitpriv.vo_q;
		*(ac) = 0;
		break;
	case 0:
	case 3:
	default:
		ptxservq = &psta->sta_xmitpriv.be_q;
		*(ac) = 2;
	break;
	}

	return ptxservq;
}

/*
 * Will enqueue pxmitframe to the proper queue,
 * and indicate it to xx_pending list.....
 */
s32 rtw_xmit_classifier(struct adapter *padapter, struct xmit_frame *pxmitframe)
{
	u8	ac_index;
	struct sta_info	*psta;
	struct tx_servq	*ptxservq;
	struct pkt_attrib	*pattrib = &pxmitframe->attrib;
	struct sta_priv	*pstapriv = &padapter->stapriv;
	struct hw_xmit	*phwxmits =  padapter->xmitpriv.hwxmits;
	int res = _SUCCESS;

	if (pattrib->psta)
		psta = pattrib->psta;
	else
		psta = rtw_get_stainfo(pstapriv, pattrib->ra);

	if (!psta) {
		res = _FAIL;
		goto exit;
	}

	ptxservq = rtw_get_sta_pending(padapter, psta, pattrib->priority, (u8 *)(&ac_index));

	if (list_empty(&ptxservq->tx_pending))
		list_add_tail(&ptxservq->tx_pending, phwxmits[ac_index].sta_list);

	list_add_tail(&pxmitframe->list, &ptxservq->sta_pending);
	ptxservq->qcnt++;
	phwxmits[ac_index].accnt++;
exit:

	return res;
}

int rtw_alloc_hwxmits(struct adapter *padapter)
{
	struct hw_xmit *hwxmits;
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;

	pxmitpriv->hwxmits = kcalloc(HWXMIT_ENTRY, sizeof(struct hw_xmit), GFP_KERNEL);
	if (!pxmitpriv->hwxmits)
		return -ENOMEM;

	hwxmits = pxmitpriv->hwxmits;

	hwxmits[0].sta_list = &pxmitpriv->vo_pending;
	hwxmits[1].sta_list = &pxmitpriv->vi_pending;
	hwxmits[2].sta_list = &pxmitpriv->be_pending;
	hwxmits[3].sta_list = &pxmitpriv->bk_pending;

	return 0;
}

static int rtw_br_client_tx(struct adapter *padapter, struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	int res, is_vlan_tag = 0, i, do_nat25 = 1;
	unsigned short vlan_hdr = 0;
	void *br_port = NULL;

	rcu_read_lock();
	br_port = rcu_dereference(padapter->pnetdev->rx_handler_data);
	rcu_read_unlock();
	spin_lock_bh(&padapter->br_ext_lock);
	if (!(skb->data[0] & 1) && br_port &&
	    memcmp(skb->data + ETH_ALEN, padapter->br_mac, ETH_ALEN) &&
	    *((__be16 *)(skb->data + ETH_ALEN * 2)) != htons(ETH_P_8021Q) &&
	    *((__be16 *)(skb->data + ETH_ALEN * 2)) == htons(ETH_P_IP) &&
	    !memcmp(padapter->scdb_mac, skb->data + ETH_ALEN, ETH_ALEN) && padapter->scdb_entry) {
		memcpy(skb->data + ETH_ALEN, GET_MY_HWADDR(padapter), ETH_ALEN);
		padapter->scdb_entry->ageing_timer = jiffies;
		spin_unlock_bh(&padapter->br_ext_lock);
	} else {
		if (*((__be16 *)(skb->data + ETH_ALEN * 2)) == htons(ETH_P_8021Q)) {
			is_vlan_tag = 1;
			vlan_hdr = *((unsigned short *)(skb->data + ETH_ALEN * 2 + 2));
			for (i = 0; i < 6; i++)
				*((unsigned short *)(skb->data + ETH_ALEN * 2 + 2 - i * 2)) = *((unsigned short *)(skb->data + ETH_ALEN * 2 - 2 - i * 2));
			skb_pull(skb, 4);
		}
		if (!memcmp(skb->data + ETH_ALEN, padapter->br_mac, ETH_ALEN) &&
		    (*((__be16 *)(skb->data + ETH_ALEN * 2)) == htons(ETH_P_IP)))
			memcpy(padapter->br_ip, skb->data + WLAN_ETHHDR_LEN + 12, 4);

		if (*((__be16 *)(skb->data + ETH_ALEN * 2)) == htons(ETH_P_IP)) {
			if (memcmp(padapter->scdb_mac, skb->data + ETH_ALEN, ETH_ALEN)) {
				padapter->scdb_entry = (struct nat25_network_db_entry *)scdb_findEntry(padapter,
							skb->data + WLAN_ETHHDR_LEN + 12);
				if (padapter->scdb_entry) {
					memcpy(padapter->scdb_mac, skb->data + ETH_ALEN, ETH_ALEN);
					memcpy(padapter->scdb_ip, skb->data + WLAN_ETHHDR_LEN + 12, 4);
					padapter->scdb_entry->ageing_timer = jiffies;
					do_nat25 = 0;
				}
			} else {
				if (padapter->scdb_entry) {
					padapter->scdb_entry->ageing_timer = jiffies;
					do_nat25 = 0;
				} else {
					memset(padapter->scdb_mac, 0, ETH_ALEN);
					memset(padapter->scdb_ip, 0, 4);
				}
			}
		}
		spin_unlock_bh(&padapter->br_ext_lock);
		if (do_nat25) {
			if (nat25_db_handle(padapter, skb, NAT25_CHECK) == 0) {
				struct sk_buff *newskb;

				if (is_vlan_tag) {
					skb_push(skb, 4);
					for (i = 0; i < 6; i++)
						*((unsigned short *)(skb->data + i * 2)) = *((unsigned short *)(skb->data + 4 + i * 2));
					*((__be16 *)(skb->data + ETH_ALEN * 2)) = htons(ETH_P_8021Q);
					*((unsigned short *)(skb->data + ETH_ALEN * 2 + 2)) = vlan_hdr;
				}

				newskb = skb_copy(skb, GFP_ATOMIC);
				if (!newskb)
					return -1;
				dev_kfree_skb_any(skb);

				*pskb = skb = newskb;
				if (is_vlan_tag) {
					vlan_hdr = *((unsigned short *)(skb->data + ETH_ALEN * 2 + 2));
					for (i = 0; i < 6; i++)
						*((unsigned short *)(skb->data + ETH_ALEN * 2 + 2 - i * 2)) = *((unsigned short *)(skb->data + ETH_ALEN * 2 - 2 - i * 2));
					skb_pull(skb, 4);
				}
			}

			res = skb_linearize(skb);
			if (res < 0)
				return -1;

			res = nat25_db_handle(padapter, skb, NAT25_INSERT);
			if (res < 0) {
				if (res == -2)
					return -1;

				return 0;
			}
		}

		memcpy(skb->data + ETH_ALEN, GET_MY_HWADDR(padapter), ETH_ALEN);

		dhcp_flag_bcast(padapter, skb);

		if (is_vlan_tag) {
			skb_push(skb, 4);
			for (i = 0; i < 6; i++)
				*((unsigned short *)(skb->data + i * 2)) = *((unsigned short *)(skb->data + 4 + i * 2));
			*((__be16 *)(skb->data + ETH_ALEN * 2)) = htons(ETH_P_8021Q);
			*((unsigned short *)(skb->data + ETH_ALEN * 2 + 2)) = vlan_hdr;
		}
	}

	/*  check if SA is equal to our MAC */
	if (memcmp(skb->data + ETH_ALEN, GET_MY_HWADDR(padapter), ETH_ALEN))
		return -1;

	return 0;
}

u32 rtw_get_ff_hwaddr(struct xmit_frame *pxmitframe)
{
	u32 addr;
	struct pkt_attrib *pattrib = &pxmitframe->attrib;

	switch (pattrib->qsel) {
	case 0:
	case 3:
		addr = BE_QUEUE_INX;
		break;
	case 1:
	case 2:
		addr = BK_QUEUE_INX;
		break;
	case 4:
	case 5:
		addr = VI_QUEUE_INX;
		break;
	case 6:
	case 7:
		addr = VO_QUEUE_INX;
		break;
	case 0x10:
		addr = BCN_QUEUE_INX;
		break;
	case 0x11:/* BC/MC in PS (HIQ) */
		addr = HIGH_QUEUE_INX;
		break;
	case 0x12:
	default:
		addr = MGT_QUEUE_INX;
		break;
	}

	return addr;
}

/*
 * The main transmit(tx) entry
 *
 * Return
 *	1	enqueue
 *	0	success, hardware will handle this xmit frame(packet)
 *	<0	fail
 */
s32 rtw_xmit(struct adapter *padapter, struct sk_buff **ppkt)
{
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;
	struct xmit_frame *pxmitframe = NULL;
	struct mlme_priv	*pmlmepriv = &padapter->mlmepriv;
	s32 res;

	pxmitframe = rtw_alloc_xmitframe(pxmitpriv);
	if (!pxmitframe)
		return -1;

	if (rcu_access_pointer(padapter->pnetdev->rx_handler_data) &&
	    check_fwstate(pmlmepriv, WIFI_STATION_STATE | WIFI_ADHOC_STATE)) {
		res = rtw_br_client_tx(padapter, ppkt);
		if (res == -1) {
			rtw_free_xmitframe(pxmitpriv, pxmitframe);
			return -1;
		}
	}

	res = update_attrib(padapter, *ppkt, &pxmitframe->attrib);

	if (res == _FAIL) {
		rtw_free_xmitframe(pxmitpriv, pxmitframe);
		return -1;
	}
	pxmitframe->pkt = *ppkt;

	rtw_led_control(padapter, LED_CTL_TX);

	pxmitframe->attrib.qsel = pxmitframe->attrib.priority;

	spin_lock_bh(&pxmitpriv->lock);
	if (xmitframe_enqueue_for_sleeping_sta(padapter, pxmitframe)) {
		spin_unlock_bh(&pxmitpriv->lock);
		return 1;
	}
	spin_unlock_bh(&pxmitpriv->lock);

	if (!rtl8188eu_hal_xmit(padapter, pxmitframe))
		return 1;

	return 0;
}

int xmitframe_enqueue_for_sleeping_sta(struct adapter *padapter, struct xmit_frame *pxmitframe)
{
	int ret = false;
	struct sta_info *psta = NULL;
	struct sta_priv *pstapriv = &padapter->stapriv;
	struct pkt_attrib *pattrib = &pxmitframe->attrib;
	struct mlme_priv *pmlmepriv = &padapter->mlmepriv;
	bool bmcst = is_multicast_ether_addr(pattrib->ra);

	if (!check_fwstate(pmlmepriv, WIFI_AP_STATE))
		return ret;

	if (pattrib->psta)
		psta = pattrib->psta;
	else
		psta = rtw_get_stainfo(pstapriv, pattrib->ra);

	if (!psta)
		return ret;

	if (pattrib->triggered == 1) {
		if (bmcst)
			pattrib->qsel = 0x11;/* HIQ */
		return ret;
	}

	if (bmcst) {
		spin_lock_bh(&psta->sleep_q.lock);

		if (pstapriv->sta_dz_bitmap) {/* if any one sta is in ps mode */
			list_del_init(&pxmitframe->list);

			list_add_tail(&pxmitframe->list, get_list_head(&psta->sleep_q));

			psta->sleepq_len++;

			pstapriv->tim_bitmap |= BIT(0);/*  */
			pstapriv->sta_dz_bitmap |= BIT(0);
			/* tx bc/mc packets after update bcn */
			update_beacon(padapter, _TIM_IE_, NULL, false);

			ret = true;
		}

		spin_unlock_bh(&psta->sleep_q.lock);

		return ret;
	}

	spin_lock_bh(&psta->sleep_q.lock);

	if (psta->state & WIFI_SLEEP_STATE) {
		u8 wmmps_ac = 0;

		if (pstapriv->sta_dz_bitmap & BIT(psta->aid)) {
			list_del_init(&pxmitframe->list);

			list_add_tail(&pxmitframe->list, get_list_head(&psta->sleep_q));

			psta->sleepq_len++;

			switch (pattrib->priority) {
			case 1:
			case 2:
				wmmps_ac = psta->uapsd_bk & BIT(0);
				break;
			case 4:
			case 5:
				wmmps_ac = psta->uapsd_vi & BIT(0);
				break;
			case 6:
			case 7:
				wmmps_ac = psta->uapsd_vo & BIT(0);
				break;
			case 0:
			case 3:
			default:
				wmmps_ac = psta->uapsd_be & BIT(0);
				break;
			}

			if (wmmps_ac)
				psta->sleepq_ac_len++;

			if (((psta->has_legacy_ac) && (!wmmps_ac)) ||
			    ((!psta->has_legacy_ac) && (wmmps_ac))) {
				pstapriv->tim_bitmap |= BIT(psta->aid);

				if (psta->sleepq_len == 1) {
					/* update BCN for TIM IE */
					update_beacon(padapter, _TIM_IE_, NULL, false);
				}
			}
			ret = true;
		}
	}

	spin_unlock_bh(&psta->sleep_q.lock);

	return ret;
}

static void dequeue_xmitframes_to_sleeping_queue(struct adapter *padapter, struct sta_info *psta, struct list_head *phead)
{
	struct list_head *plist;
	u8	ac_index;
	struct tx_servq	*ptxservq;
	struct pkt_attrib	*pattrib;
	struct xmit_frame	*pxmitframe;
	struct hw_xmit *phwxmits =  padapter->xmitpriv.hwxmits;

	plist = phead->next;

	while (phead != plist) {
		pxmitframe = container_of(plist, struct xmit_frame, list);

		plist = plist->next;

		xmitframe_enqueue_for_sleeping_sta(padapter, pxmitframe);

		pattrib = &pxmitframe->attrib;

		ptxservq = rtw_get_sta_pending(padapter, psta, pattrib->priority, (u8 *)(&ac_index));

		ptxservq->qcnt--;
		phwxmits[ac_index].accnt--;
	}
}

void stop_sta_xmit(struct adapter *padapter, struct sta_info *psta)
{
	struct sta_info *psta_bmc;
	struct sta_xmit_priv *pstaxmitpriv;
	struct sta_priv *pstapriv = &padapter->stapriv;
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;

	pstaxmitpriv = &psta->sta_xmitpriv;

	/* for BC/MC Frames */
	psta_bmc = rtw_get_bcmc_stainfo(padapter);

	spin_lock_bh(&pxmitpriv->lock);

	psta->state |= WIFI_SLEEP_STATE;

	pstapriv->sta_dz_bitmap |= BIT(psta->aid);

	dequeue_xmitframes_to_sleeping_queue(padapter, psta, &pstaxmitpriv->vo_q.sta_pending);
	list_del_init(&pstaxmitpriv->vo_q.tx_pending);

	dequeue_xmitframes_to_sleeping_queue(padapter, psta, &pstaxmitpriv->vi_q.sta_pending);
	list_del_init(&pstaxmitpriv->vi_q.tx_pending);

	dequeue_xmitframes_to_sleeping_queue(padapter, psta, &pstaxmitpriv->be_q.sta_pending);
	list_del_init(&pstaxmitpriv->be_q.tx_pending);

	dequeue_xmitframes_to_sleeping_queue(padapter, psta, &pstaxmitpriv->bk_q.sta_pending);
	list_del_init(&pstaxmitpriv->bk_q.tx_pending);

	/* for BC/MC Frames */
	pstaxmitpriv = &psta_bmc->sta_xmitpriv;
	dequeue_xmitframes_to_sleeping_queue(padapter, psta_bmc, &pstaxmitpriv->be_q.sta_pending);
	list_del_init(&pstaxmitpriv->be_q.tx_pending);

	spin_unlock_bh(&pxmitpriv->lock);
}

void wakeup_sta_to_xmit(struct adapter *padapter, struct sta_info *psta)
{
	u8 update_mask = 0, wmmps_ac = 0;
	struct sta_info *psta_bmc;
	struct list_head *xmitframe_plist, *xmitframe_phead;
	struct xmit_frame *pxmitframe = NULL;
	struct sta_priv *pstapriv = &padapter->stapriv;

	spin_lock_bh(&psta->sleep_q.lock);

	xmitframe_phead = get_list_head(&psta->sleep_q);
	xmitframe_plist = xmitframe_phead->next;

	while (xmitframe_phead != xmitframe_plist) {
		pxmitframe = container_of(xmitframe_plist, struct xmit_frame, list);

		xmitframe_plist = xmitframe_plist->next;

		list_del_init(&pxmitframe->list);

		switch (pxmitframe->attrib.priority) {
		case 1:
		case 2:
			wmmps_ac = psta->uapsd_bk & BIT(1);
			break;
		case 4:
		case 5:
			wmmps_ac = psta->uapsd_vi & BIT(1);
			break;
		case 6:
		case 7:
			wmmps_ac = psta->uapsd_vo & BIT(1);
			break;
		case 0:
		case 3:
		default:
			wmmps_ac = psta->uapsd_be & BIT(1);
			break;
		}

		psta->sleepq_len--;
		if (psta->sleepq_len > 0)
			pxmitframe->attrib.mdata = 1;
		else
			pxmitframe->attrib.mdata = 0;

		if (wmmps_ac) {
			psta->sleepq_ac_len--;
			if (psta->sleepq_ac_len > 0) {
				pxmitframe->attrib.mdata = 1;
				pxmitframe->attrib.eosp = 0;
			} else {
				pxmitframe->attrib.mdata = 0;
				pxmitframe->attrib.eosp = 1;
			}
		}

		pxmitframe->attrib.triggered = 1;

		spin_unlock_bh(&psta->sleep_q.lock);
		if (rtl8188eu_hal_xmit(padapter, pxmitframe))
			rtw_xmit_complete(padapter, pxmitframe);
		spin_lock_bh(&psta->sleep_q.lock);
	}

	if (psta->sleepq_len == 0) {
		pstapriv->tim_bitmap &= ~BIT(psta->aid);

		update_mask = BIT(0);

		if (psta->state & WIFI_SLEEP_STATE)
			psta->state ^= WIFI_SLEEP_STATE;

		if (psta->state & WIFI_STA_ALIVE_CHK_STATE) {
			psta->expire_to = pstapriv->expire_to;
			psta->state ^= WIFI_STA_ALIVE_CHK_STATE;
		}

		pstapriv->sta_dz_bitmap &= ~BIT(psta->aid);
	}

	spin_unlock_bh(&psta->sleep_q.lock);

	/* for BC/MC Frames */
	psta_bmc = rtw_get_bcmc_stainfo(padapter);
	if (!psta_bmc)
		return;

	if ((pstapriv->sta_dz_bitmap & 0xfffe) == 0x0) { /* no any sta in ps mode */
		spin_lock_bh(&psta_bmc->sleep_q.lock);

		xmitframe_phead = get_list_head(&psta_bmc->sleep_q);
		xmitframe_plist = xmitframe_phead->next;

		while (xmitframe_phead != xmitframe_plist) {
			pxmitframe = container_of(xmitframe_plist, struct xmit_frame, list);

			xmitframe_plist = xmitframe_plist->next;

			list_del_init(&pxmitframe->list);

			psta_bmc->sleepq_len--;
			if (psta_bmc->sleepq_len > 0)
				pxmitframe->attrib.mdata = 1;
			else
				pxmitframe->attrib.mdata = 0;

			pxmitframe->attrib.triggered = 1;

			spin_unlock_bh(&psta_bmc->sleep_q.lock);
			if (rtl8188eu_hal_xmit(padapter, pxmitframe))
				rtw_xmit_complete(padapter, pxmitframe);
			spin_lock_bh(&psta_bmc->sleep_q.lock);
		}

		if (psta_bmc->sleepq_len == 0) {
			pstapriv->tim_bitmap &= ~BIT(0);
			pstapriv->sta_dz_bitmap &= ~BIT(0);

			update_mask |= BIT(1);
		}

		spin_unlock_bh(&psta_bmc->sleep_q.lock);
	}

	if (update_mask)
		update_beacon(padapter, _TIM_IE_, NULL, false);
}

void xmit_delivery_enabled_frames(struct adapter *padapter, struct sta_info *psta)
{
	u8 wmmps_ac = 0;
	struct list_head *xmitframe_plist, *xmitframe_phead;
	struct xmit_frame *pxmitframe = NULL;
	struct sta_priv *pstapriv = &padapter->stapriv;

	spin_lock_bh(&psta->sleep_q.lock);

	xmitframe_phead = get_list_head(&psta->sleep_q);
	xmitframe_plist = xmitframe_phead->next;

	while (xmitframe_phead != xmitframe_plist) {
		pxmitframe = container_of(xmitframe_plist, struct xmit_frame, list);

		xmitframe_plist = xmitframe_plist->next;

		switch (pxmitframe->attrib.priority) {
		case 1:
		case 2:
			wmmps_ac = psta->uapsd_bk & BIT(1);
			break;
		case 4:
		case 5:
			wmmps_ac = psta->uapsd_vi & BIT(1);
			break;
		case 6:
		case 7:
			wmmps_ac = psta->uapsd_vo & BIT(1);
			break;
		case 0:
		case 3:
		default:
			wmmps_ac = psta->uapsd_be & BIT(1);
			break;
		}

		if (!wmmps_ac)
			continue;

		list_del_init(&pxmitframe->list);

		psta->sleepq_len--;
		psta->sleepq_ac_len--;

		if (psta->sleepq_ac_len > 0) {
			pxmitframe->attrib.mdata = 1;
			pxmitframe->attrib.eosp = 0;
		} else {
			pxmitframe->attrib.mdata = 0;
			pxmitframe->attrib.eosp = 1;
		}

		pxmitframe->attrib.triggered = 1;

		if (rtl8188eu_hal_xmit(padapter, pxmitframe))
			rtw_xmit_complete(padapter, pxmitframe);

		if ((psta->sleepq_ac_len == 0) && (!psta->has_legacy_ac) && (wmmps_ac)) {
			pstapriv->tim_bitmap &= ~BIT(psta->aid);

			/* update BCN for TIM IE */
			update_beacon(padapter, _TIM_IE_, NULL, false);
		}
	}

	spin_unlock_bh(&psta->sleep_q.lock);
}

void rtw_sctx_init(struct submit_ctx *sctx, int timeout_ms)
{
	sctx->timeout_ms = timeout_ms;
	sctx->submit_time = jiffies;
	init_completion(&sctx->done);
	sctx->status = RTW_SCTX_SUBMITTED;
}

int rtw_sctx_wait(struct submit_ctx *sctx)
{
	int ret = _FAIL;
	unsigned long expire;
	int status = 0;

	expire = sctx->timeout_ms ? msecs_to_jiffies(sctx->timeout_ms) : MAX_SCHEDULE_TIMEOUT;
	if (!wait_for_completion_timeout(&sctx->done, expire))
		/* timeout, do something?? */
		status = RTW_SCTX_DONE_TIMEOUT;
	else
		status = sctx->status;

	if (status == RTW_SCTX_DONE_SUCCESS)
		ret = _SUCCESS;

	return ret;
}

void rtw_sctx_done_err(struct submit_ctx **sctx, int status)
{
	if (*sctx) {
		(*sctx)->status = status;
		complete(&((*sctx)->done));
		*sctx = NULL;
	}
}

int rtw_ack_tx_wait(struct xmit_priv *pxmitpriv, u32 timeout_ms)
{
	struct submit_ctx *pack_tx_ops = &pxmitpriv->ack_tx_ops;

	pack_tx_ops->submit_time = jiffies;
	pack_tx_ops->timeout_ms = timeout_ms;
	pack_tx_ops->status = RTW_SCTX_SUBMITTED;

	return rtw_sctx_wait(pack_tx_ops);
}

void rtw_ack_tx_done(struct xmit_priv *pxmitpriv, int status)
{
	struct submit_ctx *pack_tx_ops = &pxmitpriv->ack_tx_ops;

	if (pxmitpriv->ack_tx)
		rtw_sctx_done_err(&pack_tx_ops, status);
}

static void rtw_check_xmit_resource(struct adapter *padapter, struct sk_buff *pkt)
{
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;
	u16 queue;

	queue = skb_get_queue_mapping(pkt);
	if (padapter->registrypriv.wifi_spec) {
		/* No free space for Tx, tx_worker is too slow */
		if (pxmitpriv->hwxmits[queue].accnt > WMM_XMIT_THRESHOLD)
			netif_stop_subqueue(padapter->pnetdev, queue);
	} else {
		if (pxmitpriv->free_xmitframe_cnt <= 4) {
			if (!netif_tx_queue_stopped(netdev_get_tx_queue(padapter->pnetdev, queue)))
				netif_stop_subqueue(padapter->pnetdev, queue);
		}
	}
}

static int rtw_mlcst2unicst(struct adapter *padapter, struct sk_buff *skb)
{
	struct	sta_priv *pstapriv = &padapter->stapriv;
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;
	struct list_head *phead, *plist;
	struct sk_buff *newskb;
	struct sta_info *psta = NULL;
	s32 res;

	spin_lock_bh(&pstapriv->asoc_list_lock);
	phead = &pstapriv->asoc_list;
	plist = phead->next;

	/* free sta asoc_queue */
	while (phead != plist) {
		psta = container_of(plist, struct sta_info, asoc_list);

		plist = plist->next;

		/* avoid   come from STA1 and send back STA1 */
		if (!memcmp(psta->hwaddr, &skb->data[6], 6))
			continue;

		newskb = skb_copy(skb, GFP_ATOMIC);

		if (newskb) {
			memcpy(newskb->data, psta->hwaddr, 6);
			res = rtw_xmit(padapter, &newskb);
			if (res < 0) {
				pxmitpriv->tx_drop++;
				dev_kfree_skb_any(newskb);
			} else {
				pxmitpriv->tx_pkts++;
			}
		} else {
			pxmitpriv->tx_drop++;

			spin_unlock_bh(&pstapriv->asoc_list_lock);
			return false;	/*  Caller shall tx this multicast frame via normal way. */
		}
	}

	spin_unlock_bh(&pstapriv->asoc_list_lock);
	dev_kfree_skb_any(skb);
	return true;
}

netdev_tx_t rtw_xmit_entry(struct sk_buff *pkt, struct net_device *pnetdev)
{
	struct adapter *padapter = (struct adapter *)rtw_netdev_priv(pnetdev);
	struct xmit_priv *pxmitpriv = &padapter->xmitpriv;
	struct mlme_priv *pmlmepriv = &padapter->mlmepriv;
	s32 res = 0;

	if (!rtw_if_up(padapter))
		goto drop_packet;

	rtw_check_xmit_resource(padapter, pkt);

	if (!rtw_mc2u_disable && check_fwstate(pmlmepriv, WIFI_AP_STATE) &&
	    (IP_MCAST_MAC(pkt->data) || ICMPV6_MCAST_MAC(pkt->data)) &&
	    (padapter->registrypriv.wifi_spec == 0)) {
		if (pxmitpriv->free_xmitframe_cnt > (NR_XMITFRAME / 4)) {
			res = rtw_mlcst2unicst(padapter, pkt);
			if (res)
				goto exit;
		}
	}

	res = rtw_xmit(padapter, &pkt);
	if (res < 0)
		goto drop_packet;

	pxmitpriv->tx_pkts++;
	goto exit;

drop_packet:
	pxmitpriv->tx_drop++;
	dev_kfree_skb_any(pkt);

exit:
	return NETDEV_TX_OK;
}
