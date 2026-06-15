# The CW Skimmer decoder

This document explains how xlog2's multi-channel CW decoder
(`src/core/services/CwSkimmer.{h,cpp}`) works — the signal-processing pipeline,
the decode algorithm, every tuning constant, the operator controls, and the
known limitations. It is the reference for anyone modifying the decoder.

It is a pragmatic re-implementation of the ideas behind VE3NEA's *CW Skimmer*,
**scaled to a single audio passband** (the rig-audio stream) rather than the
wideband IQ a real SDR skimmer processes. The guiding principle, borrowed
directly from VE3NEA, is **"soft, not hard"**: never make an irreversible
per-sample yes/no decision about whether a tone is present — compute a
*probability* and let a most-probable-path search commit to the keying only once
enough evidence is in.

---

## 1. Where it sits

```
cwsd (rig audio)  --Opus/UDP-->  AudioStreamClient  --onPcm-->  CwSkimmer
                                  (decodes Opus)      (int16 PCM)   |
                                                                    | onWaterfall / onChannel / onChannelRemoved
                                                                    v  (marshalled to the UI thread)
                                                        QtCwSkimmerPanel / CwSkimmerPanel
```

- **Input.** `CwSkimmer::pushPcm()` is called from `AudioStreamClient`'s audio
  worker with decoded int16 PCM (see `Audio.cpp`'s `onPcm` tap). It downmixes to
  mono, appends to a queue, and returns immediately — it never blocks audio
  playback. A bounded backlog (~2 s) is dropped oldest-first if the decode
  worker ever falls behind.
- **Threading.** All DSP runs on `CwSkimmer`'s own worker thread. Results are
  posted to the UI thread through the injected `IUiDispatcher`, so `onWaterfall`,
  `onChannel`, and `onChannelRemoved` always fire on the UI thread. Posted
  closures hold a `weak_ptr` liveness token, so a callback arriving after the
  skimmer is destroyed is dropped (same pattern as the other services).
- **Output.**
  - `onWaterfall(mags, minHz, maxHz)` — one max-pooled spectrum row
    (`kDisplayCols` = 256 values in [0,1]), a few tens of times a second.
  - `onChannel(id, hz, wpm, text, call)` — a decoded signal appeared or changed.
    `id` is stable (the FFT bin); the panels keep one table row per id.
  - `onChannelRemoved(id)` — a channel went idle and was dropped.

The config (`SkimmerConfig`) carries `sampleRate`/`channels` (which **must match
the audio stream**) and the analysed passband `[minHz, maxHz]` (default
250 Hz – 4 kHz; CW lives in the low audio).

---

## 2. The central design problem

A single STFT forces an unavoidable trade-off:

- **Long FFT window → fine frequency bins** (signals close in pitch become
  separate channels) **but a smeared keying envelope** (the window is a
  long time-average, so it blurs dits/gaps and the timing falls apart).
- **Short FFT window → sharp envelope** (clean keying) **but coarse bins**
  (crowded signals merge into a few channels).

Early versions of this decoder hit both failure modes in turn. The fix is a
**two-path design that decouples frequency resolution from envelope timing**:

1. A **fine FFT** does *only* channelization — the waterfall, the noise floor,
   and detecting where carriers are. Its long window is fine here because it is
   never used for timing.
2. Each detected channel then runs its **own narrowband receiver** (a digital
   down-converter + low-pass) whose output power is the keying envelope. Its time
   resolution is set by the low-pass filter, **not** the FFT window — so the
   envelope stays sharp no matter how fine the FFT is.

This is the single most important idea in the decoder. It is what lets a crowded
band decode many stations in parallel instead of collapsing to a handful.

---

## 3. Pipeline, stage by stage

### 3.1 Decimation (efficiency)

CW occupies only the low audio (the band is capped at ~4 kHz), so processing a
48 kHz stream is wasted work. The worker first **decimates to ~12 kHz**: an
anti-alias FIR (windowed-sinc, Hann-tapered, cutoff just under the new Nyquist)
followed by an integer downsample. The FIR history is carried across PCM chunks
so the decimation phase stays continuous.

- Decimation runs only when the input rate is an exact multiple of 12 kHz
  (48→÷4, 24→÷2); otherwise `decim = 1` and the pipeline runs at the native
  rate (e.g. an 8 kHz stream is processed at 8 kHz).
- The whole pipeline then runs at the lower `rate`: the FFT is ¼-size and the
  per-channel receivers run at ¼ the sample rate (the dominant cost), for the
  *same* resolution and timing. Every derived constant (`binHz`, `hopMs`, the
  low-pass coefficient, the DDC oscillator frequency) keys off the working
  `rate`, so the maths is rate-agnostic.

> **Bandwidth note.** Opus stream bandwidth is set by *bitrate*, not sample
> rate — dropping the sample rate saves CPU/audio-bandwidth, not network bytes.
> At low sample rates a high Opus bitrate matters: quantization noise from a low
> bitrate spreads across the band and the multi-channel decoder picks it up as
> spurious channels, so cwsd streams at a generous bitrate.

### 3.2 Sliding STFT → waterfall + noise floor + channel detection

A Hann-windowed FFT is computed every `hop` (= `fftSize/8`, ~5 ms steps) by a
small dependency-free iterative radix-2 `fft()`. `fftSize` is sized for ~43 ms /
~23 Hz bins at the working rate (≈512 pts @ 12 kHz, ≈2048 @ 48 kHz native).

From each FFT frame:

- **Waterfall row.** The passband bins are max-pooled into `kDisplayCols`
  columns, converted to dB above the floor, normalised to [0,1], and accumulated;
  a row is posted every `waterfallDecim` frames (~38/s). An optional
  **bandwidth-normalisation offset** `bwOffDb` is subtracted here first — see §6.1.
- **Noise floor.** The 30th-percentile bin magnitude across the passband — a
  robust estimate that ignores the strong-signal tail.
- **Channel detection (spawn).** A new channel is created at a bin that is:
  - above `spawnTh` (= floor × 7 × the operator **gate**, see §5);
  - the **dominant** peak over ±(`minSepBins`−1) bins (not merely a local max,
    so a strong carrier's FFT skirts don't spawn neighbours);
  - **not a harmonic** of an existing carrier (suppressed if a stronger channel
    sits near k/2, k/3, or k/4);
  - **not within `minSepBins`** of an existing channel; and
  - **persistent** — the candidate peak must survive `spawnPersist` (= 3)
    consecutive frames before it spawns, which rejects keying-click transients
    and noise spikes.

  `minSepBins` is ~150 Hz worth of bins — twice the receiver's low-pass cutoff —
  so two channels can never sit inside one carrier's passband (which would make
  them decode it identically). At most `kMaxChannels` (= 40) exist at once.

