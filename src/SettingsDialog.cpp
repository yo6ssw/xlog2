#include "SettingsDialog.h"

#include "UiUtil.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
// Parse an int entry, falling back to `def` on garbage (mirrors the old dialogs).
int toInt(const Gtk::Entry* e, int def) {
    try { return std::stoi(e->get_text().raw()); }
    catch (const std::exception&) { return def; }
}
std::string strOr(const Gtk::Entry* e, const char* def) {
    const std::string s = e->get_text().raw();
    return s.empty() ? std::string(def) : s;
}
}  // namespace

Gtk::Grid& SettingsDialog::addPage(Gtk::Stack& stack, const char* id, const char* title) {
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    ui::setMargin(*grid, 12);
    stack.add(*grid, id, title);
    return *grid;
}

SettingsDialog::SettingsDialog(const Settings& s, std::function<void(const Settings&)> onApply)
    : seed_(s), onApply_(std::move(onApply)) {
    set_title("Settings");
    set_modal(true);
    set_hide_on_close(true);
    set_default_size(560, 460);

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    root->set_spacing(8);
    ui::setMargin(*root, 12);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    body->set_spacing(8);
    body->set_vexpand(true);
    auto* stack = Gtk::make_managed<Gtk::Stack>();
    stack->set_hexpand(true);
    stack->set_vexpand(true);
    auto* sidebar = Gtk::make_managed<Gtk::StackSidebar>();
    sidebar->set_stack(*stack);
    body->append(*sidebar);
    body->append(*stack);
    root->append(*body);

    // A right-aligned label + widget row in `grid`.
    auto field = [](Gtk::Grid& grid, const char* text, Gtk::Widget& w, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(1.0);
        grid.attach(*l, 0, row);
        w.set_hexpand(true);
        grid.attach(w, 1, row);
    };
    auto entry = [](const std::string& text) {
        auto* e = Gtk::make_managed<Gtk::Entry>();
        e->set_text(text);
        return e;
    };

    // --- Station ---
    {
        auto& g = addPage(*stack, "station", "Station");
        myLocator_ = entry(s.myLocator);
        myLocator_->set_placeholder_text("e.g. JN58td");
        field(g, "My locator:", *myLocator_, 0);
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "Your Maidenhead grid — the world map's home point.");
        hint->set_xalign(0.0);
        g.attach(*hint, 0, 1, 2, 1);
    }

    // --- Network ---
    {
        auto& g = addPage(*stack, "network", "Network");
        udpPort_ = entry(std::to_string(s.udpPort));
        field(g, "UDP listen port:", *udpPort_, 0);
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "Receives QSOs from WSJT-X and similar (default 2237).");
        hint->set_xalign(0.0);
        g.attach(*hint, 0, 1, 2, 1);
    }

    // --- Rig ---
    {
        auto& g = addPage(*stack, "rig", "Rig");
        rigModel_ = entry(std::to_string(s.rigModel));
        rigDevice_ = entry(s.rigDevice);
        rigDevice_->set_placeholder_text("/dev/ttyUSB0");
        rigPoll_ = entry(std::to_string(s.rigPollMs));
        rigAuto_ = Gtk::make_managed<Gtk::CheckButton>("Connect to rig at startup");
        rigAuto_->set_active(s.rigAutoConnect);
        field(g, "Hamlib model:", *rigModel_, 0);
        field(g, "Device:", *rigDevice_, 1);
        field(g, "Poll (ms):", *rigPoll_, 2);
        g.attach(*rigAuto_, 1, 3);
    }

    // --- DX Cluster ---
    {
        auto& g = addPage(*stack, "dx", "DX Cluster");
        dxHost_ = entry(s.dxHost);
        dxHost_->set_placeholder_text("cluster.example.net");
        dxPort_ = entry(std::to_string(s.dxPort));
        dxLogin_ = entry(s.dxLogin);
        dxLogin_->set_placeholder_text("your callsign (sent at the login prompt)");
        dxAuto_ = Gtk::make_managed<Gtk::CheckButton>("Connect at startup");
        dxAuto_->set_active(s.dxAutoConnect);
        field(g, "Host:", *dxHost_, 0);
        field(g, "Port:", *dxPort_, 1);
        field(g, "Login call:", *dxLogin_, 2);
        g.attach(*dxAuto_, 1, 3);
    }

    // --- LoTW ---
    {
        auto& g = addPage(*stack, "lotw", "LoTW");
        lotwUser_ = entry(s.lotwUser);
        lotwPass_ = entry(s.lotwPassword); lotwPass_->set_visibility(false);
        lotwStation_ = entry(s.lotwStation);
        lotwStation_->set_placeholder_text("tqsl station location (optional)");
        tqslPath_ = entry(s.tqslPath);
        field(g, "LoTW username:", *lotwUser_, 0);
        field(g, "LoTW password:", *lotwPass_, 1);
        field(g, "Station location:", *lotwStation_, 2);
        field(g, "tqsl path:", *tqslPath_, 3);
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "Stored in plain text in ~/.config/xlog2/layout.ini (mode 0600).");
        hint->set_xalign(0.0);
        g.attach(*hint, 0, 4, 2, 1);
    }

    // --- QRZ ---
    {
        auto& g = addPage(*stack, "qrz", "QRZ");
        qrzUser_ = entry(s.qrzUser);
        qrzPass_ = entry(s.qrzPassword); qrzPass_->set_visibility(false);
        qrzCacheDays_ = entry(std::to_string(s.qrzCacheDays));
        field(g, "QRZ username:", *qrzUser_, 0);
        field(g, "QRZ password:", *qrzPass_, 1);
        field(g, "Cache lifetime (days):", *qrzCacheDays_, 2);
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "Used for the Call field's QRZ.com XML lookup. Stored mode 0600.\n"
            "Cached lookups skip the network until this old (0 = no cache).");
        hint->set_xalign(0.0);
        g.attach(*hint, 0, 3, 2, 1);
    }

    // --- CW Keyer ---
    {
        auto& g = addPage(*stack, "keyer", "CW Keyer");
        keyerHost_ = entry(s.keyerHost);
        keyerPort_ = entry(std::to_string(s.keyerPort));
        keyerSpeed_ = entry(s.keyerSpeed > 0 ? std::to_string(s.keyerSpeed) : "");
        keyerSpeed_->set_placeholder_text("wpm (blank = leave cwdaemon default)");
        field(g, "Host:", *keyerHost_, 0);
        field(g, "Port:", *keyerPort_, 1);
        field(g, "Speed:", *keyerSpeed_, 2);
        for (int i = 0; i < 9; ++i) {
            keyerMsgs_[i] = entry(s.keyerMessages[i]);
            field(g, ("F" + std::to_string(i + 1) + ":").c_str(), *keyerMsgs_[i], 3 + i);
        }
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "Tokens: %CALL% %NAME% %QTH% %RST% (from the entry form).");
        hint->set_xalign(0.0);
        g.attach(*hint, 0, 12, 2, 1);
    }

    // --- Paddle ---
    {
        auto& g = addPage(*stack, "paddle", "Paddle");
        paddleHost_ = entry(s.paddleHost);
        paddlePort_ = entry(std::to_string(s.paddlePort));
        paddleWpm_ = entry(std::to_string(s.paddleWpm));
        paddleIambicB_ = Gtk::make_managed<Gtk::CheckButton>("Iambic B (default: iambic A)");
        paddleIambicB_->set_active(s.paddleIambicB);
        paddleAutospace_ = Gtk::make_managed<Gtk::CheckButton>("Autospace (enforce inter-character spacing)");
        paddleAutospace_->set_active(s.paddleAutospace);
        paddleSidetone_ = Gtk::make_managed<Gtk::CheckButton>("Local sidetone");
        paddleSidetone_->set_active(s.paddleSidetone);
        paddleTone_ = entry(std::to_string(s.paddleToneHz));
        paddleLevel_ = entry(std::to_string(s.paddleLevel));
        paddleDevice_ = entry(s.paddleSidetoneDevice);
        paddleDevice_->set_placeholder_text("ALSA playback device, e.g. default");
        paddleMute_ = Gtk::make_managed<Gtk::CheckButton>("Mute rig audio while keying");
        paddleMute_->set_active(s.paddleMuteAudio);
        paddleMuteTail_ = entry(std::to_string(s.paddleMuteTailMs));
        field(g, "Host:", *paddleHost_, 0);
        field(g, "Port:", *paddlePort_, 1);
        field(g, "Speed (wpm):", *paddleWpm_, 2);
        g.attach(*paddleIambicB_, 1, 3);
        g.attach(*paddleAutospace_, 1, 4);
        g.attach(*paddleSidetone_, 1, 5);
        field(g, "Tone (Hz):", *paddleTone_, 6);
        field(g, "Volume (0–100):", *paddleLevel_, 7);
        field(g, "Sidetone device:", *paddleDevice_, 8);
        g.attach(*paddleMute_, 1, 9);
        field(g, "Mute tail (ms):", *paddleMuteTail_, 10);
    }

    // --- Audio ---
    {
        auto& g = addPage(*stack, "audio", "Audio");
        audioHost_ = entry(s.audioHost);
        audioPort_ = entry(std::to_string(s.audioPort));
        audioRate_ = entry(std::to_string(s.audioSampleRate));
        audioChan_ = entry(std::to_string(s.audioChannels));
        audioDevice_ = entry(s.audioDevice);
        audioDevice_->set_placeholder_text("ALSA playback device, e.g. default");
        field(g, "Host:", *audioHost_, 0);
        field(g, "Port:", *audioPort_, 1);
        field(g, "Sample rate:", *audioRate_, 2);
        field(g, "Channels:", *audioChan_, 3);
        field(g, "Playback device:", *audioDevice_, 4);
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "Opus rate (8000/12000/16000/24000/48000) and channels must match\n"
            "the cwsd `audio` section. cwsd's default port is 7355.");
        hint->set_xalign(0.0);
        g.attach(*hint, 0, 5, 2, 1);
    }

    // --- Skimmer ---
    {
        auto& g = addPage(*stack, "skimmer", "Skimmer");
        skGate_ = entry(std::to_string(s.skimmerGate));
        skMinSnr_ = entry(std::to_string(s.skimmerMinSnr));
        skKnownOnly_ = Gtk::make_managed<Gtk::CheckButton>("Paranoid: only surface DB-confirmed calls");
        skKnownOnly_->set_active(s.skimmerKnownOnly);
        skBwNormDb_ = entry(std::to_string(s.skimmerBwNormDb));
        skBwNormRef_ = entry(std::to_string(s.skimmerBwNormRefHz));
        skBwOffset_ = entry(std::to_string(s.skimmerBwOffsetDb));
        field(g, "Detection gate (dB):", *skGate_, 0);
        field(g, "Min per-channel SNR (dB):", *skMinSnr_, 1);
        g.attach(*skKnownOnly_, 1, 2);
        field(g, "Waterfall BW norm (dB/oct):", *skBwNormDb_, 3);
        field(g, "BW norm reference (Hz):", *skBwNormRef_, 4);
        field(g, "Waterfall trim (dB):", *skBwOffset_, 5);
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "BW norm dims the waterfall as the rig's IF filter narrows.");
        hint->set_xalign(0.0);
        g.attach(*hint, 0, 6, 2, 1);
    }

    // --- Sync ---
    {
        auto& g = addPage(*stack, "sync", "Sync");
        syncEnabled_ = Gtk::make_managed<Gtk::CheckButton>("Synchronise the default logbook with LAN peers");
        syncEnabled_->set_active(s.syncEnabled);
        syncSecret_ = entry(s.syncSecret); syncSecret_->set_visibility(false);
        syncPort_ = entry(std::to_string(s.syncPort));
        syncPeerHost_ = entry(s.syncPeerHost);
        syncPeerHost_->set_placeholder_text("optional WAN peer host (internet)");
        syncPeerHostAlt_ = entry(s.syncPeerHostAlt);
        syncPeerHostAlt_->set_placeholder_text("optional second WAN peer host");
        syncNodeName_ = entry(s.syncNodeName);
        syncNodeName_->set_placeholder_text("optional display name for this node");
        syncRequireIdentity_ = Gtk::make_managed<Gtk::CheckButton>(
            "Reject peers without a verified identity");
        syncRequireIdentity_->set_active(s.syncRequireIdentity);
        g.attach(*syncEnabled_, 1, 0);
        field(g, "Shared secret:", *syncSecret_, 1);
        field(g, "Listen port:", *syncPort_, 2);
        field(g, "WAN peer:", *syncPeerHost_, 3);
        field(g, "WAN peer 2:", *syncPeerHostAlt_, 4);
        field(g, "Node name:", *syncNodeName_, 5);
        g.attach(*syncRequireIdentity_, 1, 6);
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "Peers on the same LAN find each other automatically — no roles to set.\n"
            "Give every machine the same secret: it picks the mesh and (via libsodium)\n"
            "encrypts and authenticates every peer link, and each node gets a\n"
            "self-certifying identity. Choose who to sync with under Sync ▸ Trusted\n"
            "peers. WAN peers are optional hosts for syncing over the internet.");
        hint->set_xalign(0.0);
        g.attach(*hint, 0, 7, 2, 1);
    }

    // --- buttons ---
    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    buttons->set_spacing(8);
    buttons->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    auto* apply = Gtk::make_managed<Gtk::Button>("Apply");
    auto* ok = Gtk::make_managed<Gtk::Button>("OK");
    buttons->append(*cancel);
    buttons->append(*apply);
    buttons->append(*ok);
    root->append(*buttons);

    set_child(*root);

    cancel->signal_clicked().connect([this]() { set_visible(false); });
    apply->signal_clicked().connect([this]() { onApply_(collect()); });
    ok->signal_clicked().connect([this]() { onApply_(collect()); set_visible(false); });
}

