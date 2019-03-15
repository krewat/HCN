// Arthur Krewat - msalerno® - 2019/02/20
//
// HCN project - header and C++ source for Halo client-server communications.
//
// (C)2019 - Kilowatt Computers(USA)
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
//
// Key-value pairs are used for most things, because defining special packet types for each operation would be
//	complicated, and TBH, silly.
//

/*

   Copyright 2019 Kilowatt Computers

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	 http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
   
*/

#include "HCN.h"
#include <stdio.h>
#include <memory.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <tchar.h>

// The current HCN state.
enum HCN_state hcn_state[HCN_MAX_PLAYERS];

// What we are, client or server. And what type.
enum HCN_OUR_SIDE hcn_our_side = HCN_WE_ARE_UNKNOWN;
enum HCN_SERVER_TYPE hcn_server_type = HCN_NOT_A_SERVER;
enum HCN_CLIENT_TYPE hcn_client_type[HCN_MAX_PLAYERS];

// Keep a copy of the other side's handshake packet. This can include version, and other pertinent info.
struct HCN_handshake hcn_other_side[HCN_MAX_PLAYERS];

char hcn_our_version[HCN_VALUE_LENGTH] = { 0 };

// Place to set the logger callback for everything.
HCN_logger_callback hcn_logger_callback = NULL;
int hcn_debug_level = HCN_LOG_DEBUG2;

// Send packet function, provided by application.
HCN_packet_sender hcn_packet_sender = NULL;

// Callbacks for various key/value pairs are done through a single array supplied by the application:
struct HCN_key_dispatch *hcn_key_dispatch_list = NULL;

// Some enum key values. First up, server types.
struct HCN_enum_to_string HCN_server_names[] = {
	{ HCN_NOT_A_SERVER, "none"},
	{ HCN_SERVER_SAPP, "SAPP"},
	{ HCN_SERVER_HSE, "HSE\256"},
	{ HCN_SERVER_PHASOR, "Phasor"},
	{ -1, NULL}
};

struct HCN_enum_to_string HCN_client_names[] = {
	{ HCN_NOT_A_CLIENT, "none"},
	{ HCN_CLIENT_HAC2, "HAC2"} ,
	{ HCN_CLIENT_CHIMERA, "Chimera" },
	{ -1, NULL}
};

// Do an enum-to-string lookup.
char *hcn_enum_to_string(int e_num, HCN_enum_to_string *enum_list) {
	int i;

	for (i = 0; enum_list[i].name != NULL; i++) {
		if (enum_list[i].e_num == e_num) {
			return enum_list[i].name;
		}
	}
	return "";
}
// Set the HCN logger callback
void hcn_set_logger_callback(HCN_logger_callback callback) {

	hcn_logger_callback = callback;
	hcn_logger(HCN_LOG_DEBUG, "Logger function set");
}

// Set the packet sender HCN will use.
void hcn_set_packet_sender(HCN_packet_sender sender) {

	hcn_packet_sender = sender;
	hcn_logger(HCN_LOG_DEBUG, "Packet sender function set");
}

void hcn_set_keyvalue_callback_list(HCN_key_dispatch *key_list) {

	hcn_key_dispatch_list = key_list;

}

// Provide a local HCN logger using the callback.
void hcn_logger(int level, const char *string, ...) {
	va_list ap;
	int bufptr = 0;	
	char buffer[1025];

	va_start(ap, string);

	bufptr += sprintf_s(buffer, "HCN: ");						// prepend everything with HCN. If caller wants to, it can interpret this and adjust accordingly (like HSE does).

	// *** For some reason, vsprintf_s crashed HAC2, but not HSE. Further investigation warranted, See BUG id 315
	// http://uhnet1.kilonet.net/bugzilla/show_bug.cgi?id=315
	//if (vsprintf_s(&buffer[bufptr], 1024 - bufptr, string, ap) == -1) {	// Safely print into the buffer whatever we were given.

	if (vsprintf(&buffer[bufptr],  string, ap) == -1) {			// Print into the buffer whatever we were given.
		strcpy(buffer, "HCN: vsprintf_s failed");
	}

	if (hcn_logger_callback != NULL) {							// Don't bother if the callback is not set.
		hcn_logger_callback(level, buffer);
	}

	va_end(ap);

}

