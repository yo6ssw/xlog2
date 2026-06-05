#pragma once

#include "IniFile.h"

#include <array>
#include <string>

// The scalar application configuration, toolkit-neutral. Window geometry, the
// session tab list and the per-column layout stay in the IniFile directly
// (they depend on live widget state), but every backend-agnostic setting lives
// here so the presenter — not the view — owns it.
struct Settings {
    // [udp]
    int  udpPort    = 2237;   // WSJT-X default
    bool udpEnabled = false;

    // [rig]
    int         rigModel       = 1;   // 1 == RIG_MODEL_DUMMY
    std::string rigDevice;
    int         rigPollMs       = 500;
    bool        rigAutoConnect  = false;

    // [lotw]
    std::string lotwUser, lotwPassword, lotwStation, lotwLastDownload;
    std::string tqslPath = "tqsl";

    // [qrz]
    std::string qrzUser, qrzPassword;

    // [keyer]
    std::string keyerHost = "127.0.0.1";
    int         keyerPort = 6789;
    int         keyerSpeed = 0;       // 0 = leave cwdaemon's default
    std::array<std::string, 9> keyerMessages{};

    // [audio] — cwsd Opus-over-UDP rig audio stream
    bool        audioEnabled    = false;
    std::string audioHost       = "127.0.0.1";
    int         audioPort       = 7355;
    int         audioSampleRate = 48000;  // must match the server (opus rate)
    int         audioChannels   = 1;      // must match the server
    std::string audioDevice     = "default";  // ALSA playback device

    // [dxcluster]
    std::string dxHost;
    int         dxPort = 7300;
    std::string dxLogin;
    std::string dxDock = "bottom";    // top|bottom|left|right
    bool        dxVisible = false;
    bool        dxAutoConnect = false;
    int         dxPanelPos = 0;       // saved Gtk::Paned divider (0 = unset)

    // Read the scalar groups out of an IniFile (missing keys keep defaults).
    static Settings load(const IniFile& ini);
    // Write the scalar groups into an IniFile (leaving other groups untouched).
    void store(IniFile& ini) const;
};
