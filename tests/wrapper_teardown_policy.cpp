// Tests for carrier/x86-host/src/teardown_policy.hpp.
//
// These are pure-logic tests: no game, no host process, no Vulkan/GL device and no
// Windows dependency beyond the console runtime. They pin the teardown contract that
// aio-wrapper32.cpp follows when it closes the host or rebuilds the shared imports:
//
//   * host alive + no pending submission  -> nothing to wait, destroy allowed
//   * host sampled alive, then dead before its last fence signal
//                                         -> no unbounded wait, nothing destroyed,
//                                            imports retired (audit gap R3)
//   * pending GPU reference on a rebuild  -> at most one bounded wait, then retire
//   * DllMain process-exit path           -> skip every GPU wait, retire only
//
// Build: cl /nologo /W4 /WX /MT /std:c++17 tests\wrapper_teardown_policy.cpp
// Exit code 0 means every case passed.

#include "../carrier/x86-host/src/teardown_policy.hpp"

#include <cstdint>
#include <cstdio>

namespace
{

using namespace teardown_policy;

int gFailures = 0;
int gPassed = 0;

void Check(bool condition, const char *what)
{
    if (condition) return;
    std::fprintf(stderr, "  [FAIL] %s\n", what);
    ++gFailures;
}

void CheckWait(const Decision &d, WaitAction expected, const char *what)
{
    Check(d.wait == expected, what);
    if (d.wait != expected)
        std::fprintf(stderr, "         wait=%s expected=%s (%s)\n", WaitActionName(d.wait),
                     WaitActionName(expected), d.reason);
}

void CheckDestroy(const Decision &d, DestroyAction expected, const char *what)
{
    Check(d.destroy == expected, what);
    if (d.destroy != expected)
        std::fprintf(stderr, "         destroy=%s expected=%s (%s)\n", DestroyActionName(d.destroy),
                     DestroyActionName(expected), d.reason);
}

void Run(const char *name, void (*fn)())
{
    const int before = gFailures;
    fn();
    if (gFailures == before)
    {
        ++gPassed;
        std::printf("[PASS] %s\n", name);
    }
    else
    {
        std::printf("[FAIL] %s\n", name);
    }
}

// A host that is running and has no submitted GPU work: the imports can be destroyed
// immediately and no wait is needed on any path.
void CaseNoPendingHostAlive()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Untouched;
    ev.bounded_wait_supported = true;

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Skip, "no pending work must not wait");
    CheckDestroy(d, DestroyAction::Destroy, "no pending work may be destroyed");
    Check(!d.host_exited_before_signal, "no premature exit diagnostic");
}

// A host that is alive with one submitted frame that depends on its timeline: exactly
// one bounded wait is allowed, and destruction is not granted until it produces proof.
void CaseHostAlivePendingAllowsBoundedWait()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Bounded, "host alive + pending host-gated work allows one bounded wait");
    CheckDestroy(d, DestroyAction::Retire, "destroy stays provisional until the bounded wait proves completion");
    Check(d.wait_timeout_ms == kDefaultWaitTimeoutMs, "bounded wait must carry a finite timeout");
    Check(d.wait_timeout_ms == 2000, "default bounded timeout is 2 s");

    // Nothing was proven yet: a second decision still must not destroy.
    const Decision again = Decide(ev);
    CheckWait(again, WaitAction::Bounded, "an unspent bounded wait is still offered");
}

// The audit gap R3: the process was sampled alive at the HostClose entry, then exited
// before it signalled the last frame. WAIT_OBJECT_0 on the exit wait is not fence
// evidence, so no unbounded wait_idle and no destruction may follow.
void CaseHostExitedBeforeSignalRetires()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Exited;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;  // a query exists, but the value will never arrive

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Retire, "host exited before signalling: never wait again");
    CheckDestroy(d, DestroyAction::Retire, "host exited before signalling: never destroy imports");
    Check(d.host_exited_before_signal, "the premature-exit state must be reported");
}

