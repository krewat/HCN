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
//
// NOTE: many functions are provided here to manipulate certain data that could very easily be done directly
//	by the application. It's done this way to allow for future support for doing other HCN-specific things
//	when a value is changed. 
//
// ALSO: HAC2 and HSE use a "shim" module to interface with HCN - the reason being to shield the application
//	from interfacing directly with HCN. Any HCN changes can then be done in the shim, containing major
//	changes to one or two source files, ala HAC2toHCN.cpp/HAC2toHCN.h
//
//	This arrangement should be used for any other applications that would use HCN. 
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
HCN_state hcn_state[HCN_MAX_PLAYERS];

// A little bit of a state machine. Keep track of last state, compared to current state, so we can do certain things when the state changes.
HCN_state hcn_last_state[HCN_MAX_PLAYERS];

// What we are, client or server. And what type.
HCN_OUR_SIDE hcn_our_side = HCN_WE_ARE_UNKNOWN;
HCN_SERVER_TYPE hcn_server_type = HCN_NOT_A_SERVER;
HCN_CLIENT_TYPE hcn_client_type[HCN_MAX_PLAYERS];

// Keep a copy of the other side's handshake packet. This can include version, and other pertinent info.
struct HCN_handshake hcn_other_side[HCN_MAX_PLAYERS];

char hcn_our_version[HCN_VALUE_LENGTH] = { 0 };

// Logger callback function. The application provides this so we can output log information in whatever format the application wants.
HCN_logger_callback hcn_logger_callback = NULL;
int hcn_debug_level = HCN_LOG_INFO;

// Send packet function, provided by application.
HCN_application_sender hcn_application_sender = NULL;

// Callbacks to the application for datapoint updates
struct HCN_datapoint_dispatch *hcn_datapoint_dispatch_list = NULL;
int hcn_datapoint_dispatch_list_entries = 0;

// Callbacks to the application for vector updates
struct HCN_vector_dispatch *hcn_vector_dispatch_list = NULL;
int hcn_vector_dispatch_list_entries = 0;

// Callbacks to the application for various key/value pairs
struct HCN_key_dispatch *hcn_key_dispatch_list = NULL;

// State types.
struct HCN_enum_to_string HCN_state_names[] = {
	{ HCN_STATE_NONE, "NO STATE" },
	{ HCN_STATE_HANDSHAKE_C2S, "CLIENT->SERVER HANDSHAKE" },
	{ HCN_STATE_HANDSHAKE_S2C, "SERVER->CLIENT HANDSHAKE" },
	{ HCN_STATE_RUNNING, "HCN RUNNING" }
};

// Server types as enum/string pairs.
struct HCN_enum_to_string HCN_server_names[] = {
	{ HCN_NOT_A_SERVER, "none"},
	{ HCN_SERVER_SAPP, "SAPP"},
	{ HCN_SERVER_HSE, "HSE\256"},
	{ HCN_SERVER_PHASOR, "Phasor"},
	{ -1, NULL}
};

// Client types as enum/string pairs.
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

// Set the HCN debug level.
void hcn_set_debug_level(int level) {
	hcn_debug_level = level;
}

// Get the HCN debug level.
int hcn_get_debug_level() {
	return hcn_debug_level;
}

// Set the HCN logger callback
void hcn_set_logger_callback(HCN_logger_callback callback) {

	hcn_logger_callback = callback;
	hcn_logger(HCN_LOG_DEBUG2, "Logger function set");
}

// Set the packet sender HCN will use.
void hcn_set_packet_sender(HCN_application_sender application_sender) {

	hcn_application_sender = application_sender;
	hcn_logger(HCN_LOG_DEBUG2, "Application packet sender function set");
}

void hcn_set_datapoint_callback_list(HCN_datapoint_dispatch *datapoint_list, int datapoint_list_length) {

	hcn_datapoint_dispatch_list = datapoint_list;
	hcn_datapoint_dispatch_list_entries = datapoint_list_length;

}

