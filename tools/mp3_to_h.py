import subprocess
import sys
import os

if len(sys.argv) < 2:
    print("用法: python mp3_to_h.py <输入.mp3> [输出.h] [截取秒数]")
    sys.exit(2)

MP3_PATH = sys.argv[1]
OUT_H    = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "main", "music_data.h")
MAX_SEC  = float(sys.argv[3]) if len(sys.argv) > 3 else 25

print(f"Input:  {MP3_PATH}")
print(f"Output: {OUT_H}")
print(f"Max:    {MAX_SEC}s")

# Step 1: Decode MP3 to raw PCM via ffmpeg
ffmpeg = "ffmpeg.exe"
cmd = [
    ffmpeg, "-y", "-i", MP3_PATH,
    "-f", "s16le", "-acodec", "pcm_s16le",
    "-ac", "1", "-ar", "44100",
    "-t", str(MAX_SEC),
    "-loglevel", "error",
    "pipe:1"
]

print("Decoding...")
result = subprocess.run(cmd, capture_output=True, timeout=120)
if result.returncode != 0:
    print("ffmpeg error:", result.stderr.decode())
    sys.exit(1)

pcm_data = result.stdout
num_samples = len(pcm_data) // 2
print(f"PCM: {num_samples} samples ({len(pcm_data)} bytes, {num_samples / 44100:.1f}s)")

# Step 2: Write C header
import struct
unpacked = struct.unpack(f'<{num_samples}h', pcm_data)

with open(OUT_H, 'w', encoding='utf-8') as f:
    f.write('#ifndef __MUSIC_DATA_H__\n')
    f.write('#define __MUSIC_DATA_H__\n')
    f.write('#include <stdint.h>\n\n')
    f.write(f'#define MUSIC_SAMPLE_RATE 44100\n')
    f.write(f'#define MUSIC_PCM_LEN     {num_samples}\n\n')
    f.write('static const int16_t music_pcm[MUSIC_PCM_LEN] = {\n')

    per_line = 8
    for i in range(0, num_samples, per_line):
        chunk = unpacked[i:i + per_line]
        vals = ', '.join(str(v) for v in chunk)
        if i + per_line < num_samples:
            f.write(f'    {vals},\n')
        else:
            f.write(f'    {vals}\n')

    f.write('};\n\n')
    f.write('#endif\n')

size_kb = os.path.getsize(OUT_H) / 1024
print(f"Generated: {OUT_H} ({size_kb:.0f} KB)")
