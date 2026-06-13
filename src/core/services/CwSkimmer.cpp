#include "CwSkimmer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

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

char decodeSymbol(const std::string& sym) {
    const auto& t = morseTable();
    auto it = t.find(sym);
    return it == t.end() ? '\0' : it->second;
}

// Does the last whitespace-delimited token of `text` look like a callsign?
// (Letters + digits, 3..10 chars, at least one of each, only [A-Z0-9/].)
std::string callsignIn(const std::string& text) {
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
    return (hasDigit && hasAlpha) ? tok : std::string{};
}

// Per-carrier decoder state, keyed by its (stationary) FFT bin, which is also
// the stable id the UI uses to update one row per signal.
struct Channel {
    int    bin       = 0;
    bool   rawDown   = false; // instantaneous Schmitt state of the level
    bool   keyDown   = false; // debounced (confirmed) key state
    int    pending   = 0;     // frames the raw state has disagreed with keyDown
    int    runHops   = 0;     // hops elapsed in the current (confirmed) key state
    double markUnit  = 0.0;   // adaptive dit-mark length, hops (from dits only)
    double gapUnit   = 0.0;   // adaptive element-gap length, hops (from gaps only)
    std::string symbol;       // elements of the character in progress
    std::string text;         // rolling decoded text
    std::string call;
    int    idleHops  = 0;     // hops since the last decoded character
    int    wpm       = 0;
    bool   shown     = false; // has onChannel been emitted for this id?
    bool   dirty     = false; // text/wpm/call changed this frame
};

void appendChar(Channel& ch, char c) {
    ch.text.push_back(c);
    if (ch.text.size() > 60)
        ch.text.erase(0, ch.text.size() - 60);
    if (std::string call = callsignIn(ch.text); !call.empty())
        ch.call = call;
}

