import wave
import pandas as pd
import numpy as np
from pathlib import Path

# ── Config ────────────────────────────────────────────────
DATA_ROOT = Path("C:/Users/aleja/Royal Therapy/pillow-prototype/data/test")

# NIGHT_META = {
#     "n01": {"pillow": "3-layer memory foam",  "date": "2026-07-26"},
#     "n02": {"pillow": "3-layer memory foam",  "date": "2026-07-31"},
#     "n03": {"pillow": "3-layer memory foam",  "date": "2026-08-01"},
#     "n04": {"pillow": "3-layer memory foam", "date": "2026-08-02"},
#     "n05": {"pillow": "shredded memory foam", "date": "2026-08-04"},
#     "n06": {"pillow": "shredded memory foam", "date": "2026-08-05"},
#     "n07": {"pillow": "shredded memory foam", "date": "2026-08-06"},
#     "n08": {"pillow": "hotel gel microfiber", "date": "2026-08-09"},
#     "n09": {"pillow": "hotel gel microfiber", "date": "2026-08-10"},
#     "n10": {"pillow": "hotel gel microfiber", "date": "2026-08-11"},
#     "n11": {"pillow": "3-layer memory foam", "date": "2026-08-20"},
#     "n12": {"pillow": "3-layer memory foam", "date": "2026-08-21"},
#     "n13": {"pillow": "3-layer memory foam", "date": "2026-08-22"},
#     "n14": {"pillow": "3-layer memory foam", "date": "2026-08-25"},
#     "n15": {"pillow": "3-layer memory foam", "date": "2026-08-29"},
#     "n16": {"pillow": "3-layer memory foam", "date": "2026-08-30"},
#     "n17": {"pillow": "3-layer memory foam", "date": "2026-08-31"},
#     "n18": {"pillow": "hotel gel microfiber", "date": "2026-09-01"},
#     "n19": {"pillow": "hotel gel microfiber", "date": "2026-09-02"},
#     "n20": {"pillow": "hotel gel microfiber", "date": "2026-09-04"},
#     "n21": {"pillow": "hotel gel microfiber", "date": "2026-09-05"},
#     "n22": {"pillow": "hotel gel microfiber", "date": "2026-09-06"},
# }

NIGHT_META = {
    "n01": {"pillow": "3-layer memory foam",  "date": "2026-09-13"},
}

EXPECTED_SAMPLE_RATE  = 16000
EXPECTED_CHANNELS     = 1
EXPECTED_BIT_DEPTH    = 16
EXPECTED_SEGMENTS     = 7
SILENCE_THRESHOLD     = 50
SILENCE_WARN_PCT      = 80.0
CLICK_WINDOW_S        = 1.0
CLICK_THRESHOLD_MULT  = 8.0
MIN_CLICK_GAP_S       = 0.05
EMPTY_FILE_BYTES      = 200


def find_wav_files(night_dir: Path, night: str) -> tuple:
    """
    Find all WAV segments for a night in order h00 → h06.
    Returns (list of (hour_index, path) tuples, list of missing hours).
    Missing hours are detected by finding gaps in the h00-h06 sequence.
    A 0-byte file is reported separately from a missing file —
    both are failure modes but have different root causes.
    """
    wavs = []
    for f in sorted(night_dir.iterdir()):
        if f.suffix == ".wav" and f"MIC_{night}_h" in f.name:
            try:
                h = int(f.stem.split("_h")[-1])
                wavs.append((h, f))
            except ValueError:
                continue
    wavs = sorted(wavs, key=lambda x: x[0])

    # Detect missing hours in sequence
    if wavs:
        expected = set(range(wavs[0][0], wavs[-1][0] + 1))
        found    = set(h for h, _ in wavs)
        missing  = sorted(expected - found)
    else:
        missing = []

    return wavs, missing


def find_index_file(night_dir: Path, night: str) -> Path | None:
    for f in night_dir.iterdir():
        if f.suffix == ".csv" and f"MIC_{night}" in f.name:
            return f
    return None


