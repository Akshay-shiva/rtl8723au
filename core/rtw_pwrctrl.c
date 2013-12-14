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
#define _RTW_PWRCTRL_C_

#include <drv_conf.h>
#include <osdep_service.h>
#include <drv_types.h>
#include <osdep_intf.h>

#ifdef CONFIG_BT_COEXIST
#include <rtl8723a_hal.h>
#endif

#ifdef CONFIG_IPS
void ips_enter(struct rtw_adapter * padapter)
{
	struct pwrctrl_priv *pwrpriv = &padapter->pwrctrlpriv;

	down(&pwrpriv->lock);

	pwrpriv->bips_processing = true;

	/*  syn ips_mode with request */
	pwrpriv->ips_mode = pwrpriv->ips_mode_req;

	pwrpriv->ips_enter_cnts++;
	DBG_8723A("==>ips_enter cnts:%d\n",pwrpriv->ips_enter_cnts);
#ifdef CONFIG_BT_COEXIST
	BTDM_TurnOffBtCoexistBeforeEnterIPS(padapter);
#endif
	if(rf_off == pwrpriv->change_rfpwrstate )
	{
		pwrpriv->bpower_saving = true;
		DBG_8723A_LEVEL(_drv_always_, "nolinked power save enter\n");

		if(pwrpriv->ips_mode == IPS_LEVEL_2)
			pwrpriv->bkeepfwalive = true;

		rtw_ips_pwr_down(padapter);
		pwrpriv->rf_pwrstate = rf_off;
	}
	pwrpriv->bips_processing = false;

	up(&pwrpriv->lock);
}

int ips_leave(struct rtw_adapter * padapter)
{
	struct pwrctrl_priv *pwrpriv = &padapter->pwrctrlpriv;
	struct security_priv* psecuritypriv=&(padapter->securitypriv);
	struct mlme_priv		*pmlmepriv = &(padapter->mlmepriv);
	int result = _SUCCESS;
	int keyid;

	down(&pwrpriv->lock);

	if((pwrpriv->rf_pwrstate == rf_off) &&(!pwrpriv->bips_processing))
	{
		pwrpriv->bips_processing = true;
		pwrpriv->change_rfpwrstate = rf_on;
		pwrpriv->ips_leave_cnts++;
		DBG_8723A("==>ips_leave cnts:%d\n",pwrpriv->ips_leave_cnts);

		if ((result = rtw_ips_pwr_up(padapter)) == _SUCCESS) {
			pwrpriv->rf_pwrstate = rf_on;
		}
		DBG_8723A_LEVEL(_drv_always_, "nolinked power save leave\n");

		if((_WEP40_ == psecuritypriv->dot11PrivacyAlgrthm) ||(_WEP104_ == psecuritypriv->dot11PrivacyAlgrthm))
		{
			DBG_8723A("==>%s,channel(%d),processing(%x)\n",__FUNCTION__,padapter->mlmeextpriv.cur_channel,pwrpriv->bips_processing);
			set_channel_bwmode(padapter, padapter->mlmeextpriv.cur_channel, HAL_PRIME_CHNL_OFFSET_DONT_CARE, HT_CHANNEL_WIDTH_20);
			for(keyid=0;keyid<4;keyid++){
				if(pmlmepriv->key_mask & BIT(keyid)){
					if(keyid == psecuritypriv->dot11PrivacyKeyIndex)
						result=rtw_set_key(padapter,psecuritypriv, keyid, 1);
					else
						result=rtw_set_key(padapter,psecuritypriv, keyid, 0);
				}
			}
		}

		DBG_8723A("==> ips_leave.....LED(0x%08x)...\n",rtw_read32(padapter,0x4c));
		pwrpriv->bips_processing = false;

		pwrpriv->bkeepfwalive = false;
		pwrpriv->bpower_saving = false;
	}

	up(&pwrpriv->lock);

	return result;
}

#endif

#ifdef CONFIG_AUTOSUSPEND
extern void autosuspend_enter(struct rtw_adapter* padapter);
extern int autoresume_enter(struct rtw_adapter* padapter);
#endif

#ifdef SUPPORT_HW_RFOFF_DETECTED
int rtw_hw_suspend(struct rtw_adapter *padapter );
int rtw_hw_resume(struct rtw_adapter *padapter);
#endif

static bool rtw_pwr_unassociated_idle(struct rtw_adapter *adapter)
{
	struct rtw_adapter *buddy = adapter->pbuddy_adapter;
	struct mlme_priv *pmlmepriv = &(adapter->mlmepriv);
	struct xmit_priv *pxmit_priv = &adapter->xmitpriv;
#ifdef CONFIG_P2P
	struct wifidirect_info	*pwdinfo = &(adapter->wdinfo);
	struct cfg80211_wifidirect_info *pcfg80211_wdinfo = &adapter->cfg80211_wdinfo;
#endif

	bool ret = false;

	if (adapter->pwrctrlpriv.ips_deny_time >= rtw_get_current_time())
		goto exit;

	if (check_fwstate(pmlmepriv, WIFI_ASOC_STATE|WIFI_SITE_MONITOR)
		|| check_fwstate(pmlmepriv, WIFI_UNDER_LINKING|WIFI_UNDER_WPS)
		|| check_fwstate(pmlmepriv, WIFI_AP_STATE)
		|| check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE|WIFI_ADHOC_STATE)
#if defined(CONFIG_P2P) && defined(CONFIG_P2P_IPS)
		|| pcfg80211_wdinfo->is_ro_ch
#elif defined(CONFIG_P2P)
		|| !rtw_p2p_chk_state(pwdinfo, P2P_STATE_NONE)
#endif
	) {
		goto exit;
	}

	/* consider buddy, if exist */
	if (buddy) {
		struct mlme_priv *b_pmlmepriv = &(buddy->mlmepriv);
#ifdef CONFIG_P2P
		struct wifidirect_info *b_pwdinfo = &(buddy->wdinfo);
		struct cfg80211_wifidirect_info *b_pcfg80211_wdinfo = &buddy->cfg80211_wdinfo;
#endif

		if (check_fwstate(b_pmlmepriv, WIFI_ASOC_STATE|WIFI_SITE_MONITOR)
			|| check_fwstate(b_pmlmepriv, WIFI_UNDER_LINKING|WIFI_UNDER_WPS)
			|| check_fwstate(b_pmlmepriv, WIFI_AP_STATE)
			|| check_fwstate(b_pmlmepriv, WIFI_ADHOC_MASTER_STATE|WIFI_ADHOC_STATE)
#if defined(CONFIG_P2P) && defined(CONFIG_P2P_IPS)
			|| b_pcfg80211_wdinfo->is_ro_ch
#elif defined(CONFIG_P2P)
			|| !rtw_p2p_chk_state(b_pwdinfo, P2P_STATE_NONE)
#endif
		) {
			goto exit;
		}
	}

	if (pxmit_priv->free_xmitbuf_cnt != NR_XMITBUFF ||
		pxmit_priv->free_xmit_extbuf_cnt != NR_XMIT_EXTBUFF) {
		DBG_8723A_LEVEL(_drv_always_, "There are some pkts to transmit\n");
		DBG_8723A_LEVEL(_drv_info_, "free_xmitbuf_cnt: %d, free_xmit_extbuf_cnt: %d\n",
			pxmit_priv->free_xmitbuf_cnt, pxmit_priv->free_xmit_extbuf_cnt);
		goto exit;
	}

	ret = true;

