/******************************************************************************
 *
 * Copyright(c) 2007 - 2012 Realtek Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 *
 ******************************************************************************/
#define _RTW_RECV_C_
#include <osdep_service.h>
#include <drv_types.h>
#include <recv_osdep.h>
#include <mlme_osdep.h>
#include <linux/ip.h>
#include <linux/if_ether.h>
#include <ethernet.h>
#include <usb_ops.h>
#include <linux/ieee80211.h>
#include <wifi.h>
#include <circ_buf.h>

static u8 SNAP_ETH_TYPE_IPX[2] = {0x81, 0x37};

static u8 SNAP_ETH_TYPE_APPLETALK_AARP[2] = {0x80, 0xf3};
static u8 SNAP_ETH_TYPE_APPLETALK_DDP[2] = {0x80, 0x9b};
static u8 SNAP_ETH_TYPE_TDLS[2] = {0x89, 0x0d};

static u8 rtw_rfc1042_header[] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
/* Bridge-Tunnel header (for EtherTypes ETH_P_AARP and ETH_P_IPX) */
static u8 rtw_bridge_tunnel_header[] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8 };

void rtw_signal_stat_timer_hdl(unsigned long data);

void _rtw_init_sta_recv_priv(struct sta_recv_priv *psta_recvpriv)
{

_func_enter_;

	memset((u8 *)psta_recvpriv, 0, sizeof (struct sta_recv_priv));

	spin_lock_init(&psta_recvpriv->lock);

	/* for(i=0; i<MAX_RX_NUMBLKS; i++) */
	/*	_rtw_init_queue(&psta_recvpriv->blk_strms[i]); */

	_rtw_init_queue(&psta_recvpriv->defrag_q);

_func_exit_;
}

int _rtw_init_recv_priv(struct recv_priv *precvpriv, struct rtw_adapter *padapter)
{
	union recv_frame *precvframe;
	int i;
	int res=_SUCCESS;

_func_enter_;

	/*  We don't need to memset padapter->XXX to zero, because adapter is allocated by rtw_zvmalloc(). */
	/* memset((unsigned char *)precvpriv, 0, sizeof (struct  recv_priv)); */

	spin_lock_init(&precvpriv->lock);

	_rtw_init_queue(&precvpriv->free_recv_queue);
	_rtw_init_queue(&precvpriv->recv_pending_queue);
	_rtw_init_queue(&precvpriv->uc_swdec_pending_queue);

	precvpriv->adapter = padapter;

	precvpriv->free_recvframe_cnt = NR_RECVFRAME;

	rtw_os_recv_resource_init(precvpriv, padapter);

	precvpriv->pallocated_frame_buf = rtw_zvmalloc(NR_RECVFRAME * sizeof(union recv_frame) + RXFRAME_ALIGN_SZ);

	if(precvpriv->pallocated_frame_buf==NULL){
		res= _FAIL;
		goto exit;
	}
	/* memset(precvpriv->pallocated_frame_buf, 0, NR_RECVFRAME * sizeof(union recv_frame) + RXFRAME_ALIGN_SZ); */

	precvpriv->precv_frame_buf = PTR_ALIGN(precvpriv->pallocated_frame_buf, RXFRAME_ALIGN_SZ);

	precvframe = (union recv_frame*) precvpriv->precv_frame_buf;

	for(i=0; i < NR_RECVFRAME ; i++)
	{
		INIT_LIST_HEAD(&(precvframe->u.list));

		list_add_tail(&(precvframe->u.list), &(precvpriv->free_recv_queue.queue));

		res = rtw_os_recv_resource_alloc(padapter, precvframe);

		precvframe->u.hdr.len = 0;

		precvframe->u.hdr.adapter =padapter;
		precvframe++;

	}

	precvpriv->rx_pending_cnt=1;

	sema_init(&precvpriv->allrxreturnevt, 0);

	res = rtw_hal_init_recv_priv(padapter);

	setup_timer(&precvpriv->signal_stat_timer, rtw_signal_stat_timer_hdl,
		    (unsigned long)padapter);

	precvpriv->signal_stat_sampling_interval = 1000; /* ms */

	rtw_set_signal_stat_timer(precvpriv);

exit:

_func_exit_;

	return res;
}

void _rtw_free_recv_priv (struct recv_priv *precvpriv)
{
	struct rtw_adapter *padapter = precvpriv->adapter;

_func_enter_;

	rtw_free_uc_swdec_pending_queue(padapter);

	rtw_os_recv_resource_free(precvpriv);

	if(precvpriv->pallocated_frame_buf) {
		rtw_vmfree(precvpriv->pallocated_frame_buf, NR_RECVFRAME * sizeof(union recv_frame) + RXFRAME_ALIGN_SZ);
	}

	rtw_hal_free_recv_priv(padapter);

_func_exit_;
}

union recv_frame *_rtw_alloc_recvframe (_queue *pfree_recv_queue)
{
	struct recv_frame_hdr *hdr;
	struct list_head	*plist, *phead;
	struct rtw_adapter *padapter;
	struct recv_priv *precvpriv;
_func_enter_;

	if(_rtw_queue_empty(pfree_recv_queue) == true)
	{
		hdr = NULL;
	}
	else
	{
		phead = get_list_head(pfree_recv_queue);

		plist = phead->next;

		hdr = container_of(plist, struct recv_frame_hdr, list);

		list_del_init(&hdr->list);
		padapter=hdr->adapter;
		if(padapter !=NULL){
			precvpriv=&padapter->recvpriv;
			if(pfree_recv_queue == &precvpriv->free_recv_queue)
				precvpriv->free_recvframe_cnt--;
		}
	}

_func_exit_;
	return (union recv_frame *)hdr;
}

union recv_frame *rtw_alloc_recvframe (_queue *pfree_recv_queue)
{
	union recv_frame  *precvframe;

	spin_lock_bh(&pfree_recv_queue->lock);

	precvframe = _rtw_alloc_recvframe(pfree_recv_queue);

	spin_unlock_bh(&pfree_recv_queue->lock);

	return precvframe;
}

void rtw_init_recvframe(union recv_frame *precvframe, struct recv_priv *precvpriv)
{
	/* Perry: This can be removed */
	INIT_LIST_HEAD(&precvframe->u.hdr.list);

	precvframe->u.hdr.len=0;
}

int rtw_free_recvframe(union recv_frame *precvframe, _queue *pfree_recv_queue)
{
	struct rtw_adapter *padapter=precvframe->u.hdr.adapter;
	struct recv_priv *precvpriv = &padapter->recvpriv;

_func_enter_;

	if(precvframe->u.hdr.pkt)
	{
		dev_kfree_skb_any(precvframe->u.hdr.pkt);/* free skb by driver */
		precvframe->u.hdr.pkt = NULL;
	}

	spin_lock_bh(&pfree_recv_queue->lock);

	list_del_init(&(precvframe->u.hdr.list));

	precvframe->u.hdr.len = 0;

	list_add_tail(&(precvframe->u.hdr.list), get_list_head(pfree_recv_queue));

	if(padapter !=NULL){
		if(pfree_recv_queue == &precvpriv->free_recv_queue)
				precvpriv->free_recvframe_cnt++;
	}

      spin_unlock_bh(&pfree_recv_queue->lock);

_func_exit_;

	return _SUCCESS;
}

int _rtw_enqueue_recvframe(union recv_frame *precvframe, _queue *queue)
{

	struct rtw_adapter *padapter=precvframe->u.hdr.adapter;
	struct recv_priv *precvpriv = &padapter->recvpriv;

_func_enter_;

	/* INIT_LIST_HEAD(&(precvframe->u.hdr.list)); */
	list_del_init(&(precvframe->u.hdr.list));

	list_add_tail(&(precvframe->u.hdr.list), get_list_head(queue));

	if (padapter != NULL) {
		if (queue == &precvpriv->free_recv_queue)
			precvpriv->free_recvframe_cnt++;
	}

_func_exit_;

	return _SUCCESS;
}

int rtw_enqueue_recvframe(union recv_frame *precvframe, _queue *queue)
{
	int ret;

	/* _spinlock(&pfree_recv_queue->lock); */
	spin_lock_bh(&queue->lock);
	ret = _rtw_enqueue_recvframe(precvframe, queue);
	/* spin_unlock(&pfree_recv_queue->lock); */
	spin_unlock_bh(&queue->lock);

	return ret;
}

/*
int	rtw_enqueue_recvframe(union recv_frame *precvframe, _queue *queue)
{
	return rtw_free_recvframe(precvframe, queue);
}
*/

/*
caller : defrag ; recvframe_chk_defrag in recv_thread  (passive)
pframequeue: defrag_queue : will be accessed in recv_thread  (passive)

using spinlock to protect

*/

void rtw_free_recvframe_queue(_queue *pframequeue,  _queue *pfree_recv_queue)
{
	struct recv_frame_hdr *hdr;
	struct list_head *plist, *phead, *ptmp;

_func_enter_;
	spin_lock(&pframequeue->lock);

	phead = get_list_head(pframequeue);
	plist = phead->next;

	list_for_each_safe(plist, ptmp, phead) {
		hdr = container_of(plist, struct recv_frame_hdr, list);
		rtw_free_recvframe((union recv_frame *)hdr, pfree_recv_queue);
	}

	spin_unlock(&pframequeue->lock);

_func_exit_;
}

u32 rtw_free_uc_swdec_pending_queue(struct rtw_adapter *adapter)
{
	u32 cnt = 0;
	union recv_frame *pending_frame;
	while((pending_frame=rtw_alloc_recvframe(&adapter->recvpriv.uc_swdec_pending_queue))) {
		rtw_free_recvframe(pending_frame, &adapter->recvpriv.free_recv_queue);
		DBG_8723A("%s: dequeue uc_swdec_pending_queue\n", __func__);
		cnt++;
	}

	return cnt;
}

int rtw_enqueue_recvbuf_to_head(struct recv_buf *precvbuf, _queue *queue)
{
	spin_lock_bh(&queue->lock);

	list_del_init(&precvbuf->list);
	list_add(&precvbuf->list, get_list_head(queue));

	spin_unlock_bh(&queue->lock);

	return _SUCCESS;
}

int rtw_enqueue_recvbuf(struct recv_buf *precvbuf, _queue *queue)
{
	unsigned long irqL;
	spin_lock_irqsave(&queue->lock, irqL);

	list_del_init(&precvbuf->list);

	list_add_tail(&precvbuf->list, get_list_head(queue));
	spin_unlock_irqrestore(&queue->lock, irqL);
	return _SUCCESS;
}

struct recv_buf *rtw_dequeue_recvbuf (_queue *queue)
{
	unsigned long irqL;
	struct recv_buf *precvbuf;
	struct list_head	*plist, *phead;

	spin_lock_irqsave(&queue->lock, irqL);

	if(_rtw_queue_empty(queue) == true)
	{
		precvbuf = NULL;
	}
	else
	{
		phead = get_list_head(queue);

		plist = phead->next;

		precvbuf = container_of(plist, struct recv_buf, list);

		list_del_init(&precvbuf->list);

	}

	spin_unlock_irqrestore(&queue->lock, irqL);

	return precvbuf;
}