def read_wav_samples(wav_path: Path) -> tuple:
    try:
        with wave.open(str(wav_path), "rb") as wf:
            n_channels  = wf.getnchannels()
            sample_rate = wf.getframerate()
            bit_depth   = wf.getsampwidth() * 8
            n_frames    = wf.getnframes()
            duration_s  = n_frames / sample_rate if sample_rate > 0 else 0
            raw         = wf.readframes(n_frames)

        if bit_depth == 16:
            samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
        elif bit_depth == 32:
            samples = np.frombuffer(raw, dtype=np.int32).astype(np.float32)
        else:
            samples = np.frombuffer(raw, dtype=np.uint8).astype(np.float32)

        return samples, sample_rate, n_channels, bit_depth, duration_s

    except Exception:
        return None, 0, 0, 0, 0.0


def analyze_wav_segment(wav_path: Path, hour_idx: int) -> dict:
    seg = {
        "hour":             hour_idx,
        "file":             wav_path.name,
        "file_size_mb":     0.0,
        "file_size_bytes":  0,
        "readable":         False,
        "sample_rate":      0,
        "channels":         0,
        "bit_depth":        0,
        "duration_s":       0.0,
        "format_ok":        False,
        "rms_amplitude":    0.0,
        "peak_amplitude":   0.0,
        "silence_pct":      0.0,
        "dc_offset":        0.0,
        "click_count":      0,
        "dynamic_range_db": 0.0,
        "empty":            False,
        # ── NEW: failure mode classification ──────────────
        # Distinguishes between:
        # - EMPTY: file exists, 0 bytes — firmware opened
        #          but never wrote (e.g. h01 rollover failure,
        #          ACC race condition equivalent for MIC)
        # - UNPATCHED: file has data but WAV header still has
        #              placeholder 0 for data size — happens
        #              when session ended without clean shutdown
        # - CORRUPT_HEADER: file unreadable — header malformed
        "failure_mode":     None,
        "seg_notes":        "",
    }

    file_size = wav_path.stat().st_size
    seg["file_size_bytes"] = file_size
    seg["file_size_mb"]    = round(file_size / 1_048_576, 2)

    # ── Empty file detection ──────────────────────────────
    # File is on card but firmware never wrote audio data.
    # Two known causes:
    # 1. Final segment: session timer fired, WAV opened for
    #    new hour but run ended before any audio was written
    # 2. Mid-session: SD write failure at segment rollover
    #    caused first write to fail silently
    if file_size < EMPTY_FILE_BYTES:
        seg["empty"]        = True
        seg["failure_mode"] = "EMPTY — firmware opened file but never wrote data"
        seg["seg_notes"]    = seg["failure_mode"]
        return seg

    samples, sr, ch, bd, dur = read_wav_samples(wav_path)

    if samples is None or len(samples) == 0:
        # ── Unpatched header detection ────────────────────
        # File has bytes but WAV library cannot read it.
        # Most likely the WAV header still has dataBytes=0
        # because patchWavHeader() was never called — this
        # happens when power is cut before the session ends
        # cleanly. The data IS present but the header is wrong.
        seg["failure_mode"] = (
            "CORRUPT/UNPATCHED HEADER — data may be present "
            "but WAV header was never patched. "
            "Run wav_repair.py to attempt recovery."
        )
        seg["seg_notes"] = seg["failure_mode"]
        return seg

    seg["readable"]    = True
    seg["sample_rate"] = sr
    seg["channels"]    = ch
    seg["bit_depth"]   = bd
    seg["duration_s"]  = round(dur, 1)
    seg["format_ok"]   = (
        sr == EXPECTED_SAMPLE_RATE and
        ch == EXPECTED_CHANNELS and
        bd == EXPECTED_BIT_DEPTH
    )

    max_val = 32767.0
    rms     = np.sqrt(np.mean(samples ** 2))
    seg["rms_amplitude"]  = round(rms / max_val, 5)
    seg["peak_amplitude"] = round(np.max(np.abs(samples)) / max_val, 5)
    seg["dc_offset"]      = round(np.mean(samples) / max_val, 6)

    silence_count    = np.sum(np.abs(samples) < SILENCE_THRESHOLD)
    seg["silence_pct"] = round(silence_count / len(samples) * 100, 2)

    # ── Click detection ───────────────────────────────────
    window_samples  = int(CLICK_WINDOW_S * sr)
    min_gap_samples = int(MIN_CLICK_GAP_S * sr)
    click_positions = []

    for i in range(0, len(samples) - window_samples, window_samples):
        window    = samples[i:i + window_samples]
        local_rms = np.sqrt(np.mean(window ** 2))
        if local_rms < 1.0:
            continue
        threshold = local_rms * CLICK_THRESHOLD_MULT
        spikes    = np.where(np.abs(window) > threshold)[0] + i
        click_positions.extend(spikes.tolist())

    click_count = 0
    last_click  = -min_gap_samples - 1
    for pos in sorted(click_positions):
        if pos - last_click > min_gap_samples:
            click_count += 1
            last_click   = pos
    seg["click_count"] = click_count

    # ── Dynamic range ─────────────────────────────────────
    window_rms_values = []
    for i in range(0, len(samples) - window_samples, window_samples):
        w_rms = np.sqrt(np.mean(samples[i:i + window_samples] ** 2))
        if w_rms > 10.0:
            window_rms_values.append(w_rms)

    if len(window_rms_values) >= 2:
        rms_max = max(window_rms_values)
        rms_min = min(window_rms_values)
        if rms_min > 0:
            seg["dynamic_range_db"] = round(
                20 * np.log10(rms_max / rms_min), 2
            )

    notes = []
    if not seg["format_ok"]:
        notes.append(
            f"Format mismatch: {sr}Hz/{ch}ch/{bd}bit"
        )
    if seg["silence_pct"] > SILENCE_WARN_PCT:
        notes.append(f"High silence: {seg['silence_pct']:.1f}%")
    if seg["peak_amplitude"] > 0.95:
        notes.append("Clipping detected")
    if abs(seg["dc_offset"]) > 0.01:
        notes.append(f"DC offset: {seg['dc_offset']:.4f}")
    if seg["click_count"] > 100:
        notes.append(f"High click count: {seg['click_count']}")

    seg["seg_notes"] = "; ".join(notes) if notes else "Clean"
    return seg


