/*
 * Copyright 2014-21 Andre M. Maree / KSS Technologies (Pty) Ltd.
 */

#include 	"x_sntp_client.h"
#include	"hal_variables.h"
#include	"printfx.h"									// +x_definitions +stdarg +stdint +stdio
#include	"socketsX.h"
#include	"syslog.h"
#include	"x_errors_events.h"

#include	"hal_rtc.h"

#include	<math.h>
#include	<string.h>

#define	debugFLAG					0xF000

#define	debugPROTOCOL				(debugFLAG & 0x0001)
#define	debugHOSTS					(debugFLAG & 0x0002)
#define	debugCALCULATION			(debugFLAG & 0x0004)

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ############################################ Macros  ############################################

#define	STRATUM_IDX(x)	((x >= specNTP_STRATUM_RSVD_LO) ? 4 : (x == specNTP_STRATUM_UNSYNC)	? 3 : (x >= specNTP_STRATUM_SEC_LO)	? 2 : x )

// ###################################### local ie static variables ################################

ntp_t		sNtpBuf ;
int16_t		NtpHostIndex = 0 ;
uint64_t	tNTP[4] ;
int64_t		tRTD, tOFF ;

const char * const NtpHostTable[] = {
	"0.pool.ntp.org",	"1.pool.ntp.org",	"2.pool.ntp.org",	"3.pool.ntp.org",
} ;

/*
 * xNTPCalcValue() convert NTP epoch NETWORK seconds/fractions to UNIX epoch HOST microseconds
 */
uint64_t xNTPCalcValue(uint32_t Secs, uint32_t Frac) {
	uint64_t u64Val1 = ntohl(Secs) - EPOCH_SECONDS_DIFFERENCE ;	// difference between NTP and selected epoch
	u64Val1 *= MICROS_IN_SECOND ;						// convert Secs to uSec
	uint64_t u64Val2 = ntohl(Frac) / FRACTIONS_PER_MICROSEC ;	// convert Frac to uSec
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
	printfx("[NTP] LI[%s] V[%u] Mode[%s] Stratum[%s] Poll[%.1fs] Precision[%fuS]\n",
			LI_mess[sNtpBuf.LI], sNtpBuf.VN, Mode_mess[sNtpBuf.Mode], Strat_mess[STRATUM_IDX(sNtpBuf.Stratum)],
			pow(2, (double) sNtpBuf.Poll), pow(2, (double) sNtpBuf.Precision) * 1000000) ;
	printfx("[NTP] Root Delay[%d.%04d Sec]\n", ntohs(sNtpBuf.RDelUnit), ntohs(sNtpBuf.RDelFrac) / (UINT16_MAX/10000)) ;
	printfx("[NTP] Dispersion[%u.%04u Sec]\n", ntohs(sNtpBuf.RDisUnit), ntohs(sNtpBuf.RDisFrac) / (UINT16_MAX/10000)) ;
	if (sNtpBuf.Stratum <= specNTP_STRATUM_PRI)
		printfx("[NTP] Ref ID[%4s]\n", &sNtpBuf.RefID) ;
	else
		printfx("[NTP] Ref IP[%-I]\n", sNtpBuf.RefIP) ;
// determine and display the reference timestamp
	uint64_t tTemp	= xNTPCalcValue(sNtpBuf.Ref.secs, sNtpBuf.Ref.frac) ;
	printfx("[NTP] Ref: %.6R\n", tTemp);
	printfx("[NTP] (t0) %.6R\n", tNTP[0]);
	printfx("[NTP] (t1) %.6R\n", tNTP[1]);
	printfx("[NTP] (t2) %.6R\n", tNTP[2]);
	printfx("[NTP] (t3) %.6R\n", tNTP[3]);
}

/*
 * vNtpCalcCorrectTime()
 */
