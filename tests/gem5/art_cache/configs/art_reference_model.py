"""
Pure-Python reference model of the ART (Adaptive Real-Time) accelerator
two-buffer state machine.

This module replicates the logic in art.cc (recvTimingReq / recvTimingResp /
sendARTPrefetchPacket) as a deterministic state machine driven by a known
address sequence.  It produces the exact expected values for every ART stat.

Key assumption
--------------
The test system is configured so that the memory round-trip time is strictly
less than the TrafficGen request period.  This guarantees that every prefetch
(and every MSHR / bypass request) completes and its response is fully
processed before the next CPU request arrives.  Under this assumption the
model can process each request and its consequences atomically — no need to
track tick-level timing.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass

# -------------------------------------------------------------------
#  Address helpers (mirror art.hh inline methods)
# -------------------------------------------------------------------


def align_down(addr: int, blk_size: int) -> int:
    return addr & ~(blk_size - 1)


def next_sequential(addr: int, blk_size: int) -> int:
    return align_down(addr, blk_size) + blk_size


def in_flash_range(addr: int, flash_start: int, flash_end: int) -> int:
    return flash_start <= addr <= flash_end


def _hex(addr: int) -> str:
    """Hex without 0x prefix — matches gem5's addrToString()."""
    return f"{addr:x}"


# -------------------------------------------------------------------
#  Prefetch buffer model
# -------------------------------------------------------------------


@dataclass
class PrefetchBuffer:
    blk_size: int
    addr: int = 0
    valid: bool = False

    def is_hit(self, query_addr: int) -> bool:
        return (
            self.valid and align_down(query_addr, self.blk_size) == self.addr
        )

    def store(self, addr: int) -> None:
        self.addr = addr
        self.valid = True

    def invalidate(self) -> None:
        self.valid = False

    def copy_from(self, other: PrefetchBuffer) -> None:
        self.addr = other.addr
        self.valid = other.valid

    def state_str(self) -> str:
        if self.valid:
            return f"valid addr={_hex(self.addr)}"
        return "invalid"


# -------------------------------------------------------------------
#  Prefetch queue entry model
# -------------------------------------------------------------------

ENTRY_EMPTY = 0
ENTRY_ALLOCATED = 1
ENTRY_IN_SERVICE = 2


@dataclass
class PfEntry:
    addr: int = 0
    state: int = ENTRY_EMPTY

    def ready(self) -> bool:
        return self.state == ENTRY_ALLOCATED

    def in_service(self) -> bool:
        return self.state == ENTRY_IN_SERVICE

    def has_target(self) -> bool:
        return self.state != ENTRY_EMPTY

    def allocate(self, addr: int) -> None:
        self.addr = addr
        self.state = ENTRY_ALLOCATED

    def mark_in_service(self) -> None:
        assert self.state == ENTRY_ALLOCATED
        self.state = ENTRY_IN_SERVICE

    def deallocate(self) -> None:
        self.state = ENTRY_EMPTY

    def match_block(self, addr: int, blk_size: int) -> bool:
        return self.addr == align_down(addr, blk_size)

    def state_str(self) -> str:
        if self.state == ENTRY_EMPTY:
            return "EMPTY"
        if self.state == ENTRY_ALLOCATED:
            return f"ALLOC@{_hex(self.addr)}"
        return f"IN_SVC@{_hex(self.addr)}"


# -------------------------------------------------------------------
#  Stats accumulator
# -------------------------------------------------------------------


@dataclass
class ARTStats:
    currentBufferHits: int = 0
    prefetchBufferHits: int = 0
    prefetchMisses: int = 0
    bypassAccesses: int = 0
    cpuWaitEvents: int = 0
    prefetchesAllocated: int = 0
    prefetchesIssued: int = 0
    prefetchesCompleted: int = 0

    def as_dict(self) -> dict[str, int]:
        return {
            "currentBufferHits": self.currentBufferHits,
            "prefetchBufferHits": self.prefetchBufferHits,
            "prefetchMisses": self.prefetchMisses,
            "bypassAccesses": self.bypassAccesses,
            "cpuWaitEvents": self.cpuWaitEvents,
            "prefetchesAllocated": self.prefetchesAllocated,
            "prefetchesIssued": self.prefetchesIssued,
            "prefetchesCompleted": self.prefetchesCompleted,
        }

    def summary(self) -> str:
        return (
            f"hits={self.currentBufferHits} "
            f"pfHits={self.prefetchBufferHits} "
            f"misses={self.prefetchMisses} "
            f"alloc={self.prefetchesAllocated} "
            f"issued={self.prefetchesIssued} "
            f"completed={self.prefetchesCompleted}"
        )


