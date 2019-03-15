	// Arthur Krewat - msalerno� - 2019/02/20
//
// HCN project - header and C++ source for Halo client-server communications.
//
// (C)2019 - Kilowatt Computers(USA)
//
// For all the gits out there.
//
// After dealing with Sehe's NetEvents, I find that the protocol is lacking in a few crucial areas.
//
// So going forward, HCN is a way for clients like HAC2 to communicate with both HSE� and SAPP LUA scripts.
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
// I was going to make a single data packet struct, but there were so many nested unions, it was way too complicated.
//	Instead we'll define each type of packet individually. Yeah, I know, "templates". To much casting bothers me.
//
// Every step of the way, consistency checking is a must. magic #, state in the initial handshake conversation,
//	packet length, you name it. See HCN_packet_lengths array near the end of this include file.

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

#include <memory>

// Basic data structures and enums for packets.
//	Care is taken to make sure enums start at 1 to keep the number of zeroes to a minimum.

#define HCN_CHAT_TYPE	6						// This is an unused chat type that should just "pass through".
												// Sehe's NetEvents used chat type 5.

// What are we, Server or Client?
enum HCN_OUR_SIDE {
	HCN_WE_ARE_UNKNOWN = 0,
	HCN_SERVER,
	HCN_CLIENT
};

// Server type
enum HCN_SERVER_TYPE {
	HCN_NOT_A_SERVER = 0,
	HCN_SERVER_SAPP,
	HCN_SERVER_PHASOR,
	HCN_SERVER_HSE
};

// Client type.
enum HCN_CLIENT_TYPE {
	HCN_NOT_A_CLIENT = 0,
	HCN_CLIENT_HAC2,
	HCN_CLIENT_CHIMERA 
};

// There will always be an HCN_MAGIC number in the first two bytes of any message. This is for sanity checking.
// Pick something that would be unlikely to be in a normal chat. Even though we use a custom chat-type, we might
//	have to support clients or servers that cannot intercept that chat-type and are stuck dealing with "normal"
//	chat.
//
// *** Remember all packets must be terminated by a wchar_t null, essentially 2 zero bytes, aligned on an even
//		byte boundary.
//
#define HCN_MAGIC	0x1F20						// the actual order would be: 0x20, 0x1F because of little Endian

// Max packet length.
#define HCN_MAX_PACKET_LENGTH	500				// it's really 510 I think, but better to be safe than sorry.

// Max players is always 16.
#define HCN_MAX_PLAYERS			16

// Some max sizes for key/value string lengths. Includes the null terminator.
#define HCN_KEY_LENGTH		30
#define HCN_VALUE_LENGTH	128
#define HCN_KEYVALUE_LENGTH	HCN_KEY_LENGTH + HCN_VALUE_LENGTH + 1

// Zero encoding requires we define a known tag of sorts. If this tag is encountered, there is ALWAYS a next
//	byte that indicates what the desired byte should be. Remember, this is wchar_t based, so we only have to
//	encode a full 16-bit zero. Anything else does not need to be.
#define	HCN_ENCODE_TAG		0xFFFF				// Tag. Not a valid UNICODE (UTF-16) character.
#define HCN_ENCODE_ZERO		0xFF01				// If second 16-bit character is this, it decodes to a single 0x0000
#define HCN_ENCODE_ORIGINAL 0xFFFF				// If second 16-bit character is this, it decides to a 0xFFFF

// Packet type. Some of these are bidirectional, some are one-sided. BI = bidirectional. Client comes from client, Server comes from server.
enum HCN_packet_type {
	HCN_PACKET_HANDSHAKE = 1,					// BI - Start a conversation with a client. Client sends this first. Server replies in kind.
	HCN_PACKET_KEYVALUE							// Set a key to a value. SJ=ON, SJ=OFF, MTV=ON, etc.
};

// States for the state machine. At various stages of handshake, version exchange, and whatever follows that, we need to track state.
enum HCN_state {
	HCN_STATE_NONE = 1,							// we haven't done anything yet. This indicates a handshake needs to be performed.
	HCN_STATE_HANDSHAKE_C2S,					// Client sent a handshake to the server, waiting for a handshake packet in response which includes server's version, etc.
	HCN_STATE_HANDSHAKE_S2C,					// Server sent a handshake to the client, waiting for a success response 
	HCN_STATE_RUNNING,							// General running state. We're good to send or receive events as needed.
	HCN_STATE_FAILED = 0xff						// Something didn't match or pass inspection. If we're in this state, ignore everything from the other side. 
};

// The preamble to every packet.
struct HCN_preamble {
	unsigned short int magic;					// Always have a magic number.
	unsigned char packet_type;					// Packet type.
	unsigned char packet_length;				// Shouldn't need more than 256 bytes, zero encoding could make this much larger anyway.

	HCN_preamble() { magic = HCN_MAGIC; };		// Always set the magic number on construction. 

};

