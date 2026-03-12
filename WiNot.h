/*
 * WiNoT Wireless Network of Things; Ethernet over iSLER
 */

#define GLOBAL_RX_READY // set this to have the rx logic out of the IRQ scope,
						// needed when iSLERRX/TX is called in the handler
#ifndef GLOBAL_RX_READY
#define ISLER_CALLBACK_RX winot_process_rx
#endif
#include "iSLER.h"

#ifndef ROM_CFG_MAC_ADDR // should be updated in ch5xxhw.h
#define ROM_CFG_MAC_ADDR           ((const u32*)0x0007f018)
#endif

#define WINOT_ACCESSADDRESS_PRIV   (*ROM_CFG_MAC_ADDR)
#define WINOT_AP_ACCESSADDRESS     0x63683332
#define WINOT_AP_CHANNEL           35
#define WINOT_AP_CLIENTS_MAX       32
#define WINOT_FRAGMENT_SIZE        245 // can be optimized a bit I think
#define WINOT_FRAGMENTED           0x08
#define WINOT_FRAGMENTS_MAX        7
#define WINOT_FRAGMENT(f)          (f & 0x07)
#define WINOT_HAS_ALL_FRAGMENTS(f) (gs_winot_fragments_received == ((1 << (f+1)) -1))
#define WINOT_RESPONSE_WINDOW_MS   20
#define WINOT_ACTIVE_DATACHANNEL   0
#define WINOT_DATA_BUF_SIZE        (WINOT_FRAGMENT_SIZE * WINOT_FRAGMENTS_MAX) // =1715 should fit an MTU
#define WINOT_DATA_DEST_UUID(buf)  (buf[2] | (buf[3]<<8) | (buf[4]<<16) | (buf[5]<<24)) // 4 LSBs of destination MAC

__attribute__((aligned(4))) static volatile uint8_t gs_winot_data_buf[WINOT_DATA_BUF_SIZE];
static int16_t gs_winot_data_len;
static volatile uint8_t gs_winot_fragments_received; // we have 7 fragments max, this fits
static uint32_t gs_datachannel[WINOT_AP_CLIENTS_MAX +1]; // AP: channel is index, value is client uuid, gs_datachannel[0] = current data channel
static uint32_t gs_datachannel_last_activity_ms[WINOT_AP_CLIENTS_MAX +1];

typedef enum {
	WINOT_CLIENT,
	WINOT_AP,
} WiNot_role;
static WiNot_role gs_role;

typedef enum {
	WINOT_IDLE = 0,
	WINOT_LISTENING,
	WINOT_REQUESTING,
	WINOT_COMMUNICATING,
	WINOT_NEWRX,
} WiNot_state;
static WiNot_state gs_state;
static int gs_current_tick_ms;
static int gs_last_state_change_tick_ms;

static inline void winot_change_state(WiNot_state state_new) {
	switch(state_new) {
	case WINOT_IDLE:
		gs_datachannel[WINOT_ACTIVE_DATACHANNEL] = WINOT_AP_CHANNEL;
		// putchar('i');
		break;
	case WINOT_LISTENING:
		gs_datachannel[WINOT_ACTIVE_DATACHANNEL] = WINOT_AP_CHANNEL;
		iSLERRX(WINOT_AP_ACCESSADDRESS, WINOT_AP_CHANNEL, PHY_1M);
		// putchar('l');
		break;
	case WINOT_REQUESTING:
		iSLERRX(WINOT_ACCESSADDRESS_PRIV, WINOT_AP_CHANNEL, PHY_1M); // listen for response
		// putchar('r');
		break;
	case WINOT_COMMUNICATING:
		int channel = gs_datachannel[WINOT_ACTIVE_DATACHANNEL];
		iSLERRX((gs_role == WINOT_AP) ? gs_datachannel[channel] : WINOT_ACCESSADDRESS_PRIV, channel, PHY_1M);
		// putchar('c');
		// putchar(channel +'0');
		break;
	case WINOT_NEWRX:
		// putchar('n');
		break;
	}
	gs_last_state_change_tick_ms = gs_current_tick_ms;
	gs_state = state_new;
}

static inline WiNot_state winot_state() {
	return gs_state;
}