# -------------------------------------------------------------------
#  ART state-machine model
# -------------------------------------------------------------------


class ARTModel:
    """
    Cycle-accurate reference model of the ART two-buffer prefetch
    state machine.  Mirrors the logic in art.cc line-by-line.
    """

    def __init__(
        self,
        pf_blk_size: int = 8,
        flash_start: int = 0,
        flash_end: int = (1 << 64) - 1,
        enable_prefetch: bool = True,
        direct_memory_mode: bool = False,
        prefetch_on_cache_hit: bool = False,
        trace: bool = False,
        period: int = 0,
    ):
        self.pf_blk_size = pf_blk_size
        self.flash_start = flash_start
        self.flash_end = flash_end
        self.enable_prefetch = enable_prefetch
        self.direct_memory_mode = direct_memory_mode
        self.prefetch_on_cache_hit = prefetch_on_cache_hit
        self.trace = trace
        self.period = period

        self.current_buf = PrefetchBuffer(pf_blk_size)
        self.prefetch_buf = PrefetchBuffer(pf_blk_size)
        self.pf_entry = PfEntry()
        self.next_pf_addr: int = 0

        self.cache_tags: set[int] = set()

        self.stats = ARTStats()
        self._req_num: int = 0

    # -- trace helper (matches gem5 DPRINTF format) --

    def _t(self, tick: int, msg: str) -> None:
        if self.trace:
            print(f"{tick:>7d}: ref_model: {msg}", file=sys.stderr)

    def _tick(self) -> int:
        """Simulated tick for the current request.
        Called after _req_num has been incremented, so req_num * period
        gives the correct tick (first request at 1 * period).
        """
        return self._req_num * self.period

    # -- helpers --

    def _in_flash(self, addr: int) -> bool:
        return in_flash_range(addr, self.flash_start, self.flash_end)

    def _blk_addr(self, addr: int) -> int:
        return align_down(addr, self.pf_blk_size)

    def _next_seq(self, addr: int) -> int:
        return next_sequential(addr, self.pf_blk_size)

    def _is_in_cache(self, addr: int) -> bool:
        return self._blk_addr(addr) in self.cache_tags

    # -- send prefetch (mirrors sendARTPrefetchPacket) --

    def _send_prefetch(self) -> None:
        """Transition an ALLOCATED entry to IN_SERVICE."""
        assert self.pf_entry.ready()
        self.pf_entry.mark_in_service()
        self.stats.prefetchesIssued += 1
        tick = self._tick()
        self._t(
            tick,
            f"sendARTPrefetchPacket: sent prefetch for "
            f"addr={_hex(self.pf_entry.addr)} "
            f"(issued={self.stats.prefetchesIssued})",
        )

    # -- prefetch response (mirrors recvTimingResp for prefetch) --

    def _complete_prefetch(self) -> None:
        """
        Process the memory response for a prefetch that is IN_SERVICE.
        Under the "instant response" assumption this is called right
        after _send_prefetch within the same logical request step.
        """
        assert self.pf_entry.in_service()
        self.stats.prefetchesCompleted += 1
        completed_addr = self.pf_entry.addr
        self.prefetch_buf.store(self.pf_entry.addr)
        tick = self._tick()
        self._t(
            tick,
            f"recvTimingResp: prefetch response for "
            f"addr={_hex(completed_addr)}",
        )
        self._t(
            tick,
            f"recvTimingResp: stats "
            f"alloc={self.stats.prefetchesAllocated} "
            f"issued={self.stats.prefetchesIssued} "
            f"completed={self.stats.prefetchesCompleted}",
        )

        served_cpu = False

        self.pf_entry.deallocate()

        if not self.current_buf.valid:
            if not served_cpu:
                self.current_buf.copy_from(self.prefetch_buf)
                self._t(
                    tick,
                    "recvTimingResp: promoted prefetch -> " "current buffer",
                )
            self.prefetch_buf.invalidate()

            if self.pf_entry.addr == self.next_pf_addr:
                old = self.next_pf_addr
                self.next_pf_addr = self._next_seq(self.next_pf_addr)
                self._t(
                    tick,
                    f"recvTimingResp: advancing next prefetch "
                    f"addr to {_hex(self.next_pf_addr)}",
                )

            if self._in_flash(self.next_pf_addr):
                self.pf_entry.allocate(self.next_pf_addr)
                self.stats.prefetchesAllocated += 1
                self._t(
                    tick,
                    f"recvTimingResp: issued follow-up "
                    f"prefetch for addr={_hex(self.next_pf_addr)} "
                    f"(allocated={self.stats.prefetchesAllocated})",
                )
                self._send_prefetch()
                self._complete_prefetch()
            else:
                self._t(
                    tick,
                    f"recvTimingResp: next prefetch "
                    f"addr={_hex(self.next_pf_addr)} outside flash "
                    f"range, skipping",
                )
        else:
            self._t(
                tick,
                "recvTimingResp: currentBuffer valid, "
                "skipping follow-up prefetch",
            )

    # -- main request handler (mirrors recvTimingReq) --

    def process_request(self, addr: int) -> None:
        """Process a single CPU read request at *addr*."""
        req_num = self._req_num
        self._req_num += 1
        tick = self._tick()

        self._t(tick, f"recvTimingReq: addr={_hex(addr)}")
        self._t(
            tick,
            f"recvTimingReq: state "
            f"curBuf=({self.current_buf.state_str()}) "
            f"pfBuf=({self.prefetch_buf.state_str()}) "
            f"pfEntry={self.pf_entry.state_str()}",
        )

        prefetch_hit = False

        if self.enable_prefetch:
            # --- try to serve from buffers ---
            if self.current_buf.is_hit(addr):
                prefetch_hit = True
                self.stats.currentBufferHits += 1
                self._t(tick, "recvTimingReq: current buffer hit")
                self._t(
                    tick, f"recvTimingReq: stats " f"{self.stats.summary()}"
                )
            elif self.prefetch_buf.is_hit(addr):
                prefetch_hit = True
                self.stats.prefetchBufferHits += 1
                self.prefetch_buf.invalidate()
                self._t(tick, "recvTimingReq: prefetch buffer hit")
                self._t(
                    tick, f"recvTimingReq: stats " f"{self.stats.summary()}"
                )

            # --- compute next prefetch address ---
            self.next_pf_addr = self._next_seq(addr)
            self._t(
                tick,
                f"recvTimingReq: next sequential "
                f"addr={_hex(self.next_pf_addr)}",
            )

            if self.prefetch_buf.is_hit(self.next_pf_addr):
                self.next_pf_addr = self._next_seq(self.next_pf_addr)
                self._t(
                    tick,
                    "recvTimingReq: next addr already in "
                    "prefetch buffer, advancing",
                )
            elif self.pf_entry.in_service() and self.pf_entry.match_block(
                self.next_pf_addr, self.pf_blk_size
            ):
                self.next_pf_addr = self._next_seq(self.next_pf_addr)
                self._t(
                    tick,
                    "recvTimingReq: next addr already " "in-flight, advancing",
                )

            self._t(
                tick,
                f"recvTimingReq: final next prefetch "
                f"addr={_hex(self.next_pf_addr)}",
            )

            # --- invalidate current buffer ---
            self.current_buf.invalidate()

            if not self.pf_entry.in_service():
                # promote prefetch -> current
                if self.prefetch_buf.valid:
                    self.current_buf.copy_from(self.prefetch_buf)
                    self.prefetch_buf.invalidate()
                    self._t(
                        tick,
                        "recvTimingReq: promoting prefetch "
                        "-> current buffer",
                    )

                # discard stale entry
                if self.pf_entry.has_target():
                    self._t(
                        tick,
                        f"recvTimingReq: discarding stale "
                        f"prefetch target "
                        f"addr={_hex(self.pf_entry.addr)}",
                    )
                    self.pf_entry.deallocate()

                # allocate new prefetch if in flash range
                if self._in_flash(self.next_pf_addr):
                    self.pf_entry.allocate(self.next_pf_addr)
                    self.stats.prefetchesAllocated += 1
                    self._t(
                        tick,
                        f"recvTimingReq: allocated prefetch "
                        f"for addr={_hex(self.next_pf_addr)} "
                        f"(allocated="
                        f"{self.stats.prefetchesAllocated})",
                    )

                    if prefetch_hit:
                        self._t(
                            tick,
                            f"recvTimingReq: scheduling "
                            f"prefetch for "
                            f"{_hex(self.next_pf_addr)} "
                            f"immediately",
                        )
                        self._send_prefetch()
                        self._complete_prefetch()
                    else:
                        self._t(
                            tick,
                            "recvTimingReq: prefetch will be "
                            "scheduled after cache event",
                        )
                else:
                    self._t(
                        tick,
                        f"recvTimingReq: next prefetch "
                        f"addr={_hex(self.next_pf_addr)} outside "
                        f"flash range, skipping",
                    )
            else:
                self._t(tick, "recvTimingReq: prefetch in service")
                # prefetch in service — check if CPU must wait
                if self.pf_entry.match_block(addr, self.pf_blk_size):
                    if not self._is_in_cache(addr):
                        prefetch_hit = True
                        self.stats.cpuWaitEvents += 1
                        self._t(
                            tick,
                            "recvTimingReq: CPU waiting for "
                            "in-flight prefetch",
                        )
                    else:
                        self._t(
                            tick,
                            "recvTimingReq: data already in "
                            "cache, no need to wait",
                        )

        # --- early return if served by prefetch mechanism ---
        if prefetch_hit:
            self._t(
                tick,
                "recvTimingReq: served by prefetch "
                "mechanism, skipping cache access",
            )
            return

        # --- direct memory bypass ---
        if self.direct_memory_mode:
            self.stats.bypassAccesses += 1
            self._t(
                tick,
                f"recvTimingReq: direct memory bypass for "
                f"addr={_hex(addr)}",
            )
            self._t(
                tick,
                f"recvTimingReq: bypass "
                f"(bypass={self.stats.bypassAccesses})",
            )
            return

        # --- prefetch miss (goes through NoncoherentCache MSHR) ---
        if self.enable_prefetch and not prefetch_hit:
            self.stats.prefetchMisses += 1
            self._t(
                tick,
                f"recvTimingReq: prefetch miss "
                f"(misses={self.stats.prefetchMisses})",
            )

        # Check for I-Cache hit before installing the block.
        is_cache_hit = self._is_in_cache(addr)

        # MSHR path: the block gets installed in the cache tag store.
        blk = self._blk_addr(addr)
        self.cache_tags.add(blk)
        self._t(tick, "recvTimingReq: forwarding to NoncoherentCache")

        if self.pf_entry.ready():
            if not is_cache_hit:
                self._send_prefetch()
                self._complete_prefetch()
            elif self.prefetch_on_cache_hit:
                self._t(
                    tick,
                    "recvTimingReq: prefetch entry still "
                    "ready after cache access, scheduling send "
                    "(prefetchOnCacheHit)",
                )
                self._send_prefetch()
                self._complete_prefetch()