exit:
	return ret;
}

void rtw_ps_processor(struct rtw_adapter*padapter)
{
#ifdef CONFIG_P2P
	struct wifidirect_info	*pwdinfo = &( padapter->wdinfo );
#endif /* CONFIG_P2P */
	struct pwrctrl_priv *pwrpriv = &padapter->pwrctrlpriv;
	struct mlme_priv *pmlmepriv = &(padapter->mlmepriv);
#ifdef SUPPORT_HW_RFOFF_DETECTED
	rt_rf_power_state rfpwrstate;
#endif /* SUPPORT_HW_RFOFF_DETECTED */

	pwrpriv->ps_processing = true;

#ifdef SUPPORT_HW_RFOFF_DETECTED
	if(pwrpriv->bips_processing == true)
		goto exit;

	if(padapter->pwrctrlpriv.bHWPwrPindetect)
	{
	#ifdef CONFIG_AUTOSUSPEND
		if(padapter->registrypriv.usbss_enable)
		{
			if(pwrpriv->rf_pwrstate == rf_on)
			{
				if(padapter->net_closed == true)
					pwrpriv->ps_flag = true;

				rfpwrstate = RfOnOffDetect(padapter);
				DBG_8723A("@@@@- #1  %s==> rfstate:%s \n",__FUNCTION__,(rfpwrstate==rf_on)?"rf_on":"rf_off");
				if(rfpwrstate!= pwrpriv->rf_pwrstate)
				{
					if(rfpwrstate == rf_off)
					{
						pwrpriv->change_rfpwrstate = rf_off;

						pwrpriv->bkeepfwalive = true;
						pwrpriv->brfoffbyhw = true;

						autosuspend_enter(padapter);
					}
				}
			}
		}
		else
	#endif /* CONFIG_AUTOSUSPEND */
		{
			rfpwrstate = RfOnOffDetect(padapter);
			DBG_8723A("@@@@- #2  %s==> rfstate:%s \n",__FUNCTION__,(rfpwrstate==rf_on)?"rf_on":"rf_off");

			if(rfpwrstate!= pwrpriv->rf_pwrstate)
			{
				if(rfpwrstate == rf_off)
				{
					pwrpriv->change_rfpwrstate = rf_off;
					pwrpriv->brfoffbyhw = true;
					padapter->bCardDisableWOHSM = true;
					rtw_hw_suspend(padapter );
				}
				else
				{
					pwrpriv->change_rfpwrstate = rf_on;
					rtw_hw_resume(padapter );
				}
				DBG_8723A("current rf_pwrstate(%s)\n",(pwrpriv->rf_pwrstate == rf_off)?"rf_off":"rf_on");
			}
		}
		pwrpriv->pwr_state_check_cnts ++;
	}
#endif /* SUPPORT_HW_RFOFF_DETECTED */

	if (pwrpriv->ips_mode_req == IPS_NONE)
		goto exit;

	if (rtw_pwr_unassociated_idle(padapter) == false)
		goto exit;

	if((pwrpriv->rf_pwrstate == rf_on) && ((pwrpriv->pwr_state_check_cnts%4)==0))
	{
		DBG_8723A("==>%s .fw_state(%x)\n",__FUNCTION__,get_fwstate(pmlmepriv));
		#if defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND)
		#else
		pwrpriv->change_rfpwrstate = rf_off;
		#endif
		#ifdef CONFIG_AUTOSUSPEND
		if(padapter->registrypriv.usbss_enable)
		{
			if(pwrpriv->bHWPwrPindetect)
				pwrpriv->bkeepfwalive = true;

			if(padapter->net_closed == true)
				pwrpriv->ps_flag = true;

			#if defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND)
			if (true==pwrpriv->bInternalAutoSuspend) {
				DBG_8723A("<==%s .pwrpriv->bInternalAutoSuspend)(%x)\n",__FUNCTION__,pwrpriv->bInternalAutoSuspend);
			} else {
				pwrpriv->change_rfpwrstate = rf_off;
				padapter->bCardDisableWOHSM = true;
				DBG_8723A("<==%s .pwrpriv->bInternalAutoSuspend)(%x) call autosuspend_enter\n",__FUNCTION__,pwrpriv->bInternalAutoSuspend);
				autosuspend_enter(padapter);
			}
			#else
			padapter->bCardDisableWOHSM = true;
			autosuspend_enter(padapter);
			#endif	/* if defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND) */
		}
		else if(pwrpriv->bHWPwrPindetect)
		{
		}
		else
		#endif /* CONFIG_AUTOSUSPEND */
		{
			#if defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND)
			pwrpriv->change_rfpwrstate = rf_off;
			#endif	/* defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND) */

			#ifdef CONFIG_IPS
			ips_enter(padapter);
			#endif
		}
	}
exit:
	rtw_set_pwr_state_check_timer(&padapter->pwrctrlpriv);
	pwrpriv->ps_processing = false;
	return;
}

void pwr_state_check_handler(void *FunctionContext);
void pwr_state_check_handler(void *FunctionContext)
{
	struct rtw_adapter *padapter = (struct rtw_adapter *)FunctionContext;
	rtw_ps_cmd(padapter);
}

#ifdef CONFIG_LPS
/*
 *
 * Parameters
 *	padapter
 *	pslv			power state level, only could be PS_STATE_S0 ~ PS_STATE_S4
 *
 */
