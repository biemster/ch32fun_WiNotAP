#include "ch32fun.h"
#include <stdio.h>

#define SFHIP_WARN( x... ) printf( x )
#define SFHIP_IMPLEMENTATION
#define HIP_PHY_HEADER_LENGTH_BYTES 0
#define SFHIP_TCP_SOCKETS 16

#include "sfhip.h"

#ifdef USB_USE_USBD
// the USBD peripheral is not recommended, but there are really nice
// v208 boards with only those pins brought out, so support is hacked in
#include "usbd.h"
#define USBFSSetup         USBDSetup
#define USBFS_SetupReqLen  USBD_SetupReqLen
#define USBFS_SetupReqType USBD_SetupReqType
#define USBFS_PACKET_SIZE  DEF_USBD_UEP0_SIZE
int USBFS_SendEndpointNEW(int ep, uint8_t *data, int len, int copy) { return USBD_SendEndpoint(ep, data, len); }
#else
#include "fsusb.h"
#endif

#define EP_CDC_IRQ   1
#define EP_CDC_OUT   2
#define EP_CDC_IN    3
#define EP_FRAME_OUT 6
#define EP_FRAME_IN  5

#define HTTP_PORT 80

#ifdef CH570_CH572
#define LED PA9
#else
#define LED PA8
#endif

sfhip hip = {
	.ip = 0,
	.mask = 0,
	.gateway = 0,
	.self_mac = { { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } }, // will be populated from part uuid
	.hostname = "WiNotAP",
	.need_to_discover = 1, // start w/ DHCP DISCOVER
};

static sfhip_phy_packet_mtu scratch __attribute__( ( aligned( 4 ) ) );

const char http_response[] = "HTTP/1.1 200 OK\r\n"
							 "Content-Type: text/plain\r\n"
							 "Content-Length: 22\r\n"
							 "Connection: close\r\n"
							 "\r\n"
							 "Hello from WiNotAP!\r\n";

// track which sockets have already sent their HTTP response
static bool response_sent[SFHIP_TCP_SOCKETS] = { false };

#if !defined(FUNCONF_USE_DEBUGPRINTF) || !FUNCONF_USE_DEBUGPRINTF
int _write(int fd, const char *buf, int size) {
	if(USBFS_SendEndpointNEW(EP_CDC_IN, (uint8_t*)buf, size, /*copy*/1) == -1) { // -1 == busy
		// wait for 1ms to try again once more
		Delay_Ms(1);
		USBFS_SendEndpointNEW(EP_CDC_IN, (uint8_t*)buf, size, /*copy*/1);
	}
	return size;
}

int putchar(int c) {
	uint8_t single = c;
	if(USBFS_SendEndpointNEW(EP_CDC_IN, &single, 1, /*copy*/1) == -1) { // -1 == busy
		// wait for 1ms to try again once more
		Delay_Ms(1);
		USBFS_SendEndpointNEW(EP_CDC_IN, &single, 1, /*copy*/1);
	}
	return 1;
}
#endif

void blink(int n) {
	for(int i = n-1; i >= 0; i--) {
		funDigitalWrite( LED, FUN_LOW ); // Turn on LED
		Delay_Ms(33);
		funDigitalWrite( LED, FUN_HIGH ); // Turn off LED
		if(i) Delay_Ms(33);
	}
}

// -----------------------------------------------------------------------------
// IN Handler (Called by IRQ when Packet Sent to PC)
// -----------------------------------------------------------------------------
int HandleInRequest(struct _USBState *ctx, int endp, uint8_t *data, int len) {
	return 0;
}

// -----------------------------------------------------------------------------
// OUT Handler (Called by IRQ when Packet Received from PC)
// -----------------------------------------------------------------------------
void HandleDataOut(struct _USBState *ctx, int endp, uint8_t *data, int len) {
	if (endp == 0) {
		ctx->USBFS_SetupReqLen = 0;
	}
	else if( endp == EP_CDC_OUT ) {
		// cdc tty input
		
	}
	else if (endp == EP_FRAME_OUT) {
		if(len == 4 && ((uint32_t*)data)[0] == 0x010001a2) {
			USBFSReset();
			blink(2);
			jump_isprom();
		}
		else {
			// received (part of) an ethernet frame
		}

		ctx->USBFS_Endp_Busy[EP_FRAME_OUT] = 0;
	}
}