// HCN_keyvalue - takes a variable-length string of the form "key=value".
struct HCN_keyvalue_packet {
	struct HCN_preamble preamble;

	char keyvalue_length;						// Length of the key value pair.
	char keyvalue[HCN_KEYVALUE_LENGTH];			// an ASCII key-value pair, of the form "key=value". SJ=ON or SJ=OFF for example.

	HCN_keyvalue_packet() { memset(keyvalue, 0, HCN_KEYVALUE_LENGTH); } // On construction, zero out the entire string.

};

// Packet definitions.
struct HCN_packet {								// Generic packet. 
	char data[HCN_MAX_PACKET_LENGTH];
};

// HCN_PACKET_HANDSHAKE - A handshake packet. Includes versioning. 
struct HCN_handshake {							// A handshake packet.
	struct HCN_preamble preamble;				// Always need a preamble.

	unsigned char hcn_state;					// This is the intended state of the connection. 
												// - First packet from client, this is HCN_STATE_HANDSHAKE_C2S, because handshake direction is client-to-server.
												// - Server replies with this set to HCN_STATE_HANDSHAKE_S2C, because handshake direction is server-to-client.
												// - Client responds with HCN_STATE_RUNNING to indicate we're good to go, because the current state on both sides is now "running".
												// - Server does not enter RUNNING state until it receives HCN_STATE_RUNNING from client. 
												// - Client immediately goes to HCN_STATE_RUNNING after sending this.

	unsigned char hcn_type;						// enum of HCN_SERVER_TYPE or HCN_CLIENT_TYPE (based on hcn_state).
	char version[HCN_KEYVALUE_LENGTH];			// There's always a version string at the end.

	HCN_handshake() { memset(version, 0, HCN_KEYVALUE_LENGTH); } // On construction, zero out the entire string.
};

// HCN_callback_keyvalue - used by the application to define a callback for a key/value pair.
//								int player_number is supplied by the application and is simply passed through to the callback function unmodified.
//								char *key is provided by us, copied from the key-value pair array the application provides.
//								char *value is provided by the application, as a string array to copy the value to. MUST adhere to HCN_VALUE_LENGTH
typedef bool(*HCN_callback_keyvalue)(int player_number, char *key, char *value);

// HCN_key_dispatch - Application will use this to define an array of key callback functions.
struct HCN_key_dispatch {
	char *key;
	HCN_callback_keyvalue callback;
};

// Define a way to list keys and values. Useful for decoding ENUMS into text.
struct HCN_enum_to_string {
	int e_num;
	char *name;
};


// An external logger callback. Set by hcn_logger_callback(...) - so a caller can log HCN errors or debug output through it's own logger function
typedef void(*HCN_logger_callback)(int level, const char *string);

// An external "packet sender" that the application defines. Takes player_number and a packet.
typedef void(*HCN_packet_sender)(int player_number, struct HCN_packet *packet);


// Levels for HCN logger.
#define HCN_LOG_FATAL	0													// Completely fatal.
#define HCN_LOG_ERROR	1													// An error, but we can deal with it.
#define HCN_LOG_WARN	2													// Warning.
#define HCN_LOG_INFO	3													// General Info that we might not care about
#define HCN_LOG_DEBUG	4													// Full-on debug mode.
#define HCN_LOG_DEBUG2	5													// even more debugging for certain things like web events and such.


// **********************************************
// All externals are below. Data locations first:
// **********************************************

// The current HCN state.
extern enum HCN_state hcn_state[HCN_MAX_PLAYERS];
extern char hcn_our_version[HCN_VALUE_LENGTH];

// What we are, client or server. And what type.
extern enum HCN_OUR_SIDE hcn_our_side;
extern enum HCN_SERVER_TYPE hcn_server_type;
extern enum HCN_CLIENT_TYPE hcn_client_type[HCN_MAX_PLAYERS];

// Functions. Careful, some are overloaded...
extern void hcn_logger(int level, const char *string, ...);
extern void hcn_client_start();
extern void hcn_set_packet_sender(HCN_packet_sender packet_sender);
extern void hcn_set_keyvalue_callback_list(HCN_key_dispatch *key_list);
extern void hcn_what_we_are(enum HCN_OUR_SIDE our_side, enum HCN_CLIENT_TYPE client_type);
extern void hcn_what_we_are(enum HCN_OUR_SIDE our_side, enum HCN_SERVER_TYPE client_type);

extern void hcn_init(char *version);
extern void hcn_set_logger_callback(HCN_logger_callback callback);
extern void hcn_clear_player(int player_number);
extern bool hcn_valid_packet(struct HCN_packet *packet, unsigned int chat_type);
extern int hcn_encode(struct HCN_packet *packet, struct HCN_packet *source, int packet_length);
extern int hcn_decode(struct HCN_packet *packet, struct HCN_packet *source);
extern bool hcn_running(int player_number);
extern bool hcn_process_chat(int player_number, int chat_type, wchar_t *our_packet);
extern bool hcn_send_keyvalue(int player_number, char *keyvalue);

