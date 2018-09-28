// Copyright (c) 2018 Duality Blockchain Solutions Developers
// TODO: Add License

#include "dht/bootstrap.h"

#include "chainparams.h"
#include "dht/dhtsettings.h"
#include "dynode-sync.h"
#include "net.h"
#include "util.h"
#include "validation.h"

#include "libtorrent/hex.hpp" // for to_hex
#include "libtorrent/alert_types.hpp"
#include "libtorrent/bencode.hpp" // for bencode()
#include "libtorrent/kademlia/item.hpp" // for sign_mutable_item
#include "libtorrent/kademlia/ed25519.hpp"
#include "libtorrent/span.hpp"

#include <boost/thread.hpp>
#include <boost/filesystem.hpp>

#include <functional>
#include <cstdio> // for snprintf
#include <cinttypes> // for PRId64 et.al.
#include <cstdlib>
#include <fstream>

using namespace libtorrent;

static boost::thread *dhtTorrentThread;
static bool fShutdown;
session *pTorrentDHTSession = NULL;

static alert* wait_for_alert(session* dhtSession, int alert_type)
{
    LogPrintf("DHTTorrentNetwork -- wait_for_alert start.\n");
    alert* ret = nullptr;
    bool found = false;
    while (!found)
    {
        dhtSession->wait_for_alert(seconds(5));
        std::vector<alert*> alerts;
        dhtSession->pop_alerts(&alerts);
        for (std::vector<alert*>::iterator i = alerts.begin()
            , end(alerts.end()); i != end; ++i)
        {
            if ((*i)->category() == 0x1) {
                LogPrintf("DHTTorrentNetwork -- error alert message = %s, alert_type =%d\n", (*i)->message(), (*i)->type());
            }
            else if ((*i)->category() == 0x80) {
                LogPrintf("DHTTorrentNetwork -- progress alert message = %s, alert_type =%d\n", (*i)->message(), (*i)->type());
            }
            else if ((*i)->category() == 0x200) {
                LogPrintf("DHTTorrentNetwork -- performance warning alert message = %s, alert_type =%d\n", (*i)->message(), (*i)->type());
            }
            else if ((*i)->category() == 0x400) {
                LogPrintf("DHTTorrentNetwork -- dht alert message = %s, alert_type =%d\n", (*i)->message(), (*i)->type());
            }
            if ((*i)->type() != alert_type)
            {
                continue;
            }
            LogPrintf("DHTTorrentNetwork -- wait alert complete. message = %s, alert_type =%d\n", (*i)->message(), (*i)->type());
            ret = *i;
            found = true;
        }
        if (fShutdown)
            return ret;
    }
    return ret;
}

static void bootstrap(lt::session* dhtSession)
{
    LogPrintf("DHTTorrentNetwork -- bootstrapping.\n");
    wait_for_alert(dhtSession, dht_bootstrap_alert::alert_type);
    LogPrintf("DHTTorrentNetwork -- bootstrap done.\n");
}

static std::string get_log_path()
{
    boost::filesystem::path path = GetDataDir() / "dht-state.dat";
    return path.string();
}

