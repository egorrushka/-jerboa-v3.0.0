// main.cpp
// EKey-Jerboa V3.0.0
// Author: egorrushka
// Based on VanitySearch by Jean Luc PONS (GPLv3)
// License: GPLv3 - https://www.gnu.org/licenses/
//
// Usage:
//   EKey-Jerboa.exe -a <address> -s 0xSTART -e 0xEND -T <seconds> [-W <0-7>] [-G <gpuId>] [-b]

#include <sstream>
#include "Timer.h"
#include "Vanity.h"
#include "Jerboa.h"
#include "SECP256k1.h"
#include <string>
#include <string.h>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <vector>
#include <csignal>
#include <cuda_runtime.h>
#include <io.h>

#if defined(_WIN32)||defined(_WIN64)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

std::atomic<bool> Pause(false);
std::atomic<bool> Paused(false);
std::atomic<bool> stopMonitorKey(false);
int    idxcount  = 0;
double t_Paused  = 0.0;
bool   backupMode= false;
bool   autoMode  = false;
using namespace std;

VanitySearch* g_vs = nullptr;
std::atomic<bool> g_shutdown(false);

void signalHandler(int sig) {
    if (!backupMode) { printf("\n"); fflush(stdout); exit(sig); }
    if (g_shutdown.exchange(true)) exit(sig);
    printf("\n[!] Ctrl+C - shutting down..."); fflush(stdout);
    if (g_vs) g_vs->endOfSearch = true;
}

#if defined(_WIN32)||defined(_WIN64)
void monitorKeypress() {
    while (!stopMonitorKey) {
        Timer::SleepMillis(1);
        if (_kbhit()) {
            char ch = _getch();
            if (ch=='p'||ch=='P') { Pause = !Pause.load(); }
        }
    }
}
#else
// Linux keyboard monitor (simplified)
void monitorKeypress() {
    while (!stopMonitorKey) { Timer::SleepMillis(50); }
}
#endif

