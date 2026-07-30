#include "nix/util/scheduling.hh"
#include "nix/util/logging.hh"

#include <cerrno>
#include <cstring>
#include <sched.h>

namespace nix {

bool realtimeWriter = false;

/* Which thread this is, and why it is the one: sourceToSink runs the
   *reader* callback on its worker, so on an import that worker is the
   restore -- and it is the only thread in an import that writes.
   Measured with strace -f on a 3.3 GiB tree: 6.05 s inside write() on
   one tid, 0.00 s on every other (bench/BASELINE, "the stutter in real
   time"). The dump side and the two hash threads never touch the disk.

   What that thread competes for is not CPU time, it is being picked
   when it wakes. It alternates a memcpy out of the chunk ring with a
   write() and is blocked for most of an import, while the two hash
   threads beside it are SHA-256 loops that will happily use a whole
   slice. On a box with one or two vCPUs -- the shape that ships, see
   the per-vCPU table in BASELINE -- a SCHED_OTHER writer waking with
   both hashers runnable waits for a slice before it can hand them the
   next chunk, and they are idle for exactly that long.

   What this does NOT fix, stated up front because three changes in this
   family have already measured negative: balance_dirty_pages. When the
   box has no page cache to spare, the writer sits inside one write()
   for seconds and no scheduling policy makes the disk faster. This
   only addresses the gap between "runnable" and "running".

   Containment is SCHED_RESET_ON_FORK: the kernel puts every child back
   on SCHED_OTHER at nice 0 across fork, so nothing this process spawns
   inherits real-time priority. Nothing in an import forks today, but
   libstore does run external programs on other paths and a policy that
   escapes through fork is the failure mode worth designing out rather
   than auditing for.

   Minimum RR priority (1), and only this thread: the hash threads stay
   SCHED_OTHER on purpose. An RT thread that also hashed would preempt
   the work it exists to feed. The queue mutexes it shares with them are
   a deque push/pop wide, so the inversion window where an RT writer
   waits on a mutex held by a preempted hasher is microseconds; std
   mutexes have no priority inheritance, so it is a window, not zero.

   The permission is arranged where the box is built, not here:
   setup-boot.sh setcaps cap_sys_nice on import-dir and nixgen-commit
   runs it as root. So this claims the priority it was already granted
   and takes EPERM as its answer, quietly: off-box (a user importing
   into their own store, a test run) there is no capability to use and
   nothing went wrong, the import runs at normal priority. Failing
   loudly here made every such run print a warning about a knob the
   operator does not set on this side. */
void setWriterScheduling()
{
    if (!realtimeWriter)
        return;

    int prio = sched_get_priority_min(SCHED_RR);
    if (prio == -1) {
        debug("no SCHED_RR priority range: %s", strerror(errno));
        return;
    }

    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = prio;

    if (sched_setscheduler(0, SCHED_RR | SCHED_RESET_ON_FORK, &param) != 0) {
        debug("import writer stays at normal priority: %s", strerror(errno));
        return;
    }

    debug("import writer running at SCHED_RR priority %d", prio);
}

} // namespace nix
