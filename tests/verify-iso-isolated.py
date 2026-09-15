"""Check decoded alternating-speaker files from IsoIsolatedRecordingTest."""
import array
from pathlib import Path
import math
import subprocess
import sys

ffmpeg, folder = sys.argv[1], Path(sys.argv[2])
for participant in (1, 2):
    file = folder / f"isolated-{participant}.mp4"
    samples = array.array("h", subprocess.check_output([
        ffmpeg, "-v", "error", "-xerror", "-i", str(file), "-vn", "-ac", "1", "-ar", "48000", "-f", "s16le", "-"]))
    if sys.byteorder != "little":
        samples.byteswap()
    levels = []
    for second in range(4):
        chunk = samples[int((second+.25)*48000):int((second+.75)*48000)]
        assert len(chunk) == 24000
        rms = math.sqrt(sum(x*x for x in chunk)/len(chunk))
        speaking = second % 2 + 1 == participant
        assert (rms > 5000 if speaking else rms < 10), (participant, second, rms)
        levels.append(round(rms, 2))
    subprocess.run([ffmpeg, "-v", "error", "-xerror", "-i", str(file), "-f", "null", "-"], check=True)
    print(f"Participant {participant}: decoded one-second RMS {levels}; other speaker silent")
