// Copyright (c) 2019 Duality Blockchain Solutions Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dht/session.h"

#include "bdap/linkstorage.h"
#include "bdap/utils.h"
#include "dht/sessionevents.h"
#include "chainparams.h"
#include "dht/datachunk.h"
#include "dht/dataheader.h"
#include "dht/settings.h"
#include "dynode-sync.h"
#include "net.h"
#include "spork.h"
#include "util.h"
#include "utiltime.h" // for GetTimeMillis
#include "validation.h"

#include "libtorrent/alert_types.hpp"
#include "libtorrent/bencode.hpp" // for bencode()
#include <libtorrent/hex.hpp> // for to_hex
#include "libtorrent/kademlia/ed25519.hpp"
#include <libtorrent/kademlia/item.hpp> // for sign_mutable_item
#include "libtorrent/span.hpp"

#include <boost/filesystem.hpp>

#include <cstdio> // for snprintf
#include <cinttypes> // for PRId64 et.al.
#include <cstdlib>
#include <functional>
#include <fstream>
#include <thread>

using namespace libtorrent;

static std::shared_ptr<std::thread> pDHTTorrentThread;

static bool fShutdown;
static bool fStarted;

CHashTableSession* pHashTableSession;

namespace DHT {
    std::vector<std::pair<std::string, libtorrent::entry>> vPutBytes;

    void put_mutable_bytes
    (
        libtorrent::entry& e
        ,std::array<char, 64>& sig
        ,std::int64_t& seq
        ,std::string const& salt
        ,std::array<char, 32> const& pk
        ,std::array<char, 64> const& sk
        ,libtorrent::entry const& entry
        ,std::int64_t const& iSeq
    )
    {
        using dht::sign_mutable_item;
        e = entry;
        std::vector<char> bufSign;
        bencode(std::back_inserter(bufSign), e);
        dht::signature sign;
        seq = iSeq;
        LogPrintf("%s --\nSalt = %s\nSequence = %d, e = %s\n", __func__, salt, seq, e.to_string());
        sign = sign_mutable_item(bufSign, salt, dht::sequence_number(seq)
            , dht::public_key(pk.data())
            , dht::secret_key(sk.data()));
        sig = sign.bytes;
    }
}

bool Bootstrap()
{
    LogPrintf("dht", "DHTTorrentNetwork -- bootstrapping.\n");
    const int64_t timeout = 30000; // 30 seconds
    const int64_t startTime = GetTimeMillis();
    while (timeout > GetTimeMillis() - startTime)
    {
        std::vector<CEvent> events;
        MilliSleep(1500);
        if (GetLastTypeEvent(DHT_BOOTSTRAP_ALERT_TYPE_CODE, startTime, events)) 
        {
            if (events.size() > 0 ) {
                LogPrint("dht", "DHTTorrentNetwork -- Bootstrap successful.\n");
                return true;
            }
        }
    }
    LogPrint("dht", "DHTTorrentNetwork -- Bootstrap failed after 30 second timeout.\n");
    return false;
}

std::string GetSessionStatePath()
{
    boost::filesystem::path path = GetDataDir() / "dht_state.dat";
    return path.string();
}

