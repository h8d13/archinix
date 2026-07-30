#pragma once
/**
 * @file
 *
 * Scheduling policy for the thread that writes an import to disk.
 *
 * Opt-in and off by default: a program that streams a tree into a
 * store sets `realtimeWriter` before the import (see
 * `arch/import-dir.cc`), and the writer thread applies it to itself.
 */

namespace nix {

/**
 * When set, `sourceToSink`'s worker raises itself to real-time
 * round-robin scheduling. Read once per thread start, so set it before
 * the import begins.
 */
extern bool realtimeWriter;

/**
 * Applied by the writer thread to *itself* (`sched_setscheduler(0, ...)`
 * is per-thread on Linux). No-op unless `realtimeWriter` is set, and
 * never fatal or noisy: the capability is granted where the box is
 * built (setup-boot.sh), so a refusal here means it was not granted on
 * this side and the thread simply carries on at normal priority.
 */
void setWriterScheduling();

} // namespace nix
