/********************************************************************************

	GnucDNA - The Gnucleus Library
	Copyright (C) 2000-2005 John Marshall Group

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

	By contributing code you grant John Marshall Group an unlimited, non-exclusive
	license to your contribution.

	For support, questions, commercial use, etc...
	E-Mail: swabby@c0re.net

********************************************************************************/

// GET /gnutella/tigertree/v3?urn:tree:tiger/: HTTP/1.1

#include "stdafx.h"
#include "GnuCore.h"
#include "GnuPrefs.h"

#include "GnuNetworks.h"
#include "GnuCache.h"
#include "GnuRouting.h"
#include "GnuControl.h"
#include "G2Control.h"
#include "GnuTransfers.h"
#include "GnuProtocol.h"

#include "GnuShare.h"
#include "GnuFileHash.h"

#include "DnaCore.h"
#include "DnaEvents.h"

#include "hash/TigerTree2.h"
#include "Dime.h"

#include "GnuDownloadShell.h"
#include "GnuDownload.h"


CGnuDownload::CGnuDownload(CGnuDownloadShell* pShell, int HostID)
{
	m_pShell = pShell;
	m_pNet   = pShell->m_pNet;
	m_pTrans = pShell->m_pTrans;
	m_pPrefs = pShell->m_pPrefs;

	m_pShell->m_Queue[HostID].HasDownload = true;

	m_HostID	= HostID;
	m_Status	= TRANSFER_PENDING;
	m_Push		= false;
	m_PushF2F   = false;
	
	m_PartActive  = false;
	m_PartNumber  = 0;

	m_StartPos    = 0;
	m_PausePos    = 0;

	m_DoHead		   = false;
	m_HeadNotSupported = false;

	m_KeepAlive    = false;
	m_SendAltF2Fs  = false;

	m_TigerRequest  = false;
	m_TigerLength   = 0;
	m_TigerPos      = 0;
	m_TigerReqBuffer = NULL;
	m_tempTigerTree = NULL;
	m_tempTreeSize  = 0;
	m_tempTreeRes   = 0;

	m_QueuePos	  = 0;
	m_QueueLength = 0;
	m_QueueLimit  = 0;
	m_RetryMin    = 0;
	m_RetryMax    = 0;

	m_nSecsUnderLimit = 0;
	m_nSecsDead       = 0;

	// Bandwidth
	m_AvgRecvBytes.SetSize(30);

	//m_BytesRecvd = 0;

	// Proxy
	m_PushProxy = false;

	// sockets
	m_pSocket = new CReliableSocket(this);
}

CGnuDownload::~CGnuDownload()
{
	m_Status = TRANSFER_CLOSED;

	m_pShell->ConnectionDeleted(m_HostID);

	if(HostInfo()->Error == "")
		HostInfo()->Error = "Deleted";

	// Flush receive buffer
	byte pBuff[4096];
	while(m_pSocket->Receive(pBuff, 4096) > 0)
		;

	m_pSocket->Close();

	if(m_TigerReqBuffer)
		delete [] m_TigerReqBuffer;

	if(m_tempTigerTree)
		delete [] m_tempTigerTree;

	delete m_pSocket;
}


// Do not edit the following lines, which are needed by ClassWizard.
#if 0
BEGIN_MESSAGE_MAP(CGnuDownload, CAsyncSocketEx)
	//{{AFX_MSG_MAP(CGnuDownload)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()
#endif	// 0


void CGnuDownload::OnAccept(int nErrorCode)
{
	// only for udp connections is this called

	if(nErrorCode)
	{
		SetError("OnAccept Error " + NumtoStr(nErrorCode) );	
		m_pShell->SetHostState(m_HostID, FileSource::eFailed);
		
		SendPushRequest();
		
		Close();
		
		return;
	}

	HostInfo()->Tries = 0; // reset, know host can be connected to

	SetError("Accepted");

	StatusUpdate(TRANSFER_CONNECTED);

	m_Push = true;
	// wait for GIV
}
void CGnuDownload::OnConnect(int nErrorCode) 
{
	if(nErrorCode)
	{
		SetError("OnConnect Error " + NumtoStr(nErrorCode) );	
		m_pShell->SetHostState(m_HostID, FileSource::eFailed);
		
		SendPushRequest();
		
		Close();
		
		return;
	}

	HostInfo()->Tries = 0; // reset, know host can be connected to

	SetError("Connected");

	SendRequest();
}

