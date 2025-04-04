// client-sntp.c - Copyright )c) 2014-25 Andre M. Maree / KSS Technologies (Pty) Ltd.

#include "hal_platform.h"

#include "client-sntp.h"
#include "hal_rtc.h"
#include "report.h"									// +x_definitions +stdarg +stdint +stdio
#include "socketsX.h"
#include "syslog.h"
#include "errors_events.h"

#include <math.h>

// ############################################ Macros  ############################################

#define	debugFLAG					0xF000
#define debugSTATS					(debugFLAG & 0x0001)
#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

#define sntpMS_REFRESH				(60 * SECONDS_IN_MINUTE * MILLIS_IN_SECOND)
#define sntpMS_RETRY				(1 * SECONDS_IN_MINUTE * MILLIS_IN_SECOND)
#define sntpMS_CHECK				(1 * MILLIS_IN_SECOND)

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
static TickType_t TickNow = 0, TickLastRun = 0, TickNextRun = 0;

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
	if (iRV != sizeof(ntp_t))
		goto exit;
	xNetSetRecvTO(psNtpCtx, 400);
	iRV = xNetRecv(psNtpCtx, (u8_t *) &sNtpBuf, sizeof(ntp_t));
	if (iRV != sizeof(ntp_t))
		goto exit;
	// expect only server type responses with correct version and stratum
	if ((sNtpBuf.Mode != specNTP_MODE_SERVER) || 		/* Invalid mode */
		(sNtpBuf.VN != specNTP_VERSION_V4) ||			/* Invalid version */
		OUTSIDE(specNTP_STRATUM_PRI, sNtpBuf.Stratum, specNTP_STRATUM_SEC_HI)) {	/* Stratum out of range */
		SL_NOT("Host=%s (%#-I) Mode=%d  Ver=%d  Stratum=1/%d/15", psNtpCtx->pHost,
			psNtpCtx->sa_in.sin_addr.s_addr, sNtpBuf.Mode, sNtpBuf.VN, sNtpBuf.Stratum);
   		return erFAILURE;
   	}
exit:
	return iRV;
}

/**
 * @brief	Starts the SNTP task
 */
