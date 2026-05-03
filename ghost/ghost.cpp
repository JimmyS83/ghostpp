/*

   Copyright [2008] [Trevor Hogan]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   CODE PORTED FROM THE ORIGINAL GHOST PROJECT: http://ghost.pwner.org/

*/

#include "ghost.h"
#include "util.h"
#include "crc32.h"
#include "sha1.h"
#include "csvparser.h"
#include "config.h"
#include "language.h"
#include "socket.h"
#include "ghostdb.h"
#include "ghostdbsqlite.h"
#include "ghostdbmysql.h"
#include "bnet.h"
#include "map.h"
#include "packed.h"
#include "savegame.h"
#include "gameplayer.h"
#include "gameprotocol.h"
#include "gpsprotocol.h"
#include "game_base.h"
#include "game.h"
#include "game_admin.h"

#include <signal.h>
#include <stdlib.h>
#include <filesystem>

//using namespace std::filesystem;
namespace fs = std::filesystem;

#ifdef WIN32
 #include <ws2tcpip.h>		// for WSAIoctl
#endif

#define __STORMLIB_SELF__
#include <stormlib/StormLib.h>

/*

#include "ghost.h"
#include "util.h"
#include "crc32.h"
#include "sha1.h"
#include "csvparser.h"
#include "config.h"
#include "language.h"
#include "socket.h"
#include "commandpacket.h"
#include "ghostdb.h"
#include "ghostdbsqlite.h"
#include "ghostdbmysql.h"
#include "bncsutilinterface.h"
#include "warden.h"
#include "bnlsprotocol.h"
#include "bnlsclient.h"
#include "bnetprotocol.h"
#include "bnet.h"
#include "map.h"
#include "packed.h"
#include "savegame.h"
#include "replay.h"
#include "gameslot.h"
#include "gameplayer.h"
#include "gameprotocol.h"
#include "gpsprotocol.h"
#include "game_base.h"
#include "game.h"
#include "game_admin.h"
#include "stats.h"
#include "statsdota.h"
#include "sqlite3.h"

*/

#ifdef WIN32
 #include <windows.h>
 #include <winsock.h>
#endif

#include <time.h>

#ifndef WIN32
 #include <sys/time.h>
#endif

#ifdef __APPLE__
 #include <mach/mach_time.h>
#endif

string gCFGFile;
string gLogFile;
uint32_t gLogMethod;
ofstream *gLog = NULL;
CGHost *gGHost = NULL;

uint32_t GetTime( )
{
	return GetTicks( ) / 1000;
}

uint32_t GetTicks( )
{
#ifdef WIN32
	// don't use GetTickCount anymore because it's not accurate enough (~16ms resolution)
	// don't use QueryPerformanceCounter anymore because it isn't guaranteed to be strictly increasing on some systems and thus requires "smoothing" code
	// use timeGetTime instead, which typically has a high resolution (5ms or more) but we request a lower resolution on startup

	return timeGetTime( );
#elif __APPLE__
	uint64_t current = mach_absolute_time( );
	static mach_timebase_info_data_t info = { 0, 0 };
	// get timebase info
	if( info.denom == 0 )
		mach_timebase_info( &info );
	uint64_t elapsednano = current * ( info.numer / info.denom );
	// convert ns to ms
	return elapsednano / 1e6;
#else
	uint32_t ticks;
	struct timespec t;
	clock_gettime( CLOCK_MONOTONIC, &t );
	ticks = t.tv_sec * 1000;
	ticks += t.tv_nsec / 1000000;
	return ticks;
#endif
}

void SignalCatcher2( int s )
{
	CONSOLE_Print( "[!!!] caught signal " + UTIL_ToString( s ) + ", exiting NOW" );

	if( gGHost )
	{
		if( gGHost->m_Exiting )
			exit( 1 );
		else
			gGHost->m_Exiting = true;
	}
	else
		exit( 1 );
}

void SignalCatcher( int s )
{
	// signal( SIGABRT, SignalCatcher2 );
	signal( SIGINT, SignalCatcher2 );

	CONSOLE_Print( "[!!!] caught signal " + UTIL_ToString( s ) + ", exiting nicely" );

	if( gGHost )
		gGHost->m_ExitingNice = true;
	else
		exit( 1 );
}

void CONSOLE_Print( string message )
{
	cout << message << endl;

	// logging

	if( !gLogFile.empty( ) )
	{
		if( gLogMethod == 1 )
		{
			ofstream Log;
			Log.open( gLogFile.c_str( ), ios :: app );

			if( !Log.fail( ) )
			{
				time_t Now = time( NULL );
				string Time = asctime( localtime( &Now ) );

				// erase the newline

				Time.erase( Time.size( ) - 1 );
				Log << "[" << Time << "] " << message << endl;
				Log.close( );
			}
		}
		else if( gLogMethod == 2 )
		{
			if( gLog && !gLog->fail( ) )
			{
				time_t Now = time( NULL );
				string Time = asctime( localtime( &Now ) );

				// erase the newline

				Time.erase( Time.size( ) - 1 );
				*gLog << "[" << Time << "] " << message << endl;
				gLog->flush( );
			}
		}
	}
}

void DEBUG_Print( string message )
{
	cout << message << endl;
}

void DEBUG_Print( BYTEARRAY b )
{
	cout << "{ ";

        for( unsigned int i = 0; i < b.size( ); ++i )
		cout << hex << (int)b[i] << " ";

	cout << "}" << endl;
}

//
// main
//

int main( int argc, char **argv )
{
	srand( time( NULL ) );

	gCFGFile = "ghost.cfg";

	if( argc > 1 && argv[1] )
		gCFGFile = argv[1];

	// read config file

	CConfig CFG;
	CFG.Read( "default.cfg" );
	CFG.Read( gCFGFile );
	gLogFile = CFG.GetString( "bot_log", string( ) );
	gLogMethod = CFG.GetInt( "bot_logmethod", 1 );

	if( !gLogFile.empty( ) )
	{
		if( gLogMethod == 1 )
		{
			// log method 1: open, append, and close the log for every message
			// this works well on Linux but poorly on Windows, particularly as the log file grows in size
			// the log file can be edited/moved/deleted while GHost++ is running
		}
		else if( gLogMethod == 2 )
		{
			// log method 2: open the log on startup, flush the log for every message, close the log on shutdown
			// the log file CANNOT be edited/moved/deleted while GHost++ is running

			gLog = new ofstream( );
			gLog->open( gLogFile.c_str( ), ios :: app );
		}
	}

	CONSOLE_Print( "[GHOST] starting up" );

	if( !gLogFile.empty( ) )
	{
		if( gLogMethod == 1 )
			CONSOLE_Print( "[GHOST] using log method 1, logging is enabled and [" + gLogFile + "] will not be locked" );
		else if( gLogMethod == 2 )
		{
			if( gLog->fail( ) )
				CONSOLE_Print( "[GHOST] using log method 2 but unable to open [" + gLogFile + "] for appending, logging is disabled" );
			else
				CONSOLE_Print( "[GHOST] using log method 2, logging is enabled and [" + gLogFile + "] is now locked" );
		}
	}
	else
		CONSOLE_Print( "[GHOST] no log file specified, logging is disabled" );

	// catch SIGABRT and SIGINT

	// signal( SIGABRT, SignalCatcher );
	signal( SIGINT, SignalCatcher );

#ifndef WIN32
	// disable SIGPIPE since some systems like OS X don't define MSG_NOSIGNAL

	signal( SIGPIPE, SIG_IGN );
#endif

#ifdef WIN32
	// initialize timer resolution
	// attempt to set the resolution as low as possible from 1ms to 5ms

	unsigned int TimerResolution = 0;

        for( unsigned int i = 1; i <= 5; ++i )
	{
		if( timeBeginPeriod( i ) == TIMERR_NOERROR )
		{
			TimerResolution = i;
			break;
		}
		else if( i < 5 )
			CONSOLE_Print( "[GHOST] error setting Windows timer resolution to " + UTIL_ToString( i ) + " milliseconds, trying a higher resolution" );
		else
		{
			CONSOLE_Print( "[GHOST] error setting Windows timer resolution" );
			return 1;
		}
	}

	CONSOLE_Print( "[GHOST] using Windows timer with resolution " + UTIL_ToString( TimerResolution ) + " milliseconds" );
#elif __APPLE__
	// not sure how to get the resolution
#else
	// print the timer resolution

	struct timespec Resolution;

	if( clock_getres( CLOCK_MONOTONIC, &Resolution ) == -1 )
		CONSOLE_Print( "[GHOST] error getting monotonic timer resolution" );
	else
		CONSOLE_Print( "[GHOST] using monotonic timer with resolution " + UTIL_ToString( (double)( Resolution.tv_nsec / 1000 ), 2 ) + " microseconds" );
#endif

#ifdef WIN32
	// initialize winsock

	CONSOLE_Print( "[GHOST] starting winsock" );
	WSADATA wsadata;

	if( WSAStartup( MAKEWORD( 2, 2 ), &wsadata ) != 0 )
	{
		CONSOLE_Print( "[GHOST] error starting winsock" );
		return 1;
	}

	// increase process priority

	CONSOLE_Print( "[GHOST] setting process priority to \"above normal\"" );
	SetPriorityClass( GetCurrentProcess( ), ABOVE_NORMAL_PRIORITY_CLASS );
#endif

	// initialize ghost

	gGHost = new CGHost( &CFG );

	while( 1 )
	{
		// block for 50ms on all sockets - if you intend to perform any timed actions more frequently you should change this
		// that said it's likely we'll loop more often than this due to there being data waiting on one of the sockets but there aren't any guarantees

		if( gGHost->Update( 50000 ) )
			break;
	}

	// shutdown ghost

	CONSOLE_Print( "[GHOST] shutting down" );
	delete gGHost;
	gGHost = NULL;

#ifdef WIN32
	// shutdown winsock

	CONSOLE_Print( "[GHOST] shutting down winsock" );
	WSACleanup( );

	// shutdown timer

	timeEndPeriod( TimerResolution );
#endif

	if( gLog )
	{
		if( !gLog->fail( ) )
			gLog->close( );

		delete gLog;
	}

	return 0;
}

