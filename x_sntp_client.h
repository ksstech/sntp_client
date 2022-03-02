/*
 * Copyright 2014-21 Andre M. Maree / KSS Technologies (Pty) Ltd.
 * x_sntp_client.h
 */

#pragma once

#include	"hal_config.h"
#include	"FreeRTOS_Support.h"

#ifdef __cplusplus
extern "C" {
#endif

// ###################################### BUILD : CONFIG definitions ###############################


// ######################################## enumerations ###########################################

enum {
	specNTP_VERSION_V1				= 1,
	specNTP_VERSION_V2				= 2,
	specNTP_VERSION_V3				= 3,
	specNTP_VERSION_V4				= 4,
} ;

enum {
	specNTP_MODE_RESERVED			= 0,
	specNTP_MODE_SYM_ACTV			= 1,
	specNTP_MODE_SYM_PASV			= 2,
	specNTP_MODE_CLIENT				= 3,
	specNTP_MODE_SERVER				= 4,
	specNTP_MODE_BROADCAST			= 5,
	specNTP_MODE_NTP_CONTROL		= 6,
	specNTP_MODE_PRIVATE			= 7,
} ;

enum {
	specNTP_STRATUM_KISS_O_DEATH	= 0,
	specNTP_STRATUM_PRI				= 1,
	specNTP_STRATUM_SEC_LO			= 2,
	specNTP_STRATUM_SEC_HI			= 15,
	specNTP_STRATUM_UNSYNC			= 16,
	specNTP_STRATUM_RSVD_LO			= 17,
	specNTP_STRATUM_RESERVED_HI		= 255,
} ;

// #################################### NTP packet structures ######################################

typedef union {
	uint64_t	val ;
	struct {
		uint32_t	secs ;
		uint32_t	frac ;
	} ;
} ntp_ts_t ;

/************************ NTP packet structure **********************
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |MSb		 LSb|MSb         LSb|MSb         LSb|MSb         LSb|
 	+-+-+-+-+-+-+-+-+-+-+-+- NTP Packet Header -+-+-+-+-+-+-+-+-+-+-+
    |0 1|2 3 4|5 6 7|8 9 0 1 2 3 4 5|6 7 8 9 0 1 2 3|4 5 6 7 8 9 0 1|
 	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x00|L I| V N |Mode |    Stratum    |     Poll      |   Precision   |
   	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x04|                          Root Delay                           |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x08|                        Root Dispersion                        |
  	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x0C|                     Reference Identifier                      |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x10|                    Reference Timestamp (64)                   |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x18|                    Originate Timestamp (64)                   |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x20|                     Receive Timestamp (64)                    |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x28|                     Transmit Timestamp (64)                   |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x30|                 Key Identifier (optional) (32)                |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
0x34|                 Message Digest (optional) (128)               |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    Getting the data from the Transmit Timestamp (seconds) field
This is the time at which the reply departed the server for the client
	https://www.meinbergglobal.com/english/info/ntp-packet.htm
	https://tools.ietf.org/html/rfc5905									*/

typedef	struct __attribute__((__packed__)) {
	struct	{									// Header fields
		uint8_t		Mode	: 3 ;				// Mode (0=resv,active,passive,client,server,broadcast,control,rsvd)
		uint8_t		VN		: 3 ;				// Version (currently 4)
		uint8_t		LI		: 2 ;				// Leap Indicator (0=no warning,1=LastMin=61,2=LastMin=59,3=unknown)
	} ;
	uint8_t		Stratum ;						// Stratum (0=unspec,1=pri,2-15=secondary,16=unsync,17-255=rsvd)
	uint8_t		Poll ;							// Poll Exponent (max intvl between mess in log2 sec, range 4(16s) -> 17(131072s))
	int8_t		Precision ;						// Precision of system clock in log2 sec (-18 = 1uSec)
	struct {									// 0x04: Root Delay
		int16_t		RDelUnit ;
		uint16_t	RDelFrac ;
	} ;
	struct {									// 0x08: Root Dispersion
		uint16_t	RDisUnit ;
		uint16_t	RDisFrac ;
	} ;
	union {										// 0x0C: Reference ID or IP
		uint8_t		RefID[4] ;
		uint32_t	RefIP ;
	} ;
	ntp_ts_t	Ref ;
	ntp_ts_t	Orig ;
	ntp_ts_t	Recv ;
	ntp_ts_t	Xmit ;
//	Only used with authorisation ...
//	uint32_t	KeyID ;
//	uint32_t	MessDigest[4] ;
} ntp_t ;

// ################################### Global variables ############################################

// ############################### Level 2 network functions #######################################

int xNtpGetTime(uint64_t *);
void vSntpStart(void * pvPara);

#ifdef __cplusplus
}
#endif
