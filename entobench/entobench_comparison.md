# Entobench: gem5 ART vs No-ART and Real Hardware Comparison

**Configuration:** STM32G474RE @ 170 MHz, dual bank
**gem5 CPU:** SingleStageFetch MinorCPU, fetch1LineWidth=4
**gem5 firmware:** INNER_REPS=1 (entobench default), --inner-reps 1
**Cycle measurement:** Precise kernel-only (excluding m5op semihosting overhead)
**Date:** 2026-04-08

---

## Section 1: gem5 ART vs No-ART


| Benchmark          | No-ART (cyc) | ART (cyc) | Speedup | Diff%  |
| ------------------ | ------------ | --------- | ------- | ------ |
| madgwick-float-imu | 602          | 513       | 1.17x   | -14.8% |
| 5pt-float          | 115,516      | 82,131    | 1.41x   | -28.9% |
| robofly-lqr        | 390          | 318       | 1.23x   | -18.5% |
| robofly-tinympc    | 5,338,639    | 3,362,513 | 1.59x   | -37.0% |
| fastbrief-small    | 2,809,689    | 1,700,945 | 1.65x   | -39.5% |


**Notes:**

- All cycles are precise kernel-only (m5op semihosting overhead of 20-39 cycles excluded)
- All benchmarks are faster with ART cache enabled
- fastbrief-small and robofly-tinympc benefit most (1.59-1.65x) — large code footprints
- madgwick-float-imu benefits least (1.17x) — small tight FP kernel

---

## Section 2: gem5 vs Real Hardware — ART (C+P)


| Benchmark          | gem5 ART (cyc) | HW C+P (cyc/call) | HW Reps | Error%     |
| ------------------ | -------------- | ----------------- | ------- | ---------- |
| madgwick-float-imu | 513            | 386.5             | 50      | +32.7%     |
| 5pt-float          | 82,131         | 115,937.2         | 1       | -29.2%     |
| robofly-lqr        | 318            | 22,958.0          | 2       | -98.6%     |
| robofly-tinympc    | 3,362,513      | 28,520.7          | 2       | +11,689.7% |


---

## Section 3: gem5 vs Real Hardware — No-ART (None)


| Benchmark          | gem5 No-ART (cyc) | HW None (cyc/call) | HW Reps | Error%     |
| ------------------ | ----------------- | ------------------ | ------- | ---------- |
| madgwick-float-imu | 602               | 388.3              | 50      | +55.0%     |
| 5pt-float          | 115,516           | 125,324.8          | 1       | -7.8%      |
| robofly-lqr        | 390               | 22,994.0           | 2       | -98.3%     |
| robofly-tinympc    | 5,338,639         | 29,943.2           | 2       | +17,729.2% |


---

## Section 4: Relative Accuracy (Speedup Comparison)


| Benchmark          | HW Speedup (None→C+P) | gem5 Speedup (NoART→ART) | Speedup Error |
| ------------------ | --------------------- | ------------------------ | ------------- |
| madgwick-float-imu | 1.00x                 | 1.17x                    | +16.8%        |
| robofly-lqr        | 1.00x                 | 1.23x                    | +22.4%        |
| 5pt-float          | 1.08x                 | 1.41x                    | +30.1%        |
| robofly-tinympc    | 1.05x                 | 1.59x                    | +51.2%        |



| Metric                 | Value |
| ---------------------- | ----- |
| Mean abs speedup error | 30.1% |
| Max abs speedup error  | 51.2% |


---

## Section 5: Analysis

### Reliable: 5pt-float

- NoART: gem5=115,516 vs HW=125,325: **-7.8% error** — excellent
- ART: gem5=82,131 vs HW=115,937: **-29.2%** — gem5 faster, cache too effective

### Moderate: madgwick-float-imu

- ART: **+32.7%** error — gem5 slower due to 1-cycle I-Cache hit latency
- NoART: **+55.0%** error — larger gap without cache benefit

### Configuration mismatch: robofly-lqr, robofly-tinympc

- robofly-lqr: gem5=318 vs HW=22,958 — gem5 runs far less work per call
- robofly-tinympc: gem5=3.36M vs HW=28,521 — gem5 runs far more work per call
- These need configuration alignment before meaningful comparison

### Known issues

- 5pt-float ART: slow semihosting file I/O for dataset loading (~5 min)

---

## HW Reference Data


| Benchmark          | HW C+P (total) | HW None (total) | Inner Reps | HW C+P (per call) | HW None (per call) |
| ------------------ | -------------- | --------------- | ---------- | ----------------- | ------------------ |
| madgwick-float-imu | 19,323.04      | 19,413.87       | 50         | 386.5             | 388.3              |
| 5pt-float          | 115,937.22     | 125,324.82      | 1          | 115,937.2         | 125,324.8          |
| robofly-lqr        | 45,916         | 45,988          | 2          | 22,958.0          | 22,994.0           |
| robofly-tinympc    | 57,041.4       | 59,886.36       | 2          | 28,520.7          | 29,943.2           |


**Note:** fastbrief-small does not have HW reference data yet.