//
// CGHost
//

CGHost :: CGHost( CConfig *CFG )
{
	m_UDPSocket = new CUDPSocket( );
	m_UDPSocket->SetBroadcastTarget( CFG->GetString( "udp_broadcasttarget", string( ) ) );
	m_UDPSocket->SetDontRoute( CFG->GetInt( "udp_dontroute", 0 ) == 0 ? false : true );
	m_ReconnectSocket = NULL;
	m_GPSProtocol = new CGPSProtocol( );
	m_CRC = new CCRC32( );
	m_CRC->Initialize( );
	m_SHA = new CSHA1( );
	string DBType = CFG->GetString( "db_type", "sqlite3" );
	CONSOLE_Print( "[GHOST] opening primary database" );

	if( DBType == "mysql" )
	{
#ifdef GHOST_MYSQL
		m_DB = new CGHostDBMySQL( CFG );
#else
		CONSOLE_Print( "[GHOST] warning - this binary was not compiled with MySQL database support, using SQLite database instead" );
		m_DB = new CGHostDBSQLite( CFG );
#endif
	}
	else
		m_DB = new CGHostDBSQLite( CFG );

	CONSOLE_Print( "[GHOST] opening secondary (local) database" );
	m_DBLocal = new CGHostDBSQLite( CFG );

	// get a list of local IP addresses
	// this list is used elsewhere to determine if a player connecting to the bot is local or not

	CONSOLE_Print( "[GHOST] attempting to find local IP addresses" );

#ifdef WIN32
	// use a more reliable Windows specific method since the portable method doesn't always work properly on Windows
	// code stolen from: http://tangentsoft.net/wskfaq/examples/getifaces.html

	SOCKET sd = WSASocket( AF_INET, SOCK_DGRAM, 0, 0, 0, 0 );

	if( sd == SOCKET_ERROR )
		CONSOLE_Print( "[GHOST] error finding local IP addresses - failed to create socket (error code " + UTIL_ToString( WSAGetLastError( ) ) + ")" );
	else
	{
		INTERFACE_INFO InterfaceList[20];
		unsigned long nBytesReturned;

		if( WSAIoctl( sd, SIO_GET_INTERFACE_LIST, 0, 0, &InterfaceList, sizeof(InterfaceList), &nBytesReturned, 0, 0 ) == SOCKET_ERROR )
			CONSOLE_Print( "[GHOST] error finding local IP addresses - WSAIoctl failed (error code " + UTIL_ToString( WSAGetLastError( ) ) + ")" );
		else
		{
			int nNumInterfaces = nBytesReturned / sizeof(INTERFACE_INFO);

                        for( int i = 0; i < nNumInterfaces; ++i )
			{
				sockaddr_in *pAddress;
				pAddress = (sockaddr_in *)&(InterfaceList[i].iiAddress);
				CONSOLE_Print( "[GHOST] local IP address #" + UTIL_ToString( i + 1 ) + " is [" + string( inet_ntoa( pAddress->sin_addr ) ) + "]" );
				m_LocalAddresses.push_back( UTIL_CreateByteArray( (uint32_t)pAddress->sin_addr.s_addr, false ) );
			}
		}

		closesocket( sd );
	}
#else
	// use a portable method

	char HostName[255];

	if( gethostname( HostName, 255 ) == SOCKET_ERROR )
		CONSOLE_Print( "[GHOST] error finding local IP addresses - failed to get local hostname" );
	else
	{
		CONSOLE_Print( "[GHOST] local hostname is [" + string( HostName ) + "]" );
		struct hostent *HostEnt = gethostbyname( HostName );

		if( !HostEnt )
			CONSOLE_Print( "[GHOST] error finding local IP addresses - gethostbyname failed" );
		else
		{
                        for( int i = 0; HostEnt->h_addr_list[i] != NULL; ++i )
			{
				struct in_addr Address;
				memcpy( &Address, HostEnt->h_addr_list[i], sizeof(struct in_addr) );
				CONSOLE_Print( "[GHOST] local IP address #" + UTIL_ToString( i + 1 ) + " is [" + string( inet_ntoa( Address ) ) + "]" );
				m_LocalAddresses.push_back( UTIL_CreateByteArray( (uint32_t)Address.s_addr, false ) );
			}
		}
	}
#endif

	m_Language = NULL;
	m_Exiting = false;
	m_ExitingNice = false;
	m_Enabled = true;
	m_Version = "17.3";
	m_HostCounter = 1;
	m_AutoHostMaximumGames = CFG->GetInt( "autohost_maxgames", 0 );
	m_AutoHostAutoStartPlayers = CFG->GetInt( "autohost_startplayers", 0 );
	m_AutoHostGameName = CFG->GetString( "autohost_gamename", string( ) );
	m_AutoHostOwner = CFG->GetString( "autohost_owner", string( ) );
	m_LastAutoHostTime = GetTime( );
	m_AutoHostMatchMaking = false;
	m_AutoHostMinimumScore = 0.0;
	m_AutoHostMaximumScore = 0.0;
	m_AllGamesFinished = false;
	m_AllGamesFinishedTime = 0;
	m_TFT = CFG->GetInt( "bot_tft", 1 ) == 0 ? false : true;

	if( m_TFT )
		CONSOLE_Print( "[GHOST] acting as Warcraft III: The Frozen Throne" );
	else
		CONSOLE_Print( "[GHOST] acting as Warcraft III: Reign of Chaos" );

	m_HostPort = CFG->GetInt( "bot_hostport", 6112 );
	m_Reconnect = CFG->GetInt( "bot_reconnect", 1 ) == 0 ? false : true;
	m_ReconnectPort = CFG->GetInt( "bot_reconnectport", 6114 );
	m_DefaultMap = CFG->GetString( "bot_defaultmap", "map" );
	m_AdminGameCreate = CFG->GetInt( "admingame_create", 0 ) == 0 ? false : true;
	m_AdminGamePort = CFG->GetInt( "admingame_port", 6113 );
	m_AdminGamePassword = CFG->GetString( "admingame_password", string( ) );
	m_AdminGameMap = CFG->GetString( "admingame_map", string( ) );
	m_LANWar3Version = CFG->GetInt( "lan_war3version", 26 );
	m_ReplayWar3Version = CFG->GetInt( "replay_war3version", 26 );
	m_ReplayBuildNumber = CFG->GetInt( "replay_buildnumber", 6059 );
	SetConfigs( CFG );

	// load the battle.net connections
	// we're just loading the config data and creating the CBNET classes here, the connections are established later (in the Update function)

        for( uint32_t i = 1; i < 10; ++i )
	{
		// bnet2+ connections are only needed for multi-slot autohosting
		if( i > 1 && m_AutoHostMaximumGames == 0 )
			break;

		string Prefix;

		if( i == 1 )
			Prefix = "bnet_";
		else
			Prefix = "bnet" + UTIL_ToString( i ) + "_";

		string Server = CFG->GetString( Prefix + "server", string( ) );
		string ServerAlias = CFG->GetString( Prefix + "serveralias", string( ) );
		string CDKeyROC = CFG->GetString( Prefix + "cdkeyroc", string( ) );
		string CDKeyTFT = CFG->GetString( Prefix + "cdkeytft", string( ) );
		string CountryAbbrev = CFG->GetString( Prefix + "countryabbrev", "USA" );
		string Country = CFG->GetString( Prefix + "country", "United States" );
		string Locale = CFG->GetString( Prefix + "locale", "system" );
		uint32_t LocaleID;

		if( Locale == "system" )
		{
#ifdef WIN32
			LocaleID = GetUserDefaultLangID( );
#else
			LocaleID = 1033;
#endif
		}
		else
			LocaleID = UTIL_ToUInt32( Locale );

		string UserName = CFG->GetString( Prefix + "username", string( ) );
		string UserPassword = CFG->GetString( Prefix + "password", string( ) );
		string FirstChannel = CFG->GetString( Prefix + "firstchannel", "The Void" );
		string RootAdmin = CFG->GetString( Prefix + "rootadmin", string( ) );
		string BNETCommandTrigger = CFG->GetString( Prefix + "commandtrigger", "!" );

		if( BNETCommandTrigger.empty( ) )
			BNETCommandTrigger = "!";

		bool HoldFriends = CFG->GetInt( Prefix + "holdfriends", 1 ) == 0 ? false : true;
		bool HoldClan = CFG->GetInt( Prefix + "holdclan", 1 ) == 0 ? false : true;
		bool PublicCommands = CFG->GetInt( Prefix + "publiccommands", 1 ) == 0 ? false : true;
		string BNLSServer = CFG->GetString( Prefix + "bnlsserver", string( ) );
		int BNLSPort = CFG->GetInt( Prefix + "bnlsport", 9367 );
		int BNLSWardenCookie = CFG->GetInt( Prefix + "bnlswardencookie", 0 );
		unsigned char War3Version = CFG->GetInt( Prefix + "custom_war3version", 26 );
		BYTEARRAY EXEVersion = UTIL_ExtractNumbers( CFG->GetString( Prefix + "custom_exeversion", string( ) ), 4 );
		BYTEARRAY EXEVersionHash = UTIL_ExtractNumbers( CFG->GetString( Prefix + "custom_exeversionhash", string( ) ), 4 );
		string PasswordHashType = CFG->GetString( Prefix + "custom_passwordhashtype", string( ) );
		string PVPGNRealmName = CFG->GetString( Prefix + "custom_pvpgnrealmname", "PvPGN Realm" );
		uint32_t MaxMessageLength = CFG->GetInt( Prefix + "custom_maxmessagelength", 200 );

		if( Server.empty( ) )
			break;

		if( CDKeyROC.empty( ) )
		{
			CONSOLE_Print( "[GHOST] missing " + Prefix + "cdkeyroc, skipping this battle.net connection" );
			continue;
		}

		if( m_TFT && CDKeyTFT.empty( ) )
		{
			CONSOLE_Print( "[GHOST] missing " + Prefix + "cdkeytft, skipping this battle.net connection" );
			continue;
		}

		if( UserName.empty( ) )
		{
			CONSOLE_Print( "[GHOST] missing " + Prefix + "username, skipping this battle.net connection" );
			continue;
		}

		if( UserPassword.empty( ) )
		{
			CONSOLE_Print( "[GHOST] missing " + Prefix + "password, skipping this battle.net connection" );
			continue;
		}

		CONSOLE_Print( "[GHOST] found battle.net connection #" + UTIL_ToString( i ) + " for server [" + Server + "]" );

		if( Locale == "system" )
		{
#ifdef WIN32
			CONSOLE_Print( "[GHOST] using system locale of " + UTIL_ToString( LocaleID ) );
#else
			CONSOLE_Print( "[GHOST] unable to get system locale, using default locale of 1033" );
#endif
		}

		m_BNETs.push_back( new CBNET( this, Server, ServerAlias, BNLSServer, (uint16_t)BNLSPort, (uint32_t)BNLSWardenCookie, CDKeyROC, CDKeyTFT, CountryAbbrev, Country, LocaleID, UserName, UserPassword, FirstChannel, RootAdmin, BNETCommandTrigger[0], HoldFriends, HoldClan, PublicCommands, War3Version, EXEVersion, EXEVersionHash, PasswordHashType, PVPGNRealmName, MaxMessageLength, i ) );
	}

	if( m_BNETs.empty( ) )
		CONSOLE_Print( "[GHOST] warning - no battle.net connections found in config file" );

	// extract common.j and blizzard.j from War3Patch.mpq if we can
	// these two files are necessary for calculating "map_crc" when loading maps so we make sure to do it before loading the default map
	// see CMap :: Load for more information

	ExtractScripts( );

	// load the default maps (note: make sure to run ExtractScripts first)

	if( m_DefaultMap.size( ) < 4 || m_DefaultMap.substr( m_DefaultMap.size( ) - 4 ) != ".cfg" )
	{
		m_DefaultMap += ".cfg";
		CONSOLE_Print( "[GHOST] adding \".cfg\" to default map -> new default is [" + m_DefaultMap + "]" );
	}

	CConfig MapCFG;
	MapCFG.Read( m_MapCFGPath + m_DefaultMap );
	m_Map = new CMap( this, &MapCFG, m_MapCFGPath + m_DefaultMap );

	if( !m_AdminGameMap.empty( ) )
	{
		if( m_AdminGameMap.size( ) < 4 || m_AdminGameMap.substr( m_AdminGameMap.size( ) - 4 ) != ".cfg" )
		{
			m_AdminGameMap += ".cfg";
			CONSOLE_Print( "[GHOST] adding \".cfg\" to default admin game map -> new default is [" + m_AdminGameMap + "]" );
		}

		CONSOLE_Print( "[GHOST] trying to load default admin game map" );
		CConfig AdminMapCFG;
		AdminMapCFG.Read( m_MapCFGPath + m_AdminGameMap );
		m_AdminMap = new CMap( this, &AdminMapCFG, m_MapCFGPath + m_AdminGameMap );

		if( !m_AdminMap->GetValid( ) )
		{
			CONSOLE_Print( "[GHOST] default admin game map isn't valid, using hardcoded admin game map instead" );
			delete m_AdminMap;
			m_AdminMap = new CMap( this );
		}
	}
	else
	{
		CONSOLE_Print( "[GHOST] using hardcoded admin game map" );
		m_AdminMap = new CMap( this );
	}

	m_AutoHostMap = new CMap( *m_Map );

	// load per-slot autohost maps from config (autohost_cfg1, autohost_cfg2, ...)
	if( m_AutoHostMaximumGames > 0 )
	{
		bool SlotLoadError = false;

		for( uint32_t s = 1; s <= m_AutoHostMaximumGames && !SlotLoadError; ++s )
		{
			string CfgKey = "autohost_cfg" + UTIL_ToString( s );
			string CfgFile = CFG->GetString( CfgKey, string( ) );

			if( CfgFile.empty( ) )
				break;

			if( CfgFile.size( ) < 4 || CfgFile.substr( CfgFile.size( ) - 4 ) != ".cfg" )
				CfgFile += ".cfg";

			CConfig SlotCFG;
			SlotCFG.Read( m_MapCFGPath + CfgFile );
			CMap *SlotMap = new CMap( this, &SlotCFG, m_MapCFGPath + CfgFile );

			if( SlotMap->GetValid( ) )
			{
				// resolve autohost_bnetX alias to a BNET connection
				string BNetAlias = CFG->GetString( "autohost_bnet" + UTIL_ToString( s ), string( ) );
				CBNET *SlotBNet = NULL;

				if( !BNetAlias.empty( ) )
				{
					for( vector<CBNET *> :: iterator i = m_BNETs.begin( ); i != m_BNETs.end( ); ++i )
					{
						if( (*i)->GetServerAlias( ) == BNetAlias )
						{
							SlotBNet = *i;
							break;
						}
					}

					if( !SlotBNet )
					{
						CONSOLE_Print( "[GHOST] fatal error - autohost_bnet" + UTIL_ToString( s ) + " alias [" + BNetAlias + "] not found in any BNET connection, exiting" );
						delete SlotMap;
						m_Exiting = true;
						SlotLoadError = true;
						break;
					}
				}
				else
				{
					// no explicit BNET alias configured for this slot:
					// default to m_BNETs[s] so slot 1 → bnet2_, slot 2 → bnet3_, etc.
					// m_BNETs[0] (bnet_) is always reserved for the manual lobby
					if( m_BNETs.size( ) > s )
						SlotBNet = m_BNETs[s];
					else
					{
						CONSOLE_Print( "[GHOST] warning - not enough BNET connections for autohost slot " + UTIL_ToString( s ) + " (need bnet" + UTIL_ToString( s + 1 ) + "_ in config), using last available" );
						SlotBNet = m_BNETs.back( );
					}
				}

				CONSOLE_Print( "[GHOST] autohost slot " + UTIL_ToString( s ) + " loaded map [" + CfgFile + "] on BNET [" + ( SlotBNet ? SlotBNet->GetServerAlias( ) : "none" ) + "]" );
				CAutoHostSlot Slot;
				Slot.Map = SlotMap;
				Slot.GameName = m_AutoHostGameName;
				Slot.BNet = SlotBNet;
				Slot.SlotIndex = s;
				Slot.LastAutoHostTime = 0;
				// Slot.LastAutoHostTime = GetTime( ) + (uint32_t)( s - 1 ) * 30; // wait 30sec even when ghost is freshly started

				// tell PVPGN the correct port for this slot's BNET connection
				if( SlotBNet )
					SlotBNet->UpdateGameHostPort( (uint16_t)( m_HostPort + s ) );
				m_AutoHostSlots.push_back( Slot );
			}
			else
			{
				CONSOLE_Print( "[GHOST] warning - autohost slot " + UTIL_ToString( s ) + " map [" + CfgFile + "] is invalid, skipping" );
				delete SlotMap;
			}
		}
	}

	m_SaveGame = new CSaveGame( );

	// load the iptocountry data

	LoadIPToCountryData( );

	// create the admin game

	if( m_AdminGameCreate )
	{
		CONSOLE_Print( "[GHOST] creating admin game" );
		m_AdminGame = new CAdminGame( this, m_AdminMap, NULL, m_AdminGamePort, 0, "GHost++ Admin Game", m_AdminGamePassword );

		if( m_AdminGamePort == m_HostPort )
			CONSOLE_Print( "[GHOST] warning - admingame_port and bot_hostport are set to the same value, you won't be able to host any games" );

		// port layout: m_HostPort = manual lobby, m_HostPort+1..+N = autohost slots 1..N
		if( m_AutoHostMaximumGames > 0 && m_ReconnectPort > m_HostPort && m_ReconnectPort <= m_HostPort + m_AutoHostMaximumGames )
			CONSOLE_Print( "[GHOST] warning - bot_reconnectport conflicts with autohost port range (bot_hostport+1 to bot_hostport+" + UTIL_ToString( m_AutoHostMaximumGames ) + "), GProxy++ reconnects may not work" );

		if( m_AutoHostMaximumGames > 0 && m_AdminGamePort > m_HostPort && m_AdminGamePort <= m_HostPort + m_AutoHostMaximumGames )
			CONSOLE_Print( "[GHOST] warning - admingame_port conflicts with autohost port range (bot_hostport+1 to bot_hostport+" + UTIL_ToString( m_AutoHostMaximumGames ) + ")" );
	}
	else
		m_AdminGame = NULL;

	if( m_BNETs.empty( ) && !m_AdminGame )
		CONSOLE_Print( "[GHOST] warning - no battle.net connections found and no admin game created" );

#ifdef GHOST_MYSQL
	CONSOLE_Print( "[GHOST] J2EE GHost++ Version " + m_Version + " (with MySQL support)" );
#else
	CONSOLE_Print( "[GHOST] J2EE GHost++ Version " + m_Version + " (without MySQL support)" );
#endif
}

