// Teardown decisions for the 32-bit wrapper's cross-process GPU imports.
//
// The add-on shares D3D12 images and fences with a separate 64-bit host process and
// references the imports from GPU work queued on the GAME's device: it signals the
// imported fence_in, queues a GPU wait on the imported fence_out, and copies the
// imported images. The host can die at any instant, including after the process handle
// was observed alive and before it signals the last timeline value. A process handle
// therefore answers "is the host gone", never "did the GPU work that references the
// imports retire" -- those are two different questions and this header keeps them
// apart:
//
//   * host observation  : NotStarted / Alive / Exited   (process liveness)
//   * GPU state         : Untouched / Pending / Retired (import references)
//   * completion proof  : host_fence_reached / queue_idle_observed
//
// Decide() turns that evidence into two answers:
//   * WaitAction    -- Skip / one bounded host-timeline wait / drain the game queue /
//                      Retire (never wait again)
//   * DestroyAction -- may the imported images/memory/semaphores be destroyed, or must
//                      they be retired (local aliases dropped, ownership left to the
//                      game's device destruction / the driver / process exit)?
//
// Rules in one paragraph: no pending submission means destruction is safe; a pending
// submission that can only be released by the host may be waited for at most once and
// only with a deadline (the imported timeline value is the proof); a host that died
// before signalling can never release it, so nothing may be waited for or destroyed; a
// pending submission that the host does not gate can be drained on the game queue (the
// queue retires it on its own, which bounds that drain); and when no bounded proof is
// available at all the imports are retired rather than guessed about. Process exit is
// special: the loader lock is held, so the DllMain path only retires.
//
// This header is intentionally free of Windows/Vulkan/GL types and of all I/O so the
// exact same rules run in the x86 add-on and in tests/wrapper_teardown_policy.cpp
// compiled for x64. Nothing here mutates state; the caller records the proof its wait
// produced and calls Decide() again.

#pragma once

#include <cstdint>

namespace teardown_policy
{

// How long a single bounded wait on the host timeline may take. 2 s matches the
// existing host fence timeouts in the add-on; a host that cannot signal within that
// window is treated as unable to release the queued wait.
inline constexpr uint32_t kDefaultWaitTimeoutMs = 2000;

// What the wrapper observed about the host process.
enum class HostObservation
{
    NotStarted,  // no host handle (never started, or handle already closed)
    Alive,       // the process handle is signalled as running
    Exited,      // the process handle is signalled as finished
};

// Lifecycle of the GPU work that references an imported resource. Only an observed
// completion may clear Pending; a process exit does not.
enum class GpuState
{
    Untouched,  // no submission references an import (yet, or ever)
    Pending,    // a submission that references an import may not have retired
    Retired,    // completion was observed; the queue no longer references it
};

enum class WaitAction
{
    Skip,        // nothing to wait for
    Bounded,     // spend one wait of wait_timeout_ms on the host timeline
    DrainQueue,  // drain the game command queue (only once its blocking dependency is resolved)
    Retire,      // wait no more; the imports are given up instead
};

enum class DestroyAction
{
    Destroy,  // the caller may destroy the imported images/memory/semaphores now
    Retire,   // the caller must not destroy them; drop local aliases only
};

// Everything the caller knows when it decides. Values default to the conservative
// answer, so a caller that forgets a field retires resources instead of guessing.
struct Evidence
{
    // Process liveness, sampled at the start of the close and again after the
    // bounded host exit wait (WAIT_OBJECT_0 on the exit wait is NOT fence evidence).
    HostObservation host_at_entry = HostObservation::NotStarted;
    HostObservation host_after_wait = HostObservation::NotStarted;

    GpuState gpu = GpuState::Untouched;

    // True when the outstanding GPU wait can only be satisfied by the host: the
    // add-on queued a Wait(imported fence_out, frame) and the host must signal it.
    bool gpu_wait_depends_on_host = false;

    // Is it possible to observe the imported timeline at all (query support and a
    // live value)? Without it there is no bounded wait and no proof.
    bool bounded_wait_supported = false;

    // Outcome of the single bounded wait the caller spent. host_fence_reached is the
    // proof it must record when the imported timeline value was observed at or past
    // the submitted frame.
    bool bounded_wait_tried = false;
    bool bounded_wait_ok = false;
    bool host_fence_reached = false;

    // Outcome of an attempted queue drain. queue_idle_observed is the proof it must
    // record when the drain returned normally.
    bool queue_drain_tried = false;
    bool queue_idle_observed = false;

    // Can the rendering backend still be called (Vulkan queue present, GL context
    // current)? A lost device cannot be drained or destroyed through.
    bool backend_can_teardown = true;

    // DllMain DLL_PROCESS_DETACH. The loader lock is held and the process is going
    // away: no GPU wait may happen and the OS reclaims the host and its resources.
    bool process_exit = false;
};

struct Decision
{
    WaitAction wait = WaitAction::Retire;
    DestroyAction destroy = DestroyAction::Retire;
    uint32_t wait_timeout_ms = kDefaultWaitTimeoutMs;

    // Diagnostic for the "host disappeared between sampling and its last signal"
    // case: the process is gone but the GPU wait it owns can never be satisfied.
    bool host_exited_before_signal = false;

