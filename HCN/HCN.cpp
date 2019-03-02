// Arthur Krewat - msalerno® - 2019/02/20
//
// HCN project - header and C++ source for Halo client-server communications.
//
// (C)2019 - Kilowatt Computers(USA), RWG ® Server Hosting(EU) incorporating Realworld Guild
//
// For all the gits out there.
//
// After dealing with Sehe's NetEvents, I find that the protocol is lacking in a few crucial areas.
//
// So going forward, HCN is a way for clients like HAC2 to communicate with both HSE® and SAPP LUA scripts.
//
// This will be released separately, and open sourced, so Chimera or other client-side mods can communicate
//	with the server, and possibly even other resources will be involved in this module.
//
// The major point of this new module is to make it so no special changes need to be made to chat packet
//	sending and receiving. We don't need to set a length, because it will adhere to UTF-16 string lengths.
//	Mostly, this means there are no zeros (NULLs) in the stream. This way, regular Halo chat using string
//	length functions can easily deal with the data. This was a shortcoming with Sehe's NetEvents. It was 
//	binary data, so a way had to be found to modify the stock chat code so that it could be fed a data length
//	and it would send the entire packet of data. HCN will use zero-byte encoding to eliminate that problem.
//	This makes it possible to use LUA on the SAPP side to receive and send packets of data.
//
// Server-side, we provide support for up to 16 clients. Client-side, all player_number arguments are set to 0.
//	In this way, a lot of code can be reused on both sides.
//
// Much of this code makes a clear destinction between "server" and "client". HAC2 is a "client". HSE or SAPP+LUA
//	is a "server". Conversely, some packet types are generic, like key/value pairs.

#include "HCN.h"
#include <memory.h>
#include <string.h>

// The current HCN state.
enum HCN_state hcn_state[HCN_MAX_PLAYERS];

// What we are, client or server. And what type.
enum HCN_OUR_SIDE hcn_our_side = HCN_WE_ARE_UNKNOWN;
enum HCN_SERVER_TYPE hcn_server_type = HCN_NOT_A_SERVER;
enum HCN_CLIENT_TYPE hcn_client_type[HCN_MAX_PLAYERS];

// Keep a copy of the other side's handshake packet. This can include version, and other pertinent info.
struct HCN_handshake hcn_other_side[HCN_MAX_PLAYERS];

char hcn_our_version[HCN_VALUE_LENGTH];

// Initialize stuff.
void hcn_init(char *version) {
	for (int i = 0; i < HCN_MAX_PLAYERS; i++) {
		hcn_client_type[i] = HCN_NOT_A_CLIENT;
		memset(hcn_client_type + i, 0, sizeof(struct HCN_handshake));
		strcpy_s(hcn_our_version, version);
	}
}

// Clear a player's state on quit, or join.
void hcn_clear_player(int player_number) {
	int pi = (player_number == 0) ? 0 : player_number - 1;

	hcn_state[pi] = HCN_STATE_NONE;

}

// Set what we are, server or client, and what type of server or client. Overloaded function.
void hcn_what_we_are(enum HCN_OUR_SIDE our_side, enum HCN_CLIENT_TYPE client_type) {
	hcn_our_side = our_side;
	hcn_client_type[0] = client_type;						// Since we're a client, we only need to define the first entry in hcn_client_type
}
void hcn_what_we_are(enum HCN_OUR_SIDE our_side, enum HCN_SERVER_TYPE server_type) {
	hcn_our_side = our_side;
	hcn_server_type = server_type;
}

// hsn_valid_packet() - Check if a chat string contains our magic #, and chat type matches
bool hcn_valid_packet(wchar_t *chat_string, unsigned int chat_type) {

	if (chat_string[1] == HCN_MAGIC && chat_type == HCN_CHAT_TYPE) {
		return true;
	}
}

// hsn_running() - return true if state is HCN_RUNNING
bool hcn_running(int player_number) {
	int pi = (player_number == 0) ? 0 : player_number - 1;

	return hcn_state[pi] == HCN_STATE_RUNNING;

}

// Some key-value pair functions. Callers must adhere to HCN_KEY_LENGTH/HCN_VALUE_LENGTH limits.
// ** NOTE - THIS FUNCTION IS DESTRUCTIVE. It will return a pointer to the value, and termnate
//				the key by overwriting the = character with a zero
char *hcn_key_value_parse(char *input) {
	int i;
	char *p;

	p = strchr(input, '=');										// First, find the =
	if (i == NULL) return NULL;									// if it wasn't found, abort.

	*p++ = 0;													// terminate the key, and remove the =
	return p;													// Return a pointer to the value.
}


// hsn_process_chat() - actually process an incoming packet. If return is true, packet has been modified,
//		and needs to be sent back to the other side. 
//   IMPORTANT! packet pointer MUST be able to take a reply packet of HCN_MAX_PACKET_LENGTH
bool hsn_process_chat(int player_number, int chat_type, wchar_t *packet) {
	int pi = (player_number == 0) ? 0 : player_number - 1;
	struct HCN_preamble *preamble = (struct HCN_preamble *)packet;
	struct HCN_preamble outbound_preamble;
	struct HCN_handshake *handshake;
	
	if (!hcn_valid_packet(packet, chat_type)) {
		return false;
	}

	// Decode packet type
	switch (preamble->packet_type) {
	case HCN_PACKET_HANDSHAKE:												// Start of handshake.

		handshake = (struct HCN_handshake *)packet;							// Copy the entire handshake out.

		switch (hcn_our_side) {												// Server or client, we need to make decisions.
		case HCN_SERVER:													// We are a server.
			if (handshake->hcn_state == HCN_STATE_HANDSHAKE_C2S) {			// This is a client talking to us, who wants to go to state RUNNING
				hcn_state[pi] = HCN_STATE_HANDSHAKE_S2C;					// Set the current state to server->client
				hcn_other_side[pi] = *handshake;							// And keep a copy of the handshake packet.

				// Setup our reply.
				handshake->hcn_state = HCN_STATE_HANDSHAKE_S2C;

				return true;												// and tell the caller we created a reply and they should send it.
			}
			else {
				hcn_state[pi] = HCN_STATE_NONE;								// MISSION ABORT! We got something unexpected from the client.
			}
			break;;
		case HCN_CLIENT:


			break;;
		}

	}
}