"""Compile the actual audio callback with SDL shims, for Dreamcast and desktop.

No SDL or KOS runtime needed: python tests/audio_callback_test.py --cc gcc|cl
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from save_io_test import function

ROOT = Path(__file__).resolve().parent.parent

SHIMS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char Uint8;
typedef short s16;
typedef int s32;
typedef unsigned int u32;
#define BUFFER_SIZE 32768
static s16 sound_buffer[BUFFER_SIZE];
static u32 sound_buffer_base, gbc_sound_buffer_index, global_enable_audio = 1;
static int sound_mutex, sound_cv, locked, waits, signals, allow_wait;
static void SDL_LockMutex(int mutex) {
  (void)mutex; assert(!locked); locked = 1;
}
static void SDL_UnlockMutex(int mutex) {
  (void)mutex; assert(locked); locked = 0;
}
#ifndef _arch_dreamcast
static u32 wait_producer_index;
static void SDL_CondWait(int cond, int mutex) {
  (void)cond; (void)mutex;
  assert(locked && allow_wait && waits == 0);
  waits++;
  gbc_sound_buffer_index = wait_producer_index;
}
#else
/* Any accidental Dreamcast wait fails the test instead of hanging. */
#define SDL_CondWait(cond, mutex) assert(!"Dreamcast callback waited for samples")
#endif
static void SDL_CondSignal(int cond) {
  (void)cond; assert(locked); signals++;
}
'''

CHECKS = r'''
static s16 before[BUFFER_SIZE];
static struct { s16 guard_before; s16 samples[512]; s16 guard_after; } out;

static void prepare(u32 base, u32 available, int enabled) {
  u32 i;
  assert(!locked);
  waits = signals = allow_wait = 0;
  sound_buffer_base = base;
  gbc_sound_buffer_index = (base + available) % BUFFER_SIZE;
  global_enable_audio = enabled;
  for(i = 0; i < BUFFER_SIZE; i++)
    sound_buffer[i] = (s16)((int)(i % 6001) - 3000);
  memcpy(before, sound_buffer, sizeof(before));
#ifdef SOUND_UNDERRUN_FADE_FRAMES
  sound_reset_output();
#endif
}

static void call_audio(int bytes) {
  u32 i;
  int old_signals = signals;
  memset(&out, 0x5a, sizeof(out));
  sound_callback(NULL, (Uint8 *)out.samples, bytes);
  assert(!locked);
  assert(signals == old_signals + (bytes > 0));
  assert(out.guard_before == 0x5a5a && out.guard_after == 0x5a5a);
  for(i = bytes > 0 ? (u32)bytes : 0; i < sizeof(out.samples); i++)
    assert(((Uint8 *)out.samples)[i] == 0x5a);
}

static s16 scaled(s16 raw) {
  int value = raw;
  if(value > 2047) value = 2047;
  if(value < -2048) value = -2048;
  return (s16)(value * 16);
}

static void check_ring(u32 old_base, u32 consumed) {
  u32 i;
  assert(sound_buffer_base == (old_base + consumed) % BUFFER_SIZE);
  for(i = 0; i < BUFFER_SIZE; i++) {
    u32 distance = (i + BUFFER_SIZE - old_base) % BUFFER_SIZE;
    assert(sound_buffer[i] == (distance < consumed ? 0 : before[i]));
  }
}

static void boundary_cases(void) {
  static const u32 bases[] = {0, 2, 128, BUFFER_SIZE - 8, BUFFER_SIZE - 4,
                            BUFFER_SIZE - 2};
  static const u32 sizes[] = {0, 2, 4, 8, 32, 256};
  u32 b, a, r, i;
  int enabled;
  for(enabled = 0; enabled <= 1; enabled++)
  for(b = 0; b < sizeof(bases) / sizeof(bases[0]); b++)
  for(a = 0; a < sizeof(sizes) / sizeof(sizes[0]); a++)
  for(r = 0; r < sizeof(sizes) / sizeof(sizes[0]); r++) {
    u32 available = sizes[a], requested = sizes[r], consumed = requested;
#ifndef _arch_dreamcast
    if(available < requested) continue;
#else
    if(consumed > available) consumed = available;
#endif
    prepare(bases[b], available, enabled);
    call_audio((int)(requested * sizeof(s16)));
    assert(waits == 0);
    check_ring(bases[b], consumed);
    for(i = 0; i < consumed; i++)
      assert(out.samples[i] == (enabled ?
        scaled(before[(bases[b] + i) % BUFFER_SIZE]) : 0));
    for(i = consumed; i < requested; i++) {
      s16 expected = 0;
      u32 frame = (i - consumed) / 2;
      if(enabled && consumed && frame < 64) {
        s16 last = scaled(before[(bases[b] + consumed - 2 + i % 2) % BUFFER_SIZE]);
        expected = (s16)((s32)last * (63 - (int)frame) / 64);
      }
      assert(out.samples[i] == expected);
    }
  }
}

static void clipping_and_channel_order(void) {
  static const s16 raw[] = {-32768, 32767, -2048, 2047, -1, 1, 0, 0};
  u32 i;
  prepare(BUFFER_SIZE - 2, 8, 1);
  for(i = 0; i < 8; i++) sound_buffer[(BUFFER_SIZE - 2 + i) % BUFFER_SIZE] = raw[i];
  call_audio(16);
  for(i = 0; i < 8; i++) assert(out.samples[i] == scaled(raw[i]));
}

#ifdef _arch_dreamcast
static void underrun_sequence(void) {
  u32 i;
  prepare(BUFFER_SIZE - 2, 2, 1);
  sound_buffer[BUFFER_SIZE - 2] = 1024;
  sound_buffer[BUFFER_SIZE - 1] = -1024;
  call_audio(4);
  assert(out.samples[0] == 16384 && out.samples[1] == -16384);
  /* Exhaust the fade over small callbacks; it must not restart each time. */
  for(i = 0; i < 70; i++) {
    s16 expected = i < 64 ? (s16)(16384 * (63 - (int)i) / 64) : 0;
    call_audio(4);
    assert(out.samples[0] == expected && out.samples[1] == -expected);
    assert(sound_buffer_base == 0 && gbc_sound_buffer_index == 0);
    assert(sound_buffer[0] == before[0]);
  }
  /* New production resumes at the same ring position, without dropping data. */
  sound_buffer[0] = 123;
  sound_buffer[1] = -456;
  gbc_sound_buffer_index = 2;
  call_audio(4);
  assert(out.samples[0] == 1968 && out.samples[1] == -7296);
  assert(sound_buffer_base == 2);
  global_enable_audio = 0;
  call_audio(4);
  assert(out.samples[0] == 0 && out.samples[1] == 0);
  global_enable_audio = 1;
  call_audio(4);
  assert(out.samples[0] == 0 && out.samples[1] == 0);
}
#else
static void desktop_wait(void) {
  prepare(0, 2, 1);
  allow_wait = 1;
  wait_producer_index = 4;
  call_audio(8);
  assert(waits == 1);
  assert(out.samples[3] == scaled(before[3]));
  check_ring(0, 4);
}
#endif

int main(void) {
  int bytes;
  boundary_cases();
  clipping_and_channel_order();
  /* Invalid/partial lengths never consume half a stereo frame or overrun. */
  for(bytes = -1; bytes <= 7; bytes++) {
    prepare(0, 8, 0);
    call_audio(bytes);
    check_ring(0, bytes > 0 ? ((u32)bytes / 4) * 2 : 0);
  }
#ifdef _arch_dreamcast
  underrun_sequence();
  puts("Dreamcast audio callback behavior passed");
#else
  desktop_wait();
  puts("desktop audio callback behavior passed");
#endif
  return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    sound = (ROOT / "sound.c").read_text()
    producer = function(sound, "void update_gbc_sound(").rstrip()
    start = sound.index(producer) + len(producer)
    callback = sound[start:sound.index("// Special thanks to blarrg", start)]
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "audio_callback.c"
        source.write_text(SHIMS + callback + CHECKS)
        for dreamcast in (True, False):
            binary = Path(directory) / ("dreamcast.exe" if dreamcast else "desktop.exe")
            if Path(args.cc).stem.lower() == "cl":
                cmd = [args.cc, "/nologo", "/W4", "/std:c11",
                       str(source), "/Fe:" + str(binary)]
                if dreamcast:
                    cmd.append("/D_arch_dreamcast=1")
            else:
                cmd = [args.cc, "-std=c99", "-Wall", "-Wextra",
                       str(source), "-o", str(binary)]
                if dreamcast:
                    cmd.append("-D_arch_dreamcast=1")
                if args.sanitize:
                    cmd += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
            subprocess.run(cmd, cwd=directory, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
