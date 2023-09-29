/*
 * x_sntp_client.c
 * Copyright 2014-22 Andre M. Maree / KSS Technologies (Pty) Ltd.
 */

#include "hal_config.h"

#include "hal_rtc.h"
#include "x_sntp_client.h"
#include "printfx.h"									// +x_definitions +stdarg +stdint +stdio
#include "socketsX.h"
#include "syslog.h"
#include "x_errors_events.h"

#include <math.h>

#define	debugFLAG					0xF000

#define	debugPROTOCOL				(debugFLAG & 0x0001)
#define	debugHOSTS					(debugFLAG & 0x0002)
#define	debugCALCULATION			(debugFLAG & 0x0004)

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ############################################ Macros  ############################################

#define sntpMS_REFRESH				(60 * SECONDS_IN_MINUTE * MILLIS_IN_SECOND)
#define sntpMS_RETRY				(1 * SECONDS_IN_MINUTE * MILLIS_IN_SECOND)

#define	STRATUM_IDX(x)	((x >= specNTP_STRATUM_RSVD_LO) ? 4 : (x == specNTP_STRATUM_UNSYNC)	? 3 : (x >= specNTP_STRATUM_SEC_LO)	? 2 : x )

// ###################################### local ie static variables ################################

StaticTask_t ttsSNTP = { 0 };
StackType_t tsbSNTP[sntpSTACK_SIZE] = { 0 };

ntp_t sNtpBuf;
u64_t tNTP[4];
i64_t tRTD, tOFF;
int NtpHostIndex = 0;

const char * const NtpHostTable[] = {
	"0.pool.ntp.org",	"1.pool.ntp.org",	"2.pool.ntp.org",	"3.pool.ntp.org",
} ;

/*
 * xNTPCalcValue() convert NTP epoch NETWORK seconds/fractions to UNIX epoch HOST microseconds
 */
u64_t xNTPCalcValue(u32_t Secs, u32_t Frac) {
	u64_t u64Val1 = ntohl(Secs) - EPOCH_SECONDS_DIFFERENCE ;	// difference between NTP and selected epoch
	u64Val1 *= MICROS_IN_SECOND ;						// convert Secs to uSec
	u64_t u64Val2 = ntohl(Frac) / FRACTIONS_PER_MICROSEC ;	// convert Frac to uSec
	return u64Val1 + u64Val2 ;
}

/*
 * vNtpDebug() - Display the sNtpBuf header info as from server
 */
void vNtpDebug(void) {
	const char * const LI_mess[]	= { "None", "61Sec", "59Sec", "Alarm" } ;
	const char * const Mode_mess[]	= { "Unspec", "SymAct", "SymPas", "Client", "Server", "BCast", "RsvdNTP", "RsvdPriv" } ;
	const char * const Strat_mess[]= { "KofD", "Prim", "Sec", "UnSync" , "Rsvd" } ;
	// Display the header info
	printfx("[NTP] LI[%s] V[%u] Mode[%s] Stratum[%s] Poll[%.1fs] Precision[%fuS]\r\n",
			LI_mess[sNtpBuf.LI], sNtpBuf.VN, Mode_mess[sNtpBuf.Mode], Strat_mess[STRATUM_IDX(sNtpBuf.Stratum)],
			pow(2, (double) sNtpBuf.Poll), pow(2, (double) sNtpBuf.Precision) * 1000000) ;
	printfx("[NTP] Root Delay[%d.%04d Sec]\r\n", ntohs(sNtpBuf.RDelUnit), ntohs(sNtpBuf.RDelFrac) / (UINT16_MAX/10000)) ;
	printfx("[NTP] Dispersion[%u.%04u Sec]\r\n", ntohs(sNtpBuf.RDisUnit), ntohs(sNtpBuf.RDisFrac) / (UINT16_MAX/10000)) ;
	if (sNtpBuf.Stratum <= specNTP_STRATUM_PRI) {
		printfx("[NTP] Ref ID[%4s]\r\n", &sNtpBuf.RefID) ;
	} else {
		printfx("[NTP] Ref IP[%-I]\r\n", sNtpBuf.RefIP) ;
	}
// determine and display the reference timestamp
	u64_t tTemp	= xNTPCalcValue(sNtpBuf.Ref.secs, sNtpBuf.Ref.frac) ;
	printfx("[NTP] Ref: %.6R\r\n", tTemp);
	printfx("[NTP] (t0) %.6R\r\n", tNTP[0]);
	printfx("[NTP] (t1) %.6R\r\n", tNTP[1]);
	printfx("[NTP] (t2) %.6R\r\n", tNTP[2]);
	printfx("[NTP] (t3) %.6R\r\n", tNTP[3]);
}