def audit_mic_night(night: str, night_dir: Path) -> dict:
    result = {
        "night":                  night,
        "date":                   NIGHT_META.get(night, {}).get("date", "unknown"),
        "pillow":                 NIGHT_META.get(night, {}).get("pillow", "unknown"),
        "mic_status":             "FAIL",
        # ── File completeness ─────────────────────────────
        "mic_segments_found":     0,
        "mic_segments_expected":  EXPECTED_SEGMENTS,
        # ── NEW: missing and empty segment tracking ───────
        # Missing: file not on SD card at all
        # Empty: file on card, 0 bytes — opened but not written
        # These are different failure modes with different fixes
        "mic_missing_hours":      [],
        "mic_empty_segments":     0,
        "mic_unpatched_segments": 0,
        "mic_total_mb":           0.0,
        "mic_total_dur_s":        0.0,
        # ── Format ───────────────────────────────────────
        "mic_format_ok":          False,
        # ── Audio quality ─────────────────────────────────
        "mic_rms_mean":           0.0,
        "mic_rms_min":            0.0,
        "mic_rms_max":            0.0,
        "mic_peak_max":           0.0,
        "mic_silence_pct_mean":   0.0,
        "mic_dc_offset_max":      0.0,
        "mic_total_clicks":       0,
        "mic_dynamic_range_db":   0.0,
        # ── Index CSV ─────────────────────────────────────
        "mic_index_present":      False,
        "mic_index_rows":         0,
        "mic_index_ok":           False,
        "segments":               [],
        "mic_notes":              "",
    }

    wav_files, missing_hours = find_wav_files(night_dir, night)
    result["mic_segments_found"] = len(wav_files)
    result["mic_missing_hours"]  = missing_hours

    if len(wav_files) == 0:
        result["mic_notes"] = "No WAV files found"
        return result

    seg_results    = []
    total_mb       = 0.0
    total_dur_s    = 0.0
    all_format_ok  = True
    rms_values     = []
    peak_values    = []
    silence_pcts   = []
    dc_offsets     = []
    total_clicks   = 0
    dr_values      = []
    empty_count    = 0
    unpatched_count = 0

    for hour_idx, wav_path in wav_files:
        seg = analyze_wav_segment(wav_path, hour_idx)
        seg_results.append(seg)
        total_mb    += seg["file_size_mb"]
        total_dur_s += seg["duration_s"]

        if seg["empty"]:
            empty_count += 1
            continue

        if seg["failure_mode"] and "UNPATCHED" in seg["failure_mode"]:
            unpatched_count += 1
            continue

        if not seg["readable"]:
            continue

        if not seg["format_ok"]:
            all_format_ok = False

        rms_values.append(seg["rms_amplitude"])
        peak_values.append(seg["peak_amplitude"])
        silence_pcts.append(seg["silence_pct"])
        dc_offsets.append(abs(seg["dc_offset"]))
        total_clicks += seg["click_count"]
        if seg["dynamic_range_db"] > 0:
            dr_values.append(seg["dynamic_range_db"])

    result["segments"]               = seg_results
    result["mic_empty_segments"]     = empty_count
    result["mic_unpatched_segments"] = unpatched_count
    result["mic_total_mb"]           = round(total_mb, 2)
    result["mic_total_dur_s"]        = round(total_dur_s, 1)
    result["mic_format_ok"]          = all_format_ok
    result["mic_total_clicks"]       = total_clicks

    if rms_values:
        result["mic_rms_mean"] = round(np.mean(rms_values), 5)
        result["mic_rms_min"]  = round(np.min(rms_values),  5)
        result["mic_rms_max"]  = round(np.max(rms_values),  5)
    if peak_values:
        result["mic_peak_max"] = round(np.max(peak_values), 5)
    if silence_pcts:
        result["mic_silence_pct_mean"] = round(np.mean(silence_pcts), 2)
    if dc_offsets:
        result["mic_dc_offset_max"] = round(np.max(dc_offsets), 6)
    if dr_values:
        result["mic_dynamic_range_db"] = round(np.mean(dr_values), 2)

    index_file = find_index_file(night_dir, night)
    if index_file:
        try:
            idx_df = pd.read_csv(index_file)
            result["mic_index_present"] = True
            result["mic_index_rows"]    = len(idx_df)
            result["mic_index_ok"]      = len(idx_df) == len(wav_files)
        except Exception:
            result["mic_index_present"] = False

    # ── Status ────────────────────────────────────────────
    notes = []

    if missing_hours:
        notes.append(
            f"MISSING SEGMENTS: h{missing_hours} not on SD card — "
            f"different from empty: file was never created"
        )

    if result["mic_segments_found"] < EXPECTED_SEGMENTS and not missing_hours:
        notes.append(
            f"Fewer segments than expected: "
            f"{result['mic_segments_found']} of {EXPECTED_SEGMENTS}"
        )

    if result["mic_empty_segments"] > 0:
        notes.append(
            f"EMPTY SEGMENTS: {result['mic_empty_segments']} file(s) "
            f"0 bytes — firmware opened but never wrote. "
            f"Causes: final hour timer, SD write failure at rollover"
        )

    if result["mic_unpatched_segments"] > 0:
        notes.append(
            f"UNPATCHED HEADERS: {result['mic_unpatched_segments']} "
            f"file(s) unreadable — power cut before session end. "
            f"Data may be recoverable with wav_repair.py"
        )

    if not result["mic_format_ok"]:
        notes.append("Format mismatch on one or more segments")

    if result["mic_silence_pct_mean"] > SILENCE_WARN_PCT:
        notes.append(
            f"High avg silence: {result['mic_silence_pct_mean']:.1f}%"
        )

    if result["mic_total_clicks"] > 500:
        notes.append(
            f"High click count: {result['mic_total_clicks']:,} total"
        )

    dur_hrs = result["mic_total_dur_s"] / 3600
    if dur_hrs < 6.0:
        notes.append(
            f"Short total duration: {dur_hrs:.2f} hrs"
        )

    result["mic_notes"]  = "; ".join(notes) if notes else "Clean"
    result["mic_status"] = "WARN" if any(
        k in " ".join(notes)
        for k in ["MISSING", "EMPTY", "UNPATCHED",
                  "mismatch", "High avg silence", "Short"]
    ) else "OK"

    return result