static void printFaq() {
    printf("\n");
    printf("=======================================================================\n");
    printf("  EKey-Jerboa V3.0.0 -- FULL FAQ & MANUAL\n");
    printf("=======================================================================\n\n");

    printf("1. WHAT THE PROGRAM DOES\n");
    printf("   EKey-Jerboa searches a chosen key range on the GPU for the private\n");
    printf("   key of a target Bitcoin address (P2PKH, compressed) or public key.\n");
    printf("   You pick a range -- a \"chunk\" -- and the GPU generates private keys,\n");
    printf("   derives their public points on secp256k1 (the same curve Bitcoin\n");
    printf("   uses), hashes each point into a P2PKH address (SHA256 then\n");
    printf("   RIPEMD160) and compares it with the target, reporting the key the\n");
    printf("   instant it is found. It is a brute-force search over the open\n");
    printf("   Bitcoin Puzzle tasks. All heavy work runs on the GPU via CUDA.\n\n");

    printf("2. TWO ENGINES AT A GLANCE\n");
    printf("   - FULL comb (-FULL / -L; this is what the GUI launcher drives):\n");
    printf("     ONE \"comb\" of teeth (GPU threads) is laid over the WHOLE chunk at\n");
    printf("     once and an infinite van der Corput (vdC) teleport sweeps it\n");
    printf("     across depth. Main mode. See sections 4-6, 8-10.\n");
    printf("   - Deep slots (-D3..-D6, without -FULL): the chunk is cut into equal\n");
    printf("     slots visited in random order, hopping on a timer (-T), with\n");
    printf("     per-slot resume and honest completion. See section 7.\n\n");

    printf("3. OPTIONS\n");
    printf("   -a <addr>    Target P2PKH (compressed) Bitcoin address\n");
    printf("   -p <pubkey>  Target compressed public key (alternative to -a)\n");
    printf("   -s 0x<hex>   Chunk start        -e 0x<hex>  Chunk end\n");
    printf("   -r <bits>    Bit-range shorthand (e.g. -r 71 = 2^70 .. 2^71-1)\n");
    printf("   -T <sec>     Slot jump interval, Deep mode (default 30).\n");
    printf("                999999999 = read each slot to the end (no jumps)\n");
    printf("   -G <id>      GPU device id (default 0)\n");
    printf("   -W <0-7>     Grid: 0=auto  5=6144x256 (default)  7=12288x256\n");
    printf("   -b           Save/Resume progress (per-chunk progress journal)\n");
    printf("   -D4..-D6     Deep slot split deeper (OFF/default = -D3)\n");
    printf("   -FULL        ONE comb over the WHOLE chunk; no slots, no jumps\n");
    printf("   -L <pct>     Lift: dig the comb downward from <pct>%% depth,\n");
    printf("                auto-stop at the next already-dug band. Implies -FULL\n");
    printf("   --auto       Poll autocmd_gpuN.txt for vdC teleport commands from\n");
    printf("                the launcher (persistent process, no console restart)\n");
    printf("   -faq / -inf  This manual / version & credits\n\n");

    printf("=======================================================================\n");
    printf("  FULL COMB MODE (the synchronous \"comb\")\n");
    printf("=======================================================================\n\n");

    printf("4. THE COMB -- GEOMETRY\n");
    printf("   Instead of chopping the chunk into pieces and walking them one by\n");
    printf("   one, the engine lays a SINGLE comb of many teeth (GPU threads) over\n");
    printf("   the WHOLE chunk at once.\n");
    printf("   - The chunk [ksStart, ksFinish] is divided into numThreads equal\n");
    printf("     regions. Grid \"5\" = 6144 x 256 = 1,572,864 threads = the teeth.\n");
    printf("   - One tooth's region width:\n");
    printf("        stepThread = (ksFinish - ksStart + 1) / numThreads\n");
    printf("   - Tooth thId (0..numThreads-1) owns its own region starting at\n");
    printf("        ksStart + thId * stepThread.\n");
    printf("   The teeth stand on an even grid across the whole range, stepThread\n");
    printf("   apart. Any \"slice\" of the comb pierces the entire chunk at once --\n");
    printf("   one point per region. This is HORIZONTAL coverage of the chunk.\n\n");

    printf("5. THE COMB -- KEY FORMULA & CHARACTER LAYOUT\n");
    printf("   The key a tooth thId checks at a given step is:\n");
    printf("        key = ksStart + thId*stepThread + batch*STEP_SIZE + j\n");
    printf("   where STEP_SIZE = 1024 keys per batch per tooth, batch is the batch\n");
    printf("   number (grows as we dig), and j (0..STEP_SIZE-1) is the position\n");
    printf("   inside the batch. The term (batch*STEP_SIZE + j) is the common\n");
    printf("   \"offset\" inside every region -- the SAME for all teeth, so the whole\n");
    printf("   comb moves SYNCHRONOUSLY.\n");
    printf("   Character layout (example, 18 hex symbols, chunk 0x4XXXX...):\n");
    printf("     - Chunk symbol (the leading 4)            : frozen by the chunk\n");
    printf("     - High-middle ~5-6 symbols                : handed out per thId,\n");
    printf("       all ~1.5M combinations are scanned IN PARALLEL, simultaneously\n");
    printf("     - Low ~11-12 symbols                      : a SEQUENTIAL counter\n");
    printf("       (offset) inside each tooth, lowest symbol fastest, carry up\n");
    printf("   Symbols are NOT permuted: high symbols go to teeth (parallel), low\n");
    printf("   symbols are a plain increment inside a tooth. \"Pretty\" and \"messy\"\n");
    printf("   keys are equal -- the counter passes through every one exactly once.\n\n");

    printf("6. THE COMB -- \"FRONT\" AND DEPTH\n");
    printf("   - The FRONT is the common offset of all teeth: a thin line of ~1.5M\n");
    printf("     points cutting every region at one depth.\n");
    printf("   - offset = batch*STEP_SIZE. As the engine digs, batch grows and the\n");
    printf("     front goes deeper into every region, all teeth in lock-step.\n");
    printf("   - Depth as a fraction:\n");
    printf("        phase = offset / stepThread = batchCount / totalBatches  (0..1)\n");
    printf("     where totalBatches = stepThread / STEP_SIZE (batches needed for a\n");
    printf("     tooth to cross its whole region).\n");
    printf("   Key consequence: the whole comb has ONE shared position (batchCount).\n");
    printf("   There is no separate \"where is each tooth\" memory -- any tooth's\n");
    printf("   position is computed from thId and the shared batchCount.\n");
    printf("   On reaching 100%% depth the engine closes the floor-division\n");
    printf("   remainder [ksStart + numThreads*stepThread, ksFinish] with a short\n");
    printf("   CPU tail sweep (< numThreads keys) for an honest, complete pass.\n\n");

    printf("=======================================================================\n");
    printf("  THE vdC TELEPORT (van der Corput)\n");
    printf("=======================================================================\n\n");

    printf("7A. WHY\n");
    printf("    The comb covers the chunk horizontally (high symbols). vdC handles\n");
    printf("    the VERTICAL -- even movement of the front by depth. If you simply\n");
    printf("    dug the front straight down (offset 0 -> stepThread), then a stop\n");
    printf("    at an arbitrary moment would have covered only a thin slab near the\n");
    printf("    top of every region. vdC makes the front JUMP across depth so that\n");
    printf("    at ANY stopping moment the punctures lie as evenly as possible.\n\n");

    printf("7B. THE SEQUENCE\n");
    printf("    vdc(n) is the van der Corput sequence base 2: take n and reflect\n");
    printf("    its bits about the radix point.\n");
    printf("      0->0  1->.5  2->.25  3->.75  4->.125  5->.625  6->.375  7->.875\n");
    printf("    As percentages the front visits: 0%% -> 50%% -> 25%% -> 75%% ->\n");
    printf("    12.5%% -> 62.5%% -> 37.5%% -> 87.5%% -> ...\n");
    printf("    Property: each next term bisects the largest untouched gap. After\n");
    printf("    2^k points the visited set is a perfectly even grid of step 1/2^k\n");
    printf("    (max gap = min gap). It is the minimal-discrepancy 1-D sequence:\n");
    printf("    maximally uniform sampling at ANY stopping point, densifying\n");
    printf("    forever.\n\n");

    printf("7C. HOW A PHASE BECOMES FRONT MOVEMENT\n");
    printf("    The launcher computes phase = vdc(n) (a fraction in [0,1)), turns\n");
    printf("    it into a percent pct = phase*100 and sends it to the engine. The\n");
    printf("    engine sets:\n");
    printf("        liftFrom   = pct\n");
    printf("        batchCount = (pct/100) * totalBatches  ->  offset=phase*stepThread\n");
    printf("    and teleports the WHOLE comb (~1.5M teeth) to the new depth at once,\n");
    printf("    across the entire chunk. Not \"one tooth went further\" -- the whole\n");
    printf("    comb lands on a new thin line.\n\n");

    printf("7D. INFINITY AND THE COUNTER n\n");
    printf("    vdC is infinite. The launcher keeps a counter n; on each jump it\n");
    printf("    computes vdc(n) then does n += 1. The phase walk runs forever,\n");
    printf("    densifying the grid. n alone defines \"where we are\" in the\n");
    printf("    sequence -- and so it is the single resume point (see section 9).\n");
    printf("    The order of visiting does NOT change the probability of a hit:\n");
    printf("    P(find in N checked keys) = N / M, independent of order. vdC does\n");
    printf("    not buy more chance -- it spreads a limited budget more evenly.\n\n");

    printf("=======================================================================\n");
    printf("  LAUNCHER <-> ENGINE PROTOCOL\n");
    printf("=======================================================================\n\n");

    printf("8. AUTOCMD TELEPORT CHANNEL\n");
    printf("   Launcher and engine are SEPARATE processes. The launcher starts the\n");
    printf("   engine once (with --auto) and then steers it through a command file\n");
    printf("   instead of the console -- no restart per jump.\n");
    printf("   - File: autocmd_gpuN.txt, in the chunk folder. On each jump the\n");
    printf("     launcher writes it ATOMICALLY (.tmp + replace) as three numbers:\n");
    printf("        <seq> <pct> <stop>\n");
    printf("     seq = command counter (so the engine spots a NEW command),\n");
    printf("     pct = the vdC phase in percent, stop = stop flag.\n");
    printf("   - The engine, started with --auto, polls this file ~3x/sec. When\n");
    printf("     seq changes it clamps pct to [0, 99.999], sets liftFrom = pct and\n");
    printf("     batchCount = (pct/100)*totalBatches, breaks the inner loop, and\n");
    printf("     the outer loop re-keys the GPU = the teleport itself.\n");
    printf("   The engine does NOT know the word \"vdC\": it just gets \"teleport to\n");
    printf("   X%%\". Only the launcher knows X came from van der Corput. Clean\n");
    printf("   split: launcher = strategy, engine = execution.\n\n");

    printf("=======================================================================\n");
    printf("  DEEP SLOT ENGINE\n");
    printf("=======================================================================\n\n");

    printf("9. DEEP SLOT SPLIT (-D)\n");
    printf("   Without -FULL the chunk (a range of 2nd-symbol slots) is split by\n");
    printf("   the Nth hex symbol into equal slots:\n");
    printf("     slot count = (#selected slots) * 16^(N-2)\n");
    printf("     OFF/-D3 -> x16   -D4 -> x256   -D5 -> x4096   -D6 -> x65536\n");
    printf("   Example: 2 slots selected + OFF -> 32 slots.\n");
    printf("   - Random slot order: a full-period LCG permutation -- every slot\n");
    printf("     exactly once per pass, pseudo-random.\n");
    printf("   - Time jumps (-T): every T seconds the slot position is saved and\n");
    printf("     the engine hops to the next slot. -T 999999999 reads each slot to\n");
    printf("     the end before moving on.\n");
    printf("   - Resume & honest completion (-b): each slot's progress is stored,\n");
    printf("     a resumed run continues where it stopped, and the search ends\n");
    printf("     only when every slot has been fully read.\n");
    printf("   Progress folder (Deep): d{N}_{puz}_0x{lo8}-0x{hi8}_W{grid}_rnd/\n");
    printf("   Progress folder (FULL): dFULL_{puz}_0x{lo8}-0x{hi8}_W{grid}/\n");
    printf("   Different flags = different folders = no conflicts.\n\n");

    printf("=======================================================================\n");
    printf("  RESUME, FILES, CONSOLE & MATH\n");
    printf("=======================================================================\n\n");

    printf("10. SAVE & RESUME\n");
    printf("    FULL mode -- resume is BY PHASE. The whole comb has one shared\n");
    printf("    position, so \"where we stopped\" is just the vdC counter n. On START\n");
    printf("    the launcher reads n and continues with vdc(n), vdc(n+1), ...;\n");
    printf("    visited phases are not repeated. There is no per-tooth memory; the\n");
    printf("    fine position batchCount is written only with -b. Phases are thin\n");
    printf("    slices, so re-digging a phase from its top loses a negligible\n");
    printf("    amount. Deep mode -- resume is per slot (slotPos[]) via -b.\n");
    printf("    File map:\n");
    printf("      vdc_gpuN.json     (chunk folder, launcher) : n + full settings\n");
    printf("                          snapshot; on each jump only n is updated\n");
    printf("      autocmd_gpuN.txt  (chunk folder, launcher) : <seq> <pct> <stop>\n");
    printf("      deep_*.json       (chunk folder, engine, -b only) : batch_count,\n");
    printf("                          lift_from, chunk bounds, per-slot state\n");
    printf("      coverage_puzzleN.json (next to exe, launcher) : manual coverage\n\n");

    printf("11. CONSOLE LINES EXPLAINED\n");
    printf("    Setting starting keys... [100.00%%]\n");
    printf("        GPU keys are being (re)set; flickers on EVERY teleport -- it is\n");
    printf("        the signal that a jump just happened.\n");
    printf("    [Lift] ... now X%% (abs) (start Y%%)\n");
    printf("        X%% = current absolute depth; start Y%% = liftFrom = the vdC\n");
    printf("        phase this dig started from.\n");
    printf("    [Front] 70:NN%%\n");
    printf("        slot (white) : depth in %% (green) per slot of the chunk; a\n");
    printf("        found key's slot is shown in red.\n");
    printf("    band depth [...] ETA ... Gk/s\n");
    printf("        progress inside the current phase, an estimate, and speed.\n\n");

    printf("12. PROGRAM MATH (summary)\n");
    printf("    numThreads   = 6144 * 256 = 1,572,864            (teeth, grid 5)\n");
    printf("    STEP_SIZE    = 1024                              (keys/batch/tooth)\n");
    printf("    stepThread   = (ksFinish - ksStart + 1) / numThreads\n");
    printf("    totalBatches = stepThread / STEP_SIZE\n");
    printf("    phase        = batchCount / totalBatches          (0..1)\n");
    printf("    key          = ksStart + thId*stepThread + batch*STEP_SIZE + j\n");
    printf("    For a uniform random target, P(find in N keys) = N / M depends\n");
    printf("    only on N, never on order. vdC minimises 1-D discrepancy (depth),\n");
    printf("    so any stop leaves the punctures as even as possible -- not more\n");
    printf("    chance, just an honest spread of a limited budget. Throughput over\n");
    printf("    time T is keys = speed * T regardless of jump length; jump length\n");
    printf("    changes only the number of phases (uniformity) and the cost of\n");
    printf("    interruption, never the total N or the probability.\n\n");

    printf("13. WHAT TO EXPECT (scale)\n");
    printf("    A full chunk of puzzle 71 is ~2^70 ~ 1.2e21 keys. At ~1.4 Gk/s\n");
    printf("    that is astronomical time, so a full traversal does not happen.\n");
    printf("    Any real session covers a vanishingly small fraction -- the metric\n");
    printf("    is not \"is it enough\" but \"how evenly is it spread\". Real odds rise\n");
    printf("    only with more speed (a stronger GPU) or a smaller range.\n\n");

    printf("14. OPTIMIZATION / PERFORMANCE\n");
    printf("    - SINGLE_TARGET_MODE: the single target hash is kept in GPU\n");
    printf("      constant memory and compared inline (no bloom/list lookups).\n");
    printf("    - Batched group EC: points are advanced in groups with ONE modular\n");
    printf("      inversion per group (batch/Montgomery inverse) instead of one\n");
    printf("      per key -- the dominant cost is amortised across the group.\n");
    printf("    - In-place SHA256 and force-inlined RIPEMD160 on the GPU.\n");
    printf("    - Endomorphism / curve symmetry from the VanitySearch core.\n");
    printf("    - Coalesced memory access and a tunable thread grid (-W).\n");
    printf("    - CPU-side hashing uses SSE; the build enables SSE/ADX/BMI.\n");
    printf("    Measured: ~1.47 Gk/s on an RTX A4000 (sm_86), CUDA 13.1.\n\n");

    printf("15. BUILDING\n");
    printf("    Windows: a ready compile_modern.bat is in the repository -- just\n");
    printf("             run it (needs CUDA Toolkit + Visual Studio build tools).\n");
    printf("             It produces EKey-Jerboa.exe.\n");
    printf("    Linux:   a Makefile is provided. From the project root run:\n");
    printf("               make            (or: make -j$(nproc))\n");
    printf("             Needs the CUDA Toolkit (nvcc). Override if needed:\n");
    printf("               make GENCODE=\"-gencode arch=compute_86,code=sm_86\"\n");
    printf("               make CUDA_PATH=/usr/local/cuda-13.1\n");
    printf("             The sources are cross-platform -- nothing to edit.\n\n");

    printf("16. GPU ARCHITECTURES\n");
    printf("    The sources/build target current NVIDIA architectures out of the\n");
    printf("    box: sm_75 (RTX 20xx), sm_86 (RTX 30xx / A4000), sm_89 (RTX 40xx)\n");
    printf("    plus compute_89 PTX so newer cards (RTX 50xx+) run via JIT.\n");
    printf("    Edit GENCODE / -arch to match your card exactly.\n\n");

    printf("=======================================================================\n");
    printf("  THE GUI LAUNCHER (panels)\n");
    printf("=======================================================================\n\n");

    printf("17. LAUNCHER OVERVIEW\n");
    printf("    The launcher is the \"control panel\": you set the chunk and the\n");
    printf("    parameters, it builds the command line and starts EKey-Jerboa in a\n");
    printf("    separate console. All GPU work is done by the compiled engine.\n\n");

    printf("18. TOP RULER (CHUNK RANGE)\n");
    printf("    - Sliders / Start%%-End%% / 0x... fields: pick the chunk (slots =\n");
    printf("      the leading 2 hex symbols).\n");
    printf("    - \"Lock\" + arrows < > : fix the chunk width and shift it by its own\n");
    printf("      width, to walk consecutive pieces.\n\n");

    printf("19. COVERAGE MAP (3 buttons under the ruler)\n");
    printf("    A semi-manual mark of \"this piece is done\", one file per puzzle\n");
    printf("    (coverage_puzzleN.json next to the exe), independent per puzzle.\n");
    printf("    - Mark selected   : paints the chosen chunk green (accumulates).\n");
    printf("    - Remove selected : clears only the chosen chunk's mark.\n");
    printf("    - Clear all       : wipes the whole coverage map of the puzzle.\n\n");

    printf("20. vdC DENSITY SCALE (lower) & TELEPORT CONTROLS\n");
    printf("    - The density scale visualises the accumulated teleport punctures:\n");
    printf("      the longer it runs, the denser and more even the grid. On the\n");
    printf("      right: the counter n and the current grid step.\n");
    printf("    - \"Reset vdC\" zeroes n (the phase walk restarts from scratch).\n");
    printf("    - \"vdC teleport\" checkbox enables the infinite teleport mode.\n");
    printf("    - \"jump\" + sec/min/hour selector: how long to dig on a phase\n");
    printf("      before jumping (cap 100 h).\n");
    printf("    - Big countdown: time to the next jump (H:MM:SS at >=1 h, else M:SS).\n\n");

    printf("21. EXE / PUZZLE / GRID / GPU and START / STOP\n");
    printf("    - EXE: path to the engine. PUZZLE: the task (fills range+address).\n");
    printf("      GRID: the -W grid. GPU: the device id.\n");
    printf("    - START turns green while running; STOP flashes red ~1 s. On START\n");
    printf("      the launcher writes the settings snapshot into vdc_gpuN.json.\n");
    printf("    - \"RUN progress\": load a working chunk -- pick vdc_gpuN.json (n +\n");
    printf("      all settings) or any file in the chunk folder; puzzle, chunk,\n");
    printf("      GPU, jump interval+unit, checkboxes, slots and n are restored.\n\n");

    printf("22. INFO WINDOWS & TIME CALCULATOR\n");
    printf("    - \"i INF\" / \"? FAQ\": in-app help windows (no exe launch).\n");
    printf("    - Time calculator: T_sec = keys*(1000 if TKey else 1)/speed_GKeys;\n");
    printf("      T_min = T_sec/60; T_hours = T_sec/3600; keys_per_slot = keys/N.\n");
    printf("      Inputs: amount (GKey/TKey) + speed (GKey/s, default 1.4). The\n");
    printf("      \"split into N slots\" line is a paper estimate of work-per-piece;\n");
    printf("      in FULL mode the comb covers all slots at once, so the real\n");
    printf("      \"pass per tooth\" over time T is speed*T/numThreads keys/tooth.\n\n");

    printf("23. KEYBOARD\n");
    printf("    P = Pause/Resume     Ctrl+C = stop (saves first when -b is on)\n\n");

    printf("24. CONTACT\n");
    printf("    Questions / feedback: egor.gr1@gmail.com\n\n");

    printf("=======================================================================\n\n");
    exit(0);
}

