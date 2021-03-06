/*
 ***************************************************************************
 * Ralink Tech Inc.
 * 4F, No. 2 Technology 5th Rd.
 * Science-based Industrial Park
 * Hsin-chu, Taiwan, R.O.C.
 *
 * (c) Copyright 2002-2004, Ralink Technology, Inc.
 *
 * All rights reserved. Ralink's source code is an unpublished work and the
 * use of a copyright notice does not imply otherwise. This source code
 * contains confidential trade secret material of Ralink Tech. Any attemp
 * or participation in deciphering, decoding, reverse engineering or in	any
 * way altering the source code is stricitly prohibited, unless the prior
 * written consent of Ralink Technology, Inc. is obtained.
 ***************************************************************************

	Module Name:
	wpa.c
*/

#include "rt_config.h"

/*
	========================================================================

	Routine Description:
		Process MIC error indication and record MIC error timer.

	Arguments:
		pAd 		Pointer to our adapter
		pWpaKey 	Pointer to the WPA key structure

	Return Value:
		None

	IRQL = DISPATCH_LEVEL

	Note:

	========================================================================
*/
void RTMPReportMicError(RTMP_ADAPTER *pAd, PCIPHER_KEY pWpaKey)
{
	ULONG Now;
	UCHAR unicastKey = (pWpaKey->Type == PAIRWISE_KEY ? 1:0);

	/* Record Last MIC error time and count */
	NdisGetSystemUpTime(&Now);
	if (pAd->StaCfg.MicErrCnt == 0) {
		pAd->StaCfg.MicErrCnt++;
		pAd->StaCfg.LastMicErrorTime = Now;
		NdisZeroMemory(pAd->StaCfg.ReplayCounter, 8);
	} else if (pAd->StaCfg.MicErrCnt == 1) {
		if ((pAd->StaCfg.LastMicErrorTime + (60 * OS_HZ)) < Now) {
			/* Update Last MIC error time, this did not violate
			   two MIC errors within 60 seconds */
			pAd->StaCfg.LastMicErrorTime = Now;
		} else {
			RTMPSendWirelessEvent(pAd, IW_COUNTER_MEASURES_EVENT_FLAG,
					pAd->MacTab.Content[BSSID_WCID].Addr,
					BSS0, 0);

			pAd->StaCfg.LastMicErrorTime = Now;
			/* Violate MIC error counts, MIC countermeasures kicks in */
			pAd->StaCfg.MicErrCnt++;

			/*
			 We shall block all reception
			 We shall clean all Tx ring and disassoicate from AP after
			 next EAPOL frame

			 No necessary to clean all Tx ring, on HardTransmit will
			 stop sending non-802.1X EAPOL packets if
			 pAd->StaCfg.MicErrCnt greater than 2.
			*/
		}
	} else {
		/* MIC error count >= 2 */
		/* This should not happen */
		;
	}

	MlmeEnqueue(pAd, MLME_CNTL_STATE_MACHINE, OID_802_11_MIC_FAILURE_REPORT_FRAME,
			1, &unicastKey, 0);

	if (pAd->StaCfg.MicErrCnt == 2) {
		RTMPSetTimer(&pAd->StaCfg.WpaDisassocAndBlockAssocTimer, 100);
	}
}