CGHost :: ~CGHost( )
{
	delete m_UDPSocket;
	delete m_ReconnectSocket;

        for( vector<CTCPSocket *> :: iterator i = m_ReconnectSockets.begin( ); i != m_ReconnectSockets.end( ); ++i )
		delete *i;

	delete m_GPSProtocol;
	delete m_CRC;
	delete m_SHA;

        for( vector<CBNET *> :: iterator i = m_BNETs.begin( ); i != m_BNETs.end( ); ++i )
		delete *i;

	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
		delete *i;

	for( vector<CAutoHostSlot> :: iterator i = m_AutoHostSlots.begin( ); i != m_AutoHostSlots.end( ); ++i )
		delete i->Map;

	delete m_AdminGame;

        for( vector<CBaseGame *> :: iterator i = m_Games.begin( ); i != m_Games.end( ); ++i )
		delete *i;

	delete m_DB;
	delete m_DBLocal;

	// warning: we don't delete any entries of m_Callables here because we can't be guaranteed that the associated threads have terminated
	// this is fine if the program is currently exiting because the OS will clean up after us
	// but if you try to recreate the CGHost object within a single session you will probably leak resources!

	if( !m_Callables.empty( ) )
		CONSOLE_Print( "[GHOST] warning - " + UTIL_ToString( m_Callables.size( ) ) + " orphaned callables were leaked (this is not an error)" );

	delete m_Language;
	delete m_Map;
	delete m_AdminMap;
	delete m_AutoHostMap;
	delete m_SaveGame;
}