void CGnuDownload::OnReceive(int nErrorCode) 
{
	if(nErrorCode)
	{
		SetError("OnReceive Error " + NumtoStr(nErrorCode));	

		m_pShell->SetHostState(m_HostID, FileSource::eFailed);
		Close();
		return;
	}


	int BuffLength = 0;

	CGnuShare* m_pShare = m_pNet->m_pCore->m_pShare;


	// Connected and downloading
	if(m_Status == TRANSFER_RECEIVING)
	{
		int BuffSize = RECEIVE_BUFF;

		if(m_pPrefs->m_BandwidthDown)
			if(m_pShell->m_AllocBytes > 0)
			{
				if(m_pShell->m_AllocBytes > RECEIVE_BUFF)
					BuffSize = RECEIVE_BUFF;
				else
					BuffSize = m_pShell->m_AllocBytes;

				if(BuffSize == 0)
					return;
			}
			else
				return;


		BuffLength = m_pSocket->Receive(m_pBuff, BuffSize);


		// Check for errors
		switch (BuffLength)
		{
		case 0:
			return;
			break;
		case SOCKET_ERROR:
			int nError = GetLastError();
			if(WSAEWOULDBLOCK != nError)
			{
				SetError("Error Receiving");
				Close();
				return;
			}
			return;
			break;
		}

		if(m_pShell->m_AllocBytes > 0)
			m_pShell->m_AllocBytes -= BuffLength;
		else
			m_pShell->m_AllocBytes = 0;


		// Download pre-write integrity check
		/*if(m_pShell->m_CheckFile.m_hFile != CFile::hFileNull)
		{
			FilePart CurrentPart = m_pShell->m_PartList[m_PartNumber];
			int FilePos = CurrentPart.StartByte + CurrentPart.BytesCompleted;

			byte* CheckBytes = new byte[BuffLength];
			
			m_pShell->m_CheckFile.Seek(FilePos, CFile::begin);
			m_pShell->m_CheckFile.Read(CheckBytes, BuffLength);

			if( memcmp(m_pBuff, CheckBytes, BuffLength) != 0 )
			{
				CString Problem = "Write Check Failed: " + m_pShell->m_Name *//*IPtoStr(HostInfo()->Host) + " (" + m_ServerName + ")*//* + " -- StartByte=" + NumtoStr(CurrentPart.StartByte) + ", CurrentPos=" + NumtoStr(CurrentPart.StartByte + CurrentPart.BytesCompleted) + ", RecvBytes=" + NumtoStr(BuffLength);
				m_pShell->m_pTrans->m_pCore->DebugLog(Problem);
			}
			
			delete [] CheckBytes;
			CheckBytes = NULL;
		}*/


		DownloadBytes(m_pBuff, BuffLength);
	}


	// Server sending us download header
	else if(m_Status == TRANSFER_CONNECTED || m_Status == TRANSFER_QUEUED)
	{
	
		// Receive Data
		int BuffLength = m_pSocket->Receive(m_pBuff, RECEIVE_BUFF);
		int i = 0;

		// Check for errors
		switch (BuffLength)
		{
		case 0:
			SetError("No Data");
			Close();
			return;
			break;
		case SOCKET_ERROR:
			SetError("Error Receiving");
			Close();
			return;
			break;
		}


		int FileBegin = -1;

		
		// Read buffer
		for(i = 0; i < BuffLength; i++)
		{
			m_Header += m_pBuff[i];

			if( m_Header.Find("\r\n\r\n") != -1)
			{
				// Detect / Repair 0.9.1.5 header flaw
				char* xFilename = "X-Filename";

				if( BuffLength > i + 11 && memcmp( &m_pBuff[i + 1], xFilename, 10) == 0)
				{
					m_Header.Replace("\r\n\r\n", "\r\n");
					continue;
				}
				//////////
			
				FileBegin = i + 1;
				break;
			}

			else if(m_Header.GetLength() > 16384)
			{
				SetError("Handshake Error - Overflow");
				m_pShell->SetHostState(m_HostID, FileSource::eFailed);
				Close();
				return;
			}		
		}


		// If the entire header is here
		if(m_Header.Find("\r\n\r\n") != -1 || (m_Header.Find("GIV ") == 0 && m_Header.Find("\n\n") != -1 ))
		{
			HostInfo()->Handshake += m_Header;
			HostInfo()->Handshake += "\r\n";

			// Handle Push
			if (m_Header.Left(3) == "GIV")
			{
				// Get Server ID of client giving us the file
				int Front   = m_Header.Find(":") + 1;
				int End     = m_Header.Find("/");
				CString RemoteGuid  = m_Header.Mid(Front, End - Front);
				RemoteGuid.MakeUpper();

				if( EncodeBase16((byte*) &HostInfo()->PushID, 16) != RemoteGuid)
				{
					SetError("Push Error - Wrong Server ID");
					m_pShell->SetHostState(m_HostID, FileSource::eFailed);
					Close();
					return;
				}

				HostInfo()->PushSent  = false;
				m_Header = "";

				SendRequest();

				return;
			}


			m_Header = m_Header.Left( m_Header.Find("\r\n\r\n") + 4);

			// Verify HTTP header
			CString StatusLine = m_Header.Left(m_Header.Find("\r\n"));
			m_Header = m_Header.Mid(StatusLine.GetLength() + 2);

			int     Code = 0;
			CString httpVersion;
			CString ReasonPhrase;
			
			int spacepos = StatusLine.Find(' ');
			if (spacepos != -1)
			{
				httpVersion = StatusLine.Left(spacepos);
				httpVersion.MakeUpper();
				
				StatusLine = StatusLine.Mid(spacepos + 1);
				
				if (httpVersion.Left(4) == "HTTP")
				{
					// clients dont send connection header, but still stay alive
					if( httpVersion.Left(8) == "HTTP/1.1" )   
						m_KeepAlive = true; 

					//Read the Status-Code
					Code = atoi(StatusLine.Left(3));
					if (StatusLine.Mid(3, 1) == ' ')
					{
						ReasonPhrase = StatusLine.Mid(4);
					}
				}
				else
				{
					//Status-Line does not start with HTTP
					SetError("Handshake Error - No HTTP");
					m_pShell->SetHostState(m_HostID, FileSource::eFailed);
					Close();
					return;	
				}
			}
			else
			{
				//No space in Status-Line
				SetError("Handshake Error - No Space");
				m_pShell->SetHostState(m_HostID, FileSource::eFailed);
				Close();
				return;
			}
	

			// Response from push proxy server
			if(m_PushProxy)
			{
				// If good, add as alt source
				if(200 <= Code && Code < 300)
				{
					SetError("Push Sent through Proxy");

					HostInfo()->DirectHubs.push_back(m_ProxyAddress);
				}

				// If bad erase
				else
				{
					SetError("Proxy not connected to Host");	
					SendPushRequest();
				}
				
				Close();
				return;
			}


			CParsedHeaders ParsedHeaders (m_Header);


			// Get Correct Tiger uri

			// old dna/raza X-TigerTree-Path: ...
			// raza X-Thex-URI: /gnutella/thex/v1?urn:tree:tiger/:C2GKRKXQYZR3C2SCUGSMBRT7VOILXTGRS5A5GLY&depth=9&ed2k=0
			// lime X-Thex-URI: /uri-res/N2X?urn:sha1:YWSC2W6KZ3ZXDM2242CDJ7S5QECLBU22;VZZE5DTQ3YLA2CTP3BJ4SCQOKU7TWDYXPTFFLGQ

			CString TigerPath = ParsedHeaders.FindHeader("X-TigerTree-Path");
	
			if( TigerPath.IsEmpty() )
			{
				TigerPath = ParsedHeaders.FindHeader("X-Thex-URI");

				if( !TigerPath.IsEmpty() )
					HostInfo()->TigerUseThex = true;
			}


			if( !TigerPath.IsEmpty() )
			{
				CString TigerURI = TigerPath;
				TigerURI.MakeLower();

				TigerPath = ParseString(TigerPath, ';');
				TigerPath = ParseString(TigerPath, '&');
				HostInfo()->TigerPath = TigerPath;
				

				CString TigerHash;

				if(TigerURI.Left(39) == "/gnutella/tigertree/v3?urn:tree:tiger/:")
					TigerHash = TigerURI.Mid(39);

				if(TigerURI.Left(34) == "/gnutella/thex/v1?urn:tree:tiger/:") // thanks shareaza
					TigerHash = TigerURI.Mid(34); // case also has depth info
				
				if(TigerURI.Left(13) == "/uri-res/n2x?") // thanks limewire
				{
					int semipos = TigerURI.Find(";");
					if(semipos != -1)
						TigerHash   = TigerURI.Mid(semipos + 1);
				}

				// trim possible amperstand
				TigerHash = ParseString(TigerHash, '&');
					

				if( !TigerHash.IsEmpty() )
				{
					TigerHash.MakeUpper();

					HostInfo()->TigerSupport = true;
					
					if(m_pShell->m_TigerHash.IsEmpty())
						m_pShell->m_TigerHash = TigerHash;
					else if(m_pShell->m_TigerHash != TigerHash)
					{
						SetError("Incorrect Tiger Hash");
						HostInfo()->TigerSupport = false;
						m_pShell->SetHostState(m_HostID, FileSource::eFailed);
						Close();
						return;
					}

				}
			}

			// Get Hash and add host as an alt-loc
			CString Sha1URN = ParsedHeaders.FindHeader("X-Gnutella-Content-URN");

			if( Sha1URN.IsEmpty() )
				 Sha1URN = ParsedHeaders.FindHeader("Content-URN");

			if( Sha1URN.IsEmpty() )
				 Sha1URN = ParsedHeaders.FindHeader("X-Content-URN");


			Sha1URN.MakeLower();
			CString RemoteFileHash;

			if(Sha1URN.Left(9) == "urn:sha1:")
			{
				RemoteFileHash = Sha1URN.Mid(9);
				RemoteFileHash.MakeUpper();

				// Make sure hash is what we're expecting
				if(!m_pShell->m_Sha1Hash.IsEmpty() && RemoteFileHash != m_pShell->m_Sha1Hash)
				{
					SetError("Incorrect Sha1 Hash");
					m_pShell->SetHostState(m_HostID, FileSource::eFailed);
					Close();
					return;
				}

				// Make sure hash is of proper length
				else if (RemoteFileHash.GetLength() != 32)
				{
					RemoteFileHash = "";
				}

				// We're connected, server has file hash, add as alt loc
				if ( !RemoteFileHash.IsEmpty() )
				{
					if( !m_Push )
						m_pShell->AddAltLocation(HostInfo()->Address);	
					//else if(m_pSocket->m_RudpMode && m_SendAltF2Fs)
					//	m_pShell->AddF2FLocation(HostInfo()->Address, HostInfo()->DirectHubs, HostInfo()->PushID);	
				}
			}


			//Look for X-Available-Ranges header
			CString RemoteRanges = ParsedHeaders.FindHeader("X-Available-Ranges");
	
			HostInfo()->AvailableRanges.clear();

			if (!RemoteRanges.IsEmpty())
			{
				RemoteRanges.MakeLower();
				if (RemoteRanges.Left(6) == "bytes ")
				{
					CString AvailableRanges = RemoteRanges.Mid(6);

					while(!AvailableRanges.IsEmpty())
					{
						CString   tmpRange = ParseString(AvailableRanges);
						RangeType NewRange;

						int dashpos = tmpRange.Find("-");
						if (dashpos != -1 && dashpos != 0 && dashpos < tmpRange.GetLength() - 1)
						{
							NewRange.StartByte = _atoi64(tmpRange.Left(dashpos));
							NewRange.EndByte   = _atoi64(tmpRange.Mid(dashpos + 1));
						}

						HostInfo()->AvailableRanges.push_back(NewRange);
					}
				}
			}

			// Connection Header
			CString ConnectType = ParsedHeaders.FindHeader("Connection");
			if( !ConnectType.IsEmpty() )
			{
				ConnectType.MakeLower();

				if(ConnectType == "keep-alive")
					m_KeepAlive = true;
				
				if(ConnectType == "close")
					m_KeepAlive = false;
			}

			// Server Name
			CString ServerName = ParsedHeaders.FindHeader("Server");
			if( !ServerName.IsEmpty() )
			{
				m_ServerName       = ServerName;
				HostInfo()->Vendor = ServerName;
			}

			// User Agent
			CString UserAgent = ParsedHeaders.FindHeader("User-Agent");
			if( !UserAgent.IsEmpty() )
			{
				m_ServerName       = UserAgent;
				HostInfo()->Vendor = UserAgent;
			}

			// Features
			CString Features = ParsedHeaders.FindHeader("X-Features");
			while( !Features.IsEmpty() )
			{
				CString Feature = ParseString(Features, ',');

				if( Feature.CompareNoCase("fwt/1.0") == 0)
					m_SendAltF2Fs = true;
			}


			// Alt locationsB
			for (i = 0; i < ParsedHeaders.m_Headers.size(); i++)
			{
				CString HeaderName  = ParsedHeaders.m_Headers[i].Name;
				CString HeaderValue = ParsedHeaders.m_Headers[i].Value;

				HeaderName.MakeLower();
				
				
				// Alt-Location (old format)
				if (HeaderName == "alt-location" || HeaderName == "x-gnutella-alternate-location")
				{
					AltLocation OldFormat = HeaderValue;

					IPv4 Address;
					Address.Host = StrtoIP(OldFormat.HostPort.Host);
					Address.Port = OldFormat.HostPort.Port;

					AddHosttoMesh(Address);
				}
			
				// X-Alt
				else if (HeaderName == "x-alt")
				{
					// Parse headers breaks up commas
					AddHosttoMesh( AltLoctoAddress(HeaderValue) );
				}

				// X-Push-Proxy
				else if (HeaderName == "x-push-proxy" && HostInfo()->Network == NETWORK_GNUTELLA)
				{
					// Parse headers breaks up commas
					IPv4 PushProxy = StrtoIPv4( HeaderValue );

					if(PushProxy.Port == 0)
						PushProxy.Port = 6346;
						
					bool found = false;
					for(int i = 0; i < HostInfo()->DirectHubs.size(); i++)
						if(HostInfo()->DirectHubs[i].Host.S_addr == PushProxy.Host.S_addr)
						{
							HostInfo()->DirectHubs[i] = PushProxy;
							found = true;
						}

					if(!found)
						HostInfo()->DirectHubs.push_back(PushProxy);
				}
			}

			// Success code
			if( (200 <= Code && Code < 300) || Code == 302 )
			{		
				// Location header (redirected)
				CString Location = ParsedHeaders.FindHeader("Location");
				if( !Location.IsEmpty() )
				{
					FileSource WebSource; 
					m_pShell->URLtoSource(WebSource, Location);

					m_pShell->AddHost( WebSource );

					SetError("Redirected");
					Close();
					return;
				}

				// Authorization
				CString AuthChallenge = ParsedHeaders.FindHeader("X-Auth-Challenge");
				if( !AuthChallenge.IsEmpty() )
				{
					m_RemoteChallenge = AuthChallenge;
					if(m_pNet->m_pCore->m_dnaCore->m_dnaEvents)
						m_pNet->m_pCore->m_dnaCore->m_dnaEvents->DownloadChallenge(m_pShell->m_DownloadID, m_HostID, m_RemoteChallenge);
				
					// Send Answer to Challenge
					if( !m_RemoteChallengeAnswer.IsEmpty() )
					{
						CString AuthResponse = "AUTH\r\n";
						AuthResponse += "X-Auth-Response: " + m_RemoteChallengeAnswer + "\r\n";
						AuthResponse += "\r\n";
						m_pSocket->Send(AuthResponse, AuthResponse.GetLength());
					}
				}

				
				// Dont download from undeclared hosts
				if( m_ServerName.IsEmpty() )
				{
					SetError("Server Not Specified");
					m_pShell->SetHostState(m_HostID, FileSource::eFailed);
					Close();
					return;
				}


				uint64 ContentLength = 0, RemoteFileSize = 0, StartByte = 0, EndByte = 0;

				// Conent-Length
				CString strContentLength = ParsedHeaders.FindHeader("Content-Length");
				if( !strContentLength.IsEmpty() )
				{
					ContentLength = _atoi64(strContentLength);

					if(m_pShell->m_FileLength == 0)
					{
						m_pShell->m_FileLength = ContentLength;
						m_pShell->CreatePartList();	
						
						if(m_KeepAlive)
							SendRequest();
						else
						{
							Close();
							HostInfo()->RetryWait = 0;
							m_pShell->SetHostState(m_HostID, FileSource::eAlive);
						}

						return;
					}
				}

				// Content-Range
				CString ContentRange = ParsedHeaders.FindHeader("Content-Range");
				if( !ContentRange.IsEmpty() )
				{
					ContentRange.MakeLower();

					ContentRange.Replace("bytes=", "bytes ");	//Some wrongly send bytes=x-y/z in Content-Range header
					
					if(ContentRange.Find("*") != -1)
					{
						sscanf(ContentRange, "bytes */%I64u", &RemoteFileSize);
						StartByte = m_PausePos - ContentLength;
						EndByte   = m_PausePos - 1;
					}
					else
						sscanf(ContentRange, "bytes %I64u-%I64u/%I64u", &StartByte, &EndByte, &RemoteFileSize);
					
					// Usually DownloadFile with unknown size
					if( m_pShell->m_FileLength == 0 && RemoteFileSize)
					{
						m_pShell->m_FileLength = RemoteFileSize;
						m_pShell->CreatePartList();	
						
						if(m_KeepAlive)
							SendRequest();
						else
						{
							Close();
							HostInfo()->RetryWait = 0;
							m_pShell->SetHostState(m_HostID, FileSource::eAlive);
						}

						return;
					}	

					if (EndByte > m_StartPos && EndByte < m_PausePos - 1)
						m_PausePos = EndByte + 1;
				}

				// If not same
				if(StartByte != m_StartPos)
				{
					SetError("Start byte different");
					Close();
					return;
				}


				// If Tiger Tree Transfer
				if( m_TigerRequest )
				{
					if(m_TigerReqBuffer)
						delete [] m_TigerReqBuffer;

					m_TigerLength    = ContentLength;
					m_TigerReqBuffer = new byte[m_TigerLength];
					

					StatusUpdate(TRANSFER_RECEIVING);
					SetError("Receiving TigerTree");

					HostInfo()->RetryWait = 0;

					if(FileBegin != -1 && BuffLength - FileBegin > 0)
						DownloadBytes(&m_pBuff[FileBegin], BuffLength - FileBegin);
				}

				// Else Normal File Transfer
				else if(RemoteFileSize == m_pShell->m_FileLength || ContentLength == m_PausePos)
				{
					//If head req, or tiny request used when head is not supported
					if (m_DoHead || (m_PausePos == 1 && m_HeadNotSupported))	
					{
						SetError("Head Request");
						Close();	//Close after responce to head request for now
						return;
					}

					if( m_pShell->PartDone(m_PartNumber) )
					{
						SetError("File Part Done");
						Close();	
						return;
					}

					if( m_pShell->PartActive(m_PartNumber) )
					{
						SetError("File Part Busy");
						Close();	
						return;
					}

					if( m_pShell->ReadyFile() )
					{
						StatusUpdate(TRANSFER_RECEIVING);
						SetError("Receiving");

						HostInfo()->RetryWait = 0;
						m_PartActive = true;
						m_pShell->m_PartList[m_PartNumber].SourceHostID = m_HostID;

						if(FileBegin != -1 && BuffLength - FileBegin > 0)
							DownloadBytes(&m_pBuff[FileBegin], BuffLength - FileBegin);
					}
					else
					{
						SetError("Failed to ready file");
						Close();
						return;
					}

				}
				else
				{
					SetError("Remote File Size Different");
					m_pShell->SetHostState(m_HostID, FileSource::eFailed);
					Close();
					return;
				}
			}

			//Temporary error.
			//408 = Request Timeout
			//416 = Requested Range Not Satisfiable. (Someone might do partial file sharing wrong)
			else if((500 <= Code && Code < 600) || Code == 408 || Code == 416)
			{
				bool QueueSupport   = false;
				bool RemotelyQueued = false;

				for (i = 0; i < ParsedHeaders.m_Headers.size(); i++)
				{
					CString HeaderName = ParsedHeaders.m_Headers[i].Name;
					HeaderName.MakeLower();
					CString HeaderValue = ParsedHeaders.m_Headers[i].Value;
					
					// Alt-Location (old format)
					if (HeaderName == "alt-location" || HeaderName == "x-gnutella-alternate-location")
					{
						AltLocation OldFormat = HeaderValue;

						IPv4 Address;
						Address.Host = StrtoIP(OldFormat.HostPort.Host);
						Address.Port = OldFormat.HostPort.Port;

						AddHosttoMesh(Address);
					}
				
					// X-Alt
					else if (HeaderName == "x-alt")
					{
						// Parse headers breaks up commas
						AddHosttoMesh( AltLoctoAddress(HeaderValue) );
					}

					// X-Push-Proxy
					else if (HeaderName == "x-push-proxy" && HostInfo()->Network == NETWORK_GNUTELLA)
					{
						HostInfo()->DirectHubs.clear();

						while( !HeaderValue.IsEmpty() )
						{
							IPv4 PushProxy = StrtoIPv4( ParseString(HeaderValue, ',') );

							if(PushProxy.Port)
								HostInfo()->DirectHubs.push_back(PushProxy);
						}

					}

					// X-Queue
					else if (HeaderName.Right(7) == "x-queue")
					{
						QueueSupport   = true;
						RemotelyQueued = true;

						if(HeaderValue.Find("position=full") != -1 || ReasonPhrase.Right(10).CompareNoCase("queue full") == 0)
							RemotelyQueued = false;

						HeaderValue.MakeLower();
		
						int SplitPos = HeaderValue.Find("=");
						if(SplitPos != -1)
						{
							if(HeaderValue.Left(SplitPos).CompareNoCase("position") == 0)
								m_QueuePos = atoi( HeaderValue.Mid(SplitPos + 1) );

							if(HeaderValue.Left(SplitPos).CompareNoCase("length") == 0)
								m_QueueLength = atoi( HeaderValue.Mid(SplitPos + 1) );

							if(HeaderValue.Left(SplitPos).CompareNoCase("limit") == 0)
								m_QueueLimit = atoi( HeaderValue.Mid(SplitPos + 1) );

							if(HeaderValue.Left(SplitPos).CompareNoCase("pollmin") == 0)
								m_RetryMin = atoi( HeaderValue.Mid(SplitPos + 1) );

							if(HeaderValue.Left(SplitPos).CompareNoCase("pollmax") == 0)
								m_RetryMax = atoi( HeaderValue.Mid(SplitPos + 1) );
						}
						
					}

					// 'PARQ' nodes use retry-after
					else if (HeaderName.Right(11) == "retry-after")
					{ 
						m_RetryMin = atoi(HeaderValue);

						if (m_RetryMin == 0) // various conditions. already downloading, too many downloads, etc.
							m_RetryMin = 1200;

						m_RetryMax = m_RetryMin + 60;
					}
				}

				if(QueueSupport)
				{
					if(RemotelyQueued)
					{
						SetError("Number " + NumtoStr(m_QueuePos) + " in Queue of " + NumtoStr(m_QueueLength));
						StatusUpdate(TRANSFER_QUEUED);
						TRACE0("Q> Remotely queued as#" +  NumtoStr(m_QueuePos) + ". Retrying in " + NumtoStr(m_RetryMin) + ". File:" + HostInfo()->Name + " IP:" + IPtoStr(HostInfo()->Address.Host) + ".\n");	//TODO: Tor: Remove (QueueTrace)
					}
					else
					{
						SetError("Remote Queue Full");
						Close();
					}
				}
				else
				{
					SetError("Server Busy");
					Close();
				}
			}

			// Server error code
			else if(400 <= Code && Code < 500)
			{
				if (m_DoHead && Code != 404)	//Server probably doesnt support head if m_DoHead and Code is not 404 Not found
				{
					m_HeadNotSupported = true;
					SetError("No head support. Retrying without head.");
				}
				else
				{
					SetError("File Not Found");
					m_pShell->SetHostState(m_HostID, FileSource::eFailed);
				}

				Close();
			}
	
			else
			{
				SetError("Server Error - " + NumtoStr(Code));
				m_pShell->SetHostState(m_HostID, FileSource::eFailed);
				Close();
			}
		}

		// else, still waiting for rest of header
	}

	// We are not in a connected or receiving state
	else
	{
		//SetError("Not Ready to Receive");
		Close();		
	}
}

