#pragma once

#include "GnuPackets.h"

class CGnuTransfers;
class CGnuNetworks;
class CGnuDownload;
class CGnuNode;
class CGnuPrefs;
class CGnuSearch;
class CGnuCore;

#define DOWNLOAD_CHUNK_SIZE (512 * 1024) // 512 KB
#define G2_RESEARCH_INT (10 * 60) //10 minutes

#define UDP_PER_TICK 25
#define RETRY_WAIT 30

struct FilePart
{
	uint64  StartByte;
	uint64  EndByte;
	int     BytesCompleted;

	int  SourceHostID;
	bool Verified;
};

class CGnuDownloadShell  
{
public:
	CGnuDownloadShell(CGnuTransfers*);
	virtual ~CGnuDownloadShell();
	
	void Init(CString Name, uint64 FileSize, int HashID, CString Hash);
	void CreatePartList();

	void AddHost(FileSource);
	void TryNextHost();
	void TryNextUdp();
	void UdpTransmit(const FileSource &fs);
	void UdpResponse(IPv4 Source);
	void CreateDownload(FileSource &Source);

	bool CheckCompletion();

	void AddAltLocation(IPv4 Address);
	CString GetAltLocHeader(IP ToIP, int HostCount=6);

	void AddNaltLocation(IPv4 Address);
	CString GetNaltLocHeader(IP ToIP, int HostCount=6);

	void SetHostState(uint32 SourceID, FileSource::States state);
	void AddToAliveHeap(uint32 SourceID);
	void AddToWaitHeap(uint32 SourceID);
	bool CheckHeap();

	void ConnectionDeleted(uint32 SourceID);

	void Start();
	void Stop();

	bool IsDownloading();
	bool IsRemotelyQueued();

	CGnuDownload* GetCurrent();
	DWORD   GetStatus();
	uint64  GetBytesCompleted();
	CString GetMetaXML(bool file);
	
	CString GetFilePath();
	CString GetFinalPath();

	void RunFile();
	void ReSearch();

	
	void Erase();

	void BackupHosts();
	void BackupParts();

	int  m_BackupBytes;
	int  m_BackupHosts;
	int  m_BackupInterval;

	CString AvailableRangesCommaSeparated();
	int GetRange(uint64 pos, unsigned char *buf, int len);

	void Timer();

	bool ReadyFile();
	
	bool URLtoSource(FileSource &WebSource, CString URL);

	enum Status 
	{
		ePending,  // Download waiting to be actived
		eActive,   // Trying sources, downloading chunks
		eCooling,  // Cooling down before retrying sources
		eWaiting,  // All sources tried and failed, waiting for more
		eDone  // Download stopped or completed
	} m_ShellStatus;


	// Download Properties
	int     m_DownloadID;
	
	int     m_Cooling;
	int		m_HostTryPos;
	int     m_G2ResearchInt;

	bool    m_HashComputed;
	bool    m_HashVerified;
	bool	m_FileMoved;

	int		m_TotalSecCount;

	int		m_Retry;
	CString m_ReasonDead;
	bool    m_UpdatedInSecond;

	CString m_Name;

	CString m_OverrideName; // in case d/l copy fails need this because partials rely on m_Name
	CString m_OverridePath;
	
	CString m_PartialPath;
	CString m_BackupPath;

	CString m_Sha1Hash;
	CString m_TigerHash;

	byte*   m_TigerTree;
	int		m_TreeSize;
	int		m_TreeRes;

	CString m_MetaXml; // used because meta loaded after transfers loaded
	int m_MetaID;
	std::map<int, CString> m_AttributeMap;

	std::vector<CGnuDownload*> m_Sockets;

	// Researching
	CString     m_Search;
	int         m_SearchID;

	uint64	m_NextReSearch;
	int		m_ReSearchInterval;
	

	// File info
	CFileLock m_File;
	uint64    m_FileLength;

	//CFile m_CheckFile;

	int m_NextHostID;
	
	std::map<IPv4, unsigned> m_AddressMap; // IP and port to spot in m_Queue
	std::map<IP, unsigned>   m_SubnetMap; // IP (subnet) to subnet count
 	std::vector<FileSource>  m_Queue;
	std::deque<FileSource>   m_UdpQueue;

	std::deque<unsigned> m_UdpSuccessQueue;
	std::deque<unsigned> m_UntestedQueue;
	std::vector<std::pair<unsigned,uint64> > m_ReadyHeap;
	std::vector<std::pair<unsigned,uint64> > m_WaitHeap;

	struct HeapComparator
	{
		// since more in the past is 'greater', our less than operator 
		// return the more recent time as 'less'
		bool operator()(std::pair<unsigned, uint64> i, 
						std::pair<unsigned, uint64> j) const
		{
			return i.second > j.second;
		}
	};

	byte m_Packet[255];

	std::deque<IPv4> m_AltHosts;
	std::deque<IPv4> m_NaltHosts;

	// Proxy
	bool    m_UseProxy;
	CString m_DefaultProxy;

	// Bandwidth stuff
	uint32   m_AvgSpeed; 

	int m_AllocBytes;
	int m_AllocBytesTotal;

	time_t m_LastConnectAttempt;
	
	CGnuPrefs*     m_pPrefs;
	CGnuNetworks*  m_pNet;
	CGnuTransfers* m_pTrans;
	CGnuCore*	   m_pCore;

	CCriticalSection m_ShellAccess;

	/////////////////////////////////////////////////////////////////////

	
	bool PartDone(int PartNumber);
	CGnuDownload* PartActive(int PartNumber);

	bool PartVerified(int PartNumber);
	bool AllPartsActive();
	
	int m_PartSize;
	std::vector<FilePart> m_PartList;
};

