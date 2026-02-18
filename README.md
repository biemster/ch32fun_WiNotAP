!!! WIP !!!

# ch32fun_WiNotAP
ch32fun iSLER WiNot AccessPoint, access points for WCH chips using Ethernet over RF

!!! WIP !!!

# ch32fun_WiNotAP
ch32fun iSLER WiNot AccessPoint, access points for WCH chips using Ethernet over RF

## iSLER WiNot protocol definition
AP listens on channel 35 for communication requests from clients. All communication is initiated by the client using a request on the request channel.

### Request channel communication
1. All APs listen on the request channel on access address `0x63683332` (b'ch32')
2. Client pings the request channel with its 32 bit uuid + length byte + payload (can be 0):
- c -> ap: `[uuid[0], uuid[1], uuid[2], uuid[3], len, payload[0], payload[1], ..., payload[len-1]]`
- If len=0 client has no data to send, but wants to know if AP has data for it.
3. AP responds within 3 ms on access address equal to client uuid, with its own uuid + new comm channel + length byte + payload (can be 0).
- ap -> c: `[uuid[0], uuid[1], uuid[2], uuid[3], new channel, len, payload[0], payload[1], ..., payload[len-1]]`
- If new channel is 35 (request channel), there is no new data for this client.
- If new channel is lower than 35, payload is part or all of the data, communication will continue on the new channel.

### Data channel communication
1. Data channel communication is on access address equal to the client uuid.
2. Client confirms reception of AP channel allocation within 3 ms on the data channel, by sending a PDU (see 5.), length byte + payload (can be 0).
- c -> ap: `[PDU, len, payload[0], payload[1], ..., payload[len-1]]`
3. AP and client exchange messages 1 by 1 in a round robin fashion, except when the PDU indicates fragmentation (see 5.)
- ap -> c: `[PDU, len, payload[0], payload[1], ..., payload[len-1]]`
4. The channel remains open for 500ms without communication in either way, after that a new data channel has to be requested by the client.
5. `iSLERRX(...)` and `TX(...)` specify a PDU byte, originally intended for BLE frame type. In WiNot this is used to indicate a fragmented ethernet frame. If bit 4 (0x08, b1000) of the PDU is set, it's part of a fragmented frame, with bits 1 to 3 indicating which part of the frame the current fragment is (fragment index).
6. The last fragment will have a fragment index set and the fragment indicator byte (fourth bit) not set, indicating end of frame. The receiver (either client or AP) will acknowledge which fragments it received in it's next transmission by either sending it's own response data as acknowledgement, or by sending a retransmission request with all 4 LSB bits in the PDU set (0xf, b1111), and a payload with n bytes for n missing fragments indicating the missing fragments:
- [ap,c] -> [c,ap]: `[PDU(0x0f), len(n missing fragments), missing_fragment_idx1, missing_fragment_idx2, ...]`