void CGnuDownload::OnClose(int nErrorCode) 
{
	HostInfo()->Error = "Remotely Closed";

	Close();
}

void CGnuDownload::Close()
{
	m_PartActive = false;
	m_PartNumber   = 0;

	m_pShell->CheckCompletion();

	HostInfo()->RealBytesPerSec = m_AvgRecvBytes.GetAverage();

	if(HostInfo()->Error == "")
		HostInfo()->Error = "Closed";
	
	if(HostInfo()->Status == FileSource::eFailed)
		m_pShell->AddNaltLocation(HostInfo()->Address);// add to nalt list

	m_pSocket->Close();

	StatusUpdate(TRANSFER_CLOSED);
}

 bool CGnuDownload::GetStartPos()
{
	m_PartActive = false;
	m_PartNumber = 0;

	bool PartSet = false;

	CGnuDownload* pSlowest = NULL;
	int SlowestSpeed = m_AvgRecvBytes.GetAverage();

	// Find part thats not active to try
	for(int i = 0; i < m_pShell->m_PartList.size(); i++)
	{
		if( m_pShell->PartDone(i) )
			continue;

		CGnuDownload* pDown = m_pShell->PartActive(i);
		if( pDown )
		{
			if(pDown->m_AvgRecvBytes.GetAverage() < SlowestSpeed)
			{
				SlowestSpeed = pDown->m_AvgRecvBytes.GetAverage();
				pSlowest     = pDown;
			}

			continue;
		}

		FilePart EvalPart = m_pShell->m_PartList[i];
		if( !ByteIsInRanges( EvalPart.StartByte + EvalPart.BytesCompleted) )
			continue;

		m_PartNumber = i;
		PartSet = true;
		break;
	}

	// If no part set
	if( !PartSet )
	{
		// Replace slowest host
		if(pSlowest)
		{
			FilePart EvalPart = m_pShell->m_PartList[pSlowest->m_PartNumber];
			if( !ByteIsInRanges( EvalPart.StartByte + EvalPart.BytesCompleted) )
				return false;

			m_PartNumber = pSlowest->m_PartNumber;
			PartSet = true;

			pSlowest->SetError("Replaced with faster host");
			pSlowest->Close();
		}
		else
			return false;
	}

	// Set download to work with part
	FilePart CurrentPart = m_pShell->m_PartList[m_PartNumber];
	
	m_StartPos    = CurrentPart.StartByte + CurrentPart.BytesCompleted;
	m_PausePos	  = CurrentPart.EndByte + 1;

	return true;
}

