#include "ch32fun.h"
#include <stdio.h>
#include "WiNot.h"

#define SFHIP_WARN( x... ) printf( x )
#define SFHIP_IMPLEMENTATION
#define HIP_PHY_HEADER_LENGTH_BYTES 0
#define SFHIP_TCP_SOCKETS 16

#include "sfhip.h"

#define HTTP_PORT 80

#ifdef CH570_CH572
#define LED PA9
#else
#define LED PA8
#endif

#ifndef ROM_CFG_MAC_ADDR // should be updated in ch5xxhw.h
#define ROM_CFG_MAC_ADDR        ((const u32*)0x0007f018)
#endif
#define MAC_PREFIX              0xc632 // should have used base 18

#define HTTP_RESPONSE_BUF_SIZE  512

static sfhip_phy_packet_mtu scratch __attribute__( ( aligned( 4 ) ) );

const char *hexlut = "0123456789abcdef";
char hname[] = "WiNot_XXXXXXXX";
char http_response_header[] = "HTTP/1.1 200 OK\r\n"
							  "Content-Type: text/plain\r\n"
							  "Content-Length: 16\r\n"
							  "Connection: close\r\n"
							  "\r\n";

// track which sockets have already sent their HTTP response
static bool response_sent[SFHIP_TCP_SOCKETS] = { false };
static int gs_has_ip;
static int gs_has_announced;


void blink(int n) {
	for(int i = n-1; i >= 0; i--) {
		funDigitalWrite( LED, FUN_LOW ); // Turn on LED
		Delay_Ms(33);
		funDigitalWrite( LED, FUN_HIGH ); // Turn off LED
		if(i) Delay_Ms(33);
	}
}

// ----------
// sfhip instrumentation
int sfhip_send_packet( sfhip *hip, sfhip_phy_packet *data, int length ) {
	// printf("sfhip sent frame of %d bytes over WiNoT\n", length);
	winot_release_packet();
	return winot_send_packet( (const uint8_t *)data, length );
}

void sfhip_got_dhcp_lease( sfhip *hip, sfhip_address addr ) {
	uint32_t ip = HIPNTOHL( addr );
	printf( "\nGot IP: %lu.%lu.%lu.%lu\n", ( ip >> 24 ) & 0xFF, ( ip >> 16 ) & 0xFF, ( ip >> 8 ) & 0xFF, ip & 0xFF );
	printf( "HTTP server ready at http://%lu.%lu.%lu.%lu/\n\n", ( ip >> 24 ) & 0xFF, ( ip >> 16 ) & 0xFF, ( ip >> 8 ) & 0xFF, ip & 0xFF );
	gs_has_ip = 1;
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
		char buf[HTTP_RESPONSE_BUF_SIZE] = {0};
		int response_len = sizeof( http_response_header ) - 1; // -1 to exclude null term
		memcpy( buf, http_response_header, (response_len > HTTP_RESPONSE_BUF_SIZE) ? HTTP_RESPONSE_BUF_SIZE : response_len );

		memcpy( &buf[response_len], hname, sizeof(hname));
		response_len += sizeof(hname) -1;
		buf[response_len++] = '\r';
		buf[response_len++] = '\n';

		memcpy( ip_payload, buf, (response_len > max_out_payload) ? max_out_payload : response_len );
		response_sent[sockno] = true;
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


int announce_up() {
	uint8_t sidechannel_msg[] = "XXXXch32fun/Hi from UUUUUUUU!";
	int uuid_idx = strlen("XXXXch32fun/Hi from ");
	for(int i = 0; i < 4; i++) {
		sidechannel_msg[uuid_idx++] = hexlut[(((uint8_t*)ROM_CFG_MAC_ADDR)[i] >> 4) & 0xf];
		sidechannel_msg[uuid_idx++] = hexlut[((uint8_t*)ROM_CFG_MAC_ADDR)[i] & 0xf];
	}

	memcpy(sidechannel_msg, (uint8_t[]){192,168,1,12}, 4); // MQTT server
	int res_mqtt = winot_sidechannel_tx(WINOT_SIDECHANNEL_MQTT, sidechannel_msg, sizeof(sidechannel_msg) -1);
	Delay_Us(500); // don't overwhelm the AP
	memcpy(sidechannel_msg, (uint8_t[]){159,203,148,75}, 4); // ntfy.sh server
	int res_ntfy = winot_sidechannel_tx(WINOT_SIDECHANNEL_NTFY_SH, sidechannel_msg, sizeof(sidechannel_msg) -1);

	return ((res_mqtt > 0) && (res_ntfy > 0)) ? res_ntfy : 0;;
}

static inline void task_10ms() {}
static inline void task_100ms() {
	// printf(".");
	winot_request(NULL, 0); // ask if there is data
}
static inline void task_1s() {
	if(!gs_has_announced && gs_has_ip) {
		if(announce_up() > 0) {
			gs_has_announced = 1;
		}
	}
}
static inline void task_10s() {}
static inline void task_100s() {}

// ----------
int main( void ) {
	SystemInit();
	printf( "iSLER/WiNoT sfhip test (DHCP)\n" );

	funGpioInitAll();
	funPinMode( LED, GPIO_CFGLR_OUT_2Mhz_PP );

	hipmac mac = { { MAC_PREFIX >> 8, MAC_PREFIX & 0xff, 0, 0, 0, 0 } };
	int hostname_mac_idx = strlen("WiNot_");
	for(int i = 0; i < 4; i++) {
		mac.mac[i +2] = ((uint8_t*)ROM_CFG_MAC_ADDR)[i];
		hname[hostname_mac_idx++] = hexlut[(((uint8_t*)ROM_CFG_MAC_ADDR)[i] >> 4) & 0xf];
		hname[hostname_mac_idx++] = hexlut[((uint8_t*)ROM_CFG_MAC_ADDR)[i] & 0xf];
	}

	sfhip hip = {
		.ip = 0,
		.mask = 0,
		.gateway = 0,
		.self_mac = mac, // will be populated from part uuid
		.hostname = hname,
		.need_to_discover = 1, // start w/ DHCP DISCOVER
	};

	winot_init(WINOT_CLIENT, LL_TX_POWER_0_DBM);

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
			if(winot_state() == WINOT_NEWRX) {
				winot_release_packet();
			}
			// printf("sfhip accepted frame of %d bytes\n", pkt_len);
		}
		else if ( pkt ) {
			// exists but oversized
			winot_release_packet();
			// printf("sfhip discarded frame of %d bytes\n", pkt_len);
		}

		if ( next_ms < funSysTick32() ) { // DONT FORGET TO FIX THIS! IT WILL RUN OUT THE UINT32 AND STOP
			next_ms += Ticks_from_Ms(1);
			ms_cnt++;
			sfhip_tick( &hip, &scratch, /*dt_ms*/1 );
			winot_tick( /*dt_ms*/1 );

			if(!(ms_cnt %10)) task_10ms();
			if(!((ms_cnt %100) -5)) task_100ms();
			if(!((ms_cnt %1000) -10)) task_1s();
			if(!((ms_cnt %10000) -20)) task_10s();
			if(!((ms_cnt %100000) -30)) task_100s();
		}
	}
}
