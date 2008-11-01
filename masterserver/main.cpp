//-----------------------------------------------------------------------------
//
// Skulltag Master Server Source
// Copyright (C) 2004-2005 Brad Carney
// Copyright (C) 2007-2012 Skulltag Development Team
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the Skulltag Development Team nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
// 4. Redistributions in any form must be accompanied by information on how to
//    obtain complete source code for the software and any accompanying
//    software that uses the software. The source code must either be included
//    in the distribution or be available for no more than the cost of
//    distribution plus a nominal fee, and must be freely redistributable
//    under reasonable conditions. For an executable file, complete source
//    code means the source code for all modules it contains. It does not
//    include source code for modules or files that typically accompany the
//    major components of the operating system on which the executable file
//    runs.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Date (re)created:  3/7/05
//
//
// Filename: main.cpp
//
// Description: 
//
//-----------------------------------------------------------------------------

#include "../src/networkheaders.h"
#include "../src/networkshared.h"
#include "svnrevision.h"
#include "network.h"
#include "main.h"
#include <sstream>

// [BB] Needed for I_GetTime.
#ifdef _MSC_VER
#include <mmsystem.h>
#endif
#ifndef _WIN32
#include <sys/time.h>
#endif

//*****************************************************************************
//	VARIABLES

// Global server list.
static	SERVER_s				g_Servers[MAX_SERVERS];

// Message buffer we write our commands to.
static	NETBUFFER_s				g_MessageBuffer;

// This is the current time for the master server.
static	long					g_lCurrentTime;

// Global list of banned IPs.
static	IPList					g_BannedIPs;
static	IPList					g_BannedIPExemptions;
static	IPList					g_MultiServerExceptions;

// IPs of launchers that we've sent full lists to recently.
static	QueryIPQueue			g_queryIPQueue( 10 );

// [RC] IPs that are completely ignored.
static	QueryIPQueue			g_floodProtectionIPQueue( 10 );
static	QueryIPQueue			g_ShortFloodQueue( 3 );


//*****************************************************************************
//	FUNCTIONS

// Returns time in seconds
int I_GetTime (void)
{
#ifdef _MSC_VER
	static DWORD  basetime;
	DWORD         tm;

	tm = timeGetTime();
	if (!basetime)
		basetime = tm;

	return (tm-basetime)/1000;
#else
	struct timeval tv;
	struct timezone tz;
	long long int thistimereply;
	static long long int basetime;	

	gettimeofday(&tv, &tz);

	thistimereply = tv.tv_sec * 1000 + tv.tv_usec / 1000;

	if (!basetime)
		basetime = thistimereply;

	return (thistimereply - basetime)/1000;
#endif
}

//*****************************************************************************
//
void MASTERSERVER_SendBanlistToServer( SERVER_s &Server )
{
#ifndef STAY_97D2_COMPATIBLE
	NETWORK_ClearBuffer( &g_MessageBuffer );
	NETWORK_WriteByte( &g_MessageBuffer.ByteStream, MASTER_SERVER_BANLIST );

	// Write all the bans.
	NETWORK_WriteLong( &g_MessageBuffer.ByteStream, g_BannedIPs.size( ));
	for ( ULONG i = 0; i < g_BannedIPs.size( ); i++ )
		NETWORK_WriteString( &g_MessageBuffer.ByteStream, g_BannedIPs.getEntryAsString( i, false ).c_str( ));

	// Write all the exceptions.
	NETWORK_WriteLong( &g_MessageBuffer.ByteStream, g_BannedIPExemptions.size( ));
	for ( ULONG i = 0; i < g_BannedIPExemptions.size( ); i++ )
		NETWORK_WriteString( &g_MessageBuffer.ByteStream, g_BannedIPExemptions.getEntryAsString( i, false ).c_str( ));

	NETWORK_LaunchPacket( &g_MessageBuffer, Server.Address );
	Server.lBanlistTime = g_lCurrentTime;
	printf( "-> Banlist sent to %s.\n", NETWORK_AddressToString( Server.Address ));
#endif
}

//*****************************************************************************
//
long MASTERSERVER_CheckIfServerAlreadyExists( NETADDRESS_s Address )
{
	for ( unsigned long	 ulIdx = 0; ulIdx < MAX_SERVERS; ulIdx++ )
	{
		// This slot is not active. Skip over it.
		if ( g_Servers[ulIdx].bAvailable == true )
			continue;

		// The IP of the server we just heard from matches a server in our list.
		if ( NETWORK_CompareAddress( g_Servers[ulIdx].Address, Address, false ))
			return ( ulIdx );
	}

	// IP was not found in server list.
	return ( -1 );
}