def run_mic_audit():
    print("=" * 60)
    print("MIC STREAM AUDIT — ALL NIGHTS")
    print("=" * 60)

    results       = []
    segment_rows  = []

    for night, meta in NIGHT_META.items():
        night_dir = DATA_ROOT / night
        if not night_dir.exists():
            print(f"[{night}] Directory not found: {night_dir}")
            continue

        print(f"\nAuditing {night} ({meta['pillow']})...")
        result = audit_mic_night(night, night_dir)
        results.append(result)

        print(f"  Segments found:    "
              f"{result['mic_segments_found']} / "
              f"{result['mic_segments_expected']}")

        # ── NEW: missing vs empty reporting ───────────────
        if result["mic_missing_hours"]:
            print(f"  Missing hours:     "
                  f"h{result['mic_missing_hours']} "
                  f"— NOT ON SD CARD (never created)")
        else:
            print(f"  Missing hours:     None")

        print(f"  Empty segments:    {result['mic_empty_segments']} "
              f"{'⚠ file exists, 0 bytes' if result['mic_empty_segments'] > 0 else ''}")
        print(f"  Unpatched headers: {result['mic_unpatched_segments']} "
              f"{'⚠ data may be recoverable' if result['mic_unpatched_segments'] > 0 else ''}")
        print(f"  Total size:        {result['mic_total_mb']:.2f} MB")
        print(f"  Total duration:    "
              f"{result['mic_total_dur_s']:.0f}s "
              f"({result['mic_total_dur_s']/3600:.2f} hrs)")
        print(f"  Format OK:         {result['mic_format_ok']}")
        print()
        print(f"  ── Audio quality ──────────────────────")
        print(f"  RMS mean:          {result['mic_rms_mean']:.5f} "
              f"(min {result['mic_rms_min']:.5f} / "
              f"max {result['mic_rms_max']:.5f})")
        print(f"  Peak max:          {result['mic_peak_max']:.5f}")
        print(f"  Silence mean:      {result['mic_silence_pct_mean']:.2f}%")
        print(f"  DC offset max:     {result['mic_dc_offset_max']:.6f}")
        print(f"  Total clicks:      {result['mic_total_clicks']:,}")
        print(f"  Dynamic range:     {result['mic_dynamic_range_db']:.2f} dB")
        print()
        print(f"  ── Index CSV ──────────────────────────")
        print(f"  Index present:     {result['mic_index_present']}")
        print(f"  Index rows:        {result['mic_index_rows']}")
        print(f"  Index OK:          {result['mic_index_ok']}")
        print()

        print(f"  Per-segment breakdown:")
        print(f"  {'h':>3}  {'MB':>7}  {'Dur(s)':>7}  "
              f"{'RMS':>8}  {'Peak':>7}  {'Sil%':>6}  "
              f"{'Clicks':>7}  {'DR(dB)':>7}  Notes")
        print("  " + "-" * 85)

        for seg in result["segments"]:
            if seg["empty"] or seg["failure_mode"]:
                print(f"  h{seg['hour']:02d}  "
                      f"{seg['file_size_mb']:>7.2f}  "
                      f"{'—':>7}  {'—':>8}  {'—':>7}  "
                      f"{'—':>6}  {'—':>7}  {'—':>7}  "
                      f"⚠ {seg['failure_mode'] or 'EMPTY'}")
                continue
            if not seg["readable"]:
                print(f"  h{seg['hour']:02d}  "
                      f"{seg['file_size_mb']:>7.2f}  UNREADABLE")
                continue
            print(f"  h{seg['hour']:02d}  "
                  f"{seg['file_size_mb']:>7.2f}  "
                  f"{seg['duration_s']:>7.0f}  "
                  f"{seg['rms_amplitude']:>8.5f}  "
                  f"{seg['peak_amplitude']:>7.5f}  "
                  f"{seg['silence_pct']:>6.1f}  "
                  f"{seg['click_count']:>7}  "
                  f"{seg['dynamic_range_db']:>7.2f}  "
                  f"{seg['seg_notes']}")

            segment_rows.append({
                "night":            night,
                "pillow":           meta["pillow"],
                "hour":             seg["hour"],
                "file":             seg["file"],
                "file_size_mb":     seg["file_size_mb"],
                "duration_s":       seg["duration_s"],
                "format_ok":        seg["format_ok"],
                "rms_amplitude":    seg["rms_amplitude"],
                "peak_amplitude":   seg["peak_amplitude"],
                "silence_pct":      seg["silence_pct"],
                "dc_offset":        seg["dc_offset"],
                "click_count":      seg["click_count"],
                "dynamic_range_db": seg["dynamic_range_db"],
                "failure_mode":     seg["failure_mode"],
                "seg_notes":        seg["seg_notes"],
            })

        print()
        print(f"  Status:            {result['mic_status']}")
        print(f"  Notes:             {result['mic_notes']}")

    df_results = pd.DataFrame(results)

    print("\n" + "=" * 60)
    print("MIC PER-PILLOW COMPARISON")
    print("=" * 60)

    for pillow in df_results["pillow"].unique():
        subset = df_results[df_results["pillow"] == pillow]
        nights = ", ".join(subset["night"].tolist())
        print(f"\n{pillow} (nights: {nights})")
        print(f"  Avg total size:      "
              f"{subset['mic_total_mb'].mean():.2f} MB/night")
        print(f"  Avg total duration:  "
              f"{subset['mic_total_dur_s'].mean()/3600:.2f} hrs/night")
        print(f"  Avg RMS:             "
              f"{subset['mic_rms_mean'].mean():.5f}")
        print(f"  Avg silence:         "
              f"{subset['mic_silence_pct_mean'].mean():.2f}%")
        print(f"  Avg total clicks:    "
              f"{subset['mic_total_clicks'].mean():.0f} clicks/night")
        print(f"  Avg dynamic range:   "
              f"{subset['mic_dynamic_range_db'].mean():.2f} dB")
        print(f"  Total empty segs:    "
              f"{subset['mic_empty_segments'].sum()}")
        print(f"  Total missing hrs:   "
              f"{subset['mic_missing_hours'].apply(len).sum()}")
        print(f"  Total unpatched:     "
              f"{subset['mic_unpatched_segments'].sum()}")

    night_output = DATA_ROOT / "mic_audit.csv"
    seg_output   = DATA_ROOT / "mic_audit_segments.csv"

    df_results.drop(columns=["segments"]).to_csv(
        night_output, index=False
    )
    pd.DataFrame(segment_rows).to_csv(seg_output, index=False)

    print(f"\nMIC night audit saved to:   {night_output}")
    print(f"MIC segment audit saved to: {seg_output}")
    print("=" * 60)

    return df_results


if __name__ == "__main__":
    run_mic_audit()