void vNtpCalcCorrectTime(uint64_t * pTStamp) {
	int64_t		tT0, tT1 ;
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

int	xNtpRequestInfo(netx_t * psNtpCtx, uint64_t * pTStamp) {
	memset(&sNtpBuf, 0, sizeof(ntp_t)) ;
	sNtpBuf.VN	= specNTP_VERSION_V4 ;
	sNtpBuf.Mode= specNTP_MODE_CLIENT ;

	// Plug in current (possibly very wrong) client transmit time
	sNtpBuf.Xmit.secs 	= htonl(xTimeStampAsSeconds(*pTStamp) + EPOCH_SECONDS_DIFFERENCE) ;
	sNtpBuf.Xmit.frac	= htonl((*pTStamp % MICROS_IN_SECOND) * FRACTIONS_PER_MICROSEC) ;

	// send the formatted request
	int iRV = xNetWrite(psNtpCtx, (char *) &sNtpBuf, sizeof(ntp_t)) ;
	if (iRV == sizeof(ntp_t)) {
		xNetSetRecvTimeOut(psNtpCtx, 400) ;
		iRV = xNetRead(psNtpCtx, (char *) &sNtpBuf, sizeof(ntp_t)) ;
	}
	if (iRV != sizeof(ntp_t)) return iRV;
	// expect only server type responses with correct version and stratum
	if (sNtpBuf.Mode != specNTP_MODE_SERVER || sNtpBuf.VN != specNTP_VERSION_V4 ||
		OUTSIDE(specNTP_STRATUM_PRI, sNtpBuf.Stratum, specNTP_STRATUM_SEC_HI, uint8_t)) {
		SL_ERR("Host=%s  Mode=%d  Ver=%d  Stratum=%d", psNtpCtx->pHost, sNtpBuf.Mode, sNtpBuf.VN, sNtpBuf.Stratum) ;
   		return erFAILURE ;
   	}
	IF_PRINT(debugHOSTS, "Sync'ing with host %s\n", NtpHostTable[NtpHostIndex]) ;
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
int xNtpGetTime(uint64_t * pTStamp) {
	netx_t	sNtpCtx = { 0 } ;
	sNtpCtx.sa_in.sin_family	= AF_INET ;
	sNtpCtx.sa_in.sin_port		= htons(IP_PORT_NTP) ;
	sNtpCtx.type				= SOCK_DGRAM ;
#if 0
	sNtpCtx.d_open				= 1 ;
	sNtpCtx.d_write				= 1 ;
	sNtpCtx.d_read				= 1 ;
	sNtpCtx.d_data				= 1 ;
	sNtpCtx.d_eagain			= 1 ;
#endif
	for (int iRV = -1; iRV != sizeof(ntp_t) ; ) {
		sNtpCtx.pHost	= NtpHostTable[NtpHostIndex] ;
		IF_PRINT(debugHOSTS, "Connecting to host %s\n", sNtpCtx.pHost) ;
		iRV = xNetOpen(&sNtpCtx) ;
		if (iRV >= erSUCCESS)
			iRV = xNtpRequestInfo(&sNtpCtx, pTStamp) ;	// send the sNtpBuf request & check the result
		xNetClose(&sNtpCtx) ;							// close, & ignore return code..
		if (iRV != sizeof(ntp_t)) {
			vTaskDelay(pdMS_TO_TICKS(1000)) ;			// wait 1 seconds
			++NtpHostIndex ;
			NtpHostIndex %= NO_MEM(NtpHostTable) ;	// failed, step to next host...
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
	IF_PRINT(debugTRACK && ioB1GET(ioStart), debugAPPL_MESS_UP) ;
	vTaskSetThreadLocalStoragePointer(NULL, 1, (void *)taskSNTP_MASK) ;
	xRtosSetStateRUN(taskSNTP_MASK) ;

	while (bRtosVerifyState(taskSNTP_MASK)) {
		if (bRtosWaitStatusALL(flagLX_STA, pdMS_TO_TICKS(100)) == 0)
			continue;				// wait till IP running
		TickType_t	NtpLWtime = xTaskGetTickCount();	// Get the current time as a reference to start our delays.
		if (xNtpGetTime((uint64_t *) pvPara) == erSUCCESS) {
			halRTC_SetTime(*(uint64_t *) pvPara) ;
			xRtosSetStatus(flagNET_SNTP) ;
		} else
			SL_ERR("Failed to update time") ;
		NtpLWtime = xTaskGetTickCount() - NtpLWtime ;
		xRtosWaitStateDELETE(taskSNTP_MASK, pdMS_TO_TICKS(sntpINTERVAL_MS) - NtpLWtime) ;
	}
	xRtosClearStatus(flagNET_SNTP) ;
	IF_PRINT(debugTRACK && ioB1GET(ioRstrt), debugAPPL_MESS_DN) ;
	vTaskDelete(NULL) ;
}