//*****************************************************************************
//
long MASTERSERVER_AddServerToList( NETADDRESS_s Address )
{
	for ( unsigned long	ulIdx = 0; ulIdx < MAX_SERVERS; ulIdx++ )
	{
		// This slot is not active. Use it.
		if ( g_Servers[ulIdx].bAvailable == true )
		{
			// Slot is in use; no longer available to be slotted with a new server.
			g_Servers[ulIdx].bAvailable = false;

			// Set the server IP.
			g_Servers[ulIdx].Address = Address;

			// A slot was found; return the index.
			return ( ulIdx );
		}
	}

	// Could not find an available slot to put the server in.
	return ( -1 );
}

//*****************************************************************************
//
bool MASTERSERVER_RefreshIPList( IPList &List, const char *FileName )
{
	std::stringstream oldIPs;

	for ( ULONG ulIdx = 0; ulIdx < List.size(); ulIdx++ )
		oldIPs << List.getEntryAsString ( ulIdx, false ).c_str() << "-";

	if ( !(List.clearAndLoadFromFile( FileName )) )
		std::cerr << List.getErrorMessage();

	std::stringstream newIPs;

	for ( ULONG ulIdx = 0; ulIdx < List.size(); ulIdx++ )
		newIPs << List.getEntryAsString ( ulIdx, false ).c_str() << "-";

	return ( strcmp ( newIPs.str().c_str(), oldIPs.str().c_str() ) != 0 );
}

//*****************************************************************************
//
void MASTERSERVER_InitializeBans( void )
{
	const bool BannedIPsChanged = MASTERSERVER_RefreshIPList ( g_BannedIPs, "banlist.txt" );
	const bool BannedIPExemptionsChanged = MASTERSERVER_RefreshIPList ( g_BannedIPExemptions, "whitelist.txt" );

	if ( !(g_MultiServerExceptions.clearAndLoadFromFile( "multiserver_whitelist.txt" )) )
		std::cerr << g_MultiServerExceptions.getErrorMessage();

	std::cerr << "\nBan list: " << g_BannedIPs.size() << " banned IPs, " << g_BannedIPExemptions.size() << " exemptions." << std::endl;
	std::cerr << "Multi-server exceptions: " << g_MultiServerExceptions.size() << "." << std::endl;

	if ( BannedIPsChanged || BannedIPExemptionsChanged )
		std::cerr << "Ban lists were changed since last refresh\n";
/*
  // [BB] Print all banned IPs, to make sure the IP list has been parsed successfully.
	std::cerr << "Entries in blacklist:\n";
	for ( ULONG ulIdx = 0; ulIdx < g_BannedIPs.size(); ulIdx++ )
		std::cerr << g_BannedIPs.getEntryAsString(ulIdx).c_str();

	// [BB] Print all exemption-IPs, to make sure the IP list has been parsed successfully.
	std::cerr << "Entries in whitelist:\n";
	for ( ULONG ulIdx = 0; ulIdx < g_BannedIPExemptions.size(); ulIdx++ )
		std::cerr << g_BannedIPExemptions.getEntryAsString(ulIdx).c_str();
*/
}