int recvframe_chkmic(struct rtw_adapter *adapter,  union recv_frame *precvframe);
int recvframe_chkmic(struct rtw_adapter *adapter,  union recv_frame *precvframe){

	int	i,res=_SUCCESS;
	u32	datalen;
	u8	miccode[8];
	u8	bmic_err=false,brpt_micerror = true;
	u8	*pframe, *payload,*pframemic;
	u8	*mickey;
	/* u8	*iv,rxdata_key_idx=0; */
	struct	sta_info		*stainfo;
	struct	rx_pkt_attrib	*prxattrib=&precvframe->u.hdr.attrib;
	struct	security_priv	*psecuritypriv=&adapter->securitypriv;

	struct mlme_ext_priv	*pmlmeext = &adapter->mlmeextpriv;
	struct mlme_ext_info	*pmlmeinfo = &(pmlmeext->mlmext_info);
_func_enter_;

	stainfo=rtw_get_stainfo(&adapter->stapriv ,&prxattrib->ta[0]);

	if(prxattrib->encrypt ==_TKIP_)
	{
		RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("\n recvframe_chkmic:prxattrib->encrypt ==_TKIP_\n"));
		RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("\n recvframe_chkmic:da=0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x\n",
			prxattrib->ra[0],prxattrib->ra[1],prxattrib->ra[2],prxattrib->ra[3],prxattrib->ra[4],prxattrib->ra[5]));

		/* calculate mic code */
		if (stainfo!= NULL) {
			if(is_multicast_ether_addr(prxattrib->ra)) {
				mickey=&psecuritypriv->dot118021XGrprxmickey[prxattrib->key_index].skey[0];

				RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("\n recvframe_chkmic: bcmc key \n"));

				if(psecuritypriv->binstallGrpkey==false)
				{
					res=_FAIL;
					RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("\n recvframe_chkmic:didn't install group key!!!!!!!!!!\n"));
					DBG_8723A("\n recvframe_chkmic:didn't install group key!!!!!!!!!!\n");
					goto exit;
				}
			}
			else{
				mickey=&stainfo->dot11tkiprxmickey.skey[0];
				RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("\n recvframe_chkmic: unicast key \n"));
			}

			datalen=precvframe->u.hdr.len-prxattrib->hdrlen-prxattrib->iv_len-prxattrib->icv_len-8;/* icv_len included the mic code */
			pframe=precvframe->u.hdr.rx_data;
			payload=pframe+prxattrib->hdrlen+prxattrib->iv_len;

			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("\n prxattrib->iv_len=%d prxattrib->icv_len=%d\n",prxattrib->iv_len,prxattrib->icv_len));

			rtw_seccalctkipmic(mickey,pframe,payload, datalen ,&miccode[0],(unsigned char)prxattrib->priority); /* care the length of the data */

			pframemic=payload+datalen;

			bmic_err=false;

			for(i=0;i<8;i++){
				if(miccode[i] != *(pframemic+i)){
					RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("recvframe_chkmic:miccode[%d](%02x) != *(pframemic+%d)(%02x) ",i,miccode[i],i,*(pframemic+i)));
					bmic_err=true;
				}
			}

			if(bmic_err==true){

				RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("\n *(pframemic-8)-*(pframemic-1)=0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x\n",
					*(pframemic-8),*(pframemic-7),*(pframemic-6),*(pframemic-5),*(pframemic-4),*(pframemic-3),*(pframemic-2),*(pframemic-1)));
				RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("\n *(pframemic-16)-*(pframemic-9)=0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x\n",
					*(pframemic-16),*(pframemic-15),*(pframemic-14),*(pframemic-13),*(pframemic-12),*(pframemic-11),*(pframemic-10),*(pframemic-9)));

				{
					uint i;
					RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("\n ======demp packet (len=%d)======\n",precvframe->u.hdr.len));
					for(i=0;i<precvframe->u.hdr.len;i=i+8){
						RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x",
							*(precvframe->u.hdr.rx_data+i),*(precvframe->u.hdr.rx_data+i+1),
							*(precvframe->u.hdr.rx_data+i+2),*(precvframe->u.hdr.rx_data+i+3),
							*(precvframe->u.hdr.rx_data+i+4),*(precvframe->u.hdr.rx_data+i+5),
							*(precvframe->u.hdr.rx_data+i+6),*(precvframe->u.hdr.rx_data+i+7)));
					}
					RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("\n ======demp packet end [len=%d]======\n",precvframe->u.hdr.len));
					RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("\n hrdlen=%d, \n",prxattrib->hdrlen));
				}

				RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("ra=0x%.2x 0x%.2x 0x%.2x 0x%.2x 0x%.2x 0x%.2x psecuritypriv->binstallGrpkey=%d ",
					prxattrib->ra[0],prxattrib->ra[1],prxattrib->ra[2],
					prxattrib->ra[3],prxattrib->ra[4],prxattrib->ra[5],psecuritypriv->binstallGrpkey));

				/*  double check key_index for some timing issue , */
				/*  cannot compare with psecuritypriv->dot118021XGrpKeyid also cause timing issue */
				if ((is_multicast_ether_addr(prxattrib->ra)) &&
				    (prxattrib->key_index != pmlmeinfo->key_index ))
					brpt_micerror = false;

				if((prxattrib->bdecrypted ==true)&& (brpt_micerror == true))
				{
					rtw_handle_tkip_mic_err(adapter,(u8)is_multicast_ether_addr(prxattrib->ra));
					RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,(" mic error :prxattrib->bdecrypted=%d ",prxattrib->bdecrypted));
					DBG_8723A(" mic error :prxattrib->bdecrypted=%d\n",prxattrib->bdecrypted);
				}
				else
				{
					RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,(" mic error :prxattrib->bdecrypted=%d ",prxattrib->bdecrypted));
					DBG_8723A(" mic error :prxattrib->bdecrypted=%d\n",prxattrib->bdecrypted);
				}

				res=_FAIL;

			}
			else{
				/* mic checked ok */
				if((psecuritypriv->bcheck_grpkey ==false)&&(is_multicast_ether_addr(prxattrib->ra))){
					psecuritypriv->bcheck_grpkey =true;
					RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("psecuritypriv->bcheck_grpkey =true"));
				}
			}

		}
		else
		{
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("recvframe_chkmic: rtw_get_stainfo==NULL!!!\n"));
		}

		recvframe_pull_tail(precvframe, 8);

	}

exit:

_func_exit_;

	return res;
}

/* decrypt and set the ivlen,icvlen of the recv_frame */
union recv_frame * decryptor(struct rtw_adapter *padapter,union recv_frame *precv_frame);
union recv_frame * decryptor(struct rtw_adapter *padapter,union recv_frame *precv_frame)
{

	struct rx_pkt_attrib *prxattrib = &precv_frame->u.hdr.attrib;
	struct security_priv *psecuritypriv=&padapter->securitypriv;
	union recv_frame *return_packet=precv_frame;
	u32	 res=_SUCCESS;
_func_enter_;

	RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("prxstat->decrypted=%x prxattrib->encrypt = 0x%03x\n",prxattrib->bdecrypted,prxattrib->encrypt));

	if(prxattrib->encrypt>0)
	{
		u8 *iv = precv_frame->u.hdr.rx_data+prxattrib->hdrlen;
		prxattrib->key_index = ( ((iv[3])>>6)&0x3) ;

		if(prxattrib->key_index > WEP_KEYS)
		{
			DBG_8723A("prxattrib->key_index(%d) > WEP_KEYS \n", prxattrib->key_index);

			switch(prxattrib->encrypt){
				case _WEP40_:
				case _WEP104_:
					prxattrib->key_index = psecuritypriv->dot11PrivacyKeyIndex;
					break;
				case _TKIP_:
				case _AES_:
				default:
					prxattrib->key_index = psecuritypriv->dot118021XGrpKeyid;
					break;
			}
		}
	}

	if((prxattrib->encrypt>0) && ((prxattrib->bdecrypted==0) ||(psecuritypriv->sw_decrypt==true)))
	{
		psecuritypriv->hw_decrypted=false;

		#ifdef DBG_RX_DECRYPTOR
		DBG_8723A("prxstat->bdecrypted:%d,  prxattrib->encrypt:%d,  Setting psecuritypriv->hw_decrypted = %d\n"
			, prxattrib->bdecrypted ,prxattrib->encrypt, psecuritypriv->hw_decrypted);
		#endif

		switch(prxattrib->encrypt){
		case _WEP40_:
		case _WEP104_:
			rtw_wep_decrypt(padapter, (u8 *)precv_frame);
			break;
		case _TKIP_:
			res = rtw_tkip_decrypt(padapter, (u8 *)precv_frame);
			break;
		case _AES_:
			res = rtw_aes_decrypt(padapter, (u8 * )precv_frame);
			break;
		default:
				break;
		}
	}
	else if(prxattrib->bdecrypted==1
		&& prxattrib->encrypt >0
		&& (psecuritypriv->busetkipkey==1 || prxattrib->encrypt !=_TKIP_ )
		)
	{
		{
			psecuritypriv->hw_decrypted=true;
			#ifdef DBG_RX_DECRYPTOR
			DBG_8723A("prxstat->bdecrypted:%d,  prxattrib->encrypt:%d,  Setting psecuritypriv->hw_decrypted = %d\n"
			, prxattrib->bdecrypted ,prxattrib->encrypt, psecuritypriv->hw_decrypted);
			#endif

		}
	}
	else {
		#ifdef DBG_RX_DECRYPTOR
		DBG_8723A("prxstat->bdecrypted:%d,  prxattrib->encrypt:%d,  psecuritypriv->hw_decrypted:%d\n"
		, prxattrib->bdecrypted ,prxattrib->encrypt, psecuritypriv->hw_decrypted);
		#endif
	}

	if(res == _FAIL)
	{
		rtw_free_recvframe(return_packet,&padapter->recvpriv.free_recv_queue);
		return_packet = NULL;

	}

_func_exit_;

	return return_packet;
}

/* set the security information in the recv_frame */
static union recv_frame *portctrl(struct rtw_adapter *adapter,union recv_frame *precv_frame)
{
	u8   *psta_addr, *ptr;
	uint  auth_alg;
	struct recv_frame_hdr *pfhdr;
	struct sta_info *psta;
	struct sta_priv *pstapriv ;
	union recv_frame *prtnframe;
	u16	ether_type=0;
	u16  eapol_type = 0x888e;/* for Funia BD's WPA issue */
	struct rx_pkt_attrib *pattrib;

_func_enter_;

	pstapriv = &adapter->stapriv;
	psta = rtw_get_stainfo(pstapriv, psta_addr);

	auth_alg = adapter->securitypriv.dot11AuthAlgrthm;

	ptr = get_recvframe_data(precv_frame);
	pfhdr = &precv_frame->u.hdr;
	pattrib = &pfhdr->attrib;
	psta_addr = pattrib->ta;

	prtnframe = NULL;

	RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("########portctrl:adapter->securitypriv.dot11AuthAlgrthm=%d\n",adapter->securitypriv.dot11AuthAlgrthm));

	if(auth_alg==2)
	{
		if ((psta!=NULL) && (psta->ieee8021x_blocked))
		{
			/* blocked */
			/* only accept EAPOL frame */
			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("########portctrl:psta->ieee8021x_blocked==1\n"));

			prtnframe=precv_frame;

			/* get ether_type */
			ptr=ptr+pfhdr->attrib.hdrlen+pfhdr->attrib.iv_len+LLC_HEADER_SIZE;
			memcpy(&ether_type,ptr, 2);
			ether_type= ntohs((unsigned short )ether_type);

		        if (ether_type == eapol_type) {
				prtnframe=precv_frame;
			}
			else {
				/* free this frame */
				rtw_free_recvframe(precv_frame, &adapter->recvpriv.free_recv_queue);
				prtnframe=NULL;
			}
		}
		else
		{
			/* allowed */
			/* check decryption status, and decrypt the frame if needed */
			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("########portctrl:psta->ieee8021x_blocked==0\n"));
			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("portctrl:precv_frame->hdr.attrib.privacy=%x\n",precv_frame->u.hdr.attrib.privacy));

			if (pattrib->bdecrypted == 0)
			{
				RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("portctrl:prxstat->decrypted=%x\n", pattrib->bdecrypted));
			}

			prtnframe=precv_frame;
			/* check is the EAPOL frame or not (Rekey) */
			if(ether_type == eapol_type){

				RT_TRACE(_module_rtl871x_recv_c_,_drv_notice_,("########portctrl:ether_type == 0x888e\n"));
				/* check Rekey */

				prtnframe=precv_frame;
			}
			else{
				RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("########portctrl:ether_type=0x%04x\n", ether_type));
			}
		}
	}
	else
	{
		prtnframe=precv_frame;
	}

_func_exit_;

		return prtnframe;
}

int recv_decache(union recv_frame *precv_frame, u8 bretry, struct stainfo_rxcache *prxcache);
int recv_decache(union recv_frame *precv_frame, u8 bretry, struct stainfo_rxcache *prxcache)
{
	int tid = precv_frame->u.hdr.attrib.priority;

	u16 seq_ctrl = ( (precv_frame->u.hdr.attrib.seq_num&0xffff) << 4) |
		(precv_frame->u.hdr.attrib.frag_num & 0xf);

_func_enter_;

	if(tid>15)
	{
		RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_, ("recv_decache, (tid>15)! seq_ctrl=0x%x, tid=0x%x\n", seq_ctrl, tid));

		return _FAIL;
	}

	if(1)/* if(bretry) */
	{
		if(seq_ctrl == prxcache->tid_rxseq[tid])
		{
			RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_, ("recv_decache, seq_ctrl=0x%x, tid=0x%x, tid_rxseq=0x%x\n", seq_ctrl, tid, prxcache->tid_rxseq[tid]));

			return _FAIL;
		}
	}

	prxcache->tid_rxseq[tid] = seq_ctrl;

_func_exit_;

	return _SUCCESS;
}

void process_pwrbit_data(struct rtw_adapter *padapter, union recv_frame *precv_frame);
void process_pwrbit_data(struct rtw_adapter *padapter, union recv_frame *precv_frame)
{
#ifdef CONFIG_8723AU_AP_MODE
	unsigned char pwrbit;
	u8 *ptr = precv_frame->u.hdr.rx_data;
	struct rx_pkt_attrib *pattrib = &precv_frame->u.hdr.attrib;
	struct sta_priv *pstapriv = &padapter->stapriv;
	struct sta_info *psta=NULL;

	psta = rtw_get_stainfo(pstapriv, pattrib->src);

	pwrbit = GetPwrMgt(ptr);

	if(psta)
	{
		if(pwrbit)
		{
			if(!(psta->state & WIFI_SLEEP_STATE))
			{
				/* psta->state |= WIFI_SLEEP_STATE; */
				/* pstapriv->sta_dz_bitmap |= BIT(psta->aid); */

				stop_sta_xmit(padapter, psta);

				/* DBG_8723A("to sleep, sta_dz_bitmap=%x\n", pstapriv->sta_dz_bitmap); */
			}
		}
		else
		{
			if(psta->state & WIFI_SLEEP_STATE)
			{
				/* psta->state ^= WIFI_SLEEP_STATE; */
				/* pstapriv->sta_dz_bitmap &= ~BIT(psta->aid); */

				wakeup_sta_to_xmit(padapter, psta);

				/* DBG_8723A("to wakeup, sta_dz_bitmap=%x\n", pstapriv->sta_dz_bitmap); */
			}
		}

	}

#endif
}

