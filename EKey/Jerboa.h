// Jerboa.h
// EKey-Jerboa V3.0.0  -  Deep subslot engine (slot split by Nth hex symbol)
// Author fork: egorrushka
// Based on VanitySearch by Jean Luc PONS (GPLv3)
//
// NOTE: file name kept as Jerboa.h to avoid touching the build script.
//       Only the Deep engine remains — all scatter code has been removed.
#pragma once
#include "Vanity.h"
#include "Int.h"
#include <string>
#include <cstring>
#include <vector>
#include <utility>

// ════════════════════════════════════════════════════════════════════════════
// DeepEngine — slot split by the Nth hex symbol (-D3..-D6)
//
// The selected chunk (a range of 2nd-symbol slots on the launcher ruler) is
// split by the Nth hex symbol into equal subslots ("slots"):
//   slot count = (#selected 2nd-symbol slots) * 16^(N-2)
//   OFF/-D3 -> x16   -D4 -> x256   -D5 -> x4096   -D6 -> x65536
//
// Two operating modes (driven by the jump interval T = jerboaJumpSec):
//   T finite      -> JUMP mode: hop to the next slot every T seconds, each slot
//                    RESUMES from where it was left (slotPos[]), multi-pass
//                    until every slot is fully read -> honest completion.
//   T=999999999   -> SEQUENTIAL scan: read each slot to the end, then next.
//
// Slot visit order is ALWAYS Random (full-period LCG permutation over slot
// indices). Sequential ordering has been removed.
//
// No scatter anywhere. Resume state is saved per slot into the progress JSON.
// ════════════════════════════════════════════════════════════════════════════

// Per-slot resume array cap. -D6 over the full single-puzzle range gives
// 4*16^5 = 4194304 slots, so the cap covers that case (array ~16 MB). Combos
// that exceed it fall back to infinite Random sampling with resume disabled
// (a setup warning is printed).
#define DEEP_RESUME_MAX  4194304

struct DeepEngine {
    // ── Chunk & subslot ───────────────────────────────────────────────────
    Int      chunkStart, chunkEnd, chunkRange;
    Int      subslotSize;        // key space per subslot = 16^(H - deepSymbol)
    int64_t  totalSubslots;      // total subslots in chunk
    int64_t  subslotSeqIdx;      // sequential position (0..total-1)
    int64_t  lcgA;               // LCG multiplier for random order
    int64_t  lcgState;           // current LCG state = actual subslot index

    Int      subslotStart, subslotEnd;

    // ── Config ────────────────────────────────────────────────────────────
    int      deepSymbol;         // 3..6  (OFF/default = 3)
    bool     randMode;           // always true (Random LCG order; Sequential removed)
    bool     fullRange;          // -FULL: whole chunk = ONE comb, no slots/jumps
    // ── Lift (-L) ──────────────────────────────────────────────────────────
    // The "shaft" is the depth of each tooth, 0..100%. The lift starts digging
    // DOWNWARD from liftFrom% and auto-STOPS at the start of the next already-
    // dug band (liftTo%). dugBands is the per-chunk journal of dug depth
    // intervals (percent). Lift implies fullRange.
    bool     lift;               // -L active
    double   liftFrom;           // start floor %  [0..100)
    double   liftTo;             // auto stop %  = next dug band lo (or 100)
    bool     liftOccupied;       // chosen floor lands inside a dug band
    double   liftSuggest;        // nearest free floor below the occupied band
    std::vector<std::pair<double,double>> dugBands;  // sorted, merged [lo,hi] %
    int      gridProfile;
    int      puzNum;

    // ── Batch state ───────────────────────────────────────────────────────
    // 64-bit: in -FULL single-comb mode batchCount must reach
    // ceil(chunkRange / (numThreads*STEP_SIZE)), which for a 71-bit chunk is
    // ~2^40 — far beyond int range. Resume (Progress = batchCount*STEP_SIZE)
    // also depends on this staying exact.
    uint64_t batchCount;         // current batch within subslot
    uint64_t totalBatches;       // batches needed to cover whole subslot

    // ── Resume / completion ────────────────────────────────────────────────
    int*     slotPos;            // batches done per subslot (size = slotPosCap)
    int64_t  slotPosCap;         // allocated length of slotPos (0 = disabled)
    int64_t  slotsCompleted;     // number of subslots fully read
    int64_t  passCount;          // completed full passes over all slots

    // ── Perf ───────────────────────────────────────────────────────────────
    double   J_sec, jumpStartTime, speedMkey;
    uint64_t slotKeyCount, lastSnap;
    double   lastSpeedTime;

    // ── Thread ───────────────────────────────────────────────────────────────
    int      numThreads, STEP_SIZE_val;

    // ── Files ─────────────────────────────────────────────────────────────────
    char     saveFileBase[200];
    char     progressDir[300];

    // ── Display ─────────────────────────────────────────────────────────────
    char     currentSubslotHex[22];   // short slot prefix (incl. 1st symbol)
    char     currentSlotFullHex[20];  // full hex of current slot start
    bool     displayShown, inited;
    std::string _L[12];
    int      nLines;          // live count of display lines (FULL)
    int      foundSlot;       // slot index of a found key (red), -1 = none

    DeepEngine() :
        totalSubslots(0), subslotSeqIdx(0), lcgA(5), lcgState(0),
        deepSymbol(3), randMode(true), fullRange(false),
        lift(false), liftFrom(0.0), liftTo(100.0),
        liftOccupied(false), liftSuggest(0.0),
        gridProfile(0), puzNum(71),
        batchCount(0), totalBatches(1),
        slotPos(nullptr), slotPosCap(0), slotsCompleted(0), passCount(0),
        J_sec(30.0), jumpStartTime(0.0), speedMkey(1000.0),
        slotKeyCount(0), lastSnap(0), lastSpeedTime(0.0),
        numThreads(1), STEP_SIZE_val(1024),
        displayShown(false), inited(false),
        nLines(4), foundSlot(-1)
    {
        memset(currentSubslotHex, 0, sizeof(currentSubslotHex));
        memset(currentSlotFullHex,0, sizeof(currentSlotFullHex));
        memset(saveFileBase,      0, sizeof(saveFileBase));
        memset(progressDir,       0, sizeof(progressDir));
    }
};

// ── Deep API ─────────────────────────────────────────────────────────────────
void deep_setup   (DeepEngine& e, int numThreads, int STEP_SIZE,
                   BITCRACK_PARAM* bc, int gpuId=0);
bool deep_advance (DeepEngine& e, BITCRACK_PARAM* bc); // true=continue, false=chunk done
void deep_save    (const DeepEngine& e, int gpuId);
bool deep_load    (DeepEngine& e, int gpuId, BITCRACK_PARAM* bc);
void deep_display (DeepEngine& e, uint64_t totalKeys);
void deep_save_launcher(const DeepEngine& e, int gpuId, const char* launcherJson);

// ── Dug-band journal (-L lift) ───────────────────────────────────────────────
void dug_load   (DeepEngine& e, int gpuId);                 // read _dug json
void dug_save   (DeepEngine& e, int gpuId);                 // merge current band + write
void dug_merge  (std::vector<std::pair<double,double>>& v, double lo, double hi);
double dug_nextLo(const DeepEngine& e, double from);        // start of next band above 'from' (or 100)
bool   dug_occupied(const DeepEngine& e, double floorPct, double& suggestOut);
