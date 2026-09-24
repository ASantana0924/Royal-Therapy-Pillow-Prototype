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

EXPECTED_ROWS        = 25200
EXPECTED_DUR_S       = 25200
ROW_TOLERANCE        = 0.01
MAX_GAP_WARN_S       = 2
TEMP_MIN_C           = 15.0
TEMP_MAX_C           = 35.0
HUMIDITY_MIN_PCT     = 20.0
HUMIDITY_MAX_PCT     = 80.0
PRESSURE_STABLE_HPA  = 5.0
WARMUP_ROWS          = 30
EMPTY_FILE_BYTES     = 200
GAS_VALID_MIN_OHM    = 100

# ── Corrupt epoch detection ───────────────────────────────
CORRUPT_EPOCH_THRESHOLD_MS = 10000
CORRUPT_EPOCH_WINDOW       = 11


def find_env_file(night_dir: Path, night: str) -> Path | None:
    for f in night_dir.iterdir():
        if f.suffix == ".csv" and f"ENV_{night}" in f.name:
            return f
    return None


def is_valid_iso(val) -> bool:
    try:
        s = str(val).strip()
        return len(s) >= 19 and s[:4].isdigit()
    except Exception:
        return False


def audit_env_night(night: str, night_dir: Path) -> dict:
    result = {
        "night":                         night,
        "date":                          NIGHT_META.get(night, {}).get("date", "unknown"),
        "pillow":                        NIGHT_META.get(night, {}).get("pillow", "unknown"),
        "env_file":                      None,
        "env_file_bytes":                0,
        "env_status":                    "FAIL",
        # ── Completeness ──────────────────────────────────
        "env_rows":                      0,
        "env_rows_clean":                0,
        "env_row_pct":                   0.0,
        "env_dur_s":                     0.0,
        # ── Timestamp quality ─────────────────────────────
        "env_mean_gap_s":                0.0,
        "env_max_gap_s":                 0.0,
        "env_gaps_over_2s":              0,
        "env_backward_jumps":            0,
        "env_corrupt_iso_rows":          0,
        # ── NEW: corrupt epoch rows ───────────────────────
        # Same I2C race condition as ACC stream.
        # ENV calls rtc.now() in Core 1 while Core 0
        # BME688 task uses the same I2C bus — occasional
        # DS3231 reads return garbage epoch values.
        "env_corrupt_epoch_rows":        0,
        "env_corrupt_epoch_pct":         0.0,
        # ── Temperature ───────────────────────────────────
        "env_temp_min_c":                0.0,
        "env_temp_max_c":                0.0,
        "env_temp_mean_c":               0.0,
        "env_temp_std_c":                0.0,
        "env_temp_range_c":              0.0,
        "env_temp_trend_c_per_hr":       0.0,
        "env_temp_in_range":             False,
        # ── Humidity ──────────────────────────────────────
        "env_humidity_min_pct":          0.0,
        "env_humidity_max_pct":          0.0,
        "env_humidity_mean_pct":         0.0,
        "env_humidity_std_pct":          0.0,
        "env_humidity_range_pct":        0.0,
        "env_humidity_trend_pct_per_hr": 0.0,
        "env_humidity_in_range":         False,
        # ── Pressure ──────────────────────────────────────
        "env_pressure_min_hpa":          0.0,
        "env_pressure_max_hpa":          0.0,
        "env_pressure_mean_hpa":         0.0,
        "env_pressure_range_hpa":        0.0,
        "env_pressure_stable":           False,
        # ── Gas resistance ────────────────────────────────
        "env_gas_zero_pct":              0.0,
        "env_gas_nonzero_rows":          0,
        "env_gas_valid_rows":            0,
        "env_gas_mean_ohm":              0.0,
        "env_gas_min_ohm":               0.0,
        "env_gas_max_ohm":               0.0,
        # ── IAQ columns ───────────────────────────────────
        "env_iaq_col_present":           False,
        "env_iaq_all_blank":             False,
        "env_notes":                     "",
    }

    env_file = find_env_file(night_dir, night)
    if env_file is None:
        result["env_notes"] = "ENV file not found"
        return result

    result["env_file"]       = env_file.name
    result["env_file_bytes"] = env_file.stat().st_size

    if result["env_file_bytes"] < EMPTY_FILE_BYTES:
        result["env_status"] = "FAIL"
        result["env_notes"]  = (
            f"File effectively empty: {result['env_file_bytes']} bytes"
        )
        return result

    try:
        df = pd.read_csv(env_file)
    except Exception as e:
        result["env_notes"] = f"Failed to read CSV: {e}"
        return result

    required_cols = {
        "ts_epoch_ms", "ts_iso", "temp_C",
        "humidity_pct", "pressure_hPa", "gas_resistance_ohm"
    }
    if not required_cols.issubset(df.columns):
        result["env_notes"] = (
            f"Missing columns: {required_cols - set(df.columns)}"
        )
        return result

    if len(df) == 0:
        result["env_notes"] = "File is empty"
        return result

    # ── ISO corruption check ──────────────────────────────
    corrupt_iso_mask              = ~df["ts_iso"].apply(is_valid_iso)
    result["env_corrupt_iso_rows"] = int(corrupt_iso_mask.sum())
    df_clean = df[~corrupt_iso_mask].copy()

    if len(df_clean) == 0:
        result["env_notes"] = "All rows corrupt ISO"
        return result

    df_clean = df_clean.sort_values("ts_epoch_ms").reset_index(drop=True)
    result["env_rows"] = len(df_clean)

    # ── Corrupt epoch detection ───────────────────────────
    rolling_median = df_clean["ts_epoch_ms"].rolling(
        window=CORRUPT_EPOCH_WINDOW,
        center=True,
        min_periods=1
    ).median()

    corrupt_epoch_mask = (
        (df_clean["ts_epoch_ms"] - rolling_median).abs()
        > CORRUPT_EPOCH_THRESHOLD_MS
    )
    result["env_corrupt_epoch_rows"] = int(corrupt_epoch_mask.sum())
    result["env_corrupt_epoch_pct"]  = round(
        result["env_corrupt_epoch_rows"] / len(df_clean) * 100, 3
    )

    # Use only clean epoch rows for timing analysis
    df_timing = df_clean[~corrupt_epoch_mask].copy()
    result["env_rows_clean"] = len(df_timing)
    result["env_row_pct"]    = round(
        len(df_timing) / EXPECTED_ROWS * 100, 2
    )

    if len(df_timing) > 1:
        t_start_ms = df_timing["ts_epoch_ms"].iloc[0]
        t_end_ms   = df_timing["ts_epoch_ms"].iloc[-1]
        dur_s      = (t_end_ms - t_start_ms) / 1000.0
        result["env_dur_s"] = round(dur_s, 1)

    # ── Timestamp gap analysis — clean rows only ──────────
    gaps_ms = df_timing["ts_epoch_ms"].diff().dropna()
    gaps_s  = gaps_ms / 1000.0
    if len(gaps_s) > 0:
        result["env_mean_gap_s"]   = round(gaps_s.mean(), 3)
        result["env_max_gap_s"]    = round(gaps_s.max(),  3)
        result["env_gaps_over_2s"] = int((gaps_s > MAX_GAP_WARN_S).sum())
        result["env_backward_jumps"] = int((gaps_ms < 0).sum())

    # ── Temperature ───────────────────────────────────────
    temp = df_clean["temp_C"].iloc[WARMUP_ROWS:]
    result["env_temp_min_c"]    = round(temp.min(),  2)
    result["env_temp_max_c"]    = round(temp.max(),  2)
    result["env_temp_mean_c"]   = round(temp.mean(), 2)
    result["env_temp_std_c"]    = round(temp.std(),  3)
    result["env_temp_range_c"]  = round(temp.max() - temp.min(), 2)
    result["env_temp_in_range"] = (
        TEMP_MIN_C <= temp.min() and temp.max() <= TEMP_MAX_C
    )

    if len(df_timing) > WARMUP_ROWS:
        t_start_ms = df_timing["ts_epoch_ms"].iloc[0]
        time_hrs   = (
            df_timing["ts_epoch_ms"].iloc[WARMUP_ROWS:] - t_start_ms
        ) / 3_600_000.0
        temp_vals  = df_clean["temp_C"].iloc[WARMUP_ROWS:].values[
            :len(time_hrs)
        ]
        if len(time_hrs) == len(temp_vals) and len(time_hrs) > 1:
            temp_coeff = np.polyfit(time_hrs, temp_vals, 1)
            result["env_temp_trend_c_per_hr"] = round(temp_coeff[0], 4)

    # ── Humidity ──────────────────────────────────────────
    humidity = df_clean["humidity_pct"].iloc[WARMUP_ROWS:]
    result["env_humidity_min_pct"]   = round(humidity.min(),  2)
    result["env_humidity_max_pct"]   = round(humidity.max(),  2)
    result["env_humidity_mean_pct"]  = round(humidity.mean(), 2)
    result["env_humidity_std_pct"]   = round(humidity.std(),  3)
    result["env_humidity_range_pct"] = round(
        humidity.max() - humidity.min(), 2
    )
    result["env_humidity_in_range"] = (
        HUMIDITY_MIN_PCT <= humidity.min() and
        humidity.max() <= HUMIDITY_MAX_PCT
    )

    if len(df_timing) > WARMUP_ROWS:
        hum_vals = df_clean["humidity_pct"].iloc[WARMUP_ROWS:].values[
            :len(time_hrs)
        ]
        if len(time_hrs) == len(hum_vals) and len(time_hrs) > 1:
            hum_coeff = np.polyfit(time_hrs, hum_vals, 1)
            result["env_humidity_trend_pct_per_hr"] = round(
                hum_coeff[0], 4
            )

    # ── Pressure ──────────────────────────────────────────
    pressure = df_clean["pressure_hPa"].iloc[WARMUP_ROWS:]
    result["env_pressure_min_hpa"]   = round(pressure.min(),  2)
    result["env_pressure_max_hpa"]   = round(pressure.max(),  2)
    result["env_pressure_mean_hpa"]  = round(pressure.mean(), 2)
    p_range = pressure.max() - pressure.min()
    result["env_pressure_range_hpa"] = round(p_range, 2)
    result["env_pressure_stable"]    = p_range <= PRESSURE_STABLE_HPA

    # ── Gas resistance ────────────────────────────────────
    gas       = df_clean["gas_resistance_ohm"]
    nonzero   = (gas > 0).sum()
    valid_gas = gas[gas >= GAS_VALID_MIN_OHM]

    result["env_gas_nonzero_rows"] = int(nonzero)
    result["env_gas_valid_rows"]   = int(len(valid_gas))
    result["env_gas_zero_pct"]     = round(
        (1 - nonzero / len(gas)) * 100, 2
    )
    result["env_gas_mean_ohm"] = round(valid_gas.mean(), 0) \
        if len(valid_gas) > 0 else 0.0
    result["env_gas_min_ohm"]  = round(valid_gas.min(), 0) \
        if len(valid_gas) > 0 else 0.0
    result["env_gas_max_ohm"]  = round(valid_gas.max(), 0) \
        if len(valid_gas) > 0 else 0.0

    # ── IAQ columns ───────────────────────────────────────
    iaq_present = "iaq" in df_clean.columns and \
                  "iaq_accuracy" in df_clean.columns
    result["env_iaq_col_present"] = iaq_present
    if iaq_present:
        result["env_iaq_all_blank"] = (
            df_clean["iaq"].isna().all() and
            df_clean["iaq_accuracy"].isna().all()
        )

    # ── Flatline detection ────────────────────────────────
    FLATLINE_WINDOW = 600
    temp_flatline = False
    hum_flatline  = False
    if len(df_clean) > FLATLINE_WINDOW:
        for i in range(len(df_clean) - FLATLINE_WINDOW):
            if df_clean["temp_C"].iloc[
                i:i + FLATLINE_WINDOW
            ].std() < 0.01:
                temp_flatline = True
                break
        for i in range(len(df_clean) - FLATLINE_WINDOW):
            if df_clean["humidity_pct"].iloc[
                i:i + FLATLINE_WINDOW
            ].std() < 0.01:
                hum_flatline = True
                break

    # ── Status ────────────────────────────────────────────
    notes = []

    if result["env_file_bytes"] < EMPTY_FILE_BYTES:
        notes.append("EMPTY FILE")

    if result["env_dur_s"] < EXPECTED_DUR_S * 0.95:
        notes.append(f"Short session: {result['env_dur_s']:.0f}s")

    if result["env_corrupt_epoch_rows"] > 0:
        notes.append(
            f"CORRUPT EPOCH: {result['env_corrupt_epoch_rows']:,} rows "
            f"({result['env_corrupt_epoch_pct']:.2f}%) — "
            f"I2C race condition on rtc.now()"
        )

    if result["env_backward_jumps"] > 0:
        notes.append(
            f"TIMESTAMP BUG: {result['env_backward_jumps']:,} "
            f"backward jumps"
        )

    if result["env_corrupt_iso_rows"] > 0:
        notes.append(
            f"ISO CORRUPTION: {result['env_corrupt_iso_rows']} rows"
        )

    if result["env_gaps_over_2s"] > 0:
        notes.append(
            f"{result['env_gaps_over_2s']} gaps >2s"
        )

    if not result["env_temp_in_range"]:
        notes.append(
            f"Temp out of range: {result['env_temp_min_c']}–"
            f"{result['env_temp_max_c']}C"
        )

    if not result["env_pressure_stable"]:
        notes.append(
            f"Pressure unstable: {result['env_pressure_range_hpa']} hPa"
        )

    if result["env_gas_zero_pct"] == 100.0:
        notes.append(
            "Gas resistance all zero — old firmware or heater fault"
        )

    if temp_flatline:
        notes.append("Temp flatline >10 min detected")
    if hum_flatline:
        notes.append("Humidity flatline >10 min detected")

    result["env_notes"]  = "; ".join(notes) if notes else "Clean"
    result["env_status"] = "WARN" if any(
        k in " ".join(notes)
        for k in ["Short", "gaps >2s", "out of range",
                  "unstable", "TIMESTAMP", "CORRUPTION",
                  "EMPTY", "CORRUPT"]
    ) else "OK"

    return result