void process_wmmps_data(struct rtw_adapter *padapter, union recv_frame *precv_frame);
void process_wmmps_data(struct rtw_adapter *padapter, union recv_frame *precv_frame)
{
#ifdef CONFIG_8723AU_AP_MODE
	struct rx_pkt_attrib *pattrib = &precv_frame->u.hdr.attrib;
	struct sta_priv *pstapriv = &padapter->stapriv;
	struct sta_info *psta=NULL;

	psta = rtw_get_stainfo(pstapriv, pattrib->src);

	if(!psta) return;


	if(!psta->qos_option)
		return;

	if(!(psta->qos_info&0xf))
		return;

	if(psta->state&WIFI_SLEEP_STATE)
	{
		u8 wmmps_ac=0;

		switch(pattrib->priority)
		{
			case 1:
			case 2:
				wmmps_ac = psta->uapsd_bk&BIT(1);
				break;
			case 4:
			case 5:
				wmmps_ac = psta->uapsd_vi&BIT(1);
				break;
			case 6:
			case 7:
				wmmps_ac = psta->uapsd_vo&BIT(1);
				break;
			case 0:
			case 3:
			default:
				wmmps_ac = psta->uapsd_be&BIT(1);
				break;
		}

		if(wmmps_ac)
		{
			if(psta->sleepq_ac_len>0)
			{
				/* process received triggered frame */
				xmit_delivery_enabled_frames(padapter, psta);
			}
			else
			{
				/* issue one qos null frame with More data bit = 0 and the EOSP bit set (=1) */
				issue_qos_nulldata(padapter, psta->hwaddr, (u16)pattrib->priority, 0, 0);
			}
		}

	}

#endif
}

void count_rx_stats(struct rtw_adapter *padapter, union recv_frame *prframe, struct sta_info*sta)
{
	int	sz;
	struct sta_info		*psta = NULL;
	struct stainfo_stats	*pstats = NULL;
	struct rx_pkt_attrib	*pattrib = & prframe->u.hdr.attrib;
	struct recv_priv		*precvpriv = &padapter->recvpriv;

	sz = get_recvframe_len(prframe);
	precvpriv->rx_bytes += sz;

	padapter->mlmepriv.LinkDetectInfo.NumRxOkInPeriod++;

	if ((!is_broadcast_ether_addr(pattrib->dst)) &&
	    (!is_multicast_ether_addr(pattrib->dst)))
		padapter->mlmepriv.LinkDetectInfo.NumRxUnicastOkInPeriod++;

	if(sta)
		psta = sta;
	else
		psta = prframe->u.hdr.psta;

	if(psta)
	{
		pstats = &psta->sta_stats;

		pstats->rx_data_pkts++;
		pstats->rx_bytes += sz;
	}
}

int sta2sta_data_frame(
	struct rtw_adapter *adapter,
	union recv_frame *precv_frame,
	struct sta_info**psta
);
int sta2sta_data_frame(
	struct rtw_adapter *adapter,
	union recv_frame *precv_frame,
	struct sta_info**psta
)
{
	u8 *ptr = precv_frame->u.hdr.rx_data;
	int ret = _SUCCESS;
	struct rx_pkt_attrib *pattrib = & precv_frame->u.hdr.attrib;
	struct	sta_priv		*pstapriv = &adapter->stapriv;
	struct	mlme_priv	*pmlmepriv = &adapter->mlmepriv;
	u8 *mybssid  = get_bssid(pmlmepriv);
	u8 *myhwaddr = myid(&adapter->eeprompriv);
	u8 * sta_addr = NULL;
	int bmcast = is_multicast_ether_addr(pattrib->dst);

_func_enter_;

	if ((check_fwstate(pmlmepriv, WIFI_ADHOC_STATE) == true) ||
		(check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) == true))
	{

		/*  filter packets that SA is myself or multicast or broadcast */
		if (!memcmp(myhwaddr, pattrib->src, ETH_ALEN)) {
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,(" SA==myself \n"));
			ret= _FAIL;
			goto exit;
		}

		if ((memcmp(myhwaddr, pattrib->dst, ETH_ALEN)) && (!bmcast)) {
			ret= _FAIL;
			goto exit;
		}

		if (!memcmp(pattrib->bssid, "\x0\x0\x0\x0\x0\x0", ETH_ALEN) ||
		    !memcmp(mybssid, "\x0\x0\x0\x0\x0\x0", ETH_ALEN) ||
		    memcmp(pattrib->bssid, mybssid, ETH_ALEN)) {
			ret= _FAIL;
			goto exit;
		}

		sta_addr = pattrib->src;
	} else if(check_fwstate(pmlmepriv, WIFI_STATION_STATE) == true) {
		/*  For Station mode, sa and bssid should always be BSSID, and DA is my mac-address */
		if (memcmp(pattrib->bssid, pattrib->src, ETH_ALEN)) {
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("bssid != TA under STATION_MODE; drop pkt\n"));
			ret= _FAIL;
			goto exit;
		}

		sta_addr = pattrib->bssid;

	}
	else if(check_fwstate(pmlmepriv, WIFI_AP_STATE) == true)
	{
		if (bmcast)
		{
			/*  For AP mode, if DA == MCAST, then BSSID should be also MCAST */
			if (!is_multicast_ether_addr(pattrib->bssid)){
				ret= _FAIL;
				goto exit;
			}
		}
		else /*  not mc-frame */
		{
			/*  For AP mode, if DA is non-MCAST, then it must be BSSID, and bssid == BSSID */
			if (memcmp(pattrib->bssid, pattrib->dst, ETH_ALEN)) {
				ret= _FAIL;
				goto exit;
			}

			sta_addr = pattrib->src;
		}

	}
	else if(check_fwstate(pmlmepriv, WIFI_MP_STATE) == true)
	{
		memcpy(pattrib->dst, GetAddr1Ptr(ptr), ETH_ALEN);
		memcpy(pattrib->src, GetAddr2Ptr(ptr), ETH_ALEN);
		memcpy(pattrib->bssid, GetAddr3Ptr(ptr), ETH_ALEN);
		memcpy(pattrib->ra, pattrib->dst, ETH_ALEN);
		memcpy(pattrib->ta, pattrib->src, ETH_ALEN);

		sta_addr = mybssid;
	}
	else
	{
		ret  = _FAIL;
	}

	if(bmcast)
		*psta = rtw_get_bcmc_stainfo(adapter);
	else
		*psta = rtw_get_stainfo(pstapriv, sta_addr); /*  get ap_info */

	if (*psta == NULL) {
		RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("can't get psta under sta2sta_data_frame ; drop pkt\n"));
		ret= _FAIL;
		goto exit;
	}

exit:
_func_exit_;
	return ret;
}

int ap2sta_data_frame(
	struct rtw_adapter *adapter,
	union recv_frame *precv_frame,
	struct sta_info**psta );
int ap2sta_data_frame(
	struct rtw_adapter *adapter,
	union recv_frame *precv_frame,
	struct sta_info**psta )
{
	u8 *ptr = precv_frame->u.hdr.rx_data;
	struct rx_pkt_attrib *pattrib = & precv_frame->u.hdr.attrib;
	int ret = _SUCCESS;
	struct	sta_priv		*pstapriv = &adapter->stapriv;
	struct	mlme_priv	*pmlmepriv = &adapter->mlmepriv;
	u8 *mybssid  = get_bssid(pmlmepriv);
	u8 *myhwaddr = myid(&adapter->eeprompriv);
	int bmcast = is_multicast_ether_addr(pattrib->dst);

_func_enter_;

	if ((check_fwstate(pmlmepriv, WIFI_STATION_STATE) == true)
		&& (check_fwstate(pmlmepriv, _FW_LINKED) == true
			|| check_fwstate(pmlmepriv, _FW_UNDER_LINKING) == true	)
		)
	{

		/*  filter packets that SA is myself or multicast or broadcast */
		if (!memcmp(myhwaddr, pattrib->src, ETH_ALEN)) {
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,(" SA==myself \n"));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s SA="MAC_FMT", myhwaddr="MAC_FMT"\n",
				__FUNCTION__, MAC_ARG(pattrib->src), MAC_ARG(myhwaddr));
			#endif
			ret= _FAIL;
			goto exit;
		}

		/*  da should be for me */
		if (memcmp(myhwaddr, pattrib->dst, ETH_ALEN) && (!bmcast))
		{
			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,
				(" ap2sta_data_frame:  compare DA fail; DA="MAC_FMT"\n", MAC_ARG(pattrib->dst)));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s DA="MAC_FMT"\n", __func__, MAC_ARG(pattrib->dst));
			#endif
			ret= _FAIL;
			goto exit;
		}

		/*  check BSSID */
		if (!memcmp(pattrib->bssid, "\x0\x0\x0\x0\x0\x0", ETH_ALEN) ||
		    !memcmp(mybssid, "\x0\x0\x0\x0\x0\x0", ETH_ALEN) ||
		    memcmp(pattrib->bssid, mybssid, ETH_ALEN)) {
			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,
				(" ap2sta_data_frame:  compare BSSID fail ; BSSID="MAC_FMT"\n", MAC_ARG(pattrib->bssid)));
			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("mybssid="MAC_FMT"\n", MAC_ARG(mybssid)));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s BSSID="MAC_FMT", mybssid="MAC_FMT"\n",
				__FUNCTION__, MAC_ARG(pattrib->bssid), MAC_ARG(mybssid));
			DBG_8723A( "this adapter = %d, buddy adapter = %d\n", adapter->adapter_type, adapter->pbuddy_adapter->adapter_type );
			#endif

			if(!bmcast)
			{
				DBG_8723A("issue_deauth to the nonassociated ap=" MAC_FMT " for the reason(7)\n", MAC_ARG(pattrib->bssid));
				issue_deauth(adapter, pattrib->bssid, WLAN_REASON_CLASS3_FRAME_FROM_NONASSOC_STA);
			}

			ret= _FAIL;
			goto exit;
		}

		if(bmcast)
			*psta = rtw_get_bcmc_stainfo(adapter);
		else
			*psta = rtw_get_stainfo(pstapriv, pattrib->bssid); /*  get ap_info */

		if (*psta == NULL) {
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("ap2sta: can't get psta under STATION_MODE ; drop pkt\n"));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s can't get psta under STATION_MODE ; drop pkt\n", __FUNCTION__);
			#endif
			ret= _FAIL;
			goto exit;
		}

		/* if ((GetFrameSubType(ptr) & WIFI_QOS_DATA_TYPE) == WIFI_QOS_DATA_TYPE) { */
		/*  */

		if (GetFrameSubType(ptr) & BIT(6)) {
			/* No data, will not indicate to upper layer, temporily count it here */
			count_rx_stats(adapter, precv_frame, *psta);
			ret = RTW_RX_HANDLED;
			goto exit;
		}

	}
	else if ((check_fwstate(pmlmepriv, WIFI_MP_STATE) == true) &&
		     (check_fwstate(pmlmepriv, _FW_LINKED) == true) )
	{
		memcpy(pattrib->dst, GetAddr1Ptr(ptr), ETH_ALEN);
		memcpy(pattrib->src, GetAddr2Ptr(ptr), ETH_ALEN);
		memcpy(pattrib->bssid, GetAddr3Ptr(ptr), ETH_ALEN);
		memcpy(pattrib->ra, pattrib->dst, ETH_ALEN);
		memcpy(pattrib->ta, pattrib->src, ETH_ALEN);

		/*  */
		memcpy(pattrib->bssid,  mybssid, ETH_ALEN);

		*psta = rtw_get_stainfo(pstapriv, pattrib->bssid); /*  get sta_info */
		if (*psta == NULL) {
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("can't get psta under MP_MODE ; drop pkt\n"));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s can't get psta under WIFI_MP_STATE ; drop pkt\n", __FUNCTION__);
			#endif
			ret= _FAIL;
			goto exit;
		}

	}
	else if (check_fwstate(pmlmepriv, WIFI_AP_STATE) == true)
	{
		/* Special case */
		ret = RTW_RX_HANDLED;
		goto exit;
	}
	else
	{
		if (!memcmp(myhwaddr, pattrib->dst, ETH_ALEN) && (!bmcast))
		{
			*psta = rtw_get_stainfo(pstapriv, pattrib->bssid); /*  get sta_info */
			if (*psta == NULL)
			{
				DBG_8723A("issue_deauth to the ap=" MAC_FMT " for the reason(7)\n", MAC_ARG(pattrib->bssid));

				issue_deauth(adapter, pattrib->bssid, WLAN_REASON_CLASS3_FRAME_FROM_NONASSOC_STA);
			}
		}

		ret = _FAIL;
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s fw_state:0x%x\n", __FUNCTION__, get_fwstate(pmlmepriv));
		#endif
	}