bool CGHost :: Update( long usecBlock )
{
	// todotodo: do we really want to shutdown if there's a database error? is there any way to recover from this?

	if( m_DB->HasError( ) )
	{
		CONSOLE_Print( "[GHOST] database error - " + m_DB->GetError( ) );
		return true;
	}

	if( m_DBLocal->HasError( ) )
	{
		CONSOLE_Print( "[GHOST] local database error - " + m_DBLocal->GetError( ) );
		return true;
	}

	// try to exit nicely if requested to do so

	if( m_ExitingNice )
	{
		if( !m_BNETs.empty( ) )
		{
			CONSOLE_Print( "[GHOST] deleting all battle.net connections in preparation for exiting nicely" );

                        for( vector<CBNET *> :: iterator i = m_BNETs.begin( ); i != m_BNETs.end( ); ++i )
				delete *i;

			m_BNETs.clear( );
		}

		if( !m_CurrentGames.empty( ) )
		{
			CONSOLE_Print( "[GHOST] deleting " + UTIL_ToString( m_CurrentGames.size( ) ) + " current game(s) in preparation for exiting nicely" );

			for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
				delete *i;

			m_CurrentGames.clear( );
		}

		if( m_AdminGame )
		{
			CONSOLE_Print( "[GHOST] deleting admin game in preparation for exiting nicely" );
			delete m_AdminGame;
			m_AdminGame = NULL;
		}

		if( m_Games.empty( ) )
		{
			if( !m_AllGamesFinished )
			{
				CONSOLE_Print( "[GHOST] all games finished, waiting 60 seconds for threads to finish" );
				CONSOLE_Print( "[GHOST] there are " + UTIL_ToString( m_Callables.size( ) ) + " threads in progress" );
				m_AllGamesFinished = true;
				m_AllGamesFinishedTime = GetTime( );
			}
			else
			{
				if( m_Callables.empty( ) )
				{
					CONSOLE_Print( "[GHOST] all threads finished, exiting nicely" );
					m_Exiting = true;
				}
				else if( GetTime( ) - m_AllGamesFinishedTime >= 60 )
				{
					CONSOLE_Print( "[GHOST] waited 60 seconds for threads to finish, exiting anyway" );
					CONSOLE_Print( "[GHOST] there are " + UTIL_ToString( m_Callables.size( ) ) + " threads still in progress which will be terminated" );
					m_Exiting = true;
				}
			}
		}
	}

	// update callables

	for( vector<CBaseCallable *> :: iterator i = m_Callables.begin( ); i != m_Callables.end( ); )
	{
		if( (*i)->GetReady( ) )
		{
			m_DB->RecoverCallable( *i );
			delete *i;
			i = m_Callables.erase( i );
		}
		else
                        ++i;
	}

	// create the GProxy++ reconnect listener

	if( m_Reconnect )
	{
		if( !m_ReconnectSocket )
		{
			m_ReconnectSocket = new CTCPServer( );

			if( m_ReconnectSocket->Listen( m_BindAddress, m_ReconnectPort ) )
				CONSOLE_Print( "[GHOST] listening for GProxy++ reconnects on port " + UTIL_ToString( m_ReconnectPort ) );
			else
			{
				CONSOLE_Print( "[GHOST] error listening for GProxy++ reconnects on port " + UTIL_ToString( m_ReconnectPort ) );
				delete m_ReconnectSocket;
				m_ReconnectSocket = NULL;
				m_Reconnect = false;
			}
		}
		else if( m_ReconnectSocket->HasError( ) )
		{
			CONSOLE_Print( "[GHOST] GProxy++ reconnect listener error (" + m_ReconnectSocket->GetErrorString( ) + ")" );
			delete m_ReconnectSocket;
			m_ReconnectSocket = NULL;
			m_Reconnect = false;
		}
	}

	unsigned int NumFDs = 0;

	// take every socket we own and throw it in one giant select statement so we can block on all sockets

	int nfds = 0;
	fd_set fd;
	fd_set send_fd;
	FD_ZERO( &fd );
	FD_ZERO( &send_fd );

	// 1. all battle.net sockets

        for( vector<CBNET *> :: iterator i = m_BNETs.begin( ); i != m_BNETs.end( ); ++i )
		NumFDs += (*i)->SetFD( &fd, &send_fd, &nfds );

	// 2. the current game's server and player sockets

	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
		NumFDs += (*i)->SetFD( &fd, &send_fd, &nfds );

	// 3. the admin game's server and player sockets

	if( m_AdminGame )
		NumFDs += m_AdminGame->SetFD( &fd, &send_fd, &nfds );

	// 4. all running games' player sockets

        for( vector<CBaseGame *> :: iterator i = m_Games.begin( ); i != m_Games.end( ); ++i )
		NumFDs += (*i)->SetFD( &fd, &send_fd, &nfds );

	// 5. the GProxy++ reconnect socket(s)

	if( m_Reconnect && m_ReconnectSocket )
	{
		m_ReconnectSocket->SetFD( &fd, &send_fd, &nfds );
                ++NumFDs;
	}

        for( vector<CTCPSocket *> :: iterator i = m_ReconnectSockets.begin( ); i != m_ReconnectSockets.end( ); ++i )
	{
		(*i)->SetFD( &fd, &send_fd, &nfds );
                ++NumFDs;
	}

	// before we call select we need to determine how long to block for
	// previously we just blocked for a maximum of the passed usecBlock microseconds
	// however, in an effort to make game updates happen closer to the desired latency setting we now use a dynamic block interval
	// note: we still use the passed usecBlock as a hard maximum

        for( vector<CBaseGame *> :: iterator i = m_Games.begin( ); i != m_Games.end( ); ++i )
	{
		if( (*i)->GetNextTimedActionTicks( ) * 1000 < usecBlock )
			usecBlock = (*i)->GetNextTimedActionTicks( ) * 1000;
	}

	// always block for at least 1ms just in case something goes wrong
	// this prevents the bot from sucking up all the available CPU if a game keeps asking for immediate updates
	// it's a bit ridiculous to include this check since, in theory, the bot is programmed well enough to never make this mistake
	// however, considering who programmed it, it's worthwhile to do it anyway

	if( usecBlock < 1000 )
		usecBlock = 1000;

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = usecBlock;

	struct timeval send_tv;
	send_tv.tv_sec = 0;
	send_tv.tv_usec = 0;

#ifdef WIN32
	select( 1, &fd, NULL, NULL, &tv );
	select( 1, NULL, &send_fd, NULL, &send_tv );
#else
	select( nfds + 1, &fd, NULL, NULL, &tv );
	select( nfds + 1, NULL, &send_fd, NULL, &send_tv );
#endif

	if( NumFDs == 0 )
	{
		// we don't have any sockets (i.e. we aren't connected to battle.net maybe due to a lost connection and there aren't any games running)
		// select will return immediately and we'll chew up the CPU if we let it loop so just sleep for 50ms to kill some time

		MILLISLEEP( 50 );
	}

	bool AdminExit = false;
	bool BNETExit = false;

	// update current games (lobbies)

	{
		uint32_t i = 0;

		while( i < m_CurrentGames.size( ) )
		{
			CBaseGame *game = m_CurrentGames[i];
			bool ShouldDelete = game->Update( &fd, &send_fd );

			// game may have removed itself from m_CurrentGames if it started (EventGameStarted)
			bool StillPresent = false;

			for( vector<CBaseGame *> :: iterator j = m_CurrentGames.begin( ); j != m_CurrentGames.end( ); ++j )
			{
				if( *j == game ) { StillPresent = true; break; }
			}

			if( !StillPresent )
			{
				// game started and removed itself - vector already shrank, i stays same
				continue;
			}

			if( ShouldDelete )
			{
				CONSOLE_Print( "[GHOST] deleting current game [" + game->GetGameName( ) + "]" );

				CBNET *AdvBNet = game->GetAdvertisedBNet( );

				m_CurrentGames.erase( m_CurrentGames.begin( ) + i );
				delete game;

				if( AdvBNet )
				{
					AdvBNet->QueueGameUncreate( );

					// only enter chat if no other lobby is advertising on this BNET
					bool BNetStillHasLobby = false;

					for( vector<CBaseGame *> :: iterator j = m_CurrentGames.begin( ); j != m_CurrentGames.end( ); ++j )
					{
						if( (*j)->GetAdvertisedBNet( ) == AdvBNet )
						{
							BNetStillHasLobby = true;
							break;
						}
					}

					if( !BNetStillHasLobby )
						AdvBNet->QueueEnterChat( );
				}
				// i stays same, vector shrank
			}
			else
			{
				game->UpdatePost( &send_fd );
				++i;
			}
		}
	}

	// update admin game

	if( m_AdminGame )
	{
		if( m_AdminGame->Update( &fd, &send_fd ) )
		{
			CONSOLE_Print( "[GHOST] deleting admin game" );
			delete m_AdminGame;
			m_AdminGame = NULL;
			AdminExit = true;
		}
		else if( m_AdminGame )
			m_AdminGame->UpdatePost( &send_fd );
	}

	// update running games

	for( vector<CBaseGame *> :: iterator i = m_Games.begin( ); i != m_Games.end( ); )
	{
		if( (*i)->Update( &fd, &send_fd ) )
		{
			CONSOLE_Print( "[GHOST] deleting game [" + (*i)->GetGameName( ) + "]" );
			EventGameDeleted( *i );
			delete *i;
			i = m_Games.erase( i );
		}
		else
		{
			(*i)->UpdatePost( &send_fd );
                        ++i;
		}
	}

	// update battle.net connections

        for( vector<CBNET *> :: iterator i = m_BNETs.begin( ); i != m_BNETs.end( ); ++i )
	{
		if( (*i)->Update( &fd, &send_fd ) )
			BNETExit = true;
	}

	// update GProxy++ reliable reconnect sockets

	if( m_Reconnect && m_ReconnectSocket )
	{
		CTCPSocket *NewSocket = m_ReconnectSocket->Accept( &fd );

		if( NewSocket )
			m_ReconnectSockets.push_back( NewSocket );
	}

	for( vector<CTCPSocket *> :: iterator i = m_ReconnectSockets.begin( ); i != m_ReconnectSockets.end( ); )
	{
		if( (*i)->HasError( ) || !(*i)->GetConnected( ) || GetTime( ) - (*i)->GetLastRecv( ) >= 10 )
		{
			delete *i;
			i = m_ReconnectSockets.erase( i );
			continue;
		}

		(*i)->DoRecv( &fd );
		string *RecvBuffer = (*i)->GetBytes( );
		BYTEARRAY Bytes = UTIL_CreateByteArray( (unsigned char *)RecvBuffer->c_str( ), RecvBuffer->size( ) );

		// a packet is at least 4 bytes

		if( Bytes.size( ) >= 4 )
		{
			if( Bytes[0] == GPS_HEADER_CONSTANT )
			{
				// bytes 2 and 3 contain the length of the packet

				uint16_t Length = UTIL_ByteArrayToUInt16( Bytes, false, 2 );

				if( Length >= 4 )
				{
					if( Bytes.size( ) >= Length )
					{
						if( Bytes[1] == CGPSProtocol :: GPS_RECONNECT && Length == 13 )
						{
							unsigned char PID = Bytes[4];
							uint32_t ReconnectKey = UTIL_ByteArrayToUInt32( Bytes, false, 5 );
							uint32_t LastPacket = UTIL_ByteArrayToUInt32( Bytes, false, 9 );

							// look for a matching player in a running game

							CGamePlayer *Match = NULL;

                                                        for( vector<CBaseGame *> :: iterator j = m_Games.begin( ); j != m_Games.end( ); ++j )
							{
								if( (*j)->GetGameLoaded( ) )
								{
									CGamePlayer *Player = (*j)->GetPlayerFromPID( PID );

									if( Player && Player->GetGProxy( ) && Player->GetGProxyReconnectKey( ) == ReconnectKey )
									{
										Match = Player;
										break;
									}
								}
							}

							if( Match )
							{
								// reconnect successful!

								*RecvBuffer = RecvBuffer->substr( Length );
								Match->EventGProxyReconnect( *i, LastPacket );
								i = m_ReconnectSockets.erase( i );
								continue;
							}
							else
							{
								(*i)->PutBytes( m_GPSProtocol->SEND_GPSS_REJECT( REJECTGPS_NOTFOUND ) );
								(*i)->DoSend( &send_fd );
								delete *i;
								i = m_ReconnectSockets.erase( i );
								continue;
							}
						}
						else
						{
							(*i)->PutBytes( m_GPSProtocol->SEND_GPSS_REJECT( REJECTGPS_INVALID ) );
							(*i)->DoSend( &send_fd );
							delete *i;
							i = m_ReconnectSockets.erase( i );
							continue;
						}
					}
				}
				else
				{
					(*i)->PutBytes( m_GPSProtocol->SEND_GPSS_REJECT( REJECTGPS_INVALID ) );
					(*i)->DoSend( &send_fd );
					delete *i;
					i = m_ReconnectSockets.erase( i );
					continue;
				}
			}
			else
			{
				(*i)->PutBytes( m_GPSProtocol->SEND_GPSS_REJECT( REJECTGPS_INVALID ) );
				(*i)->DoSend( &send_fd );
				delete *i;
				i = m_ReconnectSockets.erase( i );
				continue;
			}
		}

		(*i)->DoSend( &send_fd );
                ++i;
	}

	// autohost

	if( !m_AutoHostGameName.empty( ) && m_AutoHostMaximumGames != 0 && m_AutoHostAutoStartPlayers != 0 && GetTime( ) - m_LastAutoHostTime >= 30 )
	{
		if( !m_ExitingNice && m_Enabled )
		{
			uint32_t TotalActive = (uint32_t)( m_CurrentGames.size( ) + m_Games.size( ) );

			if( !m_AutoHostSlots.empty( ) )
			{
				// multi-slot autohosting: maintain one lobby per slot

				for( uint32_t s = 0; s < m_AutoHostSlots.size( ) && TotalActive < m_MaxGames; ++s )
				{
					CMap *SlotMap = m_AutoHostSlots[s].Map;

					// check if this slot already has a lobby or running game (by slot index)
					uint32_t ThisSlotIndex = m_AutoHostSlots[s].SlotIndex;
					bool SlotActive = false;

					for( vector<CBaseGame *> :: iterator g = m_CurrentGames.begin( ); g != m_CurrentGames.end( ); ++g )
					{
						if( (*g)->GetAutoHostSlotIndex( ) == ThisSlotIndex )
						{
							SlotActive = true;
							break;
						}
					}

					if( !SlotActive )
					{
						for( vector<CBaseGame *> :: iterator g = m_Games.begin( ); g != m_Games.end( ); ++g )
						{
							if( (*g)->GetAutoHostSlotIndex( ) == ThisSlotIndex )
							{
								SlotActive = true;
								break;
							}
						}
					}

					if( SlotActive )
						continue;

					if( !SlotMap->GetValid( ) )
					{
						CONSOLE_Print( "[GHOST] autohost slot " + UTIL_ToString( s + 1 ) + " map [" + SlotMap->GetCFGFile( ) + "] is invalid, skipping" );
						continue;
					}

					// use fixed slot index in game name and port - never changes regardless of host counter
					string GameName = m_AutoHostGameName + " #" + UTIL_ToString( m_AutoHostSlots[s].SlotIndex );
					string SlotOwner = m_AutoHostSlots[s].BNet ? m_AutoHostSlots[s].BNet->GetUserName( ) : m_AutoHostOwner;

					if( GameName.size( ) > 31 )
					{
						CONSOLE_Print( "[GHOST] stopped auto hosting slot " + UTIL_ToString( m_AutoHostSlots[s].SlotIndex ) + ", game name [" + GameName + "] is too long" );
						continue;
					}

					// check per-slot timer - stagger slot creation
					if( GetTime( ) - m_AutoHostSlots[s].LastAutoHostTime < 30 )
						continue;

					size_t PrevSize = m_CurrentGames.size();
					CreateGame(SlotMap, GAME_PUBLIC, false, GameName, SlotOwner, SlotOwner, m_AutoHostServer, false, true, m_AutoHostSlots[s].BNet, m_AutoHostSlots[s].SlotIndex);

					if (m_CurrentGames.size() > PrevSize)
					{
						CBaseGame* NewLobby = m_CurrentGames.back();
						NewLobby->SetAutoStartPlayers(m_AutoHostAutoStartPlayers);
						NewLobby->SetAutoHostSlotIndex(m_AutoHostSlots[s].SlotIndex);

						string SlotSuffix = " #" + UTIL_ToString(m_AutoHostSlots[s].SlotIndex);
						string BaseVHName = m_VirtualHostName;

						if (BaseVHName.size() + SlotSuffix.size() > 15)
							BaseVHName = BaseVHName.substr(0, 15 - SlotSuffix.size());

						NewLobby->SetVirtualHostName(BaseVHName + SlotSuffix);
						m_AutoHostSlots[s].LastAutoHostTime = GetTime();
					}

					TotalActive = (uint32_t)( m_CurrentGames.size( ) + m_Games.size( ) );
				}
			}
			else
			{
				// single-map autohosting: original behavior, one lobby at a time

				bool AutoHostActive = false;

				for( vector<CBaseGame *> :: iterator g = m_CurrentGames.begin( ); g != m_CurrentGames.end( ); ++g )
					if( (*g)->GetIsAutoHostGame( ) ) { AutoHostActive = true; break; }

				if( !AutoHostActive )
					for( vector<CBaseGame *> :: iterator g = m_Games.begin( ); g != m_Games.end( ); ++g )
						if( (*g)->GetIsAutoHostGame( ) ) { AutoHostActive = true; break; }

				if( !AutoHostActive && TotalActive < m_MaxGames && TotalActive < m_AutoHostMaximumGames )
				{
					if( m_AutoHostMap->GetValid( ) )
					{
						string GameName = m_AutoHostGameName + " #1";

						if( GameName.size( ) <= 31 )
						{
							CreateGame( m_AutoHostMap, GAME_PUBLIC, false, GameName, m_AutoHostOwner, m_AutoHostOwner, m_AutoHostServer, false, true, m_BNETs.empty( ) ? NULL : m_BNETs[0], 1 );

							if( !m_CurrentGames.empty( ) )
							{
								CBaseGame *NewLobby = m_CurrentGames.back( );
								NewLobby->SetAutoStartPlayers( m_AutoHostAutoStartPlayers );
								NewLobby->SetAutoHostSlotIndex( 1 );

								if( m_AutoHostMatchMaking )
								{
									if( !m_Map->GetMapMatchMakingCategory( ).empty( ) )
									{
										if( !( m_Map->GetMapOptions( ) & MAPOPT_FIXEDPLAYERSETTINGS ) )
											CONSOLE_Print( "[GHOST] autohostmm - map_matchmakingcategory [" + m_Map->GetMapMatchMakingCategory( ) + "] found but matchmaking can only be used with fixed player settings, matchmaking disabled" );
										else
										{
											CONSOLE_Print( "[GHOST] autohostmm - map_matchmakingcategory [" + m_Map->GetMapMatchMakingCategory( ) + "] found, matchmaking enabled" );
											NewLobby->SetMatchMaking( true );
											NewLobby->SetMinimumScore( m_AutoHostMinimumScore );
											NewLobby->SetMaximumScore( m_AutoHostMaximumScore );
										}
									}
									else
										CONSOLE_Print( "[GHOST] autohostmm - map_matchmakingcategory not found, matchmaking disabled" );
								}
							}
						}
						else
						{
							CONSOLE_Print( "[GHOST] stopped auto hosting, next game name [" + GameName + "] is too long (the maximum is 31 characters)" );
							m_AutoHostGameName.clear( );
							m_AutoHostOwner.clear( );
							m_AutoHostServer.clear( );
							m_AutoHostMaximumGames = 0;
							m_AutoHostAutoStartPlayers = 0;
							m_AutoHostMatchMaking = false;
							m_AutoHostMinimumScore = 0.0;
							m_AutoHostMaximumScore = 0.0;
						}
					}
					else
					{
						CONSOLE_Print( "[GHOST] stopped auto hosting, map config file [" + m_AutoHostMap->GetCFGFile( ) + "] is invalid" );
						m_AutoHostGameName.clear( );
						m_AutoHostOwner.clear( );
						m_AutoHostServer.clear( );
						m_AutoHostMaximumGames = 0;
						m_AutoHostAutoStartPlayers = 0;
						m_AutoHostMatchMaking = false;
						m_AutoHostMinimumScore = 0.0;
						m_AutoHostMaximumScore = 0.0;
					}
				}
			}
		}

		m_LastAutoHostTime = GetTime( );
	}

	return m_Exiting || AdminExit || BNETExit;
}

