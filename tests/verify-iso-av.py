"""Decode the six-second IsoTrackWriter fixture and check actual media timing.

Usage: python tests/verify-iso-av.py <ffmpeg-bin-directory> <fixture-directory>
"""
import array
import json
from pathlib import Path
import subprocess
import sys

bin_dir, fixture = map(Path, sys.argv[1:])
ffmpeg, probe = str(bin_dir / "ffmpeg"), str(bin_dir / "ffprobe")
files = sorted(fixture.glob("*.mp4"))
assert files, "No participant files"
assert not list(fixture.glob("*.wav")), "Unexpected separate audio files"
for file in files:
    info = json.loads(subprocess.check_output([
        probe, "-v", "error", "-count_frames", "-show_streams", "-of", "json", str(file)]))
    streams = info["streams"]
    assert len(streams) == 2, streams
    video = next(s for s in streams if s["codec_type"] == "video")
    audio = next(s for s in streams if s["codec_type"] == "audio")
    assert (video["codec_name"], video["width"], video["height"], video["r_frame_rate"], int(video["nb_read_frames"])) == ("h264", 1920, 1080, "30/1", 180), video
    assert (audio["codec_name"], audio["sample_rate"], audio["channels"]) == ("aac", "48000", 2), audio
    assert video.get("color_range") == "pc" and video.get("color_space") == "bt709", video
    subprocess.run([ffmpeg, "-v", "error", "-xerror", "-i", str(file), "-f", "null", "-"], check=True)
    # Decode centre luma without resampling the frame cadence; retain stream PTS origin.
    pixels = subprocess.check_output([ffmpeg, "-v", "error", "-i", str(file), "-an", "-vf", "crop=2:2:960:540,format=gray", "-fps_mode", "passthrough", "-f", "rawvideo", "-"])
    bright = [pixels[i] > 180 for i in range(0, len(pixels), 4)]
    pcm = array.array("h", subprocess.check_output([ffmpeg, "-v", "error", "-i", str(file), "-vn", "-ac", "1", "-ar", "48000", "-f", "s16le", "-"]))
    if sys.byteorder != "little":
        pcm.byteswap()
    loud = [max(map(abs, pcm[i:i+480]), default=0) > 4000 for i in range(0, len(pcm), 480)]
    if file.stem == "silent":
        assert not any(bright) and max(map(abs, pcm), default=0) == 0, "No-input track must be black and silent"
        print(f"{file.name}: continuous 1080p video and silent AAC with no input callbacks")
        continue
    chroma = subprocess.check_output([ffmpeg, "-v", "error", "-i", str(file), "-an", "-vf", "crop=2:2:960:540", "-pix_fmt", "yuvj420p", "-fps_mode", "passthrough", "-f", "rawvideo", "-"])
    for frame in (15, 45, 75, 105, 135, 165):
        u, v = chroma[frame*6+4:frame*6+6]
        assert abs(u-90) <= 4 and abs(v-180) <= 4, (file, frame, u, v)
    vt = [float(video["start_time"]) + i/30 for i, value in enumerate(bright) if value and (i == 0 or not bright[i-1])]
    at = [float(audio["start_time"]) + i/100 for i, value in enumerate(loud) if value and (i == 0 or not loud[i-1])]
    assert len(vt) == 5 and len(at) == 6, (vt, at)
    offsets = [min(abs(v-a) for a in at) for v in vt]
    assert max(offsets) <= .040, (file, vt, at, offsets)
    # Bunched callback timestamps must not rewind/overwrite a 100 ms tone.
    ends = [i for i, value in enumerate(loud) if not value and i > 0 and loud[i-1]]
    starts = [i for i, value in enumerate(loud) if value and (i == 0 or not loud[i-1])]
    assert len(ends) == len(starts) and all(8 <= e-s <= 12 for s, e in zip(starts, ends)), (starts, ends)
    # Camera-off second must hold the last dark picture; audio continues independently.
    assert not any(bright[120:150]), "Unexpected flash during camera gap"
    # 4:3 input must have black bars, and changing back to 16:9 must fill the canvas.
    edges = subprocess.check_output([ffmpeg, "-v", "error", "-i", str(file), "-an", "-vf", "crop=2:2:0:540,format=gray", "-fps_mode", "passthrough", "-f", "rawvideo", "-"])
    assert edges[100*4] < 20 and edges[160*4] > 20, "Incorrect aspect-ratio conformance"
    print(f"{file.name}: 180 frames, H264/AAC 1080p30, full decode passed, max flash/tone offset {max(offsets)*1000:.1f} ms")