exit:

_func_exit_;

	return ret;
}

int sta2ap_data_frame(
	struct rtw_adapter *adapter,
	union recv_frame *precv_frame,
	struct sta_info**psta );
int sta2ap_data_frame(
	struct rtw_adapter *adapter,
	union recv_frame *precv_frame,
	struct sta_info**psta )
{
	u8 *ptr = precv_frame->u.hdr.rx_data;
	struct rx_pkt_attrib *pattrib = & precv_frame->u.hdr.attrib;
	struct	sta_priv		*pstapriv = &adapter->stapriv;
	struct	mlme_priv	*pmlmepriv = &adapter->mlmepriv;
	unsigned char *mybssid  = get_bssid(pmlmepriv);
	int ret=_SUCCESS;

_func_enter_;

	if (check_fwstate(pmlmepriv, WIFI_AP_STATE) == true)
	{
		/* For AP mode, RA=BSSID, TX=STA(SRC_ADDR), A3=DST_ADDR */
		if(memcmp(pattrib->bssid, mybssid, ETH_ALEN))
		{
			ret= _FAIL;
			goto exit;
		}

		*psta = rtw_get_stainfo(pstapriv, pattrib->src);
		if (*psta == NULL)
		{
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("can't get psta under AP_MODE; drop pkt\n"));
			DBG_8723A("issue_deauth to sta=" MAC_FMT " for the reason(7)\n", MAC_ARG(pattrib->src));

			issue_deauth(adapter, pattrib->src, WLAN_REASON_CLASS3_FRAME_FROM_NONASSOC_STA);

			ret = RTW_RX_HANDLED;
			goto exit;
		}

		process_pwrbit_data(adapter, precv_frame);

		if ((GetFrameSubType(ptr) & WIFI_QOS_DATA_TYPE) == WIFI_QOS_DATA_TYPE) {
			process_wmmps_data(adapter, precv_frame);
		}

		if (GetFrameSubType(ptr) & BIT(6)) {
			/* No data, will not indicate to upper layer, temporily count it here */
			count_rx_stats(adapter, precv_frame, *psta);
			ret = RTW_RX_HANDLED;
			goto exit;
		}
	}
	else {
		u8 *myhwaddr = myid(&adapter->eeprompriv);
		if (memcmp(pattrib->ra, myhwaddr, ETH_ALEN)) {
			ret = RTW_RX_HANDLED;
			goto exit;
		}
		DBG_8723A("issue_deauth to sta=" MAC_FMT " for the reason(7)\n", MAC_ARG(pattrib->src));
		issue_deauth(adapter, pattrib->src, WLAN_REASON_CLASS3_FRAME_FROM_NONASSOC_STA);
		ret = RTW_RX_HANDLED;
		goto exit;
	}

exit:

_func_exit_;

	return ret;
}

int validate_recv_ctrl_frame(struct rtw_adapter *padapter, union recv_frame *precv_frame);
int validate_recv_ctrl_frame(struct rtw_adapter *padapter, union recv_frame *precv_frame)
{
#ifdef CONFIG_8723AU_AP_MODE
	struct rx_pkt_attrib *pattrib = &precv_frame->u.hdr.attrib;
	struct sta_priv *pstapriv = &padapter->stapriv;
	u8 *pframe = precv_frame->u.hdr.rx_data;
	/* uint len = precv_frame->u.hdr.len; */

	/* DBG_8723A("+validate_recv_ctrl_frame\n"); */

	if (GetFrameType(pframe) != WIFI_CTRL_TYPE)
	{
		return _FAIL;
	}

	/* receive the frames that ra(a1) is my address */
	if (memcmp(GetAddr1Ptr(pframe), myid(&padapter->eeprompriv), ETH_ALEN))
	{
		return _FAIL;
	}

	/* only handle ps-poll */
	if(GetFrameSubType(pframe) == WIFI_PSPOLL)
	{
		u16 aid;
		u8 wmmps_ac=0;
		struct sta_info *psta=NULL;

		aid = GetAid(pframe);
		psta = rtw_get_stainfo(pstapriv, GetAddr2Ptr(pframe));

		if((psta==NULL) || (psta->aid!=aid))
		{
			return _FAIL;
		}

		/* for rx pkt statistics */
		psta->sta_stats.rx_ctrl_pkts++;

		switch(pattrib->priority)
		{
			case 1:
			case 2:
				wmmps_ac = psta->uapsd_bk&BIT(0);
				break;
			case 4:
			case 5:
				wmmps_ac = psta->uapsd_vi&BIT(0);
				break;
			case 6:
			case 7:
				wmmps_ac = psta->uapsd_vo&BIT(0);
				break;
			case 0:
			case 3:
			default:
				wmmps_ac = psta->uapsd_be&BIT(0);
				break;
		}

		if(wmmps_ac)
			return _FAIL;

		if(psta->state & WIFI_STA_ALIVE_CHK_STATE)
		{
			DBG_8723A("%s alive check-rx ps-poll\n", __func__);
			psta->expire_to = pstapriv->expire_to;
			psta->state ^= WIFI_STA_ALIVE_CHK_STATE;
		}

		if((psta->state&WIFI_SLEEP_STATE) && (pstapriv->sta_dz_bitmap&BIT(psta->aid)))
		{
			struct list_head *xmitframe_plist, *xmitframe_phead;
			struct xmit_frame *pxmitframe;
			struct xmit_priv *pxmitpriv = &padapter->xmitpriv;

			/* spin_lock_bh(&psta->sleep_q.lock); */
			spin_lock_bh(&pxmitpriv->lock);

			xmitframe_phead = get_list_head(&psta->sleep_q);
			xmitframe_plist = xmitframe_phead->next;

			if (!list_empty(xmitframe_phead)) {
				pxmitframe = container_of(xmitframe_plist, struct xmit_frame, list);

				xmitframe_plist = xmitframe_plist->next;

				list_del_init(&pxmitframe->list);

				psta->sleepq_len--;

				if(psta->sleepq_len>0)
					pxmitframe->attrib.mdata = 1;
                                else
					pxmitframe->attrib.mdata = 0;

				pxmitframe->attrib.triggered = 1;

	                        /* DBG_8723A("handling ps-poll, q_len=%d, tim=%x\n", psta->sleepq_len, pstapriv->tim_bitmap); */

				rtw_hal_xmitframe_enqueue(padapter, pxmitframe);

				if(psta->sleepq_len==0)
				{
					pstapriv->tim_bitmap &= ~BIT(psta->aid);

					/* DBG_8723A("after handling ps-poll, tim=%x\n", pstapriv->tim_bitmap); */

					/* upate BCN for TIM IE */
					/* update_BCNTIM(padapter); */
					update_beacon(padapter, _TIM_IE_, NULL, false);
				}

				/* spin_unlock_bh(&psta->sleep_q.lock); */
				spin_unlock_bh(&pxmitpriv->lock);

			}
			else
			{
				/* spin_unlock_bh(&psta->sleep_q.lock); */
				spin_unlock_bh(&pxmitpriv->lock);

				/* DBG_8723A("no buffered packets to xmit\n"); */
				if(pstapriv->tim_bitmap&BIT(psta->aid))
				{
					if(psta->sleepq_len==0)
					{
						DBG_8723A("no buffered packets to xmit\n");

						/* issue nulldata with More data bit = 0 to indicate we have no buffered packets */
						issue_nulldata(padapter, psta->hwaddr, 0, 0, 0);
					}
					else
					{
						DBG_8723A("error!psta->sleepq_len=%d\n", psta->sleepq_len);
						psta->sleepq_len=0;
					}

					pstapriv->tim_bitmap &= ~BIT(psta->aid);

					/* upate BCN for TIM IE */
					/* update_BCNTIM(padapter); */
					update_beacon(padapter, _TIM_IE_, NULL, false);
				}

			}

		}

	}

#endif

	return _FAIL;
}

union recv_frame* recvframe_chk_defrag(struct rtw_adapter *padapter, union recv_frame *precv_frame);
int validate_recv_mgnt_frame(struct rtw_adapter *padapter, union recv_frame *precv_frame);
int validate_recv_mgnt_frame(struct rtw_adapter *padapter, union recv_frame *precv_frame)
{
	/* struct mlme_priv *pmlmepriv = &adapter->mlmepriv; */

	RT_TRACE(_module_rtl871x_recv_c_, _drv_info_, ("+validate_recv_mgnt_frame\n"));

	precv_frame = recvframe_chk_defrag(padapter, precv_frame);
	if (precv_frame == NULL) {
		RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_,("%s: fragment packet\n",__FUNCTION__));
		return _SUCCESS;
	}

	{
		/* for rx pkt statistics */
		struct sta_info *psta = rtw_get_stainfo(&padapter->stapriv, GetAddr2Ptr(precv_frame->u.hdr.rx_data));
		if (psta) {
			psta->sta_stats.rx_mgnt_pkts++;
			if (GetFrameSubType(precv_frame->u.hdr.rx_data) == WIFI_BEACON)
				psta->sta_stats.rx_beacon_pkts++;
			else if (GetFrameSubType(precv_frame->u.hdr.rx_data) == WIFI_PROBEREQ)
				psta->sta_stats.rx_probereq_pkts++;
			else if (GetFrameSubType(precv_frame->u.hdr.rx_data) == WIFI_PROBERSP) {
				if (!memcmp(padapter->eeprompriv.mac_addr, GetAddr1Ptr(precv_frame->u.hdr.rx_data), ETH_ALEN))
					psta->sta_stats.rx_probersp_pkts++;
				else if (is_broadcast_ether_addr(GetAddr1Ptr(precv_frame->u.hdr.rx_data))
					|| is_multicast_mac_addr(GetAddr1Ptr(precv_frame->u.hdr.rx_data)))
					psta->sta_stats.rx_probersp_bm_pkts++;
				else
					psta->sta_stats.rx_probersp_uo_pkts++;
			}
		}
	}

	mgt_dispatcher(padapter, precv_frame);

	return _SUCCESS;
}

int validate_recv_data_frame(struct rtw_adapter *adapter, union recv_frame *precv_frame);
int validate_recv_data_frame(struct rtw_adapter *adapter, union recv_frame *precv_frame)
{
	u8 bretry;
	u8 *psa, *pda, *pbssid;
	struct sta_info *psta = NULL;
	u8 *ptr = precv_frame->u.hdr.rx_data;
	struct rx_pkt_attrib	*pattrib = & precv_frame->u.hdr.attrib;
	struct sta_priv		*pstapriv = &adapter->stapriv;
	struct security_priv	*psecuritypriv = &adapter->securitypriv;
	int ret = _SUCCESS;

_func_enter_;

	bretry = GetRetry(ptr);
	pda = get_da(ptr);
	psa = get_sa(ptr);
	pbssid = get_hdr_bssid(ptr);

	if(pbssid == NULL){
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s pbssid == NULL\n", __func__);
		#endif
		ret= _FAIL;
		goto exit;
	}

	memcpy(pattrib->dst, pda, ETH_ALEN);
	memcpy(pattrib->src, psa, ETH_ALEN);

	memcpy(pattrib->bssid, pbssid, ETH_ALEN);

	switch(pattrib->to_fr_ds)
	{
		case 0:
			memcpy(pattrib->ra, pda, ETH_ALEN);
			memcpy(pattrib->ta, psa, ETH_ALEN);
			ret = sta2sta_data_frame(adapter, precv_frame, &psta);
			break;

		case 1:
			memcpy(pattrib->ra, pda, ETH_ALEN);
			memcpy(pattrib->ta, pbssid, ETH_ALEN);
			ret = ap2sta_data_frame(adapter, precv_frame, &psta);
			break;

		case 2:
			memcpy(pattrib->ra, pbssid, ETH_ALEN);
			memcpy(pattrib->ta, psa, ETH_ALEN);
			ret = sta2ap_data_frame(adapter, precv_frame, &psta);
			break;

		case 3:
			memcpy(pattrib->ra, GetAddr1Ptr(ptr), ETH_ALEN);
			memcpy(pattrib->ta, GetAddr2Ptr(ptr), ETH_ALEN);
			ret =_FAIL;
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,(" case 3\n"));
			break;

		default:
			ret =_FAIL;
			break;

	}

	if(ret ==_FAIL){
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s case:%d, res:%d\n", __FUNCTION__, pattrib->to_fr_ds, ret);
		#endif
		goto exit;
	} else if (ret == RTW_RX_HANDLED) {
		goto exit;
	}

	if(psta==NULL){
		RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,(" after to_fr_ds_chk; psta==NULL \n"));
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s psta == NULL\n", __func__);
		#endif
		ret= _FAIL;
		goto exit;
	}

	/* psta->rssi = prxcmd->rssi; */
	/* psta->signal_quality= prxcmd->sq; */
	precv_frame->u.hdr.psta = psta;

	pattrib->amsdu=0;
	pattrib->ack_policy = 0;
	/* parsing QC field */
	if(pattrib->qos == 1)
	{
		pattrib->priority = GetPriority((ptr + 24));
		pattrib->ack_policy = GetAckpolicy((ptr + 24));
		pattrib->amsdu = GetAMsdu((ptr + 24));
		pattrib->hdrlen = pattrib->to_fr_ds==3 ? 32 : 26;

		if(pattrib->priority!=0 && pattrib->priority!=3)
		{
			adapter->recvpriv.bIsAnyNonBEPkts = true;
		}
	}
	else
	{
		pattrib->priority=0;
		pattrib->hdrlen = pattrib->to_fr_ds==3 ? 30 : 24;
	}

	if(pattrib->order)/* HT-CTRL 11n */
	{
		pattrib->hdrlen += 4;
	}

	precv_frame->u.hdr.preorder_ctrl = &psta->recvreorder_ctrl[pattrib->priority];

	/*  decache, drop duplicate recv packets */
	if(recv_decache(precv_frame, bretry, &psta->sta_recvpriv.rxcache) == _FAIL)
	{
		RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("decache : drop pkt\n"));
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s recv_decache return _FAIL\n", __func__);
		#endif
		ret= _FAIL;
		goto exit;
	}

	if(pattrib->privacy){

		RT_TRACE(_module_rtl871x_recv_c_, _drv_info_,
			 ("validate_recv_data_frame:pattrib->privacy=%x\n",
			 pattrib->privacy));
		RT_TRACE(_module_rtl871x_recv_c_, _drv_info_,
			 ("\n ^^^^^^^^^^^is_multicast_ether_addr(pattrib->ra(0x%02x))=%d^^^^^^^^^^^^^^^6\n",
			 pattrib->ra[0], is_multicast_ether_addr(pattrib->ra)));

		GET_ENCRY_ALGO(psecuritypriv, psta, pattrib->encrypt,
			       is_multicast_ether_addr(pattrib->ra));

		RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("\n pattrib->encrypt=%d\n",pattrib->encrypt));

		SET_ICE_IV_LEN(pattrib->iv_len, pattrib->icv_len, pattrib->encrypt);
	}
	else
	{
		pattrib->encrypt = 0;
		pattrib->iv_len = pattrib->icv_len = 0;
	}