# -------------------------------------------------------------------
#  Traffic-pattern generators
# -------------------------------------------------------------------


def generate_linear_addresses(
    start_addr: int,
    end_addr: int,
    block_size: int,
    duration: int,
    period: int,
) -> list[int]:
    """
    Reproduce gem5 LinearGen: sequential addresses from *start_addr*
    to *end_addr* (exclusive), stepping by *block_size*, wrapping at
    the end.

    gem5 LinearGen emits its first packet at state_start + period, so
    the total count is duration // period - 1.
    """
    num_requests = duration // period - 1
    addrs = []
    cur = start_addr
    for _ in range(num_requests):
        addrs.append(cur)
        cur += block_size
        if cur >= end_addr:
            cur = start_addr
    return addrs


def generate_branch_addresses(
    seq_len: int,
    targets: list[int],
    block_size: int,
    num_requests: int,
) -> list[int]:
    """
    Generate a deterministic non-sequential pattern that models
    branch-heavy code.

    Produces *seq_len* sequential fetches starting from each target,
    then jumps to the next target in the list.  Cycles through
    *targets* until *num_requests* are emitted.
    """
    addrs = []
    target_idx = 0
    while len(addrs) < num_requests:
        base = targets[target_idx % len(targets)]
        for i in range(seq_len):
            if len(addrs) >= num_requests:
                break
            addrs.append(base + i * block_size)
        target_idx += 1
    return addrs