bool CGnuDownload::ByteIsInRanges(uint64 StartByte )
{
	FileSource* pParam = HostInfo();

	// All ranges available
	if(pParam->AvailableRanges.empty())
		return true;

	// Check if ByteNo is inside available ranges
	for (int i = 0; i < pParam->AvailableRanges.size(); i++)
	{
		if (StartByte >= pParam->AvailableRanges[i].StartByte && StartByte <= pParam->AvailableRanges[i].EndByte)
			return true;
	}

	return false;
}


void CGnuDownload::SendRequest()
{
	HostInfo()->Handshake = "";
	m_Header = "";

	m_DoHead = false;	//For GetStartPosPartial (GetStartPos would set this to true)
	m_TigerRequest = false;

	if(m_PushProxy)
	{
		SendPushProxyRequest();
		return;
	}

	// Check if host marked as corrupt
	if( HostInfo()->Status == FileSource::eCorrupt )
	{
		Close();
		return;
	}

	// Server is alive
	m_pShell->SetHostState(m_HostID, FileSource::eAlive);


	// Send TigerTree request if available
	if( HostInfo()->TigerSupport && m_pShell->m_TigerTree == NULL && m_pShell->m_FileLength)
	{
		bool RequestPending = false;

		for(int i = 0; i < m_pShell->m_Sockets.size(); i++)
			if( m_pShell->m_Sockets[i]->m_TigerRequest )
			{
				RequestPending = true;
				break;
			}

		if( !RequestPending )
		{
			SendTigerRequest();
			return;
		}
	}
	

	// Get file start pos
	if( !GetStartPos() )
	{
		if( HostInfo()->AvailableRanges.size() || m_pShell->m_FileLength == 0)
			m_DoHead = true;
		else
		{
			SetError("Could not find a start pos");
			Close();
			return;
		}
	}
	

	CString GetFile;

	if( HostInfo()->Network == NETWORK_WEB )
	{
		char buf[1024];
		DWORD len = sizeof buf;
		if (InternetCanonicalizeUrl(HostInfo()->Path, buf, &len, ICU_BROWSER_MODE))
            GetFile.Format("%s HTTP/1.1\r\n", buf);
		else
			GetFile.Format("%s HTTP/1.1\r\n", HostInfo()->Path);
	}
	else
	{
		// If no name or index, request by hash
		if( !m_pShell->m_Sha1Hash.IsEmpty() ) // && m_pShell->m_Name == "" || HostInfo()->FileIndex == 0))
			GetFile.Format("/uri-res/N2R?urn:sha1:%s HTTP/1.1\r\n", m_pShell->m_Sha1Hash);
		else
			GetFile.Format("/get/%ld/%s HTTP/1.1\r\n", HostInfo()->FileIndex, (LPCTSTR) HostInfo()->Name);
	}

	CString Host = HostInfo()->HostStr;
	if( HostInfo()->HostStr.IsEmpty() )
		Host = IPv4toStr(HostInfo()->Address); 
		
	if( m_pShell->m_UseProxy )
		GetFile = "http://" + Host + GetFile;

	// Head or get request
	if (!m_DoHead)
		GetFile = "GET " + GetFile;
	else
		GetFile = "HEAD " + GetFile;


	// Host header
	GetFile += "Host: " + Host + "\r\n";  

	// User-agent header
	GetFile += "User-Agent: " + m_pNet->m_pCore->GetUserAgent() + "\r\n";

	// Listen-IP header   Listen-IP: 0.0.0.0:634
	GetFile += "Listen-IP: " +  IPtoStr(m_pNet->m_CurrentIP) + ":" + NumtoStr(m_pNet->m_CurrentPort) + "\r\n";
	
	// Connection header
	GetFile += "Connection: Keep-Alive\r\n";
	
	// Proxy-Connection header
	if( m_pShell->m_UseProxy && HostInfo()->Network != NETWORK_WEB )
		GetFile += "Proxy-Connection: close\r\n";
	
	// Range header
	if( !m_DoHead && m_PausePos)
		GetFile	+= "Range: bytes=" + NumtoStr(m_StartPos) + "-" + NumtoStr(m_PausePos - 1) + "\r\n";

	if( HostInfo()->Network != NETWORK_WEB )
	{
		// X-Queue
		GetFile	+= "X-Queue: 0.1\r\n";

		// X-Features
		std::vector<CString> Features;
		Features.push_back("g2/1.0");
		
		//crit - not yet
		/*if( !m_pNet->m_TcpFirewall || m_pNet->m_UdpFirewall != UDP_BLOCK)
		{
			Features.push_back("fwalt/0.1");
		
			if(m_pNet->m_TcpFirewall)
				Features.push_back("fwt/1.0");
		}*/
	
		CString FeatureList;
		for(int i = 0; i < Features.size(); i++)
			FeatureList += Features[i] + ",";

		GetFile += "X-Features: " + FeatureList.TrimRight(",") + "\r\n";
	}


	// X-Gnutella-Content-URN
	if(!m_pShell->m_Sha1Hash.IsEmpty() && HostInfo()->Network != NETWORK_WEB)
	{
		if( HostInfo()->Network == NETWORK_GNUTELLA )
			GetFile	+= "X-Gnutella-Content-URN: urn:sha1:" + m_pShell->m_Sha1Hash + "\r\n";
		else if( HostInfo()->Network == NETWORK_G2 )
			GetFile	+= "Content-URN: urn:sha1:" + m_pShell->m_Sha1Hash + "\r\n";

		//Include self if not firewalled. File is partially shared.
		//Do this even if not byte is downloaded yet, because there will be
		if( m_pShell->GetBytesCompleted() )
		{
			IPv4 LocalAddr;
			LocalAddr.Host = m_pNet->m_CurrentIP;
			LocalAddr.Port = m_pNet->m_CurrentPort;
			
			if(m_pPrefs->m_ForcedHost.S_addr)
				LocalAddr.Host = m_pPrefs->m_ForcedHost;


			if(!m_pNet->m_TcpFirewall)
				m_pShell->AddAltLocation(LocalAddr);
			else if(m_pNet->m_UdpFirewall != UDP_BLOCK)
			{
				//m_pShell->AddF2FLocation(LocalAddr, , );
			}
		}

		// Alt-Location
		GetFile += m_pShell->GetAltLocHeader( HostInfo()->Address.Host );
		// NAlt-Location
		GetFile += m_pShell->GetNaltLocHeader( HostInfo()->Address.Host );
	}

	// End header
	GetFile		+= "\r\n";

	// Send Header
	m_pSocket->Send(GetFile, GetFile.GetLength());

	m_Request = GetFile;
	HostInfo()->Handshake += GetFile;

	m_nSecsDead = 0;

	// Dont change status if queued
	if(m_Status != TRANSFER_QUEUED)
		StatusUpdate(TRANSFER_CONNECTED);
}

