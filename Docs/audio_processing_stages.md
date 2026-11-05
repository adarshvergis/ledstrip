# Audio processing stages

What each stage of the processing pipeline does, why it is there, and what it does to the
block of samples it receives. Source: `firmware/App/audio_processing/stages/stage_*.c`;
ids and parameter enums: `stage_params.h`. How to add stages: [extending.md](extending.md).

## 1. The input array and the frame

* Every **4 ms** the real-time task gets one block of **64 samples** at **16 kHz**
  (`AUDIO_HOP_SIZE`, `SAMPLE_RATE` in `build_config.h`).
* Data acquisition takes the newest DMA block and selects the microphone slot.
  * The INMP441 delivers 24-bit samples left-aligned in 32-bit words; these are used directly
    as **Q31** (+/-1.0 = full scale, `int32_t`).
  * The result goes into `audio_frame_t.samples[64]`.
* The frame then passes through the stages of the active algorithm in order. Each stage:
  * may **modify `samples[]` in place**: Front-end, Filter A/B;
  * may **only read** `samples[]` and add **features** to the frame: Envelope, Spectrum, Beat;
  * may only read features and **draw `leds[]`**: render stages.
* Before each block `beat`, `beat_strength` and `spectrum_valid` are cleared. Other features
  keep their last value.
* All per-block maths is fixed point:
  * Q31 samples, Q2.30 filter coefficients, Q15 FFT;
  * levels 0..32767 (Q15, 32767 = 1.0);
  * dB values in Q8 (1/256 dB) or Q16.

  Floats are used only in `configure()`, when a parameter changes.

```
samples[64] Q31
   |
   v
[0 Front-end] -> [1 Filter A] -> [2 Filter B]          modify samples[] in place
   |
   +-> [3 Envelope] -> envelope, envelope_db
   |        +-> [4 AGC] -> level
   +-> [5 Spectrum] -> bands[12], centroid, dominant_band     (keeps its own 256-sample history)
   +-> [6 Beat] -> beat, beat_strength, bpm_x10, beat_phase   (keeps its own filtered copy)
   +-> [11 Melody] -> melody: voice active, pitch, salience, note starts   (own 512-sample history)
   |
   +-> [12 Music analysis] -> music: hits, clarity, scene, layer weights, genre   (features only)
   |
   v
[8..10, 13 Render] -> leds[60]   (read features only)
```

## 2. Algorithms (which stages run)

| # | Algorithm | Stage order |
|---|---|---|
| 0 | Music show, default | Front-end, Filter A, Envelope, AGC, Spectrum, Beat, Melody, Music analysis, Render show |
| 1 | Beat (all LEDs) | Front-end, Filter A, Envelope, AGC, Beat, Render beat flash |
| 2 | Spectrum bars | Front-end, Filter A, Filter B, Envelope, AGC, Spectrum, Render spectrum |
| 3 | Beat segments | Front-end, Filter A, Envelope, AGC, Spectrum, Beat, Render beat segments |

A stage used by several algorithms has one shared parameter set. For example, the mic gain
is calibrated once for all algorithms.

Stage id 7 belonged to "Render level" of the removed amplitude algorithm. Ids are persistent
calibration keys, so 7 stays unused.

## 3. Signal conditioning stages (modify `samples[]`)

### Stage 0: Front-end (`stage_frontend.c`)

**What:** microphone channel selection, DC removal, input gain.

**Why:**

* Selects the I2S slot that actually carries the microphone signal.
* MEMS microphones and the I2S path have a DC offset, which would distort every level
  measurement downstream.
* Room music arrives at roughly -40..-50 dBFS, so it needs gain before thresholds and the
  FFT see useful numbers.

**Operations on the input array:**

1. **Channel:** `configure()` passes the `mic_channel` setting to data acquisition.
   * *auto* sums |x| of both slots over the first 64 blocks (256 ms) and picks the louder one.
   * The selection happens before the pipeline runs.
2. **DC blocker**, per sample, in place: `y[n] = x[n] - x[n-1] + R * y[n-1]`, with
   `R = 1 - 2*pi*fc/fs`. This is a first-order high-pass at `dc_cutoff`.