### 3.3 Per-channel narrowband receiver → keying envelope

For each channel, every hop of input samples is:

1. **Down-converted** to baseband by multiplying with a complex local oscillator
   tuned to the carrier (`loRe/loIm`, advanced each sample by the per-channel
   rotation phasor `rotRe/rotIm` set from the bin frequency at spawn). The
   oscillator phasor is renormalised each hop to stay unit-length.
2. **Low-pass filtered** by a 2-stage one-pole IIR (`f1*`, `f2*`; cutoff ~70 Hz
   — wide enough to pass CW keying sidebands, narrow enough to reject carriers a
   few bins away).
3. The filtered output **power** `P = f2Re² + f2Im²` is the channel's keying
   envelope for that hop. Because the time resolution comes from the 70 Hz
   filter, this envelope is sharp regardless of the FFT window length.

### 3.4 Soft tone-present probability

Three quantities are tracked per channel from the envelope power `P`:

| field | tracking | purpose |
|---|---|---|
| `noisePow` | gently min-tracked (down 0.1, up 0.002) | **decode** noise reference; deliberately *compressed* (it can't fully reach the floor between dits) so the keying decision stays clean and stable |
| `snrFloor` | aggressively min-tracked (down 0.5, up 0.0003) | **SNR-gate** noise reference; reaches the true floor, so peak/floor is a wide, honest SNR |
| `peakPow`  | fast-attack, slow-decay (up instantly, down 0.001) | keyed-level / strength proxy (used by the SNR gate and the ghost de-dup tiebreak) |

The instantaneous SNR is `zdb = 10·log10(P / noisePow)`. This maps through a
**logistic** to a tone-present probability:

```
pOn = 1 / (1 + exp(-(zdb - kZ0) / kZs))
```

with `kZ0` = 6 dB (the SNR at which P(tone)=0.5) and `kZs` = 4 dB (softness).
This is the "soft, not hard" core: every frame contributes a probability in
(0,1), never a thresholded yes/no.

The **Min-SNR operator control** (§5) acts here: if a channel's *characteristic*
SNR `10·log10(peakPow / snrFloor)` is below the slider, `pOn` is forced to 0 for
that frame — the channel is treated as silent and **not decoded at all**. Note
this gates on the channel-level SNR (peak vs settled floor), so it cleanly
separates strong from weak signals without disturbing the per-frame keying
dynamics of channels that pass.

### 3.5 Fixed-lag Viterbi → most-probable keying

A per-channel **2-state hidden Markov model** (states: key-down, key-up) is run
as a streaming Viterbi over the `pOn` sequence:

- **Emissions:** `log(pOn)` for the down state, `log(1−pOn)` for up.
- **Transitions:** staying in a state costs `log(0.94)`; flipping costs
  `log(0.06)`. This transition penalty is what makes the path *hysteretic*: it
  rejects single-frame noise blips and rides through brief signal fades, with no
  Schmitt trigger or debounce hack. Two running path log-probabilities
  (`dOn`/`dOff`, renormalised each frame) and two back-pointer ring buffers
  (`fromOn`/`fromOff`, length `kViterbiLag` = 32) are maintained.
- **Fixed-lag finalisation:** the key state of the frame `kViterbiLag` ago is
  committed by back-tracking the current best path to it. 32 frames (~160 ms) of
  look-ahead is enough that the segmentation at that lag is stable. This is the
  decode's intrinsic latency.

The finalised key states feed the run-length decoder (§3.6) and are recorded one
bit per frame into `keyHist` (a 128-bit history used for ghost detection, §4.3).

### 3.6 Run-length → Morse → text

Each finalised run (a mark while key-down, a gap while key-up) is classified
against **adaptive duration references**, kept *separately* for marks and gaps
because the finite analysis window inflates marks and shrinks gaps (so a single
shared "dit length" never settles):

- `markUnit` — the dit-mark length. A mark longer than `1.8 × markUnit` is a
  **dah** (`-`), else a **dit** (`.`). Only dits refine `markUnit`.
- `gapUnit` — the inter-element gap length.

Both use **`unitTrack()`**: jump *down* fast (0.4) toward a shorter interval,
leak *up* slowly (0.02). This min-tracking locks onto the true unit within a
couple of dits **even if the first element is a dah** — the failure mode that
collapses a naïve 2-means clustering (where a dah-seeded "dit" cluster never
recovers).

Gaps are handled **incrementally during the silence** by `growGap()`, not when
the next mark starts:

- gap ≥ 2 units → close the current character (look up `symbol` in the Morse
  table, append the letter) — once per gap (`charDone`);
- gap ≥ 6 units → also append a word space — once per gap (`wordDone`).

Doing this during the gap means the **last character of a transmission is
emitted** instead of being lost waiting for a next mark that never comes.

`wpm` is reported from the unit estimate: `1200 / (unit_ms)`, clamped 5–80.

The character decode (`decodeChar()`) is a **duration-vector match**, the core
idea borrowed from fldigi's SOM (Self-Organizing Map) decoder. A naïve exact
lookup of the hard-classified dot/dash string mis-decodes or drops the whole
character whenever one element sits near the dit/dah boundary. Instead, the raw
mark durations of the character are kept (`elemHops`) and matched against each
Morse pattern's ideal duration vector — `markUnit` per dit, `dahUnit` per dah
(both learned per channel; `dahUnit` is needed because the finite window
compresses the measured dah:dit ratio below the textbook 3:1). The score is the
mean squared per-element error in dit units, and the best-fitting pattern wins.
Deferring the dit/dah decision to the character level lets the whole character's
timing resolve a borderline element — e.g. a 5-dah `0` with one slightly short
dah, which a fixed-threshold classifier reads as `9`, fits the all-dah pattern
~2× better and decodes correctly.

Three guards keep it honest:

- **Maturity** — the SOM only runs once the channel has cleanly decoded ≥ 2
  multi-element characters (`richChars`), i.e. the references are settled. During
  cold start it falls back to exact lookup, so unsettled references can't
  manufacture wrong characters (which would pollute the callsign).
- **Confidence** — it commits only to a good fit (mean error < 0.5 dit²) that is
  clearly ahead of the runner-up (a *ratio* margin, `second > 1.8 × best`, which
  scales with error magnitude); a genuine near-tie abstains rather than guesses.
- **Fallbacks** — on a non-confident SOM result it falls back to exact lookup
  (the original behaviour), then to an **edit-distance** recovery for length
  errors from a merged/split element (a single confident edit on a ≥4-element
  symbol, e.g. `------`→`0`). So it is never worse than the old exact lookup.

### 3.7 Callsign extraction + master-callsign validation

`callsignIn()` takes the last whitespace-delimited token of the decoded text and
**validates its syntax** (`isCallsign()`) before treating it as a call at all,
so noise words and prosigns (`TEST`, `CQ`, `QSO`, `599`) are never surfaced or
fed to the master-list correction. The regex is

```
^(?:[A-Z0-9]{1,4}/)?(?:[A-Z][A-Z0-9]?|[0-9][A-Z])\d[A-Z]{1,4}(?:/[A-Z0-9]{1,4})?$
```

— an ITU-style structural core (a prefix of *letter + optional letter/digit* or
*digit + letter*, then a single digit, then a 1–4 letter suffix) that accepts
every real call: 2-letter prefixes (`RI0SP`, `KH6AA`), digit-first prefixes
(`9A1A`, `2E0ABC`) and `E7`/`H4`-style letter+digit prefixes. It is wrapped to
also accept an optional **portable prefix** (`DL/…`) and **portable suffix**
(`…/P`, `…/MM`, `…/QRP`). It is matched whole (`std::regex_match`, compiled once)
against a token first length-bounded to 3–16 chars. (A strict per-first-letter
ITU allocation table was considered but rejected — it dropped valid calls such
as `RI0SP`/`KH6`/`9A1A`; `master.scp` provides the finer filtering instead.)

If a **master-callsign list** (Super-Check-Partial, e.g. `MASTER.SCP`) has been
loaded — `CwSkimmer::loadCallsignDb()`, from `$XDG_DATA_HOME/xlog2/master.scp` —
the token is then **validated and corrected** against it (`lookupCall()`), which
is the single biggest accuracy lever in the real CW Skimmer:

- an exact list hit marks the call **known** (DB-confirmed);
- otherwise, edit-distance-1 variants (delete / substitute / insert over the call
  alphabet) are looked up in the set, and if **exactly one** distinct list entry
  matches, the call is corrected to it (also known) — this fixes the common
  single-character decode errors (`K3L`→`K3LR`, `W1AX`→`W1AW`) directly;
- if neither, the call stays an **unvalidated guess** (not known).

**Confidence-gated substitution.** A *substitution* edit can turn one valid call
into a *different* valid call (`RI0SP`→`RA0SP`: `I`/`..` → `A`/`.-`), silently
rewriting a cleanly-copied special-event call that simply isn't in the list. So
a substitution is only attempted at character positions the decoder was *unsure*
about. `decodeChar()` reports a `confident` flag per character — set for a
committed SOM fit or an exact hard-classified lookup, cleared only for an
edit-distance length-error recovery (a malformed symbol) — and `appendChar()`
records it into a `Channel::conf` string kept aligned 1:1 with the rolling text.
`lookupCall()` skips substitutions where the matching `conf` char is "clean", so
a cleanly-decoded call is never overwritten, while a call with a recovered
(uncertain) element can still be corrected. Deletions and insertions are *not*
gated: they repair a token that came out the wrong length (a merged/split
element), which is inherently a decode error rather than a competing valid call.

The list is an immutable `unordered_set` behind a `shared_ptr`, swapped under a
mutex so a reload never disturbs an in-flight decode; the worker grabs a
reference once per audio chunk. The correction is generate-and-test against the
hash set (cheap even for a 100k-entry list) and only runs on an exact miss.

The **Paranoid** control (`setKnownCallsOnly`, §6) uses the known flag to surface
only channels with a DB-confirmed call. Without a loaded list the heuristic match
is used as before and nothing is ever "known".

---

## 4. Keeping one channel per real signal

Several mechanisms stop the panel filling with duplicates and junk.

### 4.1 Spurious E/T suppression (noise bursts)

A noise burst that briefly spawns a channel produces at most a single one-element
character — an `E` (dit) or `T` (dah). Real CW always contains multi-element
characters. So a channel is **only surfaced to the UI once it has decoded ≥ 2
multi-element characters** (`richChars`). A burst that only ever makes a lone
E/T never qualifies and stays hidden, then ages out silently.

### 4.2 De-duplication (adjacency + callsign)

In a pass after decoding, two channels are merged (the weaker dropped) if they:

- sit within `minSepBins` of each other (drifted onto the same carrier); **or**
- decode the **same callsign** (a harmonic at any separation).

### 4.3 Ghost suppression (lockstep keying)

An *image*, intermod product, or codec artifact reproduces its parent's **exact
keying** at a different pitch — too far apart for the adjacency rule and often
decoding different/garbled text, so neither §4.2 rule catches it. The giveaway is
the timing: it keys in lockstep with its parent.

Each channel keeps a 128-frame key-state history (`keyHist`). Two channels are
judged the same signal if their on-bits overlap heavily — Jaccard
`|A∧B| / |A∨B| ≥ 65%` (over a meaningful amount of activity). Independent signals
overlap ~25% even when sending the same text (different timing); a true ghost is
~90%. The **stronger** channel (`peakPow`) is kept — a ghost is always an
attenuated copy — with decode quality (`richChars`, then text length) as the
tiebreak.

### 4.4 Aging

A channel that produces no finalised keying for `removeAfterHops` (~30 s) is
removed and `onChannelRemoved` fired.

---

## 5. Adapting to a new station on the same frequency

A channel locks onto one station's speed and power. When a *different* operator
takes over the same frequency (slower/faster, weaker/stronger), the previous
station's references would garble them — e.g. a slower op's dits look like dahs
until `markUnit` leaks up.

The decoder detects the **transmission boundary** and re-learns. The trigger is
**carrier presence**, not the decoded key-state: the decoded state is too
noise-sensitive (gap noise injects spurious key-downs that would defeat a
silence timer), whereas the FFT peak `mag[bin]` is a clean, noise-immune signal —
a real op keys a clear peak every dit/dah (< 1 s apart), so a "carrier-gone"
counter stays low through a transmission and only climbs in true silence.

After `newStationHops` (~3 s) of carrier absence, the channel resets its
**per-station** state once — `markUnit`, `gapUnit`, `peakPow` (back to the noise
floor so the SNR re-builds), and the callsign guess — while **keeping** the
per-channel noise floor (a property of the frequency, not the station) and the
already-decoded text.

---

## 6. Operator controls

Both panels (Qt `QtCwSkimmerPanel`, gtkmm `CwSkimmerPanel`) expose two sliders and
a checkbox. Each is a thread-safe atomic the worker reads every frame, so changes
take effect live and survive a stop/start; all persist in the `[skimmer]`
settings group.

| Control | Range | Acts on | Effect |
|---|---|---|---|
| **Gate** | −12…+24 dB (0 = default) | channel **spawn** threshold (`spawnTh`) | Squelch on *detection*. Higher → only stronger peaks above the noise floor start a channel (rejects noise/ghosts); a channel that clears it still decodes at full sensitivity. Lower → catches weaker signals (more noise channels). |
| **Min SNR** | 0…30 dB (0 = off) | the **decoder** (`pOn` forced to 0 below it) | Per-channel signal-vs-own-noise floor. A channel whose characteristic SNR is below the slider is not decoded — its keying is treated as silence, so it produces no text and never surfaces. Stronger signals survive a higher setting than weaker ones. |
| **Show only calls in database** | on/off (disabled if no `master.scp`) | channel **surfacing** (the known flag) | Paranoid mode: only channels whose decoded callsign is confirmed in the master-callsign list are shown; an already-shown channel that goes quiet without ever confirming a call is dropped. Callsign correction (§3.7) happens regardless of this toggle. |

The waterfall uses an *inferno*-style palette (dark/cool noise floor → hot
purple→red→orange→yellow→white) with the colour stops packed into the upper range
and a gamma > 1, so noise recedes and differences in power among strong signals
read as clear colour steps. The decode table is kept ordered by frequency with
in-place field updates (a channel's frequency is fixed, so rows move only on
insert/remove).

### 6.1 Bandwidth normalisation (waterfall brightness vs. rig filter)

Narrowing the rig's IF/DSP filter makes the waterfall **brighter** even though no
signal changed, for two reinforcing reasons: the rig's **AGC** lifts the surviving
passband as there is less noise power to ride, and the skimmer's **noise floor**
(the 30th-percentile across the *whole* 250–4000 Hz analysis band, §3.2)
**collapses** — most of the band is now filtered-out near-silence, so the
percentile lands there. Both widen the displayed `(signal − floor)`.

The optional compensation dims the waterfall by `bwOffDb` dB as the live passband
narrows below a reference width, holding the floor roughly put across filter
changes:

```
bwOffDb = bw_offset_db  +  clamp( bw_norm_db · log2(bw_norm_ref_hz / passbandHz),  0, 45 )   // dim term only when passbandHz < ref
```

`bw_offset_db` is a constant baseline applied first (it is *subtracted* from the
displayed level, so a positive value dims the whole waterfall and a negative value
brightens it; default 0); the second, bandwidth-dependent term dims further as the
filter narrows.

It is **display only** — channel detection, the per-channel SNR and the decode are
untouched. The passband width arrives from `RigController::onFilter` (so it needs
a Hamlib-connected rig that reports per-mode widths; otherwise it is 0 and no
compensation is applied) and the worker recomputes the offset every frame, so a
filter change takes effect at once. `bw_norm_db` is **empirical** — it folds in
both the AGC rise and the percentile-floor collapse, so tune it to taste. Both
knobs live in `[skimmer]` and are ini-only (no slider): `bw_norm_db` (dB per
octave, default 6; 0 disables), `bw_norm_ref_hz` (the width treated as 0 dB,
default 2800 — set it to the widest filter you use so narrower ones only dim), and
`bw_offset_db` (constant baseline trim subtracted first, default 0; positive dims,
negative brightens the whole waterfall regardless of filter width).

---

## 7. Tuning constants (quick reference)

All in `CwSkimmer::worker()` unless noted. Values are at the ~12 kHz working rate.

| Constant | Value | Meaning |
|---|---|---|
| `fftSize` | `nextPow2(rate·0.043)` | ~43 ms window, ~23 Hz bins (channelization only) |
| `hop` | `fftSize/8` | ~5 ms STFT step |
| low-pass cutoff | 70 Hz | per-channel receiver bandwidth (envelope time resolution) |
| `kViterbiLag` | 32 frames | keying look-ahead (~160 ms decode latency) |
| `kZ0` / `kZs` | 6 dB / 4 dB | tone-present logistic midpoint / softness |
| `kLogStay` / `kLogFlip` | log .94 / log .06 | HMM self-transition / flip cost |
| `spawnPersist` | 3 frames | peak persistence before a channel spawns |
| `minSepBins` | ~150 Hz | minimum channel spacing (= 2× receiver bandwidth) |
| `kMaxChannels` | 40 | concurrent-channel cap |
| dah threshold | 1.8 × `markUnit` | mark longer than this is a dah |
| char gap / word gap | 2 / 6 units | inter-character / word-space thresholds |
| `richChars` to surface | 2 | multi-element chars before a channel is shown |
| ghost Jaccard | 65% | keying overlap to merge a ghost |
| `newStationHops` | ~3 s | carrier-absence before re-learning a new op |
| `removeAfterHops` | ~30 s | idle time before a channel is dropped |
| `bw_norm_db` | 6 dB/oct (0 = off) | waterfall dim per octave of filter narrowing (§6.1, `[skimmer]` ini) |
| `bw_norm_ref_hz` | 2800 Hz | passband width treated as 0 dB (§6.1, `[skimmer]` ini) |
| `bw_offset_db` | 0 dB | constant waterfall trim applied first; +dims/−brightens (§6.1, `[skimmer]` ini) |

---

## 8. Known limitations

- **Cold start.** The first character or two of a transmission is often wrong —
  the unit length isn't yet learned. Inherent to streaming Morse decoders.
- **Dictionary is optional.** The *master-list* validation/correction (§3.7) only
  kicks in when a `master.scp` is present; the syntax gate (`isCallsign()`) always
  applies. Without a list, callsigns are syntax-checked but never DB-confirmed,
  and the accuracy ceiling is lower — in heavy QRN that forms fake Morse
  *as strong as* the signal, per-channel SNR can't separate them and only the
  dictionary check rejects the junk. With a list loaded, Paranoid mode closes
  most of that gap.
- **Single passband, not wideband IQ.** This decodes the rig's audio passband
  (a few kHz), not a whole band like an SDR-fed skimmer.
- **Single passband, not wideband IQ.** This decodes the rig's audio passband
  (a few kHz), not a whole band like an SDR-fed skimmer.
- **Frequency resolution.** Two genuinely independent stations closer than the
  ~150 Hz receiver bandwidth are reported as one channel — the narrowband
  receiver cannot cleanly separate them anyway.

---

## 9. Comparison with fldigi, and a follow-up

fldigi's CW decoder (`cw_rtty/cw.cxx`, `morse.cxx`) is **single-channel** (it
decodes the one tuned signal). Comparing the per-channel decode:

