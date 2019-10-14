/*
 * Copyright 2014-19 Andre M Maree / KSS Technologies (Pty) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * x_sntp_client.c
 */

#include	"x_config.h"
#include 	"x_sntp_client.h"
#include	"x_sockets.h"
#include	"x_errors_events.h"
#include	"x_syslog.h"

#include	"hal_rtc.h"

#include	<limits.h>
#include	<string.h>
#include	<math.h>

#define	debugFLAG						0x0000

#define	debugPROTOCOL					(debugFLAG * 0x0001)
#define	debugHOSTS						(debugFLAG * 0x0002)
#define	debugCALCULATION				(debugFLAG * 0x0004)

// ############################################ Macros  ############################################

#define	STRATUM_IDX(x)	((x >= specNTP_STRATUM_RSVD_LO) ? 4 : (x == specNTP_STRATUM_UNSYNC)	? 3 : (x >= specNTP_STRATUM_SEC_LO)	? 2 : x )

// ###################################### local ie static variables ################################

ntp_t		sNtpBuf ;
int16_t		NtpHostIndex = 0 ;
uint64_t	tNTP[4] ;
int64_t		tRTD, tOFF ;

const char * NtpHostTable[] = {
#if 0
	"ntp1.meraka.csir.co.za",
	"ntp1.neology.co.za",
	"ntp2.meraka.csir.co.za",
	"ntp2.neology.co.za",
	"0.za.pool.ntp.org",
	"1.za.pool.ntp.org",
	"2.za.pool.ntp.org",
	"3.za.pool.ntp.org",
#else
	"0.pool.ntp.org",
	"1.pool.ntp.org",
	"2.pool.ntp.org",
	"3.pool.ntp.org",
#endif
} ;

#define		NTP_TABLE_SIZE			( sizeof(NtpHostTable) / sizeof(NtpHostTable[0]) )

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
void	vNtpDebug(void) {
	const char *	LI_mess[]	= { "None", "61Sec", "59Sec", "Alarm" } ;
	const char *	Mode_mess[]	= { "Unspec", "SymAct", "SymPas", "Client", "Server", "BCast", "RsvdNTP", "RsvdPriv" } ;
	const char *	Strat_mess[]= { "KofD", "Prim", "Sec", "UnSync" , "Rsvd" } ;
	// Display the header info
	PRINT("[NTP] LI[%s] V[%u] Mode[%s] Stratum[%s] Poll[%.1fs] Precision[%fuS]\n",
			LI_mess[sNtpBuf.LI], sNtpBuf.VN, Mode_mess[sNtpBuf.Mode], Strat_mess[STRATUM_IDX(sNtpBuf.Stratum)],
			pow(2, (double) sNtpBuf.Poll), pow(2, (double) sNtpBuf.Precision) * 1000000) ;
	PRINT("[NTP] Root Delay[%d.%04d Sec]\n", ntohs(sNtpBuf.RDelUnit), ntohs(sNtpBuf.RDelFrac) / (UINT16_MAX/10000)) ;
	PRINT("[NTP] Dispersion[%u.%04u Sec]\n", ntohs(sNtpBuf.RDisUnit), ntohs(sNtpBuf.RDisFrac) / (UINT16_MAX/10000)) ;
	if (sNtpBuf.Stratum <= specNTP_STRATUM_PRI) {
		PRINT("[NTP] Ref ID[%4s]\n", &sNtpBuf.RefID) ;
	} else {
		PRINT("[NTP] Ref IP[%-I]\n", ntohl(sNtpBuf.RefIP)) ;
	}

// determine and display the reference timestamp
	uint64_t tTemp	= xNTPCalcValue(sNtpBuf.Ref.secs, sNtpBuf.Ref.frac) ;
	static TSZ_t tt ;
	tt.usecs = tTemp ;
	PRINT("[NTP] Ref: %Z\n", &tt) ;
// Display the 4 different timestamps
	tt.usecs = tNTP[0] ;
	PRINT("[NTP] (t0) %Z\n", &tt) ;
	tt.usecs = tNTP[1] ;
	PRINT("[NTP] (t1) %Z\n", &tt) ;
	tt.usecs = tNTP[2] ;
	PRINT("[NTP] (t2) %Z\n", &tt) ;
	tt.usecs = tNTP[3] ;
	PRINT("[NTP] (t3) %Z\n", &tt) ;
}

/*
 * vNtpCalcCorrectTime()
 */
void	vNtpCalcCorrectTime(uint64_t * pTStamp) {
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
	IF_SL_DBG(debugCALCULATION, "'%s' %R tOFF=%'lld uS tRTD=%'lld uS", NtpHostTable[NtpHostIndex], *pTStamp, tOFF, tRTD) ;
}