void CGHost :: EventBNETConnecting( CBNET *bnet )
{
	if( m_AdminGame )
		m_AdminGame->SendAllChat( m_Language->ConnectingToBNET( bnet->GetServer( ) ) );

	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
		(*i)->SendAllChat( m_Language->ConnectingToBNET( bnet->GetServer( ) ) );
}

void CGHost :: EventBNETConnected( CBNET *bnet )
{
	if( m_AdminGame )
		m_AdminGame->SendAllChat( m_Language->ConnectedToBNET( bnet->GetServer( ) ) );

	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
		(*i)->SendAllChat( m_Language->ConnectedToBNET( bnet->GetServer( ) ) );
}

void CGHost :: EventBNETDisconnected( CBNET *bnet )
{
	if( m_AdminGame )
		m_AdminGame->SendAllChat( m_Language->DisconnectedFromBNET( bnet->GetServer( ) ) );

	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
		(*i)->SendAllChat( m_Language->DisconnectedFromBNET( bnet->GetServer( ) ) );
}

void CGHost :: EventBNETLoggedIn( CBNET *bnet )
{
	if( m_AdminGame )
		m_AdminGame->SendAllChat( m_Language->LoggedInToBNET( bnet->GetServer( ) ) );

	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
		(*i)->SendAllChat( m_Language->LoggedInToBNET( bnet->GetServer( ) ) );
}