static void printInfo() {
    printf("\n");
    printf("=======================================================================\n");
    printf("  EKey-Jerboa V3.0.0 -- Version Info & Credits\n");
    printf("=======================================================================\n\n");
    printf("  WHAT IT IS\n");
    printf("  EKey-Jerboa is a GPU-accelerated brute-force search tool for the\n");
    printf("  open Bitcoin \"puzzle\" challenges. Given a target P2PKH address (or\n");
    printf("  compressed public key) and a key range (a \"chunk\"), it sweeps that\n");
    printf("  range on the GPU looking for the matching private key, and saves\n");
    printf("  progress so a search can be paused and resumed.\n\n");
    printf("  PRINCIPLE\n");
    printf("  In its main FULL mode the whole chunk is covered by one \"comb\" of\n");
    printf("  1,572,864 thread-teeth spread evenly across the range; the high hex\n");
    printf("  symbols are scanned in parallel across the teeth, the low symbols by\n");
    printf("  a sequential counter inside each tooth. On top of it runs an\n");
    printf("  infinite van der Corput (vdC) teleport that moves the common front\n");
    printf("  across depth with minimal discrepancy, so a stop at ANY moment\n");
    printf("  leaves the sample as even as possible. Order does not change the\n");
    printf("  probability of a hit -- vdC buys evenness, not extra chance. A\n");
    printf("  classic Deep-slot engine (-D3..-D6, random LCG order, timer jumps)\n");
    printf("  is also available. Run -faq for the full manual.\n\n");
    printf("  ENGINE\n");
    printf("  Cryptography is secp256k1 (the same curve as Bitcoin); all heavy\n");
    printf("  work runs on the GPU via CUDA. A fork of VanitySearch.\n\n");
    printf("  AUTHORSHIP\n");
    printf("  Programmed by Claude (Anthropic) -- model Claude Opus 4.8 --\n");
    printf("  under the meticulous guidance and direction of egorrushka, who\n");
    printf("  designed the engine behaviour, drove the architecture and tested\n");
    printf("  every single build.\n\n");
    printf("  BASED ON\n");
    printf("  The respected sources of VanitySearch by Jean Luc PONS (GPLv3).\n");
    printf("  https://github.com/JeanLucPons/VanitySearch\n");
    printf("  Fork author: egorrushka. Author additions: the FULL synchronous\n");
    printf("  comb, the infinite vdC teleport, the file-command control protocol,\n");
    printf("  and the whole GUI launcher.\n\n");
    printf("  MADE IN\n");
    printf("  Written in Ukraine, under the extreme conditions of war, in the\n");
    printf("  glorious city of Chernihiv.\n\n");
    printf("  DISCLAIMER\n");
    printf("  Intended for the lawful open Bitcoin Puzzle tasks and for research.\n");
    printf("  Use responsibly.\n\n");
    printf("  CONTACT\n");
    printf("  Feedback and questions: egor.gr1@gmail.com\n\n");
    printf("  License : GPLv3 (fork of VanitySearch; original (c) Jean Luc PONS)\n");
    printf("  GPU     : CUDA C++ (NVIDIA), sm_75/sm_86/sm_89 + PTX fallback\n");
    printf("  Tested  : RTX A4000 (sm_86), CUDA 13.1 -- ~1.47 Gk/s\n\n");
    printf("=======================================================================\n\n");
    exit(0);
}