exit:

_func_exit_;

	return ret;
}

int validate_recv_frame(struct rtw_adapter *adapter, union recv_frame *precv_frame)
{
	/* shall check frame subtype, to / from ds, da, bssid */

	/* then call check if rx seq/frag. duplicated. */

	u8 type;
	u8 subtype;
	int retval = _SUCCESS;

	struct rx_pkt_attrib *pattrib = & precv_frame->u.hdr.attrib;

	u8 *ptr = precv_frame->u.hdr.rx_data;
	u8  ver =(unsigned char) (*ptr)&0x3 ;
#ifdef CONFIG_FIND_BEST_CHANNEL
	struct mlme_ext_priv *pmlmeext = &adapter->mlmeextpriv;
#endif

_func_enter_;

#ifdef CONFIG_FIND_BEST_CHANNEL
	if (pmlmeext->sitesurvey_res.state == SCAN_PROCESS) {
		int ch_set_idx = rtw_ch_set_search_ch(pmlmeext->channel_set, rtw_get_oper_ch(adapter));
		if (ch_set_idx >= 0)
			pmlmeext->channel_set[ch_set_idx].rx_count++;
	}
#endif

	/* add version chk */
	if(ver!=0){
		RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("validate_recv_data_frame fail! (ver!=0)\n"));
		retval= _FAIL;
		goto exit;
	}

	type =  GetFrameType(ptr);
	subtype = GetFrameSubType(ptr); /* bit(7)~bit(2) */

	pattrib->to_fr_ds = get_tofr_ds(ptr);

	pattrib->frag_num = GetFragNum(ptr);
	pattrib->seq_num = GetSequence(ptr);

	pattrib->pw_save = GetPwrMgt(ptr);
	pattrib->mfrag = GetMFrag(ptr);
	pattrib->mdata = GetMData(ptr);
	pattrib->privacy = GetPrivacy(ptr);
	pattrib->order = GetOrder(ptr);

{
	u8 bDumpRxPkt;
	rtw_hal_get_def_var(adapter, HAL_DEF_DBG_DUMP_RXPKT, &(bDumpRxPkt));
	if(bDumpRxPkt ==1){/* dump all rx packets */
		int i;
		DBG_8723A("############################# \n");

		for(i=0; i<64;i=i+8)
			DBG_8723A("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:\n", *(ptr+i),
			*(ptr+i+1), *(ptr+i+2) ,*(ptr+i+3) ,*(ptr+i+4),*(ptr+i+5), *(ptr+i+6), *(ptr+i+7));
		DBG_8723A("############################# \n");
	}
	else if(bDumpRxPkt ==2){
		if(type== WIFI_MGT_TYPE){
			int i;
			DBG_8723A("############################# \n");

			for(i=0; i<64;i=i+8)
				DBG_8723A("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:\n", *(ptr+i),
				*(ptr+i+1), *(ptr+i+2) ,*(ptr+i+3) ,*(ptr+i+4),*(ptr+i+5), *(ptr+i+6), *(ptr+i+7));
			DBG_8723A("############################# \n");
		}
	}
	else if(bDumpRxPkt ==3){
		if(type== WIFI_DATA_TYPE){
			int i;
			DBG_8723A("############################# \n");

			for(i=0; i<64;i=i+8)
				DBG_8723A("%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:\n", *(ptr+i),
				*(ptr+i+1), *(ptr+i+2) ,*(ptr+i+3) ,*(ptr+i+4),*(ptr+i+5), *(ptr+i+6), *(ptr+i+7));
			DBG_8723A("############################# \n");
		}
	}
}
	switch (type)
	{
		case WIFI_MGT_TYPE: /* mgnt */
			retval = validate_recv_mgnt_frame(adapter, precv_frame);
			if (retval == _FAIL)
			{
				RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("validate_recv_mgnt_frame fail\n"));
			}
			retval = _FAIL; /*  only data frame return _SUCCESS */
			break;
		case WIFI_CTRL_TYPE: /* ctrl */
			retval = validate_recv_ctrl_frame(adapter, precv_frame);
			if (retval == _FAIL)
			{
				RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("validate_recv_ctrl_frame fail\n"));
			}
			retval = _FAIL; /*  only data frame return _SUCCESS */
			break;
		case WIFI_DATA_TYPE: /* data */
			rtw_led_control(adapter, LED_CTL_RX);
			pattrib->qos = (subtype & BIT(7))? 1:0;
			retval = validate_recv_data_frame(adapter, precv_frame);
			if (retval == _FAIL)
			{
				struct recv_priv *precvpriv = &adapter->recvpriv;
				/* RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("validate_recv_data_frame fail\n")); */
				precvpriv->rx_drop++;
			}
			break;
		default:
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("validate_recv_data_frame fail! type=0x%x\n", type));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME validate_recv_data_frame fail! type=0x%x\n", type);
			#endif
			retval = _FAIL;
			break;
	}

exit:

_func_exit_;

	return retval;
}

/* remove the wlanhdr and add the eth_hdr */

static int wlanhdr_to_ethhdr ( union recv_frame *precvframe)
{
	int	rmv_len;
	u16	eth_type, len;
	u8	bsnaphdr;
	u8	*psnap_type;
	struct ieee80211_snap_hdr	*psnap;

	int ret=_SUCCESS;
	struct rtw_adapter			*adapter =precvframe->u.hdr.adapter;
	struct mlme_priv	*pmlmepriv = &adapter->mlmepriv;

	u8	*ptr = get_recvframe_data(precvframe) ; /*  point to frame_ctrl field */
	struct rx_pkt_attrib *pattrib = & precvframe->u.hdr.attrib;

_func_enter_;

	if(pattrib->encrypt){
		recvframe_pull_tail(precvframe, pattrib->icv_len);
	}

	psnap=(struct ieee80211_snap_hdr	*)(ptr+pattrib->hdrlen + pattrib->iv_len);
	psnap_type=ptr+pattrib->hdrlen + pattrib->iv_len+SNAP_SIZE;
	/* convert hdr + possible LLC headers into Ethernet header */
	/* eth_type = (psnap_type[0] << 8) | psnap_type[1]; */
	if ((!memcmp(psnap, rtw_rfc1042_header, SNAP_SIZE) &&
	     memcmp(psnap_type, SNAP_ETH_TYPE_IPX, 2) &&
	     memcmp(psnap_type, SNAP_ETH_TYPE_APPLETALK_AARP, 2)) ||
	     /* eth_type != ETH_P_AARP && eth_type != ETH_P_IPX) || */
	    !memcmp(psnap, rtw_bridge_tunnel_header, SNAP_SIZE)) {
		/* remove RFC1042 or Bridge-Tunnel encapsulation and replace EtherType */
		bsnaphdr = true;
	}
	else {
		/* Leave Ethernet header part of hdr and full payload */
		bsnaphdr = false;
	}

	rmv_len = pattrib->hdrlen + pattrib->iv_len +(bsnaphdr?SNAP_SIZE:0);
	len = precvframe->u.hdr.len - rmv_len;

	RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("\n===pattrib->hdrlen: %x,  pattrib->iv_len:%x ===\n\n", pattrib->hdrlen,  pattrib->iv_len));

	memcpy(&eth_type, ptr+rmv_len, 2);
	eth_type= ntohs((unsigned short )eth_type); /* pattrib->ether_type */
	pattrib->eth_type = eth_type;

	if ((check_fwstate(pmlmepriv, WIFI_MP_STATE) == true))
	{
		ptr += rmv_len ;
		*ptr = 0x87;
		*(ptr+1) = 0x12;

		eth_type = 0x8712;
		/*  append rx status for mp test packets */
		ptr = recvframe_pull(precvframe, (rmv_len-sizeof(struct ethhdr)+2)-24);
		memcpy(ptr, get_rxmem(precvframe), 24);
		ptr+=24;
	}
	else {
		ptr = recvframe_pull(precvframe, (rmv_len-sizeof(struct ethhdr)+ (bsnaphdr?2:0)));
	}

	memcpy(ptr, pattrib->dst, ETH_ALEN);
	memcpy(ptr+ETH_ALEN, pattrib->src, ETH_ALEN);

	if(!bsnaphdr) {
		len = htons(len);
		memcpy(ptr+12, &len, 2);
	}

_func_exit_;
	return ret;
}

/* perform defrag */
union recv_frame * recvframe_defrag(struct rtw_adapter *adapter,_queue *defrag_q);
union recv_frame * recvframe_defrag(struct rtw_adapter *adapter,_queue *defrag_q)
{
	struct list_head *plist, *phead, *ptmp;
	u8	*data,wlanhdr_offset;
	u8	curfragnum;
	struct recv_frame_hdr *pfhdr,*pnfhdr;
	union recv_frame* prframe, *pnextrframe;
	_queue	*pfree_recv_queue;

_func_enter_;

	curfragnum=0;
	pfree_recv_queue=&adapter->recvpriv.free_recv_queue;

	phead = get_list_head(defrag_q);
	plist = phead->next;
	pfhdr = container_of(plist, struct recv_frame_hdr, list);
	prframe = (union recv_frame *)pfhdr;
	list_del_init(&(pfhdr->list));

	if(curfragnum!=pfhdr->attrib.frag_num)
	{
		/* the first fragment number must be 0 */
		/* free the whole queue */
		rtw_free_recvframe(prframe, pfree_recv_queue);
		rtw_free_recvframe_queue(defrag_q, pfree_recv_queue);

		return NULL;
	}

	curfragnum++;

	phead = get_list_head(defrag_q);

	data=get_recvframe_data(prframe);

	list_for_each_safe(plist, ptmp, phead) {
		pnfhdr = container_of(plist, struct recv_frame_hdr , list);
		pnextrframe = (union recv_frame *)pnfhdr;
		/* check the fragment sequence  (2nd ~n fragment frame) */

		if(curfragnum!=pnfhdr->attrib.frag_num)
		{
			/* the fragment number must be increasing  (after decache) */
			/* release the defrag_q & prframe */
			rtw_free_recvframe(prframe, pfree_recv_queue);
			rtw_free_recvframe_queue(defrag_q, pfree_recv_queue);
			return NULL;
		}

		curfragnum++;

		/* copy the 2nd~n fragment frame's payload to the first fragment */
		/* get the 2nd~last fragment frame's payload */

		wlanhdr_offset = pnfhdr->attrib.hdrlen + pnfhdr->attrib.iv_len;

		recvframe_pull(pnextrframe, wlanhdr_offset);

		/* append  to first fragment frame's tail (if privacy frame, pull the ICV) */
		recvframe_pull_tail(prframe, pfhdr->attrib.icv_len);

		/* memcpy */
		memcpy(pfhdr->rx_tail, pnfhdr->rx_data, pnfhdr->len);

		recvframe_put(prframe, pnfhdr->len);

		pfhdr->attrib.icv_len=pnfhdr->attrib.icv_len;
	};

	/* free the defrag_q queue and return the prframe */
	rtw_free_recvframe_queue(defrag_q, pfree_recv_queue);

	RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("Performance defrag!!!!!\n"));