// Same gap without the "alive at entry" sample: a host that is already gone can never
// release the queued wait, whatever the backend supports.
void CaseHostDeadAtEntryRetires()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Exited;
    ev.host_after_wait = HostObservation::Exited;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Retire, "host dead at entry must not wait");
    CheckDestroy(d, DestroyAction::Retire, "host dead at entry must retire the imports");
    Check(d.host_exited_before_signal, "dead host without a signal is the premature-exit state");
}

// A rebuild (ReleaseShared) with pending GPU references and a host that is gone: the
// images/memory must be retired, not destroyed, and the decision must be immediate.
void CaseRebuildWithPendingRefsRetires()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::NotStarted;  // handle already closed by HostClose
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.backend_can_teardown = true;
    ev.bounded_wait_supported = false;  // no host timeline to observe any more

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Retire, "rebuild with unproven refs must not wait");
    CheckDestroy(d, DestroyAction::Retire, "rebuild with unproven refs must retire, not destroy");
}

// A lost backend (device/context gone) cannot be drained or destroyed through.
void CaseBackendLostRetires()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.backend_can_teardown = false;

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Skip, "lost backend must not wait");
    CheckDestroy(d, DestroyAction::Retire, "lost backend must not destroy imports");
}

// One bounded wait without an observed timeline value: the caller spent its only
// bounded wait, so the safe outcome is retirement (bounded return, no queue drain).
void CaseBoundedWaitTimeoutRetires()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;

    const Decision first = Decide(ev);
    CheckWait(first, WaitAction::Bounded, "the timeout scenario starts with one bounded wait");

    ev.bounded_wait_tried = true;
    ev.bounded_wait_ok = false;  // vkWaitSemaphores timed out
    const Decision second = Decide(ev);
    CheckWait(second, WaitAction::Retire, "a timed-out bounded wait must end in retirement");
    CheckDestroy(second, DestroyAction::Retire, "a timed-out bounded wait must not destroy imports");
}

// A bounded wait that reports success but records no completion proof must not be
// trusted (the lambda contract is "proof or nothing").
void CaseBoundedWaitWithoutProofRetires()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;
    ev.bounded_wait_tried = true;
    ev.bounded_wait_ok = true;
    ev.host_fence_reached = false;

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Retire, "success without proof is not proof");
    CheckDestroy(d, DestroyAction::Retire, "success without proof must retire");
}

// The happy path: the bounded wait observes the host frame, the queue drain completes,
// and only then may the imports be destroyed.
void CaseBoundedWaitThenDrainThenDestroy()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;

    CheckWait(Decide(ev), WaitAction::Bounded, "happy path starts with one bounded wait");

    ev.bounded_wait_tried = true;
    ev.bounded_wait_ok = true;
    ev.host_fence_reached = true;
    const Decision drain = Decide(ev);
    CheckWait(drain, WaitAction::DrainQueue, "an observed host frame unlocks the queue drain");
    CheckDestroy(drain, DestroyAction::Retire, "destruction only follows a completed drain");

    ev.queue_drain_tried = true;
    ev.queue_idle_observed = true;
    const Decision done = Decide(ev);
    CheckWait(done, WaitAction::Skip, "an idle queue needs no further wait");
    CheckDestroy(done, DestroyAction::Destroy, "an idle queue permits destruction");
}

// A queue drain that does not complete: retirement, not a second drain.
void CaseQueueDrainFailureRetires()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;
    ev.bounded_wait_tried = true;
    ev.bounded_wait_ok = true;
    ev.host_fence_reached = true;

    CheckWait(Decide(ev), WaitAction::DrainQueue, "a signalled host value unlocks the drain");

    ev.queue_drain_tried = true;
    ev.queue_idle_observed = false;
    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Retire, "a failed drain must not be retried");
    CheckDestroy(d, DestroyAction::Retire, "a failed drain must retire");
}

// A Vulkan device without timeline-query support cannot prove completion: refuse the
// unbounded wait_idle instead of guessing.
void CaseNoBoundedProbeRetires()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = false;

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Retire, "no bounded probe means no unbounded wait");
    CheckDestroy(d, DestroyAction::Retire, "no bounded probe means retirement");
}

