/*
 * WiNoT Wireless Network of Things; Ethernet over iSLER
 */

#define ISLER_CALLBACK isler_rx_isr
void ISLER_CALLBACK(void); // this should have been in extralibs/iSLER.h
#include "iSLER.h"

#define WINOT_AP_ACCESSADDRESS 0x63683332
#define WINOT_AP_CHANNEL       35
#define WINOT_AP_CLIENTS_MAX   32
#define WINOT_FRAGMENT_SIZE    250 // can be optimized a bit I think
#define WINOT_DATA_BUF_SIZE    1600 // should fit an MTU
__attribute__((aligned(4))) static volatile uint8_t gs_winot_data_buf[WINOT_DATA_BUF_SIZE];
static int16_t gs_winot_data_len;

#define WINOT_FRAGMENTED       0x08
#define WINOT_FRAGMENT(f)      (f & 0x07)
typedef enum {
	WINOT_CLIENT,
	WINOT_AP,
} WiNot_role;
static WiNot_role role;

typedef enum {
	WINOT_IDLE = 0,
	WINOT_LISTENING,
	WINOT_REQUESTING,
	WINOT_NEGOTIATING,
	WINOT_COMMUNICATING,
	WINOT_NEWRX,
} WiNot_state;
static WiNot_state state;
static int current_tick;
static int last_comm_tick[WINOT_AP_CLIENTS_MAX];

void isler_rx_isr() {
	uint8_t *frame = (uint8_t *)LLE_BUF;

	if(role == WINOT_AP && state == WINOT_LISTENING) {
		uint8_t len = frame[4];
		// memcpy((uint8_t*)gs_winot_data_buf, &frame[5], len);
		memcpy((uint8_t*)gs_winot_data_buf, frame, 5);
		gs_winot_data_len = len;
		state = WINOT_NEGOTIATING;
	}
	else if(state == WINOT_COMMUNICATING) {
		uint8_t pdu = frame[0];
		uint8_t len = frame[1];
		uint8_t frag = WINOT_FRAGMENT(pdu);

		if(frag) {
			int write_idx = (frag * WINOT_FRAGMENT_SIZE);
			if(pdu & WINOT_FRAGMENTED) {
				memcpy((uint8_t*)&gs_winot_data_buf[write_idx], &frame[2], len);
				state = WINOT_COMMUNICATING;
			}
			else {
				// last fragment
				memcpy((uint8_t*)&gs_winot_data_buf[write_idx], &frame[2], len);
				gs_winot_data_len = write_idx +len;
				state = WINOT_NEWRX;
			}
		}
		else {
			// not fragmented
			memcpy((uint8_t*)gs_winot_data_buf, &frame[2], len);
			gs_winot_data_len = len;
			state = WINOT_NEWRX;
		}
	}
}

WiNot_state winot_init(WiNot_role r, int tx_power) {
	role = r;
	iSLERInit(tx_power);
	if(role == WINOT_AP) {
		iSLERRX(WINOT_AP_ACCESSADDRESS, WINOT_AP_CHANNEL, PHY_1M);
		state = WINOT_LISTENING;
	}

	return state;
}

WiNot_state winot_tick(int dt_ms) {
	current_tick++;
	if(state == WINOT_NEGOTIATING) {
		printf("client uuid: %02x %02x %02x %02x, len=%d\n", gs_winot_data_buf[0], gs_winot_data_buf[1], gs_winot_data_buf[2], gs_winot_data_buf[3], gs_winot_data_buf[4]);
		iSLERRX(WINOT_AP_ACCESSADDRESS, WINOT_AP_CHANNEL, PHY_1M);
		state = WINOT_LISTENING;
	}
	return state;
}