void hcn_set_vector_callback_list(HCN_vector_dispatch *vector_list, int vector_list_length) {

	hcn_vector_dispatch_list = vector_list;
	hcn_vector_dispatch_list_entries = vector_list_length;

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

	if (level <= hcn_debug_level) {
		bufptr += sprintf_s(buffer, "HCN: ");					// prepend everything with HCN. If caller wants to, it can interpret this and adjust accordingly (like HSE does).

		// *** For some reason, vsprintf_s crashed HAC2, but not HSE. Further investigation warranted, See BUG id 315
		// http://uhnet1.kilonet.net/bugzilla/show_bug.cgi?id=315
		//if (vsprintf_s(&buffer[bufptr], 1024 - bufptr, string, ap) == -1) {	// Safely print into the buffer whatever we were given.

		if (vsprintf(&buffer[bufptr], string, ap) == -1) {			// Print into the buffer whatever we were given.
			strcpy(buffer, "HCN: vsprintf_s failed");
		}

		if (hcn_logger_callback != NULL) {					// Don't bother if the callback is not set.
			hcn_logger_callback(level, buffer);
		}
	}

	va_end(ap);

}

// Initialize stuff.
void hcn_init(char *version) {

	// For the love of Christ, why would they do this?
	_CrtSetDebugFillThreshold(0);							// Turn off filling destination buffers with 0xFE for "safe" functions.

	for (int i = 0; i < HCN_MAX_PLAYERS; i++) {
		hcn_state[i] = HCN_STATE_NONE;
		hcn_client_type[i] = HCN_NOT_A_CLIENT;
		memset(&hcn_other_side[i], 0, sizeof(struct HCN_handshake));
	}
	strcpy_s(hcn_our_version, version);
	hcn_logger(HCN_LOG_DEBUG, "HCN initialized, caller version = %s", hcn_our_version);
		
}

// hcn_on_tick() - called every tick - doesn't HAVE to be every tick, but it's a prefect place to call it from.
void hcn_on_tick() {

	// for now, this does nothing. yet.

}

// Clear a player's state on quit, or join.
void hcn_clear_player(int player_number) {
	int pi = (player_number == 0) ? 0 : player_number - 1;

	hcn_logger(HCN_LOG_DEBUG2, "Clearing player state player = %d", player_number);
	hcn_state[pi] = HCN_STATE_NONE;

}

// Set what we are, server or client, and what type of server or client. Overloaded function.
void hcn_what_we_are(HCN_OUR_SIDE our_side, HCN_CLIENT_TYPE client_type) {
	hcn_our_side = our_side;
	hcn_client_type[0] = client_type;					// Since we're a client, we only need to define the first entry in hcn_client_type
}
void hcn_what_we_are(HCN_OUR_SIDE our_side, HCN_SERVER_TYPE server_type) {
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
	char *p;

	p = strchr(input, '=');							// First, find the =
	if (p == NULL) return NULL;						// if it wasn't found, abort.
	
	*p++ = 0;								// terminate the key, and remove the =
	return p;								// Return a pointer to the value.
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
		if (*s == 0) {							// If we find a 16-bit zero in the incoming packet,
			*p++ = HCN_ENCODE_TAG;					// Translate that to a TAG and the ZERO tag.
			*p++ = HCN_ENCODE_ZERO;
			s++;
			length += 2;
		}
		else if (*s == HCN_ENCODE_TAG) {				// If we find a 16-bit TAG in the incoming packet,
			*p++ = HCN_ENCODE_TAG;					// Encode THAT as a special TAG.
			*p++ = HCN_ENCODE_TAG;
			s++;
			length += 2;
		}
		else {
			*p++ = *s++;						// Otherwise, just copy that 16-bit character.
			length += 1;
		}
	}
	
	*p++ = 0;								// Null terminate the output string.

	return p - (wchar_t *)packet;						// This should return the 16-bit character length of the resulting packet.

}