def run_env_audit():
    print("=" * 60)
    print("ENV STREAM AUDIT — ALL NIGHTS")
    print("=" * 60)

    results = []

    for night, meta in NIGHT_META.items():
        night_dir = DATA_ROOT / night
        if not night_dir.exists():
            print(f"[{night}] Directory not found: {night_dir}")
            continue

        print(f"\nAuditing {night} ({meta['pillow']})...")
        result = audit_env_night(night, night_dir)
        results.append(result)

        print(f"  File:              {result['env_file']}")
        print(f"  File size:         {result['env_file_bytes']:,} bytes")
        print(f"  Rows (total):      {result['env_rows']:,}")
        print(f"  Rows (clean):      {result['env_rows_clean']:,} "
              f"({result['env_row_pct']:.1f}% of {EXPECTED_ROWS:,})")
        print(f"  Duration:          {result['env_dur_s']:.0f}s "
              f"({result['env_dur_s']/3600:.2f} hrs)")
        print()
        print(f"  ── Timestamp quality ──────────────────")
        print(f"  Mean gap:          {result['env_mean_gap_s']:.3f}s")
        print(f"  Max gap:           {result['env_max_gap_s']:.3f}s")
        print(f"  Gaps >2s:          {result['env_gaps_over_2s']}")
        print(f"  Backward jumps:    {result['env_backward_jumps']:,}  "
              f"{'WARN: TIMESTAMP BUG' if result['env_backward_jumps'] > 0 else 'OK'}")
        print(f"  Corrupt ISO rows:  {result['env_corrupt_iso_rows']:,}  "
              f"{'WARN' if result['env_corrupt_iso_rows'] > 0 else 'OK'}")
        print(f"  Corrupt epoch rows:{result['env_corrupt_epoch_rows']:,}  "
              f"({result['env_corrupt_epoch_pct']:.2f}%)  "
              f"{'WARN: I2C RACE' if result['env_corrupt_epoch_rows'] > 0 else 'OK'}")
        print()
        print(f"  ── Temperature ────────────────────────")
        print(f"  Min/Max:           {result['env_temp_min_c']:.2f} / "
              f"{result['env_temp_max_c']:.2f} C "
              f"({'OK' if result['env_temp_in_range'] else 'OUT OF RANGE'})")
        print(f"  Mean / Std:        {result['env_temp_mean_c']:.2f} / "
              f"{result['env_temp_std_c']:.3f} C")
        print(f"  Range:             {result['env_temp_range_c']:.2f} C")
        print(f"  Trend:             {result['env_temp_trend_c_per_hr']:+.4f} C/hr")
        print()
        print(f"  ── Humidity ───────────────────────────")
        print(f"  Min/Max:           {result['env_humidity_min_pct']:.2f} / "
              f"{result['env_humidity_max_pct']:.2f} % "
              f"({'OK' if result['env_humidity_in_range'] else 'OUT OF RANGE'})")
        print(f"  Mean / Std:        {result['env_humidity_mean_pct']:.2f} / "
              f"{result['env_humidity_std_pct']:.3f} %")
        print(f"  Range:             {result['env_humidity_range_pct']:.2f} %")
        print(f"  Trend:             "
              f"{result['env_humidity_trend_pct_per_hr']:+.4f} %/hr")
        print()
        print(f"  ── Pressure ───────────────────────────")
        print(f"  Mean:              {result['env_pressure_mean_hpa']:.2f} hPa")
        print(f"  Range:             {result['env_pressure_range_hpa']:.2f} hPa "
              f"({'stable' if result['env_pressure_stable'] else 'UNSTABLE'})")
        print()
        print(f"  ── Gas resistance ─────────────────────")
        print(f"  Zero %:            {result['env_gas_zero_pct']:.1f}%")
        print(f"  Valid rows (>100O):{result['env_gas_valid_rows']:,}")
        if result["env_gas_valid_rows"] > 0:
            print(f"  Gas mean:          {result['env_gas_mean_ohm']:,.0f} O")
            print(f"  Gas min/max:       {result['env_gas_min_ohm']:,.0f} / "
                  f"{result['env_gas_max_ohm']:,.0f} O")
        else:
            print(f"  Gas mean:          -- (no valid readings)")
        print()
        print(f"  IAQ cols present:  {result['env_iaq_col_present']}")
        print(f"  IAQ all blank:     {result['env_iaq_all_blank']}")
        print()
        print(f"  Status:            {result['env_status']}")
        print(f"  Notes:             {result['env_notes']}")

    df_results = pd.DataFrame(results)

    print("\n" + "=" * 60)
    print("ENV PER-PILLOW COMPARISON")
    print("=" * 60)

    for pillow in df_results["pillow"].unique():
        subset = df_results[df_results["pillow"] == pillow]
        nights = ", ".join(subset["night"].tolist())
        print(f"\n{pillow} (nights: {nights})")
        print(f"  Avg clean rows:          "
              f"{subset['env_rows_clean'].mean():,.0f}")
        print(f"  Avg corrupt epoch rows:  "
              f"{subset['env_corrupt_epoch_rows'].mean():.0f}")
        print(f"  Avg corrupt epoch %:     "
              f"{subset['env_corrupt_epoch_pct'].mean():.2f}%")
        print(f"  Avg temp mean:           "
              f"{subset['env_temp_mean_c'].mean():.2f} C")
        print(f"  Avg temp trend:          "
              f"{subset['env_temp_trend_c_per_hr'].mean():+.4f} C/hr")
        print(f"  Avg humidity mean:       "
              f"{subset['env_humidity_mean_pct'].mean():.2f} %")
        print(f"  Avg gas valid rows:      "
              f"{subset['env_gas_valid_rows'].mean():.0f}")
        print(f"  Avg gas mean (valid):    "
              f"{subset['env_gas_mean_ohm'].mean():,.0f} O")

    output_path = DATA_ROOT / "env_audit.csv"
    df_results.to_csv(output_path, index=False)
    print(f"\nENV audit saved to: {output_path}")
    print("=" * 60)

    return df_results


if __name__ == "__main__":
    run_env_audit()