_func_exit_;

	return prframe;
}

/* check if need to defrag, if needed queue the frame to defrag_q */
union recv_frame* recvframe_chk_defrag(struct rtw_adapter *padapter, union recv_frame *precv_frame)
{
	u8	ismfrag;
	u8	fragnum;
	u8	*psta_addr;
	struct recv_frame_hdr *pfhdr;
	struct sta_info *psta;
	struct sta_priv *pstapriv;
	struct list_head *phead;
	union recv_frame *prtnframe = NULL;
	_queue *pfree_recv_queue, *pdefrag_q;

_func_enter_;

	pstapriv = &padapter->stapriv;

	pfhdr = &precv_frame->u.hdr;

	pfree_recv_queue = &padapter->recvpriv.free_recv_queue;

	/* need to define struct of wlan header frame ctrl */
	ismfrag = pfhdr->attrib.mfrag;
	fragnum = pfhdr->attrib.frag_num;

	psta_addr = pfhdr->attrib.ta;
	psta = rtw_get_stainfo(pstapriv, psta_addr);
	if (psta == NULL)
	{
		u8 type = GetFrameType(pfhdr->rx_data);
		if (type != WIFI_DATA_TYPE) {
			psta = rtw_get_bcmc_stainfo(padapter);
			pdefrag_q = &psta->sta_recvpriv.defrag_q;
		} else
			pdefrag_q = NULL;
	}
	else
		pdefrag_q = &psta->sta_recvpriv.defrag_q;

	if ((ismfrag==0) && (fragnum==0))
	{
		prtnframe = precv_frame;/* isn't a fragment frame */
	}

	if (ismfrag==1)
	{
		/* 0~(n-1) fragment frame */
		/* enqueue to defraf_g */
		if(pdefrag_q != NULL)
		{
			if(fragnum==0)
			{
				/* the first fragment */
				if(_rtw_queue_empty(pdefrag_q) == false)
				{
					/* free current defrag_q */
					rtw_free_recvframe_queue(pdefrag_q, pfree_recv_queue);
				}
			}

			/* Then enqueue the 0~(n-1) fragment into the defrag_q */

			/* spin_lock(&pdefrag_q->lock); */
			phead = get_list_head(pdefrag_q);
			list_add_tail(&pfhdr->list, phead);
			/* spin_unlock(&pdefrag_q->lock); */

			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("Enqueuq: ismfrag = %d, fragnum= %d\n", ismfrag,fragnum));

			prtnframe=NULL;

		}
		else
		{
			/* can't find this ta's defrag_queue, so free this recv_frame */
			rtw_free_recvframe(precv_frame, pfree_recv_queue);
			prtnframe=NULL;
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("Free because pdefrag_q ==NULL: ismfrag = %d, fragnum= %d\n", ismfrag, fragnum));
		}

	}

	if((ismfrag==0)&&(fragnum!=0))
	{
		/* the last fragment frame */
		/* enqueue the last fragment */
		if(pdefrag_q != NULL)
		{
			/* spin_lock(&pdefrag_q->lock); */
			phead = get_list_head(pdefrag_q);
			list_add_tail(&pfhdr->list,phead);
			/* spin_unlock(&pdefrag_q->lock); */

			/* call recvframe_defrag to defrag */
			RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("defrag: ismfrag = %d, fragnum= %d\n", ismfrag, fragnum));
			precv_frame = recvframe_defrag(padapter, pdefrag_q);
			prtnframe=precv_frame;

		}
		else
		{
			/* can't find this ta's defrag_queue, so free this recv_frame */
			rtw_free_recvframe(precv_frame, pfree_recv_queue);
			prtnframe=NULL;
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("Free because pdefrag_q ==NULL: ismfrag = %d, fragnum= %d\n", ismfrag,fragnum));
		}

	}

	if((prtnframe!=NULL)&&(prtnframe->u.hdr.attrib.privacy))
	{
		/* after defrag we must check tkip mic code */
		if(recvframe_chkmic(padapter,  prtnframe)==_FAIL)
		{
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("recvframe_chkmic(padapter,  prtnframe)==_FAIL\n"));
			rtw_free_recvframe(prtnframe,pfree_recv_queue);
			prtnframe=NULL;
		}
	}

_func_exit_;

	return prtnframe;
}

#define ENDIAN_FREE 1

int amsdu_to_msdu(struct rtw_adapter *padapter, union recv_frame *prframe);
int amsdu_to_msdu(struct rtw_adapter *padapter, union recv_frame *prframe)
{
	int	a_len, padding_len;
	u16	eth_type, nSubframe_Length;
	u8	nr_subframes, i;
	unsigned char *pdata;
	struct rx_pkt_attrib *pattrib;
	unsigned char *data_ptr;
	struct sk_buff *sub_skb,*subframes[MAX_SUBFRAME_COUNT];
	struct recv_priv *precvpriv = &padapter->recvpriv;
	_queue *pfree_recv_queue = &(precvpriv->free_recv_queue);
	int	ret = _SUCCESS;
	nr_subframes = 0;

	pattrib = &prframe->u.hdr.attrib;

	recvframe_pull(prframe, prframe->u.hdr.attrib.hdrlen);

	if(prframe->u.hdr.attrib.iv_len >0)
	{
		recvframe_pull(prframe, prframe->u.hdr.attrib.iv_len);
	}

	a_len = prframe->u.hdr.len;

	pdata = prframe->u.hdr.rx_data;

	while(a_len > ETH_HLEN) {

		/* Offset 12 denote 2 mac address */
#ifdef ENDIAN_FREE
		/* nSubframe_Length = ntohs(*((u16*)(pdata + 12))); */
		nSubframe_Length = RTW_GET_BE16(pdata + 12);
#else /*  ENDIAN_FREE */
		nSubframe_Length = *((u16*)(pdata + 12));
		/* m==>change the length order */
		nSubframe_Length = (nSubframe_Length>>8) + (nSubframe_Length<<8);
		/* ntohs(nSubframe_Length); */
#endif /*  ENDIAN_FREE */

		if (a_len < (ETH_HLEN + nSubframe_Length)) {
			DBG_8723A("nRemain_Length is %d and nSubframe_Length is : %d\n",a_len,nSubframe_Length);
			goto exit;
		}

		/* move the data point to data content */
		pdata += ETH_HLEN;
		a_len -= ETH_HLEN;

		/* Allocate new skb for releasing to upper layer */
#ifdef CONFIG_SKB_COPY
		sub_skb = dev_alloc_skb(nSubframe_Length + 12);
		if(sub_skb)
		{
			skb_reserve(sub_skb, 12);
			data_ptr = (u8 *)skb_put(sub_skb, nSubframe_Length);
			memcpy(data_ptr, pdata, nSubframe_Length);
		}
		else
#endif /*  CONFIG_SKB_COPY */
		{
			sub_skb = skb_clone(prframe->u.hdr.pkt, GFP_ATOMIC);
			if(sub_skb)
			{
				sub_skb->data = pdata;
				sub_skb->len = nSubframe_Length;
				skb_set_tail_pointer(sub_skb, nSubframe_Length);
			}
			else
			{
				DBG_8723A("skb_clone() Fail!!! , nr_subframes = %d\n",nr_subframes);
				break;
			}
		}


		/* sub_skb->dev = padapter->pnetdev; */
		subframes[nr_subframes++] = sub_skb;

		if(nr_subframes >= MAX_SUBFRAME_COUNT) {
			DBG_8723A("ParseSubframe(): Too many Subframes! Packets dropped!\n");
			break;
		}

		pdata += nSubframe_Length;
		a_len -= nSubframe_Length;
		if(a_len != 0) {
			padding_len = 4 - ((nSubframe_Length + ETH_HLEN) & (4-1));
			if(padding_len == 4) {
				padding_len = 0;
			}

			if(a_len < padding_len) {
				goto exit;
			}
			pdata += padding_len;
			a_len -= padding_len;
		}
	}

	for(i=0; i<nr_subframes; i++){
		sub_skb = subframes[i];
		/* convert hdr + possible LLC headers into Ethernet header */
#ifdef ENDIAN_FREE
		/* eth_type = ntohs(*(u16*)&sub_skb->data[6]); */
		eth_type = RTW_GET_BE16(&sub_skb->data[6]);
#else /*  ENDIAN_FREE */
		eth_type = (sub_skb->data[6] << 8) | sub_skb->data[7];
#endif /*  ENDIAN_FREE */
		if (sub_skb->len >= 8 &&
		    ((!memcmp(sub_skb->data, rtw_rfc1042_header, SNAP_SIZE) &&
		      eth_type != ETH_P_AARP && eth_type != ETH_P_IPX) ||
		     !memcmp(sub_skb->data, rtw_bridge_tunnel_header, SNAP_SIZE) )) {
			/* remove RFC1042 or Bridge-Tunnel encapsulation and replace EtherType */
			skb_pull(sub_skb, SNAP_SIZE);
			memcpy(skb_push(sub_skb, ETH_ALEN), pattrib->src, ETH_ALEN);
			memcpy(skb_push(sub_skb, ETH_ALEN), pattrib->dst, ETH_ALEN);
		} else {
			u16 len;
			/* Leave Ethernet header part of hdr and full payload */
			len = htons(sub_skb->len);
			memcpy(skb_push(sub_skb, 2), &len, 2);
			memcpy(skb_push(sub_skb, ETH_ALEN), pattrib->src, ETH_ALEN);
			memcpy(skb_push(sub_skb, ETH_ALEN), pattrib->dst, ETH_ALEN);
		}

		/* Indicat the packets to upper layer */
		sub_skb->protocol = eth_type_trans(sub_skb, padapter->pnetdev);
		sub_skb->dev = padapter->pnetdev;

		sub_skb->ip_summed = CHECKSUM_NONE;
		netif_rx(sub_skb);
	}

exit:

	prframe->u.hdr.len=0;
	rtw_free_recvframe(prframe, pfree_recv_queue);/* free this recv_frame */

	return ret;
}

int check_indicate_seq(struct recv_reorder_ctrl *preorder_ctrl, u16 seq_num);
int check_indicate_seq(struct recv_reorder_ctrl *preorder_ctrl, u16 seq_num)
{
	u8	wsize = preorder_ctrl->wsize_b;
	u16	wend = (preorder_ctrl->indicate_seq + wsize -1) & 0xFFF;/*  4096; */

	/*  Rx Reorder initialize condition. */
	if (preorder_ctrl->indicate_seq == 0xFFFF)
	{
		preorder_ctrl->indicate_seq = seq_num;
		#ifdef DBG_RX_SEQ
		DBG_8723A("DBG_RX_SEQ %s:%d init IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
			preorder_ctrl->indicate_seq, seq_num);
		#endif

		/* DbgPrint("check_indicate_seq, 1st->indicate_seq=%d\n", precvpriv->indicate_seq); */
	}

	/* DbgPrint("enter->check_indicate_seq(): IndicateSeq: %d, NewSeq: %d\n", precvpriv->indicate_seq, seq_num); */

	/*  Drop out the packet which SeqNum is smaller than WinStart */
	if( SN_LESS(seq_num, preorder_ctrl->indicate_seq) )
	{
		/* RT_TRACE(COMP_RX_REORDER, DBG_LOUD, ("CheckRxTsIndicateSeq(): Packet Drop! IndicateSeq: %d, NewSeq: %d\n", pTS->RxIndicateSeq, NewSeqNum)); */
		/* DbgPrint("CheckRxTsIndicateSeq(): Packet Drop! IndicateSeq: %d, NewSeq: %d\n", precvpriv->indicate_seq, seq_num); */

		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("%s IndicateSeq: %d > NewSeq: %d\n", __FUNCTION__,
			preorder_ctrl->indicate_seq, seq_num);
		#endif

		return false;
	}

	/*  */
	/*  Sliding window manipulation. Conditions includes: */
	/*  1. Incoming SeqNum is equal to WinStart =>Window shift 1 */
	/*  2. Incoming SeqNum is larger than the WinEnd => Window shift N */
	/*  */
	if( SN_EQUAL(seq_num, preorder_ctrl->indicate_seq) )
	{
		preorder_ctrl->indicate_seq = (preorder_ctrl->indicate_seq + 1) & 0xFFF;
		#ifdef DBG_RX_SEQ
		DBG_8723A("DBG_RX_SEQ %s:%d SN_EQUAL IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
			preorder_ctrl->indicate_seq, seq_num);
		#endif
	}
	else if(SN_LESS(wend, seq_num))
	{
		/* RT_TRACE(COMP_RX_REORDER, DBG_LOUD, ("CheckRxTsIndicateSeq(): Window Shift! IndicateSeq: %d, NewSeq: %d\n", pTS->RxIndicateSeq, NewSeqNum)); */
		/* DbgPrint("CheckRxTsIndicateSeq(): Window Shift! IndicateSeq: %d, NewSeq: %d\n", precvpriv->indicate_seq, seq_num); */

		/*  boundary situation, when seq_num cross 0xFFF */
		if(seq_num >= (wsize - 1))
			preorder_ctrl->indicate_seq = seq_num + 1 -wsize;
		else
			preorder_ctrl->indicate_seq = 0xFFF - (wsize - (seq_num + 1)) + 1;

		#ifdef DBG_RX_SEQ
		DBG_8723A("DBG_RX_SEQ %s:%d SN_LESS(wend, seq_num) IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
			preorder_ctrl->indicate_seq, seq_num);
		#endif
	}

	/* DbgPrint("exit->check_indicate_seq(): IndicateSeq: %d, NewSeq: %d\n", precvpriv->indicate_seq, seq_num); */

	return true;
}

