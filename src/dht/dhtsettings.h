// Copyright (c) 2018 Duality Blockchain Solutions Developers
// TODO: Add License

#ifndef DYNAMIC_DHT_DHTSETTINGS_H
#define DYNAMIC_DHT_DHTSETTINGS_H

#include <libtorrent/session.hpp>

static constexpr int MIN_DHT_PROTO_VERSION = 71000;

class CDHTSettings {
private:
    libtorrent::session_params params;
    std::string listen_interfaces;
    std::string dht_bootstrap_nodes;
    std::string user_agent;
public:
    CDHTSettings();

    void LoadSettings();

    libtorrent::settings_pack GetSettingsPack() const { return params.settings; }
    libtorrent::dht_settings GetDHTSettings() const { return params.dht_settings; }
    libtorrent::session_params GetSessionParams() const { return params; }

private:
    void LoadPeerList();

};

#endif // DYNAMIC_DHT_DHTSETTINGS_H