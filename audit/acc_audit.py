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

# ── Thresholds ────────────────────────────────────────────
GAP_WARN_MS              = 40
GAP_SEVERE_MS            = 200
EXPECTED_HZ              = 100
EXPECTED_ROWS            = 2520000
EXPECTED_DUR_S           = 25200
HZ_TOLERANCE             = 0.10
BIAS_WARN_G              = 0.05
MOVEMENT_THRESH_G        = 1.02
MOVEMENT_MIN_DUR_S       = 0.1
EMPTY_FILE_BYTES         = 200

# ── Corrupt epoch detection ───────────────────────────────
# A ts_epoch_ms value is corrupt if it deviates from its
# rolling median by more than this threshold in milliseconds.
# Caused by I2C bus race condition between Core 0 (BME688)
# and Core 1 (rtc.now()) returning garbage DS3231 reads.
CORRUPT_EPOCH_THRESHOLD_MS = 10000   # 10 seconds deviation
CORRUPT_EPOCH_WINDOW       = 11      # rolling median window


def find_acc_file(night_dir: Path, night: str) -> Path | None:
    for f in night_dir.iterdir():
        if f.suffix == ".csv" and f"ACC_{night}" in f.name:
            return f
    return None


def is_valid_iso(val) -> bool:
    try:
        s = str(val).strip()
        return len(s) >= 19 and s[:4].isdigit()
    except Exception:
        return False


def count_movement_events(
    magnitude: pd.Series,
    timestamps_ms: pd.Series,
    threshold_g: float,
    min_dur_s: float
) -> dict:
    above       = magnitude > threshold_g
    total_s     = (timestamps_ms.iloc[-1] - timestamps_ms.iloc[0]) / 1000.0
    events      = []
    in_event    = False
    event_start = None

    for i in range(len(above)):
        if above.iloc[i] and not in_event:
            in_event    = True
            event_start = timestamps_ms.iloc[i]
        elif not above.iloc[i] and in_event:
            in_event = False
            dur_s = (timestamps_ms.iloc[i] - event_start) / 1000.0
            if dur_s >= min_dur_s:
                events.append(dur_s)

    if in_event:
        dur_s = (timestamps_ms.iloc[-1] - event_start) / 1000.0
        if dur_s >= min_dur_s:
            events.append(dur_s)

    total_active_s = sum(events)
    return {
        "event_count":     len(events),
        "total_active_s":  round(total_active_s, 1),
        "avg_event_dur_s": round(np.mean(events), 2) if events else 0.0,
        "max_event_dur_s": round(max(events), 2)     if events else 0.0,
        "active_pct":      round(
            (total_active_s / total_s) * 100, 2
        ) if total_s > 0 else 0.0,
    }