3. **Gain**, per sample, in place: `x = sat(x * gain)`, with the gain in Q16.16 from
   `input_gain` dB. The result saturates at Q31 full scale instead of wrapping.

| Parameter | Default | Range |
|---|---|---|
| input_gain | 18 dB | 0..48 dB |
| dc_cutoff | 30 Hz | 5..200 Hz |
| mic_channel | auto | auto / left / right |

### Stages 1 and 2: Filter A / Filter B (`stage_filter.c`)

**What:** two independent, fully configurable second-order (biquad) filters, in the RBJ
Audio-EQ-Cookbook designs:

* off, low-pass, high-pass, band-pass
* notch, peaking EQ, low shelf, high shelf

**Why:**

* Filter A defaults to a **45 Hz high-pass**, which removes rumble, desk vibration and
  handling noise that would otherwise trigger the level and beat detection.
* Filter B is off by default. It is a free slot for experiments, e.g. a band-pass that makes
  the amplitude algorithm follow vocals, or a notch for a mains hum.
* Two instances of the same code with separate stage ids give two independently tunable
  filters.

**Operations on the input array:** a Direct Form I biquad, per sample, in place:

* `y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2`
* coefficients in Q2.30, 64-bit accumulator (products shifted right by 2 for headroom),
  result saturated to Q31
* filter memory (`x1, x2, y1, y2`) carries over between blocks
* *off* skips the stage entirely

Coefficients are computed in `configure()` from type, frequency, Q and gain.

| Parameter | Default (A / B) | Range |
|---|---|---|
| type | highpass / off | off, lowpass, highpass, bandpass, notch, peak, lowshelf, highshelf |
| frequency | 45 Hz / 1000 Hz | 20..7900 Hz |
| q | 0.707 | 0.1..10 |
| gain (peak / shelf only) | 0 dB | -24..+24 dB |

## 4. Analysis stages (read `samples[]`, write features)

### Stage 3: Envelope (`stage_envelope.c`)

**What:** the loudness of the signal over time, as a smoothed amplitude.

**Why:** a single block's level jumps around. An envelope follower with a fast attack and a
slower release gives a stable loudness value that still reacts immediately to hits.

**Operations on the input array (read only):**

1. One value per block: **RMS** (`sqrt(mean(x^2))`, default) or **peak** (`max |x|`) over the
   64 samples.
2. One-pole smoothing with separate time constants:
   `y += alpha * (x - y)`, using `alpha_attack` when rising and `alpha_release` when falling
   (alpha = 1 - e^(-T/tau), per block).
3. Conversion to dBFS: `20*log10(y)` through a fixed-point log2 table, minus the full-scale
   reference.

**Outputs:**

* `envelope`: linear amplitude, Q31
* `envelope_db`: dBFS, Q8

| Parameter | Default | Range |
|---|---|---|
| attack | 4 ms | 0.5..200 ms |
| release | 150 ms | 5..2000 ms |
| detector | rms | rms / peak |

### Stage 4: AGC (`stage_agc.c`)

**What:** automatic level normalisation. It turns the envelope (in dBFS) into a **0..1
level** relative to how loud the music has recently been.

**Why:**

* The absolute loudness depends on speaker volume, distance and the song.
* Thresholds such as "LEDs on above 0.5" only make sense on a normalised scale.
* A noise gate keeps the LEDs dark in silence.
* A maximum gain stops a quiet room from being amplified into "music".

**Operations (on `envelope_db`, not on the samples):**

1. **Peak tracker:** jumps up instantly to a louder envelope, then decays at `peak_decay`
   dB/s, but never below the gate.
2. **Reference:** `ref = max(peak, -max_gain) - headroom`.
3. **Noise gate with 3 dB hysteresis:** below `noise_gate`, level = 0.
4. **Mapping:**
   * *log* (default): `level = (env - (ref - range)) / range`, so the top `range` dB under the
     reference map linearly to 0..1;
   * *linear*: `level = 10^((env - ref)/20)`.
5. Clamp to 0..32767.

**Output:** `level`, 0..32767.

| Parameter | Default | Range |
|---|---|---|
| peak_decay | 3 dB/s | 0.2..30 dB/s |
| noise_gate | -62 dBFS | -100..-20 dBFS |
| max_gain | 40 dB | 0..70 dB |
| headroom | 2 dB | 0..12 dB |
| curve | log | log / linear |
| range | 24 dB | 6..60 dB |