// Pending work that does not depend on the host (its own fence/copy) retires through
// the game queue on its own, which makes that drain bounded in practice.
void CasePendingWithoutHostDependencyDrains()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = false;
    ev.bounded_wait_supported = false;

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::DrainQueue, "host-independent pending work is drained, not retired");
    CheckDestroy(d, DestroyAction::Retire, "destruction is provisional until the drain proves idle");

    ev.queue_drain_tried = true;
    ev.queue_idle_observed = true;
    const Decision done = Decide(ev);
    CheckWait(done, WaitAction::Skip, "drained host-independent work needs no wait");
    CheckDestroy(done, DestroyAction::Destroy, "drained host-independent work may be destroyed");
}

// The GL path has no imported fence value; the bounded `FeedGlWaitIdle` proves the
// whole stream (including glWaitSemaphoreEXT) finished, i.e. the queue is idle.
void CaseGlBoundedWaitProvesQueueIdle()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;

    CheckWait(Decide(ev), WaitAction::Bounded, "GL still starts with the bounded idle wait");

    ev.bounded_wait_tried = true;
    ev.bounded_wait_ok = true;
    ev.queue_idle_observed = true;  // FeedGlWaitIdle satisfied the stream fence
    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Skip, "an idle GL stream needs no queue drain");
    CheckDestroy(d, DestroyAction::Destroy, "an idle GL stream permits destruction");
}

// DllMain DLL_PROCESS_DETACH: the loader lock is held. No GPU wait and no destruction
// is ever allowed, even with the host apparently alive and work pending.
void CaseProcessExitSkipsEverything()
{
    Evidence ev;
    ev.process_exit = true;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::Alive;
    ev.gpu = GpuState::Pending;
    ev.gpu_wait_depends_on_host = true;
    ev.bounded_wait_supported = true;

    const Decision d = Decide(ev);
    CheckWait(d, WaitAction::Skip, "process exit must not wait on the GPU");
    CheckDestroy(d, DestroyAction::Retire, "process exit must not destroy imports");
    Check(!d.host_exited_before_signal, "process exit is not the premature-exit state");
}

void CaseLatestHostPrefersPostWaitSample()
{
    Evidence ev;
    ev.host_at_entry = HostObservation::Alive;
    ev.host_after_wait = HostObservation::NotStarted;
    Check(LatestHost(ev) == HostObservation::Alive, "without a post sample the entry sample counts");

    ev.host_after_wait = HostObservation::Exited;
    Check(LatestHost(ev) == HostObservation::Exited, "a post-wait exit overrides the entry sample");
}

// Miniature of the wrapper's teardown loop: apply the decision, perform at most the one
// bounded wait / one drain, and require termination in a handful of decisions. This is
// what "the close path must return in bounded time" means at the policy level.
struct FakeTeardown
{
    Evidence ev;
    int decisions = 0;
    int boundedWaits = 0;
    uint32_t waitedMs = 0;
    int queueDrains = 0;

    Decision Run(bool hostSignals, bool drainCompletes)
    {
        Decision d;
        for (int i = 0; i < 8; ++i)
        {
            d = Decide(ev);
            ++decisions;
            if (d.wait == WaitAction::Bounded)
            {
                ++boundedWaits;
                waitedMs += d.wait_timeout_ms;
                ev.bounded_wait_tried = true;
                ev.bounded_wait_ok = hostSignals;
                ev.host_fence_reached = hostSignals;
                continue;
            }
            if (d.wait == WaitAction::DrainQueue)
            {
                ++queueDrains;
                ev.queue_drain_tried = true;
                ev.queue_idle_observed = drainCompletes;
                continue;
            }
            return d;
        }
        return d;
    }
};

