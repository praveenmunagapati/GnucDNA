#pragma once

#pragma pack (push, 1)

struct packet_Header;
struct packet_Ping;
struct packet_Pong;
struct packet_Push;
struct packet_Query;
struct packet_QueryHit;
struct packet_Bye;

struct packet_QueryHitItem;
struct packet_QueryHitEx;

struct packet_RouteTableReset;
struct packet_RouteTablePatch;

struct packet_Log;

struct packet_Header		// Size 23
{
	GUID  Guid;					// 0  - 15							
	BYTE  Function;				// 16
	BYTE  TTL;					// 17
	BYTE  Hops;					// 18							
	UINT  Payload;				// 19 - 22
};


struct packet_Ping			// Size 23
{
	packet_Header Header;		// 0  - 22	
};


struct packet_Pong			// Size 37
{
	packet_Header Header;		// 0  - 22							
	WORD Port;					// 23 - 24
	IP Host;					// 25 - 28
	UINT FileCount;			// 29 - 32
	UINT FileSize;				// 33 - 36
};


struct packet_Push			// Size 49
{
	packet_Header Header;		// 0  - 22;							
	GUID ServerID;				// 23 - 38
	UINT Index;				    // 39 - 42
	IP Host;					// 43 - 46
	WORD Port;					// 47 - 48
};


struct packet_Query			// Size 26+
{		
	packet_Header Header;		// 0  - 22						
	WORD Speed;					// 23 - 24
	// Search					// 25+
};


struct packet_QueryHit		// Size 35+
{
	packet_Header Header;		// 0  - 22
	byte TotalHits;				// 23
	WORD Port;					// 24 - 25
	IP   Host;					// 26 - 29
	UINT Speed;					// 30 - 33
	// QueryHitItems			// 34+
	
	// QueryHit Descriptor

	// ClientGuid				// Last 16 bytes
};

struct packet_Bye				// 23+
{
	packet_Header Header;		// 0 - 22
	// Reason					// 23+
};

struct packet_QueryHitItem	// Size 9+
{
	UINT Index;					// 0  - 3
	UINT Size;					// 4  - 7
	
	// FileName					// 8+	
};

struct packet_QueryHitEx	    // Size 6+
{
	byte VendorID[4];			// 0  - 3
	byte Length;				// 4

	// Public Sector
	byte Push		 : 1; // 5
	byte FlagBad 	 : 1;
	byte FlagBusy	 : 1;
	byte FlagStable  : 1;
	byte FlagSpeed	 : 1;
	byte FlagTrash   : 3;

	byte FlagPush	 : 1; // 6
	byte Bad		 : 1;
	byte Busy		 : 1;
	byte Stable  	 : 1;
	byte Speed		 : 1;
	byte Trash		 : 3;

	WORD MetaSize;

	// Private Sector

};

struct packet_RouteTableReset	// Size 29
{
	packet_Header Header;		// 0  - 22

	byte PacketType;			// 23
	UINT TableLength;			// 24 - 27
	byte Infinity;				// 28
};

struct packet_RouteTablePatch	// Size 29+
{
	packet_Header Header;		// 0  - 22

	byte PacketType;			// 23
	byte SeqNum;				// 24
	byte SeqSize;				// 25

	byte Compression;		    // 26
	byte EntryBits;				// 27

	// Patch Table...			// 28+
};

#pragma pack (pop)