#include "Settings.h"

Settings Settings::load(const IniFile& ini) {
    Settings s;

    s.udpPort    = ini.getInt("udp", "port", s.udpPort);
    s.udpEnabled = ini.getBool("udp", "enabled", s.udpEnabled);

    s.rigModel       = ini.getInt("rig", "model", s.rigModel);
    s.rigDevice      = ini.getString("rig", "device", s.rigDevice);
    s.rigPollMs      = ini.getInt("rig", "poll_ms", s.rigPollMs);
    s.rigAutoConnect = ini.getBool("rig", "autoconnect", s.rigAutoConnect);

    s.lotwUser         = ini.getString("lotw", "username", s.lotwUser);
    s.lotwPassword     = ini.getString("lotw", "password", s.lotwPassword);
    s.lotwStation      = ini.getString("lotw", "station_location", s.lotwStation);
    s.tqslPath         = ini.getString("lotw", "tqsl_path", s.tqslPath);
    s.lotwLastDownload = ini.getString("lotw", "last_download", s.lotwLastDownload);

    s.qrzUser     = ini.getString("qrz", "username", s.qrzUser);
    s.qrzPassword = ini.getString("qrz", "password", s.qrzPassword);

    s.keyerHost  = ini.getString("keyer", "host", s.keyerHost);
    s.keyerPort  = ini.getInt("keyer", "port", s.keyerPort);
    s.keyerSpeed = ini.getInt("keyer", "speed", s.keyerSpeed);
    for (int i = 0; i < 9; ++i)
        s.keyerMessages[i] =
            ini.getString("keyer", "message" + std::to_string(i + 1), s.keyerMessages[i]);

    s.audioEnabled    = ini.getBool("audio", "enabled", s.audioEnabled);
    s.audioHost       = ini.getString("audio", "host", s.audioHost);
    s.audioPort       = ini.getInt("audio", "port", s.audioPort);
    s.audioSampleRate = ini.getInt("audio", "sample_rate", s.audioSampleRate);
    s.audioChannels   = ini.getInt("audio", "channels", s.audioChannels);
    s.audioDevice     = ini.getString("audio", "device", s.audioDevice);

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

    ini.setString("lotw", "username", lotwUser);
    ini.setString("lotw", "password", lotwPassword);
    ini.setString("lotw", "station_location", lotwStation);
    ini.setString("lotw", "tqsl_path", tqslPath);
    ini.setString("lotw", "last_download", lotwLastDownload);

    ini.setString("qrz", "username", qrzUser);
    ini.setString("qrz", "password", qrzPassword);

    ini.setString("keyer", "host", keyerHost);
    ini.setInt("keyer", "port", keyerPort);
    ini.setInt("keyer", "speed", keyerSpeed);
    for (int i = 0; i < 9; ++i)
        ini.setString("keyer", "message" + std::to_string(i + 1), keyerMessages[i]);

    ini.setBool("audio", "enabled", audioEnabled);
    ini.setString("audio", "host", audioHost);
    ini.setInt("audio", "port", audioPort);
    ini.setInt("audio", "sample_rate", audioSampleRate);
    ini.setInt("audio", "channels", audioChannels);
    ini.setString("audio", "device", audioDevice);

    ini.setString("dxcluster", "host", dxHost);
    ini.setInt("dxcluster", "port", dxPort);
    ini.setString("dxcluster", "login", dxLogin);
    ini.setString("dxcluster", "dock", dxDock);
    ini.setBool("dxcluster", "visible", dxVisible);
    ini.setBool("dxcluster", "autoconnect", dxAutoConnect);
    if (dxVisible && dxPanelPos > 0)
        ini.setInt("dxcluster", "position", dxPanelPos);
}