static int save_dht_state(session* dhtSession)
{
    entry torrentEntry;
    dhtSession->save_state(torrentEntry, session::save_dht_state);
    std::vector<char> state;
    bencode(std::back_inserter(state), torrentEntry);
    std::fstream f(get_log_path().c_str(), std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    f.write(state.data(), state.size());
    LogPrintf("DHTTorrentNetwork -- save_dht_state complete.\n");
    return 0;
}

static bool load_dht_state(session* dhtSession)
{
    std::fstream f(get_log_path().c_str(), std::ios_base::in | std::ios_base::binary | std::ios_base::ate);

    auto const size = f.tellg();
    if (static_cast<int>(size) <= 0) return false;
    f.seekg(0, std::ios_base::beg);

    std::vector<char> state;
    state.resize(static_cast<std::size_t>(size));

    f.read(state.data(), state.size());
    if (f.fail())
    {
        LogPrintf("DHTTorrentNetwork -- failed to read dht-state.log\n");
        return false;
    }

    bdecode_node e;
    error_code ec;
    bdecode(state.data(), state.data() + state.size(), e, ec);
    if (ec) {
        LogPrintf("DHTTorrentNetwork -- failed to parse dht-state.log file: (%d) %s\n", ec.value(), ec.message());
        return false;
    }
    else
    {
        LogPrintf("DHTTorrentNetwork -- load dht state from dht-state.log\n");
        dhtSession->load_state(e);
    }
    return true;
}

void static DHTTorrentNetwork(const CChainParams& chainparams, CConnman& connman)
{
    LogPrintf("DHTTorrentNetwork -- started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("dht-torrent-network");
    
    try {
        CDHTSettings settings;
        // Busy-wait for the network to come online so we get a full list of Dynodes
        do {
            bool fvNodesEmpty = connman.GetNodeCount(CConnman::CONNECTIONS_ALL) == 0;
            if (!fvNodesEmpty && !IsInitialBlockDownload() && dynodeSync.IsSynced() && dynodeSync.IsBlockchainSynced())
                break;
            MilliSleep(1000);
            if (fShutdown)
                break;

        } while (true);

        // boot strap the DHT LibTorrent network
        // with current peers and Dynodes
        settings.LoadSettings();
        pTorrentDHTSession = new session(settings.GetSettingsPack());
        load_dht_state(pTorrentDHTSession);
        bootstrap(pTorrentDHTSession);
        save_dht_state(pTorrentDHTSession);
        if (!pTorrentDHTSession) {
            throw std::runtime_error("DHT Torrent network bootstraping error.");
        }
        while (!fShutdown) {
            MilliSleep(5000);
            if (!pTorrentDHTSession->is_dht_running()) {
                LogPrintf("DHTTorrentNetwork -- not running.  Loading from file and restarting bootstrap.\n");
                load_dht_state(pTorrentDHTSession);
                bootstrap(pTorrentDHTSession);
                save_dht_state(pTorrentDHTSession);
            }
        }
    }
    catch (const boost::thread_interrupted&)
    {
        LogPrintf("DHTTorrentNetwork -- terminated\n");
        throw;
    }
    catch (const std::runtime_error& e)
    {
        LogPrintf("DHTTorrentNetwork -- runtime error: %s\n", e.what());
        return;
    }
}

void StopTorrentDHTNetwork()
{
    save_dht_state(pTorrentDHTSession);
    fShutdown = true;
    if (dhtTorrentThread != NULL)
    {
        dhtTorrentThread->interrupt();
        delete dhtTorrentThread;
        dhtTorrentThread = NULL;
        LogPrintf("DHTTorrentNetwork -- StopTorrentDHTNetwork stopped.\n");
    }
    else {
        LogPrintf("DHTTorrentNetwork --StopTorrentDHTNetwork dhtTorrentThreads is null.  Stop not needed.\n");
    }
}

void StartTorrentDHTNetwork(const CChainParams& chainparams, CConnman& connman)
{
    LogPrintf("DHTTorrentNetwork -- Log file = %s.\n", get_log_path());
    fShutdown = false;
    if (dhtTorrentThread != NULL)
         StopTorrentDHTNetwork();

    dhtTorrentThread = new  boost::thread(DHTTorrentNetwork, boost::cref(chainparams), boost::ref(connman));
}

bool GetDHTMutableData(const std::array<char, 32>& public_key, const std::string& entrySalt, std::string& entryValue, int64_t& lastSequence, bool fWaitForAuthoritative)
{
    //TODO: DHT add locks
    LogPrintf("DHTTorrentNetwork -- GetDHTMutableData started.\n");

    if (!pTorrentDHTSession) {
        //message = "DHTTorrentNetwork -- GetDHTMutableData Error. pTorrentDHTSession is null.";
        return false;
    }

    if (!pTorrentDHTSession->is_dht_running()) {
        LogPrintf("DHTTorrentNetwork -- GetDHTMutableData Restarting DHT.\n");
        if (!load_dht_state(pTorrentDHTSession)) {
            LogPrintf("DHTTorrentNetwork -- GetDHTMutableData Couldn't load previous settings.  Trying to bootstrap again.\n");
            bootstrap(pTorrentDHTSession);
        }
        else {
            LogPrintf("DHTTorrentNetwork -- GetDHTMutableData  setting loaded from file.\n");
        }
    }
    else {
        LogPrintf("DHTTorrentNetwork -- GetDHTMutableData DHT already running.  Bootstrap not needed.\n");
    }

    pTorrentDHTSession->dht_get_item(public_key, entrySalt);
    LogPrintf("DHTTorrentNetwork -- MGET: %s, salt = %s\n", aux::to_hex(public_key), entrySalt);

    bool authoritative = false;
    if (fWaitForAuthoritative) {
        while (!authoritative)
        {
            alert* dhtAlert = wait_for_alert(pTorrentDHTSession, dht_mutable_item_alert::alert_type);

            dht_mutable_item_alert* dhtGetAlert = alert_cast<dht_mutable_item_alert>(dhtAlert);
            authoritative = dhtGetAlert->authoritative;
            entryValue = dhtGetAlert->item.to_string();
            lastSequence = dhtGetAlert->seq;
            LogPrintf("DHTTorrentNetwork -- GetDHTMutableData %s: %s\n", authoritative ? "auth" : "non-auth", entryValue);
        }
    }
    else {
        alert* dhtAlert = wait_for_alert(pTorrentDHTSession, dht_mutable_item_alert::alert_type);
        dht_mutable_item_alert* dhtGetAlert = alert_cast<dht_mutable_item_alert>(dhtAlert);
        authoritative = dhtGetAlert->authoritative;
        entryValue = dhtGetAlert->item.to_string();
        lastSequence = dhtGetAlert->seq;
        LogPrintf("DHTTorrentNetwork -- GetDHTMutableData %s: %s\n", authoritative ? "auth" : "non-auth", entryValue);
    }

    if (entryValue == "<uninitialized>")
        return false;

    return true;
}

static void put_mutable
(
    entry& e
    ,std::array<char, 64>& sig
    ,std::int64_t& seq
    ,std::string const& salt
    ,std::array<char, 32> const& pk
    ,std::array<char, 64> const& sk
    ,char const* str
    ,std::int64_t const& iSeq
)
{
    using dht::sign_mutable_item;
    if (str != NULL) {
        e = std::string(str);
        std::vector<char> buf;
        bencode(std::back_inserter(buf), e);
        dht::signature sign;
        seq = iSeq + 1;
        sign = sign_mutable_item(buf, salt, dht::sequence_number(seq)
            , dht::public_key(pk.data())
            , dht::secret_key(sk.data()));
        sig = sign.bytes;
    }
}

bool PutDHTMutableData(const std::array<char, 32>& public_key, const std::array<char, 64>& private_key, const std::string& entrySalt, const int64_t& lastSequence
                        ,char const* dhtValue, std::string& message)
{
    //TODO: (DHT) add locks
    LogPrintf("DHTTorrentNetwork -- PutMutableData started.\n");

    if (!pTorrentDHTSession) {
        message = "DHTTorrentNetwork -- PutDHTMutableData Error. pTorrentDHTSession is null.";
        return false;
    }

    if (!pTorrentDHTSession->is_dht_running()) {
        LogPrintf("DHTTorrentNetwork -- PutDHTMutableData Restarting DHT.\n");
        if (!load_dht_state(pTorrentDHTSession)) {
            LogPrintf("DHTTorrentNetwork -- PutDHTMutableData Couldn't load previous settings.  Trying to bootstrap again.\n");
            bootstrap(pTorrentDHTSession);
        }
        else {
            LogPrintf("DHTTorrentNetwork -- PutDHTMutableData  setting loaded from file.\n");
        }
    }
    else {
        LogPrintf("DHTTorrentNetwork -- PutDHTMutableData DHT already running.  Bootstrap not needed.\n");
    }
    
    pTorrentDHTSession->dht_put_item(public_key, std::bind(&put_mutable, std::placeholders::_1, std::placeholders::_2, 
                                        std::placeholders::_3, std::placeholders::_4, public_key, private_key, dhtValue, lastSequence), entrySalt);

    LogPrintf("DHTTorrentNetwork -- MPUT public key: %s, salt = %s, seq=%d\n", aux::to_hex(public_key), entrySalt, lastSequence);
    alert* dhtAlert = wait_for_alert(pTorrentDHTSession, dht_put_alert::alert_type);
    dht_put_alert* dhtPutAlert = alert_cast<dht_put_alert>(dhtAlert);
    message = dhtPutAlert->message();
    LogPrintf("DHTTorrentNetwork -- PutMutableData %s\n", message);

    if (dhtPutAlert->num_success == 0)
        return false;

    return true;
}