// x_sntp_client.c - Copyright )c) 2014-24 Andre M. Maree / KSS Technologies (Pty) Ltd.

#include "hal_platform.h"

#include "hal_rtc.h"
#include "x_sntp_client.h"
#include "printfx.h"									// +x_definitions +stdarg +stdint +stdio
#include "socketsX.h"
#include "syslog.h"
#include "errors_events.h"

#include <math.h>

// ############################################ Macros  ############################################

#define	debugFLAG					0xF000
#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

#define sntpMS_REFRESH				(60 * SECONDS_IN_MINUTE * MILLIS_IN_SECOND)
#define sntpMS_RETRY				(1 * SECONDS_IN_MINUTE * MILLIS_IN_SECOND)

#define	STRATUM_IDX(x)				((x >= specNTP_STRATUM_RSVD_LO) ? 4 : \
									(x == specNTP_STRATUM_UNSYNC)	? 3 : \
									(x >= specNTP_STRATUM_SEC_LO)	? 2 : x )

// ###################################### local ie static variables ################################

StaticTask_t ttsSNTP = { 0 };
StackType_t tsbSNTP[sntpSTACK_SIZE] = { 0 };

ntp_t sNtpBuf;
u64_t TimeOld, TimeNew;
i64_t tRTD, tOFF;
int NtpHostIndex = 0;

/**
 * @brief	convert NTP epoch NETWORK seconds/fractions to UNIX epoch HOST microseconds
 * @return
 */
u64_t xNTPCalcValue(u32_t Secs, u32_t Frac) {
	u64_t u64Val1 = ntohl(Secs) - EPOCH_SECONDS_DIFFERENCE;	// difference between NTP and selected epoch
	u64Val1 *= MICROS_IN_SECOND;							// convert Secs to uSec
	u64_t u64Val2 = ntohl(Frac) / FRACTIONS_PER_MICROSEC;	// convert Frac to uSec
	return u64Val1 + u64Val2;
}

/**
 * @brief
 * @param	psNtpCtx pointer to network connection context
 * @param	pTStamp pointer to u64_t location for storing correct time
 * @return	size of NTP packet or error code
*/
int	xNtpRequestInfo(netx_t * psNtpCtx, u64_t * pTStamp) {
	memset(&sNtpBuf, 0, sizeof(ntp_t));
	sNtpBuf.VN = specNTP_VERSION_V4;
	sNtpBuf.Mode = specNTP_MODE_CLIENT;
	// Plug in current (possibly very wrong) client transmit time
	TimeOld = *pTStamp;									// save time at start of request
	sNtpBuf.Xmit.secs = htonl((TimeOld / MICROS_IN_SECOND) + EPOCH_SECONDS_DIFFERENCE);
	sNtpBuf.Xmit.frac = htonl((TimeOld % MICROS_IN_SECOND) * FRACTIONS_PER_MICROSEC);
	// send the formatted request
	int iRV = xNetSend(psNtpCtx, (u8_t *) &sNtpBuf, sizeof(ntp_t));
	if (iRV != sizeof(ntp_t)) goto exit;
	xNetSetRecvTO(psNtpCtx, 400);
	iRV = xNetRecv(psNtpCtx, (u8_t *) &sNtpBuf, sizeof(ntp_t));
	if (iRV != sizeof(ntp_t)) goto exit;
	// expect only server type responses with correct version and stratum
	if (sNtpBuf.Mode != specNTP_MODE_SERVER || sNtpBuf.VN != specNTP_VERSION_V4 ||
		OUTSIDE(specNTP_STRATUM_PRI, sNtpBuf.Stratum, specNTP_STRATUM_SEC_HI)) {
		SL_ERR("Host=%s  Mode=%d  Ver=%d  Stratum=1/%d/15", psNtpCtx->pHost, sNtpBuf.Mode, sNtpBuf.VN, sNtpBuf.Stratum);
   		return erFAILURE;
   	}
exit:
	return iRV;
}

/**
 * @brief	Starts the SNTP task
 */