void CGHost :: EventBNETGameRefreshed( CBNET *bnet )
{
	if( m_AdminGame )
		m_AdminGame->SendAllChat( m_Language->BNETGameHostingSucceeded( bnet->GetServer( ) ) );

	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
	{
		if( (*i)->GetAdvertisedBNet( ) == bnet )
			(*i)->EventGameRefreshed( bnet->GetServer( ) );
	}
}

void CGHost :: EventBNETGameRefreshFailed( CBNET *bnet )
{
	for( vector<CBaseGame *> :: iterator g = m_CurrentGames.begin( ); g != m_CurrentGames.end( ); ++g )
	{
		CBaseGame *game = *g;

		if( game->GetAdvertisedBNet( ) != bnet )
			continue;

		if( game->GetAdvertisedBNet( ) )
		{
			game->GetAdvertisedBNet( )->QueueChatCommand( m_Language->UnableToCreateGameTryAnotherName( bnet->GetServer( ), game->GetGameName( ) ) );

			if( !game->GetCreatorName( ).empty( ) )
				game->GetAdvertisedBNet( )->QueueChatCommand( m_Language->UnableToCreateGameTryAnotherName( bnet->GetServer( ), game->GetGameName( ) ), game->GetCreatorName( ), true );
		}

		if( m_AdminGame )
			m_AdminGame->SendAllChat( m_Language->BNETGameHostingFailed( bnet->GetServer( ), game->GetGameName( ) ) );

		game->SendAllChat( m_Language->UnableToCreateGameTryAnotherName( bnet->GetServer( ), game->GetGameName( ) ) );

		if( game->GetNumHumanPlayers( ) == 0 )
			game->SetExiting( true );

		game->SetRefreshError( true );
	}
}

void CGHost :: EventBNETConnectTimedOut( CBNET *bnet )
{
	if( m_AdminGame )
		m_AdminGame->SendAllChat( m_Language->ConnectingToBNETTimedOut( bnet->GetServer( ) ) );

	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
		(*i)->SendAllChat( m_Language->ConnectingToBNETTimedOut( bnet->GetServer( ) ) );
}