int32_t	xNtpRequestInfo(netx_t * psNtpCtx, uint64_t * pTStamp) {
	memset(&sNtpBuf, 0, sizeof(ntp_t)) ;
	sNtpBuf.VN			= specNTP_VERSION_V4 ;
	sNtpBuf.Mode		= specNTP_MODE_CLIENT ;

	// Plug in current (possibly very wrong) client transmit time
	sNtpBuf.Xmit.secs 	= htonl(xTimeStampAsSeconds(*pTStamp) + EPOCH_SECONDS_DIFFERENCE) ;
	sNtpBuf.Xmit.frac	= htonl((*pTStamp % MICROS_IN_SECOND) * FRACTIONS_PER_MICROSEC) ;

	// send the formatted request
	int32_t iRV = xNetWrite(psNtpCtx, (char *) &sNtpBuf, sizeof(ntp_t)) ;
	if (iRV == sizeof(ntp_t)) {
		xNetSetRecvTimeOut(psNtpCtx, 400) ;
		iRV = xNetRead(psNtpCtx, (char *) &sNtpBuf, sizeof(ntp_t)) ;
	}
	if (iRV != sizeof(ntp_t)) {
		return iRV ;
	}
	// expect only server type responses with correct version and stratum
	if (sNtpBuf.Mode != specNTP_MODE_SERVER ||
		sNtpBuf.VN != specNTP_VERSION_V4	||
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
int32_t xNtpGetTime(uint64_t * pTStamp) {
	netx_t	sNtpCtx ;
	memset(&sNtpCtx, 0, sizeof(netx_t)) ;
	sNtpCtx.sa_in.sin_family	= AF_INET ;
	sNtpCtx.sa_in.sin_port		= htons(IP_PORT_NTP) ;
	sNtpCtx.type				= SOCK_DGRAM ;
#if 0
	sNtpCtx.d_open				= 1 ;
	sNtpCtx.d_write				= 1 ;
	sNtpCtx.d_read				= 1 ;
#endif
	for (int32_t iRV = -1; iRV != sizeof(ntp_t) ; ) {
		sNtpCtx.pHost	= NtpHostTable[NtpHostIndex] ;
		IF_PRINT(debugHOSTS, "Connecting to host %s\n", sNtpCtx.pHost) ;
		iRV = xNetOpen(&sNtpCtx) ;
		if (iRV >= erSUCCESS) {
			iRV = xNtpRequestInfo(&sNtpCtx, pTStamp) ;	// send the sNtpBuf request & check the result
		}
		xNetClose(&sNtpCtx) ;							// close, & ignore return code..
		if (iRV != sizeof(ntp_t)) {
			vTaskDelay(pdMS_TO_TICKS(1000)) ;			// wait 1 seconds
			++NtpHostIndex ;
			NtpHostIndex %= NTP_TABLE_SIZE ;			// failed, step to next host...
		}
	}

	// end of loop, must have a valid HOST
	vNtpCalcCorrectTime(pTStamp) ;						// calculate & update correct time
	SL_INFO("%s(%#-I)  %R  tOFF=%'llduS  tRTD=%'llduS", NtpHostTable[NtpHostIndex], sNtpCtx.sa_in.sin_addr.s_addr, *pTStamp, tOFF, tRTD) ;
	IF_EXEC_0(debugPROTOCOL, vNtpDebug) ;
	return erSUCCESS ;
}

/*
 * vSntpTask()
 */
void	vSntpTask(void * pvPara) {
	IF_TRACK(debugAPPL_THREADS, debugAPPL_MESS_UP) ;
	xRtosSetStateRUN(taskSNTP) ;

	while (bRtosVerifyState(taskSNTP)) {
		if ((xRtosWaitStatus(flagL3_STA, pdMS_TO_TICKS(100)) & flagL3_STA) == 0) {
			continue ;									// first wait till IP is up and running
		}
		TickType_t	NtpLWtime = xTaskGetTickCount();	// Get the current time as a reference to start our delays.
		if (xNtpGetTime((uint64_t *) pvPara) == erSUCCESS) {
			halRTC_SetTime(*(uint64_t *) pvPara) ;
			xRtosSetStatus(flagNET_SNTP) ;
		} else {
			SL_ERR("Failed to update time") ;
		}
		NtpLWtime = xTaskGetTickCount() - NtpLWtime ;
		xRtosWaitStateDELETE(taskSNTP, pdMS_TO_TICKS(sntpINTERVAL_MS) - NtpLWtime) ;
	}
	xRtosClearStatus(flagNET_SNTP) ;
	IF_TRACK(debugAPPL_THREADS, debugAPPL_MESS_DN) ;
	vTaskDelete(NULL) ;
}

void	vTaskSntpInit(uint64_t * pTStamp) { xRtosTaskCreate(vSntpTask, "SNTP", sntpSTACK_SIZE, pTStamp, sntpPRIORITY, NULL, INT_MAX) ; }