void winot_print_routing_table() {
	int c = 0;
	printf("\r\nrouting table:\r\n");
	while(gs_datachannel[++c]) {
		printf("ch%d: %08lx\r\n", c, gs_datachannel[c]);
	}
}

__HIGH_CODE
void winot_burst_tx_and_communicate(uint32_t access_address, uint8_t channel, const uint8_t *data, int len) {
	int sent = 0;
	int frag_idx = 0;

	while(sent < len) {
		int chunk = len - sent;
		uint8_t pdu = WINOT_FRAGMENT(frag_idx);
		// putchar('b');
		// putchar(frag_idx +'0');

		if(chunk > WINOT_FRAGMENT_SIZE) {
			chunk = WINOT_FRAGMENT_SIZE;
			pdu |= WINOT_FRAGMENTED;
		}

		__attribute__((aligned(4))) static uint8_t tx_buf[WINOT_FRAGMENT_SIZE +4]; // static to use global RAM, not stack
		tx_buf[0] = pdu;
		memcpy(&tx_buf[4], &data[sent], chunk);

		iSLERTX(access_address, tx_buf, chunk +4, channel, PHY_1M);

		Delay_Ms(1); // This is less than ideal, but RX on the other side is doing slow memcpy. Really though? this just did a memcpy too!
		gs_current_tick_ms += 1;

		sent += chunk;
		frag_idx++;
	}

	winot_change_state(WINOT_COMMUNICATING);
}

__HIGH_CODE
void winot_defragment(uint8_t *frame) {
	uint8_t pdu = frame[0];
	uint8_t len = frame[1] -2; // header is 4 bytes, but pdu + len bytes are not taken into account
	uint8_t frag = WINOT_FRAGMENT(pdu);
	int write_idx = (frag * WINOT_FRAGMENT_SIZE);
	// putchar('f');
	// putchar(frag +'0');

	if (write_idx +len > WINOT_DATA_BUF_SIZE) return; // Drop invalid fragment
	memcpy((uint8_t*)&gs_winot_data_buf[write_idx], &frame[4], len);
	gs_winot_fragments_received |= (1 << frag);

	if(!(pdu & WINOT_FRAGMENTED)) {
		// last fragment
		if(!WINOT_HAS_ALL_FRAGMENTS(frag)) {
			// should request resending missing fragments
			printf("missing fragments: rx:%02x\n", gs_winot_fragments_received);
		}
		else {
			gs_winot_data_len = write_idx +len;
			gs_winot_fragments_received = 0;
			winot_change_state(WINOT_NEWRX);
		}
	}
}

__HIGH_CODE
int winot_routing_table_store(uint32_t  client_uuid) {
	// find oldest/open spot, or if we are already known
	int idx = 1;
	for(int i = 0; i < WINOT_AP_CLIENTS_MAX; i++) {
		int c = i +1; // clients are stored at idx +1;
		if(gs_datachannel[c] == client_uuid) {
			idx = c;
			break;
		}
		else if(gs_datachannel_last_activity_ms[c] < gs_datachannel_last_activity_ms[idx]) {
			idx = c;
		}
		else if(gs_datachannel[c] == 0) {
			idx = c;
			break;
		}
	}

	gs_datachannel[idx] = client_uuid;
	gs_datachannel_last_activity_ms[idx] = gs_current_tick_ms;
	return idx;
}