/*
 * vNtpCalcCorrectTime()
 */
void vNtpCalcCorrectTime(u64_t * pTStamp) {
	i64_t		tT0, tT1 ;
	tNTP[0]	= xNTPCalcValue(sNtpBuf.Orig.secs , sNtpBuf.Orig.frac) ;
	tNTP[1]	= xNTPCalcValue(sNtpBuf.Recv.secs , sNtpBuf.Recv.frac) ;
	tNTP[2]	= xNTPCalcValue(sNtpBuf.Xmit.secs , sNtpBuf.Xmit.frac) ;
	tNTP[3] = *pTStamp ;
// Calculate the Round Trip Delay
	tT0		= tNTP[3] - tNTP[0] ;
	tT1		= tNTP[2] - tNTP[1] ;
	tRTD	= tT0 - tT1 ;
// Then do the Offset in steps...
	tT0		= tNTP[1] - tNTP[0] ;
	tT1		= tNTP[2] - tNTP[3] ;
	tOFF	= tT0 + tT1 ;
	tOFF	/= 2 ;
	*pTStamp = tNTP[0] + tRTD + tOFF ;
	IF_SL_INFO(debugCALCULATION, "'%s' %.6R tOFF=%'lld uS tRTD=%'lld uS", NtpHostTable[NtpHostIndex], *pTStamp, tOFF, tRTD) ;
}

int	xNtpRequestInfo(netx_t * psNtpCtx, u64_t * pTStamp) {
	memset(&sNtpBuf, 0, sizeof(ntp_t)) ;
	sNtpBuf.VN	= specNTP_VERSION_V4 ;
	sNtpBuf.Mode= specNTP_MODE_CLIENT ;

	// Plug in current (possibly very wrong) client transmit time
	sNtpBuf.Xmit.secs 	= htonl(xTimeStampAsSeconds(*pTStamp) + EPOCH_SECONDS_DIFFERENCE) ;
	sNtpBuf.Xmit.frac	= htonl((*pTStamp % MICROS_IN_SECOND) * FRACTIONS_PER_MICROSEC) ;

	// send the formatted request
	int iRV = xNetSend(psNtpCtx, (u8_t *) &sNtpBuf, sizeof(ntp_t)) ;
	if (iRV == sizeof(ntp_t)) {
		xNetSetRecvTO(psNtpCtx, 400);
		iRV = xNetRecv(psNtpCtx, (u8_t *) &sNtpBuf, sizeof(ntp_t)) ;
	}
	if (iRV != sizeof(ntp_t))
		return iRV;
	// expect only server type responses with correct version and stratum
	if (sNtpBuf.Mode != specNTP_MODE_SERVER || sNtpBuf.VN != specNTP_VERSION_V4 ||
		OUTSIDE(specNTP_STRATUM_PRI, sNtpBuf.Stratum, specNTP_STRATUM_SEC_HI)) {
		SL_ERR("Host=%s  Mode=%d  Ver=%d  Stratum=%d", psNtpCtx->pHost, sNtpBuf.Mode, sNtpBuf.VN, sNtpBuf.Stratum) ;
   		return erFAILURE ;
   	}
	IF_P(debugHOSTS, "Sync'ing with host %s\r\n", NtpHostTable[NtpHostIndex]) ;
	return iRV ;
}

