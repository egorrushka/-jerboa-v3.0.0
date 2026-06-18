// Vanity.h

/*
 * This file is part of the VanitySearch distribution (https://github.com/JeanLucPons/VanitySearch).
 * Copyright (c) 2019 Jean Luc PONS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef VANITYH
#define VANITYH

#include <string>
#include <vector>
#include "SECP256k1.h" 
#include "GPU/GPUEngine.h"
#include <atomic>
#ifdef WIN64
#include <Windows.h>
#endif

#define P2PKH  0
#define P2SH   1
#define BECH32 2
#define PUBKEY 3

extern std::atomic<bool> Pause;
extern std::atomic<bool> Paused;
extern int idxcount;
extern double t_Paused;
extern bool backupMode;
extern bool autoMode;   // --auto : autopilot persistent process (timer floor jumps)
extern std::atomic<bool> g_shutdown_initiated; 

class VanitySearch;

#ifdef WIN64
#define LOCK(mutex) WaitForSingleObject(mutex,INFINITE);
#define UNLOCK(mutex) ReleaseMutex(mutex);
#else
#include <pthread.h>
#define LOCK(mutex)  pthread_mutex_lock(&(mutex));
#define UNLOCK(mutex) pthread_mutex_unlock(&(mutex));
#endif

typedef struct {
	VanitySearch* obj;
	int  threadId;
	bool isRunning;
	bool hasStarted;
	int  gridSizeX;
	int  gridSizeY;
	int  gpuId;
	Int  THnextKey;
} TH_PARAM;

typedef struct {
	char* address;
	int addressLength;
	address_t sAddress;	
	bool* found;
	bool isFull;
	addressl_t lAddress;
	uint8_t hash160[20];
} ADDRESS_ITEM;

typedef struct {
	std::vector<ADDRESS_ITEM>* items;
	bool found;
} ADDRESS_TABLE_ITEM;

#define COMB_SLOTS_MAX 2000  // maximum slots

typedef struct {
	Int      ksStart;
	Int      ksNext;
	Int      ksFinish;
	// ── COMB mode ─────────────────────────────────────────────────────────
	bool     combMode;           // -C or -S
	bool     combSequential;     // -S: sweep order 0,1,...,N-1
	int      combSlotsCount;     // -N: slots count (50/100/250/500/1000)
	Int      combCoverage;       // chunk_size / combSlotsCount
	Int      combSlots[2048];    // slot offsets from chunk start
	int      combSlotOrder[2048]; // original index 0..N-1 for each shuffled slot
	int      combCurrentPass;    // current slot index
	bool     combDone;           // all slots visited this cycle
	double   combJumpMinutes;    // -J: minutes per slot (0=auto)
	// ── Interleave ────────────────────────────────────────────────────────
	double   combInterleaveStep; // -I: base shift per cycle in %
	int      combCycleNum;       // current cycle
	int      combCycleTotal;     // ceil(100/step)
	Int      combBaseOffset;     // chunkStart + step*cycleNum
    // EKey-Jerboa V3.0.0 by egorrushka
    bool   randSlotMode;     // true = Random (LCG) slot order; false = Sequential
    double jerboaJumpSec;    // jump interval in seconds (999999999 = no-jump/sequential)
    // Launcher config JSON string (passed via --launcher "..." from GUI launcher)
    // Stored as-is into the .launcher satellite file at job start.
    char   launcherJson[2048];
    int    deepMode;      // 3..5 = Deep subslot mode (-D3..-D5); split by Nth hex symbol
    int    gridProfile;   // -W grid profile (for progress folder naming)
    // EKey-Jerboa V3.0.0 by egorrushka
    // -FULL : single-comb mode. The whole chunk is treated as ONE slot; the GPU
    // threads form a single round-robin comb over the entire range, advancing a
    // single clean front (batchCount). No slot splitting, no jumps, no per-slot
    // resume array — resume is a single number. Honest completion = full chunk
    // covered, followed by a CPU tail sweep of the floor-division remainder.
    bool   fullRangeMode;
    // EKey-Jerboa V3.0.0 by egorrushka
    // -L <floor%> : "lift". Dig the comb DOWNWARD starting from floor% depth,
    // auto-stop at the next already-dug band. Implies FULL. Per-chunk journal
    // of dug depth bands lets the launcher warn on re-digging.
    bool   liftMode;
    double liftFloor;     // start floor in percent [0..100)
} BITCRACK_PARAM;

class VanitySearch {
public:
	VanitySearch(Secp256K1* secp, std::vector<std::string>& address, int searchMode,
		bool stop, std::string outputFile, uint32_t maxFound, BITCRACK_PARAM* bc);

	void Search(std::vector<int> gpuId, std::vector<int> gridSize);
	void FindKeyGPU(TH_PARAM* p);
    void findKeyGPU_Deep  (TH_PARAM* p);  // Deep subslot mode -D3..-D5 (only engine)

    
    std::atomic<bool> endOfSearch;

private:
    
    uint64_t targetPubKeyX[4];
    int targetPubKeyParity; // 0 for even, 1 for odd

    
    std::string GetHex(std::vector<unsigned char>& buffer);
    std::string GetExpectedTimeBitCrack(double keyRate, double keyCount, BITCRACK_PARAM* bc);
    bool checkPrivKey(std::string addr, Int& key, int32_t incr, int endomorphism, bool mode);
    void checkAddr(int prefIdx, uint8_t* hash160, Int& key, int32_t incr, int endomorphism, bool mode);
    void checkAddrSSE(uint8_t* h1, uint8_t* h2, uint8_t* h3, uint8_t* h4,
        int32_t incr1, int32_t incr2, int32_t incr3, int32_t incr4,
        Int& key, int endomorphism, bool mode);
    void checkAddresses(bool compressed, Int key, int i, Point p1);
    void checkAddressesSSE(bool compressed, Int key, int i, Point p1, Point p2, Point p3, Point p4);
    void output(std::string addr, std::string pAddr, std::string pAddrHex);
    bool isAlive(TH_PARAM* p);
    bool isSingularAddress(std::string pref);
    bool hasStarted(TH_PARAM* p);
    uint64_t getGPUCount();
    bool initAddress(std::string& address, ADDRESS_ITEM* it);
    void updateFound();
    void getGPUStartingKeys(Int& tRangeStart, Int& tRangeEnd, int groupSize, int numThreadsGPU, Point* publicKeys, uint64_t Progress);
    void enumCaseUnsentiveAddress(std::string s, std::vector<std::string>& list);
    
    void PrintStats(uint64_t keys_n, double ttot, const Int& total_keyspace);
    std::string format_time_long(double seconds);
    void saveBackup(int idxcount, double t_Paused, int gpuid);

#ifdef WIN64
	HANDLE mutex;
	HANDLE ghMutex;	
#else
	pthread_mutex_t  mutex;
	pthread_mutex_t  ghMutex;	
#endif	

    
	Secp256K1* secp;
	Int startKey;		
	uint64_t      counters[256];	
	double startTime;
	int searchType;
	int searchMode;
	bool stopWhenFound;
	
	int numGPUs;
	int nbFoundKey;
	uint32_t nbAddress;
	std::string outputFile;
	bool useSSE;
	bool onlyFull;
	uint32_t maxFound;	
	std::vector<ADDRESS_TABLE_ITEM> addresses;
	std::vector<address_t> usedAddress;
	std::vector<LADDRESS> usedAddressL;
	std::vector<std::string>& inputAddresses;	
	BITCRACK_PARAM* bc;
	void saveProgress(TH_PARAM* p, Int& lastSaveKey, BITCRACK_PARAM* bc);
	Int firstGPUThreadLastPrivateKey;
	Int beta;
	Int lambda;
	Int beta2;
	Int lambda2;
};

#endif // VANITYH