def audit_acc_night(night: str, night_dir: Path) -> dict:
    result = {
        "night":                    night,
        "date":                     NIGHT_META.get(night, {}).get("date", "unknown"),
        "pillow":                   NIGHT_META.get(night, {}).get("pillow", "unknown"),
        "acc_file":                 None,
        "acc_file_bytes":           0,
        "acc_status":               "FAIL",
        # ── Completeness ──────────────────────────────────
        "acc_rows":                 0,
        "acc_rows_clean":           0,
        "acc_dur_s":                0.0,
        "acc_effective_hz":         0.0,
        "acc_hz_vs_spec_pct":       0.0,
        # ── Timestamp gaps ────────────────────────────────
        "acc_mean_gap_ms":          0.0,
        "acc_gaps_over_40ms":       0,
        "acc_gaps_over_200ms":      0,
        "acc_max_gap_ms":           0.0,
        "acc_gap_40ms_pct":         0.0,
        # ── Timestamp quality ─────────────────────────────
        "acc_backward_jumps":       0,
        "acc_corrupt_iso_rows":     0,
        # ── NEW: corrupt epoch rows ───────────────────────
        # Rows where ts_epoch_ms deviates >10 seconds from
        # rolling median — caused by I2C race condition
        # between Core 0 BME688 and Core 1 rtc.now() call.
        # These rows have valid ISO timestamps but wrong
        # epoch values — they corrupt duration and Hz calcs.
        "acc_corrupt_epoch_rows":   0,
        "acc_corrupt_epoch_pct":    0.0,
        # ── Magnitude stats ───────────────────────────────
        "acc_mag_mean":             0.0,
        "acc_mag_std":              0.0,
        "acc_mag_max":              0.0,
        # ── Bias check ────────────────────────────────────
        "acc_bias_error_g":         0.0,
        "acc_bias_ok":              False,
        # ── Per-axis stats ────────────────────────────────
        "acc_x_std":                0.0,
        "acc_y_std":                0.0,
        "acc_z_std":                0.0,
        # ── Movement events ───────────────────────────────
        "acc_movement_events":      0,
        "acc_movement_total_s":     0.0,
        "acc_movement_avg_dur_s":   0.0,
        "acc_movement_max_dur_s":   0.0,
        "acc_movement_active_pct":  0.0,
        # ── Micro-movement ────────────────────────────────
        "acc_micro_movement_pct":   0.0,
        "acc_notes":                "",
    }

    acc_file = find_acc_file(night_dir, night)
    if acc_file is None:
        result["acc_notes"] = "ACC file not found"
        return result

    result["acc_file"]       = acc_file.name
    result["acc_file_bytes"] = acc_file.stat().st_size

    if result["acc_file_bytes"] < EMPTY_FILE_BYTES:
        result["acc_status"] = "FAIL"
        result["acc_notes"]  = (
            f"File effectively empty: {result['acc_file_bytes']} bytes. "
            f"Firmware opened file but never wrote data — "
            f"likely I2C bus race condition at session start."
        )
        return result

    try:
        df = pd.read_csv(acc_file)
    except Exception as e:
        result["acc_notes"] = f"Failed to read CSV: {e}"
        return result

    required_cols = {"ts_epoch_ms", "ts_iso",
                     "accel_x_g", "accel_y_g", "accel_z_g"}
    if not required_cols.issubset(df.columns):
        result["acc_notes"] = (
            f"Missing columns: {required_cols - set(df.columns)}"
        )
        return result

    if len(df) == 0:
        result["acc_notes"] = "File is empty"
        return result

    result["acc_rows"] = len(df)

    # ── ISO corruption check ──────────────────────────────
    corrupt_iso_mask            = ~df["ts_iso"].apply(is_valid_iso)

    # ── Raw backward jump count — BEFORE any filtering ───
    # Checks the original file order, not the sorted order.
    # This matches what Mariia's analysis pipeline sees when
    # reading the file sequentially row by row.
    raw_gaps     = df["ts_epoch_ms"].diff().dropna()
    raw_backward = int((raw_gaps < 0).sum())
    result["acc_raw_backward_jumps"] = raw_backward
    print(f"  Raw backward jumps (unfiltered): {raw_backward:,}")
    
    result["acc_corrupt_iso_rows"] = int(corrupt_iso_mask.sum())
    df_clean = df[~corrupt_iso_mask].copy()

    if len(df_clean) == 0:
        result["acc_notes"] = "All rows corrupt ISO"
        return result

    df_clean = df_clean.sort_values("ts_epoch_ms").reset_index(drop=True)

    # ── Corrupt epoch detection ───────────────────────────
    # The rolling median of ts_epoch_ms represents the
    # expected value at each point in time. A row that
    # deviates by more than 10 seconds from this median
    # has a corrupt epoch value — the DS3231 returned
    # garbage during an I2C collision with the BME688.
    # We flag and exclude these rows before calculating
    # duration and Hz so they don't skew the results.
    rolling_median = df_clean["ts_epoch_ms"].rolling(
        window=CORRUPT_EPOCH_WINDOW,
        center=True,
        min_periods=1
    ).median()

    corrupt_epoch_mask = (
        (df_clean["ts_epoch_ms"] - rolling_median).abs()
        > CORRUPT_EPOCH_THRESHOLD_MS
    )
    result["acc_corrupt_epoch_rows"] = int(corrupt_epoch_mask.sum())
    result["acc_corrupt_epoch_pct"]  = round(
        result["acc_corrupt_epoch_rows"] / len(df_clean) * 100, 3
    )

    # Exclude corrupt epoch rows for timing analysis
    df_timing = df_clean[~corrupt_epoch_mask].copy()
    result["acc_rows_clean"] = len(df_timing)

    # ── Duration — use clean rows only ───────────────────
    # Previously used raw min/max after sort which was
    # dominated by corrupt outlier values. Now uses only
    # rows with valid epoch timestamps.
    if len(df_timing) > 1:
        t_start_ms = df_timing["ts_epoch_ms"].iloc[0]
        t_end_ms   = df_timing["ts_epoch_ms"].iloc[-1]
        dur_s      = (t_end_ms - t_start_ms) / 1000.0
        result["acc_dur_s"] = round(dur_s, 1)

        if dur_s > 0:
            eff_hz = len(df_timing) / dur_s
            result["acc_effective_hz"]   = round(eff_hz, 2)
            result["acc_hz_vs_spec_pct"] = round(
                (eff_hz / EXPECTED_HZ) * 100, 1
            )

    # ── Timestamp gap analysis — clean rows only ──────────
    gaps = df_timing["ts_epoch_ms"].diff().dropna()
    if len(gaps) > 0:
        result["acc_mean_gap_ms"]     = round(gaps.mean(), 2)
        result["acc_max_gap_ms"]      = round(gaps.max(), 2)
        result["acc_gaps_over_40ms"]  = int((gaps > GAP_WARN_MS).sum())
        result["acc_gaps_over_200ms"] = int((gaps > GAP_SEVERE_MS).sum())
        result["acc_gap_40ms_pct"]    = round(
            result["acc_gaps_over_40ms"] / len(gaps) * 100, 2
        )
        result["acc_backward_jumps"]  = int((gaps < 0).sum())

    # ── Magnitude stats — use all clean ISO rows ──────────
    # Signal quality uses all rows regardless of epoch
    # corruption since the accelerometer readings themselves
    # are valid — only the timestamp is wrong on bad rows
    df_clean["magnitude"] = np.sqrt(
        df_clean["accel_x_g"]**2 +
        df_clean["accel_y_g"]**2 +
        df_clean["accel_z_g"]**2
    )
    result["acc_mag_mean"] = round(df_clean["magnitude"].mean(), 4)
    result["acc_mag_std"]  = round(df_clean["magnitude"].std(),  4)
    result["acc_mag_max"]  = round(df_clean["magnitude"].max(),  4)

    # ── Bias check ────────────────────────────────────────
    bias_error = abs(result["acc_mag_mean"] - 1.0)
    result["acc_bias_error_g"] = round(bias_error, 4)
    result["acc_bias_ok"]      = bias_error <= BIAS_WARN_G

    # ── Per-axis std ──────────────────────────────────────
    result["acc_x_std"] = round(df_clean["accel_x_g"].std(), 4)
    result["acc_y_std"] = round(df_clean["accel_y_g"].std(), 4)
    result["acc_z_std"] = round(df_clean["accel_z_g"].std(), 4)

    # ── Movement events — use clean timing rows ───────────
    if len(df_timing) > 1:
        movement = count_movement_events(
            magnitude=df_clean.loc[df_timing.index, "magnitude"],
            timestamps_ms=df_timing["ts_epoch_ms"],
            threshold_g=MOVEMENT_THRESH_G,
            min_dur_s=MOVEMENT_MIN_DUR_S,
        )
        result["acc_movement_events"]     = movement["event_count"]
        result["acc_movement_total_s"]    = movement["total_active_s"]
        result["acc_movement_avg_dur_s"]  = movement["avg_event_dur_s"]
        result["acc_movement_max_dur_s"]  = movement["max_event_dur_s"]
        result["acc_movement_active_pct"] = movement["active_pct"]

    # ── Micro-movement ────────────────────────────────────
    micro_threshold = (
        result["acc_mag_mean"] + 2 * result["acc_mag_std"]
    )
    result["acc_micro_movement_pct"] = round(
        (df_clean["magnitude"] > micro_threshold).sum()
        / len(df_clean) * 100, 3
    )

    # ── Status ────────────────────────────────────────────
    notes = []

    if result["acc_file_bytes"] < EMPTY_FILE_BYTES:
        notes.append("EMPTY FILE — firmware never wrote data")

    if result["acc_dur_s"] < EXPECTED_DUR_S * 0.95:
        notes.append(
            f"Short session: {result['acc_dur_s']:.0f}s "
            f"vs expected {EXPECTED_DUR_S}s"
        )

    hz_ratio = result["acc_effective_hz"] / EXPECTED_HZ if EXPECTED_HZ else 0
    if hz_ratio < (1 - HZ_TOLERANCE):
        notes.append(
            f"Low Hz: {result['acc_effective_hz']:.1f} Hz "
            f"({result['acc_hz_vs_spec_pct']:.1f}% of spec)"
        )

    if result["acc_gaps_over_200ms"] > 0:
        notes.append(
            f"{result['acc_gaps_over_200ms']} severe gaps >200ms"
        )

    if result["acc_backward_jumps"] > 0:
        notes.append(
            f"TIMESTAMP BUG: {result['acc_backward_jumps']:,} "
            f"backward ts_epoch_ms jumps"
        )

    if result["acc_corrupt_epoch_rows"] > 0:
        notes.append(
            f"CORRUPT EPOCH: {result['acc_corrupt_epoch_rows']:,} rows "
            f"({result['acc_corrupt_epoch_pct']:.2f}%) have ts_epoch_ms "
            f">10s from rolling median — I2C race condition on rtc.now()"
        )

    if result["acc_corrupt_iso_rows"] > 0:
        notes.append(
            f"ISO CORRUPTION: {result['acc_corrupt_iso_rows']} "
            f"garbled ts_iso rows"
        )

    if not result["acc_bias_ok"]:
        notes.append(
            f"Bias error {result['acc_bias_error_g']:.4f}g — "
            f"sensor may have shifted"
        )

    row_pct = (len(df_timing) / EXPECTED_ROWS) * 100
    notes.append(
        f"Clean row count: {row_pct:.1f}% of theoretical 100 Hz"
    )

    result["acc_notes"]  = "; ".join(notes) if notes else "Clean"
    result["acc_status"] = "WARN" if any(
        k in " ".join(notes)
        for k in ["Short", "severe", "Bias", "TIMESTAMP",
                  "CORRUPT", "EMPTY"]
    ) else "OK"

    return result