// hcn_decode() - Decode a packet, converting special sequences to normalized 16-bit wchar_t.
//			NOTE - this function returns a 16-bit byte packet_length
//			ALSO - takes a string of 16-bit characters, with a 16-bit null termination.
//					
int hcn_decode(struct HCN_packet *packet, struct HCN_packet *source) {
	wchar_t *op = (wchar_t *)packet;
	wchar_t *p = (wchar_t *)packet;
	wchar_t *s = (wchar_t *)source;
	int length = 0;
	int packet_length = wcslen(s);						// length of encoded packet

	/*
	char buffer[1024];
	int bufptr = 0;
	int cols = 0;
	bufptr += sprintf(buffer + bufptr, "%d ", packet_length);
	for (int i = 0; i < packet_length; i++) {
		bufptr += sprintf(buffer + bufptr, "%04x ", s[i]);
		cols++;
		if (cols > 10) {
			hcn_logger(HCN_LOG_DEBUG2, buffer);
			bufptr = cols = 0;
		}

	}
	if (cols > 0 ) hcn_logger(HCN_LOG_DEBUG2, buffer);
	*/

	while (length < HCN_MAX_PACKET_LENGTH / 2 && length < packet_length) {
		if (*s == HCN_ENCODE_TAG) {					// If we find the tag for a special sequence in the incoming packet,
			s++;
			if (*s == HCN_ENCODE_ZERO) {				// And it's a zero tag,
				*p++ = 0;					// store a 16-bit zero.
				packet_length--;				// need to reduce the input source packet length for each encoding.
			}
			else if (*s == HCN_ENCODE_TAG) {			// if it's a double sequence tag,
				*p++ = HCN_ENCODE_TAG;				// store a single sequence tag.
				packet_length--;				// need to reduce the input source packet length for each encoding.
			}
			else {
				return 0;					// This decode failed. There was garbage in the packet.
			}
			s++;							// If we got here, it means it was a valid ENCODE 
		}
		else {
			*p++ = *s++;						// If not a special tag, just copy the 16-bit character.
		}
		length++;							// processed a 16-bit character.
	}

	/*
	bufptr = cols = 0;
	bufptr += sprintf(buffer + bufptr, "%d ", length);
	for (int i = 0; i < length; i++) {
		bufptr += sprintf(buffer + bufptr, "%04x ", op[i]);
		cols++;
		if (cols > 10) {
			hcn_logger(HCN_LOG_DEBUG2, buffer);
			bufptr = cols = 0;
		}

	}
	if (cols > 0) hcn_logger(HCN_LOG_DEBUG2, buffer);
	*/

	return length;

}

// hcn_packet_sender() - Called to send a packet that has not been encoded yet. We take care of the lengths, encoding, etc.
//	Supplied length is BYTE
void hcn_packet_sender(int player_number, HCN_packet *packet, int packet_length) {
	HCN_preamble *preamble = (HCN_preamble *)packet;			// Get a preamble pointer.
	HCN_packet encoded_packet;						// We need a place to encode the packet to.

	preamble->packet_length = packet_length;				// First, store the unencoded packet length in 8-bit bytes.

	preamble->encoded_length = hcn_encode(&encoded_packet, packet, packet_length);	// Encoded packet length is wchar_t (16-bit bytes).
	hcn_application_sender(player_number, &encoded_packet);			// Send the packet using the supplied packet sender.

}

// hcn_client_start() - Start the handshake from the client-side. Client implies player index 0.
void hcn_client_start() {
	struct HCN_handshake handshake;
	struct HCN_packet *packet = (struct HCN_packet *)&handshake;
	struct HCN_packet enc_packet;
	int length = 0;

	if (hcn_application_sender == NULL) {
		hcn_logger(HCN_LOG_WARN, "HCN packet sender not set when hcn_client_start() called!");
		return;
	}

	strcpy_s(handshake.version, sizeof(handshake), hcn_our_version);	// First, copy our version string in.
	handshake.version[strlen(hcn_our_version) + 1] = 0;			// Double-null terminate that string.
	length = hcn_length(&handshake.preamble, handshake.version);		// Compute the length of the fixed part of the handshake packet.
	length += strlen(hcn_our_version);					// Make sure we include the length of the version string.
	handshake.preamble.packet_type = HCN_PACKET_HANDSHAKE;			// make sure they know it's a handshake.
	handshake.preamble.packet_length = length;				// Set the packet length.

	handshake.hcn_type = hcn_client_type[0];				// We are whatever we were set to.
	handshake.hcn_state = HCN_STATE_HANDSHAKE_C2S;				// And this is client-to-server.

	hcn_packet_sender(0, packet, length);					// Send the packet, encoding it on the fly.

	hcn_other_side[0].hcn_state = HCN_STATE_HANDSHAKE_C2S;			// Record that the other side was sent a Client->Server handshake packet.

}