int enqueue_reorder_recvframe(struct recv_reorder_ctrl *preorder_ctrl, union recv_frame *prframe);
int enqueue_reorder_recvframe(struct recv_reorder_ctrl *preorder_ctrl, union recv_frame *prframe)
{
	struct rx_pkt_attrib *pattrib = &prframe->u.hdr.attrib;
	_queue *ppending_recvframe_queue = &preorder_ctrl->pending_recvframe_queue;
	struct list_head *phead, *plist, *ptmp;
	struct recv_frame_hdr *hdr;
	struct rx_pkt_attrib *pnextattrib;

	/* DbgPrint("+enqueue_reorder_recvframe()\n"); */

	/* spin_lock_irqsave(&ppending_recvframe_queue->lock); */
	/* spin_lock_ex(&ppending_recvframe_queue->lock); */

	phead = get_list_head(ppending_recvframe_queue);

	list_for_each_safe(plist, ptmp, phead) {
		hdr = container_of(plist, struct recv_frame_hdr, list);
		pnextattrib = &hdr->attrib;

		if (SN_LESS(pnextattrib->seq_num, pattrib->seq_num)) {
			continue;
		} else if(SN_EQUAL(pnextattrib->seq_num, pattrib->seq_num)) {
			/* Duplicate entry is found!! Do not insert current entry. */
			/* RT_TRACE(COMP_RX_REORDER, DBG_TRACE, ("InsertRxReorderList(): Duplicate packet is dropped!! IndicateSeq: %d, NewSeq: %d\n", pTS->RxIndicateSeq, SeqNum)); */

			/* spin_unlock_irqrestore(&ppending_recvframe_queue->lock); */
			return false;
		} else {
			break;
		}

		/* DbgPrint("enqueue_reorder_recvframe():while\n"); */

	}

	/* spin_lock_irqsave(&ppending_recvframe_queue->lock); */
	/* spin_lock_ex(&ppending_recvframe_queue->lock); */

	list_del_init(&(prframe->u.hdr.list));

	list_add_tail(&(prframe->u.hdr.list), plist);

	/* spin_unlock_ex(&ppending_recvframe_queue->lock); */
	/* spin_unlock_irqrestore(&ppending_recvframe_queue->lock); */

	/* RT_TRACE(COMP_RX_REORDER, DBG_TRACE, ("InsertRxReorderList(): Pkt insert into buffer!! IndicateSeq: %d, NewSeq: %d\n", pTS->RxIndicateSeq, SeqNum)); */
	return true;
}

int recv_indicatepkts_in_order(struct rtw_adapter *padapter, struct recv_reorder_ctrl *preorder_ctrl, int bforced);
int recv_indicatepkts_in_order(struct rtw_adapter *padapter, struct recv_reorder_ctrl *preorder_ctrl, int bforced)
{
	/* u8 bcancelled; */
	struct list_head	*phead, *plist;
	union recv_frame *prframe;
	struct recv_frame_hdr *prhdr;
	struct rx_pkt_attrib *pattrib;
	/* u8 index = 0; */
	int bPktInBuf = false;
	struct recv_priv *precvpriv = &padapter->recvpriv;
	_queue *ppending_recvframe_queue = &preorder_ctrl->pending_recvframe_queue;

	/* DbgPrint("+recv_indicatepkts_in_order\n"); */

	/* spin_lock_irqsave(&ppending_recvframe_queue->lock); */
	/* spin_lock_ex(&ppending_recvframe_queue->lock); */

	phead =		get_list_head(ppending_recvframe_queue);
	plist = phead->next;

	/*  Handling some condition for forced indicate case. */
	if(bforced==true)
	{
		if (list_empty(phead)) {
			/*  spin_unlock_irqrestore(&ppending_recvframe_queue->lock); */
			/* spin_unlock_ex(&ppending_recvframe_queue->lock); */
			return true;
		}

		prhdr = container_of(plist, struct recv_frame_hdr, list);
	        pattrib = &prhdr->attrib;
		preorder_ctrl->indicate_seq = pattrib->seq_num;
		#ifdef DBG_RX_SEQ
		DBG_8723A("DBG_RX_SEQ %s:%d IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
			preorder_ctrl->indicate_seq, pattrib->seq_num);
		#endif
	}

	/*  Prepare indication list and indication. */
	/*  Check if there is any packet need indicate. */
	while (!list_empty(phead)) {

		prhdr = container_of(plist, struct recv_frame_hdr, list);
		prframe = (union recv_frame *)prhdr;
		pattrib = &prhdr->attrib;

		if(!SN_LESS(preorder_ctrl->indicate_seq, pattrib->seq_num))
		{
			RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_,
				 ("recv_indicatepkts_in_order: indicate=%d seq=%d amsdu=%d\n",
				  preorder_ctrl->indicate_seq, pattrib->seq_num, pattrib->amsdu));

			plist = plist->next;
			list_del_init(&(prframe->u.hdr.list));

			if(SN_EQUAL(preorder_ctrl->indicate_seq, pattrib->seq_num))
			{
				preorder_ctrl->indicate_seq = (preorder_ctrl->indicate_seq + 1) & 0xFFF;
				#ifdef DBG_RX_SEQ
				DBG_8723A("DBG_RX_SEQ %s:%d IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
					preorder_ctrl->indicate_seq, pattrib->seq_num);
				#endif
			}

			if(!pattrib->amsdu)
			{
				if ((padapter->bDriverStopped == false) &&
				    (padapter->bSurpriseRemoved == false))
				{

					rtw_recv_indicatepkt(padapter, prframe);/* indicate this recv_frame */

				}
			}
			else if(pattrib->amsdu==1)
			{
				if(amsdu_to_msdu(padapter, prframe)!=_SUCCESS)
				{
					rtw_free_recvframe(prframe, &precvpriv->free_recv_queue);
				}
			}
			else
			{
				/* error condition; */
			}

			/* Update local variables. */
			bPktInBuf = false;

		}
		else
		{
			bPktInBuf = true;
			break;
		}

		/* DbgPrint("recv_indicatepkts_in_order():while\n"); */

	}

	/* spin_unlock_ex(&ppending_recvframe_queue->lock); */
	/* spin_unlock_irqrestore(&ppending_recvframe_queue->lock); */

	return bPktInBuf;
}

int recv_indicatepkt_reorder(struct rtw_adapter *padapter, union recv_frame *prframe);
int recv_indicatepkt_reorder(struct rtw_adapter *padapter, union recv_frame *prframe)
{
	int retval = _SUCCESS;
	struct rx_pkt_attrib *pattrib = &prframe->u.hdr.attrib;
	struct recv_reorder_ctrl *preorder_ctrl = prframe->u.hdr.preorder_ctrl;
	_queue *ppending_recvframe_queue = &preorder_ctrl->pending_recvframe_queue;

	if (!pattrib->amsdu) {
		/* s1. */
		wlanhdr_to_ethhdr(prframe);

		if ((pattrib->qos!=1) || (pattrib->eth_type==0x0806) ||
		    (pattrib->ack_policy!=0)) {
			if ((padapter->bDriverStopped == false) &&
			    (padapter->bSurpriseRemoved == false)) {
				RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_, ("@@@@  recv_indicatepkt_reorder -recv_func recv_indicatepkt\n" ));

				rtw_recv_indicatepkt(padapter, prframe);
				return _SUCCESS;
			}

			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s pattrib->qos !=1\n", __FUNCTION__);
			#endif

			return _FAIL;
		}

		if (preorder_ctrl->enable == false) {
			/* indicate this recv_frame */
			preorder_ctrl->indicate_seq = pattrib->seq_num;
			#ifdef DBG_RX_SEQ
			DBG_8723A("DBG_RX_SEQ %s:%d IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
				preorder_ctrl->indicate_seq, pattrib->seq_num);
			#endif

			rtw_recv_indicatepkt(padapter, prframe);

			preorder_ctrl->indicate_seq = (preorder_ctrl->indicate_seq + 1)%4096;
			#ifdef DBG_RX_SEQ
			DBG_8723A("DBG_RX_SEQ %s:%d IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
				preorder_ctrl->indicate_seq, pattrib->seq_num);
			#endif

			return _SUCCESS;
		}
	} else if(pattrib->amsdu==1) {
		 /* temp filter -> means didn't support A-MSDUs in a A-MPDU */
		if (preorder_ctrl->enable == false) {
			preorder_ctrl->indicate_seq = pattrib->seq_num;
			#ifdef DBG_RX_SEQ
			DBG_8723A("DBG_RX_SEQ %s:%d IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
				preorder_ctrl->indicate_seq, pattrib->seq_num);
			#endif

			retval = amsdu_to_msdu(padapter, prframe);

			preorder_ctrl->indicate_seq = (preorder_ctrl->indicate_seq + 1)%4096;
			#ifdef DBG_RX_SEQ
			DBG_8723A("DBG_RX_SEQ %s:%d IndicateSeq: %d, NewSeq: %d\n", __FUNCTION__, __LINE__,
				preorder_ctrl->indicate_seq, pattrib->seq_num);
			#endif

			if (retval != _SUCCESS) {
				#ifdef DBG_RX_DROP_FRAME
				DBG_8723A("DBG_RX_DROP_FRAME %s amsdu_to_msdu fail\n", __FUNCTION__);
				#endif
			}
			return retval;
		}
	}

	spin_lock_bh(&ppending_recvframe_queue->lock);

	RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_,
		 ("recv_indicatepkt_reorder: indicate=%d seq=%d\n",
		  preorder_ctrl->indicate_seq, pattrib->seq_num));

	/* s2. check if winstart_b(indicate_seq) needs to been updated */
	if (!check_indicate_seq(preorder_ctrl, pattrib->seq_num)) {
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s check_indicate_seq fail\n", __FUNCTION__);
		#endif
		goto _err_exit;
	}

	/* s3. Insert all packet into Reorder Queue to maintain its ordering. */
	if (!enqueue_reorder_recvframe(preorder_ctrl, prframe)) {
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s enqueue_reorder_recvframe fail\n", __FUNCTION__);
		#endif
		goto _err_exit;
	}

	/* s4. */
	/*  Indication process. */
	/*  After Packet dropping and Sliding Window shifting as above, we can now just indicate the packets */
	/*  with the SeqNum smaller than latest WinStart and buffer other packets. */
	/*  */
	/*  For Rx Reorder condition: */
	/*  1. All packets with SeqNum smaller than WinStart => Indicate */
	/*  2. All packets with SeqNum larger than or equal to WinStart => Buffer it. */
	/*  */

	if (recv_indicatepkts_in_order(padapter, preorder_ctrl, false)==true) {
		mod_timer(&preorder_ctrl->reordering_ctrl_timer,
			  jiffies + msecs_to_jiffies(REORDER_WAIT_TIME));
		spin_unlock_bh(&ppending_recvframe_queue->lock);
	} else {
		spin_unlock_bh(&ppending_recvframe_queue->lock);
		del_timer_sync(&preorder_ctrl->reordering_ctrl_timer);
	}

_success_exit:

	return _SUCCESS;

_err_exit:

        spin_unlock_bh(&ppending_recvframe_queue->lock);

	return _FAIL;
}

void rtw_reordering_ctrl_timeout_handler(unsigned long pcontext)
{
	struct recv_reorder_ctrl *preorder_ctrl = (struct recv_reorder_ctrl *)pcontext;
	struct rtw_adapter *padapter = preorder_ctrl->padapter;
	_queue *ppending_recvframe_queue = &preorder_ctrl->pending_recvframe_queue;

	if(padapter->bDriverStopped ||padapter->bSurpriseRemoved)
	{
		return;
	}

	/* DBG_8723A("+rtw_reordering_ctrl_timeout_handler()=>\n"); */

	spin_lock_bh(&ppending_recvframe_queue->lock);

	if(recv_indicatepkts_in_order(padapter, preorder_ctrl, true)==true)
	{
		mod_timer(&preorder_ctrl->reordering_ctrl_timer,
			  jiffies + msecs_to_jiffies(REORDER_WAIT_TIME));
	}

	spin_unlock_bh(&ppending_recvframe_queue->lock);
}