void CGnuDownload::SendPushProxyRequest()
{
	// /gnet/push-proxy?guid=<ServentIdAsGivenInTheQueryHitAsABase16UrlEncodedString>
	// X-Node: <IP>:<PORT>

	FileSource* pSource = HostInfo();

	CString FileRequest;
	if(m_PushF2F)	
		FileRequest = "&file=" + NumtoStr(F2F_PUSH_INDEX);	

	CString GetProxy;
	GetProxy += "HEAD /gnet/push-proxy?guid=" + EncodeBase16((byte*) &pSource->PushID, 16) + FileRequest + " HTTP/1.1\r\n";
	GetProxy += "X-Node: " + IPtoStr(m_pNet->m_CurrentIP) + ":" + NumtoStr(m_pNet->m_CurrentPort) + "\r\n";
	GetProxy += "\r\n";

	m_pSocket->Send(GetProxy, GetProxy.GetLength());

	m_nSecsDead = 0;
	m_Request = GetProxy;
	
	pSource->PushSent   = true;
	pSource->Handshake += GetProxy;

	SetError("Push Proxy Connected");
	StatusUpdate(TRANSFER_CONNECTED);
}

void CGnuDownload::SendTigerRequest()
{
	m_PartActive = false;
	m_PartNumber = 0;

	m_StartPos = 0;

	m_TigerRequest = true;


	CString GetTree;

	GetTree.Format("GET %s HTTP/1.1\r\n", HostInfo()->TigerPath);

	// Add host header
	CString Host; 
	Host.Format("Host: %s:%i\r\n", IPtoStr(HostInfo()->Address.Host), HostInfo()->Address.Port); 
	GetTree += Host;  


	// Add user-agent header
	GetTree += "User-Agent: " + m_pNet->m_pCore->GetUserAgent() + "\r\n";

	// Listen-IP header   Listen-IP: 0.0.0.0:634
	GetTree += "Listen-IP: " +  IPtoStr(m_pNet->m_CurrentIP) + ":" + NumtoStr(m_pNet->m_CurrentPort) + "\r\n";
	
	// Connection header
	GetTree += "Connection: Keep-Alive\r\n";
	
	// Accept header
	if( HostInfo()->TigerUseThex )
		GetTree	+= "Accept: application/dime\r\n";
	else
		GetTree	+= "Accept: application/tigertree-breadthfirst\r\n";

	// Range header
	GetTree	+= "Range: bytes=0-\r\n";

	// X-Queue
	GetTree	+= "X-Queue: 0.1\r\n";

	// X-Features
	GetTree += "X-Features: g2/1.0\r\n";

	// End header
	GetTree		+= "\r\n";

	// Send Header
	m_pSocket->Send(GetTree, GetTree.GetLength());

	m_Request = GetTree;
	HostInfo()->Handshake += GetTree;

	m_nSecsDead = 0;


	// Dont change status if queued
	if(m_Status != TRANSFER_QUEUED)
		StatusUpdate(TRANSFER_CONNECTED);
}