__HIGH_CODE
void winot_process_rx() {
	uint8_t *frame = (uint8_t *)LLE_BUF;
	uint8_t pdu = frame[0];
	uint8_t len = frame[1] -2; // header is 4 bytes, but pdu + len bytes are not taken into account

	if(gs_role == WINOT_AP && gs_state == WINOT_LISTENING) {
		len -= 4; // on the request channel header is 4 bytes larger
		int response_len = 0;
		uint32_t client_uuid = *(uint32_t*)&frame[4];
		int ap_needs_datachannel = (gs_winot_data_len && (gs_winot_data_len > WINOT_FRAGMENT_SIZE) && (client_uuid == WINOT_DATA_DEST_UUID(gs_winot_data_buf)));
		int client_needs_datachannel = (pdu & WINOT_FRAGMENTED);

		gs_datachannel[WINOT_ACTIVE_DATACHANNEL] = winot_routing_table_store(client_uuid);

		if(!ap_needs_datachannel && !client_needs_datachannel) {
			gs_datachannel[WINOT_ACTIVE_DATACHANNEL] = WINOT_AP_CHANNEL;
		}

		__attribute__((aligned(4))) static uint8_t response[WINOT_FRAGMENT_SIZE + 8] = {0};
		response[0] = 0; // pdu
		response[2] = (uint8_t)gs_datachannel[WINOT_ACTIVE_DATACHANNEL];
		*(uint32_t*)&response[4] = WINOT_ACCESSADDRESS_PRIV;

		if(gs_winot_data_len && !ap_needs_datachannel) {
			// copy AP data into response to client
			memcpy(response +8, (uint8_t*)gs_winot_data_buf, gs_winot_data_len);
			response_len = gs_winot_data_len;
			gs_winot_data_len = 0;
		}

		if(len) {
			// copy client request data into AP buffer
			// If we had data waiting for another client or that did not fit in the response, we drop that
			memcpy((uint8_t*)gs_winot_data_buf, frame +8, len);
			gs_winot_data_len = len;
			winot_change_state(WINOT_NEWRX);
		}

		iSLERTX(client_uuid, response, response_len +8, WINOT_AP_CHANNEL, PHY_1M);
		if(gs_state != WINOT_NEWRX) {
			winot_change_state((response[2] == WINOT_AP_CHANNEL) ? WINOT_LISTENING : WINOT_COMMUNICATING);
		}
	}
	else if(gs_role == WINOT_CLIENT && gs_state == WINOT_REQUESTING) {
		len -= 4; // on the request channel header is 4 bytes larger
		uint8_t datachannel = frame[2];
		uint32_t ap_uuid = *(uint32_t*)&frame[4]; // TODO: store in routing table

		if(len) {
			memcpy((uint8_t*)gs_winot_data_buf, frame +8, len);
			gs_winot_data_len = len;
			winot_change_state(WINOT_NEWRX);
		}

		if(datachannel == WINOT_AP_CHANNEL) {
			// didn't get a channel, so no more data is coming in
			if(gs_state == WINOT_REQUESTING) {
				winot_change_state(WINOT_IDLE);
			}
		}
		else {
			gs_datachannel[WINOT_ACTIVE_DATACHANNEL] = datachannel;
			if(gs_winot_data_len) {
				winot_burst_tx_and_communicate(WINOT_ACCESSADDRESS_PRIV, gs_datachannel[WINOT_ACTIVE_DATACHANNEL], (uint8_t*)gs_winot_data_buf, gs_winot_data_len);
				gs_winot_data_len = 0;
			}
			else {
				winot_change_state(WINOT_COMMUNICATING);
			}
		}
	}
	else if(gs_state == WINOT_COMMUNICATING) {
		// Keep the client active in the routing table
		if (gs_role == WINOT_AP) {
			int active_channel = gs_datachannel[WINOT_ACTIVE_DATACHANNEL];
			if (active_channel && active_channel <= WINOT_AP_CLIENTS_MAX) {
				gs_datachannel_last_activity_ms[active_channel] = gs_current_tick_ms;
			}
		}

		winot_defragment(frame);
		if(gs_state != WINOT_NEWRX) {
			// keep listenening
			winot_change_state(WINOT_COMMUNICATING);
		}
	}
	else {
		// noise, keep listening
		winot_change_state((gs_role == WINOT_AP) ? WINOT_LISTENING : WINOT_IDLE);
	}
}

WiNot_state winot_init(WiNot_role r, int tx_power) {
	gs_role = r;
	iSLERInit(tx_power);

	// gs_state is initialized to WINOT_IDLE for WINOT_CLIENT
	if(gs_role == WINOT_AP) {
		winot_change_state(WINOT_LISTENING);
	}

	return gs_state;
}