// hsn_process_chat() - actually process an incoming packet. If return is true, we did work.
//		*** Assume the chat text (our_packet) is null-terminated because Halo would normally
//			supply a wchar_t string. If it doesn't, all bets are off.
bool hcn_process_chat(int player_number, int chat_type, wchar_t *our_packet) {
	int i;
	int pi = (player_number == 0) ? 0 : player_number - 1;
	char key[HCN_KEYVALUE_LENGTH];
	char *value;
	int length, encoded_length;
	struct HCN_packet packet;
	struct HCN_packet reply_packet;
	struct HCN_preamble *encoded_preamble = (struct HCN_preamble *)our_packet;
	struct HCN_preamble *preamble = (struct HCN_preamble *)&packet;
	struct HCN_handshake *handshake = (struct HCN_handshake *)&packet;	// get a handshake packet pointer.
	struct HCN_keyvalue_packet *keyvalue_packet = (HCN_keyvalue_packet *)&packet;// get a keyvalue packet pointer.
	struct HCN_keyvalue_packet keyvalue;

	encoded_length = wcslen(our_packet);
	if (encoded_preamble->encoded_length != encoded_length) {		// The preamble is setup specifically so that we can look at it's contents without decoding first.
		hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): length of encoded packet doesn't match - %d vs. %d", encoded_length, encoded_preamble->encoded_length);
	}

	length = hcn_decode(&packet, (struct HCN_packet *)our_packet);		// Now, hcn_decode it.

	if (length != preamble->packet_length) {
		hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): length of decoded packet doesn't match - %d vs. %d", length, encoded_preamble->packet_length);
	}
	if (!hcn_valid_packet(&packet, chat_type)) {
		hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): Invalid packet received");
		return false;
	}

	hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): got a valid packet");

	// Decode packet type
	switch (preamble->packet_type) {
		
	// Handle handshake packets
	case HCN_PACKET_HANDSHAKE:
		hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): Got a handshake packet");

		switch (hcn_our_side) {						// Server or client, we need to make decisions.
		case HCN_SERVER:						// We are a server.
			hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): We are a SERVER and got a packet from a client");
			if (handshake->hcn_state == HCN_STATE_HANDSHAKE_C2S) {	// This is a client talking to us, who wants to go to state RUNNING
				hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): Got a client calling in, player_number %d", player_number);
				hcn_state[pi] = HCN_STATE_RUNNING;		// Set the current state of this client to running.
				//hcn_other_side[pi] = *handshake;		// And keep a copy of the handshake packet.
				memcpy(&hcn_other_side[pi], handshake, hcn_length(&handshake->preamble, handshake->version) + strlen(handshake->version) + 1); // copy out just the part we need.

				// Setup our reply.
				handshake->hcn_state = HCN_STATE_HANDSHAKE_S2C;	// tell the client that our state is Server->Client
				handshake->hcn_type = hcn_server_type;		// make sure we tell the client what we are.
				
				strcpy(handshake->version, hcn_our_version);
				hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): Sending back a handshake with state %d", handshake->hcn_state);

				hcn_logger(HCN_LOG_DEBUG, "Client version %s %s", hcn_enum_to_string(hcn_other_side[pi].hcn_type, HCN_client_names), hcn_other_side[pi].version);

				length = hcn_length(&handshake->preamble, handshake->version) + strlen(handshake->version) + 1; // Compute the un-encoded length in 8-bit bytes.

				hcn_packet_sender(player_number, &packet, length); // Send it.

				return true;					// and tell the caller we did something.
			}
			else {
				hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): SERVER got an unknown state %d - going idle", handshake->hcn_state);
				hcn_state[pi] = HCN_STATE_NONE;			// MISSION ABORT! We got something unexpected from the client.
			}
			break;;
		case HCN_CLIENT:
			hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): We are a CLIENT and got a packet from a server");

			if (handshake->hcn_state == HCN_STATE_HANDSHAKE_S2C && hcn_other_side[0].hcn_state==HCN_STATE_HANDSHAKE_C2S) { // This is from a server, so check the "other side's" state.
				hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): Got a server calling in");
				hcn_state[0] = HCN_STATE_RUNNING;		// we got back a handshake from the server, so we're running.
				hcn_other_side[0] = *handshake;			// And keep a copy of the handshake packet.
				hcn_other_side[0].hcn_state = HCN_STATE_RUNNING;// Set our copy of the handshake for this server, to state=RUNNING.

				hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): Got handshake from server");

				hcn_logger(HCN_LOG_DEBUG, "Server version %s %s", hcn_enum_to_string(hcn_other_side[pi].hcn_type, HCN_server_names), hcn_other_side[pi].version);

				return true;					// we did something, YAY!
			}
			else {
				hcn_logger(HCN_LOG_DEBUG, "hcn_process_chat(): CLIENT got an unknown state %d - going idle", handshake->hcn_state);
				hcn_state[0] = HCN_STATE_NONE;			// MISSION ABORT! We got something unexpected from the server
				return false;
			}
			break;;

		}
		break;;

	// Datapoints.
	case HCN_PACKET_DATAPOINT:
		hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): Got a list of datapoint values");
		return hcn_datapoint_packet_handler(player_number, &packet);
		break;;

	// Vector updates.
	case HCN_PACKET_VECTOR:
		hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): Got a list of vector values");
		return hcn_vector_packet_handler(player_number, &packet);
		break;;


	// Keyvalue pair.
	case HCN_PACKET_KEYVALUE:
		hcn_logger(HCN_LOG_DEBUG2, "hcn_process_chat(): Got a keyvalue packet");

		if (keyvalue_packet->keyvalue_length == strlen(keyvalue_packet->keyvalue)) {

			hcn_logger(HCN_LOG_DEBUG2, "keyvalue = %s for player %d", keyvalue_packet->keyvalue, player_number);

			// See if we have a callback for this key/value pair.
			if (hcn_key_dispatch_list != NULL) {
				
				memcpy(&keyvalue, keyvalue_packet, sizeof(struct HCN_preamble) + keyvalue_packet->keyvalue_length + 1); // copy out just the part we need.

				value = hcn_key_value_parse(keyvalue.keyvalue);	// Break the key/value in two.
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
	int length;
	struct HCN_keyvalue_packet kv_packet;
	struct HCN_packet *packet = (struct HCN_packet *)&kv_packet;

	if (hcn_application_sender == NULL) {
		hcn_logger(HCN_LOG_WARN, "hcn_send_keyvalue(): Application packet sender not set yet!");
		return false;
	}

	if (hcn_state[pi] == HCN_STATE_RUNNING) {				// if the state is "RUNNING" we can go ahead and send it.
		hcn_logger(HCN_LOG_DEBUG2, "HCN sending keyvalue '%s' to player %d", keyvalue, player_number);
		kv_packet.preamble.packet_type = HCN_PACKET_KEYVALUE;		// Packet type
		kv_packet.keyvalue_length = strlen(keyvalue);			// make sure we have a char* length.
		strcpy_s(kv_packet.keyvalue, HCN_KEYVALUE_LENGTH, keyvalue);	// Copy the keyvalue pair in.
		length = hcn_length(&kv_packet.preamble, kv_packet.keyvalue) + kv_packet.keyvalue_length; // Get the un-encoded length.
		hcn_packet_sender(player_number, packet, length);		// and send the actual packet.
		return true;
	}
	else {
		hcn_logger(HCN_LOG_DEBUG, "Other side status is not RUNNING, state = %d, pi = %d", hcn_state[pi], pi);
	}

	return false;								// indicate we failed.

}

// hcn_datapoint_packet_handler() - Decode a datapoint packet. Assume the packet has already been decoded and verified.
bool hcn_datapoint_packet_handler(int player_number, HCN_packet *packet) {
	int i;
	HCN_datapoint_type dp_type;
	HCN_datapoint_packet *dps = (HCN_datapoint_packet *)packet;

	for (i = 0; i < HCN_MAX_DATAPOINTS && i < dps->dp_count; i++) {
		dp_type = dps->dps[i].dp_type;
		if (dp_type == 0 || dp_type > hcn_datapoint_dispatch_list_entries) {
			hcn_logger(HCN_LOG_DEBUG, "Invalid datapoint type %d", dp_type);
			return false;						// ABORT if the datapoint type is unknown. Chances are the rest of the packet is bad anyway.
		}
		hcn_datapoint_dispatch_list[dp_type].callback(player_number, dp_type, &dps->dps[i]); // Call the application's handler for this vector type.
	}
	return true;

}

// hcn_vector_packet_handler() - Decode a vector packet. Assume the packet has already been decoded and verified.
bool hcn_vector_packet_handler(int player_number, HCN_packet *packet) {
	int i;
	HCN_vector_type vt;
	HCN_vector_packet *vectors = (HCN_vector_packet *)packet;

	for (i = 0; i < HCN_MAX_VECTORS && i < vectors->vector_count; i++) {
		vt = vectors->vectors[i].vector_type;
		if (vt == 0 || vt > hcn_vector_dispatch_list_entries) {
			hcn_logger(HCN_LOG_DEBUG, "Invalid vector type %d", vt);
			return false;						// ABORT if the vector type is unknown. Chances are the rest of the packet is bad anyway.
		}
		hcn_vector_dispatch_list[vt].callback(player_number, vt, &vectors->vectors[i].vector); // Call the application's handler for this vector type.
	}
	return true;

}

// hcn_send_datapoints() - allow an application to provide a list of datapoints, and send them to the other side.
bool hcn_send_datapoints(int player_number, struct HCN_datapoint *dps, int dp_count) {
	int i, length;
	struct HCN_datapoint_packet dp_packet;
	struct HCN_packet *packet = (HCN_packet *)&dp_packet;
	struct HCN_packet encoded_packet;

	if (dp_count > HCN_MAX_DATAPOINTS) return false;			// make sure we're not asked to send too many.

	// Find the length of the datapoint packet without any datapoints in it. This allows us to have a variable-length packet
	//	dependent on how many datapoints we're supplied.
	length = sizeof(struct HCN_datapoint_packet) - (sizeof(HCN_datapoint) * HCN_MAX_DATAPOINTS);

	dp_packet.preamble.packet_type = HCN_PACKET_DATAPOINT;			// Set the packet type.

	for (i = 0; i < HCN_MAX_DATAPOINTS && i < dp_count; i++) {		// Loop through the vectors provided to us.
		dp_packet.dps[i] = dps[i];					// Copy out the datapoint.
		length += sizeof(HCN_datapoint);				// Keep track of the length.
	}

	dp_packet.dp_count = dp_count;						// Set the datapoint count.

	hcn_packet_sender(player_number, packet, length);			// Send it.

	return true;

}

// hcn_send_vectors() - allow an application to provide a list of vectors, and send them to the other side.
bool hcn_send_vectors(int player_number, struct HCN_vector *vectors, int vector_count) {
	int i, length;
	struct HCN_vector_packet vp;
	struct HCN_packet *packet = (HCN_packet *)&vp;
	struct HCN_packet encoded_packet;

	if (vector_count > HCN_MAX_VECTORS) return false;			// make sure we're not asked to send too many.

	// Find the length of the vector packet without any vectors in it. This allows us to have a variable-length packet
	//	dependent on how many vectors we're supplied.
	length = sizeof(struct HCN_vector_packet) - (sizeof(HCN_vector) * HCN_MAX_VECTORS);

	vp.preamble.packet_type = HCN_PACKET_VECTOR;				// Set the packet type.

	for (i = 0; i < HCN_MAX_VECTORS && i < vector_count; i++) {		// Loop through the vectors provided to us.
		vp.vectors[i] = vectors[i];					// Copy out the vector.
		length += sizeof(HCN_vector);					// Keep track of the length.
	}

	vp.vector_count = vector_count;						// make sure we have a vector count.

	hcn_packet_sender(player_number, packet, length);			// Send the raw packet.

	return true;

}