// Initialize stuff.
void hcn_init(char *version) {

	// For the love of Christ, why would they do this?
	_CrtSetDebugFillThreshold(0);									// Turn off filling destination buffers with 0xFE for "safe" functions.

	for (int i = 0; i < HCN_MAX_PLAYERS; i++) {
		hcn_client_type[i] = HCN_NOT_A_CLIENT;
		memset(&hcn_other_side[i], 0, sizeof(struct HCN_handshake));
	}
	strcpy_s(hcn_our_version, version);
	hcn_logger(HCN_LOG_INFO, "HCN initialized, caller version = %s", hcn_our_version);
	//hcn_logger(HCN_LOG_INFO, "HCN initialized");
		
}

// Clear a player's state on quit, or join.
void hcn_clear_player(int player_number) {
	int pi = (player_number == 0) ? 0 : player_number - 1;

	hcn_logger(HCN_LOG_DEBUG, "Clearing player state player = %d", player_number);
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
bool hcn_valid_packet(struct HCN_packet *packet, unsigned int chat_type) {
	wchar_t *chat_string = (wchar_t *)packet;

	if (chat_string[0] == HCN_MAGIC && chat_type == HCN_CHAT_TYPE) {
		return true;
	}
	return false;
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

	p = strchr(input, '=');												// First, find the =
	if (i == NULL) return NULL;											// if it wasn't found, abort.
	
	*p++ = 0;															// terminate the key, and remove the =
	return p;															// Return a pointer to the value.
}

// Compute the length of a section of a packet. Many packets start with a preamble, and end with a variable-length c string.
//		So this function makes things easier to look at than a constant stream of casts.
//		YES I KNOW this is bad joo-joo in terms of portability, but we're working with a 32-bit Windows application
//			that only runs on Intel. So there.
int hcn_length(struct HCN_preamble *preamble, char *string) {
	return string - (char *)preamble;
}

// hcn_encode() - Encode a packet, converting wchar_t zeroes to a special sequence. 
//			NOTE - this function takes a byte packet_length, but returns a wchar_t length.
//			ALSO - operate on 16-bit characters, with a 16-bit null termination.
//					And includes that null in the length returned.
int hcn_encode(struct HCN_packet *packet, struct HCN_packet *source, int packet_length) {
	wchar_t *p = (wchar_t *)packet;
	wchar_t *s = (wchar_t *)source;
	int length = 0;

	while (length < HCN_MAX_PACKET_LENGTH / 2 && length < ((packet_length / 2) + packet_length % 2)) {
		if (*s == 0) {													// If we find a 16-bit zero in the incoming packet,
			*p++ = HCN_ENCODE_TAG;										// Translate that to a TAG and the ZERO tag.
			*p++ = HCN_ENCODE_ZERO;
			s++;
			length += 2;
		}
		else if (*s == HCN_ENCODE_TAG) {								// If we find a 16-bit TAG in the incoming packet,
			*p++ = HCN_ENCODE_TAG;										// Encode THAT as a special TAG.
			*p++ = HCN_ENCODE_TAG;
			s++;
			length += 2;
		}
		else {
			*p++ = *s++;												// Otherwise, just copy that 16-bit character.
			length += 1;
		}
	}
	
	*p++ = 0;															// Null terminate the output string.

	return p - (wchar_t *)packet;										// This should return the 16-bit character length of the resulting packet.

}

// hcn_decode() - Decode a packet, converting special sequences to normalized 16-bit wchar_t.
//			NOTE - this function returns a 16-bit byte packet_length
//			ALSO - takes a string of 16-bit characters, with a 16-bit null termination.
//					
int hcn_decode(struct HCN_packet *packet, struct HCN_packet *source) {
	wchar_t *p = (wchar_t *)packet;
	wchar_t *s = (wchar_t *)source;
	int length = 0;
	int packet_length = wcslen((wchar_t *)source) + 1;					// length of packet, always include a null terminator.

	while (length < HCN_MAX_PACKET_LENGTH / 2 && length < packet_length) {
		hcn_logger(HCN_LOG_DEBUG2, "hcn_decode: packet = %04x", *s);
		if (*s == HCN_ENCODE_TAG) {										// If we find the tag for a special sequence in the incoming packet,
			s++;
			if (*s == HCN_ENCODE_ZERO) {								// And it's a zero tag,
				*p++ = 0;												// store a 16-bit zero.
			}
			else if (*s == HCN_ENCODE_TAG) {							// if it's a double sequence tag,
				*p++ = HCN_ENCODE_TAG;									// store a single sequence tag.
			}
			s++;														// Don't know WTF happened, just skip it.
		}
		else {
			*p++ = *s++;												// If not a special tag, just copy the 16-bit character.
		}
		length ++;														// processed a 16-bit character.
	}

	return length;														// This should return the 8-bit character length of the resulting packet.

}

// hcn_client_start() - Start the handshake from the client-side.
void hcn_client_start() {
	struct HCN_handshake handshake;
	struct HCN_packet *packet = (struct HCN_packet *)&handshake;
	struct HCN_packet enc_packet;
	int length = 0;

	if (hcn_packet_sender == NULL) {
		hcn_logger(HCN_LOG_WARN, "HCN packet sender not set when hcn_client_start() called!");
		return;
	}

	strcpy_s(handshake.version, sizeof(handshake), hcn_our_version);	// First, copy our version string in.
	handshake.version[strlen(hcn_our_version) + 1] = 0;					// Double-null terminate that string.
	length = hcn_length(&handshake.preamble, handshake.version);		// Compute the length of the fixed part of the handshake packet.
	length += strlen(hcn_our_version);									// Make sure we include the length of the version string.
	handshake.preamble.packet_type = HCN_PACKET_HANDSHAKE;				// make sure they know it's a handshake.
	handshake.preamble.packet_length = length;							// Set the packet length.

	handshake.hcn_type = hcn_client_type[0];								// We are HAC2.
	handshake.hcn_state = HCN_STATE_HANDSHAKE_C2S;						// And this is client-to-server.

	length = hcn_encode(&enc_packet, packet, length);					// Encoded packet might have a longer length. AND, the length will now be wchar_t.
	hcn_packet_sender(0, &enc_packet);									// Send the packet using the supplied packet sender.

	hcn_other_side[0].hcn_state = HCN_STATE_HANDSHAKE_C2S;				// Record that the other side was sent a Client->Server handshake packet.

}

// hsn_process_chat() - actually process an incoming packet. If return is true, we did work.
//		*** Assume the chat text (our_packet) is null-terminated because Halo would normally
//			supply a wchar_t string. If it doesn't, all bets are off.
bool hcn_process_chat(int player_number, int chat_type, wchar_t *our_packet) {
	int i;
	int pi = (player_number == 0) ? 0 : player_number - 1;
	char key[HCN_KEYVALUE_LENGTH];
	char *value;
	int length;
	struct HCN_packet packet;
	struct HCN_packet reply_packet;
	struct HCN_preamble *preamble = (struct HCN_preamble *)&packet;
	struct HCN_handshake *handshake = (struct HCN_handshake *)&packet;	// get a handshake packet pointer.
	struct HCN_keyvalue_packet *keyvalue_packet = (HCN_keyvalue_packet *)&packet;// get a keyvalue packet pointer.
	struct HCN_keyvalue_packet keyvalue;
			
	length = hcn_decode(&packet, (struct HCN_packet *)our_packet);			// before we do anything, hcn_decode it.

	if (!hcn_valid_packet(&packet, chat_type)) {
		hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): Invalid packet received");
		return false;
	}

	hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): got a valid packet");

	// Decode packet type
	switch (preamble->packet_type) {
		
	// Handle handshake packets
	case HCN_PACKET_HANDSHAKE:
		hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): Got a handshake packet");

		switch (hcn_our_side) {												// Server or client, we need to make decisions.
		case HCN_SERVER:													// We are a server.
			hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): We are a SERVER and got a packet from a client");
			if (handshake->hcn_state == HCN_STATE_HANDSHAKE_C2S) {			// This is a client talking to us, who wants to go to state RUNNING
				hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): Got a client calling in, player_number %d", player_number);
				hcn_state[pi] = HCN_STATE_RUNNING;							// Set the current state of this client to running.
				//hcn_other_side[pi] = *handshake;							// And keep a copy of the handshake packet.
				memcpy(&hcn_other_side[pi], handshake, hcn_length(&handshake->preamble, handshake->version) + strlen(handshake->version) + 1); // copy out just the part we need.

				// Setup our reply.
				handshake->hcn_state = HCN_STATE_HANDSHAKE_S2C;				// tell the client that our state is Server->Client
				handshake->hcn_type = hcn_server_type;						// make sure we tell the client what we are.
				strcpy(handshake->version, hcn_our_version);
				hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): Sending back a handshake with state %d", handshake->hcn_state);

				hcn_logger(HCN_LOG_INFO, "Client version %s %s", hcn_enum_to_string(hcn_other_side[pi].hcn_type, HCN_client_names), hcn_other_side[pi].version);

				hcn_encode(&reply_packet, &packet, hcn_length(&handshake->preamble, handshake->version) + strlen(handshake->version) + 1);

				hcn_packet_sender(player_number, &packet);					// send our reply now.

				return true;												// and tell the caller we did something.
			}
			else {
				hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): SERVER got an unknown state %d - going idle", handshake->hcn_state);
				hcn_state[pi] = HCN_STATE_NONE;								// MISSION ABORT! We got something unexpected from the client.
			}
			break;;
		case HCN_CLIENT:
			hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): We are a CLIENT and got a packet from a server");

			if (handshake->hcn_state == HCN_STATE_HANDSHAKE_S2C && hcn_other_side[0].hcn_state==HCN_STATE_HANDSHAKE_C2S) { // This is from a server, so check the "other side's" state.
				hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): Got a server calling in");
				hcn_state[0] = HCN_STATE_RUNNING;							// we got back a handshake from the server, so we're running.
				hcn_other_side[0] = *handshake;								// And keep a copy of the handshake packet.
				hcn_other_side[0].hcn_state = HCN_STATE_RUNNING;			// Set our copy of the handshake for this server, to state=RUNNING.

				hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): Got handshake from server");

				hcn_logger(HCN_LOG_INFO, "Server version %s %s", hcn_enum_to_string(hcn_other_side[pi].hcn_type, HCN_server_names), hcn_other_side[pi].version);

				return true;												// we did something, YAY!
			}
			else {
				hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): CLIENT got an unknown state %d - going idle", handshake->hcn_state);
				hcn_state[0] = HCN_STATE_NONE;								// MISSION ABORT! We got something unexpected from the server
				return false;
			}
			break;;

		}
		break;;

	// Keyvalue pair.
	case HCN_PACKET_KEYVALUE:
		hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): Got a keyvalue packet");

		if (keyvalue_packet->keyvalue_length == strlen(keyvalue_packet->keyvalue)) {

			hcn_logger(HCN_LOG_DEBUG, "keyvalue = %s for player %d", keyvalue_packet->keyvalue, player_number);

			// See if we have a callback for this key/value pair.
			if (hcn_key_dispatch_list != NULL) {
				
				memcpy(&keyvalue, keyvalue_packet, sizeof(struct HCN_preamble) + keyvalue_packet->keyvalue_length + 1); // copy out just the part we need.

				value = hcn_key_value_parse(keyvalue.keyvalue);			// Break the key/value in two.
				for (i = 0; hcn_key_dispatch_list[i].key != NULL; i++) {
					if (stricmp(hcn_key_dispatch_list[i].key, keyvalue.keyvalue) == 0) {
						hcn_key_dispatch_list[i].callback(player_number, keyvalue.keyvalue, value);
						return true;
					}
				}
			}
			else {
				hcn_logger(HCN_LOG_WARN, "HCN got a keyvalue but the application hasn't defined a list of keyvalues");
				return false;
			}

		}
		else {
			hcn_logger(HCN_LOG_DEBUG, "keyvalue length did not match actual length - sent=%d, keyvalue=%d", keyvalue.keyvalue_length, strlen(keyvalue.keyvalue));
			return false;
		}
		break;;

	}
	return false;
}