int HandleSetupCustom( struct _USBState *ctx, int setup_code ) {
	int ret = -1;
	if ( ctx->USBFS_SetupReqType & USB_REQ_TYP_CLASS ) {
		switch ( setup_code ) {
		case CDC_SET_LINE_CODING:
		case CDC_SET_LINE_CTLSTE:
		case CDC_SEND_BREAK:
			ret = ( ctx->USBFS_SetupReqLen ) ? ctx->USBFS_SetupReqLen : -1;
			break;
		case CDC_GET_LINE_CODING:
			ret = ctx->USBFS_SetupReqLen;
			break;
		default:
			ret = 0;
			break;
		}
	}
	else {
		ret = 0; // Go to STALL
	}
	return ret;
}

// ----------
// WiNot rx/tx
int winot_send_packet(const uint8_t *data, int length) {
	for(int i = 0; i < length; i += USBFS_PACKET_SIZE) {
		USBFS_SendEndpointNEW(EP_FRAME_IN, (uint8_t*)&data[i], ((length -i > USBFS_PACKET_SIZE) ? USBFS_PACKET_SIZE : (length -i)), /*copy*/0);
	}
	printf("sent frame of %d bytes\n", length);
	return length;
}

uint8_t* winot_poll_packet(uint16_t *length) {
	return NULL;
}

// ----------
// sfhip instrumentation
int sfhip_send_packet( sfhip *hip, sfhip_phy_packet *data, int length ) {
	return winot_send_packet( (const uint8_t *)data, length );
}

void sfhip_got_dhcp_lease( sfhip *hip, sfhip_address addr ) {
	uint32_t ip = HIPNTOHL( addr );
	printf( "\nGot IP: %lu.%lu.%lu.%lu\n", ( ip >> 24 ) & 0xFF, ( ip >> 16 ) & 0xFF, ( ip >> 8 ) & 0xFF, ip & 0xFF );
	printf( "HTTP server ready at http://%lu.%lu.%lu.%lu/\n\n", ( ip >> 24 ) & 0xFF, ( ip >> 16 ) & 0xFF, ( ip >> 8 ) & 0xFF, ip & 0xFF );
}

// called by sfhip when a new TCP connection arrives
// 0 to accept, -1 to reject
int sfhip_tcp_accept_connection( sfhip *hip, int sockno, int localport, hipbe32 remote_host ) {
	if ( localport == HIPHTONS( HTTP_PORT ) ) {
		return 0;
	}
	return -1;
}

// called when TCP data arrives or connection state changes
sfhip_length_or_tcp_code sfhip_tcp_event(
	sfhip *hip, int sockno, uint8_t *ip_payload, int ip_payload_length, int max_out_payload, int acked ) {
	// if we received data and haven't sent our response yet, send it now
	if ( ip_payload_length > 0 && !response_sent[sockno] ) {
		response_sent[sockno] = true;
		int response_len = sizeof( http_response ) - 1; // -1 to exclude null term
		if ( response_len > max_out_payload ) {
			response_len = max_out_payload;
		}
		memcpy( ip_payload, http_response, response_len );
		// printf( "." ); // debug: dot per request
		return response_len;
	}

	// was ACKed, close conn
	if ( acked > 0 && response_sent[sockno] ) {
		return SFHIP_TCP_OUTPUT_FIN;
	}

	return 0; // no action needed
}


void sfhip_tcp_socket_closed( sfhip *hip, int sockno ) {
	response_sent[sockno] = false;
}

// ----------
int main( void ) {
	SystemInit();
	printf( "USB sfhip test (DHCP)\n" );

	funGpioInitAll();
	funPinMode( LED, GPIO_CFGLR_OUT_2Mhz_PP );

	USBFSSetup();

	blink(5);
	uint32_t next_ms = funSysTick32() + Ticks_from_Ms(1);
	uint32_t ms_cnt = 0;
	while ( 1 ) {
		uint16_t pkt_len;
		const uint8_t *pkt = winot_poll_packet( &pkt_len );

		// process received packet if valid and within MTU limits
		if ( pkt && pkt_len > 0 && pkt_len <= SFHIP_MTU ) {
			// hand packet to sfhip for processing
			sfhip_accept_packet( &hip, (sfhip_phy_packet_mtu *)pkt, pkt_len );
		}
		else if ( pkt ) {
			// exists but oversized
		}

		if ( next_ms < funSysTick32() ) {
			next_ms += Ticks_from_Ms(1);
			ms_cnt++;
			sfhip_tick( &hip, &scratch, /*dt_ms*/1 );

			if(!(ms_cnt %100)) printf(".");
		}
	}
}