static void printHelp() {
    printf("\nEKey-Jerboa V3.0.0  by egorrushka\n");
    printf("Based on VanitySearch by Jean Luc PONS\n\n");
    printf("Usage: EKey-Jerboa.exe -a <address> -s 0xSTART -e 0xEND [options]\n\n");
    printf("Required:\n");
    printf("  -a <addr>   Target Bitcoin P2PKH address\n");
    printf("  -p <pubkey> Target public key (compressed hex, alternative to -a)\n");
    printf("  -s <hex>    Chunk start (hex, e.g. 0x400000000000000000)\n");
    printf("  -e <hex>    Chunk end   (hex, e.g. 0x7fffffffffffffffff)\n");
    printf("  -r <bits>   Bit range (alternative to -s/-e)\n\n");
    printf("Options:\n");
    printf("  -T <sec>    Jump interval in seconds (default 30)\n");
    printf("  -W <0-7>    Grid profile  0=auto 5=6144x256 (A4000 default)\n");
    printf("  -G <id>     GPU device ID (default 0)\n");
    printf("  -b          Resume mode\n");
    printf("  -D4..-D6    Split deeper (OFF/default = -D3)\n");
    printf("  -FULL       Single comb over the WHOLE chunk (no slots/jumps);\n");
    printf("              one clean front, single-number resume. Implies no -T.\n");
    printf("  -L <pct>    Lift: dig the comb downward from <pct>%% depth, auto-stop\n");
    printf("              at the next already-dug band. Implies -FULL. Use -b to\n");
    printf("              keep the per-chunk 'dug bands' journal.\n");
    printf("  -faq        Full manual\n");
    printf("  -inf        Version and credits\n");
    printf("  -h          This help\n\n");
    printf("Grid profiles (-W):\n");
    printf("  0:auto  1:512  2:1024  3:2048  4:4096  5:6144  6:8192  7:12288\n\n");
    exit(0);
}