### Stage 5: Spectrum (`stage_spectrum.c`)

**What:** a frequency analysis. It produces 12 logarithmically spaced band levels (0..1),
the spectral centroid and the loudest band.

**Why:**

* The spectrum-bar algorithm needs per-band levels: which band decides *which* LEDs, the
  band level decides *how many*.
* The beat-segment algorithm places pulses by the centroid (bass-heavy -> one end,
  bright/treble-heavy -> other end).
* Log spacing matches how we hear pitch.
* Per-band normalisation keeps quiet bands (e.g. treble) visible next to the loud bass.

**Operations on the input array:**

1. **History:** the 64 new samples are converted Q31 -> Q15 and appended to a 256-sample ring
   buffer. The FFT therefore always sees the last 16 ms, recomputed every 4 ms (75 % overlap).
2. **Block floating point:** find the largest |x| in the window and shift all samples left
   so it uses the full Q15 range. Quiet signals keep their resolution; the shift is subtracted
   again in dB (6.02 dB per bit).
3. **Hann window** (`x[i] * w[i]`, Q15), then copy into a complex array (imaginary part 0).
4. **256-point radix-2 FFT** in Q15, scaled by 1/2 per stage, so it cannot overflow.
   Bin spacing is 62.5 Hz.
5. **Band power:** `re^2 + im^2` per bin, summed over each band's bin range. Band edges are
   log-spaced from `min_freq` to `max_freq`; every band gets at least one bin.
6. **dBFS:** `10*log10(power)` minus the full-scale reference (80 dB) and the block-floating
   shift.
7. **Normalisation:**
   * *normalize on*: each band has its own peak tracker (instant up, `norm_decay` dB/s down,
     never below `floor + range`), and `value = (dB - (peak - range)) / range`;
   * *normalize off*: `value = (dB - floor) / range`.
8. **Smoothing:** attack/release one-pole per band, then clamp to 0..32767.
9. **Centroid:** `sum(k * P[k]) / sum(P[k])` over the analysed bins, as a fraction of Nyquist.
   **Dominant band** = the band with the highest smoothed value.

**Outputs:** `bands[12]`, `centroid` (Q15 fraction of 8 kHz), `dominant_band`,
`spectrum_valid`.

| Parameter | Default | Range |
|---|---|---|
| min_freq | 60 Hz | 30..500 Hz |
| max_freq | 6000 Hz | 1000..7900 Hz |
| range | 36 dB | 10..80 dB |
| floor | -80 dBFS | -110..-30 dBFS |
| attack | 8 ms | 0..200 ms |
| release | 180 ms | 10..2000 ms |
| normalize | on | bool |
| norm_decay | 2 dB/s | 0.2..20 dB/s |

### Stage 6: Beat detector (`stage_beat.c`)

**What:** detects the **main beat** (kick / bass drum), estimates the **tempo (BPM)**, and
outputs beats that are phase-locked to that tempo.

**Why:**

* Reacting to every loudness change is not "the beat": hi-hats, vocals and off-beats would
  all trigger.
* The kick lives in the bass band, so the detector looks for sudden bass energy increases
  (onsets).
* Real music has missing or weak kicks and strong off-beats. Knowing the tempo lets the
  detector:
  * ignore off-beat onsets;
  * predict beats that are missing ("flywheel");
  * re-lock if it ever latched onto the wrong phase.

**Operations on the input array:**

1. **Bass copy:** the 64 samples are copied to a scratch buffer and low-pass filtered (biquad
   at `bass_cutoff`, Q 0.707). The frame's `samples[]` are **not** modified.
2. **Bass energy:** `sum(x^2)` of the block, summed over the last 4 blocks (16 ms, about one
   bass period, because a single 4 ms block wobbles within one kick), converted to dB.
3. **Onset detection function (ODF):** `max(0, E_dB - slow average of E_dB)`, where the slow
   average has tau = 150 ms. It is large when the bass gets suddenly louder than it was.
   Output as `onset`.
