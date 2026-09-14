"""Compile the Dreamcast frame pacing and auto frameskip logic from main.c.

No KOS runtime needed: python tests/frame_pacing_test.py --cc gcc|cl
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
#include <stdlib.h>
typedef unsigned int u32;
typedef long long s64;
typedef unsigned long long u64;
typedef enum { auto_frameskip, manual_frameskip, no_frameskip } frameskip_type;
static frameskip_type current_frameskip_type = auto_frameskip;
static u32 frameskip_value = 4, random_skip, frameskip_counter;
static u32 synchronize_flag = 1, skip_next_frame;
'''

CHECKS = r'''
static u64 now_us;
static u32 drawn, skipped, skip_run, longest_skip_run;

static void reset(frameskip_type type, u32 value, u32 synchronize) {
  current_frameskip_type = type;
  frameskip_value = value;
  synchronize_flag = synchronize;
  random_skip = 0;
  frameskip_counter = 0;
  frame_deadline_us = 0;
  frames_skipped_in_row = 0;
  skip_next_frame = 0;
  now_us = 1000000;
}

static void clear_counts(void) {
  drawn = skipped = skip_run = longest_skip_run = 0;
}

/* One emulated frame: emulation costs emulate_us, and drawing adds draw_us
   when this frame is drawn. Then the per-frame pacing call and its wait. */
static u32 frame(u32 emulate_us, u32 draw_us) {
  u32 wait;
  if(skip_next_frame) {
    skipped++;
    if(++skip_run > longest_skip_run) longest_skip_run = skip_run;
    now_us += emulate_us;
  } else {
    drawn++;
    skip_run = 0;
    now_us += emulate_us + draw_us;
  }
  wait = dc_frame_pace(now_us);
  now_us += wait;
  return wait;
}

static s64 lag(void) {
  return (s64)(now_us - frame_deadline_us);
}

static void full_speed_is_paced(void) {
  u32 i;
  u64 start;
  reset(auto_frameskip, 4, 1);
  frame(10000, 2000);
  start = now_us;
  clear_counts();
  for(i = 0; i < 600; i++)
    frame(10000, 2000);
  assert(skipped == 0);
  assert(now_us - start == 600ULL * GBA_FRAME_US);
}

static void off_never_skips(void) {
  u32 i;
  reset(no_frameskip, 4, 1);
  frame(20000, 10000);
  clear_counts();
  for(i = 0; i < 300; i++) {
    u32 wait = frame(20000, 10000);
    assert(wait == 0);
  }
  assert(skipped == 0 && drawn == 300);
}

static void auto_skip_holds_real_time(void) {
  /* 12 ms to emulate and 8 ms to draw: drawing every frame is 84% speed.
     Skipping should draw about 4743 / 8000 = 59% of frames and stay on real
     time. */
  u32 i;
  u64 start;
  reset(auto_frameskip, 4, 1);
  frame(12000, 8000);
  start = now_us;
  clear_counts();
  for(i = 0; i < 3000; i++) {
    frame(12000, 8000);
    assert(lag() <= 2 * GBA_FRAME_US);
  }
  assert(longest_skip_run <= 4);
  assert(drawn * 100 >= 3000 * 55 && drawn * 100 <= 3000 * 65);
  assert(now_us - start <= 3000ULL * GBA_FRAME_US + 2 * GBA_FRAME_US);
}

static void too_slow_draws_one_in_five(void) {
  /* Emulation alone takes 30 ms: skip the limit, draw, and drop the backlog
     instead of letting it grow. */
  u32 i;
  reset(auto_frameskip, 4, 1);
  frame(30000, 5000);
  clear_counts();
  for(i = 0; i < 500; i++) {
    frame(30000, 5000);
    assert(lag() <= GBA_MAX_LAG_US + 35000);
  }
  assert(longest_skip_run == 4);
  assert(drawn * 5 >= 500 - 5);
}

static void stall_does_not_cause_long_skip(void) {
  /* A one-second load must not be followed by a second of skipped frames. */
  u32 i;
  reset(auto_frameskip, 4, 1);
  for(i = 0; i < 60; i++)
    frame(10000, 2000);
  now_us += 1000000;
  clear_counts();
  for(i = 0; i < 60; i++)
    frame(10000, 2000);
  assert(skipped <= 1);
}

static void fast_forward_draws_one_in_five(void) {
  u32 i;
  reset(auto_frameskip, 4, 0);
  clear_counts();
  for(i = 0; i < 100; i++) {
    u32 wait = frame(1000, 1000);
    assert(wait == 0);
  }
  assert(longest_skip_run == 4 && drawn >= 19 && drawn <= 21);
}

static void manual_pattern_ignores_time(void) {
  u32 i;
  reset(manual_frameskip, 2, 1);
  clear_counts();
  for(i = 0; i < 30; i++)
    frame(5000, 1000);
  assert(drawn == 10 && skipped == 20 && longest_skip_run == 2);
}

static void zero_limit_never_skips(void) {
  u32 i;
  reset(auto_frameskip, 0, 1);
  clear_counts();
  for(i = 0; i < 100; i++)
    frame(30000, 5000);
  assert(skipped == 0);
}

int main(void) {
  full_speed_is_paced();
  off_never_skips();
  auto_skip_holds_real_time();
  too_slow_draws_one_in_five();
  stall_does_not_cause_long_skip();
  fast_forward_draws_one_in_five();
  manual_pattern_ignores_time();
  zero_limit_never_skips();
  puts("Dreamcast frame pacing behavior passed");
  return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    main_c = (ROOT / "main.c").read_text()
    pace = function(main_c, "static u32 dc_frame_pace(u64 now_us)").rstrip()
    start = main_c.index("#define GBA_FRAME_US")
    end = main_c.index(pace) + len(pace)
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "frame_pacing.c"
        binary = Path(directory) / "frame_pacing.exe"
        source.write_text(SHIMS + main_c[start:end] + "\n" + CHECKS)
        if Path(args.cc).stem.lower() == "cl":
            cmd = [args.cc, "/nologo", "/W4", "/std:c11", str(source),
                   "/Fe:" + str(binary)]
        else:
            cmd = [args.cc, "-std=c99", "-Wall", "-Wextra", str(source),
                   "-o", str(binary)]
        subprocess.run(cmd, cwd=directory, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