    // Human-readable reason for logs and tests (static storage: do not free).
    const char *reason = "";
};

inline HostObservation LatestHost(const Evidence &evidence)
{
    // The post-wait sample wins when it exists: the host may have died between the
    // entry sample and the exit wait, and that is exactly the unsafe direction.
    if (evidence.host_after_wait != HostObservation::NotStarted) return evidence.host_after_wait;
    return evidence.host_at_entry;
}

// The whole policy. Deterministic, allocation-free, no I/O.
//
// Contract: when `wait` is Bounded or DrainQueue the returned `destroy` is only a
// provisional plan -- the caller performs that one wait, records the proof it produced
// (host_fence_reached / queue_idle_observed / queue_drain_tried) and calls Decide()
// again. Only a decision with wait == Skip or wait == Retire carries the final
// destroy answer.
inline Decision Decide(const Evidence &evidence)
{
    Decision decision;

    // 1. Process exit: the loader lock is held and the OS reclaims the host handle and
    //    every allocation of this process. Waiting on the GPU here can deadlock the
    //    exit path and can never make destruction safe, so retire unconditionally.
    if (evidence.process_exit)
    {
        decision.wait = WaitAction::Skip;
        decision.destroy = DestroyAction::Retire;
        decision.reason = "process exit: the OS reclaims the host and the imported resources";
        return decision;
    }

    // 2. A lost/lost-context backend cannot be drained or used for destruction. Drop
    //    the local aliases and leave the imported objects to the device teardown.
    if (!evidence.backend_can_teardown)
    {
        decision.wait = WaitAction::Skip;
        decision.destroy = DestroyAction::Retire;
        decision.reason = "the rendering backend is no longer callable; dropping local aliases only";
        return decision;
    }

    // 3. Nothing was submitted against the imports, completion was already observed,
    //    or the queue was drained to completion: destruction is safe, and a dead host
    //    cannot change that.
    if (evidence.gpu == GpuState::Untouched || evidence.gpu == GpuState::Retired ||
        evidence.queue_idle_observed)
    {
        decision.wait = WaitAction::Skip;
        decision.destroy = DestroyAction::Destroy;
        decision.reason = evidence.queue_idle_observed
                              ? "the game queue was observed idle"
                              : "no pending GPU submission references the imports";
        return decision;
    }

    // From here on the GPU work is still Pending.
    if (evidence.queue_drain_tried)
    {
        decision.wait = WaitAction::Retire;
        decision.destroy = DestroyAction::Retire;
        decision.reason = "the game queue did not retire the pending work within its bound";
        return decision;
    }

    const HostObservation latest = LatestHost(evidence);
    const bool host_alive = latest == HostObservation::Alive;

    // 4. The pending work waits on the host's timeline, so only the host can make the
    //    queued GPU wait satisfiable. A value that was already signalled is proof even
    //    if the host died afterwards; a host that died first is not.
    if (evidence.gpu_wait_depends_on_host)
    {
        if (evidence.host_fence_reached)
        {
            decision.wait = WaitAction::DrainQueue;
            decision.destroy = DestroyAction::Retire;  // provisional: decide again after the drain
            decision.reason = "the host already signalled the last frame; draining the queue retires the wait";
            return decision;
        }

        if (!host_alive)
        {
            // The audit gap (R3): the exit wait returned WAIT_OBJECT_0, but the last
            // frame was never signalled. An unbounded wait_idle() would hang forever.
            decision.wait = WaitAction::Retire;
            decision.destroy = DestroyAction::Retire;
            decision.host_exited_before_signal = true;
            decision.reason = "the host exited before signalling the last frame; the queued GPU wait can never retire";
            return decision;
        }

        if (!evidence.bounded_wait_supported)
        {
            decision.wait = WaitAction::Retire;
            decision.destroy = DestroyAction::Retire;
            decision.reason = "no bounded way to observe the host timeline; refusing an unbounded wait";
            return decision;
        }

        if (evidence.bounded_wait_tried)
        {
            // The caller already spent the one bounded wait. It either timed out or
            // returned without a completion proof, so nothing more may be waited on.
            decision.wait = WaitAction::Retire;
            decision.destroy = DestroyAction::Retire;
            decision.host_exited_before_signal = latest == HostObservation::Exited;
            decision.reason = "the bounded wait did not observe the host frame; retiring";
            return decision;
        }

        decision.wait = WaitAction::Bounded;
        decision.destroy = DestroyAction::Retire;  // provisional: decide again after the wait
        decision.reason = "one bounded wait on the host timeline is allowed";
        return decision;
    }

    // 5. The pending work does not depend on the host (its own signal/copy, e.g. the
    //    frame message failed after signal), so the game queue retires it on its own.
    decision.wait = WaitAction::DrainQueue;
    decision.destroy = DestroyAction::Retire;  // provisional: decide again after the drain
    decision.reason = "pending GPU work does not depend on the host; draining the queue is bounded";
    return decision;
}

inline const char *WaitActionName(WaitAction action)
{
    switch (action)
    {
    case WaitAction::Skip: return "skip";
    case WaitAction::Bounded: return "bounded-wait";
    case WaitAction::DrainQueue: return "drain-queue";
    case WaitAction::Retire: return "retire";
    }
    return "?";
}

inline const char *DestroyActionName(DestroyAction action)
{
    switch (action)
    {
    case DestroyAction::Destroy: return "destroy";
    case DestroyAction::Retire: return "retire";
    }
    return "?";
}

}  // namespace teardown_policy