| Stage | fldigi | here |
|---|---|---|
| Tone detection | amplitude demod + AGC + **hysteresis Schmitt** (`CWupper`/`CWlower`), attack/decay averages | **soft probability + fixed-lag Viterbi** |
| Unit tracking | `update_tracking()` confirms dot-dah pairs → `two_dots` | min-tracked `markUnit`/`gapUnit` |
| Character decode | **SOM** (`find_winner`, distance on normalised durations) or exact lookup | **duration-vector SOM** + exact + edit-distance fallbacks (§3.6) |

We are **ahead on detection/segmentation** — the soft-probability Viterbi is
strictly more robust than fldigi's Schmitt-trigger amplitude detector, which is
the main reason this decoder copes with fades and low SNR.

The idea worth borrowing was fldigi's **SOM character matcher**, and it is now
adopted in full (§3.6): the per-element durations are kept and matched against
each pattern's ideal duration vector, deferring the dit/dah decision to the
character level so a borderline element is resolved by the whole character's
timing. A naïve exact lookup hard-classifies each element first, so most
single-element errors land on a *different valid* character (Morse is dense) — a
wrong letter no string-level match can fix; the duration-vector match avoids that
by construction. It is maturity- and confidence-gated, with exact lookup and an
edit-distance length-error recovery as fallbacks, so it never decodes worse than
the old exact lookup.

The other big accuracy lever from CW Skimmer — the **master-callsign / SCP
dictionary check** — is also implemented (§3.7): decoded callsigns are validated
and edit-distance-corrected against an optional `MASTER.SCP`, with a Paranoid
mode that surfaces only DB-confirmed calls. This is what closes the heavy-QRN gap
that pure DSP can't.
