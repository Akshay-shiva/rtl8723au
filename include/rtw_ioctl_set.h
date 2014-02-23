/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
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
 ******************************************************************************/
#ifndef __RTW_IOCTL_SET_H_
#define __RTW_IOCTL_SET_H_

#include <drv_types.h>


typedef u8 NDIS_802_11_PMKID_VALUE[16];

typedef struct _BSSIDInfo {
	NDIS_802_11_MAC_ADDRESS  BSSID;
	NDIS_802_11_PMKID_VALUE  PMKID;
} BSSIDInfo, *PBSSIDInfo;


u8 rtw_set_802_11_add_key(struct rtw_adapter *padapter,
			  struct ndis_802_11_key *key);
u8 rtw_set_802_11_authentication_mode(struct rtw_adapter *pdapter,
				      enum ndis_802_11_auth_mode authmode);
u8 rtw_set_802_11_bssid(struct rtw_adapter *padapter, u8 *bssid);
u8 rtw_set_802_11_add_wep(struct rtw_adapter * padapter,
			  struct ndis_802_11_wep *wep);
u8 rtw_set_802_11_disassociate(struct rtw_adapter *padapter);
u8 rtw_set_802_11_bssid_list_scan(struct rtw_adapter *padapter,
				  struct ndis_802_11_ssid *pssid, int ssid_max_num);
u8 rtw_set_802_11_infrastructure_mode(struct rtw_adapter *padapter,
				      enum ndis_802_11_net_infra networktype);
u8 rtw_set_802_11_remove_wep(struct rtw_adapter * padapter, u32 keyindex);
u8 rtw_set_802_11_ssid(struct rtw_adapter * padapter, struct ndis_802_11_ssid * ssid);

u8 rtw_validate_ssid(struct ndis_802_11_ssid *ssid);

u16 rtw_get_cur_max_rate(struct rtw_adapter *adapter);
void rtw_indicate_wx_assoc_event(struct rtw_adapter *padapter);
void rtw_indicate_wx_disassoc_event(struct rtw_adapter *padapter);
void indicate_wx_scan_complete_event(struct rtw_adapter *padapter);
s32 FillH2CCmd(struct rtw_adapter *padapter, u8 ElementID, u32 CmdLen, u8 *pCmdBuffer);

#endif