# -------------------------------------------------------------------
#  High-level entry point used by the test runner
# -------------------------------------------------------------------

CYCLE = 1000  # ticks per cycle at 1 GHz
BLOCK_SIZE = 8
PF_BLK_SIZE = 8
SEQ_PERIOD = 20 * CYCLE  # 20 cycles — enough for follow-up prefetch chain
BYPASS_PERIOD = 10 * CYCLE
DURATION = 20_000_000  # 20M ticks — keeps ~999 requests at 20-cycle period
SHORT_DURATION = 10_000_000

# Branch-penalty: deterministic targets that defeat sequential prefetch.
BRANCH_TARGETS = [
    0x0000,
    0x1000,
    0x0800,
    0x1800,
    0x0400,
    0x1400,
    0x0C00,
    0x1C00,
    0x0200,
    0x1200,
    0x0A00,
    0x1A00,
    0x0600,
    0x1600,
    0x0E00,
    0x1E00,
]
BRANCH_SEQ_LEN = 1

# Each cycling state lasts 2 * SEQ_PERIOD so the first (and only)
# packet fires before the state transitions.
BRANCH_STATE_DURATION = 2 * SEQ_PERIOD


def _build_model_and_addrs(
    test_type, trace=False, prefetch_on_cache_hit=False
):
    """Return (model, address_list) for *test_type*."""
    if test_type == "hw_sequential_prefetch":
        addrs = generate_linear_addresses(
            0, 4096, BLOCK_SIZE, DURATION, SEQ_PERIOD
        )
        model = ARTModel(
            pf_blk_size=PF_BLK_SIZE,
            flash_start=0,
            flash_end=(1 << 64) - 1,
            enable_prefetch=True,
            direct_memory_mode=False,
            prefetch_on_cache_hit=prefetch_on_cache_hit,
            trace=trace,
            period=SEQ_PERIOD,
        )

    elif test_type == "hw_branch_penalty":
        n_targets = len(BRANCH_TARGETS)
        one_cycle_ticks = n_targets * BRANCH_STATE_DURATION
        num_req = DURATION // one_cycle_ticks * n_targets
        remaining_states = (
            DURATION % one_cycle_ticks
        ) // BRANCH_STATE_DURATION
        num_req += remaining_states
        addrs = generate_branch_addresses(
            BRANCH_SEQ_LEN, BRANCH_TARGETS, BLOCK_SIZE, num_req
        )
        model = ARTModel(
            pf_blk_size=PF_BLK_SIZE,
            flash_start=0,
            flash_end=(1 << 64) - 1,
            enable_prefetch=True,
            direct_memory_mode=False,
            prefetch_on_cache_hit=prefetch_on_cache_hit,
            trace=trace,
            period=BRANCH_STATE_DURATION,
        )

    elif test_type == "hw_buffer_promotion":
        addrs = generate_linear_addresses(
            0, 2048, BLOCK_SIZE, DURATION, SEQ_PERIOD
        )
        model = ARTModel(
            pf_blk_size=PF_BLK_SIZE,
            flash_start=0,
            flash_end=(1 << 64) - 1,
            enable_prefetch=True,
            direct_memory_mode=False,
            prefetch_on_cache_hit=prefetch_on_cache_hit,
            trace=trace,
            period=SEQ_PERIOD,
        )

    elif test_type == "hw_prefetch_disable":
        addrs = generate_linear_addresses(
            0, 4096, BLOCK_SIZE, DURATION, SEQ_PERIOD
        )
        model = ARTModel(
            pf_blk_size=PF_BLK_SIZE,
            enable_prefetch=False,
            direct_memory_mode=False,
            trace=trace,
            period=SEQ_PERIOD,
        )

    elif test_type == "hw_direct_memory_bypass":
        addrs = generate_linear_addresses(
            0, 4096, BLOCK_SIZE, DURATION, BYPASS_PERIOD
        )
        model = ARTModel(
            pf_blk_size=PF_BLK_SIZE,
            flash_start=0,
            flash_end=(1 << 64) - 1,
            enable_prefetch=False,
            direct_memory_mode=True,
            trace=trace,
            period=BYPASS_PERIOD,
        )

    elif test_type == "hw_flash_range_bounds":
        addrs = generate_linear_addresses(
            0, 1024, BLOCK_SIZE, DURATION, SEQ_PERIOD
        )
        model = ARTModel(
            pf_blk_size=PF_BLK_SIZE,
            flash_start=0x0000,
            flash_end=0x00FF,
            enable_prefetch=True,
            direct_memory_mode=False,
            prefetch_on_cache_hit=prefetch_on_cache_hit,
            trace=trace,
            period=SEQ_PERIOD,
        )

    elif test_type == "hw_completion_fidelity":
        addrs = generate_linear_addresses(
            0, 512, BLOCK_SIZE, SHORT_DURATION, SEQ_PERIOD
        )
        model = ARTModel(
            pf_blk_size=PF_BLK_SIZE,
            flash_start=0,
            flash_end=(1 << 64) - 1,
            enable_prefetch=True,
            direct_memory_mode=False,
            prefetch_on_cache_hit=prefetch_on_cache_hit,
            trace=trace,
            period=SEQ_PERIOD,
        )
    else:
        raise ValueError(f"Unknown test type: {test_type}")

    return model, addrs