void rtw_set_rpwm(struct rtw_adapter *padapter, u8 pslv)
{
	u8	rpwm;
	struct pwrctrl_priv *pwrpriv = &padapter->pwrctrlpriv;

_func_enter_;

	pslv = PS_STATE(pslv);

	if (true == pwrpriv->btcoex_rfon)
	{
		if (pslv < PS_STATE_S4)
			pslv = PS_STATE_S3;
	}

#ifdef CONFIG_LPS_RPWM_TIMER
	if (pwrpriv->brpwmtimeout == true)
	{
		DBG_8723A("%s: RPWM timeout, force to set RPWM(0x%02X) again!\n", __FUNCTION__, pslv);
	}
	else
#endif /*  CONFIG_LPS_RPWM_TIMER */
	{
	if ( (pwrpriv->rpwm == pslv)
#ifdef CONFIG_LPS_LCLK
		|| ((pwrpriv->rpwm >= PS_STATE_S2)&&(pslv >= PS_STATE_S2))
#endif
		)
	{
		RT_TRACE(_module_rtl871x_pwrctrl_c_,_drv_err_,
			("%s: Already set rpwm[0x%02X], new=0x%02X!\n", __FUNCTION__, pwrpriv->rpwm, pslv));
		return;
	}
	}

	if ((padapter->bSurpriseRemoved == true) ||
		(padapter->hw_init_completed == false))
	{
		RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_err_,
				 ("%s: SurpriseRemoved(%d) hw_init_completed(%d)\n",
				  __FUNCTION__, padapter->bSurpriseRemoved, padapter->hw_init_completed));

		pwrpriv->cpwm = PS_STATE_S4;

		return;
	}

	if (padapter->bDriverStopped == true)
	{
		RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_err_,
				 ("%s: change power state(0x%02X) when DriverStopped\n", __FUNCTION__, pslv));

		if (pslv < PS_STATE_S2) {
			RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_err_,
					 ("%s: Reject to enter PS_STATE(0x%02X) lower than S2 when DriverStopped!!\n", __FUNCTION__, pslv));
			return;
		}
	}

	rpwm = pslv | pwrpriv->tog;
#ifdef CONFIG_LPS_LCLK
	/*  only when from PS_STATE S0/S1 to S2 and higher needs ACK */
	if ((pwrpriv->cpwm < PS_STATE_S2) && (pslv >= PS_STATE_S2))
		rpwm |= PS_ACK;
#endif
	RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
			 ("rtw_set_rpwm: rpwm=0x%02x cpwm=0x%02x\n", rpwm, pwrpriv->cpwm));

	pwrpriv->rpwm = pslv;

#ifdef CONFIG_LPS_RPWM_TIMER
	if (rpwm & PS_ACK)
		_set_timer(&pwrpriv->pwr_rpwm_timer, LPS_RPWM_WAIT_MS);
#endif /*  CONFIG_LPS_RPWM_TIMER */
	rtw_hal_set_hwreg(padapter, HW_VAR_SET_RPWM, (u8 *)(&rpwm));

	pwrpriv->tog += 0x80;

#ifdef CONFIG_LPS_LCLK
	/*  No LPS 32K, No Ack */
	if (!(rpwm & PS_ACK))
#endif
	{
		pwrpriv->cpwm = pslv;
	}

_func_exit_;
}

u8 PS_RDY_CHECK(struct rtw_adapter * padapter);
u8 PS_RDY_CHECK(struct rtw_adapter * padapter)
{
	u32 curr_time, delta_time;
	struct pwrctrl_priv	*pwrpriv = &padapter->pwrctrlpriv;
	struct mlme_priv	*pmlmepriv = &(padapter->mlmepriv);

	curr_time = rtw_get_current_time();
	delta_time = curr_time -pwrpriv->DelayLPSLastTimeStamp;

	if(delta_time < LPS_DELAY_TIME)
	{
		return false;
	}

	if ((check_fwstate(pmlmepriv, _FW_LINKED) == false) ||
		(check_fwstate(pmlmepriv, _FW_UNDER_SURVEY) == true) ||
		(check_fwstate(pmlmepriv, WIFI_AP_STATE) == true) ||
		(check_fwstate(pmlmepriv, WIFI_ADHOC_MASTER_STATE) == true) ||
		(check_fwstate(pmlmepriv, WIFI_ADHOC_STATE) == true) )
		return false;
#ifdef CONFIG_WOWLAN
	if(true == pwrpriv->bInSuspend && pwrpriv->wowlan_mode)
		return true;
	else
		return false;
#else
	if(true == pwrpriv->bInSuspend )
		return false;
#endif
	if( (padapter->securitypriv.dot11AuthAlgrthm == dot11AuthAlgrthm_8021X) && (padapter->securitypriv.binstallGrpkey == false) )
	{
		DBG_8723A("Group handshake still in progress !!!\n");
		return false;
	}
	if (!rtw_cfg80211_pwr_mgmt(padapter))
		return false;

	return true;
}

void rtw_set_ps_mode(struct rtw_adapter *padapter, u8 ps_mode, u8 smart_ps, u8 bcn_ant_mode)
{
	struct pwrctrl_priv *pwrpriv = &padapter->pwrctrlpriv;
#ifdef CONFIG_P2P
	struct wifidirect_info	*pwdinfo = &( padapter->wdinfo );
#endif /* CONFIG_P2P */
#ifdef CONFIG_TDLS
	struct sta_priv *pstapriv = &padapter->stapriv;
	_irqL irqL;
	int i, j;
	_list	*plist, *phead;
	struct sta_info *ptdls_sta;
#endif /* CONFIG_TDLS */

_func_enter_;

	RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
			 ("%s: PowerMode=%d Smart_PS=%d\n",
			  __FUNCTION__, ps_mode, smart_ps));

	if(ps_mode > PM_Card_Disable) {
		RT_TRACE(_module_rtl871x_pwrctrl_c_,_drv_err_,("ps_mode:%d error\n", ps_mode));
		return;
	}

	if (pwrpriv->pwr_mode == ps_mode)
	{
		if (PS_MODE_ACTIVE == ps_mode) return;

		if ((pwrpriv->smart_ps == smart_ps) &&
			(pwrpriv->bcn_ant_mode == bcn_ant_mode))
		{
			return;
		}
	}

#ifdef CONFIG_LPS_LCLK
	down(&pwrpriv->lock);