4. **Adaptive threshold:** `max(min_onset, threshold_factor * mean(ODF))`, with the mean over
   tau = 1.5 s.
   * **Onset** = ODF above the threshold while *armed*. It re-arms only after the ODF has
     dropped below half the threshold, so one kick gives one onset.
5. **Tempo:**
   * The ODF is decimated by 2 (125 Hz) into a ~3 s history.
   * Every 32 blocks (128 ms): remove the mean, compute the **autocorrelation** over the lags
     for `min_bpm`..`max_bpm`, weighted by a log-Gaussian prior centred on 120 BPM (octave
     width). This prevents half/double-tempo picks.
   * The best lag is refined with **parabolic interpolation**.
   * Smoothing: changes under 10 % are smoothed in; a different tempo must be seen 3 times in
     a row before it is adopted.
   * Confidence = correlation / energy; below ~0.12 the tempo counts as unknown.
6. **Beat decision** (phase tracking, `t` = time since the last beat, `P` = period):
   * Without tempo lock (or no tempo yet): every onset at least `0.6 * 60/max_bpm` after the
     previous beat is a beat.
   * With lock, for an onset at:
     * `t >= P - tol*P`: a beat, re-syncs the phase.
     * `t <= tol*P` (just after a predicted beat): no new beat; the phase is pulled half-way
       towards the onset.
     * anything else: an off-phase onset. If 3 or more arrive at a consistent offset and are
       1.5x stronger than the in-phase ones, the detector re-locks onto them.
   * With lock and no onset when `t` reaches `P`: a **predicted beat**, up to
     `flywheel_beats` in a row, at 3/4 of the last strength.

**Outputs:**

* `beat`: true in the block with a beat
* `beat_strength`: Q15
* `bpm_x10`
* `beat_phase`: Q15 position inside the period
* `beat_count`
* `onset`: Q8 dB
* `tempo_conf` (Q15), `beat_predicted` (flywheel beat), `bass_onset` + `bass_onset_strength`
  (every bass onset, also off-beat ones; the music analysis uses them as kicks)

Measured delay: a beat fires one block (~4 ms) after the kick onset. Host tests at
95/120/128/140 BPM and with missing kicks are in `UnitTest/host/test_host.c`.

| Parameter | Default | Range |
|---|---|---|
| bass_cutoff | 150 Hz | 60..400 Hz |
| threshold_factor | 1.6 x | 1.0..4.0 |
| min_onset | 2.5 dB | 0.5..15 dB |
| min_bpm | 75 | 40..140 |
| max_bpm | 180 | 100..220 |
| tempo_lock | on | bool |
| lock_tolerance | 0.18 period | 0.05..0.45 |
| flywheel_beats | 4 | 0..16 |

### Stage 11: Melody (`stage_melody.c`), used by algorithm 0

**What:** finds the prominent pitched voice (guitar solo, vocal line, synth lead): is one
playing, which pitch, when does a new note start.

**Why:** when there is no clear beat the strip should follow the lead instead; individual
notes become individual sparks.

* The 256-point spectrum (62.5 Hz bins) is too coarse for pitch, so this stage keeps its own
  512-sample history (31.25 Hz bins, 32 ms window).
* A chord or a drum hit must not count as "a voice": salience measures how much one harmonic
  series dominates.

**Operations (every 2nd block, i.e. every 8 ms):**

1. Hann window + 512-point Q15 FFT with block floating point (as in Spectrum); power per bin;
   power of each 3-bin main lobe `L(k)`.