void CGHost :: EventBNETWhisper( CBNET *bnet, string user, string message )
{
	if( m_AdminGame )
	{
		m_AdminGame->SendAdminChat( "[W: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );

		for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
			(*i)->SendLocalAdminChat( "[W: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );

                for( vector<CBaseGame *> :: iterator i = m_Games.begin( ); i != m_Games.end( ); ++i )
			(*i)->SendLocalAdminChat( "[W: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );
	}
}

void CGHost :: EventBNETChat( CBNET *bnet, string user, string message )
{
	if( m_AdminGame )
	{
		m_AdminGame->SendAdminChat( "[L: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );

		for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
			(*i)->SendLocalAdminChat( "[L: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );

                for( vector<CBaseGame *> :: iterator i = m_Games.begin( ); i != m_Games.end( ); ++i )
			(*i)->SendLocalAdminChat( "[L: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );
	}
}

void CGHost :: EventBNETEmote( CBNET *bnet, string user, string message )
{
	if( m_AdminGame )
	{
		m_AdminGame->SendAdminChat( "[E: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );

		for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
			(*i)->SendLocalAdminChat( "[E: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );

                for( vector<CBaseGame *> :: iterator i = m_Games.begin( ); i != m_Games.end( ); ++i )
			(*i)->SendLocalAdminChat( "[E: " + bnet->GetServerAlias( ) + "] [" + user + "] " + message );
	}
}

void CGHost :: EventGameDeleted( CBaseGame *game )
{
	CBNET *AdvBNet = game->GetAdvertisedBNet( );

	if( AdvBNet )
	{
		AdvBNet->QueueChatCommand( m_Language->GameIsOver( game->GetDescription( ) ) );

		if( !game->GetCreatorName( ).empty( ) )
			AdvBNet->QueueChatCommand( m_Language->GameIsOver( game->GetDescription( ) ), game->GetCreatorName( ), true );

		// port is free, bot could go back to intercept orders
		// but only if on this BNET connection doesnt exists another active lobby or live game
		bool BNetStillBusy = false;

		for (vector<CBaseGame*> ::iterator i = m_CurrentGames.begin(); i != m_CurrentGames.end(); ++i)
		{
			if ((*i)->GetAdvertisedBNet() == AdvBNet)
			{
				BNetStillBusy = true;
				break;
			}
		}

		if (!BNetStillBusy)
		{
			for (vector<CBaseGame*> ::iterator i = m_Games.begin(); i != m_Games.end(); ++i)
			{
				if ((*i)->GetAdvertisedBNet() == AdvBNet)
				{
					BNetStillBusy = true;
					break;
				}
			}
		}

		if (!BNetStillBusy)
			AdvBNet->QueueEnterChat();

		if ( game->GetIsAutoHostGame( ) )
		{
			for ( uint32_t s = 0; s < m_AutoHostSlots.size( ); ++s )
			{
				if ( m_AutoHostSlots[s].SlotIndex == game->GetAutoHostSlotIndex( ) )
				{
					m_AutoHostSlots[s].LastAutoHostTime = GetTime( );
					break;
				}
			}
		}
	}
}

CBaseGame *CGHost :: GetManualLobby( )
{
	for( vector<CBaseGame *> :: iterator i = m_CurrentGames.begin( ); i != m_CurrentGames.end( ); ++i )
	{
		if( !(*i)->GetIsAutoHostGame( ) )
			return *i;
	}

	return NULL;
}



void CGHost :: ReloadConfigs( )
{
	CConfig CFG;
	CFG.Read( "default.cfg" );
	CFG.Read( gCFGFile );
	SetConfigs( &CFG );
}

void CGHost :: SetConfigs( CConfig *CFG )
{
	// this doesn't set EVERY config value since that would potentially require reconfiguring the battle.net connections
	// it just set the easily reloadable values

	m_LanguageFile = CFG->GetString( "bot_language", "language.cfg" );
	delete m_Language;
	m_Language = new CLanguage( m_LanguageFile );
	m_Warcraft3Path = UTIL_AddPathSeperator( CFG->GetString( "bot_war3path", "C:\\Program Files\\Warcraft III\\" ) );
	m_BindAddress = CFG->GetString( "bot_bindaddress", string( ) );
	m_ReconnectWaitTime = CFG->GetInt( "bot_reconnectwaittime", 3 );
	m_MaxGames = CFG->GetInt( "bot_maxgames", 5 );
	string BotCommandTrigger = CFG->GetString( "bot_commandtrigger", "!" );

	if( BotCommandTrigger.empty( ) )
		BotCommandTrigger = "!";

	m_CommandTrigger = BotCommandTrigger[0];
	m_MapCFGPath = UTIL_AddPathSeperator( CFG->GetString( "bot_mapcfgpath", string( ) ) );
	m_SaveGamePath = UTIL_AddPathSeperator( CFG->GetString( "bot_savegamepath", string( ) ) );
	m_MapPath = UTIL_AddPathSeperator( CFG->GetString( "bot_mappath", string( ) ) );
	m_SaveReplays = CFG->GetInt( "bot_savereplays", 0 ) == 0 ? false : true;
	m_ReplayPath = UTIL_AddPathSeperator( CFG->GetString( "bot_replaypath", string( ) ) );
	m_VirtualHostName = CFG->GetString( "bot_virtualhostname", "|cFF4080C0GHost" );
	m_HideIPAddresses = CFG->GetInt( "bot_hideipaddresses", 0 ) == 0 ? false : true;
	m_CheckMultipleIPUsage = CFG->GetInt( "bot_checkmultipleipusage", 1 ) == 0 ? false : true;

	// fill map and cfg cache at startup
	try {
		fs::path p(m_MapPath);
		if (fs::exists(p))
			for (fs::directory_iterator i(p); i != fs::directory_iterator(); ++i)
				if (!is_directory(i->status()))
					m_CachedMapList.push_back(i->path().filename().string());
		sort(m_CachedMapList.begin(), m_CachedMapList.end());
	}
	catch (...) {}

	try {
		fs::path p(m_MapCFGPath);
		if (exists(p))
			for (fs::directory_iterator i(p); i != fs::directory_iterator(); ++i)
				if (!is_directory(i->status()) && i->path().extension() == ".cfg")
					m_CachedCFGList.push_back(i->path().filename().string());
		sort(m_CachedCFGList.begin(), m_CachedCFGList.end());
	}
	catch (...) {}

	if( m_VirtualHostName.size( ) > 15 )
	{
		m_VirtualHostName = "|cFF4080C0GHost";
		CONSOLE_Print( "[GHOST] warning - bot_virtualhostname is longer than 15 characters, using default virtual host name" );
	}

	m_SpoofChecks = CFG->GetInt( "bot_spoofchecks", 2 );
	m_RequireSpoofChecks = CFG->GetInt( "bot_requirespoofchecks", 0 ) == 0 ? false : true;
	m_AllAdmins = CFG->GetInt("bot_alladmins", 0) == 0 ? false : true;
	m_ReserveAdmins = CFG->GetInt( "bot_reserveadmins", 1 ) == 0 ? false : true;
	m_RefreshMessages = CFG->GetInt( "bot_refreshmessages", 0 ) == 0 ? false : true;
	m_AutoLock = CFG->GetInt( "bot_autolock", 0 ) == 0 ? false : true;
	m_AutoSave = CFG->GetInt( "bot_autosave", 0 ) == 0 ? false : true;
	m_AllowDownloads = CFG->GetInt( "bot_allowdownloads", 0 );
	m_PingDuringDownloads = CFG->GetInt( "bot_pingduringdownloads", 0 ) == 0 ? false : true;
	m_MaxDownloaders = CFG->GetInt( "bot_maxdownloaders", 3 );
	m_MaxDownloadSpeed = CFG->GetInt( "bot_maxdownloadspeed", 100 );
	m_LCPings = CFG->GetInt( "bot_lcpings", 1 ) == 0 ? false : true;
	m_AutoKickPing = CFG->GetInt( "bot_autokickping", 400 );
	m_BanMethod = CFG->GetInt( "bot_banmethod", 1 );
	m_IPBlackListFile = CFG->GetString( "bot_ipblacklistfile", "ipblacklist.txt" );
	m_LobbyTimeLimit = CFG->GetInt( "bot_lobbytimelimit", 10 );
	m_Latency = CFG->GetInt( "bot_latency", 100 );
	m_SyncLimit = CFG->GetInt( "bot_synclimit", 50 );
	m_VoteKickAllowed = CFG->GetInt( "bot_votekickallowed", 1 ) == 0 ? false : true;
	m_VoteKickPercentage = CFG->GetInt( "bot_votekickpercentage", 100 );

	if( m_VoteKickPercentage > 100 )
	{
		m_VoteKickPercentage = 100;
		CONSOLE_Print( "[GHOST] warning - bot_votekickpercentage is greater than 100, using 100 instead" );
	}

	m_MOTDFile = CFG->GetString( "bot_motdfile", "motd.txt" );
	m_GameLoadedFile = CFG->GetString( "bot_gameloadedfile", "gameloaded.txt" );
	m_GameOverFile = CFG->GetString( "bot_gameoverfile", "gameover.txt" );
	m_LocalAdminMessages = CFG->GetInt( "bot_localadminmessages", 1 ) == 0 ? false : true;
	m_TCPNoDelay = CFG->GetInt( "tcp_nodelay", 0 ) == 0 ? false : true;
	m_MatchMakingMethod = CFG->GetInt( "bot_matchmakingmethod", 1 );
	m_MapGameType = CFG->GetUInt( "bot_mapgametype", 0 );
}

void CGHost :: ExtractScripts( )
{
	string PatchMPQFileName = m_Warcraft3Path + "War3Patch.mpq";
	HANDLE PatchMPQ;

	if( SFileOpenArchive( PatchMPQFileName.c_str( ), 0, MPQ_OPEN_FORCE_MPQ_V1, &PatchMPQ ) )
	{
		CONSOLE_Print( "[GHOST] loading MPQ file [" + PatchMPQFileName + "]" );
		HANDLE SubFile;

		// common.j

		if( SFileOpenFileEx( PatchMPQ, "Scripts\\common.j", 0, &SubFile ) )
		{
			uint32_t FileLength = SFileGetFileSize( SubFile, NULL );

			if( FileLength > 0 && FileLength != 0xFFFFFFFF )
			{
				char *SubFileData = new char[FileLength];
				DWORD BytesRead = 0;

				if( SFileReadFile( SubFile, SubFileData, FileLength, &BytesRead ) )
				{
					CONSOLE_Print( "[GHOST] extracting Scripts\\common.j from MPQ file to [" + m_MapCFGPath + "common.j]" );
					UTIL_FileWrite( m_MapCFGPath + "common.j", (unsigned char *)SubFileData, BytesRead );
				}
				else
					CONSOLE_Print( "[GHOST] warning - unable to extract Scripts\\common.j from MPQ file" );

				delete [] SubFileData;
			}

			SFileCloseFile( SubFile );
		}
		else
			CONSOLE_Print( "[GHOST] couldn't find Scripts\\common.j in MPQ file" );

		// blizzard.j

		if( SFileOpenFileEx( PatchMPQ, "Scripts\\blizzard.j", 0, &SubFile ) )
		{
			uint32_t FileLength = SFileGetFileSize( SubFile, NULL );

			if( FileLength > 0 && FileLength != 0xFFFFFFFF )
			{
				char *SubFileData = new char[FileLength];
				DWORD BytesRead = 0;

				if( SFileReadFile( SubFile, SubFileData, FileLength, &BytesRead ) )
				{
					CONSOLE_Print( "[GHOST] extracting Scripts\\blizzard.j from MPQ file to [" + m_MapCFGPath + "blizzard.j]" );
					UTIL_FileWrite( m_MapCFGPath + "blizzard.j", (unsigned char *)SubFileData, BytesRead );
				}
				else
					CONSOLE_Print( "[GHOST] warning - unable to extract Scripts\\blizzard.j from MPQ file" );

				delete [] SubFileData;
			}

			SFileCloseFile( SubFile );
		}
		else
			CONSOLE_Print( "[GHOST] couldn't find Scripts\\blizzard.j in MPQ file" );

		SFileCloseArchive( PatchMPQ );
	}
	else
		CONSOLE_Print( "[GHOST] warning - unable to load MPQ file [" + PatchMPQFileName + "] - error code " + UTIL_ToString( GetLastError( ) ) );
}

void CGHost :: LoadIPToCountryData( )
{
	ifstream in;
	in.open( "ip-to-country.csv" );

	if( in.fail( ) )
		CONSOLE_Print( "[GHOST] warning - unable to read file [ip-to-country.csv], iptocountry data not loaded" );
	else
	{
		CONSOLE_Print( "[GHOST] started loading [ip-to-country.csv]" );

		// the begin and commit statements are optimizations
		// we're about to insert ~4 MB of data into the database so if we allow the database to treat each insert as a transaction it will take a LONG time
		// todotodo: handle begin/commit failures a bit more gracefully

		if( !m_DBLocal->Begin( ) )
			CONSOLE_Print( "[GHOST] warning - failed to begin local database transaction, iptocountry data not loaded" );
		else
		{
			unsigned char Percent = 0;
			string Line;
			string IP1;
			string IP2;
			string Country;
			CSVParser parser;

			// get length of file for the progress meter

			in.seekg( 0, ios :: end );
			uint32_t FileLength = in.tellg( );
			in.seekg( 0, ios :: beg );

			while( !in.eof( ) )
			{
				getline( in, Line );

				if( Line.empty( ) )
					continue;

				parser << Line;
				parser >> IP1;
				parser >> IP2;
				parser >> Country;
				m_DBLocal->FromAdd( UTIL_ToUInt32( IP1 ), UTIL_ToUInt32( IP2 ), Country );

				// it's probably going to take awhile to load the iptocountry data (~10 seconds on my 3.2 GHz P4 when using SQLite3)
				// so let's print a progress meter just to keep the user from getting worried

				unsigned char NewPercent = (unsigned char)( (float)in.tellg( ) / FileLength * 100 );

				if( NewPercent != Percent && NewPercent % 10 == 0 )
				{
					Percent = NewPercent;
					CONSOLE_Print( "[GHOST] iptocountry data: " + UTIL_ToString( Percent ) + "% loaded" );
				}
			}

			if( !m_DBLocal->Commit( ) )
				CONSOLE_Print( "[GHOST] warning - failed to commit local database transaction, iptocountry data not loaded" );
			else
				CONSOLE_Print( "[GHOST] finished loading [ip-to-country.csv]" );
		}

		in.close( );
	}
}

void CGHost :: CreateGame( CMap *map, unsigned char gameState, bool saveGame, string gameName, string ownerName, string creatorName, string creatorServer, bool whisper, bool isAutoHostGame, CBNET *bnet, uint32_t autoHostSlotIndex )
{
	if( !m_Enabled )
	{
		if( bnet )
			bnet->QueueChatCommand( m_Language->UnableToCreateGameDisabled( gameName ), creatorName, whisper );

		if( m_AdminGame )
			m_AdminGame->SendAllChat( m_Language->UnableToCreateGameDisabled( gameName ) );

		return;
	}

	if( gameName.size( ) > 31 )
	{
		if( bnet )
			bnet->QueueChatCommand( m_Language->UnableToCreateGameNameTooLong( gameName ), creatorName, whisper );

		if( m_AdminGame )
			m_AdminGame->SendAllChat( m_Language->UnableToCreateGameNameTooLong( gameName ) );

		return;
	}

	if( !map->GetValid( ) )
	{
		if( bnet )
			bnet->QueueChatCommand( m_Language->UnableToCreateGameInvalidMap( gameName ), creatorName, whisper );

		if( m_AdminGame )
			m_AdminGame->SendAllChat( m_Language->UnableToCreateGameInvalidMap( gameName ) );

		return;
	}

	if( saveGame )
	{
		if( !m_SaveGame->GetValid( ) )
		{
			if( bnet )
				bnet->QueueChatCommand( m_Language->UnableToCreateGameInvalidSaveGame( gameName ), creatorName, whisper );

			if( m_AdminGame )
				m_AdminGame->SendAllChat( m_Language->UnableToCreateGameInvalidSaveGame( gameName ) );

			return;
		}

		string MapPath1 = m_SaveGame->GetMapPath( );
		string MapPath2 = map->GetMapPath( );
		transform( MapPath1.begin( ), MapPath1.end( ), MapPath1.begin( ), (int(*)(int))tolower );
		transform( MapPath2.begin( ), MapPath2.end( ), MapPath2.begin( ), (int(*)(int))tolower );

		if( MapPath1 != MapPath2 )
		{
			CONSOLE_Print( "[GHOST] path mismatch, saved game path is [" + MapPath1 + "] but map path is [" + MapPath2 + "]" );

			if( bnet )
				bnet->QueueChatCommand( m_Language->UnableToCreateGameSaveGameMapMismatch( gameName ), creatorName, whisper );

			if( m_AdminGame )
				m_AdminGame->SendAllChat( m_Language->UnableToCreateGameSaveGameMapMismatch( gameName ) );

			return;
		}

		if( m_EnforcePlayers.empty( ) )
		{
			if( bnet )
				bnet->QueueChatCommand( m_Language->UnableToCreateGameMustEnforceFirst( gameName ), creatorName, whisper );

			if( m_AdminGame )
				m_AdminGame->SendAllChat( m_Language->UnableToCreateGameMustEnforceFirst( gameName ) );

			return;
		}
	}

	// allow multiple lobbies - only block if manual lobby exists and this is a manual create
	CBaseGame *ManualLobby = GetManualLobby( );

	if( !isAutoHostGame && ManualLobby && !ManualLobby->GetIsAutoHostGame( ) )
	{
		if( bnet )
			bnet->QueueChatCommand( m_Language->UnableToCreateGameAnotherGameInLobby( gameName, ManualLobby->GetDescription( ) ), creatorName, whisper );

		if( m_AdminGame )
			m_AdminGame->SendAllChat( m_Language->UnableToCreateGameAnotherGameInLobby( gameName, ManualLobby->GetDescription( ) ) );

		return;
	}

	if( m_Games.size( ) >= m_MaxGames )
	{
		if( bnet )
			bnet->QueueChatCommand( m_Language->UnableToCreateGameMaxGamesReached( gameName, UTIL_ToString( m_MaxGames ) ), creatorName, whisper );

		if( m_AdminGame )
			m_AdminGame->SendAllChat( m_Language->UnableToCreateGameMaxGamesReached( gameName, UTIL_ToString( m_MaxGames ) ) );

		return;
	}

	CONSOLE_Print( "[GHOST] creating game [" + gameName + "]" );

	// assign host port
	// for autohost games: static port = m_HostPort + SlotIndex (slot 1 = m_HostPort+1, slot 2 = m_HostPort+2, ...)
	// for manual games:   always m_HostPort (static, reserved for manual use)
	uint16_t GameHostPort;

	if( isAutoHostGame )
		GameHostPort = (uint16_t)( m_HostPort + ( autoHostSlotIndex > 0 ? autoHostSlotIndex : 1 ) );
	else
		GameHostPort = m_HostPort;

	CONSOLE_Print( "[GHOST] assigning host port " + UTIL_ToString( GameHostPort ) + " to game [" + gameName + "]" );

	CBaseGame *NewGame;

	if( saveGame )
		NewGame = new CGame( this, map, m_SaveGame, GameHostPort, gameState, gameName, ownerName, creatorName, creatorServer );
	else
		NewGame = new CGame( this, map, NULL, GameHostPort, gameState, gameName, ownerName, creatorName, creatorServer );

	m_CurrentGames.push_back( NewGame );
	NewGame->SetIsAutoHostGame( isAutoHostGame );

	// todotodo: check if listening failed and report the error to the user

	if( m_SaveGame )
	{
		NewGame->SetEnforcePlayers( m_EnforcePlayers );
		m_EnforcePlayers.clear( );
	}

	// bnet = the BNET connection for both advertising and creator announcements
	// for autohost: passed as slot.BNet; for manual: passed as this (the CBNET handler)
	CBNET *AdvertisedBNet = bnet ? bnet : ( m_BNETs.empty( ) ? NULL : m_BNETs[0] );

	NewGame->SetAdvertisedBNet( AdvertisedBNet );

	// send creation announcement to the creator BNET only (no broadcast to other BNETs)
	if( bnet && !isAutoHostGame )
	{
		string AnnounceMsg = ( gameState == GAME_PRIVATE ) ?
			m_Language->CreatingPrivateGame( gameName, ownerName ) :
			m_Language->CreatingPublicGame( gameName, ownerName );

		if( whisper )
			bnet->QueueChatCommand( AnnounceMsg, creatorName, true );
		else
			bnet->QueueChatCommand( AnnounceMsg );
	}

	// advertise game on the assigned BNET only
	// QueueEnterChat before GameCreate allows PVPGN to process GameCreate command
	if ( AdvertisedBNet )
	{
		AdvertisedBNet->QueueEnterChat();

		if (saveGame)
			AdvertisedBNet->QueueGameCreate( gameState, gameName, string( ), map, m_SaveGame, NewGame->GetHostCounter( ) );
		else
			AdvertisedBNet->QueueGameCreate( gameState, gameName, string( ), map, NULL, NewGame->GetHostCounter( ) );
	}

	if( m_AdminGame )
	{
		if( gameState == GAME_PRIVATE )
			m_AdminGame->SendAllChat( m_Language->CreatingPrivateGame( gameName, ownerName ) );
		else if( gameState == GAME_PUBLIC )
			m_AdminGame->SendAllChat( m_Language->CreatingPublicGame( gameName, ownerName ) );
	}

	// private games: rejoin chat on the advertising BNET (except pvpgn)
	if( gameState == GAME_PRIVATE && AdvertisedBNet && AdvertisedBNet->GetPasswordHashType( ) != "pvpgn" )
		AdvertisedBNet->QueueEnterChat( );

	// hold friends and/or clan members on the advertising BNET only
	if( AdvertisedBNet )
	{
		if( AdvertisedBNet->GetHoldFriends( ) )
			AdvertisedBNet->HoldFriends( NewGame );

		if( AdvertisedBNet->GetHoldClan( ) )
			AdvertisedBNet->HoldClan( NewGame );
	}
}