#endif

	/* if(pwrpriv->pwr_mode == PS_MODE_ACTIVE) */
	if(ps_mode == PS_MODE_ACTIVE)
	{
#ifdef CONFIG_P2P_PS
		if(pwdinfo->opp_ps == 0)
#endif /* CONFIG_P2P_PS */
		{
			DBG_8723A("rtw_set_ps_mode: Leave 802.11 power save\n");

#ifdef CONFIG_TDLS
			spin_lock_bh(&pstapriv->sta_hash_lock);

			for(i=0; i< NUM_STA; i++)
			{
				phead = &(pstapriv->sta_hash[i]);
				plist = phead->next;

				while ((rtw_end_of_queue_search(phead, plist)) == false)
				{
					ptdls_sta = container_of(plist, struct sta_info, hash_list);

					if( ptdls_sta->tdls_sta_state & TDLS_LINKED_STATE )
						issue_nulldata_to_TDLS_peer_STA(padapter, ptdls_sta, 0);
					plist = plist->next;
				}
			}

			spin_unlock_bh(&pstapriv->sta_hash_lock);
#endif /* CONFIG_TDLS */

			pwrpriv->pwr_mode = ps_mode;
			rtw_set_rpwm(padapter, PS_STATE_S4);
			rtw_hal_set_hwreg(padapter, HW_VAR_H2C_FW_PWRMODE, (u8 *)(&ps_mode));
			pwrpriv->bFwCurrentInPSMode = false;
		}
	}
	else
	{
		if (PS_RDY_CHECK(padapter)
#ifdef CONFIG_BT_COEXIST
			|| (BT_1Ant(padapter) == true)
#endif
			)
		{
			DBG_8723A("%s: Enter 802.11 power save\n", __FUNCTION__);

#ifdef CONFIG_TDLS
			spin_lock_bh(&pstapriv->sta_hash_lock);

			for(i=0; i< NUM_STA; i++)
			{
				phead = &(pstapriv->sta_hash[i]);
				plist = phead->next;

				while ((rtw_end_of_queue_search(phead, plist)) == false)
				{
					ptdls_sta = container_of(plist, struct sta_info, hash_list);

					if( ptdls_sta->tdls_sta_state & TDLS_LINKED_STATE )
						issue_nulldata_to_TDLS_peer_STA(padapter, ptdls_sta, 1);
					plist = plist->next;
				}
			}

			spin_unlock_bh(&pstapriv->sta_hash_lock);
#endif /* CONFIG_TDLS */

			pwrpriv->bFwCurrentInPSMode = true;
			pwrpriv->pwr_mode = ps_mode;
			pwrpriv->smart_ps = smart_ps;
			pwrpriv->bcn_ant_mode = bcn_ant_mode;
			rtw_hal_set_hwreg(padapter, HW_VAR_H2C_FW_PWRMODE, (u8 *)(&ps_mode));

#ifdef CONFIG_P2P_PS
			/*  Set CTWindow after LPS */
			if(pwdinfo->opp_ps == 1)
				p2p_ps_wk_cmd(padapter, P2P_PS_ENABLE, 0);
#endif /* CONFIG_P2P_PS */

#ifdef CONFIG_LPS_LCLK
			if (pwrpriv->alives == 0)
				rtw_set_rpwm(padapter, PS_STATE_S0);
#else
			rtw_set_rpwm(padapter, PS_STATE_S2);
#endif
		}
	}

#ifdef CONFIG_LPS_LCLK
	up(&pwrpriv->lock);
#endif

_func_exit_;
}

/*
 * Return:
 *	0:	Leave OK
 *	-1:	Timeout
 *	-2:	Other error
 */
s32 LPS_RF_ON_check(struct rtw_adapter *padapter, u32 delay_ms)
{
	u32 start_time;
	u8 bAwake = false;
	s32 err = 0;

	start_time = rtw_get_current_time();
	while (1)
	{
		rtw_hal_get_hwreg(padapter, HW_VAR_FWLPS_RF_ON, &bAwake);
		if (true == bAwake)
			break;

		if (true == padapter->bSurpriseRemoved)
		{
			err = -2;
			DBG_8723A("%s: device surprise removed!!\n", __FUNCTION__);
			break;
		}

		if (rtw_get_passing_time_ms(start_time) > delay_ms)
		{
			err = -1;
			DBG_8723A("%s: Wait for FW LPS leave more than %u ms!!!\n", __FUNCTION__, delay_ms);
			break;
		}
		rtw_usleep_os(100);
	}

	return err;
}

/*  */
/*	Description: */
/*		Enter the leisure power save mode. */
/*  */
void LPS_Enter(struct rtw_adapter *padapter)
{
	struct pwrctrl_priv	*pwrpriv = &padapter->pwrctrlpriv;
	struct mlme_priv	*pmlmepriv = &(padapter->mlmepriv);
	struct rtw_adapter *buddy = padapter->pbuddy_adapter;

_func_enter_;

/*	DBG_8723A("+LeisurePSEnter\n"); */

	if (PS_RDY_CHECK(padapter) == false)
		return;

	if (true == pwrpriv->bLeisurePs)
	{
		/*  Idle for a while if we connect to AP a while ago. */
		if(pwrpriv->LpsIdleCount >= 2) /*   4 Sec */
		{
			if(pwrpriv->pwr_mode == PS_MODE_ACTIVE)
			{
				pwrpriv->bpower_saving = true;
				DBG_8723A("%s smart_ps:%d\n", __func__, pwrpriv->smart_ps);
				/* For Tenda W311R IOT issue */
				rtw_set_ps_mode(padapter, pwrpriv->power_mgnt, pwrpriv->smart_ps, 0);
			}
		}
		else
			pwrpriv->LpsIdleCount++;
	}

/*	DBG_8723A("-LeisurePSEnter\n"); */

_func_exit_;
}

/*  */
/*	Description: */
/*		Leave the leisure power save mode. */
/*  */
void LPS_Leave(struct rtw_adapter *padapter)
{
#define LPS_LEAVE_TIMEOUT_MS 100

	struct pwrctrl_priv	*pwrpriv = &padapter->pwrctrlpriv;
	u32 start_time;
	u8 bAwake = false;

_func_enter_;

	if (pwrpriv->bLeisurePs) {
		if(pwrpriv->pwr_mode != PS_MODE_ACTIVE) {
			rtw_set_ps_mode(padapter, PS_MODE_ACTIVE, 0, 0);

			if(pwrpriv->pwr_mode == PS_MODE_ACTIVE)
				LPS_RF_ON_check(padapter, LPS_LEAVE_TIMEOUT_MS);
		}
	}

	pwrpriv->bpower_saving = false;

/*	DBG_8723A("-LeisurePSLeave\n"); */

_func_exit_;
}
#endif

/*  */
/*  Description: Leave all power save mode: LPS, FwLPS, IPS if needed. */
/*  Move code to function by tynli. 2010.03.26. */
/*  */
void LeaveAllPowerSaveMode(struct rtw_adapter *Adapter)
{
	struct mlme_priv	*pmlmepriv = &(Adapter->mlmepriv);
	u8	enqueue = 0;

_func_enter_;

	/* DBG_8723A("%s.....\n",__FUNCTION__); */
	if (check_fwstate(pmlmepriv, _FW_LINKED) == true)
	{ /* connect */
#ifdef CONFIG_LPS_LCLK
		enqueue = 1;
#endif

#ifdef CONFIG_P2P_PS
		p2p_ps_wk_cmd(Adapter, P2P_PS_DISABLE, enqueue);
#endif /* CONFIG_P2P_PS */

#ifdef CONFIG_LPS
		rtw_lps_ctrl_wk_cmd(Adapter, LPS_CTRL_LEAVE, enqueue);
#endif

#ifdef CONFIG_LPS_LCLK
		LPS_Leave_check(Adapter);
#endif
	}
	else
	{
		if(Adapter->pwrctrlpriv.rf_pwrstate== rf_off)
		{
			#ifdef CONFIG_AUTOSUSPEND
			if(Adapter->registrypriv.usbss_enable) {
				usb_disable_autosuspend(adapter_to_dvobj(Adapter)->pusbdev);
			}
			else
			#endif
			{
			}
		}
	}

_func_exit_;
}