//*****************************************************************************
//
void MASTERSERVER_ParseCommands( BYTESTREAM_s *pByteStream )
{
	long			lCommand;
	NETADDRESS_s	AddressFrom;

	AddressFrom = NETWORK_GetFromAddress( );

	// [RC] If this IP is in our flood queue, ignore it completely.
	if ( g_floodProtectionIPQueue.addressInQueue( AddressFrom ) || g_ShortFloodQueue.addressInQueue( AddressFrom ))
	{
		while ( NETWORK_ReadByte( pByteStream ) != -1 ) // [RC] Is this really necessary?
			;
		return;
	}

	// Is this IP banned? Send the user an explanation, and ignore the IP for 30 seconds.
	if ( !g_BannedIPExemptions.isIPInList( AddressFrom ) && g_BannedIPs.isIPInList( AddressFrom ))
	{
		NETWORK_ClearBuffer( &g_MessageBuffer );
		NETWORK_WriteLong( &g_MessageBuffer.ByteStream, MSC_IPISBANNED );
		NETWORK_LaunchPacket( &g_MessageBuffer, AddressFrom );

		printf( "* Received challenge from banned IP (%s). Ignoring for 10 seconds.\n", NETWORK_AddressToString( AddressFrom ));
		g_queryIPQueue.addAddress( AddressFrom, g_lCurrentTime, &std::cerr );
		return;
	}

	lCommand = NETWORK_ReadLong( pByteStream );
	switch ( lCommand )
	{

	// Server is telling master server of its existance.
	case SERVER_MASTER_CHALLENGE:
	case SERVER_MASTER_CHALLENGE_OVERRIDE:
		{
			long			lServerIdx;
			NETADDRESS_s	Address;

//			if ( lCommand == SERVER_MASTER_CHALLENGE )
				Address = AddressFrom;
/*			else
			{
				char	szAddress[32];

				// Read in the overridden IP that the server is sending us.
				ULONG	ulIP1 = NETWORK_ReadByte( pByteStream );
				ULONG	ulIP2 = NETWORK_ReadByte( pByteStream );
				ULONG	ulIP3 = NETWORK_ReadByte( pByteStream );
				ULONG	ulIP4 = NETWORK_ReadByte( pByteStream );
				ULONG	ulPort = NETWORK_ReadShort( pByteStream );

				// Make sure it's valid.
				if (( ulIP1 > 255 ) || ( ulIP2 > 255 ) || ( ulIP3 > 255 ) || ( ulIP4 > 255 ) || ( ulPort > 65535 ))
				{
					printf( "* Invalid overriden IP (%d.%d.%d.%d:%d) from %s.\n", ulIP1, ulIP2, ulIP3, ulIP4, ulPort, NETWORK_AddressToString( NETWORK_GetFromAddress( )));
					return;
				}

				// Build the IP string.
				sprintf( szAddress, "%d.%d.%d.%d:%d", ulIP1, ulIP2, ulIP3, ulIP4, ulPort );
				NETWORK_StringToAddress( szAddress, &Address );
			}
*/
			lServerIdx = MASTERSERVER_CheckIfServerAlreadyExists( Address );

			// This is a new server; add it to the list.
			if ( lServerIdx == -1 )
			{
				unsigned int iNumOtherServers = 0;

				// First count the number of servers from this IP.
				for ( unsigned long	ulIdx = 0; ulIdx < MAX_SERVERS; ulIdx++ )
				{
					if ( !g_Servers[ulIdx].bAvailable && NETWORK_CompareAddress( g_Servers[ulIdx].Address, Address, true ))
						iNumOtherServers++;
				}

				if ( iNumOtherServers >= 10 && !g_MultiServerExceptions.isIPInList( Address ))
					printf( "* More than 10 servers received from %s. Ignoring request...\n", NETWORK_AddressToString( Address ));
				else
				{
					lServerIdx = MASTERSERVER_AddServerToList( Address );
					if ( lServerIdx == -1 )
						printf( "ERROR: Server list full!\n" );
					else
					{
						g_Servers[lServerIdx].lLastReceived = g_lCurrentTime;						
						printf( "+ Adding %s to the server list.\n", NETWORK_AddressToString( g_Servers[lServerIdx].Address ));
						MASTERSERVER_SendBanlistToServer( g_Servers[lServerIdx] );
					}
				}
			}

			// Command is from a server already on the list. It's just sending us a heartbeat.
			else
				g_Servers[lServerIdx].lLastReceived = g_lCurrentTime;

			// Ignore IP for 10 seconds.
		//	if ( !g_MultiServerExceptions.isIPInList( Address ) )
		//		g_floodProtectionIPQueue.addAddress( AddressFrom, g_lCurrentTime, &std::cerr );
			return;
		}
	// Launcher is asking master server for server list.
	case LAUNCHER_SERVER_CHALLENGE:
		{
			unsigned long	ulIdx;

			NETWORK_ClearBuffer( &g_MessageBuffer );

			// Did this IP query us recently? If so, send it an explanation, and ignore it completely for 3 seconds.
			if ( g_queryIPQueue.addressInQueue( AddressFrom ))
			{
				NETWORK_WriteLong( &g_MessageBuffer.ByteStream, MSC_REQUESTIGNORED );
				NETWORK_LaunchPacket( &g_MessageBuffer, AddressFrom );

				printf( "* Extra launcher challenge from %s. Ignoring for 3 seconds.\n", NETWORK_AddressToString( AddressFrom ));
				g_ShortFloodQueue.addAddress( AddressFrom, g_lCurrentTime, &std::cerr );
				return;
			}
			
			printf( "-> Sending server list to %s.\n", NETWORK_AddressToString( AddressFrom ));

			// Wait 10 seconds before sending this IP the server list again.
			g_queryIPQueue.addAddress( AddressFrom, g_lCurrentTime, &std::cerr );

			// Send the list of servers.
			NETWORK_WriteLong( &g_MessageBuffer.ByteStream, MSC_BEGINSERVERLIST );
			for ( ulIdx = 0; ulIdx < MAX_SERVERS; ulIdx++ )
			{
				// Not an active server.
				if ( g_Servers[ulIdx].bAvailable )
					continue;

				// Tell the launcher the IP of this server on the list.
				NETWORK_WriteByte( &g_MessageBuffer.ByteStream, MSC_SERVER );
				NETWORK_WriteByte( &g_MessageBuffer.ByteStream, g_Servers[ulIdx].Address.abIP[0] );
				NETWORK_WriteByte( &g_MessageBuffer.ByteStream, g_Servers[ulIdx].Address.abIP[1] );
				NETWORK_WriteByte( &g_MessageBuffer.ByteStream, g_Servers[ulIdx].Address.abIP[2] );
				NETWORK_WriteByte( &g_MessageBuffer.ByteStream, g_Servers[ulIdx].Address.abIP[3] );
				NETWORK_WriteShort( &g_MessageBuffer.ByteStream, ntohs( g_Servers[ulIdx].Address.usPort ));
			}

			// Tell the launcher that we're done sending servers.
			NETWORK_WriteByte( &g_MessageBuffer.ByteStream, MSC_ENDSERVERLIST );

			// Send the launcher our packet.
			NETWORK_LaunchPacket( &g_MessageBuffer, AddressFrom );
			return;
		}
	}

	printf( "* Received unknown challenge (%d) from %s. Ignoring for 10 seconds...\n", lCommand, NETWORK_AddressToString( AddressFrom ));
	g_floodProtectionIPQueue.addAddress( AddressFrom, g_lCurrentTime, &std::cerr );
}