static void vSntpTask(void * pTStamp) {
	int iRV;
	u64_t tNTP[3];
	char caHostName[24];
	netx_t sNtpCtx = { 0 };
	sNtpCtx.sa_in.sin_family = AF_INET;
	sNtpCtx.sa_in.sin_port = htons(IP_PORT_NTP);
	sNtpCtx.c.type = SOCK_DGRAM;
	sNtpCtx.flags = SO_REUSEADDR;

	halEventUpdateRunTasks(0, 1);
	while (halEventWaitTasksOK(0, portMAX_DELAY)) {
		if (halEventWaitStatus(flagLX_STA, pdMS_TO_TICKS(sntpMS_RETRY)) == 0)
			continue;
		TickNow = xTaskGetTickCount();					// single reference time for update timing
		if (TickNow < TickNextRun) {
			vTaskDelay(pdMS_TO_TICKS(sntpMS_CHECK));
			continue;
		}
		TickLastRun = TickNow;
		TickNextRun = TickLastRun + pdMS_TO_TICKS(sntpMS_REFRESH);
		do {
			iRV = 0;
			snprintfx(caHostName, sizeof(caHostName), sNVSvars.GeoCode[0] ? "%d.%>s.pool.ntp.org" : "%d.pool.ntp.org", NtpHostIndex, sNVSvars.GeoCode);
			sNtpCtx.pHost = caHostName;					/* Select NTP host */
			iRV = xNetOpen(&sNtpCtx);					/* Connect to selected host */
			if (iRV > erFAILURE)						/* If connected request SNTP info */
				iRV = xNtpRequestInfo(&sNtpCtx, pTStamp);
			xNetClose(&sNtpCtx);						/* close the session */
			if (iRV != sizeof(ntp_t)) {					/* invalid packet received, delay then step to next host */
				vTaskDelay(pdMS_TO_TICKS(sntpMS_RETRY));
				++NtpHostIndex;					
				NtpHostIndex %= 4;
			}
		} while(iRV != sizeof(ntp_t));
		{	// have a valid HOST and response, calculate the time...
			tNTP[0]	= xNTPCalcValue(sNtpBuf.Orig.secs, sNtpBuf.Orig.frac);
			tNTP[1]	= xNTPCalcValue(sNtpBuf.Recv.secs, sNtpBuf.Recv.frac);
			tNTP[2]	= xNTPCalcValue(sNtpBuf.Xmit.secs, sNtpBuf.Xmit.frac);
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
			halEventUpdateStatus(flagNET_SNTP, 1);
			SL_NOT("%s(%#-I)  %.6R  Adj=%!.6R", caHostName, sNtpCtx.sa_in.sin_addr.s_addr, TimeNew, tOFF - tRTD);
		}
		{	// generate debug output
		#if debugSTATS
			const char * const LI_mess[] = { "None", "61Sec", "59Sec", "Alarm" };
			const char * const Mode_mess[] = { "Unspec", "SymAct", "SymPas", "Client", "Server", "BCast", "RsvdNTP", "RsvdPriv" };
			const char * const Strat_mess[] = { "KofD", "Prim", "Sec", "UnSync" , "Rsvd" };
			// Display the header info
			PX("[NTP] LI[%s] V[%u] Mode[%s] Stratum[%s] Poll[%.1fs] Precision[%fuS]" strNL,
				LI_mess[sNtpBuf.LI], sNtpBuf.VN, Mode_mess[sNtpBuf.Mode], Strat_mess[STRATUM_IDX(sNtpBuf.Stratum)],
				pow(2, (double) sNtpBuf.Poll), pow(2, (double) sNtpBuf.Precision) * 1000000);
			PX("[NTP] Root Delay[%d.%04d Sec]" strNL, ntohs(sNtpBuf.RDelUnit), ntohs(sNtpBuf.RDelFrac) / (UINT16_MAX/10000));
			PX("[NTP] Dispersion[%u.%04u Sec]" strNL, ntohs(sNtpBuf.RDisUnit), ntohs(sNtpBuf.RDisFrac) / (UINT16_MAX/10000));
			if (sNtpBuf.Stratum <= specNTP_STRATUM_PRI)
				PX("[NTP] Ref ID[%4s]" strNL, &sNtpBuf.RefID);
			else
				PX("[NTP] Ref IP[%-I]" strNL, sNtpBuf.RefIP);
			// determine and display the reference timestamp
			PX("[NTP] Ref: %.6R" strNL, xNTPCalcValue(sNtpBuf.Ref.secs, sNtpBuf.Ref.frac));
			PX("[NTP] (t0) %.6R" strNL, tNTP[0]);
			PX("[NTP] (t1) %.6R" strNL, tNTP[1]);
			PX("[NTP] (t2) %.6R" strNL, tNTP[2]);
			PX("[NTP] (t3) %.6R" strNL, tNTP[3]);
		#endif
		}
	}
	vTaskDelete(NULL);
}

void vSntpStart(void * pTStamp) {
	const task_param_t sSntpParam = {
		.pxTaskCode = vSntpTask,
		.pcName = "sntp",
		.usStackDepth = sntpSTACK_SIZE,
		.uxPriority = sntpPRIORITY,
		.pxStackBuffer = tsbSNTP,
		.pxTaskBuffer = &ttsSNTP,
		.xCoreID = tskNO_AFFINITY,
		.xMask = taskSNTP_MASK,
	};
	xTaskCreateWithMask(&sSntpParam, pTStamp); 
}

int xSntpReport(report_t * psR) {
	return report(psR, "%CSNTP_C%C\tLast=%lu  Now=%lu  Next=%lu" strNL,xpfCOL(colourFG_CYAN,0), xpfCOL(attrRESET,0), TickLastRun/configTICK_RATE_HZ, TickNow/configTICK_RATE_HZ, TickNextRun/configTICK_RATE_HZ);
}