#ifdef CONFIG_LPS_LCLK
void LPS_Leave_check(
	PADAPTER padapter)
{
	struct pwrctrl_priv *pwrpriv;
	u32	start_time;
	u8	bReady;

_func_enter_;

	pwrpriv = &padapter->pwrctrlpriv;

	bReady = false;
	start_time = rtw_get_current_time();

	yield();

	while(1)
	{
		down(&pwrpriv->lock);

		if ((padapter->bSurpriseRemoved == true)
			|| (padapter->hw_init_completed == false)
			|| (padapter->bDriverStopped== true)
			|| (pwrpriv->pwr_mode == PS_MODE_ACTIVE)
			)
		{
			bReady = true;
		}

		up(&pwrpriv->lock);

		if(true == bReady)
			break;

		if(rtw_get_passing_time_ms(start_time)>100)
		{
			DBG_8723A("Wait for cpwm event  than 100 ms!!!\n");
			break;
		}
		msleep(1);
	}

_func_exit_;
}

/*
 * Caller:ISR handler...
 *
 * This will be called when CPWM interrupt is up.
 *
 * using to update cpwn of drv; and drv willl make a decision to up or down pwr level
 */
void cpwm_int_hdl(
	PADAPTER padapter,
	struct reportpwrstate_parm *preportpwrstate)
{
	struct pwrctrl_priv *pwrpriv;

_func_enter_;

	pwrpriv = &padapter->pwrctrlpriv;

	down(&pwrpriv->lock);

#ifdef CONFIG_LPS_RPWM_TIMER
	if (pwrpriv->rpwm < PS_STATE_S2)
	{
		DBG_8723A("%s: Redundant CPWM Int. RPWM=0x%02X CPWM=0x%02x\n", __func__, pwrpriv->rpwm, pwrpriv->cpwm);
		up(&pwrpriv->lock);
		goto exit;
	}
#endif /*  CONFIG_LPS_RPWM_TIMER */

	pwrpriv->cpwm = PS_STATE(preportpwrstate->state);
	pwrpriv->cpwm_tog = preportpwrstate->state & PS_TOGGLE;

	if (pwrpriv->cpwm >= PS_STATE_S2)
	{
		if (pwrpriv->alives & CMD_ALIVE)
			up(&padapter->cmdpriv.cmd_queue_sema);

		if (pwrpriv->alives & XMIT_ALIVE)
			up(&padapter->xmitpriv.xmit_sema);
	}

	up(&pwrpriv->lock);

exit:
	RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
			 ("cpwm_int_hdl: cpwm=0x%02x\n", pwrpriv->cpwm));

_func_exit_;
}

static void cpwm_event_callback(struct work_struct *work)
{
	struct pwrctrl_priv *pwrpriv = container_of(work, struct pwrctrl_priv, cpwm_event);
	struct rtw_adapter *adapter = container_of(pwrpriv, struct rtw_adapter, pwrctrlpriv);
	struct reportpwrstate_parm report;

	/* DBG_8723A("%s\n",__FUNCTION__); */

	report.state = PS_STATE_S2;
	cpwm_int_hdl(adapter, &report);
}

#ifdef CONFIG_LPS_RPWM_TIMER
static void rpwmtimeout_workitem_callback(struct work_struct *work)
{
	PADAPTER padapter;
	struct pwrctrl_priv *pwrpriv;

	pwrpriv = container_of(work, struct pwrctrl_priv, rpwmtimeoutwi);
	padapter = container_of(pwrpriv, struct rtw_adapter, pwrctrlpriv);
/*	DBG_8723A("+%s: rpwm=0x%02X cpwm=0x%02X\n", __func__, pwrpriv->rpwm, pwrpriv->cpwm); */

	down(&pwrpriv->lock);
	if ((pwrpriv->rpwm == pwrpriv->cpwm) || (pwrpriv->cpwm >= PS_STATE_S2))
	{
		DBG_8723A("%s: rpwm=0x%02X cpwm=0x%02X CPWM done!\n", __func__, pwrpriv->rpwm, pwrpriv->cpwm);
		goto exit;
	}
	up(&pwrpriv->lock);

	if (rtw_read8(padapter, 0x100) != 0xEA)
	{
		struct reportpwrstate_parm report;

		report.state = PS_STATE_S2;
		DBG_8723A("\n%s: FW already leave 32K!\n\n", __func__);
		cpwm_int_hdl(padapter, &report);
		return;
	}

	down(&pwrpriv->lock);

	if ((pwrpriv->rpwm == pwrpriv->cpwm) || (pwrpriv->cpwm >= PS_STATE_S2))
	{
		DBG_8723A("%s: cpwm=%d, nothing to do!\n", __func__, pwrpriv->cpwm);
		goto exit;
	}
	pwrpriv->brpwmtimeout = true;
	rtw_set_rpwm(padapter, pwrpriv->rpwm);
	pwrpriv->brpwmtimeout = false;

exit:
	up(&pwrpriv->lock);
}

/*
 * This function is a timer handler, can't do any IO in it.
 */
static void pwr_rpwm_timeout_handler(void *FunctionContext)
{
	PADAPTER padapter;
	struct pwrctrl_priv *pwrpriv;

	padapter = (PADAPTER)FunctionContext;
	pwrpriv = &padapter->pwrctrlpriv;
/*	DBG_8723A("+%s: rpwm=0x%02X cpwm=0x%02X\n", __func__, pwrpriv->rpwm, pwrpriv->cpwm); */

	if ((pwrpriv->rpwm == pwrpriv->cpwm) || (pwrpriv->cpwm >= PS_STATE_S2))
	{
		DBG_8723A("+%s: cpwm=%d, nothing to do!\n", __func__, pwrpriv->cpwm);
		return;
	}

	schedule_work(&pwrpriv->rpwmtimeoutwi);
}
#endif /*  CONFIG_LPS_RPWM_TIMER */

static inline void register_task_alive(struct pwrctrl_priv *pwrctrl, u32 tag)
{
	pwrctrl->alives |= tag;
}

static inline void unregister_task_alive(struct pwrctrl_priv *pwrctrl, u32 tag)
{
	pwrctrl->alives &= ~tag;
}

/*
 * Caller: rtw_xmit_thread
 *
 * Check if the fw_pwrstate is okay for xmit.
 * If not (cpwm is less than S3), then the sub-routine
 * will raise the cpwm to be greater than or equal to S3.
 *
 * Calling Context: Passive
 *
 * Return Value:
 *	 _SUCCESS	rtw_xmit_thread can write fifo/txcmd afterwards.
 *	 _FAIL		rtw_xmit_thread can not do anything.
 */
