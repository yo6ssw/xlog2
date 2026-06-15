#include "Settings.h"

Settings Settings::load(const IniFile& ini) {
    Settings s;

    s.udpPort    = ini.getInt("udp", "port", s.udpPort);
    s.udpEnabled = ini.getBool("udp", "enabled", s.udpEnabled);

    s.rigModel       = ini.getInt("rig", "model", s.rigModel);
    s.rigDevice      = ini.getString("rig", "device", s.rigDevice);
    s.rigPollMs      = ini.getInt("rig", "poll_ms", s.rigPollMs);
    s.rigAutoConnect = ini.getBool("rig", "autoconnect", s.rigAutoConnect);
    s.rigDock        = ini.getString("rig", "dock", s.rigDock);
    s.rigVisible     = ini.getBool("rig", "visible", s.rigVisible);
    s.rigPanelPos    = ini.getInt("rig", "position", s.rigPanelPos);

    s.lotwUser         = ini.getString("lotw", "username", s.lotwUser);
    s.lotwPassword     = ini.getString("lotw", "password", s.lotwPassword);
    s.lotwStation      = ini.getString("lotw", "station_location", s.lotwStation);
    s.tqslPath         = ini.getString("lotw", "tqsl_path", s.tqslPath);
    s.lotwLastDownload = ini.getString("lotw", "last_download", s.lotwLastDownload);

    s.qrzUser     = ini.getString("qrz", "username", s.qrzUser);
    s.qrzPassword = ini.getString("qrz", "password", s.qrzPassword);
    s.qrzCacheDays = ini.getInt("qrz", "cache_days", s.qrzCacheDays);

    s.myLocator   = ini.getString("station", "locator", s.myLocator);
    s.mapDock     = ini.getString("map", "dock", s.mapDock);
    s.mapVisible  = ini.getBool("map", "visible", s.mapVisible);
    s.mapPanelPos = ini.getInt("map", "position", s.mapPanelPos);

    s.keyerHost  = ini.getString("keyer", "host", s.keyerHost);
    s.keyerPort  = ini.getInt("keyer", "port", s.keyerPort);
    s.keyerSpeed = ini.getInt("keyer", "speed", s.keyerSpeed);
    for (int i = 0; i < 9; ++i)
        s.keyerMessages[i] =
            ini.getString("keyer", "message" + std::to_string(i + 1), s.keyerMessages[i]);

    s.paddleEnabled = ini.getBool("paddle", "enabled", s.paddleEnabled);
    s.paddleHost    = ini.getString("paddle", "host", s.paddleHost);
    s.paddlePort    = ini.getInt("paddle", "port", s.paddlePort);
    s.paddleWpm     = ini.getInt("paddle", "wpm", s.paddleWpm);
    s.paddleIambicB = ini.getBool("paddle", "iambic_b", s.paddleIambicB);
    s.paddleAutospace = ini.getBool("paddle", "autospace", s.paddleAutospace);
    s.paddleSidetone = ini.getBool("paddle", "sidetone", s.paddleSidetone);
    s.paddleToneHz  = ini.getInt("paddle", "tone_hz", s.paddleToneHz);
    s.paddleLevel   = ini.getInt("paddle", "level", s.paddleLevel);
    s.paddleSidetoneDevice = ini.getString("paddle", "sidetone_device", s.paddleSidetoneDevice);
    s.paddleMuteAudio = ini.getBool("paddle", "mute_audio", s.paddleMuteAudio);
    s.paddleMuteTailMs = ini.getInt("paddle", "mute_tail_ms", s.paddleMuteTailMs);

    s.audioEnabled    = ini.getBool("audio", "enabled", s.audioEnabled);
    s.audioHost       = ini.getString("audio", "host", s.audioHost);
    s.audioPort       = ini.getInt("audio", "port", s.audioPort);
    s.audioSampleRate = ini.getInt("audio", "sample_rate", s.audioSampleRate);
    s.audioChannels   = ini.getInt("audio", "channels", s.audioChannels);
    s.audioDevice     = ini.getString("audio", "device", s.audioDevice);

    s.skimmerDock     = ini.getString("skimmer", "dock", s.skimmerDock);
    s.skimmerVisible  = ini.getBool("skimmer", "visible", s.skimmerVisible);
    s.skimmerPanelPos = ini.getInt("skimmer", "position", s.skimmerPanelPos);
    s.skimmerGate     = ini.getInt("skimmer", "gate", s.skimmerGate);
    s.skimmerMinSnr   = ini.getInt("skimmer", "min_snr", s.skimmerMinSnr);
    s.skimmerKnownOnly = ini.getBool("skimmer", "known_only", s.skimmerKnownOnly);
    s.skimmerBwNormDb    = ini.getInt("skimmer", "bw_norm_db", s.skimmerBwNormDb);
    s.skimmerBwNormRefHz = ini.getInt("skimmer", "bw_norm_ref_hz", s.skimmerBwNormRefHz);
    s.skimmerBwOffsetDb  = ini.getInt("skimmer", "bw_offset_db", s.skimmerBwOffsetDb);

    s.dxHost        = ini.getString("dxcluster", "host", s.dxHost);
    s.dxPort        = ini.getInt("dxcluster", "port", s.dxPort);
    s.dxLogin       = ini.getString("dxcluster", "login", s.dxLogin);
    s.dxDock        = ini.getString("dxcluster", "dock", s.dxDock);
    s.dxVisible     = ini.getBool("dxcluster", "visible", s.dxVisible);
    s.dxAutoConnect = ini.getBool("dxcluster", "autoconnect", s.dxAutoConnect);
    s.dxPanelPos    = ini.getInt("dxcluster", "position", s.dxPanelPos);

    return s;
}