void CaseLoopConvergesOnHostDeath()
{
    FakeTeardown fake;
    fake.ev.host_at_entry = HostObservation::Alive;
    fake.ev.host_after_wait = HostObservation::Exited;  // died while the close waited
    fake.ev.gpu = GpuState::Pending;
    fake.ev.gpu_wait_depends_on_host = true;
    fake.ev.bounded_wait_supported = true;

    const Decision d = fake.Run(false, false);
    Check(fake.decisions == 1, "the dead-before-signal loop settles in one decision");
    Check(fake.boundedWaits == 0, "the dead-before-signal loop spends no bounded wait");
    Check(fake.queueDrains == 0, "the dead-before-signal loop never drains the queue");
    CheckWait(d, WaitAction::Retire, "the loop retires without waiting");
    CheckDestroy(d, DestroyAction::Retire, "the loop retires resources");
    Check(d.host_exited_before_signal, "the loop reports the premature exit");
}

void CaseLoopStopsAfterOneTimedOutWait()
{
    FakeTeardown fake;
    fake.ev.host_at_entry = HostObservation::Alive;
    fake.ev.host_after_wait = HostObservation::Alive;
    fake.ev.gpu = GpuState::Pending;
    fake.ev.gpu_wait_depends_on_host = true;
    fake.ev.bounded_wait_supported = true;

    const Decision d = fake.Run(false, false);  // host never signals, vkWaitSemaphores times out
    Check(fake.boundedWaits == 1, "a stalled host costs exactly one bounded wait");
    Check(fake.waitedMs == kDefaultWaitTimeoutMs, "the stalled host costs exactly one timeout");
    Check(fake.queueDrains == 0, "a stalled host never reaches the unbounded queue drain");
    CheckWait(d, WaitAction::Retire, "the stalled host ends in retirement");
    CheckDestroy(d, DestroyAction::Retire, "the stalled host retires resources");
}

void CaseLoopFinishesCleanly()
{
    FakeTeardown fake;
    fake.ev.host_at_entry = HostObservation::Alive;
    fake.ev.host_after_wait = HostObservation::Alive;
    fake.ev.gpu = GpuState::Pending;
    fake.ev.gpu_wait_depends_on_host = true;
    fake.ev.bounded_wait_supported = true;

    const Decision d = fake.Run(true, true);
    Check(fake.boundedWaits == 1, "the clean path spends one bounded wait");
    Check(fake.queueDrains == 1, "the clean path drains the queue once");
    Check(fake.decisions <= 4, "the clean path settles in a few decisions");
    CheckWait(d, WaitAction::Skip, "the clean path ends without waiting");
    CheckDestroy(d, DestroyAction::Destroy, "the clean path destroys the imports");
}

}  // namespace

int main()
{
    Run("no pending work / host alive -> destroy", CaseNoPendingHostAlive);
    Run("host alive + pending -> one bounded wait", CaseHostAlivePendingAllowsBoundedWait);
    Run("host exited before signal -> retire (audit R3)", CaseHostExitedBeforeSignalRetires);
    Run("host dead at entry -> retire", CaseHostDeadAtEntryRetires);
    Run("rebuild with unproven refs -> retire", CaseRebuildWithPendingRefsRetires);
    Run("lost backend -> retire", CaseBackendLostRetires);
    Run("bounded wait timeout -> retire", CaseBoundedWaitTimeoutRetires);
    Run("bounded wait without proof -> retire", CaseBoundedWaitWithoutProofRetires);
    Run("bounded wait, drain, destroy", CaseBoundedWaitThenDrainThenDestroy);
    Run("queue drain failure -> retire", CaseQueueDrainFailureRetires);
    Run("no bounded probe -> retire", CaseNoBoundedProbeRetires);
    Run("host-independent pending -> drain", CasePendingWithoutHostDependencyDrains);
    Run("GL idle wait proves queue idle", CaseGlBoundedWaitProvesQueueIdle);
    Run("process exit skips every GPU wait", CaseProcessExitSkipsEverything);
    Run("post-wait host sample wins", CaseLatestHostPrefersPostWaitSample);
    Run("loop: host died before signal", CaseLoopConvergesOnHostDeath);
    Run("loop: one timed-out wait then retire", CaseLoopStopsAfterOneTimedOutWait);
    Run("loop: clean teardown", CaseLoopFinishesCleanly);

    std::printf("wrapper teardown policy: %d passed, %d failed\n", gPassed, gFailures);
    return gFailures == 0 ? 0 : 1;
}