void vSntpTask(void * pTStamp) {
	int iRV;
	u64_t tNTP[3];
	netx_t sNtpCtx = { 0 };
	sNtpCtx.sa_in.sin_family = AF_INET;
	sNtpCtx.sa_in.sin_port = htons(IP_PORT_NTP);
	sNtpCtx.type = SOCK_DGRAM;
	sNtpCtx.flags = SO_REUSEADDR;
	vTaskSetThreadLocalStoragePointer(NULL, buildFRTLSP_EVT_MASK, (void *)taskSNTP_MASK);
	xRtosSetTaskRUN(taskSNTP_MASK);
	while (bRtosTaskWaitOK(taskSNTP_MASK, portMAX_DELAY)) {
		TickType_t NtpDelay = 0, NtpLWtime = xTaskGetTickCount();
		if (xRtosWaitStatus(flagLX_STA, pdMS_TO_TICKS(sntpMS_REFRESH - sntpMS_RETRY))) {
			// Find next host to connect to, connect and get SNTP info
			// Will stay in this loop as long as not successful connecting AND getting info
			char caHostName[24];
			for (iRV = -1; iRV != sizeof(ntp_t); ) {
				if (sNVSvars.GeoCode[0]) {
					snprintfx(caHostName, sizeof(caHostName), "%d.%>s.pool.ntp.org", NtpHostIndex, sNVSvars.GeoCode);
				} else {
					snprintfx(caHostName, sizeof(caHostName), "%d.pool.ntp.org", NtpHostIndex);
				}
				sNtpCtx.pHost = caHostName;
				IF_PX(debugHOSTS, "Connecting to host %s" strNL, sNtpCtx.pHost);
				iRV = xNetOpen(&sNtpCtx);
				if (iRV >= erSUCCESS)
					iRV = xNtpRequestInfo(&sNtpCtx, pTStamp);	// send the sNtpBuf request & check the result
				xNetClose(&sNtpCtx);					// close, & ignore return code..
				if (iRV == sizeof(ntp_t))				// Got a valid packet, HOORAY !
					break;
				vTaskDelay(pdMS_TO_TICKS(1000));		// wait 1 seconds
				++NtpHostIndex;
				NtpHostIndex %= 4;						// failed, step to next host...
			}
			{	// end of loop, must have a valid HOST and response, calculate the time...
				tNTP[0]	= xNTPCalcValue(sNtpBuf.Orig.secs , sNtpBuf.Orig.frac);
				tNTP[1]	= xNTPCalcValue(sNtpBuf.Recv.secs , sNtpBuf.Recv.frac);
				tNTP[2]	= xNTPCalcValue(sNtpBuf.Xmit.secs , sNtpBuf.Xmit.frac);
				// Calculate the Round Trip Delay
				i64_t tT0 = TimeOld - tNTP[0];
				i64_t tT1 = tNTP[2] - tNTP[1];
				tRTD = tT0 - tT1;
				// Then do the Offset in steps...
				tT0 = tNTP[1] - tNTP[0];
				tT1 = tNTP[2] - TimeOld;
				tOFF = (tT0 + tT1) / 2;
				// Houston, we have updated time...
				TimeNew = tNTP[0] + tRTD + tOFF;			// save the new time
				halRTC_SetTime(*(u64_t*)pTStamp = TimeNew);	// Immediately make available for use
				xRtosSetStatus(flagNET_SNTP);
				SL_NOT("%s(%#-I)  %.6R  Adj=%!.6R", caHostName, sNtpCtx.sa_in.sin_addr.s_addr, TimeNew, tOFF - tRTD);
			}
				}
			}
			NtpDelay = pdMS_TO_TICKS(sntpMS_REFRESH);
		{	// generate debug output
		#if 0
			const char * const LI_mess[] = { "None", "61Sec", "59Sec", "Alarm" };
			const char * const Mode_mess[] = { "Unspec", "SymAct", "SymPas", "Client", "Server", "BCast", "RsvdNTP", "RsvdPriv" };
			const char * const Strat_mess[] = { "KofD", "Prim", "Sec", "UnSync" , "Rsvd" };
			// Display the header info
			wprintfx(psR, "[NTP] LI[%s] V[%u] Mode[%s] Stratum[%s] Poll[%.1fs] Precision[%fuS]" strNL,
				LI_mess[sNtpBuf.LI], sNtpBuf.VN, Mode_mess[sNtpBuf.Mode], Strat_mess[STRATUM_IDX(sNtpBuf.Stratum)],
				pow(2, (double) sNtpBuf.Poll), pow(2, (double) sNtpBuf.Precision) * 1000000);
			wprintfx(NULL, "[NTP] Root Delay[%d.%04d Sec]" strNL, ntohs(sNtpBuf.RDelUnit), ntohs(sNtpBuf.RDelFrac) / (UINT16_MAX/10000));
			wprintfx(NULL, "[NTP] Dispersion[%u.%04u Sec]" strNL, ntohs(sNtpBuf.RDisUnit), ntohs(sNtpBuf.RDisFrac) / (UINT16_MAX/10000));
			if (sNtpBuf.Stratum <= specNTP_STRATUM_PRI)	wprintfx(NULL, "[NTP] Ref ID[%4s]" strNL, &sNtpBuf.RefID);
			else										wprintfx(NULL, "[NTP] Ref IP[%-I]" strNL, sNtpBuf.RefIP);
			// determine and display the reference timestamp
			wprintfx(NULL, "[NTP] Ref: %.6R" strNL, xNTPCalcValue(sNtpBuf.Ref.secs, sNtpBuf.Ref.frac));
			wprintfx(NULL, "[NTP] (t0) %.6R" strNL, tNTP[0]);
			wprintfx(NULL, "[NTP] (t1) %.6R" strNL, tNTP[1]);
			wprintfx(NULL, "[NTP] (t2) %.6R" strNL, tNTP[2]);
			wprintfx(NULL, "[NTP] (t3) %.6R" strNL, tNTP[3]);
		#endif
		}
		NtpLWtime = xTaskGetTickCount() - NtpLWtime;
		(void)xRtosWaitTaskDELETE(taskSNTP_MASK, NtpDelay - NtpLWtime);
	}
	xRtosClearStatus(flagNET_SNTP);
	vTaskDelete(NULL);
}

void vSntpStart(void * pTStamp) {
	xTaskCreateStaticPinnedToCore(vSntpTask, "sntp", sntpSTACK_SIZE, pTStamp, sntpPRIORITY, tsbSNTP, &ttsSNTP, tskNO_AFFINITY);
}