bool CGnuDownload::StartDownload()
{
	// Reset handshake
	HostInfo()->Handshake = "";
	
	SetError("Connecting...");

	StatusUpdate(TRANSFER_CONNECTING);

	if( !m_pSocket->Create() )
	{
		SetError("Unable to create socket");
		return false;
	}

	if(  m_pShell->m_UseProxy && m_LocalProxy.host.IsEmpty() )
	{
		if( !m_pShell->m_DefaultProxy.IsEmpty() )
			m_LocalProxy = StrtoProxy(m_pShell->m_DefaultProxy);
		else
			m_LocalProxy = m_pPrefs->GetRandProxy();
	}

	m_ConnectAddress.Host = m_pShell->m_UseProxy ? StrtoIP(m_LocalProxy.host) : HostInfo()->Address.Host;
	m_ConnectAddress.Port = m_pShell->m_UseProxy ? m_LocalProxy.port : HostInfo()->Address.Port;

	if(m_PushProxy)
	{
		m_ConnectAddress.Host = m_ProxyAddress.Host;
		m_ConnectAddress.Port = m_ProxyAddress.Port;

		SetError("Push Proxy " + IPtoStr(m_ConnectAddress.Host) + ":" + NumtoStr(m_ConnectAddress.Port) + "...");
		StatusUpdate(TRANSFER_CONNECTING);
	}

	// Attempt connect
	if( !m_pSocket->Connect(IPtoStr(m_ConnectAddress.Host), m_ConnectAddress.Port) )
	{
		int ErrorCode = GetLastError();

		if(ErrorCode != WSAEWOULDBLOCK)
		{
			// Get error code
			SetError("Connect Error " + NumtoStr(ErrorCode) );
			
			SendPushRequest();
			
			return false;
		}
	}

	return true;
}