def run_acc_audit():
    print("=" * 60)
    print("ACC STREAM AUDIT — ALL NIGHTS")
    print("=" * 60)

    results = []

    for night, meta in NIGHT_META.items():
        night_dir = DATA_ROOT / night
        if not night_dir.exists():
            print(f"[{night}] Directory not found: {night_dir}")
            continue

        print(f"\nAuditing {night} ({meta['pillow']})...")
        result = audit_acc_night(night, night_dir)
        results.append(result)

        print(f"  File:              {result['acc_file']}")
        print(f"  File size:         {result['acc_file_bytes']:,} bytes")
        print(f"  Rows (total):      {result['acc_rows']:,}")
        print(f"  Rows (clean):      {result['acc_rows_clean']:,}")
        print(f"  Duration:          {result['acc_dur_s']:.0f}s "
              f"({result['acc_dur_s']/3600:.2f} hrs)")
        print(f"  Effective Hz:      {result['acc_effective_hz']:.1f} Hz "
              f"({result['acc_hz_vs_spec_pct']:.1f}% of 100 Hz spec)")
        print(f"  Mean gap:          {result['acc_mean_gap_ms']:.1f} ms")
        print(f"  Gaps >40ms:        {result['acc_gaps_over_40ms']:,} "
              f"({result['acc_gap_40ms_pct']:.1f}% of samples)")
        print(f"  Gaps >200ms:       {result['acc_gaps_over_200ms']:,}")
        print(f"  Max gap:           {result['acc_max_gap_ms']:.0f} ms")
        print()
        print(f"  ── Timestamp quality ──────────────────")
        print(f"  Backward jumps:    {result['acc_backward_jumps']:,}  "
              f"{'WARN: TIMESTAMP BUG' if result['acc_backward_jumps'] > 0 else 'OK'}")
        print(f"  Corrupt ISO rows:  {result['acc_corrupt_iso_rows']:,}  "
              f"{'WARN: CORRUPTION' if result['acc_corrupt_iso_rows'] > 0 else 'OK'}")
        print(f"  Corrupt epoch rows:{result['acc_corrupt_epoch_rows']:,}  "
              f"({result['acc_corrupt_epoch_pct']:.2f}%)  "
              f"{'WARN: I2C RACE CONDITION' if result['acc_corrupt_epoch_rows'] > 0 else 'OK'}")
        print()
        print(f"  ── Signal quality ─────────────────────")
        print(f"  Mag mean:          {result['acc_mag_mean']:.4f} g")
        print(f"  Mag std:           {result['acc_mag_std']:.4f} g")
        print(f"  Mag max:           {result['acc_mag_max']:.4f} g")
        print(f"  Bias error:        {result['acc_bias_error_g']:.4f} g "
              f"({'OK' if result['acc_bias_ok'] else 'WARN'})")
        print(f"  X/Y/Z std:         {result['acc_x_std']:.4f} / "
              f"{result['acc_y_std']:.4f} / {result['acc_z_std']:.4f} g")
        print(f"  Micro-movement:    {result['acc_micro_movement_pct']:.3f}% "
              f">2s from mean")
        print()
        print(f"  ── Movement events ────────────────────")
        print(f"  Events:            {result['acc_movement_events']}")
        print(f"  Total active:      {result['acc_movement_total_s']:.1f}s "
              f"({result['acc_movement_active_pct']:.2f}%)")
        print(f"  Avg duration:      {result['acc_movement_avg_dur_s']:.2f}s")
        print(f"  Max duration:      {result['acc_movement_max_dur_s']:.2f}s")
        print()
        print(f"  Status:            {result['acc_status']}")
        print(f"  Notes:             {result['acc_notes']}")

    df_results = pd.DataFrame(results)

    print("\n" + "=" * 60)
    print("ACC PER-PILLOW COMPARISON")
    print("=" * 60)

    for pillow in df_results["pillow"].unique():
        subset = df_results[df_results["pillow"] == pillow]
        nights = ", ".join(subset["night"].tolist())
        print(f"\n{pillow} (nights: {nights})")
        print(f"  Avg effective Hz:        "
              f"{subset['acc_effective_hz'].mean():.1f}")
        print(f"  Avg clean rows:          "
              f"{subset['acc_rows_clean'].mean():,.0f}")
        print(f"  Avg corrupt epoch rows:  "
              f"{subset['acc_corrupt_epoch_rows'].mean():.0f}")
        print(f"  Avg corrupt epoch %:     "
              f"{subset['acc_corrupt_epoch_pct'].mean():.2f}%")
        print(f"  Avg bias error:          "
              f"{subset['acc_bias_error_g'].mean():.4f} g")
        print(f"  Avg magnitude std:       "
              f"{subset['acc_mag_std'].mean():.4f} g")
        print(f"  Avg movement events:     "
              f"{subset['acc_movement_events'].mean():.1f} events/night")
        print(f"  Avg movement total:      "
              f"{subset['acc_movement_total_s'].mean():.1f}s/night")

    output_path = DATA_ROOT / "acc_audit.csv"
    df_results.to_csv(output_path, index=False)
    print(f"\nACC audit saved to: {output_path}")
    print("=" * 60)

    return df_results


if __name__ == "__main__":
    run_acc_audit()