int process_recv_indicatepkts(struct rtw_adapter *padapter, union recv_frame *prframe);
int process_recv_indicatepkts(struct rtw_adapter *padapter, union recv_frame *prframe)
{
	int retval = _SUCCESS;
	/* struct recv_priv *precvpriv = &padapter->recvpriv; */
	/* struct rx_pkt_attrib *pattrib = &prframe->u.hdr.attrib; */
	struct mlme_priv	*pmlmepriv = &padapter->mlmepriv;

#ifdef CONFIG_80211N_HT

	struct ht_priv	*phtpriv = &pmlmepriv->htpriv;

	if(phtpriv->ht_option==true)  /* B/G/N Mode */
	{
		/* prframe->u.hdr.preorder_ctrl = &precvpriv->recvreorder_ctrl[pattrib->priority]; */

		if(recv_indicatepkt_reorder(padapter, prframe)!=_SUCCESS)/*  including perform A-MPDU Rx Ordering Buffer Control */
		{
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s recv_indicatepkt_reorder error!\n", __FUNCTION__);
			#endif

			if ((padapter->bDriverStopped == false) &&
			    (padapter->bSurpriseRemoved == false))
			{
				retval = _FAIL;
				return retval;
			}
		}
	}
	else /* B/G mode */
#endif
	{
		retval=wlanhdr_to_ethhdr (prframe);
		if(retval != _SUCCESS)
		{
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("wlanhdr_to_ethhdr: drop pkt \n"));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s wlanhdr_to_ethhdr error!\n", __FUNCTION__);
			#endif
			return retval;
		}

		if ((padapter->bDriverStopped ==false)&&( padapter->bSurpriseRemoved==false))
		{
			/* indicate this recv_frame */
			RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_, ("@@@@ process_recv_indicatepkts- recv_func recv_indicatepkt\n" ));
			rtw_recv_indicatepkt(padapter, prframe);

		}
		else
		{
			RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_, ("@@@@ process_recv_indicatepkts- recv_func free_indicatepkt\n" ));

			RT_TRACE(_module_rtl871x_recv_c_, _drv_notice_, ("recv_func:bDriverStopped(%d) OR bSurpriseRemoved(%d)", padapter->bDriverStopped, padapter->bSurpriseRemoved));
			retval = _FAIL;
			return retval;
		}

	}

	return retval;
}

static int recv_func_prehandle(struct rtw_adapter *padapter, union recv_frame *rframe)
{
	int ret = _SUCCESS;
	struct rx_pkt_attrib *pattrib = &rframe->u.hdr.attrib;
	struct recv_priv *precvpriv = &padapter->recvpriv;
	_queue *pfree_recv_queue = &padapter->recvpriv.free_recv_queue;

	/* check the frame crtl field and decache */
	ret = validate_recv_frame(padapter, rframe);
	if (ret != _SUCCESS)
	{
		RT_TRACE(_module_rtl871x_recv_c_, _drv_info_, ("recv_func: validate_recv_frame fail! drop pkt\n"));
		rtw_free_recvframe(rframe, pfree_recv_queue);/* free this recv_frame */
		goto exit;
	}

exit:
	return ret;
}

static int recv_func_posthandle(struct rtw_adapter *padapter, union recv_frame *prframe)
{
	int ret = _SUCCESS;
	union recv_frame *orig_prframe = prframe;
	struct rx_pkt_attrib *pattrib = &prframe->u.hdr.attrib;
	struct recv_priv *precvpriv = &padapter->recvpriv;
	_queue *pfree_recv_queue = &padapter->recvpriv.free_recv_queue;

	/*  DATA FRAME */
	rtw_led_control(padapter, LED_CTL_RX);

	prframe = decryptor(padapter, prframe);
	if (prframe == NULL) {
		RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("decryptor: drop pkt\n"));
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s decryptor: drop pkt\n", __FUNCTION__);
		#endif
		ret = _FAIL;
		goto _recv_data_drop;
	}

	prframe = recvframe_chk_defrag(padapter, prframe);
	if(prframe==NULL)	{
		RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("recvframe_chk_defrag: drop pkt\n"));
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s recvframe_chk_defrag: drop pkt\n", __FUNCTION__);
		#endif
		goto _recv_data_drop;
	}

	prframe=portctrl(padapter, prframe);
	if (prframe == NULL) {
		RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("portctrl: drop pkt \n"));
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s portctrl: drop pkt\n", __FUNCTION__);
		#endif
		ret = _FAIL;
		goto _recv_data_drop;
	}

	count_rx_stats(padapter, prframe, NULL);

#ifdef CONFIG_80211N_HT
	ret = process_recv_indicatepkts(padapter, prframe);
	if (ret != _SUCCESS)
	{
		RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("recv_func: process_recv_indicatepkts fail! \n"));
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s process_recv_indicatepkts fail!\n", __FUNCTION__);
		#endif
		rtw_free_recvframe(orig_prframe, pfree_recv_queue);/* free this recv_frame */
		goto _recv_data_drop;
	}
#else /*  CONFIG_80211N_HT */
	if (!pattrib->amsdu)
	{
		ret = wlanhdr_to_ethhdr (prframe);
		if (ret != _SUCCESS)
		{
			RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("wlanhdr_to_ethhdr: drop pkt \n"));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s wlanhdr_to_ethhdr: drop pkt\n", __FUNCTION__);
			#endif
			rtw_free_recvframe(orig_prframe, pfree_recv_queue);/* free this recv_frame */
			goto _recv_data_drop;
		}

		if ((padapter->bDriverStopped == false) && (padapter->bSurpriseRemoved == false))
		{
			RT_TRACE(_module_rtl871x_recv_c_, _drv_alert_, ("@@@@ recv_func: recv_func rtw_recv_indicatepkt\n" ));
			/* indicate this recv_frame */
			ret = rtw_recv_indicatepkt(padapter, prframe);
			if (ret != _SUCCESS)
			{
				#ifdef DBG_RX_DROP_FRAME
				DBG_8723A("DBG_RX_DROP_FRAME %s rtw_recv_indicatepkt fail!\n", __FUNCTION__);
				#endif
				goto _recv_data_drop;
			}
		}
		else
		{
			RT_TRACE(_module_rtl871x_recv_c_, _drv_alert_, ("@@@@  recv_func: rtw_free_recvframe\n" ));
			RT_TRACE(_module_rtl871x_recv_c_, _drv_debug_, ("recv_func:bDriverStopped(%d) OR bSurpriseRemoved(%d)", padapter->bDriverStopped, padapter->bSurpriseRemoved));
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s ecv_func:bDriverStopped(%d) OR bSurpriseRemoved(%d)\n", __FUNCTION__,
				padapter->bDriverStopped, padapter->bSurpriseRemoved);
			#endif
			ret = _FAIL;
			rtw_free_recvframe(orig_prframe, pfree_recv_queue); /* free this recv_frame */
		}

	}
	else if(pattrib->amsdu==1)
	{

		ret = amsdu_to_msdu(padapter, prframe);
		if(ret != _SUCCESS)
		{
			#ifdef DBG_RX_DROP_FRAME
			DBG_8723A("DBG_RX_DROP_FRAME %s amsdu_to_msdu fail\n", __FUNCTION__);
			#endif
			rtw_free_recvframe(orig_prframe, pfree_recv_queue);
			goto _recv_data_drop;
		}
	}
	else
	{
		#ifdef DBG_RX_DROP_FRAME
		DBG_8723A("DBG_RX_DROP_FRAME %s what is this condition??\n", __FUNCTION__);
		#endif
		goto _recv_data_drop;
	}
#endif /*  CONFIG_80211N_HT */

_exit_recv_func:
	return ret;

_recv_data_drop:
	precvpriv->rx_drop++;
	return ret;
}

int recv_func(struct rtw_adapter *padapter, union recv_frame *rframe);
int recv_func(struct rtw_adapter *padapter, union recv_frame *rframe)
{
	int ret;
	struct rx_pkt_attrib *prxattrib = &rframe->u.hdr.attrib;
	struct recv_priv *recvpriv = &padapter->recvpriv;
	struct security_priv *psecuritypriv=&padapter->securitypriv;
	struct mlme_priv *mlmepriv = &padapter->mlmepriv;

	/* check if need to handle uc_swdec_pending_queue*/
	if (check_fwstate(mlmepriv, WIFI_STATION_STATE) && psecuritypriv->busetkipkey)
	{
		union recv_frame *pending_frame;

		while((pending_frame=rtw_alloc_recvframe(&padapter->recvpriv.uc_swdec_pending_queue))) {
			if (recv_func_posthandle(padapter, pending_frame) == _SUCCESS)
				DBG_8723A("%s: dequeue uc_swdec_pending_queue\n", __func__);
		}
	}

	ret = recv_func_prehandle(padapter, rframe);

	if(ret == _SUCCESS) {

		/* check if need to enqueue into uc_swdec_pending_queue*/
		if (check_fwstate(mlmepriv, WIFI_STATION_STATE) &&
			!is_multicast_ether_addr(prxattrib->ra) && prxattrib->encrypt>0 &&
			(prxattrib->bdecrypted == 0 ||psecuritypriv->sw_decrypt == true) &&
			!is_wep_enc(psecuritypriv->dot11PrivacyAlgrthm) &&
			!psecuritypriv->busetkipkey) {
			rtw_enqueue_recvframe(rframe, &padapter->recvpriv.uc_swdec_pending_queue);
			DBG_8723A("%s: no key, enqueue uc_swdec_pending_queue\n", __func__);
			goto exit;
		}

		ret = recv_func_posthandle(padapter, rframe);
	}

exit:
	return ret;
}

s32 rtw_recv_entry(union recv_frame *precvframe)
{
	struct rtw_adapter *padapter;
	struct recv_priv *precvpriv;
	s32 ret=_SUCCESS;

_func_enter_;

/*	RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("+rtw_recv_entry\n")); */

	padapter = precvframe->u.hdr.adapter;

	precvpriv = &padapter->recvpriv;

	if ((ret = recv_func(padapter, precvframe)) == _FAIL)
	{
		RT_TRACE(_module_rtl871x_recv_c_,_drv_info_,("rtw_recv_entry: recv_func return fail!!!\n"));
		goto _recv_entry_drop;
	}

	precvpriv->rx_pkts++;

_func_exit_;

	return ret;

_recv_entry_drop:

	/* RT_TRACE(_module_rtl871x_recv_c_,_drv_err_,("_recv_entry_drop\n")); */

_func_exit_;

	return ret;
}

void rtw_signal_stat_timer_hdl(unsigned long data)
{
	struct rtw_adapter *adapter = (struct rtw_adapter *)data;
	struct recv_priv *recvpriv = &adapter->recvpriv;

	u32 tmp_s, tmp_q;
	u8 avg_signal_strength = 0;
	u8 avg_signal_qual = 0;
	u32 num_signal_strength = 0;
	u32 num_signal_qual = 0;
	u8 _alpha = 3; /*  this value is based on converging_constant = 5000 and sampling_interval = 1000 */

	if(adapter->recvpriv.is_signal_dbg) {
		/* update the user specific value, signal_strength_dbg, to signal_strength, rssi */
		adapter->recvpriv.signal_strength= adapter->recvpriv.signal_strength_dbg;
		adapter->recvpriv.rssi=(s8)translate_percentage_to_dbm((u8)adapter->recvpriv.signal_strength_dbg);
	} else {

		if(recvpriv->signal_strength_data.update_req == 0) {/*  update_req is clear, means we got rx */
			avg_signal_strength = recvpriv->signal_strength_data.avg_val;
			num_signal_strength = recvpriv->signal_strength_data.total_num;
			/*  after avg_vals are accquired, we can re-stat the signal values */
			recvpriv->signal_strength_data.update_req = 1;
		}

		if(recvpriv->signal_qual_data.update_req == 0) {/*  update_req is clear, means we got rx */
			avg_signal_qual = recvpriv->signal_qual_data.avg_val;
			num_signal_qual = recvpriv->signal_qual_data.total_num;
			/*  after avg_vals are accquired, we can re-stat the signal values */
			recvpriv->signal_qual_data.update_req = 1;
		}

		/* update value of signal_strength, rssi, signal_qual */
		if(check_fwstate(&adapter->mlmepriv, _FW_UNDER_SURVEY) == false) {
			tmp_s = (avg_signal_strength+(_alpha-1)*recvpriv->signal_strength);
			if(tmp_s %_alpha)
				tmp_s = tmp_s/_alpha + 1;
			else
				tmp_s = tmp_s/_alpha;
			if(tmp_s>100)
				tmp_s = 100;

			tmp_q = (avg_signal_qual+(_alpha-1)*recvpriv->signal_qual);
			if(tmp_q %_alpha)
				tmp_q = tmp_q/_alpha + 1;
			else
				tmp_q = tmp_q/_alpha;
			if(tmp_q>100)
				tmp_q = 100;

			recvpriv->signal_strength = tmp_s;
			recvpriv->rssi = (s8)translate_percentage_to_dbm(tmp_s);
			recvpriv->signal_qual = tmp_q;

			#if defined(DBG_RX_SIGNAL_DISPLAY_PROCESSING) && 1
			DBG_8723A("%s signal_strength:%3u, rssi:%3d, signal_qual:%3u"
				", num_signal_strength:%u, num_signal_qual:%u"
				"\n"
				, __FUNCTION__
				, recvpriv->signal_strength
				, recvpriv->rssi
				, recvpriv->signal_qual
				, num_signal_strength, num_signal_qual
			);
			#endif
		}
	}
	rtw_set_signal_stat_timer(recvpriv);
}