Settings SettingsDialog::collect() const {
    Settings s = seed_;  // preserve the fields not edited here

    s.myLocator = myLocator_->get_text().raw();

    s.udpPort = toInt(udpPort_, s.udpPort);

    s.rigModel = toInt(rigModel_, s.rigModel);
    s.rigDevice = rigDevice_->get_text().raw();
    s.rigPollMs = std::max(50, toInt(rigPoll_, s.rigPollMs));
    s.rigAutoConnect = rigAuto_->get_active();

    s.dxHost = dxHost_->get_text().raw();
    s.dxPort = toInt(dxPort_, 7300);
    s.dxLogin = dxLogin_->get_text().raw();
    s.dxAutoConnect = dxAuto_->get_active();

    s.lotwUser = lotwUser_->get_text().raw();
    s.lotwPassword = lotwPass_->get_text().raw();
    s.lotwStation = lotwStation_->get_text().raw();
    s.tqslPath = strOr(tqslPath_, "tqsl");

    s.qrzUser = qrzUser_->get_text().raw();
    s.qrzPassword = qrzPass_->get_text().raw();
    s.qrzCacheDays = toInt(qrzCacheDays_, s.qrzCacheDays);

    s.keyerHost = strOr(keyerHost_, "127.0.0.1");
    s.keyerPort = toInt(keyerPort_, 6789);
    {
        const std::string sp = keyerSpeed_->get_text().raw();
        s.keyerSpeed = sp.empty() ? 0 : toInt(keyerSpeed_, 0);
    }
    for (int i = 0; i < 9; ++i)
        s.keyerMessages[i] = keyerMsgs_[i]->get_text().raw();

    s.paddleHost = strOr(paddleHost_, "127.0.0.1");
    s.paddlePort = toInt(paddlePort_, 6790);
    s.paddleWpm = std::max(1, toInt(paddleWpm_, 20));
    s.paddleIambicB = paddleIambicB_->get_active();
    s.paddleAutospace = paddleAutospace_->get_active();
    s.paddleSidetone = paddleSidetone_->get_active();
    s.paddleToneHz = std::max(1, toInt(paddleTone_, 600));
    s.paddleLevel = toInt(paddleLevel_, 50);
    s.paddleSidetoneDevice = strOr(paddleDevice_, "default");
    s.paddleMuteAudio = paddleMute_->get_active();
    s.paddleMuteTailMs = std::max(0, toInt(paddleMuteTail_, s.paddleMuteTailMs));

    s.audioHost = strOr(audioHost_, "127.0.0.1");
    s.audioPort = toInt(audioPort_, 7355);
    s.audioSampleRate = toInt(audioRate_, 48000);
    s.audioChannels = toInt(audioChan_, 1);
    s.audioDevice = strOr(audioDevice_, "default");

    s.syncEnabled = syncEnabled_->get_active();
    s.syncPeerHost = syncPeerHost_->get_text().raw();
    s.syncPeerHostAlt = syncPeerHostAlt_->get_text().raw();
    s.syncPort = toInt(syncPort_, s.syncPort);
    s.syncSecret = syncSecret_->get_text().raw();
    s.syncNodeName = syncNodeName_->get_text().raw();
    s.syncRequireIdentity = syncRequireIdentity_->get_active();

    s.skimmerGate = toInt(skGate_, s.skimmerGate);
    s.skimmerMinSnr = toInt(skMinSnr_, s.skimmerMinSnr);
    s.skimmerKnownOnly = skKnownOnly_->get_active();
    s.skimmerBwNormDb = toInt(skBwNormDb_, s.skimmerBwNormDb);
    s.skimmerBwNormRefHz = toInt(skBwNormRef_, s.skimmerBwNormRefHz);
    s.skimmerBwOffsetDb = toInt(skBwOffset_, s.skimmerBwOffsetDb);

    return s;
}