// A completed key-up gap, measured in dit units, closes the current symbol
// and/or inserts spacing.
void closeGap(Channel& ch, double units) {
    if (units >= 2.0 && !ch.symbol.empty()) {  // inter-character (or longer)
        if (char c = decodeSymbol(ch.symbol))
            appendChar(ch, c);
        ch.symbol.clear();
        ch.idleHops = 0;
        ch.dirty = true;
    }
    if (units >= 6.0 && !ch.text.empty() && ch.text.back() != ' ') {  // word gap
        appendChar(ch, ' ');
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
    // The analysis window must be well shorter than a CW dit (54 ms at 22 wpm) or
    // the FFT smears energy across the element gaps — inflating marks and
    // shrinking gaps — and the timing falls apart. ~11 ms (512 pts @ 48 kHz)
    // keeps that distortion small while ~94 Hz bins still separate CW signals.
    const std::size_t fftSize = nextPow2(cfg.sampleRate * 0.008);  // 512 @ 48 kHz (~11 ms)
    const std::size_t hop     = std::max<std::size_t>(32, fftSize / 4);
    const double      hopMs   = 1000.0 * hop / cfg.sampleRate;
    const double      binHz   = static_cast<double>(cfg.sampleRate) / fftSize;

    const double maxHz = std::min(cfg.maxHz, cfg.sampleRate / 2.0 - binHz);
    const int kMin = std::max<int>(1, static_cast<int>(cfg.minHz / binHz));
    const int kMax = std::min<int>(static_cast<int>(fftSize / 2) - 1,
                                   static_cast<int>(maxHz / binHz));
    const double dispMinHz = kMin * binHz;
    const double dispMaxHz = kMax * binHz;

    // Aging / classification limits derived from the hop rate.
    const int    removeAfterHops = static_cast<int>(5000.0 / hopMs);   // 5 s idle
    const int    debounceHops = std::max(2, static_cast<int>(6.0 / hopMs));  // confirm an edge (~6 ms)
    const int    glitchHops   = std::max(debounceHops + 1, static_cast<int>(18.0 / hopMs));  // shorter than any real dit (~45 wpm)
    const int    spawnPersist = 3;     // frames a peak must persist before a channel spawns
    const int    minSepBins   = 4;     // keep channels at least this far apart
    const std::size_t kMaxChannels = 40;

    // Hann window for the STFT.
    std::vector<float> window(fftSize);
    for (std::size_t i = 0; i < fftSize; ++i)
        window[i] = 0.5f - 0.5f * std::cos(2.0 * M_PI * i / (fftSize - 1));

    std::vector<float> buf;            // accumulated mono samples
    std::vector<float> re(fftSize), im(fftSize);
    std::vector<float> mag(fftSize / 2 + 1);
    std::map<int, Channel> channels;   // id (original bin) -> state
    std::map<int, int>     cand;       // candidate peak bin -> consecutive-frame count

    int waterfallDecim = std::max(1, static_cast<int>((cfg.sampleRate / hop) / 38.0));
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
        buf.insert(buf.end(), chunk.begin(), chunk.end());

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
            const float floor = std::max(1e-6f, sortbuf[pIdx]);
            const float onTh    = floor * 4.5f;   // Schmitt-trigger thresholds
            const float offTh   = floor * 2.5f;
            const float spawnTh = floor * 7.0f;   // a much stronger peak to start a channel

            // 1) Spawn channels at strong local-maximum peaks, but only after the
            //    peak has persisted (rejects keying-transient clicks and noise) and
            //    not too close to an existing channel (one carrier = one channel).
            auto nearChannel = [&](int k) {
                for (auto& [id, ch] : channels)
                    if (std::abs(ch.bin - k) < minSepBins)
                        return true;
                return false;
            };
            std::map<int, int> nextCand;
            if (channels.size() < kMaxChannels) {
                for (int k = kMin + 1; k < kMax; ++k) {
                    if (mag[k] < spawnTh || mag[k] < mag[k - 1] || mag[k] < mag[k + 1])
                        continue;
                    if (nearChannel(k))
                        continue;
                    const int age = (cand.count(k) ? cand[k] : 0) + 1;
                    if (age >= spawnPersist) {
                        Channel ch;
                        ch.bin = k;
                        channels.emplace(k, std::move(ch));
                    } else {
                        nextCand[k] = age;  // keep accumulating
                    }
                }
            }
            cand.swap(nextCand);  // candidates not seen this frame reset to zero

            // 2) Advance every channel's keying envelope + Morse decode.
            for (auto it = channels.begin(); it != channels.end();) {
                Channel& ch = it->second;
                // A CW carrier is stationary in the audio passband, so the bin is
                // fixed; read the level as the max over the carrier's ±1-bin width
                // (robust to a tone sitting between two bins) without moving it.
                float level = mag[ch.bin];
                if (ch.bin - 1 >= kMin) level = std::max(level, mag[ch.bin - 1]);
                if (ch.bin + 1 <= kMax) level = std::max(level, mag[ch.bin + 1]);

                // Schmitt-trigger the raw level, then debounce: a state change is
                // committed only after the new state holds for `debounceHops`, so a
                // noise blip can't inject a false element or split a long gap.
                if (!ch.rawDown && level > onTh)
                    ch.rawDown = true;
                else if (ch.rawDown && level < offTh)
                    ch.rawDown = false;

                ++ch.runHops;
                if (ch.rawDown != ch.keyDown) {
                    if (++ch.pending >= debounceHops) {
                        // Confirmed flip: the previous key state's run just ended.
                        // Its true length excludes the `pending` frames already
                        // spent in the new state.
                        const bool wasMark = ch.keyDown;
                        const double dur = std::max(1, ch.runHops - ch.pending);
                        ch.keyDown = ch.rawDown;
                        ch.runHops = ch.pending;
                        ch.pending = 0;

                        // Track the dit-mark and element-gap lengths in SEPARATE
                        // references: the finite FFT window inflates marks and
                        // shrinks gaps, so a shared estimate would never settle.
                        // Each reference min-tracks (jumps down to the shortest
                        // interval — a dit / an element gap — instantly, and leaks
                        // up slowly to follow a slow-down), which locks on within
                        // one dit regardless of whether the first element was a dah.
                        auto minTrack = [](double& ref, double x) {
                            if (ref <= 0.0) ref = x;             // seed
                            else if (x < ref) ref += 0.4 * (x - ref);  // move toward a shorter unit (resists outliers)
                            else ref += 0.02 * (x - ref);        // leak up slowly to follow a slow-down
                        };
                        if (wasMark) {  // a mark ended
                            // Ignore a mark shorter than any real dit: a noise
                            // burst, not an element. An absolute floor (not one
                            // relative to markUnit) so it can't reject genuine dits
                            // while markUnit is still seeded high from a lead dah.
                            if (dur < glitchHops) {
                                ++it;
                                continue;
                            }
                            const bool dah = ch.markUnit > 0.0 && dur > 1.8 * ch.markUnit;
                            if (!dah)
                                minTrack(ch.markUnit, dur);   // only dits define the dit length
                            else if (ch.markUnit <= 0.0)
                                ch.markUnit = dur;            // first-ever mark seeds even if a dah
                            ch.symbol.push_back(dah ? '-' : '.');
                            const double u = (ch.markUnit + (ch.gapUnit > 0 ? ch.gapUnit : ch.markUnit)) / 2;
                            ch.wpm = std::clamp(static_cast<int>(std::lround(
                                         1200.0 / std::max(1.0, u * hopMs))), 5, 80);
                            ch.dirty = ch.dirty || ch.shown;
                        } else if (ch.markUnit > 0.0) {  // a gap ended (ignore the lead-in)
                            const double gu = ch.gapUnit > 0 ? ch.gapUnit : ch.markUnit;
                            const double units = dur / gu;
                            closeGap(ch, units);
                            // Define the gap unit only from real element gaps: short
                            // enough to be one unit, but not a sub-dit fragment left
                            // by a rejected glitch mark (which would collapse it).
                            if (units < 1.8 && dur >= glitchHops)
                                minTrack(ch.gapUnit, dur);
                            else if (ch.gapUnit <= 0.0 && dur >= glitchHops)
                                ch.gapUnit = dur;             // until one is seen, accept the first real gap
                        }
                    }
                } else {
                    ch.pending = 0;
                }

                // Idle accounting + aging (only count once truly quiet).
                if (!ch.keyDown && ch.symbol.empty())
                    ++ch.idleHops;
                else
                    ch.idleHops = 0;
                if (ch.idleHops > removeAfterHops) {
                    if (ch.shown)
                        postRemoved(it->first);
                    it = channels.erase(it);
                    continue;
                }
                // Emit once there is decoded text; thereafter on any change.
                if (ch.dirty || (!ch.shown && !ch.text.empty())) {
                    ch.shown = true;
                    ch.dirty = false;
                    postChannel(it->first, ch.bin * binHz, ch.wpm, ch.text, ch.call);
                }
                ++it;
            }

            // 2b) Merge channels that have drifted onto the same carrier: keep the
            //     one with more decoded text, drop the other. Collect first, then
            //     erase, so the map isn't mutated mid-iteration.
            std::vector<int> toDrop;
            for (auto a = channels.begin(); a != channels.end(); ++a)
                for (auto b = std::next(a); b != channels.end(); ++b)
                    if (std::abs(a->second.bin - b->second.bin) < minSepBins)
                        toDrop.push_back(a->second.text.size() >= b->second.text.size()
                                             ? b->first : a->first);
            for (int id : toDrop) {
                auto it = channels.find(id);
                if (it == channels.end())
                    continue;
                if (it->second.shown)
                    postRemoved(id);
                channels.erase(it);
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
                rowMax[c] = std::max(rowMax[c], std::clamp((db - fl) / 45.0f, 0.0f, 1.0f));
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