//*****************************************************************************
//
void MASTERSERVER_CheckTimeouts( void )
{
	unsigned long	ulIdx;

	for ( ulIdx = 0; ulIdx < MAX_SERVERS; ulIdx++ )
	{
		// An open slot; not an active server.
		if ( g_Servers[ulIdx].bAvailable )
			continue;

		// If the server has timed out, make it an open slot!
		if (( g_lCurrentTime - g_Servers[ulIdx].lLastReceived ) >= 60 )
		{
			g_Servers[ulIdx].bAvailable = true;
			printf( "- Server at %s timed out.\n", NETWORK_AddressToString( g_Servers[ulIdx].Address ));
		}

		// Periodically send the banlist to servers.
		if (( g_lCurrentTime - g_Servers[ulIdx].lBanlistTime ) >= 60 * 15 )
			MASTERSERVER_SendBanlistToServer( g_Servers[ulIdx] );
	}
}

//*****************************************************************************
//
int main( )
{
	BYTESTREAM_s	*pByteStream;
	unsigned long	ulIdx;

	std::cerr << "=== S K U L L T A G | Master ===\n";
	std::cerr << "Revision: "SVN_REVISION_STRING"\n";

#ifdef STAY_97D2_COMPATIBLE
	std::cerr << "Compatible with: 0.97d2" << std::endl;
#else
	std::cerr << "Compatible with: 0.97d3" << std::endl;
#endif
	std::cerr << "Port: " << DEFAULT_MASTER_PORT << std::endl << std::endl;

	// Initialize the network system.
	NETWORK_Construct( DEFAULT_MASTER_PORT );

	for ( ulIdx = 0; ulIdx < MAX_SERVERS; ulIdx++ )
		g_Servers[ulIdx].bAvailable = true;

	// Initialize the message buffer we send messages to the launcher in.
	NETWORK_InitBuffer( &g_MessageBuffer, MAX_UDP_PACKET, BUFFERTYPE_WRITE );
	NETWORK_ClearBuffer( &g_MessageBuffer );

	// Initialize the bans subsystem.
	std::cerr << "Initializing ban list...\n";
	MASTERSERVER_InitializeBans( );
	int lastParsingTime = I_GetTime( );

	// Done setting up!
	std::cerr << "\n=== Master server started! ===\n";

	while ( 1 )
	{
		g_lCurrentTime = I_GetTime( );
		I_DoSelect( );
	
		while ( NETWORK_GetPackets( ))
		{
			// Set up our byte stream.
			pByteStream = &NETWORK_GetNetworkMessageBuffer( )->ByteStream;
			pByteStream->pbStream = NETWORK_GetNetworkMessageBuffer( )->pbData;
			pByteStream->pbStreamEnd = pByteStream->pbStream + NETWORK_GetNetworkMessageBuffer( )->ulCurrentSize;

			// Now parse the packet.
			MASTERSERVER_ParseCommands( pByteStream );
		}

		// Update the ignore queues.
		g_queryIPQueue.adjustHead ( g_lCurrentTime );
		g_floodProtectionIPQueue.adjustHead ( g_lCurrentTime );
		g_ShortFloodQueue.adjustHead ( g_lCurrentTime );

		// See if any servers have timed out.
		MASTERSERVER_CheckTimeouts( );

		// [BB] Reparse the ban list every 15 minutes.
		if ( g_lCurrentTime > lastParsingTime + 15*60 )
		{
			std::cerr << "~ Reparsing the ban lists...\n";
			MASTERSERVER_InitializeBans( );
			lastParsingTime = g_lCurrentTime;
		}
	}	
	
	return ( 0 );
}