2. **Candidates:** bins in `min_freq..max_freq` that are a local peak and carry at least 1/4
   of their harmonic sum `H(k) = L(k) + L(2k)/2 + L(3k)/3` (rejects the "missing
   fundamental" of a chord and bass leaking in). The best `H` wins.
3. **Salience** = `H(best) / power above min_freq` x **dominance**, where dominance =
   `2 * (H(best) - H(second)) / H(best)` and "second" is the best candidate that is not on
   the winner's harmonic series. One voice: ~0.9; a three-note pad: ~0; drums: ~0.05.
   Smoothed with 25 ms.
4. **Pitch:** strongest bin of the fundamental's lobe, parabolic interpolation in dB, converted
   to semitones (MIDI note number, Q8).
5. **Notes:**
   * the voice is *active* while the salience is above `salience` (off after two frames below
     80 % of it);
   * a **note start** is a pitch change > 0.8 semitone that holds for `note_min`, or a
     re-attack (the voice's energy jumps by `reattack` dB at the same pitch);
   * smaller changes (vibrato, bends) glide the pitch with `glide`.
6. **Level:** voice-region energy against its own peak (3 dB/s decay), 30 dB range -> 0..1.

**Outputs:** `melody.active`, `melody.note_on`, `melody.pitch_q8`, `melody.salience`,
`melody.level`.

| Parameter | Default | Range |
|---|---|---|
| min_freq | 150 Hz | 80..1000 Hz |
| max_freq | 2000 Hz | 400..4000 Hz |
| salience | 0.35 | 0.05..0.9 |
| note_min | 24 ms | 8..200 ms |
| glide | 20 ms | 0..200 ms |
| reattack | 4 dB | 1..15 dB |

### Stage 12: Music analysis (`stage_music.c`), used by algorithm 0

**What:** the "scene director". From the features of the stages before it, it works out what
the music is doing right now and how much each render layer of the music show should show.

**Why:** the beat detector alone is right during a drop or chorus, but in a solo, a break or a
drum fill there is no steady beat to follow. These situations need their own visuals, and the
change between them must be smooth.

**Operations (inputs: `band_db[]`, `bands[0]`, `envelope_db`, `level`, beat outputs,
melody):**

1. **Drum hits** (`music.hits`, one bit per hit class, with `hit_strength[]`):
   * spectral flux per band = dB rise above a 60 ms baseline, only for bands within 30 dB of
     the loudest; averaged over groups MID (bands 2-5), upper mids (6-9) and HIGH (10-11);
     onset per group as in the beat stage (adaptive threshold 1.8 x mean, at least
     `hit_threshold`, re-armed below half of it), and the group's loudest band must be close
     to the loudest group;
   * **kick** = the beat stage's bass onset (the 256-point FFT cannot resolve a 50 Hz kick or
     808); if the mids rise clearly more it is a low **tom**;
   * MID onset with the upper mids rising too = **snare** (or a clap from the upper mids),
     otherwise **tom**; `tom_pos` = flux-weighted band (rolls sweep along the strip);
   * HIGH onset = **hi-hat** at once; if it still rings 100 ms later it is a **crash**;
   * while a lone pitched voice plays (melody salience > 0.5, no groove) mid-range rises are
     its notes, not drums;
   * **hi-hat roll**: 4+ hats less than 1/5 beat apart.
2. **Clarity** (how clear the beat is): tempo confidence x share of the last 8 beats that had a
   real onset (a flywheel beat with a kick within 80 ms counts) x bass presence (a melody alone
   can fool the tempo tracker). Rises with 300 ms, falls with 800 ms.
3. **Intensity:** 300 ms vs 8 s loudness, +-6 dB -> 0..1.
4. **Build-up:** loudness AND brightness (upper bands vs mids) both rising over the last 2 s,
   with the beat thinning out or a snare roll, for at least 1.5 s.
5. **Drop:** armed for 4 s after a build-up or after 2 s of low intensity. A strong kick with the
   bass >= 6 dB above its 2 s average, the bass band near the top and the overall level back
   near the long-term level, confirmed if the bass stays up for ~40 ms (rejects noise spikes of
   a riser). Lasts `drop_bars` bars (at most 20 s); the drop beat becomes beat 1 of the bar.
6. **Fill:** tom + snare hits in the last 0.5 s against 2.5 hits per beat (scaled by
   `fill_sensitivity` and the genre). Held 0.4 s.
7. **Bar position:** beats counted modulo `beats_per_bar`; the bar position with clearly stronger
   beats (and crashes) becomes the downbeat.
8. **Layer weights** (crossfaded: `scene_fade` in, twice that out):
   * lead = salience above 0.2 x (0.4 + recent note starts) x lead gains: a moving melody counts
     fully, a held pad / drone only a little;
   * groove = soft step of clarity around `clarity_threshold`, reduced by half the lead and by
     a fill; during a drop groove is forced to full at once;
   * perc = fill score; ambient = the rest; all 0 in silence (level 0 for 0.5 s).
9. **Scene** (for display / debugging): silence > drop > build > fill > lead > groove >
   ambient.
10. **Genre:** fixed by `genre`, or *auto*: once per second rules score each genre from ~10 s
    statistics (tempo, kick / snare / hat rate per beat, hi-hat rolls, kick / 808 tail length,
    bass share, clarity, salience). E.g. 118-135 BPM, clear beat, kick on every beat -> EDM;
    long 808 tails, hat rolls, half-time -> trap; snare on 2 and 4 at 80-100 BPM -> hip-hop.
    The first decision needs 8 s of music and 3 wins in a row, later changes 10 wins in a row.
    The effective genre is published as `music.genre`.

**Outputs:** `music` (see `music_t` in `audio_types.h`): scene, genre, bar_beat, hits,
hit_strength, tom_pos, w_groove / w_lead / w_perc / w_ambient, intensity, clarity, build, sub,
drop, hat_roll.

| Parameter | Default | Range |
|---|---|---|
| genre | auto | auto / edm / pop / rock / metal / rnb / hiphop / trap / orchestral |
| clarity_threshold | 0.35 | 0.05..0.9 |
| lead_sensitivity | 1.0 x | 0..3 |
| fill_sensitivity | 1.0 x | 0..3 |
| hit_threshold | 4 dB | 1..15 dB |
| scene_fade | 250 ms | 20..3000 ms |
| drop_detect | on | bool |
| drop_bars | 8 | 1..32 |
| beats_per_bar | 4 | 2..8 |

**Genre profiles** (`music_profiles.c`): what a genre changes.

| Genre | Groove | Lead / percussion | Palette |
|---|---|---|---|
| EDM | leans to all LEDs, on/off 90 ms, halves / segments / chase, white strobe on the drop | lead x0.8 | blue / violet |
| Pop | neutral, on/off 110 ms, halves / centre burst, snare accents | lead x1.1 | pink / gold |
| Rock | slightly to all, decaying 260 ms, centre burst / halves, snare accents | lead x1.2, percussion x1.2, fills x1.2 | warm orange |
| Metal | leans to all, on/off 60 ms, centre / segments / chase, strobe on the drop | lead x1.3, percussion x1.3, fills x0.7 (blast beats) | red |
| R&B | leans to some, soft 450 ms centre glow, sub-bass glow | lead x1.3, percussion x0.7 | purple / magenta |
| Hip-hop | leans to some, 220 ms, snare accents, sub-bass glow | neutral | gold / amber |
| Trap | leans to some, 300 ms, clap = full flash, strong 808 glow, hat rolls run along the strip | percussion x1.1 | dark red / violet |
| Orchestral | rarely all, 600 ms swells, clarity threshold +0.15 | lead x1.5, percussion x0.6 | warm pastel |

## 5. Render stages (read features, draw `leds[]`)

* Render stages do not touch `samples[]`.
* Each one writes all `num_leds` entries of `leds[]`, as RGB 0..255.
* The light-control stage then applies global brightness, the error overlay and the power
  limiter.
* Colours come from integer HSV; brightness ramps use a gamma 2.2 table.

### Stage 8: Render beat flash (`stage_render_beat_flash.c`), used by algorithm 1

**What:** all LEDs on with every beat.

**Why:** the "all LEDs react to the main beat" behaviour. A colour step per beat makes
consecutive beats distinguishable.

**Operations (inputs: `beat`, `beat_strength`):** on a beat:

* amplitude = 255, or 90 + 165 * strength if `use_strength` is on;
* hue += `hue_step`.

Then, depending on the mode:

* *on/off* (default): full amplitude for `on_time`, then off.
* *flash*: brightness decays exponentially (factor e^(-4 ms / decay) per block), with gamma.
* Never below `floor`.

Every LED gets the same HSV colour.

| Parameter | Default | Range |
|---|---|---|
| mode | on/off | on/off / flash |
| on_time | 110 ms | 10..1000 ms |
| decay | 220 ms | 20..2000 ms |
| hue | 280 deg | 0..359 |
| saturation | 1.0 | 0..1 |
| hue_step | 40 deg | 0..180 |
| use_strength | on | bool |
| floor | 0 | 0..0.5 |

### Stage 9: Render spectrum (`stage_render_spectrum.c`), used by algorithm 2

**What:** the strip is divided into 12 segments, one per frequency band (*which* LEDs). Each
segment shows a bar whose length follows that band's level (*how many* LEDs), with peak
markers.

**Why:** a direct, readable visualisation of the frequency content.

* The threshold hides the noise floor; the gain lets small bands fill the bar.
* Peak hold shows recent maxima, as on a hi-fi spectrum display.

**Operations (input: `bands[]`):** per band:

1. `v = (band - threshold) / (1 - threshold)`, then `* gain`, clamped. This is the bar
   height as a fraction of the segment.
2. **Peak marker:** follows the bar up, holds for `peak_hold`, then falls at `peak_fall`
   segments/s.
3. **Draw the segment:**
   * lit LEDs = height x segment length;
   * the last LED is partially dimmed, for sub-LED resolution;
   * *fade* ramps brightness along the bar;
   * the peak LED is brightened.
4. Hue goes from `hue_low` (band 0, bass) to `hue_high` (band 11, treble).

**Layouts:**

* *segments*: the bar grows from each segment's start.
* *centered*: the bar grows out from each segment's middle.
* *mirror*: drawn on half the strip and mirrored, with the bass in the middle.

| Parameter | Default | Range |
|---|---|---|
| layout | segments | segments / centered / mirror |
| hue_low | 0 deg | 0..359 |
| hue_high | 270 deg | 0..359 |
| gain | 1.0 x | 0.25..4 |
| threshold | 0.08 | 0..0.5 |
| peak_hold | 400 ms | 0..2000 ms |
| peak_fall | 2.5 seg/s | 0.2..20 |
| fade | on | bool |

### Stage 10: Render beat segments (`stage_render_beat_segments.c`), used by algorithm 3

**What:** every beat spawns a light pulse. Its **position** comes from the spectral centroid
(*which* LEDs) and its **width** from the beat strength (*how many* LEDs). Pulses fade,
optionally widen, and overlap additively.

**Why:** the "beat-driven, but not all LEDs" behaviour. The position carries information
about the sound: a bassy beat lights one end, a bright snare/clap-heavy beat the other.

**Operations (inputs: `beat`, `beat_strength`, `centroid`, `dominant_band`):**

1. **Adaptive centroid range:** track the min/max centroid with slow relaxation, so positions
   use the whole strip whatever the music's typical brightness.
2. On a beat, create a pulse in a ring of up to `max_pulses`:
   * **position** (mode `position`):
     * *centroid*: normalised centroid (default);
     * *dominant band*: centre of that band;
     * *sweep*: golden-ratio steps along the strip;
   * **width**: `min_width + strength * (max_width - min_width)`;
   * **hue** (mode `hue_mode`):
     * *position*: 0..300 deg along the strip;
     * *beat cycle*: +47 deg per beat;
     * *fixed*: `hue`.
3. Every block:
   * clear the strip;
   * draw each live pulse as a triangle (bright centre, linear falloff to its half-width),
     adding colours together;
   * multiply its amplitude by e^(-4 ms / decay);
   * with `spread`, grow the width by ~1.6 % per block up to `max_width`.

| Parameter | Default | Range |
|---|---|---|
| min_width | 3 LEDs | 1..30 |
| max_width | 18 LEDs | 2..60 |
| decay | 450 ms | 50..3000 ms |
| position | centroid | centroid / dominant band / sweep |
| hue_mode | position | position / beat cycle / fixed |
| hue | 200 deg | 0..359 |
| saturation | 1.0 | 0..1 |
| max_pulses | 5 | 1..8 |
| spread | on | bool |

### Stage 13: Render show (`stage_render_show.c`), used by algorithm 0

**What:** the music show's light. Four layers, each drawn into a scratch buffer and added to
the strip scaled by its weight from the music analysis; colours from the genre profile's
palette (or a custom hue).

**Why:** each musical situation gets a matching visual, and crossfading the layers avoids hard
cuts when the analysis changes its mind.

**Layers:**

1. **Groove** (weight `w_groove`), on every beat:
   * score for "all LEDs" = 0.35 x intensity + 0.25 x (strength relative to recent beats) +
     0.3 on the downbeat + 0.35 during a drop + genre bias + `full_bias` - 0.4 x lead weight;
   * score >= 0.5, the first beat of a drop, or (trap) a clap: **all LEDs**;
   * otherwise **some LEDs**, the genre's patterns rotating per bar: alternating halves, a
     centre burst whose width follows the beat strength (30-90 % of the strip), every other
     of 8 segments (alternating), or a chase (segment = beat in the bar);
   * on/off for the genre's (or `on_time`) duration, or a decaying flash; colour changes per
     bar; white strobe on the first drop beat (EDM, metal);
   * snare accents (3 LEDs at both ends) and a sub-bass / 808 glow from the centre, depending
     on the genre.
2. **Lead** (weight `w_lead`): every note start spawns a spark (up to `max_sparks`):
   position = pitch inside the recently played range (at least one octave, adapts slowly),
   hue = pitch class x 30 deg from the palette's lead hue; while the note sounds the spark
   follows its pitch (bends, vibrato) and level; released notes fade with `lead_trail`. A dim
   glow (<= 12 %) follows the voice's salience.
3. **Percussion** (weight `w_perc` + 1/4 of the other layers, x genre percussion gain):
   tom = flash at `tom_pos` (2-6 LEDs, 120 ms); snare = whitish centre burst; kick (when the
   groove layer is weak) = pulses at both ends; hi-hat = 1-6 random single-LED twinkles
   (`sparkle`), during a roll they run along the strip; crash = shimmer over the whole strip
   (600 ms).
4. **Ambient** (weight `w_ambient`): a slow two-colour gradient drifting along the strip,
   brightness = level x `ambient`; during a build-up bars grow from both ends towards the
   centre and flicker with the hits.

| Parameter | Default | Range |
|---|---|---|
| palette | genre | genre / custom |
| hue (custom palette) | 280 deg | 0..359 |
| saturation | 1.0 | 0..1 |
| full_bias | 0 | -1..1 |
| on_time | 0 = genre | 0..1000 ms |
| lead_trail | 300 ms | 20..3000 ms |
| sparkle | 1.0 x | 0..3 |
| ambient | 0.3 | 0..1 |
| max_sparks | 6 | 1..8 |

## 6. Cost per block

Rough figures at 160 MHz; measured values are shown by the CLI `stages` command and the
diagnostics view.

| Stage | Main cost |
|---|---|
| Front-end, Filter A/B | ~64 x (DC + gain) or 5 MACs per sample, a few us each |
| Envelope, AGC | one RMS over 64 samples plus a few scalar ops |
| Spectrum | 256-point FFT (8 x 128 butterflies) + window + band sums, ~0.2 ms |
| Beat | 64-sample biquad + energy every block; 3 s autocorrelation every 32 blocks (~0.6 ms then) |
| Melody | 512-point FFT + harmonic sums every 2nd block (estimate ~0.5 ms then; not yet measured on the device) |
| Music analysis | band flux and a few hundred scalar ops per block |
| Render stages | 60 LEDs of integer HSV, well under 0.1 ms; Render show draws up to four layers |
| Live spectrum taps | a 64-sample copy per block for each tap the host requested (at most 4), nothing otherwise; the FFTs run in the comm task |

Whole pipeline: well under the 4 ms block period (typically 0.3-0.6 ms), so the latency is
dominated by the block length and the LED wire time (`architecture.md`, section 2).

## 7. Live spectrum taps (host view, not a stage)

The host's spectrum view can look at the signal after any stage (`spectrum_taps.c`). The
pipeline copies each block at a requested tap (after stage id n, or after the last stage)
into a 256-sample ring; the comm task, at the live stream rate, applies the same Hann window,
block floating point and 256-point Q15 FFT as the Spectrum stage and sends 72 log-spaced
points (one bin each up to ~3.9 kHz, groups of 4-8 bins above). The reference is 1.76 dB
below the Spectrum stage's band reference, because a point is usually a single bin rather
than the whole Hann main lobe, so a sine reads its true dBFS level. Tapping the stage after
a filter shows that filter's effect; stages that do not change `samples[]` (analysis, render)
pass the signal on unchanged. A tap of a stage that is not in the running algorithm sends
no spectrum. Wire format: `protocol.md`, `LIVE_SUBSCRIBE`.