void CGnuDownload::SendPushRequest()
{
	FileSource* HostSource = HostInfo();

	if( HostSource->Network == NETWORK_GNUTELLA )
	{
		bool SendPush = false;

		// if theres a firewall or
		if( !m_pNet->m_TcpFirewall)
			SendPush = true;

		if( m_pNet->m_TcpFirewall && HostSource->SupportF2F && m_pNet->m_UdpFirewall != UDP_BLOCK)
		{
				m_PushF2F  = true;
				HostInfo()->FileIndex = F2F_PUSH_INDEX;
		}
				
		if(SendPush || m_PushF2F)
		{
			if(HostSource->DirectHubs.size())
				DoPushProxy();
			
			if( m_pNet->m_pGnu ) 
			{
				m_pNet->m_pGnu->m_pProtocol->Send_Push( *HostSource );
				SetError("Gnutella Push Sent");
			}

			if(m_PushF2F)
			{
				// check first if there already is a udp socket trying this host
				// push can still go through because connection sending udp packets to right place
				for(int i = 0; i < m_pShell->m_Sockets.size(); i++)
				{
					CGnuDownload* download = m_pShell->m_Sockets[i];
					
					if(download->m_HostID == HostSource->SourceID && download->m_pSocket->m_RudpMode)
						return;
				}
				
				CGnuDownload* download  = new CGnuDownload(m_pShell, HostSource->SourceID);
				m_pShell->m_Sockets.push_back(download);
				
				download->m_pSocket->RudpConnect(HostInfo()->Address, m_pNet, true);
				download->StatusUpdate(TRANSFER_CONNECTING);
			}
		}
	}

	if( m_pNet->m_pG2 && HostSource->Network == NETWORK_G2 )
	{
		// If we're behind firewall a push attempt can not be received
		if(m_pNet->m_TcpFirewall)
			return;
		
		m_pNet->m_pG2->Send_PUSH(HostSource);
		SetError("G2 Push Sent");
	}

	HostSource->PushSent = true;
}

void CGnuDownload::DoPushProxy()
{
	// Close() must be called after this function

	FileSource* pSource = HostInfo();
	
	// Create new download to talk with proxy server
	CGnuDownload* pSock = new CGnuDownload(m_pShell, m_HostID);

	pSock->m_PushProxy = true;
	pSock->m_PushF2F   = m_PushF2F;

	// Get random push proxy ultrapeer
	int index = rand() % pSource->DirectHubs.size();
	pSock->m_ProxyAddress = pSource->DirectHubs[index];
	
	// Erase selected ultrapeer so it isnt retried again
	std::vector<IPv4>::iterator itProxy = pSource->DirectHubs.begin();
	while( itProxy != pSource->DirectHubs.end() )
		if( (*itProxy).Host.S_addr == pSock->m_ProxyAddress.Host.S_addr && (*itProxy).Port == pSock->m_ProxyAddress.Port)
			itProxy = pSource->DirectHubs.erase(itProxy);
		else
			itProxy++;

	if(pSock->StartDownload())
		m_pShell->m_Sockets.push_back(pSock);
	else
		delete pSock;
}

void CGnuDownload::StopDownload()
{
	SetError("Stopped");
	Close();
}


void CGnuDownload::DownloadBytes(byte* pBuff, int nSize)
{
	// Stream Test
	/* if(m_pShell->m_Name != "stream.mp3")
		if(m_pShell->m_File.m_hFile == CFileLock::hFileNull)
		{
			SetError("File not open");
			Close();
			return;
		}*/
	
	// If downloading tigertree
	if( m_TigerRequest )
	{
		// Download all data before parsing
		if(m_TigerPos < m_TigerLength)
		{
			int CopySize = nSize;
			if(m_TigerPos + CopySize > m_TigerLength)
				CopySize = m_TigerLength - m_TigerPos;

			memcpy(m_TigerReqBuffer + m_TigerPos, pBuff, CopySize);	
			m_TigerPos += CopySize;
			m_AvgRecvBytes.Input(nSize);
		}

		// TigerTree data downloaded
		if(m_TigerPos == m_TigerLength)
		{
			bool TigerSet = false;

			// THEX based uses DIME format
			if( HostInfo()->TigerUseThex )
			{
				// Re-Set m_TigerLength here, used for LoadTigerTree
				DIME ThexData(m_TigerReqBuffer, m_TigerLength);

				DimeRecord CurrentRecord;
				while( ThexData.ReadNextRecord(CurrentRecord) == DIME::READ_GOOD)
				{	
					if(CurrentRecord.Type == "http://open-content.net/spec/thex/breadthfirst")
					{
						m_TigerLength = CurrentRecord.DataLength;

						if( !LoadTigerTree() )
						{
							HostInfo()->TigerSupport = false;
							SetError("TigerTree Load Failed");
							Close();	
							return;
						}

						memcpy(m_tempTigerTree, CurrentRecord.Data, m_tempTreeSize);
						TigerSet = true;	
					}

					if(CurrentRecord.Last)
						break;
				}
			}

			// No THEX, content-length is entire tree
			else
			{
				if( !LoadTigerTree() ) // uses m_TigerLength set to content-length
				{
					HostInfo()->TigerSupport = false;
					SetError("TigerTree Load Failed");
					Close();	
					return;
				}

				memcpy(m_tempTigerTree, m_TigerReqBuffer, m_tempTreeSize);	
				TigerSet = true;
			}

			
			// copy over to shell to be used for verification
			if(TigerSet)
			{
				m_pShell->m_TigerTree = new byte[m_tempTreeSize];
				memcpy(m_pShell->m_TigerTree, m_tempTigerTree, m_tempTreeSize);
				m_pShell->m_TreeSize = m_tempTreeSize;
				m_pShell->m_TreeRes  = m_tempTreeRes;
				m_pShell->BackupHosts(); // Backup tree
			}
			else
				HostInfo()->TigerSupport = false;


			if(m_KeepAlive && !m_pShell->CheckCompletion() )
			{
				StatusUpdate(TRANSFER_CONNECTED);
				SendRequest();
			}
			else
			{
				Close();
				HostInfo()->RetryWait = 0;
				m_pShell->SetHostState(m_HostID, FileSource::eAlive);
			} 
		}

		return;
	}


	// Normal File Transfer
	FilePart* pPart = &m_pShell->m_PartList[m_PartNumber];
	
	uint64 FilePos = pPart->StartByte + pPart->BytesCompleted;

	// Prevent download overruns
	if(FilePos + nSize > pPart->EndByte + 1)
	{
		nSize -= (FilePos + nSize) - (pPart->EndByte + 1);
	}

	// Write data to file
	try
	{
		/* Stream Test
		if(m_pShell->m_Name == "stream.mp3")
		{
			byte chkBuff[RECEIVE_BUFF];
			memset(chkBuff, 0, RECEIVE_BUFF);

			if( memcmp(pBuff, chkBuff, nSize) != 0 )
				m_pShell->m_pTrans->m_pCore->DebugLog("Download Check Failed at " + CommaIze(NumtoStr(m_BytesRecvd)) + " - Recvd " + CommaIze(NumtoStr(nSize)));
		}
		else
		{*/
			m_pShell->m_File.SeekandWrite(FilePos, pBuff, nSize);

			//m_pShell->m_pCore->DebugLog("Download", "pos " + CommaIze(NumtoStr(FilePos)) + " writing " + CommaIze(NumtoStr(nSize)) + " bytes from " + IPv4toStr(HostInfo()->Address));
		//}

		pPart->BytesCompleted += nSize;

		m_AvgRecvBytes.Input(nSize);
	}
	catch(CFileException* e)
	{
		SetError(GetFileError(e));
		e->Delete();
		Close();
		return;
	}
		

	if(!m_pShell->m_UpdatedInSecond)
		m_pShell->m_UpdatedInSecond = true;
	
	

	if( m_pShell->PartDone(m_PartNumber) )
	{
		// Stream Test
		/*if(m_pShell->m_Name == "stream.mp3")
		{
			m_pShell->m_PartList[m_PartNumber].BytesCompleted = 0;
			m_pShell->m_PartList[m_PartNumber].Verified = 0;
		}*/

		if(m_KeepAlive)
		{
			if( !m_pShell->CheckCompletion() )
			{
				StatusUpdate(TRANSFER_CONNECTED);
				SendRequest();
			}
			else
				Close();

			return;
		}
		
		Close();

		HostInfo()->RetryWait = 0;
		m_pShell->SetHostState(m_HostID, FileSource::eAlive);
	}
}