// hcn_send_keyvalue() - send a key-value pair to the other side.
bool hcn_send_keyvalue(int player_number, char *keyvalue) {
	int pi = (player_number == 0) ? 0 : player_number - 1;
	struct HCN_keyvalue_packet kv_packet;
	struct HCN_packet *packet = (struct HCN_packet *)&kv_packet;

	if (hcn_packet_sender == NULL) {
		hcn_logger(HCN_LOG_WARN, "hcn_send_keyvalue(): Packet sender not set yet!");
		return false;
	}

	hcn_logger(HCN_LOG_DEBUG, "HCN sending keyvalue '%s' to player %d", keyvalue, player_number);

	if (hcn_state[pi] == HCN_STATE_RUNNING) {							// if the state is "RUNNING" we can go ahead and send it.
		kv_packet.preamble.packet_type = HCN_PACKET_KEYVALUE;			// Packet type
		kv_packet.keyvalue_length = strlen(keyvalue);					// make sure we have a char* length.
		strcpy_s(kv_packet.keyvalue, HCN_KEYVALUE_LENGTH, keyvalue);	// Copy the keyvalue pair in.
		kv_packet.preamble.packet_length = hcn_length(&kv_packet.preamble, kv_packet.keyvalue) + kv_packet.keyvalue_length;
		hcn_packet_sender(player_number, packet);						// and send the actual packet.
		return true;
	}
	else {
		hcn_logger(HCN_LOG_DEBUG, "Other side status is not RUNNING, state = %d, pi = %d", hcn_state[pi], pi);
	}

	return false;														// indicate we failed.

}