int SaveSessionState(session* dhtSession)
{
    entry torrentEntry;
    dhtSession->save_state(torrentEntry, session::save_dht_state);
    std::vector<char> state;
    bencode(std::back_inserter(state), torrentEntry);
    std::fstream f(GetSessionStatePath().c_str(), std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    f.write(state.data(), state.size());
    LogPrint("dht", "DHTTorrentNetwork -- SaveSessionState complete.\n");
    return 0;
}

bool LoadSessionState(session* dhtSession)
{
    std::fstream f(GetSessionStatePath().c_str(), std::ios_base::in | std::ios_base::binary | std::ios_base::ate);

    auto const size = f.tellg();
    if (static_cast<int>(size) <= 0) return false;
    f.seekg(0, std::ios_base::beg);

    std::vector<char> state;
    state.resize(static_cast<std::size_t>(size));

    f.read(state.data(), state.size());
    if (f.fail())
    {
        LogPrint("dht", "DHTTorrentNetwork -- LoadSessionState failed to read dht-state.log\n");
        return false;
    }

    bdecode_node e;
    error_code ec;
    bdecode(state.data(), state.data() + state.size(), e, ec);
    if (ec) {
        LogPrint("dht", "DHTTorrentNetwork -- LoadSessionState failed to parse dht-state.log file: (%d) %s\n", ec.value(), ec.message());
        return false;
    }
    else
    {
        LogPrint("dht", "DHTTorrentNetwork -- LoadSessionState load dht state from dht-state.log\n");
        dhtSession->load_state(e);
    }
    return true;
}

void static DHTTorrentNetwork(const CChainParams& chainparams, CConnman& connman)
{
    LogPrint("dht", "DHTTorrentNetwork -- starting\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("dht-session");
    
    try {
        CDHTSettings settings;
        // Busy-wait for the network to come online so we get a full list of Dynodes
        do {
            bool fvNodesEmpty = connman.GetNodeCount(CConnman::CONNECTIONS_ALL) == 0;
            if (!fvNodesEmpty && !IsInitialBlockDownload() && dynodeSync.IsSynced() && 
                dynodeSync.IsBlockchainSynced() && sporkManager.IsSporkActive(SPORK_30_ACTIVATE_BDAP))
                    break;

            MilliSleep(1000);
            if (fShutdown)
                return;

        } while (true);
        
        fStarted = true;
        LogPrintf("DHTTorrentNetwork -- started\n");
        // with current peers and Dynodes
        settings.LoadSettings();
        pHashTableSession->Session = settings.GetSession();
        
        if (!pHashTableSession->Session)
            throw std::runtime_error("DHT Torrent network bootstraping error.");
        
        StartEventListener(pHashTableSession->Session);
    }
    catch (const std::runtime_error& e)
    {
        fShutdown = true;
        LogPrintf("DHTTorrentNetwork -- runtime error: %s\n", e.what());
        return;
    }
}

void StopTorrentDHTNetwork()
{
    LogPrintf("DHTTorrentNetwork -- StopTorrentDHTNetwork begin.\n");
    fShutdown = true;
    MilliSleep(300);
    StopEventListener();
    MilliSleep(30);
    if (pDHTTorrentThread != NULL)
    {
        LogPrint("dht", "DHTTorrentNetwork -- StopTorrentDHTNetwork trying to stop.\n");
        if (fStarted) { 
            libtorrent::session_params params;
            params.settings.set_bool(settings_pack::enable_dht, false);
            params.settings.set_int(settings_pack::alert_mask, 0x0);
            pHashTableSession->Session->apply_settings(params.settings);
            pHashTableSession->Session->abort();
        }
        pDHTTorrentThread->join();
        LogPrint("dht", "DHTTorrentNetwork -- StopTorrentDHTNetwork abort.\n");
    }
    else {
        LogPrint("dht", "DHTTorrentNetwork --StopTorrentDHTNetwork pDHTTorrentThreads is null.  Stop not needed.\n");
    }
    pDHTTorrentThread = NULL;
    LogPrintf("DHTTorrentNetwork -- Stopped.\n");
}

void StartTorrentDHTNetwork(const CChainParams& chainparams, CConnman& connman)
{
    LogPrint("dht", "DHTTorrentNetwork -- Log file = %s.\n", GetSessionStatePath());
    fShutdown = false;
    fStarted = false;
    if (pDHTTorrentThread != NULL)
         StopTorrentDHTNetwork();

    pDHTTorrentThread = std::make_shared<std::thread>(std::bind(&DHTTorrentNetwork, std::cref(chainparams), std::ref(connman)));
}

void GetDHTStats(session_status& stats, std::vector<dht_lookup>& vchDHTLookup, std::vector<dht_routing_bucket>& vchDHTBuckets)
{
    LogPrint("dht", "DHTTorrentNetwork -- GetDHTStats started.\n");

    if (!pHashTableSession->Session) {
        return;
    }

    if (!pHashTableSession->Session->is_dht_running()) {
        return;
        //LogPrint("dht", "DHTTorrentNetwork -- GetDHTStats Restarting DHT.\n");
        //if (!LoadSessionState(pHashTableSession->Session)) {
        //    LogPrint("dht", "DHTTorrentNetwork -- GetDHTStats Couldn't load previous settings.  Trying to bootstrap again.\n");
        //    Bootstrap();
        //}
        //else {
        //    LogPrint("dht", "DHTTorrentNetwork -- GetDHTStats setting loaded from file.\n");
        //}
    }
    else {
        LogPrint("dht", "DHTTorrentNetwork -- GetDHTStats DHT already running.  Bootstrap not needed.\n");
    }

    pHashTableSession->Session->post_dht_stats();
    //get alert from map
    //alert* dhtAlert = WaitForResponse(pHashTableSession->Session, dht_stats_alert::alert_type);
    //dht_stats_alert* dhtStatsAlert = alert_cast<dht_stats_alert>(dhtAlert);
    //vchDHTLookup = dhtStatsAlert->active_requests;
    //vchDHTBuckets = dhtStatsAlert->routing_table;
    stats = pHashTableSession->Session->status();
}

void CHashTableSession::CleanUpPutCommandMap()
{
    int64_t nCurrentTime = GetTime();
    std::map<HashRecordKey, uint32_t>::iterator it;
    for (it = mPutCommands.begin(); it != mPutCommands.end(); ++it) {
        if (nCurrentTime > it->second + DHT_RECORD_LOCK_SECONDS) {
            //TODO (DHT): Change to LogPrint to make less chatty when not in debug mode.
            LogPrintf("CHashTableSession::%s -- Erased %s\n", __func__, it->first.second);
            mPutCommands.erase(it);
        }
    }
}

uint32_t CHashTableSession::GetLastPutDate(const HashRecordKey& recordKey)
{
    if (mPutCommands.find(recordKey) != mPutCommands.end() ) {
        std::map<HashRecordKey, uint32_t>::iterator it;
        for (it = mPutCommands.begin(); it != mPutCommands.end(); ++it) {
            if (it->first == recordKey)
                return it->second;
        }     
    }
    return 0;
}

bool CHashTableSession::SubmitPut(const std::array<char, 32> public_key, const std::array<char, 64> private_key, const int64_t lastSequence, CDataRecord record)
{
    strPutErrorMessage = "";
    HashRecordKey recordKey = std::make_pair(public_key, record.OperationCode());
    int64_t nLastUpdate = (int64_t)GetLastPutDate(recordKey);
    if (DHT_RECORD_LOCK_SECONDS >= (GetTime() - nLastUpdate))
    {
        strPutErrorMessage = "Record is locked. You need to wait at least " + std::to_string(DHT_RECORD_LOCK_SECONDS) + " seconds before updating the same record in the DHT.";
        return false;
    }
    mPutCommands[recordKey] = GetTime();
    vDataEntries.push_back(record);
    std::string strHeaderSalt = record.GetHeader().Salt;
    DHT::vPutBytes.clear();
    libtorrent::entry entryHeaderHex(record.HeaderHex);
    DHT::vPutBytes.push_back(std::make_pair(strHeaderSalt, entryHeaderHex));
    for (const CDataChunk& chunk : record.GetChunks()) {
        libtorrent::entry entryChunkRaw(stringFromVch(chunk.vchValue));
        DHT::vPutBytes.push_back(std::make_pair(chunk.Salt, entryChunkRaw));
        LogPrint("dht", "CHashTableSession::%s -- chunk salt: %s, value: %s\n", __func__, chunk.Salt, entryChunkRaw.to_string());
    }

    for (const std::pair<std::string, libtorrent::entry>& pair : DHT::vPutBytes) {
        Session->dht_put_item(public_key, std::bind(&DHT::put_mutable_bytes, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, 
                                public_key, private_key, pair.second, lastSequence), pair.first);
    }
    nPutRecords++;
    if (nPutRecords % 32 == 0)
        CleanUpPutCommandMap();

    return true;
}

bool CHashTableSession::SubmitGet(const std::array<char, 32>& public_key, const std::string& recordSalt)
{
    //TODO: DHT add locks
    if (!Session) {
        //message = "DHTTorrentNetwork -- GetDHTMutableData Error. Session is null.";
        return false;
    }

    if (!Session->is_dht_running()) {
        LogPrintf("CHashTableSession::%s -- GetDHTMutableData Restarting DHT.\n", __func__);
        if (!LoadSessionState(Session)) {
            LogPrintf("DHTTorrentNetwork -- GetDHTMutableData Couldn't load previous settings.  Trying to Bootstrap again.\n");
            if (!Bootstrap())
                return false;
        }
        else {
            LogPrintf("CHashTableSession::%s -- GetDHTMutableData  setting loaded from file.\n", __func__);
        }
    }

    Session->dht_get_item(public_key, recordSalt);
    LogPrint("dht", "CHashTableSession::%s -- pubkey = %s, salt = %s\n", __func__, aux::to_hex(public_key), recordSalt);

    return true;
}

bool CHashTableSession::SubmitGet(const std::array<char, 32>& public_key, const std::string& recordSalt, const int64_t& timeout, 
                            std::string& recordValue, int64_t& lastSequence, bool& fAuthoritative)
{
    std::string infoHash = GetInfoHash(aux::to_hex(public_key),recordSalt);
    RemoveDHTGetEvent(infoHash);
    if (!SubmitGet(public_key, recordSalt))
        return false;

    MilliSleep(40);
    CMutableGetEvent data;
    int64_t startTime = GetTimeMillis();
    while (timeout > GetTimeMillis() - startTime)
    {
        if (FindDHTGetEvent(infoHash, data)) {
            std::string strData = data.Value();
            // TODO (DHT): check the last position for the single quote character
            if (strData.substr(0, 1) == "'") {
                recordValue = strData.substr(1, strData.size() - 2);
            }
            else {
                recordValue = strData;
            }
            lastSequence = data.SequenceNumber();
            fAuthoritative = data.Authoritative();
            LogPrint("dht", "CHashTableSession::%s -- salt = %s, value = %s, seq = %d, auth = %u\n", __func__, recordSalt, recordValue, lastSequence, fAuthoritative);
            return true;
        }
        MilliSleep(10);
    }
    return false;
}

static std::vector<unsigned char> Array32ToVector(const std::array<char, 32>& key32)
{
    std::vector<unsigned char> vchKey;
    for(unsigned int i = 0; i < sizeof(key32); i++) {
        vchKey.push_back(key32[i]);
    }
    return vchKey;
}

bool CHashTableSession::SubmitGetRecord(const std::array<char, 32>& public_key, const std::array<char, 32>& private_seed, const std::string& strOperationType, int64_t& iSequence, CDataRecord& record)
{
    bool fAuthoritative = false;
    uint16_t nTotalSlots = 32;
    uint16_t nHeaderAttempts = 3;
    std::string strHeaderHex;
    std::string strHeaderSalt = strOperationType + ":" + std::to_string(0);
    CRecordHeader header;
    if (!SubmitGet(public_key, strHeaderSalt, 2000, strHeaderHex, iSequence, fAuthoritative)) {
        unsigned int i = 0;
        while (i < nHeaderAttempts) {
            strHeaderHex = "";
            if (header.IsNull()) {
                if (SubmitGet(public_key, strHeaderSalt, 2000, strHeaderHex, iSequence, fAuthoritative)) {
                    break;
                }
            }
            else {
                break;
            }
            i++;
        }
    }
    header.LoadHex(strHeaderHex);
    if (!header.IsNull() && header.nChunks > 0) {
        std::vector<CDataChunk> vChunks;
        for(unsigned int i = 0; i < header.nChunks; i++) {
            std::string strChunkSalt = strOperationType + ":" + std::to_string(i+1);
            std::string strChunk;
            if (!SubmitGet(public_key, strChunkSalt, 2000, strChunk, iSequence, fAuthoritative)) {
                strPutErrorMessage = "Failed to get record chunk.";
                return false;
            }
            CDataChunk chunk(i, i + 1, strChunkSalt, strChunk);
            vChunks.push_back(chunk);
        }
        CDataRecord getRecord(strOperationType, nTotalSlots, header, vChunks, Array32ToVector(private_seed));
        if (record.HasError()) {
            strPutErrorMessage = strprintf("Record has errors: %s\n", __func__, getRecord.ErrorMessage());
            return false;
        }
        record = getRecord;
        return true;
    }
    return false;
}

bool CHashTableSession::GetDataFromMap(const std::array<char, 32>& public_key, const std::string& recordSalt, CMutableGetEvent& event)
{
    std::string infoHash = GetInfoHash(aux::to_hex(public_key), recordSalt);
    if (FindDHTGetEvent(infoHash, event)) {
        LogPrint("dht", "CHashTableSession::%s -- pubkey = %s, salt = %s, value = %s, seq = %d, auth = %u\n", __func__, event.PublicKey(), event.Salt(), event.Value(), event.SequenceNumber(), event.Authoritative());
        return true;
    }
    return false;
}

bool CHashTableSession::SubmitGetAllRecordsSync(const std::vector<CLinkInfo>& vchLinkInfo, const std::string& strOperationType, std::vector<CDataRecord>& vchRecords)
{
    std::vector<std::pair<CLinkInfo, std::string>> headerValues;
    for (const CLinkInfo& linkInfo : vchLinkInfo) {
        int64_t iSequence;
        std::array <char, 32> arrPubKey;
        aux::from_hex(stringFromVch(linkInfo.vchSenderPubKey), arrPubKey.data());
        CDataRecord record;
        if (SubmitGetRecord(arrPubKey, linkInfo.arrReceivePrivateSeed, strOperationType, iSequence, record))
        {
            record.vchOwnerFQDN = linkInfo.vchFullObjectPath;
            vchRecords.push_back(record);
        }
    }
    return true;
}

static std::array<char, 32> EncodedVectorCharToArray32(const std::vector<unsigned char>& vchKey)
{
    std::string strSeed = stringFromVch(vchKey);
    std::array<char, 32> array32;
    aux::from_hex(strSeed, array32.data());
    return array32;
}

bool CHashTableSession::SubmitGetAllRecordsAsync(const std::vector<CLinkInfo>& vchLinkInfo, const std::string& strOperationType, std::vector<CDataRecord>& vchRecords)
{
    uint16_t nTotalSlots = 32;
    strPutErrorMessage = "";
    // Get the headers first
    for (const CLinkInfo& linkInfo : vchLinkInfo) {
        std::string strHeaderHex;
        std::string strHeaderSalt = strOperationType + ":" + std::to_string(0);
        SubmitGet(EncodedVectorCharToArray32(linkInfo.vchSenderPubKey), strHeaderSalt);
        MilliSleep(10);
    }

    MilliSleep(300); // Wait for headers data
    std::vector<std::pair<CLinkInfo, CMutableGetEvent>> eventHeaders;
    for (const CLinkInfo& linkInfo : vchLinkInfo) {
        std::string strHeaderSalt = strOperationType + ":" + std::to_string(0);
        const std::array<char, 32>& public_key = EncodedVectorCharToArray32(linkInfo.vchSenderPubKey);
        CMutableGetEvent mutableGetData;
        if (GetDataFromMap(public_key, strHeaderSalt, mutableGetData))
        {
            eventHeaders.push_back(std::make_pair(linkInfo, mutableGetData));
        }
    }
    for (const std::pair<CLinkInfo, CMutableGetEvent>& eventHeader: eventHeaders)
    {
        std::string strHeaderHex = eventHeader.second.Value();
        if (strHeaderHex.substr(0, 1) == "'") {
            strHeaderHex = strHeaderHex.substr(1, strHeaderHex.size() - 2);
        }
        CRecordHeader header(strHeaderHex);
        if (!header.IsNull() && nTotalSlots >= header.nChunks) {
            std::vector<CDataChunk> vChunks;
            for(unsigned int i = 0; i < header.nChunks; i++) {
                std::string strChunkSalt = strOperationType + ":" + std::to_string(i+1);
                std::array <char, 32> arrPubKey;
                aux::from_hex(eventHeader.second.PublicKey(), arrPubKey.data());
                SubmitGet(arrPubKey, strChunkSalt);
                MilliSleep(20);
            }
        }
    }
    if (eventHeaders.size() > 0) {
        MilliSleep(350); // Wait for records data
        for (const std::pair<CLinkInfo, CMutableGetEvent>& eventHeader: eventHeaders)
        {
            std::string strHeaderHex = eventHeader.second.Value();
            if (strHeaderHex.substr(0, 1) == "'") {
                strHeaderHex = strHeaderHex.substr(1, strHeaderHex.size() - 2);
            }
            CRecordHeader header(strHeaderHex);
            if (!header.IsNull() && nTotalSlots >= header.nChunks) {
                bool fSkip = false;
                std::vector<CDataChunk> vChunks;
                for(unsigned int i = 0; i < header.nChunks; i++) {
                    std::string strChunkSalt = strOperationType + ":" + std::to_string(i+1);
                    std::array <char, 32> arrPubKey;
                    aux::from_hex(eventHeader.second.PublicKey(), arrPubKey.data());
                    CMutableGetEvent eventData;
                    if (GetDataFromMap(arrPubKey, strChunkSalt, eventData)) {
                        std::string strHexValue = eventData.Value();
                        if (strHexValue.substr(0, 1) == "'") {
                            strHexValue = strHexValue.substr(1, strHexValue.size() - 2);
                        }
                        CDataChunk chunk(i, i + 1, strChunkSalt, strHexValue);
                        vChunks.push_back(chunk);
                    }
                    else {
                        bool fAuthoritative;
                        int64_t iSequence;
                        std::string strHeaderHex;
                        if (SubmitGet(arrPubKey, strChunkSalt, 2000, strHeaderHex, iSequence, fAuthoritative)) {
                            CDataChunk chunk(i, i + 1, strChunkSalt, strHeaderHex);
                            vChunks.push_back(chunk);
                        }
                        else {
                            LogPrintf("%s -- Skipped %s record for %s, chunk salt = %s\n", __func__, strOperationType, stringFromVch(eventHeader.first.vchFullObjectPath), strChunkSalt);
                            fSkip = true;
                            i = header.nChunks + 1; 
                        }
                    }
                }
                if (!fSkip) {
                    // TODO: make sure all chunks and header have the same sequence number.
                    CDataRecord record(strOperationType, nTotalSlots, header, vChunks, Array32ToVector(eventHeader.first.arrReceivePrivateSeed));
                    if (record.HasError()) {
                        strPutErrorMessage = strPutErrorMessage + strprintf("\nRecord has errors: %s\n", __func__, record.ErrorMessage());
                    }
                    else {
                        LogPrintf("%s -- Found %s record for %s\n", __func__, strOperationType, stringFromVch(eventHeader.first.vchFullObjectPath));
                        record.vchOwnerFQDN = eventHeader.first.vchFullObjectPath;
                        vchRecords.push_back(record);
                    }
                }
            }
        }
    }
    return true;
}