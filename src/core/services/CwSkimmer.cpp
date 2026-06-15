#include "CwSkimmer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using CallSet = std::unordered_set<std::string>;

constexpr int kDisplayCols = 256;  // waterfall width the panel renders

// ---- a small dependency-free iterative radix-2 FFT --------------------------
// In-place; `re`/`im` are length n == power of two. We only ever feed real
// input (im preset to 0) and read magnitudes, so a plain complex FFT is plenty.
void fft(std::vector<float>& re, std::vector<float>& im) {
    const std::size_t n = re.size();
    // bit-reversal permutation
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const float wr = static_cast<float>(std::cos(ang));
        const float wi = static_cast<float>(std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::size_t a = i + k, b = i + k + len / 2;
                const float tr = re[b] * cr - im[b] * ci;
                const float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

int popcount128(unsigned __int128 v) {
    return __builtin_popcountll(static_cast<unsigned long long>(v)) +
           __builtin_popcountll(static_cast<unsigned long long>(v >> 64));
}

std::size_t nextPow2(double v) {
    std::size_t n = 256;
    while (n < static_cast<std::size_t>(v) && n < 4096)
        n <<= 1;
    return n;
}

// ---- Morse table ------------------------------------------------------------
const std::unordered_map<std::string, char>& morseTable() {
    static const std::unordered_map<std::string, char> t = {
        {".-", 'A'},   {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},  {".", 'E'},
        {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'}, {"..", 'I'},   {".---", 'J'},
        {"-.-", 'K'},  {".-..", 'L'}, {"--", 'M'},   {"-.", 'N'},   {"---", 'O'},
        {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},  {"...", 'S'},  {"-", 'T'},
        {"..-", 'U'},  {"...-", 'V'}, {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},
        {"--..", 'Z'},
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
        {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
        {"---..", '8'}, {"----.", '9'},
        {"-..-.", '/'}, {"--..--", ','}, {".-.-.-", '.'}, {"..--..", '?'},
        {"-...-", '='}, {".-.-.", '+'}, {"-.--.", '('},
    };
    return t;
}

int editDistance(const std::string& a, const std::string& b) {
    const std::size_t n = a.size(), m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (std::size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= m; ++j) {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({sub, prev[j] + 1, cur[j - 1] + 1});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// Decode one character. This is the heart borrowed from fldigi's SOM decoder:
// rather than hard-classifying each mark into a dit/dash and looking the string
// up (which drops or mis-decodes the whole character when one element sits near
// the dit/dah boundary), match the *raw mark durations* against each Morse
// pattern's ideal duration vector and pick the best fit. Deferring the dit/dah
// decision to the character level lets the whole character's timing resolve a
// borderline element — and the decoder *abstains* on genuine ambiguity instead
// of guessing.
//
//   dur       : the char's mark durations (hops), one per element
//   sym       : the same marks hard-classified to '.'/'-' (exact-lookup fallback)
//   dit, dah  : learned dit / dah length references (hops); dah defaults to 2.5x
//
// Order: confident SOM (same length) -> exact string lookup (old behaviour) ->
// edit-distance recovery (length errors). So it is never worse than exact lookup.
char decodeChar(const std::vector<float>& dur, const std::string& sym,
                double dit, double dah, bool mature) {
    const auto& t = morseTable();

    // 1) SOM: nearest same-length pattern by per-element duration distance (mean
    //    squared error in dit units). This is the primary decision *once the
    //    channel is established* (`mature`: it has already cleanly decoded a couple
    //    of characters, so the dit/dah references are settled) — only then is
    //    deferring the per-element call to the character's timing more reliable
    //    than a hard classification. During cold start the references are unsettled
    //    and the SOM would manufacture wrong characters (polluting the callsign),
    //    so it is skipped in favour of exact lookup until the channel proves real.
    //    Accept only when the best fit is good and clearly ahead of the runner-up,
    //    so ambiguous elements are left to abstain rather than be guessed.
    if (mature && dit > 0.0 && dah > 0.0) {
        double best = 1e9, second = 1e9;
        char bestCh = '\0';
        for (const auto& [pat, ch] : t) {
            if (pat.size() != dur.size())
                continue;
            double dist = 0.0;
            for (std::size_t i = 0; i < pat.size(); ++i) {
                const double ideal = (pat[i] == '-') ? dah : dit;
                const double e = (dur[i] - ideal) / dit;
                dist += e * e;
            }
            dist /= pat.size();
            if (dist < best) { second = best; best = dist; bestCh = ch; }
            else if (dist < second) { second = dist; }
        }
        // Commit only to a good fit that is clearly ahead of the runner-up (a
        // *ratio* margin, which scales with the error magnitude — so a pattern
        // fitting ~2x better wins, while a near-tie abstains rather than guesses).
        if (bestCh && best < 0.5 && second > best * 1.8)
            return bestCh;
    }

    // 2) Exact lookup of the hard-classified string (the original primary path;
    //    also the path taken during cold start before the SOM is trusted).
    if (auto it = t.find(sym); it != t.end())
        return it->second;

    // 3) Edit-distance recovery for length errors (a merged/split element makes an
    //    invalid pattern). Confident single edit only, and not on short symbols
    //    where one error is genuinely ambiguous (E/T/I/A/N/M).
    if (sym.size() < 4)
        return '\0';
    int eBest = 99, eSecond = 99;
    char eCh = '\0';
    for (const auto& [pat, ch] : t) {
        const int d = editDistance(sym, pat);
        if (d < eBest) { eSecond = eBest; eBest = d; eCh = ch; }
        else if (d < eSecond) { eSecond = d; }
    }
    return (eBest <= 1 && eSecond > eBest) ? eCh : '\0';
}

// Find the master-callsign list entry for `tok`: an exact match (so `known`
// becomes true and the call is returned unchanged), or — if there is no exact
// match — a unique entry exactly one edit away (corrected, also `known`). Edit-
// distance-1 variants (delete/substitute/insert over the call alphabet) are
// generated and looked up in the set, which is far cheaper than scanning a
// 100k-entry list; the correction is accepted only when exactly one distinct
// entry matches, so an ambiguous near-call is never silently changed.
std::string lookupCall(const std::string& tok, const CallSet& db, bool& known) {
    known = false;
    if (db.count(tok)) { known = true; return tok; }
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/";
    CallSet hits;
    std::string v;
    for (std::size_t i = 0; i < tok.size(); ++i) {       // deletions
        v = tok; v.erase(i, 1);
        if (db.count(v)) hits.insert(v);
    }
    for (std::size_t i = 0; i < tok.size(); ++i)          // substitutions
        for (const char* a = alpha; *a; ++a) {
            if (*a == tok[i]) continue;
            v = tok; v[i] = *a;
            if (db.count(v)) hits.insert(v);
        }
    for (std::size_t i = 0; i <= tok.size(); ++i)         // insertions
        for (const char* a = alpha; *a; ++a) {
            v = tok; v.insert(v.begin() + i, *a);
            if (db.count(v)) hits.insert(v);
        }
    if (hits.size() == 1) { known = true; return *hits.begin(); }
    return tok;  // not in the DB and no unambiguous correction — keep the guess
}

// The last whitespace-delimited token of `text`, if it looks like a callsign
// (letters + digits, 3..10 chars, at least one of each, only [A-Z0-9/]). When a
// master-callsign set is supplied it is also validated/corrected against it and
// `known` reports whether the result is confirmed in the list.
std::string callsignIn(const std::string& text, const CallSet* db, bool& known) {
    known = false;
    std::size_t end = text.find_last_not_of(' ');
    if (end == std::string::npos)
        return {};
    std::size_t start = text.find_last_of(' ', end);
    start = (start == std::string::npos) ? 0 : start + 1;
    const std::string tok = text.substr(start, end - start + 1);
    if (tok.size() < 3 || tok.size() > 10)
        return {};
    bool hasDigit = false, hasAlpha = false;
    for (char c : tok) {
        if (c >= '0' && c <= '9')
            hasDigit = true;
        else if (c >= 'A' && c <= 'Z')
            hasAlpha = true;
        else if (c != '/')
            return {};
    }
    if (!(hasDigit && hasAlpha))
        return {};
    if (db && !db->empty())
        return lookupCall(tok, *db, known);
    return tok;  // no DB: heuristic match only, never "known"
}

// Frames of look-ahead for the per-channel ON/OFF Viterbi. The most-probable
// keying path for a frame is fixed once this much later evidence is in.
constexpr int kViterbiLag = 32;

// Per-carrier decoder state, keyed by its (stationary) FFT bin, which is also
// the stable id the UI uses to update one row per signal.
struct Channel {
    int    bin = 0;

    // Soft, most-probable keying segmentation (à la CW Skimmer's "compute the
    // probability the signal is present" rather than a hard per-sample decision):
    // a fixed-lag 2-state (key down/up) Viterbi over per-frame tone-present
    // probabilities. The path-transition cost rejects noise blips and rides
    // through fades without a Schmitt trigger or debounce hacks.
    float  dOn = 0.0f, dOff = 0.0f;           // running path log-probs (normalised)
    std::array<bool, kViterbiLag> fromOn{};   // ON-node predecessor was ON? (ring)
    std::array<bool, kViterbiLag> fromOff{};  // OFF-node predecessor was ON? (ring)
    long   vframes  = 0;                       // frames fed to the Viterbi
    bool   curState = false;                   // last finalised key state (down=true)
    int    runHops  = 0;                       // finalised frames in the current state
    bool   started  = false;                   // a finalised run is in progress
    unsigned __int128 keyHist = 0;             // last 128 finalised key states (1 bit/frame);
                                               // compared across channels to spot a ghost
                                               // (an image / intermod / codec artifact keys
                                               // in lockstep with its parent at another pitch)

    // Per-channel narrowband receiver: complex down-conversion to baseband + a
    // 2-stage low-pass. The keying envelope's time resolution comes from the
    // low-pass, not the FFT window, so a fine FFT can resolve closely-spaced
    // carriers while each envelope stays sharp — this is what lets a crowded
    // band decode in parallel instead of merging into a few channels.
    double loRe = 1.0, loIm = 0.0;   // local-oscillator phasor at the carrier
    double rotRe = 1.0, rotIm = 0.0; // per-sample LO rotation (from the bin freq)
    double f1Re = 0.0, f1Im = 0.0;   // low-pass stage 1
    double f2Re = 0.0, f2Im = 0.0;   // low-pass stage 2
    double noisePow = 0.0;           // gently-tracked key-up power; the decode threshold
                                     // denominator (kept compressed for clean keying)
    double snrFloor = 0.0;           // aggressively-tracked true noise floor, for the SNR
                                     // gate only — reaches the real floor so peak/floor is a
                                     // wide, meaningful SNR (separate from the decode noise)
    double peakPow  = 0.0;           // tracked key-down power; a ghost is an attenuated
                                     // artifact, so the stronger of two ghosts is the real one

    // Adaptive unit references (hops), separate for marks vs gaps because the
    // finite window inflates marks and shrinks gaps. Each tracks the *shortest*
    // recurring interval — a dit / an element gap — moving down fast and leaking
    // up slowly, so it locks onto the true unit within a couple of dits even when
    // the first element is a dah (robust where a 2-means clustering collapses).
    double markUnit = 0.0;
    double gapUnit  = 0.0;
    double dahUnit  = 0.0;    // adaptive dah-mark length (hops), the SOM's "3-unit"
                              // reference; learned from dahs because the finite
                              // window compresses the measured dah:dit ratio below 3

    std::string symbol;       // hard dit/dash classification (exact-lookup fallback)
    std::vector<float> elemHops;  // raw mark durations (hops) of the char in progress,
                                  // for the duration-vector (SOM) character match
    std::string text;         // rolling decoded text
    std::string call;
    int    richChars = 0;     // count of decoded multi-element chars (real-CW evidence)
    bool   charDone = false;  // current gap already closed the character?
    bool   wordDone = false;  // current gap already added a word space?
    int    idleHops = 0;      // finalised quiet frames since the last character
    int    carrierGone = 0;   // frames since the carrier peak was clearly present;
                              // a robust transmission-boundary signal (noise-immune)
    int    wpm = 0;
    bool   shown = false;     // has onChannel been emitted for this id?
    bool   dirty = false;     // text/wpm/call changed
    bool   callKnown = false; // ch.call is confirmed in the master-callsign DB
};

void appendChar(Channel& ch, char c, const CallSet* db) {
    ch.text.push_back(c);
    if (ch.text.size() > 100)   // keep only the last 100 decoded chars in the UI
        ch.text.erase(0, ch.text.size() - 100);
    bool known = false;
    if (std::string call = callsignIn(ch.text, db, known); !call.empty()) {
        ch.call = call;
        ch.callKnown = known;
    }
}

void closeSymbol(Channel& ch, const CallSet* db) {
    if (!ch.elemHops.empty()) {
        if (char c = decodeChar(ch.elemHops, ch.symbol, ch.markUnit, ch.dahUnit,
                                ch.richChars >= 2)) {
            appendChar(ch, c, db);
            // A valid character of two or more elements is evidence of real CW —
            // a lone band-noise burst only ever makes a one-element 'E' or 'T'.
            if (ch.elemHops.size() >= 2)
                ++ch.richChars;
        }
        ch.symbol.clear();
        ch.elemHops.clear();
        ch.dirty = true;
    }
}

// Min-track a unit reference toward the shortest recurring interval: jump down
// fast (a dit / element gap), leak up slowly (a slow-down). Resists outliers and
// survives a dah-first cold start where a 2-means clustering would collapse.
void unitTrack(double& ref, double x) {
    if (ref <= 0.0) ref = x;
    else if (x < ref) ref += 0.4 * (x - ref);
    else ref += 0.02 * (x - ref);
}

// A finalised mark (key-down run) just ended. Record its raw duration for the
// SOM character match, and hard-classify it (dit/dah) to maintain the dit and dah
// length references and the fallback string. The hard call is only for reference
// tracking + fallback; the actual character decision is the SOM's (decodeChar).
void decodeMark(Channel& ch, int durHops, double hopMs) {
    const double d = std::max(1, durHops);
    const bool dah = ch.markUnit > 0.0 && d > 1.8 * ch.markUnit;
    if (!dah) {
        unitTrack(ch.markUnit, d);             // dits define the dit length
    } else {
        if (ch.markUnit <= 0.0) ch.markUnit = d;  // first-ever mark seeds even if a dah
        // Track the dah length toward the dah cluster (EMA; the SOM's "3-unit" ref).
        ch.dahUnit = ch.dahUnit <= 0.0 ? d : ch.dahUnit + 0.2 * (d - ch.dahUnit);
    }
    ch.symbol.push_back(dah ? '-' : '.');
    ch.elemHops.push_back(static_cast<float>(d));
    const double unit = (ch.markUnit + (ch.gapUnit > 0 ? ch.gapUnit : ch.markUnit)) / 2.0;
    ch.wpm = std::clamp(static_cast<int>(std::lround(
                 1200.0 / std::max(1.0, unit * hopMs))), 5, 80);
    ch.dirty = ch.dirty || ch.shown;  // wpm shown live
}

// Called each finalised key-up frame: as the silence grows past the
// inter-character then word thresholds, close the pending character and add a
// space — once each. Doing this *during* the gap (not when the next mark starts)
// means the last character of a transmission is emitted instead of being lost.
void growGap(Channel& ch, int gapHops, const CallSet* db) {
    if (ch.markUnit <= 0.0) return;  // no unit reference yet (lead-in)
    const double gu = ch.gapUnit > 0.0 ? ch.gapUnit : ch.markUnit;
    const double units = gapHops / gu;
    if (!ch.charDone && units >= 2.0) {  // inter-character gap
        closeSymbol(ch, db);
        ch.charDone = true;
    }
    if (!ch.wordDone && units >= 6.0 && !ch.text.empty() && ch.text.back() != ' ') {
        appendChar(ch, ' ', db);
        ch.wordDone = true;
        ch.dirty = true;
    }
}

}  // namespace

// -----------------------------------------------------------------------------

CwSkimmer::~CwSkimmer() {
    onWaterfall      = nullptr;
    onChannel        = nullptr;
    onChannelRemoved = nullptr;
    stop();
}

void CwSkimmer::start(const SkimmerConfig& cfg) {
    stop();
    alive_ = std::make_shared<bool>(true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.clear();
        rate_ = cfg.sampleRate;
    }
    running_.store(true);
    thread_ = std::thread(&CwSkimmer::worker, this, cfg);
}

void CwSkimmer::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable())
            thread_.join();
        return;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    std::lock_guard<std::mutex> lk(mu_);
    queue_.clear();
    rate_ = 0;
}

std::size_t CwSkimmer::loadCallsignDb(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return 0;
    auto set = std::make_shared<CallSet>();
    std::string line;
    while (std::getline(in, line)) {
        // One call per line; tolerate comments/headers and stray whitespace or
        // comma separators. Uppercase and keep only valid call characters.
        for (char& c : line) c = static_cast<char>(std::toupper((unsigned char)c));
        std::size_t i = 0;
        while (i < line.size()) {
            if (line[i] == '#') break;  // comment to end of line
            std::size_t j = i;
            while (j < line.size()) {
                const char c = line[j];
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/') ++j;
                else break;
            }
            if (j > i) {
                const std::string tok = line.substr(i, j - i);
                if (tok.size() >= 3 && tok.size() <= 12)
                    set->insert(tok);
            }
            i = (j > i) ? j : i + 1;
        }
    }
    {
        std::lock_guard<std::mutex> lk(dbMu_);
        callDb_ = set;
    }
    return set->size();
}

bool CwSkimmer::hasCallsignDb() const {
    std::lock_guard<std::mutex> lk(dbMu_);
    return callDb_ && !callDb_->empty();
}

void CwSkimmer::pushPcm(const std::int16_t* samples, int frames, int channels,
                        int sampleRate) {
    if (!running_.load() || frames <= 0 || channels <= 0)
        return;
    std::lock_guard<std::mutex> lk(mu_);
    if (sampleRate != rate_)
        return;  // mismatched stream; the shell restarts us on a rate change
    // Bound the backlog so a stalled worker can't grow memory without limit
    // (~2 s of audio); drop the oldest if we are already that far behind.
    const std::size_t cap = static_cast<std::size_t>(rate_) * 2;
    if (queue_.size() > cap)
        queue_.erase(queue_.begin(), queue_.end() - cap / 2);
    queue_.reserve(queue_.size() + frames);
    for (int i = 0; i < frames; ++i) {
        int acc = 0;
        for (int c = 0; c < channels; ++c)
            acc += samples[i * channels + c];
        queue_.push_back(static_cast<float>(acc) / (channels * 32768.0f));
    }
    cv_.notify_one();
}

void CwSkimmer::worker(SkimmerConfig cfg) {
    // CW only lives in the low audio (<= ~4 kHz), so a high-rate stream is wasted
    // work: decimate to ~12 kHz first and run the whole pipeline there. 12 kHz
    // lands the FFT on 512 pts with the *same* ~23 Hz bins / ~43 ms window as
    // 48 kHz/2048 — identical resolution and timing — but the FFT is 4x smaller
    // and the per-channel narrowband receivers (the dominant cost: one complex
    // filter per channel per sample) run at a quarter of the rate.
    const int decim = (cfg.sampleRate > 12000 && cfg.sampleRate % 12000 == 0)
                          ? cfg.sampleRate / 12000 : 1;
    const int rate  = cfg.sampleRate / decim;   // working sample rate
    // Anti-alias FIR (windowed sinc) applied before downsampling; cutoff just
    // below the new Nyquist so nothing folds into the band we keep.
    std::vector<float> aaTaps;
    if (decim > 1) {
        const int n = 8 * decim + 1;
        const double fc = 0.45 / decim;   // cutoff, normalised to the input rate
        const int mid = n / 2;
        double sum = 0.0;
        aaTaps.resize(n);
        for (int i = 0; i < n; ++i) {
            const double t = i - mid;
            const double sinc = (t == 0.0) ? 2.0 * fc
                                           : std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
            const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (n - 1));  // Hann
            aaTaps[i] = static_cast<float>(sinc * w);
            sum += aaTaps[i];
        }
        for (float& v : aaTaps) v /= static_cast<float>(sum);  // unity DC gain
    }
    std::vector<float> aaHist;   // input-sample history carried across chunks

    // A fine FFT (~512 pts @ 12 kHz → ~23 Hz bins) is used only for the waterfall
    // and channel detection, so its long window doesn't hurt timing: each
    // channel's keying envelope comes from its own narrowband receiver (below),
    // not the FFT. hop is fftSize/8 (~5 ms steps) for responsive envelope sampling.
    const std::size_t fftSize = nextPow2(rate * 0.043);
    const std::size_t hop     = std::max<std::size_t>(32, fftSize / 8);
    // 2-stage low-pass coefficient for the per-channel receiver (~70 Hz cutoff:
    // passes CW keying sidebands, rejects carriers a few bins away).
    const double lpA = 1.0 - std::exp(-2.0 * M_PI * 70.0 / rate);
    const double      hopMs   = 1000.0 * hop / rate;
    const double      binHz   = static_cast<double>(rate) / fftSize;

    const double maxHz = std::min(cfg.maxHz, rate / 2.0 - binHz);
    const int kMin = std::max<int>(1, static_cast<int>(cfg.minHz / binHz));
    const int kMax = std::min<int>(static_cast<int>(fftSize / 2) - 1,
                                   static_cast<int>(maxHz / binHz));
    const double dispMinHz = kMin * binHz;
    const double dispMaxHz = kMax * binHz;

    const int    removeAfterHops = static_cast<int>(30000.0 / hopMs);  // drop after 30 s idle
    // After this much continuous silence the next keying is a new transmission —
    // likely a different op on the same frequency — so the channel re-learns its
    // speed and power from scratch instead of carrying the previous station's.
    const int    newStationHops = static_cast<int>(3000.0 / hopMs);    // 3 s gap
    const int    spawnPersist = 3;     // frames a peak must persist before a channel spawns
    // Channels must be at least the per-channel receiver's bandwidth apart, or two
    // of them sit inside one carrier's passband and decode it identically (the
    // "same signal on several frequencies" duplicate). ~2x the 70 Hz low-pass.
    const int    minSepBins   = std::max(4, static_cast<int>(150.0 / binHz));
    const std::size_t kMaxChannels = 40;

    // Soft tone-present model + Viterbi transition cost.
    const double kZ0      = 6.0;       // SNR (dB) at which P(tone) = 0.5
    const double kZs      = 4.0;       // logistic softness (dB)
    const double kLogStay = std::log(0.94);   // 2-state HMM self-transition
    const double kLogFlip = std::log(0.06);   // ...and key-state flip

    // Hann window for the STFT.
    std::vector<float> window(fftSize);
    for (std::size_t i = 0; i < fftSize; ++i)
        window[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * i / (fftSize - 1));

    std::vector<float> buf;            // accumulated mono samples
    std::vector<float> re(fftSize), im(fftSize);
    std::vector<float> mag(fftSize / 2 + 1);
    std::map<int, Channel> channels;   // id (original bin) -> state
    std::map<int, int>     cand;       // candidate peak bin -> consecutive-frame count

    // Target waterfall row rate (rows/s); the divisor sets the scroll speed.
    int waterfallDecim = std::max(1, static_cast<int>((rate / hop) / 38.0));
    int sinceEmit = 0;
    std::vector<float> rowMax(kDisplayCols, 0.0f);

    while (running_.load()) {
        std::vector<float> chunk;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load())
                break;
            chunk.swap(queue_);
        }
        // Grab the current master-callsign set + Paranoid flag once per chunk
        // (cheap shared_ptr copy + atomic read), so a mid-run loadCallsignDb()
        // swap is picked up without locking inside the per-character decode.
        std::shared_ptr<const CallSet> db;
        {
            std::lock_guard<std::mutex> lk(dbMu_);
            db = callDb_;
        }
        const CallSet* dbp = db ? db.get() : nullptr;
        const bool knownOnly = knownOnly_.load(std::memory_order_relaxed);
        if (decim == 1) {
            buf.insert(buf.end(), chunk.begin(), chunk.end());
        } else {
            // Anti-alias filter + downsample by `decim`, carrying the FIR history
            // across chunks so the decimation phase stays continuous.
            aaHist.insert(aaHist.end(), chunk.begin(), chunk.end());
            const std::size_t n = aaTaps.size();
            std::size_t i = 0;
            for (; i + n <= aaHist.size(); i += decim) {
                float acc = 0.0f;
                for (std::size_t j = 0; j < n; ++j)
                    acc += aaTaps[j] * aaHist[i + j];
                buf.push_back(acc);
            }
            aaHist.erase(aaHist.begin(), aaHist.begin() + i);
        }

        std::size_t pos = 0;
        while (buf.size() - pos >= fftSize) {
            for (std::size_t i = 0; i < fftSize; ++i) {
                re[i] = buf[pos + i] * window[i];
                im[i] = 0.0f;
            }
            fft(re, im);
            for (int k = kMin; k <= kMax; ++k)
                mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);

            // Noise floor: the 30th-percentile magnitude across the passband.
            static thread_local std::vector<float> sortbuf;
            sortbuf.assign(mag.begin() + kMin, mag.begin() + kMax + 1);
            const std::size_t pIdx = sortbuf.size() / 3;
            std::nth_element(sortbuf.begin(), sortbuf.begin() + pIdx, sortbuf.end());
            const float  floor    = std::max(1e-6f, sortbuf[pIdx]);
            // Operator gating level (dB): a squelch on channel creation. Raising it
            // requires a stronger peak above the noise floor to start a channel, so
            // weak signals, noise and ghosts are not picked up — but a channel that
            // does clear it decodes at full sensitivity (the per-frame keying gate
            // below is left at its default, so the decode quality is unaffected).
            const double gate     = gateDb_.load(std::memory_order_relaxed);
            const float  gateLin  = static_cast<float>(std::pow(10.0, gate / 20.0));
            const float  spawnTh  = floor * 7.0f * gateLin;  // peak strength to start a channel
            const double minSnr   = minSnrDb_.load(std::memory_order_relaxed);  // per-channel gate

            // 1) Spawn channels at strong local-maximum peaks, but only after the
            //    peak has persisted (rejects keying-transient clicks and noise) and
            //    not too close to an existing channel (one carrier = one channel).
            auto nearChannel = [&](int k) {
                for (auto& [id, ch] : channels)
                    if (std::abs(ch.bin - k) < minSepBins)
                        return true;
                return false;
            };
            // k is an audio harmonic of an existing carrier if a stronger channel
            // sits near k/2, k/3 or k/4 — suppress it (a real second carrier would
            // not be locked to an integer multiple of another's frequency).
            auto isHarmonic = [&](int k) {
                for (int n = 2; n <= 4; ++n) {
                    const int kf = (k + n / 2) / n;
                    for (auto& [id, ch] : channels)
                        if (std::abs(ch.bin - kf) <= 1 && mag[ch.bin] >= mag[k])
                            return true;
                }
                return false;
            };
            std::map<int, int> nextCand;
            if (channels.size() < kMaxChannels) {
                for (int k = kMin + 1; k < kMax; ++k) {
                    if (mag[k] < spawnTh)
                        continue;
                    // Require the dominant peak over ±(minSep-1) bins, not just a
                    // local max, so the FFT skirts of a strong carrier don't spawn
                    // spurious channels between real signals.
                    bool peak = true;
                    for (int j = -(minSepBins - 1); j <= minSepBins - 1; ++j) {
                        const int n = k + j;
                        if (n >= kMin && n <= kMax && mag[n] > mag[k]) { peak = false; break; }
                    }
                    if (!peak)
                        continue;
                    if (nearChannel(k) || isHarmonic(k))
                        continue;
                    const int age = (cand.count(k) ? cand[k] : 0) + 1;
                    if (age >= spawnPersist) {
                        Channel ch;
                        ch.bin = k;
                        const double omega = -2.0 * M_PI * (k * binHz) / rate;
                        ch.rotRe = std::cos(omega);  // LO at the carrier frequency
                        ch.rotIm = std::sin(omega);
                        channels.emplace(k, std::move(ch));
                    } else {
                        nextCand[k] = age;  // keep accumulating
                    }
                }
            }
            cand.swap(nextCand);  // candidates not seen this frame reset to zero

            // 2) For each channel: a soft tone-present probability, a fixed-lag
            //    Viterbi that picks the most-probable keying segmentation, and a
            //    duration-cluster Morse decode of the finalised runs.
            for (auto it = channels.begin(); it != channels.end();) {
                Channel& ch = it->second;

                // Carrier-presence tracking off the FFT peak (noise-immune, unlike the
                // decoded key-state): a real op keys a clear peak every dit/dah, so the
                // counter stays low through a transmission and only climbs in true
                // silence. After a transmission's worth of silence, reset the
                // per-station adaptive state once so the next op on this frequency is
                // learned fresh (speed + power), instead of carrying the previous
                // station's references (which would garble a slower/weaker op).
                if (mag[ch.bin] > spawnTh)
                    ch.carrierGone = 0;
                else
                    ++ch.carrierGone;
                if (ch.carrierGone == newStationHops) {
                    ch.markUnit = ch.gapUnit = ch.dahUnit = 0.0;
                    ch.peakPow  = ch.snrFloor;
                    ch.call.clear();
                    ch.callKnown = false;
                }

                // Narrowband receiver: down-convert this hop's samples to baseband
                // at the carrier, 2-stage low-pass, and take the output power as
                // the keying envelope. The low-pass (not the FFT window) sets the
                // time resolution, so the envelope stays sharp even with fine bins.
                double P = 0.0;
                for (std::size_t i = 0; i < hop; ++i) {
                    const double x = buf[pos + i];
                    const double bRe = x * ch.loRe, bIm = x * ch.loIm;
                    const double nlo = ch.loRe * ch.rotRe - ch.loIm * ch.rotIm;
                    ch.loIm = ch.loRe * ch.rotIm + ch.loIm * ch.rotRe;
                    ch.loRe = nlo;
                    ch.f1Re += lpA * (bRe - ch.f1Re);  ch.f1Im += lpA * (bIm - ch.f1Im);
                    ch.f2Re += lpA * (ch.f1Re - ch.f2Re);  ch.f2Im += lpA * (ch.f1Im - ch.f2Im);
                    P = ch.f2Re * ch.f2Re + ch.f2Im * ch.f2Im;
                }
                const double lm = std::sqrt(ch.loRe * ch.loRe + ch.loIm * ch.loIm);
                if (lm > 1e-9) { ch.loRe /= lm; ch.loIm /= lm; }  // keep the phasor unit-length

                // Per-channel noise floor: fall fast so it reaches the true floor
                // within a key-up gap, rise very slowly so a held mark barely lifts
                // it. A floor that doesn't settle would compress the SNR and blur
                // strong vs weak signals.
                if (ch.noisePow <= 0.0) ch.noisePow = P;
                else if (P < ch.noisePow) ch.noisePow += 0.1 * (P - ch.noisePow);
                else ch.noisePow += 0.002 * (P - ch.noisePow);
                // Separate true-floor tracker for the SNR gate: fall fast to the real
                // floor in a key-up gap, rise very slowly so a held mark barely lifts
                // it — a settled floor makes peak/floor a wide, honest SNR.
                if (ch.snrFloor <= 0.0) ch.snrFloor = P;
                else if (P < ch.snrFloor) ch.snrFloor += 0.5 * (P - ch.snrFloor);
                else ch.snrFloor += 0.0003 * (P - ch.snrFloor);
                // Track the key-down level (fast up, slow down) as a strength proxy
                // (the per-channel SNR numerator, and the ghost-dedup tiebreak).
                if (P > ch.peakPow) ch.peakPow = P;
                else ch.peakPow += 0.001 * (P - ch.peakPow);
                const double zdb = 10.0 * std::log10((P + 1e-12) / (ch.noisePow + 1e-12));
                // Soft tone-present probability — never a hard per-frame decision.
                double pOn = 1.0 / (1.0 + std::exp(-(zdb - kZ0) / kZs));
                // Min-SNR gate on the decoder: a channel whose characteristic SNR
                // (its keyed peak level vs its own noise floor) is below the floor is
                // not decoded at all — its keying is forced to key-up (silence), so
                // only signals over the SNR are considered. Channels above the floor
                // keep the original, well-tuned per-frame dynamics, so raising the
                // slider never degrades the decode of the signals it keeps.
                const double chSnr = 10.0 * std::log10((ch.peakPow + 1e-12) /
                                                       (ch.snrFloor + 1e-12));
                if (chSnr < minSnr)
                    pOn = 0.0;
                const double eOn  = std::log(std::max(1e-4, pOn));
                const double eOff = std::log(std::max(1e-4, 1.0 - pOn));

                // Viterbi step for the two key states (down / up).
                const double onFromOn  = ch.dOn + kLogStay,  onFromOff = ch.dOff + kLogFlip;
                const bool   onPrevOn  = onFromOn >= onFromOff;
                const double offFromOff = ch.dOff + kLogStay, offFromOn = ch.dOn + kLogFlip;
                const bool   offPrevOn = offFromOn > offFromOff;
                const double nOn  = eOn  + (onPrevOn  ? onFromOn  : onFromOff);
                const double nOff = eOff + (offPrevOn ? offFromOn : offFromOff);
                const double norm = std::max(nOn, nOff);
                ch.dOn  = static_cast<float>(nOn  - norm);
                ch.dOff = static_cast<float>(nOff - norm);
                const int slot = static_cast<int>(ch.vframes % kViterbiLag);
                ch.fromOn[slot]  = onPrevOn;
                ch.fromOff[slot] = offPrevOn;
                ++ch.vframes;

                // Fixed-lag finalise: the keying of the frame kViterbiLag ago is
                // settled — backtrack the best path to it and decode that run.
                if (ch.vframes >= kViterbiLag) {
                    bool st = ch.dOn >= ch.dOff;
                    long f = ch.vframes - 1;
                    for (int s = 0; s < kViterbiLag - 1; ++s) {
                        st = st ? ch.fromOn[f % kViterbiLag] : ch.fromOff[f % kViterbiLag];
                        --f;
                    }
                    if (!ch.started) {
                        ch.started = true;
                        ch.curState = st;
                        ch.runHops = 1;
                    } else if (st == ch.curState) {
                        ++ch.runHops;
                        if (!ch.curState) growGap(ch, ch.runHops, dbp);  // close char/word as silence grows
                    } else if (st) {                 // gap -> mark: the gap just ended
                        const double gu = ch.gapUnit > 0.0 ? ch.gapUnit : ch.markUnit;
                        if (ch.markUnit > 0.0 && ch.runHops < 1.8 * gu)
                            unitTrack(ch.gapUnit, ch.runHops);  // an element gap defines the gap unit
                        // The element gap and the dit are the same unit (measured
                        // ratio ~1.0), so keep gapUnit anchored to markUnit. Without
                        // this, one glitch-short gap min-tracks gapUnit too low; real
                        // element gaps then exceed 1.8*gapUnit, so they neither update
                        // it (it can't climb back) nor read as element gaps — every
                        // gap then looks like a character gap and the text decodes as
                        // a string of one-element E's and T's.
                        if (ch.markUnit > 0.0)
                            ch.gapUnit = std::clamp(ch.gapUnit, 0.7 * ch.markUnit,
                                                                1.3 * ch.markUnit);
                        ch.curState = true;
                        ch.runHops = 1;
                    } else {                          // mark -> gap: the mark just ended
                        decodeMark(ch, ch.runHops, hopMs);
                        ch.curState = false;
                        ch.runHops = 1;
                        ch.charDone = ch.wordDone = false;  // a fresh gap begins
                    }
                    if (!ch.curState && ch.symbol.empty()) ++ch.idleHops;
                    else ch.idleHops = 0;
                    ch.keyHist = (ch.keyHist << 1) | static_cast<unsigned __int128>(st ? 1 : 0);
                }

                // Drop on long silence; under Paranoid (known-only), also drop a
                // shown channel that has gone quiet (~new-station gap) without ever
                // confirming a callsign — so the list stays calls-in-DB only.
                if (ch.idleHops > removeAfterHops ||
                    (knownOnly && ch.shown && !ch.callKnown && ch.idleHops > newStationHops)) {
                    if (ch.shown)
                        postRemoved(it->first);
                    it = channels.erase(it);
                    continue;
                }
                // Surface a channel only once it has shown real-CW structure (a
                // couple of multi-element characters); a noise burst that only ever
                // makes a lone 'E'/'T' never qualifies and stays hidden. Below the
                // min-SNR floor the keying isn't decoded at all (see above), so a
                // weak channel produces no characters and never surfaces here. Under
                // Paranoid, hold a channel back until its callsign is DB-confirmed.
                const bool eligible = ch.richChars >= 2 && (!knownOnly || ch.callKnown);
                if (ch.shown || eligible) {
                    if (!ch.shown || ch.dirty) {
                        ch.shown = true;
                        ch.dirty = false;
                        postChannel(it->first, ch.bin * binHz, ch.wpm, ch.text, ch.call);
                    }
                }
                ++it;
            }

            // 2b) De-duplicate. Two channels are the same signal if they: sit within
            //     a receiver bandwidth of each other; decode the same callsign (a
            //     harmonic at any separation); or key in lockstep — a ghost (an
            //     image, intermod product or codec artifact) reproduces its parent's
            //     exact on/off timing at a different pitch, so the recent keying
            //     histories overlap almost perfectly (Jaccard of the on-bits), which
            //     independent signals never do. Keep the cleaner copy (more valid
            //     characters, then more text); collect first, then erase.
            std::vector<int> toDrop;
            for (auto a = channels.begin(); a != channels.end(); ++a)
                for (auto b = std::next(a); b != channels.end(); ++b) {
                    const Channel& A = a->second;
                    const Channel& B = b->second;
                    bool dup = std::abs(A.bin - B.bin) < minSepBins ||
                               (!A.call.empty() && A.call == B.call);
                    if (!dup) {
                        const int inter = popcount128(A.keyHist & B.keyHist);
                        const int uni   = popcount128(A.keyHist | B.keyHist);
                        // Enough shared keying activity, and they're on together for
                        // most of it — same Morse, so one is a ghost of the other.
                        // Independent signals overlap ~25% (different timing) even
                        // sending the same text; a ghost is ~90%, so 65% has margin
                        // both ways and tolerates a sloppy (partial) ghost.
                        if (uni >= 24 && inter * 100 >= 65 * uni)
                            dup = true;
                    }
                    if (dup) {
                        // Keep the stronger signal (a ghost is the attenuated copy);
                        // fall back to the cleaner/longer decode if strengths tie.
                        bool keepA;
                        if (A.peakPow > B.peakPow * 1.15) keepA = true;
                        else if (B.peakPow > A.peakPow * 1.15) keepA = false;
                        else keepA = A.richChars != B.richChars
                                         ? A.richChars > B.richChars
                                         : A.text.size() >= B.text.size();
                        toDrop.push_back(keepA ? b->first : a->first);
                    }
                }
            for (int id : toDrop) {
                auto it = channels.find(id);
                if (it == channels.end())
                    continue;
                if (it->second.shown)
                    postRemoved(id);
                channels.erase(it);
            }

            // Waterfall level trim (dB) subtracted from the whole display. It folds
            // two pieces, both per-frame so a live filter or settings change takes
            // effect at once (a few cheap atomic reads + a log2):
            //   - a constant baseline offset (bwOffsetDb_), applied first — a positive
            //     value dims and a negative value brightens the whole waterfall;
            //   - then a per-bandwidth dimming that grows as the rig filter narrows
            //     below the reference, holding the floor put across filter changes
            //     (see setBandwidthNorm).
            float bwOffDb = static_cast<float>(bwOffsetDb_.load(std::memory_order_relaxed));
            {
                const int bwDb  = bwNormDb_.load(std::memory_order_relaxed);
                const int bwRef = bwNormRefHz_.load(std::memory_order_relaxed);
                const int bwHz  = filterBwHz_.load(std::memory_order_relaxed);
                if (bwDb > 0 && bwRef > 0 && bwHz > 0 && bwHz < bwRef)
                    bwOffDb += std::clamp(
                        static_cast<float>(bwDb) *
                            static_cast<float>(std::log2(static_cast<double>(bwRef) / bwHz)),
                        0.0f, 45.0f);
            }

            // 3) Fold this frame into the pending waterfall row (max-pooled).
            for (int c = 0; c < kDisplayCols; ++c) {
                const int k0 = kMin + (kMax - kMin) * c / kDisplayCols;
                const int k1 = std::max(k0 + 1, kMin + (kMax - kMin) * (c + 1) / kDisplayCols);
                float m = 0.0f;
                for (int k = k0; k < k1 && k <= kMax; ++k)
                    m = std::max(m, mag[k]);
                const float db = 20.0f * std::log10(m + 1e-6f);
                const float fl = 20.0f * std::log10(floor + 1e-6f);
                rowMax[c] = std::max(rowMax[c],
                                     std::clamp((db - fl - bwOffDb) / 45.0f, 0.0f, 1.0f));
            }
            if (++sinceEmit >= waterfallDecim) {
                postWaterfall(rowMax, dispMinHz, dispMaxHz);
                std::fill(rowMax.begin(), rowMax.end(), 0.0f);
                sinceEmit = 0;
            }

            pos += hop;
        }
        if (pos > 0)
            buf.erase(buf.begin(), buf.begin() + pos);
    }
}

void CwSkimmer::postWaterfall(std::vector<float> mags, double minHz, double maxHz) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), mags = std::move(mags), minHz, maxHz]() {
        if (!w.expired() && onWaterfall) onWaterfall(mags, minHz, maxHz);
    });
}

void CwSkimmer::postChannel(int id, double hz, int wpm, std::string text, std::string call) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), id, hz, wpm,
              text = std::move(text), call = std::move(call)]() {
        if (!w.expired() && onChannel) onChannel(id, hz, wpm, text, call);
    });
}

void CwSkimmer::postRemoved(int id) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), id]() {
        if (!w.expired() && onChannelRemoved) onChannelRemoved(id);
    });
}