def compute_expected_stats(
    test_type: str,
    prefetch_on_cache_hit: bool = False,
) -> dict[str, int]:
    """
    Compute the exact expected ART statistics for *test_type* by
    running the reference model over the same traffic pattern used
    by the gem5 test.
    """
    model, addrs = _build_model_and_addrs(
        test_type, trace=False, prefetch_on_cache_hit=prefetch_on_cache_hit
    )
    for a in addrs:
        model.process_request(a)
    return model.stats.as_dict()


# -------------------------------------------------------------------
#  Standalone execution for development / debugging
# -------------------------------------------------------------------

if __name__ == "__main__":
    import argparse

    all_test_types = [
        "hw_sequential_prefetch",
        "hw_branch_penalty",
        "hw_buffer_promotion",
        "hw_prefetch_disable",
        "hw_direct_memory_bypass",
        "hw_flash_range_bounds",
        "hw_completion_fidelity",
    ]

    parser = argparse.ArgumentParser(
        description="ART reference model -- compute expected stats"
    )
    parser.add_argument(
        "tests", nargs="*", default=all_test_types, help="Test type(s) to run"
    )
    parser.add_argument(
        "--trace",
        action="store_true",
        help="Print per-request trace to stderr",
    )
    parser.add_argument(
        "--prefetch-on-cache-hit",
        action="store_true",
        help="Issue prefetch immediately on I-Cache hit",
    )
    args = parser.parse_args()

    for tt in args.tests:
        pf_label = "ON" if args.prefetch_on_cache_hit else "OFF"
        print(f"\n{'=' * 60}")
        print(
            f"  {tt}  (trace={'ON' if args.trace else 'OFF'}, "
            f"prefetchOnCacheHit={pf_label})"
        )
        print(f"{'=' * 60}")

        model, addrs = _build_model_and_addrs(
            tt,
            trace=args.trace,
            prefetch_on_cache_hit=args.prefetch_on_cache_hit,
        )
        print(f"  address count: {len(addrs)}", file=sys.stderr)
        for a in addrs:
            model.process_request(a)

        expected = model.stats.as_dict()
        for k, v in expected.items():
            print(f"  {k:30s} = {v}")