void CGnuDownload::StatusUpdate(DWORD Status)
{
	m_nSecsDead = 0;

	m_Status = Status;
	//m_Current->ChangeTime = CTime::GetCurrentTime();
	
	m_pShell->m_UpdatedInSecond = true;
}

void CGnuDownload::SetError(CString Error)
{
	if(m_pSocket->m_RudpMode)
		Error += " (udp)";

	HostInfo()->Error = Error;
}

FileSource* CGnuDownload::HostInfo()
{
	ASSERT( m_HostID >= 0 && m_HostID < m_pShell->m_Queue.size() );

	return &m_pShell->m_Queue[m_HostID];
}

void CGnuDownload::Timer()
{
	m_pSocket->OnSecond();
	
	HostInfo()->RealBytesPerSec = m_AvgRecvBytes.GetAverage();

	// Time out downloads
	if(m_Status == TRANSFER_CONNECTING)
	{
		if( m_pNet->NetStat.GetStatus(m_ConnectAddress) != -1 || !m_pNet->NetStat.IsLoaded())
			m_nSecsDead++;

		if(m_nSecsDead > TRANSFER_TIMEOUT)
		{
			SetError("Attempt Timed Out");

			if( HostInfo()->Tries > 10)
				m_pShell->SetHostState(m_HostID, FileSource::eFailed);
			else
			{
				HostInfo()->RetryWait = RETRY_WAIT;
				m_pShell->SetHostState(m_HostID, FileSource::eTryAgain);
			}
			
			SendPushRequest();	
			
			Close();
		}	
	}

	if(m_Status == TRANSFER_CONNECTED)
	{
		m_nSecsDead++;

		if(m_nSecsDead > TRANSFER_TIMEOUT)
		{
			SetError("No Response");
			SendPushRequest();
			
			Close();
		}
	}

	if(m_Status == TRANSFER_RECEIVING)
	{

		// Check for dead transfer
		if(m_AvgRecvBytes.m_SecondSum == 0)
		{
			m_nSecsDead++;

			if(m_nSecsDead > 10)
			{
				SetError("No Response Receiving");
				Close();
			}
		}
		else
			m_nSecsDead = 0;

		if(m_pPrefs->m_MinDownSpeed)
		{
			// Check if its under the bandwidth limit
			if((float)m_AvgRecvBytes.m_SecondSum / (float)1024 < m_pPrefs->m_MinDownSpeed)
				m_nSecsUnderLimit++;
			else
				m_nSecsUnderLimit = 0;

			if(m_nSecsUnderLimit > 15)
			{
				SetError("Below Minimum Speed");
				Close();
			}
		}
	}

	if(m_Status == TRANSFER_QUEUED)
	{
		m_RetryMin--;

		if (m_RetryMin == 0)
		{
			//Time to send a queue update again
			TRACE0("Q> Repolling queue. File:" + HostInfo()->Name + " IP:" + IPtoStr(HostInfo()->Address.Host) + "\n");	//TODO: Tor: Remove (QueueTrace)
			
			SendRequest();
		}
	}

	m_AvgRecvBytes.Next();
}

bool CGnuDownload::LoadTigerTree()
{
	// Create list so we can calc depth we need and depth host is giving us
	int NodeCount  = m_pShell->m_FileLength / BLOCKSIZE;
	uint64 NodeRes = BLOCKSIZE;

	if(m_pShell->m_FileLength % BLOCKSIZE > 0)
		NodeCount++;


	std::list<int> LayerSizes; // Number of nodes in each layer of tree
	
	while(NodeCount > 1)
	{
		LayerSizes.push_front(NodeCount);
		NodeCount = NodeCount / 2 + (NodeCount % 2);	
		NodeRes *= 2;
	}
	LayerSizes.push_front( 1 );


	// Go through list until we have right size tree
	int i = 0, LocalLength = 0;
	std::list<int>::iterator itLayer;
	for(i = 1, itLayer = LayerSizes.begin(); itLayer != LayerSizes.end(); i++, itLayer++)
	{
		LocalLength += (*itLayer) * TIGERSIZE;

		if(m_TigerLength < LocalLength)
			break;

		if(m_TigerLength == LocalLength || NodeRes <= m_pShell->m_PartSize)
		{
			if(m_tempTigerTree)
				delete [] m_tempTigerTree;

			m_tempTigerTree = new byte[LocalLength];
			m_tempTreeSize  = LocalLength;
			m_tempTreeRes   = NodeRes;

			return true;
		}

		
		NodeRes /= 2;
	}

	return false;
}

void CGnuDownload::AddHosttoMesh(IPv4 Address)
{
	FileSource Info;
	Info.Address  = Address;
	Info.Size     = m_pShell->m_FileLength;
	Info.Sha1Hash = m_pShell->m_Sha1Hash;

	m_pShell->AddHost(Info);
}