WiNot_state winot_request(const uint8_t *data, int len) {
	// Only WINOT_CLIENT can request, and only when IDLE
	if((gs_state == WINOT_IDLE) && (len <= WINOT_DATA_BUF_SIZE)) {
		if(len > WINOT_FRAGMENT_SIZE) {
			memcpy((uint8_t*)gs_winot_data_buf, data, len);
			gs_winot_data_len = len;

			__attribute__((aligned(4))) static uint8_t response[8] = {0};
			response[0] = WINOT_FRAGMENTED;// pdu fragmented, ask for data channel
			*(uint32_t*)&response[4] = WINOT_ACCESSADDRESS_PRIV;
			iSLERTX(WINOT_AP_ACCESSADDRESS, response, 8, WINOT_AP_CHANNEL, PHY_1M);
		}
		else {
			if(len > 0) {
				if(data == gs_winot_data_buf) {
					// this happens when a protocol stack writes its response back into the request buffer
					// we just need to move the data 8 bytes to the left to make room for the header
					// TODO: figure out if there is a faster way to do this
					for(int i = len -1; i >= 0; i--) {
						gs_winot_data_buf[i +8] = gs_winot_data_buf[i];
					}
				}
				else {
					memcpy((uint8_t*)&gs_winot_data_buf[8], data, len);
				}
			}
			gs_winot_data_buf[0] = 0;// pdu not fragmented
			*(uint32_t*)&gs_winot_data_buf[4] = WINOT_ACCESSADDRESS_PRIV;
			gs_winot_data_len = 0;
			iSLERTX(WINOT_AP_ACCESSADDRESS, (uint8_t*)gs_winot_data_buf, 8+len, WINOT_AP_CHANNEL, PHY_1M);
		}
		winot_change_state(WINOT_REQUESTING);
	}
	return gs_state;
}

int winot_send_packet(const uint8_t *data, int len) {
	if(len > WINOT_DATA_BUF_SIZE) return -1;

	if(gs_role == WINOT_CLIENT) {
		if(gs_state == WINOT_COMMUNICATING) {
			winot_burst_tx_and_communicate(WINOT_ACCESSADDRESS_PRIV, gs_datachannel[WINOT_ACTIVE_DATACHANNEL], data, len);
		}
		else if(gs_state == WINOT_IDLE) {
			winot_request(data, len);
		}
	}
	else if(gs_role == WINOT_AP) {
		memcpy((uint8_t*)gs_winot_data_buf, data, len);
		gs_winot_data_len = len;
		// putchar('d');
	}

	return len;
}

uint8_t* winot_poll_packet(uint16_t *len) {
	if(gs_state == WINOT_NEWRX) {
		*len = gs_winot_data_len;
		return (uint8_t*)gs_winot_data_buf;
	}
	return NULL;
}

void winot_release_packet() {
	if(gs_state == WINOT_NEWRX) {
		WiNot_state new_state = (gs_role == WINOT_AP) ? WINOT_LISTENING : WINOT_IDLE;
		if(gs_datachannel[WINOT_ACTIVE_DATACHANNEL] != WINOT_AP_CHANNEL) {
			// we were on a data channel, get back to that
			new_state = WINOT_COMMUNICATING;
		}
		winot_change_state(new_state);
		gs_winot_data_len = 0;
	}
}

WiNot_state winot_tick(int dt_ms) {
#ifdef GLOBAL_RX_READY
	if(rx_ready) {
		winot_process_rx();
		rx_ready = 0;
	}
#endif

	gs_current_tick_ms += dt_ms;
	int elapsed = gs_current_tick_ms - gs_last_state_change_tick_ms;

	switch(gs_state) {
	case WINOT_REQUESTING: // gs_role == WINOT_CLIENT
		if(elapsed > WINOT_RESPONSE_WINDOW_MS) {
			winot_change_state(WINOT_IDLE);
		}
		break;
	case WINOT_COMMUNICATING:
		if(elapsed > WINOT_RESPONSE_WINDOW_MS) {
			winot_change_state((gs_role == WINOT_AP) ? WINOT_LISTENING : WINOT_IDLE);
		}
		else if(gs_winot_data_len) {
			// we have data ready to be sent
			int channel = gs_datachannel[WINOT_ACTIVE_DATACHANNEL];
			uint32_t client_uuid = gs_datachannel[channel];
			if(WINOT_DATA_DEST_UUID(gs_winot_data_buf) == client_uuid) {
				winot_burst_tx_and_communicate(client_uuid, channel, (uint8_t*)gs_winot_data_buf, gs_winot_data_len);
				gs_winot_data_len = 0;
			}
		}
		break;
	case WINOT_IDLE:
	case WINOT_LISTENING:
	case WINOT_NEWRX:
	default:
		break;
	}

	return gs_state;
}
