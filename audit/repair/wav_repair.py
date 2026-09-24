import struct
from pathlib import Path

def repair_wav(wav_path: Path) -> bool:
    """
    Repair a WAV file with a missing or zero data size in the header.
    The audio data is assumed to start at byte 44 (standard WAV header).
    Calculates the correct data size from the actual file size.
    """
    file_size = wav_path.stat().st_size

    if file_size < 44:
        print(f"SKIP {wav_path.name} — too small to be a valid WAV ({file_size} bytes)")
        return False

    data_size  = file_size - 44        # audio bytes after the header
    chunk_size = 36 + data_size        # RIFF chunk size

    with open(wav_path, "r+b") as f:
        # Read existing header values
        f.seek(4)
        existing_chunk = struct.unpack("<I", f.read(4))[0]
        f.seek(40)
        existing_data  = struct.unpack("<I", f.read(4))[0]

        if existing_data == data_size:
            print(f"OK   {wav_path.name} — header already correct ({data_size:,} bytes)")
            return True

        print(f"FIX  {wav_path.name} — data_size {existing_data:,} -> {data_size:,} bytes "
              f"({data_size / 1048576:.2f} MB, ~{data_size / 32000:.1f}s audio)")

        # Patch RIFF chunk size at byte 4
        f.seek(4)
        f.write(struct.pack("<I", chunk_size))

        # Patch data chunk size at byte 40
        f.seek(40)
        f.write(struct.pack("<I", data_size))

    return True


def repair_night(night_dir: Path) -> None:
    wavs = sorted(night_dir.glob("*.wav"))
    if not wavs:
        print(f"No WAV files found in {night_dir}")
        return

    print(f"\nRepairing WAV files in {night_dir.name}...")
    for wav in wavs:
        repair_wav(wav)


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print("Usage:")
        print("  Repair one file:   python wav_repair.py path/to/file.wav")
        print("  Repair one night:  python wav_repair.py path/to/n23/")
        print("  Repair all nights: python wav_repair.py path/to/data/")
        sys.exit(1)

    target = Path(sys.argv[1])

    if target.suffix == ".wav":
        # Single file
        repair_wav(target)
    elif target.is_dir():
        # Check if it contains WAVs directly (single night folder)
        # or subfolders (data root with multiple nights)
        wavs = list(target.glob("*.wav"))
        if wavs:
            repair_night(target)
        else:
            # Treat as data root — repair all night subfolders
            nights = sorted([d for d in target.iterdir() if d.is_dir()])
            for night in nights:
                repair_night(night)
    else:
        print(f"ERROR: {target} is not a .wav file or directory")
        sys.exit(1)