s32 rtw_register_tx_alive(struct rtw_adapter *padapter)
{
	s32 res;
	struct pwrctrl_priv *pwrctrl;
	u8 pslv;

_func_enter_;

	res = _SUCCESS;
	pwrctrl = &padapter->pwrctrlpriv;
#ifdef CONFIG_BT_COEXIST
	if (true == padapter->pwrctrlpriv.btcoex_rfon)
		pslv = PS_STATE_S3;
	else
#endif
	{
		pslv = PS_STATE_S2;
	}

	down(&pwrctrl->lock);

	register_task_alive(pwrctrl, XMIT_ALIVE);

	if (pwrctrl->bFwCurrentInPSMode == true)
	{
		RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
				 ("rtw_register_tx_alive: cpwm=0x%02x alives=0x%08x\n",
				  pwrctrl->cpwm, pwrctrl->alives));

		if (pwrctrl->cpwm < pslv)
		{
			if (pwrctrl->cpwm < PS_STATE_S2)
				res = _FAIL;
			if (pwrctrl->rpwm < pslv)
				rtw_set_rpwm(padapter, pslv);
		}
	}

	up(&pwrctrl->lock);

_func_exit_;

	return res;
}

/*
 * Caller: rtw_cmd_thread
 *
 * Check if the fw_pwrstate is okay for issuing cmd.
 * If not (cpwm should be is less than S2), then the sub-routine
 * will raise the cpwm to be greater than or equal to S2.
 *
 * Calling Context: Passive
 *
 * Return Value:
 *	_SUCCESS	rtw_cmd_thread can issue cmds to firmware afterwards.
 *	_FAIL		rtw_cmd_thread can not do anything.
 */
s32 rtw_register_cmd_alive(struct rtw_adapter *padapter)
{
	s32 res;
	struct pwrctrl_priv *pwrctrl;
	u8 pslv;

_func_enter_;

	res = _SUCCESS;
	pwrctrl = &padapter->pwrctrlpriv;
#ifdef CONFIG_BT_COEXIST
	if (true == padapter->pwrctrlpriv.btcoex_rfon)
		pslv = PS_STATE_S3;
	else
#endif
	{
		pslv = PS_STATE_S2;
	}

	down(&pwrctrl->lock);

	register_task_alive(pwrctrl, CMD_ALIVE);

	if (pwrctrl->bFwCurrentInPSMode == true)
	{
		RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_info_,
				 ("rtw_register_cmd_alive: cpwm=0x%02x alives=0x%08x\n",
				  pwrctrl->cpwm, pwrctrl->alives));

		if (pwrctrl->cpwm < pslv)
		{
			if (pwrctrl->cpwm < PS_STATE_S2)
			res = _FAIL;
			if (pwrctrl->rpwm < pslv)
				rtw_set_rpwm(padapter, pslv);
		}
	}

	up(&pwrctrl->lock);

_func_exit_;

	return res;
}

/*
 * Caller: rx_isr
 *
 * Calling Context: Dispatch/ISR
 *
 * Return Value:
 *	_SUCCESS
 *	_FAIL
 */
s32 rtw_register_rx_alive(struct rtw_adapter *padapter)
{
	struct pwrctrl_priv *pwrctrl;

_func_enter_;

	pwrctrl = &padapter->pwrctrlpriv;

	down(&pwrctrl->lock);

	register_task_alive(pwrctrl, RECV_ALIVE);
	RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
			 ("rtw_register_rx_alive: cpwm=0x%02x alives=0x%08x\n",
			  pwrctrl->cpwm, pwrctrl->alives));

	up(&pwrctrl->lock);

_func_exit_;

	return _SUCCESS;
}

/*
 * Caller: evt_isr or evt_thread
 *
 * Calling Context: Dispatch/ISR or Passive
 *
 * Return Value:
 *	_SUCCESS
 *	_FAIL
 */
s32 rtw_register_evt_alive(struct rtw_adapter *padapter)
{
	struct pwrctrl_priv *pwrctrl;

_func_enter_;

	pwrctrl = &padapter->pwrctrlpriv;

	down(&pwrctrl->lock);

	register_task_alive(pwrctrl, EVT_ALIVE);
	RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
			 ("rtw_register_evt_alive: cpwm=0x%02x alives=0x%08x\n",
			  pwrctrl->cpwm, pwrctrl->alives));

	up(&pwrctrl->lock);

_func_exit_;

	return _SUCCESS;
}

/*
 * Caller: ISR
 *
 * If ISR's txdone,
 * No more pkts for TX,
 * Then driver shall call this fun. to power down firmware again.
 */
void rtw_unregister_tx_alive(struct rtw_adapter *padapter)
{
	struct pwrctrl_priv *pwrctrl;

_func_enter_;

	pwrctrl = &padapter->pwrctrlpriv;

	down(&pwrctrl->lock);

	unregister_task_alive(pwrctrl, XMIT_ALIVE);

	if ((pwrctrl->pwr_mode != PS_MODE_ACTIVE) &&
		(pwrctrl->bFwCurrentInPSMode == true))
	{
		RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
				 ("%s: cpwm=0x%02x alives=0x%08x\n",
				  __FUNCTION__, pwrctrl->cpwm, pwrctrl->alives));

		if ((pwrctrl->alives == 0) &&
			(pwrctrl->cpwm > PS_STATE_S0))
		{
			rtw_set_rpwm(padapter, PS_STATE_S0);
		}
	}

	up(&pwrctrl->lock);

_func_exit_;
}

/*
 * Caller: ISR
 *
 * If all commands have been done,
 * and no more command to do,
 * then driver shall call this fun. to power down firmware again.
 */
void rtw_unregister_cmd_alive(struct rtw_adapter *padapter)
{
	struct pwrctrl_priv *pwrctrl;

_func_enter_;

	pwrctrl = &padapter->pwrctrlpriv;

	down(&pwrctrl->lock);

	unregister_task_alive(pwrctrl, CMD_ALIVE);

	if ((pwrctrl->pwr_mode != PS_MODE_ACTIVE) &&
		(pwrctrl->bFwCurrentInPSMode == true))
	{
		RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_info_,
				 ("%s: cpwm=0x%02x alives=0x%08x\n",
				  __FUNCTION__, pwrctrl->cpwm, pwrctrl->alives));

		if ((pwrctrl->alives == 0) &&
			(pwrctrl->cpwm > PS_STATE_S0))
		{
			rtw_set_rpwm(padapter, PS_STATE_S0);
		}
	}

	up(&pwrctrl->lock);

_func_exit_;
}

/*
 * Caller: ISR
 */
void rtw_unregister_rx_alive(struct rtw_adapter *padapter)
{
	struct pwrctrl_priv *pwrctrl;

_func_enter_;

	pwrctrl = &padapter->pwrctrlpriv;

	down(&pwrctrl->lock);

	unregister_task_alive(pwrctrl, RECV_ALIVE);

	RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
			 ("rtw_unregister_rx_alive: cpwm=0x%02x alives=0x%08x\n",
			  pwrctrl->cpwm, pwrctrl->alives));

	up(&pwrctrl->lock);