void WpaMicFailureReportFrame(PRTMP_ADAPTER pAd, MLME_QUEUE_ELEM *Elem)
{
	PUCHAR pOutBuffer;
	UCHAR Header802_3[14];
	ULONG FrameLen = 0;
	UCHAR *mpool;
	PEAPOL_PACKET pPacket;
	UCHAR Mic[16];
	BOOLEAN bUnicast;

	DBGPRINT(RT_DEBUG_TRACE, ("WpaMicFailureReportFrame ----->\n"));

	bUnicast = (Elem->Msg[0] == 1 ? TRUE:FALSE);
	pAd->Sequence = ((pAd->Sequence) + 1) & (MAX_SEQ_NUMBER);

	/* init 802.3 header and Fill Packet */
	MAKE_802_3_HEADER(Header802_3, pAd->CommonCfg.Bssid, pAd->CurrentAddress,
			EAPOL);

	/* Allocate memory for output */
	mpool = os_alloc_mem(TX_EAPOL_BUFFER);
	if (mpool == NULL) {
		return;
	}

	pPacket = (PEAPOL_PACKET)mpool;
	NdisZeroMemory(pPacket, TX_EAPOL_BUFFER);

	pPacket->ProVer = EAPOL_VER;
	pPacket->ProType = EAPOLKey;
	pPacket->KeyDesc.Type = WPA1_KEY_DESC;

	/* Request field presented */
	pPacket->KeyDesc.KeyInfo.Request = 1;

	if (pAd->StaCfg.wdev.WepStatus == Ndis802_11AESEnable)
		pPacket->KeyDesc.KeyInfo.KeyDescVer = 2;
	else /* TKIP */
		pPacket->KeyDesc.KeyInfo.KeyDescVer = 1;

	pPacket->KeyDesc.KeyInfo.KeyType = (bUnicast ? PAIRWISEKEY : GROUPKEY);

	/* KeyMic field presented */
	pPacket->KeyDesc.KeyInfo.KeyMic  = 1;

	/* Error field presented */
	pPacket->KeyDesc.KeyInfo.Error  = 1;

	/* Update packet length after decide Key data payload */
	SET_UINT16_TO_ARRARY(pPacket->Body_Len, MIN_LEN_OF_EAPOL_KEY_MSG)

	/* Key Replay Count */
	NdisMoveMemory(pPacket->KeyDesc.ReplayCounter, pAd->StaCfg.ReplayCounter,
			LEN_KEY_DESC_REPLAY);
	inc_byte_array(pAd->StaCfg.ReplayCounter, 8);

	/* Convert to little-endian format. */
	*((USHORT *)&pPacket->KeyDesc.KeyInfo) = cpu2le16(*((USHORT *)&pPacket->KeyDesc.KeyInfo));

	pOutBuffer = MlmeAllocateMemory();  /* allocate memory */
	if (pOutBuffer == NULL) {
		os_free_mem(mpool);
		return;
	}

	/*
	   Prepare EAPOL frame for MIC calculation
	   Be careful, only EAPOL frame is counted for MIC calculation
	*/
	MakeOutgoingFrame(pOutBuffer, &FrameLen,
			CONV_ARRARY_TO_UINT16(pPacket->Body_Len) + 4, pPacket,
			END_OF_ARGS);

	/* Prepare and Fill MIC value */
	NdisZeroMemory(Mic, sizeof(Mic));
	if(pAd->StaCfg.wdev.WepStatus  == Ndis802_11AESEnable) {
		/* AES */
		UCHAR digest[20] = {0};
		RT_HMAC_SHA1(pAd->StaCfg.PTK, LEN_PTK_KCK, pOutBuffer, FrameLen,
				digest, SHA1_DIGEST_SIZE);
		NdisMoveMemory(Mic, digest, LEN_KEY_DESC_MIC);
	} else {
		/* TKIP */
		RT_HMAC_MD5(pAd->StaCfg.PTK, LEN_PTK_KCK, pOutBuffer, FrameLen,
				Mic, MD5_DIGEST_SIZE);
	}

	NdisMoveMemory(pPacket->KeyDesc.KeyMic, Mic, LEN_KEY_DESC_MIC);

	/* Copy frame to Tx ring and send MIC failure report frame to authenticator */
	RTMPToWirelessSta(pAd, &pAd->MacTab.Content[BSSID_WCID], Header802_3,
			LENGTH_802_3, (PUCHAR)pPacket,
			CONV_ARRARY_TO_UINT16(pPacket->Body_Len) + 4, FALSE);

	MlmeFreeMemory(pOutBuffer);
	os_free_mem(mpool);

	DBGPRINT(RT_DEBUG_TRACE, ("WpaMicFailureReportFrame <-----\n"));
}


void WpaDisassocApAndBlockAssoc(PVOID SystemSpecific1, PVOID FunctionContext,
	PVOID SystemSpecific2, IN PVOID SystemSpecific3)
{
	RTMP_ADAPTER *pAd = (PRTMP_ADAPTER)FunctionContext;
	MLME_DISASSOC_REQ_STRUCT DisassocReq;

	/* Disassoc from current AP first */
	DBGPRINT(RT_DEBUG_TRACE,
			("RTMPReportMicError - disassociate with current AP after sending second continuous EAPOL frame\n"));
	DisassocParmFill(pAd, &DisassocReq, pAd->CommonCfg.Bssid, REASON_MIC_FAILURE);
	MlmeEnqueue(pAd, ASSOC_STATE_MACHINE, MT2_MLME_DISASSOC_REQ,
			sizeof(MLME_DISASSOC_REQ_STRUCT), &DisassocReq, 0);

	pAd->Mlme.CntlMachine.CurrState = CNTL_WAIT_DISASSOC;
	pAd->StaCfg.bBlockAssoc = TRUE;
}


/*
	========================================================================

	Routine Description:
		Send EAPoL-Start packet to AP.

	Arguments:
		pAd		- NIC Adapter pointer

	Return Value:
		None

	IRQL = DISPATCH_LEVEL

	Note:
		Actions after link up
		1. Change the correct parameters
		2. Send EAPOL - START

	========================================================================
*/
void WpaSendEapolStart(RTMP_ADAPTER *pAd, UCHAR *pBssid)
{
	IEEE8021X_FRAME Packet;
	UCHAR Header802_3[14];

	DBGPRINT(RT_DEBUG_TRACE, ("-----> WpaSendEapolStart\n"));

	NdisZeroMemory(Header802_3,sizeof(UCHAR)*14);

	MAKE_802_3_HEADER(Header802_3, pBssid, &pAd->CurrentAddress[0], EAPOL);

	/* Zero message 2 body */
	NdisZeroMemory(&Packet, sizeof(Packet));
	Packet.Version = EAPOL_VER;
	Packet.Type = EAPOLStart;
	Packet.Length = cpu2be16(0);

	/* Copy frame to Tx ring */
	RTMPToWirelessSta((PRTMP_ADAPTER)pAd, &pAd->MacTab.Content[BSSID_WCID],
			Header802_3, LENGTH_802_3, (PUCHAR)&Packet, 4, TRUE);

	DBGPRINT(RT_DEBUG_TRACE, ("<----- WpaSendEapolStart\n"));
}