void Settings::store(IniFile& ini) const {
    ini.setInt("udp", "port", udpPort);
    ini.setBool("udp", "enabled", udpEnabled);

    ini.setInt("rig", "model", rigModel);
    ini.setString("rig", "device", rigDevice);
    ini.setInt("rig", "poll_ms", rigPollMs);
    ini.setBool("rig", "autoconnect", rigAutoConnect);
    ini.setString("rig", "dock", rigDock);
    ini.setBool("rig", "visible", rigVisible);
    if (rigVisible && rigPanelPos > 0)
        ini.setInt("rig", "position", rigPanelPos);

    ini.setString("lotw", "username", lotwUser);
    ini.setString("lotw", "password", lotwPassword);
    ini.setString("lotw", "station_location", lotwStation);
    ini.setString("lotw", "tqsl_path", tqslPath);
    ini.setString("lotw", "last_download", lotwLastDownload);

    ini.setString("qrz", "username", qrzUser);
    ini.setString("qrz", "password", qrzPassword);
    ini.setInt("qrz", "cache_days", qrzCacheDays);

    ini.setString("station", "locator", myLocator);
    ini.setString("map", "dock", mapDock);
    ini.setBool("map", "visible", mapVisible);
    ini.setInt("map", "position", mapPanelPos);

    ini.setString("keyer", "host", keyerHost);
    ini.setInt("keyer", "port", keyerPort);
    ini.setInt("keyer", "speed", keyerSpeed);
    for (int i = 0; i < 9; ++i)
        ini.setString("keyer", "message" + std::to_string(i + 1), keyerMessages[i]);

    ini.setBool("paddle", "enabled", paddleEnabled);
    ini.setString("paddle", "host", paddleHost);
    ini.setInt("paddle", "port", paddlePort);
    ini.setInt("paddle", "wpm", paddleWpm);
    ini.setBool("paddle", "iambic_b", paddleIambicB);
    ini.setBool("paddle", "autospace", paddleAutospace);
    ini.setBool("paddle", "sidetone", paddleSidetone);
    ini.setInt("paddle", "tone_hz", paddleToneHz);
    ini.setInt("paddle", "level", paddleLevel);
    ini.setString("paddle", "sidetone_device", paddleSidetoneDevice);
    ini.setBool("paddle", "mute_audio", paddleMuteAudio);
    ini.setInt("paddle", "mute_tail_ms", paddleMuteTailMs);

    ini.setBool("audio", "enabled", audioEnabled);
    ini.setString("audio", "host", audioHost);
    ini.setInt("audio", "port", audioPort);
    ini.setInt("audio", "sample_rate", audioSampleRate);
    ini.setInt("audio", "channels", audioChannels);
    ini.setString("audio", "device", audioDevice);

    ini.setString("skimmer", "dock", skimmerDock);
    ini.setBool("skimmer", "visible", skimmerVisible);
    if (skimmerVisible && skimmerPanelPos > 0)
        ini.setInt("skimmer", "position", skimmerPanelPos);
    ini.setInt("skimmer", "gate", skimmerGate);
    ini.setInt("skimmer", "min_snr", skimmerMinSnr);
    ini.setBool("skimmer", "known_only", skimmerKnownOnly);
    ini.setInt("skimmer", "bw_norm_db", skimmerBwNormDb);
    ini.setInt("skimmer", "bw_norm_ref_hz", skimmerBwNormRefHz);
    ini.setInt("skimmer", "bw_offset_db", skimmerBwOffsetDb);

    ini.setString("dxcluster", "host", dxHost);
    ini.setInt("dxcluster", "port", dxPort);
    ini.setString("dxcluster", "login", dxLogin);
    ini.setString("dxcluster", "dock", dxDock);
    ini.setBool("dxcluster", "visible", dxVisible);
    ini.setBool("dxcluster", "autoconnect", dxAutoConnect);
    if (dxVisible && dxPanelPos > 0)
        ini.setInt("dxcluster", "position", dxPanelPos);
}