_func_exit_;
}

void rtw_unregister_evt_alive(struct rtw_adapter *padapter)
{
	struct pwrctrl_priv *pwrctrl;

_func_enter_;

	pwrctrl = &padapter->pwrctrlpriv;

	unregister_task_alive(pwrctrl, EVT_ALIVE);

	RT_TRACE(_module_rtl871x_pwrctrl_c_, _drv_notice_,
			 ("rtw_unregister_evt_alive: cpwm=0x%02x alives=0x%08x\n",
			  pwrctrl->cpwm, pwrctrl->alives));

	up(&pwrctrl->lock);

_func_exit_;
}
#endif	/* CONFIG_LPS_LCLK */

#ifdef CONFIG_RESUME_IN_WORKQUEUE
static void resume_workitem_callback(struct work_struct *work);
#endif /* CONFIG_RESUME_IN_WORKQUEUE */

void rtw_init_pwrctrl_priv(struct rtw_adapter *padapter)
{
	struct pwrctrl_priv *pwrctrlpriv = &padapter->pwrctrlpriv;

_func_enter_;

	sema_init(&pwrctrlpriv->lock, 1);
	pwrctrlpriv->rf_pwrstate = rf_on;
	pwrctrlpriv->ips_enter_cnts=0;
	pwrctrlpriv->ips_leave_cnts=0;
	pwrctrlpriv->bips_processing = false;

	pwrctrlpriv->ips_mode = padapter->registrypriv.ips_mode;
	pwrctrlpriv->ips_mode_req = padapter->registrypriv.ips_mode;

	pwrctrlpriv->pwr_state_check_interval = RTW_PWR_STATE_CHK_INTERVAL;
	pwrctrlpriv->pwr_state_check_cnts = 0;
	pwrctrlpriv->bInternalAutoSuspend = false;
	pwrctrlpriv->bInSuspend = false;
	pwrctrlpriv->bkeepfwalive = false;

#ifdef CONFIG_AUTOSUSPEND
#ifdef SUPPORT_HW_RFOFF_DETECTED
	pwrctrlpriv->pwr_state_check_interval = (pwrctrlpriv->bHWPwrPindetect) ?1000:2000;
#endif
#endif

	pwrctrlpriv->LpsIdleCount = 0;
	pwrctrlpriv->power_mgnt =padapter->registrypriv.power_mgnt;/*  PS_MODE_MIN; */
	pwrctrlpriv->bLeisurePs = (PS_MODE_ACTIVE != pwrctrlpriv->power_mgnt)?true:false;

	pwrctrlpriv->bFwCurrentInPSMode = false;

	pwrctrlpriv->rpwm = 0;
	pwrctrlpriv->cpwm = PS_STATE_S4;

	pwrctrlpriv->pwr_mode = PS_MODE_ACTIVE;
	pwrctrlpriv->smart_ps = padapter->registrypriv.smart_ps;
	pwrctrlpriv->bcn_ant_mode = 0;

	pwrctrlpriv->tog = 0x80;

	pwrctrlpriv->btcoex_rfon = false;

#ifdef CONFIG_LPS_LCLK
	rtw_hal_set_hwreg(padapter, HW_VAR_SET_RPWM, (u8 *)(&pwrctrlpriv->rpwm));

	INIT_WORK(&pwrctrlpriv->cpwm_event, cpwm_event_callback);

#ifdef CONFIG_LPS_RPWM_TIMER
	pwrctrlpriv->brpwmtimeout = false;
	INIT_WORK(&pwrctrlpriv->rpwmtimeoutwi, rpwmtimeout_workitem_callback);
	_init_timer(&pwrctrlpriv->pwr_rpwm_timer, padapter->pnetdev, pwr_rpwm_timeout_handler, padapter);
#endif /*  CONFIG_LPS_RPWM_TIMER */
#endif /*  CONFIG_LPS_LCLK */

	_init_timer(&(pwrctrlpriv->pwr_state_check_timer), padapter->pnetdev, pwr_state_check_handler, (u8 *)padapter);

#ifdef CONFIG_RESUME_IN_WORKQUEUE
	INIT_WORK(&pwrctrlpriv->resume_work, resume_workitem_callback);
	pwrctrlpriv->rtw_workqueue = create_singlethread_workqueue("rtw_workqueue");
#endif /* CONFIG_RESUME_IN_WORKQUEUE */

_func_exit_;
}

void rtw_free_pwrctrl_priv(struct rtw_adapter *adapter)
{
	struct pwrctrl_priv *pwrctrlpriv = &adapter->pwrctrlpriv;

_func_enter_;

	/* memset((unsigned char *)pwrctrlpriv, 0, sizeof(struct pwrctrl_priv)); */

#ifdef CONFIG_RESUME_IN_WORKQUEUE
	if (pwrctrlpriv->rtw_workqueue) {
		flush_workqueue(pwrctrlpriv->rtw_workqueue);
		destroy_workqueue(pwrctrlpriv->rtw_workqueue);
	}
#endif

_func_exit_;
}

#ifdef CONFIG_RESUME_IN_WORKQUEUE
extern int rtw_resume_process(struct rtw_adapter *padapter);
static void resume_workitem_callback(struct work_struct *work)
{
	struct pwrctrl_priv *pwrpriv = container_of(work, struct pwrctrl_priv, resume_work);
	struct rtw_adapter *adapter = container_of(pwrpriv, struct rtw_adapter, pwrctrlpriv);

	DBG_8723A("%s\n",__FUNCTION__);

	rtw_resume_process(adapter);
}

void rtw_resume_in_workqueue(struct pwrctrl_priv *pwrpriv)
{
	/*  accquire system's suspend lock preventing from falliing asleep while resume in workqueue */
	rtw_lock_suspend();

	queue_work(pwrpriv->rtw_workqueue, &pwrpriv->resume_work);
}
#endif /* CONFIG_RESUME_IN_WORKQUEUE */

u8 rtw_interface_ps_func(struct rtw_adapter *padapter,HAL_INTF_PS_FUNC efunc_id,u8* val)
{
	u8 bResult = true;
	rtw_hal_intf_ps_func(padapter,efunc_id,val);

	return bResult;
}

inline void rtw_set_ips_deny(struct rtw_adapter *padapter, u32 ms)
{
	struct pwrctrl_priv *pwrpriv = &padapter->pwrctrlpriv;
	pwrpriv->ips_deny_time = rtw_get_current_time() + rtw_ms_to_systime(ms);
}

/*
* rtw_pwr_wakeup - Wake the NIC up from: 1)IPS. 2)USB autosuspend
* @adapter: pointer to _adapter structure
* @ips_deffer_ms: the ms wiil prevent from falling into IPS after wakeup
* Return _SUCCESS or _FAIL
*/