/*
 *! xNetNTPGetTime() - Gets the current time from the selected (S)sNtpBuf server
 *! \brief  	This function obtains the sNtpBuf time from the server.
 *! \param[in]	hostname - name of sNtpBuf host to query
 *! \param[out]	pSystime - pointer to system time structure
 *! \return		NEGATIVE - socket or application error code
 *				ZERO	 -	if all OK
 */
int xNtpGetTime(u64_t * pTStamp) {
	netx_t	sNtpCtx = { 0 };
	sNtpCtx.sa_in.sin_family = AF_INET;
	sNtpCtx.sa_in.sin_port = htons(IP_PORT_NTP);
	sNtpCtx.type = SOCK_DGRAM;
	sNtpCtx.flags = SO_REUSEADDR;
#if 0
	sNtpCtx.d = (netx_dbg_t) * { .o=1, .w=1, .r=1, .d=1, .ea=1 };
#endif
	for (int iRV = -1; iRV != sizeof(ntp_t) ; ) {
		sNtpCtx.pHost	= NtpHostTable[NtpHostIndex] ;
		IF_P(debugHOSTS, "Connecting to host %s\r\n", sNtpCtx.pHost) ;
		iRV = xNetOpen(&sNtpCtx) ;
		if (iRV >= erSUCCESS)
			iRV = xNtpRequestInfo(&sNtpCtx, pTStamp) ;	// send the sNtpBuf request & check the result
		xNetClose(&sNtpCtx) ;							// close, & ignore return code..
		if (iRV != sizeof(ntp_t)) {
			vTaskDelay(pdMS_TO_TICKS(1000)) ;			// wait 1 seconds
			++NtpHostIndex ;
			NtpHostIndex %= NO_MEM(NtpHostTable) ;		// failed, step to next host...
		}
	}

	// end of loop, must have a valid HOST
	vNtpCalcCorrectTime(pTStamp) ;						// calculate & update correct time
	SL_NOT("%s(%#-I)  %.6R  tOFF=%'llduS  tRTD=%'llduS", NtpHostTable[NtpHostIndex], sNtpCtx.sa_in.sin_addr.s_addr, *pTStamp, tOFF, tRTD) ;
	IF_EXEC_0(debugPROTOCOL, vNtpDebug) ;
	return erSUCCESS ;
}

/*
 * vSntpTask()
 */
void vSntpTask(void * pvPara) {
	vTaskSetThreadLocalStoragePointer(NULL, buildFRTLSP_EVT_MASK, (void *)taskSNTP_MASK);
	xRtosTaskSetRUN(taskSNTP_MASK);
	while (bRtosTaskWaitOK(taskSNTP_MASK, portMAX_DELAY)) {
		TickType_t NtpDelay = 0, NtpLWtime = xTaskGetTickCount();
		if (bRtosWaitStatusALL(flagLX_STA, pdMS_TO_TICKS(sntpMS_REFRESH - sntpMS_RETRY))) {
			if (xNtpGetTime((u64_t *) pvPara) == erSUCCESS) {
				halRTC_SetTime(*(u64_t *) pvPara);
				xRtosSetStatus(flagNET_SNTP);
				NtpDelay = pdMS_TO_TICKS(sntpMS_REFRESH);
			} else {
				SL_ERR("Failed to update time");
				NtpDelay = pdMS_TO_TICKS(sntpMS_RETRY);
			}
		}
		NtpLWtime = xTaskGetTickCount() - NtpLWtime;
		xRtosTaskWaitDELETE(taskSNTP_MASK, NtpDelay - NtpLWtime);
	}
	xRtosClearStatus(flagNET_SNTP);
	vRtosTaskDelete(NULL);
}

void vSntpStart(void * pvPara) {
	xRtosTaskCreateStatic(vSntpTask, "sntp", sntpSTACK_SIZE, pvPara, sntpPRIORITY, tsbSNTP, &ttsSNTP, tskNO_AFFINITY);
}
