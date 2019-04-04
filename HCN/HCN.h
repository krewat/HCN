// Arthur Krewat - msalerno® - 2019/02/20
//
// HCN project - header and C++ source for Halo client-server communications.
//
// (C)2019 - Kilowatt Computers(USA)
//
// For all the gits out there.
//
// If you're a C++ snob, I don't want to hear it. Look at HAC2, and the HCN shim is C++, or at least
//	as much as I can take without vomitting.
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
// Every step of the way, consistency checking is a must. magic #, state in the initial handshake conversation,
//	packet length, you name it. See HCN_packet_lengths array near the end of this include file.
//	A concerted effort was made to use "safe" string functions everywhere. If you find one that isn't, let me know.
//

/*

   (C) Copyright 2019 Kilowatt Computers

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

#pragma once

#include <memory>

// We need to pack packets as densely as possible, so set this:
#pragma pack(push, 1)

// Basic data structures and enums for packets.
//	Care is taken to make sure enums start at 1 to keep the number of zeroes to a minimum.

#define HCN_CHAT_TYPE	6						// This is an unused chat type that should just "pass through".
									// Sehe's NetEvents used chat type 5.

// What are we, Server or Client?
enum HCN_OUR_SIDE : unsigned char {
	HCN_WE_ARE_UNKNOWN = 0,
	HCN_SERVER,
	HCN_CLIENT
};

// Server type
enum HCN_SERVER_TYPE : unsigned char {
	HCN_NOT_A_SERVER = 0,
	HCN_SERVER_SAPP,
	HCN_SERVER_PHASOR,
	HCN_SERVER_HSE
};

// Client type.
enum HCN_CLIENT_TYPE : unsigned char {
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
#define HCN_MAGIC	0x1F20					// the actual order would be: 0x20, 0x1F because of little Endian

// Max packet length.
#define HCN_MAX_PACKET_LENGTH	500				// it's really 510 I think, but better to be safe than sorry.

// Max players is always 16.
#define HCN_MAX_PLAYERS			16

// Some max sizes for key/value string lengths. Includes the null terminator.
#define HCN_KEY_LENGTH		30
#define HCN_VALUE_LENGTH	128
#define HCN_KEYVALUE_LENGTH	HCN_KEY_LENGTH + HCN_VALUE_LENGTH + 1

// Zero encoding requires we define a known tag. If this tag is encountered, there is ALWAYS a next
//	byte that indicates what the desired byte should be. Remember, this is wchar_t based, so we only have to
//	encode a full 16-bit zero. 
#define	HCN_ENCODE_TAG		0xFFFF				// Tag. Not a valid UNICODE (UTF-16) character.
#define HCN_ENCODE_ZERO		0xFF01				// If second 16-bit character is this, it decodes to a single 0x0000
#define HCN_ENCODE_ORIGINAL	0xFFFF				// If second 16-bit character is this, it decodes to a 0xFFFF


// A 3D vector (location, velocity, whatever). We need to define this here so that the application can actually use the
//	data that HCN provides.
struct HCN_vect3d {
	float x, y, z;

	void clear() {
		x = y = z = 0.0;
	}

	bool valid() {
		return this->x != 0.0 || this->y != 0.0 || this->z != 0.0;
	}

	bool operator=(HCN_vect3d *check)
	{
		return this->x == check->x && this->y == check->y && this->z == check->z;
	}

	HCN_vect3d& operator*=(float rhs)
	{
		x *= rhs;
		y *= rhs;
		z *= rhs;
		return *this;
	}

};


// Packet type. Some of these are bidirectional, some are one-sided. BI = bidirectional. Client comes from client, Server comes from server.
enum HCN_packet_type : unsigned char {
	HCN_PACKET_HANDSHAKE = 1,				// BI - Start a conversation with a client. Client sends this first. Server replies in kind.
	HCN_PACKET_DATAPOINT,					// BI - Report or update a datapoint. INT, FLOAT, whatever. Time remaining and tickrate are the first uses.
	HCN_PACKET_VECTOR,					// BI - Update multiple vectors. Usually server->client for biped location/velocity, or tag locations like flags.
	HCN_PACKET_KEYVALUE,					// BI - Pass a key and a value. SJ=ON, SJ=OFF, MTV=ON, etc.
	HCN_PACKET_TEXT						// BI - Text of various types, possibly with a color set.
};

// States for the state machine. At various stages of handshake, version exchange, and whatever follows that, we need to track state.
enum HCN_state : unsigned char {
	HCN_STATE_NONE = 1,					// we haven't done anything yet. This indicates a handshake needs to be performed.
	HCN_STATE_HANDSHAKE_C2S,				// Client sent a handshake to the server, waiting for a handshake packet in response which includes server's version, etc.
	HCN_STATE_HANDSHAKE_S2C,				// Server sent a handshake to the client, waiting for a success response 
	HCN_STATE_RUNNING					// General running state. We're good to send or receive data as needed.
};

//
// Packet definitions.
//
// NOTE - For any packets that contain multiple sets of data, say datapoints or vectors, the length of the packet varies depending
//		on how many of each there are. So if we send, for example, only one datapoint, the packet contains only one datapoint.
//		If the packet contains a string, it's the last thing in the packet, and the packet is variable length based on how long
//		the string is. This does mean that if we need to send multiple strings, like key/value pairs, the formatting requires
//		a string separator that can be easily parsed. 

struct HCN_packet {						// Generic packet. 
	char data[HCN_MAX_PACKET_LENGTH];
};

// The preamble to every packet. This is designed in such a way that it's contents can be used BEFORE the packet is decoded.
//	Lengths are always non-zero, etc.
struct HCN_preamble {
	unsigned short int magic;				// Always have a magic number.
	unsigned char packet_type;				// Packet type.
	unsigned char packet_length;				// Shouldn't need more than 256 bytes, zero encoding could make this much larger anyway.
	unsigned char encoded_length;				// This is the encoded length, minus any zero termination (wchar_t)

	HCN_preamble() { magic = HCN_MAGIC; };			// Always set the magic number on construction. 

};

// HCN_handshake - A handshake packet. Includes versioning. 
struct HCN_handshake {						// A handshake packet.
	struct HCN_preamble preamble;				// Always need a preamble.

	unsigned char hcn_state;				// This is the intended state of the connection. 
								// - First packet from client, this is HCN_STATE_HANDSHAKE_C2S, because handshake direction is client-to-server.
								// - Server replies with this set to HCN_STATE_HANDSHAKE_S2C, because handshake direction is server-to-client.
								// - Client immediately goes to HCN_STATE_RUNNING after receiving this.

	unsigned char hcn_type;					// enum of HCN_SERVER_TYPE or HCN_CLIENT_TYPE (based on hcn_state).
	char version[HCN_KEYVALUE_LENGTH];			// There's always a version string at the end.

	HCN_handshake() { memset(version, 0, HCN_KEYVALUE_LENGTH); } // On construction, zero out the entire string.

	int size() const { return sizeof(preamble) + sizeof(hcn_type); } // Return the size of the base packet without the version string.

};

//
// HCN data points - provide a mechanism to report or update certain data values by the client or server. 
//
#define HCN_MAX_DATAPOINTS	6				// We can update up to 6 data points at a time.

// HCN_datapoint_type - List of data points we can update.
enum HCN_datapoint_type : unsigned char {
	HCN_DATAPOINT_NOT_DEFINED,				// Don't use zero for anything.
	HCN_DATAPOINT_TIMEREMAINING,				// Time remaining.
	HCN_DATAPOINT_TICKRATE,					// The current tickrate.
	HCN_DATAPOINT_GRAVITY					// Gravity sync.
};

// A single datapoint.
struct HCN_datapoint {
	HCN_datapoint_type dp_type;				// The actual datapoint type.
	union {							// Provide support for the following:
		short int dp_shortint;
		int dp_int;
		unsigned int dp_uint;
		float dp_float;
		//double dp_double;				// Not wanting to go full 64-bit yet.
	};
};

// A datapoint packet.
struct HCN_datapoint_packet {
	struct HCN_preamble preamble;

	unsigned char dp_count;					// The number of datapoints in this packet.
	struct HCN_datapoint dps[HCN_MAX_DATAPOINTS];		// And the actual datapoints.

	int size() const { return sizeof(preamble) + sizeof(dp_count); } // Return the size of the base packet.
};

typedef bool(*HCN_callback_datapoint)(int player_number, HCN_datapoint_type dp_type, struct HCN_datapoint *dp);

// A way to define a list of callbacks for altering datapoints.
struct HCN_datapoint_dispatch {
	HCN_datapoint_type datapoint_type;
	HCN_callback_datapoint callback;
};



//
// HCN vectors - provide a mechanism to monitor or update vectors, of different types, without wasting packets.
//			More condensed than datapoints.
//

#define HCN_MAX_VECTORS		4				// Max vectors in a single packet. Because of zero encoding, we don't want to overrun the max packet length.

// HCN_vector_type - List of vectors we can update
enum HCN_vector_type : unsigned char {
	HCN_VECTOR_NOT_DEFINED,
	HCN_VECTOR_BIPED_LOCATION,				// BIPED location
	HCN_VECTOR_BIPED_VELOCITY,				// BIPED velocity
	HCN_VECTOR_RED_FLAG,					// Sync the location of the red flag.
	HCN_VECTOR_BLUE_FLAG					// Sync the location of the blue flag.
};


// HCN_vector - define a single vector.
struct HCN_vector {
	HCN_vector_type vector_type;				// The vector type to update.
	struct HCN_vect3d vector;				// The actual vector - x/y/z
};

// HCN_vector_packet - a packet that can contain multiple vector updates.
//	More than one vector can be updated at once, up to 4 at a time.
struct HCN_vector_packet {
	struct HCN_preamble preamble;

	unsigned char vector_count;				// Count of vectors we're updating.
	struct HCN_vector vectors[HCN_MAX_VECTORS];		// Define the array of vectors.

	int size() const { return sizeof(preamble) + sizeof(vector_count); } // Return the size of the base packet.

};

typedef bool(*HCN_callback_vector)(int player_number, HCN_vector_type, struct HCN_vect3d *vector);

// A way to define a list of callbacks for altering vectors.
struct HCN_vector_dispatch {
	HCN_vector_type vector_type;
	HCN_callback_vector callback;
};


// HCN_keyvalue - takes a variable-length string of the form "key=value". 
struct HCN_keyvalue_packet {
	struct HCN_preamble preamble;

	char keyvalue_length;					// Length of the key value pair.
	char keyvalue[HCN_KEYVALUE_LENGTH];			// an ASCII key-value pair, of the form "key=value". SJ=ON or SJ=OFF for example.

	HCN_keyvalue_packet() { memset(keyvalue, 0, HCN_KEYVALUE_LENGTH); } // On construction, zero out the entire string.

	int size() const { return sizeof(preamble) + sizeof(keyvalue_length); } // Return the size of the base packet.

};

// HCN_callback_keyvalue - used by the application to define a callback for a key/value pair.
//	int player_number is supplied by the application and is simply passed through to the callback function unmodified.
//	char *key is provided by us, copied from the key-value pair array the application provides.
//	char *value is provided by the application, as a string array to copy the value to. MUST adhere to HCN_VALUE_LENGTH
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

// HCN_text_type - List of text types
enum HCN_text_type : unsigned char {
	HCN_TEXT_NOT_DEFINED,
	HCN_TEXT_CHAT,						// A regular chat type.
	HCN_TEXT_CONSOLE,					// Console text. NARROW CHARACTERS!
	HCN_TEXT_HUD						// HUD text.
};

// HCN_text_color - Mirrors HAC2's text colors for now. If more are needed, start at enum 20
enum HCN_text_color : unsigned char {
	HCN_COLOR_DEFAULT,
	HCN_COLOR_RED,
	HCN_COLOR_GREEN,
	HCN_COLOR_BLUE,
	HCN_COLOR_YELLOW,
	HCN_COLOR_WHITE
};

#define HCN_TEXT_LENGTH		200				// Max length of an HCN text string in wchar_t

// HCN_text_packet - a packet that contains text of some type.
struct HCN_text_packet {
	struct HCN_preamble preamble;

	HCN_text_type text_type;				// The type of text.
	HCN_text_color color;					// The color of the text.
	unsigned char text_length;				// and the length of the text.
	union {
		wchar_t text[HCN_TEXT_LENGTH];			// UTF-16 text.
		char text8[HCN_TEXT_LENGTH];			// 8-bit characters (console)
	};

	int size() const { return sizeof(preamble) + sizeof(text_type) + sizeof(color); } // Return the size of the base packet.

};

typedef bool(*HCN_callback_text)(int player_number, HCN_text_type text_type, struct HCN_text_packet *packet);

// A way to define a list of callbacks for displaying text.
struct HCN_text_dispatch {
	HCN_text_type text_type;
	HCN_callback_text callback;
};


// Turn off tight packing.
#pragma pack(pop)

// An external logger callback. Set by hcn_logger_callback(...) - so a caller can log HCN errors or debug output through it's own logger function
typedef void(*HCN_logger_callback)(int level, const char *string);

// An external "application packet sender" that the application defines. Takes player_number and a packet.
typedef void(*HCN_application_sender)(int player_number, struct HCN_packet *packet);

// Levels for HCN logger.
enum HCN_log_level {
	HCN_LOG_FATAL = 0,					// Completely fatal.
	HCN_LOG_ERROR,						// An error, but we can deal with it.
	HCN_LOG_WARN,						// Warning.
	HCN_LOG_INFO,						// General Info that we might not care about
	HCN_LOG_DEBUG,						// Full-on debug mode.
	HCN_LOG_DEBUG2						// even more debugging for certain things like web events and such.
};

// **********************************************
// All externals are below. Data locations first:
// **********************************************

// The current HCN state.
extern HCN_state hcn_state[HCN_MAX_PLAYERS];
extern char hcn_our_version[HCN_VALUE_LENGTH];

// What we are, client or server. And what type.
extern HCN_OUR_SIDE hcn_our_side;
extern HCN_SERVER_TYPE hcn_server_type;
extern HCN_CLIENT_TYPE hcn_client_type[HCN_MAX_PLAYERS];

// Functions. Careful, some are overloaded...
extern void hcn_logger(int level, const char *string, ...);
extern int hcn_get_debug_level();
extern void hcn_set_debug_level(int level);
extern void hcn_client_start();
extern void hcn_set_packet_sender(HCN_application_sender application_sender);
extern void hcn_set_datapoint_callback_list(HCN_datapoint_dispatch *datapoint_list, int datapoint_list_length);
extern void hcn_set_vector_callback_list(HCN_vector_dispatch *vector_list, int vector_list_length);
extern void hcn_set_keyvalue_callback_list(HCN_key_dispatch *key_list);
extern void hcn_set_text_callback_list(HCN_text_dispatch *text_list, int text_list_length);

extern struct HCN_enum_to_string HCN_state_names[];
extern struct HCN_enum_to_string HCN_server_names[];
extern struct HCN_enum_to_string HCN_client_names[];

extern void hcn_init(char *version);
extern void hcn_what_we_are(HCN_OUR_SIDE our_side, HCN_CLIENT_TYPE client_type);
extern void hcn_what_we_are(HCN_OUR_SIDE our_side, HCN_SERVER_TYPE client_type);
extern void hcn_on_tick();
extern bool hcn_running(int player_number);
extern void hcn_set_logger_callback(HCN_logger_callback callback);
extern void hcn_clear_player(int player_number);
extern bool hcn_value_bool(char *value);
extern bool hcn_valid_packet(struct HCN_packet *packet, unsigned int chat_type);
extern int hcn_encode(struct HCN_packet *packet, struct HCN_packet *source, int packet_length);
extern int hcn_decode(struct HCN_packet *packet, struct HCN_packet *source);
extern void hcn_packet_sender(int player_number, HCN_packet *packet, int packet_length);
extern char *hcn_enum_to_string(int e_num, HCN_enum_to_string *enum_list);
extern bool hcn_process_chat(int player_number, int chat_type, wchar_t *our_packet);
extern bool hcn_datapoint_packet_handler(int player_number, HCN_packet *packet);
extern bool hcn_vector_packet_handler(int player_number, HCN_packet *packet);
extern bool hcn_text_packet_handler(int player_number, HCN_packet *packet);
extern bool hcn_send_datapoints(int player_number, struct HCN_datapoint *dps, int dp_count);
extern bool hcn_send_vectors(int player_number, struct HCN_vector *vectors, int vector_count);
extern bool hcn_send_keyvalue(int player_number, char *keyvalue);
extern bool hcn_send_text(int player_number, HCN_text_type type, HCN_text_color color, wchar_t *text);
extern bool hcn_send_text(int player_number, HCN_text_type type, HCN_text_color color, char *text);