int _rtw_pwr_wakeup(struct rtw_adapter *padapter, u32 ips_deffer_ms, const char *caller)
{
	struct pwrctrl_priv *pwrpriv = &padapter->pwrctrlpriv;
	struct mlme_priv *pmlmepriv = &padapter->mlmepriv;
	int ret = _SUCCESS;
	u32 start = rtw_get_current_time();

	if (pwrpriv->ips_deny_time < rtw_get_current_time() + rtw_ms_to_systime(ips_deffer_ms))
		pwrpriv->ips_deny_time = rtw_get_current_time() + rtw_ms_to_systime(ips_deffer_ms);

	if (pwrpriv->ps_processing) {
		DBG_8723A("%s wait ps_processing...\n", __func__);
		while (pwrpriv->ps_processing && rtw_get_passing_time_ms(start) <= 3000)
			msleep(10);
		if (pwrpriv->ps_processing)
			DBG_8723A("%s wait ps_processing timeout\n", __func__);
		else
			DBG_8723A("%s wait ps_processing done\n", __func__);
	}

#ifdef DBG_CONFIG_ERROR_DETECT
	if (rtw_hal_sreset_inprogress(padapter)) {
		DBG_8723A("%s wait sreset_inprogress...\n", __func__);
		while (rtw_hal_sreset_inprogress(padapter) && rtw_get_passing_time_ms(start) <= 4000)
			msleep(10);
		if (rtw_hal_sreset_inprogress(padapter))
			DBG_8723A("%s wait sreset_inprogress timeout\n", __func__);
		else
			DBG_8723A("%s wait sreset_inprogress done\n", __func__);
	}
#endif

	if (pwrpriv->bInternalAutoSuspend == false && pwrpriv->bInSuspend) {
		DBG_8723A("%s wait bInSuspend...\n", __func__);
		while (pwrpriv->bInSuspend &&
		       (rtw_get_passing_time_ms(start) <= 3000)) {
			msleep(10);
		}
		if (pwrpriv->bInSuspend)
			DBG_8723A("%s wait bInSuspend timeout\n", __func__);
		else
			DBG_8723A("%s wait bInSuspend done\n", __func__);
	}

	/* System suspend is not allowed to wakeup */
	if((pwrpriv->bInternalAutoSuspend == false) && (true == pwrpriv->bInSuspend )){
		ret = _FAIL;
		goto exit;
	}

	/* block??? */
	if((pwrpriv->bInternalAutoSuspend == true)  && (padapter->net_closed == true)) {
		ret = _FAIL;
		goto exit;
	}

	/* I think this should be check in IPS, LPS, autosuspend functions... */
	if (check_fwstate(pmlmepriv, _FW_LINKED) == true)
	{
#if defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND)
		if(true==pwrpriv->bInternalAutoSuspend){
			if(0==pwrpriv->autopm_cnt){
				if (usb_autopm_get_interface(adapter_to_dvobj(padapter)->pusbintf) < 0)
					DBG_8723A( "can't get autopm: \n");
			pwrpriv->autopm_cnt++;
		}
#endif	/* if defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND) */
		ret = _SUCCESS;
		goto exit;
#if defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND)
		}
#endif	/* if defined (CONFIG_BT_COEXIST)&& defined (CONFIG_AUTOSUSPEND) */
	}

	if(rf_off == pwrpriv->rf_pwrstate )
	{
#ifdef CONFIG_AUTOSUSPEND
		 if(pwrpriv->brfoffbyhw==true)
		{
			DBG_8723A("hw still in rf_off state ...........\n");
			ret = _FAIL;
			goto exit;
		}
		else if(padapter->registrypriv.usbss_enable)
		{
			DBG_8723A("%s call autoresume_enter....\n",__FUNCTION__);
			if(_FAIL ==  autoresume_enter(padapter))
			{
				DBG_8723A("======> autoresume fail.............\n");
				ret = _FAIL;
				goto exit;
			}
		}
		else
#endif
		{
#ifdef CONFIG_IPS
			DBG_8723A("%s call ips_leave....\n",__FUNCTION__);
			if(_FAIL ==  ips_leave(padapter))
			{
				DBG_8723A("======> ips_leave fail.............\n");
				ret = _FAIL;
				goto exit;
			}
#endif
		}
	}

	/* TODO: the following checking need to be merged... */
	if(padapter->bDriverStopped
		|| !padapter->bup
		|| !padapter->hw_init_completed
	){
		DBG_8723A("%s: bDriverStopped=%d, bup=%d, hw_init_completed=%u\n"
			, caller
			, padapter->bDriverStopped
			, padapter->bup
			, padapter->hw_init_completed);
		ret= false;
		goto exit;
	}

exit:
	if (pwrpriv->ips_deny_time < rtw_get_current_time() + rtw_ms_to_systime(ips_deffer_ms))
		pwrpriv->ips_deny_time = rtw_get_current_time() + rtw_ms_to_systime(ips_deffer_ms);
	return ret;
}

int rtw_pm_set_lps(struct rtw_adapter *padapter, u8 mode)
{
	int	ret = 0;
	struct pwrctrl_priv *pwrctrlpriv = &padapter->pwrctrlpriv;

	if ( mode < PS_MODE_NUM )
	{
		if(pwrctrlpriv->power_mgnt !=mode)
		{
			if(PS_MODE_ACTIVE == mode)
			{
				LeaveAllPowerSaveMode(padapter);
			}
			else
			{
				pwrctrlpriv->LpsIdleCount = 2;
			}
			pwrctrlpriv->power_mgnt = mode;
			pwrctrlpriv->bLeisurePs = (PS_MODE_ACTIVE != pwrctrlpriv->power_mgnt)?true:false;
		}
	}
	else
	{
		ret = -EINVAL;
	}

	return ret;
}

int rtw_pm_set_ips(struct rtw_adapter *padapter, u8 mode)
{
	struct pwrctrl_priv *pwrctrlpriv = &padapter->pwrctrlpriv;

	if( mode == IPS_NORMAL || mode == IPS_LEVEL_2 ) {
		rtw_ips_mode_req(pwrctrlpriv, mode);
		DBG_8723A("%s %s\n", __FUNCTION__, mode == IPS_NORMAL?"IPS_NORMAL":"IPS_LEVEL_2");
		return 0;
	}
	else if(mode ==IPS_NONE){
		rtw_ips_mode_req(pwrctrlpriv, mode);
		DBG_8723A("%s %s\n", __FUNCTION__, "IPS_NONE");
		if((padapter->bSurpriseRemoved ==0)&&(_FAIL == rtw_pwr_wakeup(padapter)) )
			return -EFAULT;
	}
	else {
		return -EINVAL;
	}
	return 0;
}