static const int GRIDS[] = {-1,512,1024,2048,4096,6144,8192,12288};
static const int NGRID   = 8;

static bool parseHex(const string& raw, Int& out) {
    string s = raw;
    if (s.size()>=2&&s[0]=='0'&&(s[1]=='x'||s[1]=='X')) s=s.substr(2);
    if (s.empty()||s.size()>64) return false;
    for (char c:s) if (!isxdigit((unsigned char)c)) return false;
    while (s.size()<64) s.insert(s.begin(),'0');
    vector<char> b(s.begin(),s.end()); b.push_back('\0');
    out.SetBase16(b.data()); return true;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    thread inputThread(monitorKeypress);
    Timer::Init();
    setvbuf(stdout, NULL, _IONBF, 0); // disable CRT buffering: Win32 console API must stay in sync with printf
    Secp256K1* secp = new Secp256K1(); secp->Init();

    if (argc < 2) printHelp();

    string taddr, tpubkey, hexStart, hexEnd;
    int gpuId=0, bits=0, gridProfile=0;
    double jumpSec = 30.0;
    int  deepMode = 0;
    bool fullRange = false;   // -FULL : single comb over whole chunk
    bool   liftMode  = false; // -L : lift (dig from a depth floor, stop at next dug band)
    double liftFloor = 0.0;   // start floor %
    string launcherJson;  // launcher JSON (optional, from GUI)

    for (int i=1; i<argc; i++) {
        string arg = argv[i];
        if      (arg=="-h"||arg=="--help") printHelp();
        else if (arg=="-b") backupMode=true;
        else if (arg=="--auto") autoMode=true;
        else if (arg=="-FULL"||arg=="-F") fullRange=true;
        else if (arg=="-L"&&i+1<argc) { liftMode=true; liftFloor=atof(argv[++i]); }
        else if (arg=="-a"&&i+1<argc) taddr    = argv[++i];
        else if (arg=="-p"&&i+1<argc) tpubkey  = argv[++i];
        else if (arg=="-s"&&i+1<argc) hexStart  = argv[++i];
        else if (arg=="-e"&&i+1<argc) hexEnd    = argv[++i];
        else if (arg=="-r"&&i+1<argc) bits = atoi(argv[++i]);
        else if (arg=="-T"&&i+1<argc) jumpSec = atof(argv[++i]);
        else if (arg=="-R") { /* order is always Random now — accepted, no-op */ }
        else if (arg.size()==3 && arg[0]=='-' &&
                 (arg[1]=='D'||arg[1]=='d') &&
                 arg[2]>='3' && arg[2]<='6')
            deepMode = arg[2] - '0';
        else if (arg.size()==3 && arg[0]=='-' &&
                 (arg[1]=='D'||arg[1]=='d') &&
                 arg[2]>='7' && arg[2]<='8') {
            fprintf(stderr,"[ERROR] -D: max level is 6 (per-slot resume cap ~4.2M slots)\n");
            exit(-1);
        }
        else if (arg=="-faq") printFaq();
        else if (arg=="-inf") printInfo();
        else if (arg=="-J"&&i+1<argc) jumpSec = atof(argv[++i])*60.0; // minutes compat
        else if (arg=="-G"&&i+1<argc) gpuId = atoi(argv[++i]);
        else if (arg=="-W"&&i+1<argc) {
            gridProfile = atoi(argv[++i]);
            if (gridProfile<0||gridProfile>=NGRID){fprintf(stderr,"[ERROR] -W: 0-%d\n",NGRID-1);exit(-1);}
        }
        else if (arg=="--launcher-file"&&i+1<argc) {
            // ?????? JSON ?? ?????????? ????? (??????? ???????? ????????????? ? cmd.exe)
            std::string lfpath = argv[++i];
            FILE* lf = fopen(lfpath.c_str(), "r");
            if (lf) {
                fseek(lf, 0, SEEK_END); long sz = ftell(lf); rewind(lf);
                if (sz > 0 && sz < 4096) {
                    launcherJson.resize(sz);
                    fread(&launcherJson[0], 1, sz, lf);
                    // trim trailing whitespace/newlines
                    while (!launcherJson.empty() &&
                           (launcherJson.back()=='\n'||launcherJson.back()=='\r'||launcherJson.back()==' '))
                        launcherJson.pop_back();
                }
                fclose(lf);
            }
        }
        else { fprintf(stderr,"[ERROR] Unknown: %s\n",arg.c_str()); printHelp(); }
    }

    if (taddr.empty()&&tpubkey.empty()){fprintf(stderr,"[ERROR] Need -a or -p\n");printHelp();}
    bool useChunk = (!hexStart.empty()||!hexEnd.empty());
    if (useChunk&&(hexStart.empty()||hexEnd.empty())){fprintf(stderr,"[ERROR] Need both -s and -e\n");exit(-1);}
    if (!useChunk&&bits==0){fprintf(stderr,"[ERROR] Need -s/-e or -r\n");printHelp();}
    if (jumpSec<1.0) jumpSec=1.0;
    if (deepMode==0) deepMode=3;   // default: split by 2nd symbol (16 slots/top) = -D3
    if (liftMode) fullRange = true;             // -L implies FULL
    if (liftFloor < 0.0)    liftFloor = 0.0;
    if (liftFloor > 99.999) liftFloor = 99.999;
    if (fullRange) jumpSec = 999999999.0;  // -FULL: single comb, no slot jumps

    // Enable ANSI + set fixed console window size (Windows)
#if defined(_WIN32)||defined(_WIN64)
    {
        HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD m=0;
        if(GetConsoleMode(h,&m))
            SetConsoleMode(h,m|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        // Fixed window: 190 cols x 40 rows  (wide enough for 8-sector table)
        COORD buf={190,2000};
        SetConsoleScreenBufferSize(h,buf);
        SMALL_RECT win={0,0,189,39};
        SetConsoleWindowInfo(h,TRUE,&win);
        // Set window title
        SetConsoleTitleA("EKey-Jerboa V3.0.0  by egorrushka");
    }
#endif

    // CUDA check
    int devCount=0;
    cudaGetDeviceCount(&devCount);
    if (devCount==0){fprintf(stderr,"[ERROR] No CUDA GPU found\n");exit(-1);}
    if (gpuId>=devCount||gpuId<0){fprintf(stderr,"[ERROR] Invalid GPU id %d\n",gpuId);exit(-1);}

    // Build params
    BITCRACK_PARAM bc={};
    if (useChunk) {
        if (!parseHex(hexStart,bc.ksStart)||!parseHex(hexEnd,bc.ksFinish)){fprintf(stderr,"[ERROR] Bad hex\n");exit(-1);}
        if (bc.ksFinish.IsLower(&bc.ksStart)){fprintf(stderr,"[ERROR] end < start\n");exit(-1);}
    } else {
        bc.ksStart.SetInt32(1); if(bits>1) bc.ksStart.ShiftL(bits-1);
        bc.ksFinish.SetInt32(1); bc.ksFinish.ShiftL(bits); bc.ksFinish.SubOne();
    }
    bc.ksNext.Set(&bc.ksStart);
    // Order is always Random (LCG full-period). Sequential mode removed.
    bc.randSlotMode  = true;
    bc.jerboaJumpSec = jumpSec;
    // Legacy COMB fields unused but zeroed
    bc.combMode=false; bc.combSequential=false; bc.combSlotsCount=0;
    bc.combDone=false; bc.combJumpMinutes=0; bc.combInterleaveStep=0;
    bc.combCycleNum=0; bc.combCycleTotal=0; bc.combCurrentPass=0;
    bc.combCoverage.SetInt32(0); bc.combBaseOffset.SetInt32(0);
    bc.deepMode      = deepMode;
    bc.gridProfile   = gridProfile;
    bc.fullRangeMode = fullRange;
    bc.liftMode      = liftMode;
    bc.liftFloor     = liftFloor;
    // Store launcher JSON string (from --launcher "...") for satellite file
    memset(bc.launcherJson, 0, sizeof(bc.launcherJson));
    if (!launcherJson.empty())
        strncpy(bc.launcherJson, launcherJson.c_str(), sizeof(bc.launcherJson)-1);

    // Print header
    printf("\n[+] EKey-Jerboa V3.0.0  by egorrushka\n");
    if (!tpubkey.empty())
        printf("[+] Search : %s [Public Key]\n", tpubkey.c_str());
    else
        printf("[+] Search : %s [P2PKH/Compressed]\n", taddr.c_str());
    time_t now=time(NULL);
    char tbuf[64]; ctime_s(tbuf,sizeof(tbuf),&now);
    printf("[+] Start  : %s", tbuf);
    {
        string cs = bc.ksStart.GetBase16();
        string ce = bc.ksFinish.GetBase16();
        // trim leading zeros
        size_t n=cs.find_first_not_of('0'); if(n!=string::npos)cs=cs.substr(n);else cs="0";
        n=ce.find_first_not_of('0'); if(n!=string::npos)ce=ce.substr(n);else ce="0";
        printf("[+] Chunk  : 0x%s -> 0x%s\n", cs.c_str(), ce.c_str());
    }
    // Jump interval display: show "No Jump" when T=999999999
    if (jumpSec >= 999999998.0)
        printf("[+] Jump   : No Jump (sequential scan)\n");
    else
        printf("[+] Jump   : %.1f sec/slot\n", jumpSec);
    if (fullRange)
        printf("[+] Mode   : FULL  (single comb over whole chunk, no jumps)\n");
    else
        printf("[+] Mode   : Deep D%d  (Random)\n", deepMode);
    if (liftMode)
        printf("[+] Lift   : start floor %.2f%%  (continuous forward dig)\n", liftFloor);
    fflush(stdout);

    vector<string> targets;
    if (!tpubkey.empty()) targets.push_back(tpubkey);
    else targets.push_back(taddr);

    uint32_t maxFound = 65536*4;
    VanitySearch* v = new VanitySearch(secp, targets, SEARCH_COMPRESSED, true, "", maxFound, &bc);
    g_vs = v;

    int gx = (gridProfile>0&&gridProfile<NGRID) ? GRIDS[gridProfile] : 6144;
    vector<int> gpuIds={gpuId};
    vector<int> gridSizes={gx,256};
    if (gx>0)
        printf("[+] Grid   : %d x 256 = %d threads\n", gx, gx*256);
    fflush(stdout);

    v->Search(gpuIds, gridSizes);

    stopMonitorKey=true;
    if (inputThread.joinable()) inputThread.join();
    printf("\n");
    delete v; delete secp;
    return 0;
}
