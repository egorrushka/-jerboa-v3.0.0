//  GPU/GPUEngine.cu

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
#ifndef WIN64
#include <unistd.h>
#include <stdio.h>
#endif

#include "GPUEngine.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <stdint.h>
#include "../hash/sha256.h"
#include "../hash/ripemd160.h"
#include "../Timer.h"
#include "../Vanity.h"

#include "GPUGroup.h"
#include "GPUMath.h"
#include "GPUHash.h"
#include "GPUBase58.h"
#include "GPUWildcard.h"
#include "GPUCompute.h"
#include <iostream>

#include <omp.h>

int _ConvertSMVer2Cores(int major, int minor) {

    // Defines for GPU Architecture types (using the SM version to determine
    // the # of cores per SM
    typedef struct {
        int SM;  // 0xMm (hexidecimal notation), M = SM Major version,
        // and m = SM minor version
        int Cores;
    } sSMtoCores;

    sSMtoCores nGpuArchCoresPerSM[] = {
        {0x60,  64},
        {0x61, 128},
        {0x62, 128},
        {0x70,  64},
        {0x72,  64},
        {0x75,  64},
        {0x80,  64},
        {0x86,  128},
        {0x89,  128},
        {-1, -1} };

    int index = 0;

    while (nGpuArchCoresPerSM[index].SM != -1) {
        if (nGpuArchCoresPerSM[index].SM == ((major << 4) + minor)) {
            return nGpuArchCoresPerSM[index].Cores;
        }

        index++;
    }

    return 0;

}

#define GRP_SIZE 1024
#define STEP_SIZE GRP_SIZE*1

__global__ void comp_keys(address_t* sAddress, uint32_t* lookup32, uint64_t* keys, uint32_t* out) {

    uint64_t* startx = keys + (blockIdx.x * blockDim.x) * 8;
    uint64_t* starty = keys + (blockIdx.x * blockDim.x) * 8 + 4 * blockDim.x;

    uint64_t dx[4];  
    uint64_t px[4];
    uint64_t py[4];
    uint64_t dy[4];
    uint64_t sxn[4];
    uint64_t syn[4];
    uint64_t sx[4];
    uint64_t sy[4];
    uint64_t sx_gx[4];
    uint8_t odd_py;
    uint32_t h[5];
    uint64_t inverse[5];

    uint64_t subp[GRP_SIZE/2][4];
    
    __syncthreads();
    Load256A(sx, startx);
    Load256A(sy, starty);

    uint32_t i;

    
    odd_py = sy[0] & 1;
    if ((d_searchInfo & 1) == SEARCH_MODE_PUBKEY) { 
        CheckPointPubKey(sx, odd_py, GRP_SIZE / 2, out);
    } else {
        _GetHash160Comp(sx, odd_py, (uint8_t*)h);
        CheckPoint_Opt(h, GRP_SIZE / 2, sAddress, lookup32, out);
    }
    __syncthreads();

    ModSub256(sxn, _2Gnx, sx);
    Load256(subp[GRP_SIZE / 2 - 1], sxn);
    for (i = GRP_SIZE / 2 - 1; i > 0; i--) {
        ModSub256(syn, Gx[i], sx);
        _ModMult(sxn, syn);
        Load256(subp[i - 1], sxn);
    }

    ModSub256(inverse, Gx[0], sx);
    _ModMult(inverse, sxn);

    inverse[4] = 0;
    _ModInv(inverse);

    __syncthreads();
    
    ModNeg256(syn, sy);
    ModNeg256(sxn, sx);

    for (i = 0; i < GRP_SIZE / 2 - 1; i++) {

        __syncthreads();
        ModSub256(sx_gx, Gx[i], sxn);

        _ModMult(dx, subp[i], inverse);

        //----------------------------------- 

        ModSub256(dy, Gy[i], sy);
        _ModMult(dy, dx);
        _ModSqr(px, dy);
        ModSub256(px, sx_gx);

        ModSub256(py, sx, px);
        _ModMult(py, dy);
        ModSub256isOdd(py, sy, &odd_py);

        
        if ((d_searchInfo & 1) == SEARCH_MODE_PUBKEY) {
            CheckPointPubKey(px, odd_py, GRP_SIZE / 2 + (i + 1), out);
        } else {
            _GetHash160Comp(px, odd_py, (uint8_t*)h);
            CheckPoint_Opt(h, GRP_SIZE / 2 + (i + 1), sAddress, lookup32, out);
        }
        
       //-----------------------------------      
        __syncthreads();

        ModSub256(dy, syn, Gy[i]);
        _ModMult(dy, dx);
        _ModSqr(px, dy);
        ModSub256(px, sx_gx);

        ModSub256(py, px, sx);
        _ModMult(py, dy);
        ModSub256isOdd(syn, py, &odd_py);

        
        if ((d_searchInfo & 1) == SEARCH_MODE_PUBKEY) {
            CheckPointPubKey(px, odd_py, GRP_SIZE / 2 - (i + 1), out);
        } else {
            _GetHash160Comp(px, odd_py, (uint8_t*)h);
            CheckPoint_Opt(h, GRP_SIZE / 2 - (i + 1), sAddress, lookup32, out);
        }
  
        ModSub256(dx, Gx[i], sx);
        _ModMult(inverse, dx);

    }

    __syncthreads();

    _ModMult(dx, subp[i], inverse);

    ModSub256(dy, syn, Gy[i]);
    _ModMult(dy, dx);
    _ModSqr(px, dy);
    ModSub256(px, sx);
    ModSub256(px, Gx[i]);

    ModSub256(py, px, sx);
    _ModMult(py, dy);
    ModSub256isOdd(syn, py, &odd_py);

    
    if ((d_searchInfo & 1) == SEARCH_MODE_PUBKEY) {
        CheckPointPubKey(px, odd_py, 0, out);
    } else {
        _GetHash160Comp(px, odd_py, (uint8_t*)h);
        CheckPoint_Opt(h, 0, sAddress, lookup32, out);
    }
    //--------------------------------------------
    __syncthreads();

    ModSub256(dy, _2Gny, sy);
    ModSub256(dx, Gx[i], sx);
    _ModMult(inverse, dx);

    _ModMult(dy, inverse);
    _ModSqr(px, dy);
    ModSub256(px, sx);
    ModSub256(px, _2Gnx);

    ModSub256(py, _2Gnx, px);
    _ModMult(py, dy);
    ModSub256(py, _2Gny);               

    __syncthreads();
    Store256A(startx, px);
    Store256A(starty, py);

}

// ---------------------------------------------------------------------------------------
int NB_TRHEAD_PER_GROUP;

using namespace std;

int g_gpuId;
std::string globalGPUname;

GPUEngine::GPUEngine(int gpuId, uint32_t maxFound) {

    cudaDeviceProp deviceProp;
    cudaGetDeviceProperties(&deviceProp, gpuId);

    NB_TRHEAD_PER_GROUP = 256;                                         
    int nbThreadGroup = deviceProp.multiProcessorCount * 128;

    // Always use full grid (no power-of-2 rounding)
    // Previously only -R mode used the full grid — now always optimal

    
    g_gpuId = gpuId;

    
    this->rekey = rekey;
    initialised = false;
    cudaError_t err;

    int deviceCount = 0;
    cudaError_t error_id = cudaGetDeviceCount(&deviceCount);

    if (error_id != cudaSuccess) {
        printf("GPUEngine: CudaGetDeviceCount %s\n", cudaGetErrorString(error_id));
        return;
    }

    // This function call returns 0 if there are no CUDA capable devices.
    if (deviceCount == 0) {
        printf("GPUEngine: There are no available device(s) that support CUDA\n");
        return;
    }

    err = cudaSetDevice(gpuId);
    if (err != cudaSuccess) {
        printf("GPUEngine: %s\n", cudaGetErrorString(err));
        return;
    }

    err = cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
    if (err != cudaSuccess) {
        fprintf(stderr, "GPUEngine: %s\n", cudaGetErrorString(err));
        return;
    }

   
    this->nbThread = nbThreadGroup * NB_TRHEAD_PER_GROUP;
    this->maxFound = maxFound;
    this->outputSize = (maxFound * ITEM_SIZE + 4);

    char tmp[512];
    sprintf(tmp,"GPU #%d %s [sm_%d%d] (%dx%d cores) Grid(%dx%d)",
    gpuId, deviceProp.name,
    deviceProp.major, deviceProp.minor,
    deviceProp.multiProcessorCount,
    _ConvertSMVer2Cores(deviceProp.major, deviceProp.minor),
    nbThread / NB_TRHEAD_PER_GROUP,
    NB_TRHEAD_PER_GROUP);

    deviceName = std::string(tmp);

    globalGPUname = deviceProp.name;

    
    err = cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);
    if (err != cudaSuccess) {
        printf("GPUEngine: %s\n", cudaGetErrorString(err));
        return;
    }

    //size_t stackSize = 49152;
    //err = cudaDeviceSetLimit(cudaLimitStackSize, stackSize);
    //if (err != cudaSuccess) {
    //  printf("GPUEngine: %s\n", cudaGetErrorString(err));
    //  return;
    //}

    /*
    size_t heapSize = ;
    err = cudaDeviceSetLimit(cudaLimitMallocHeapSize, heapSize);
    if (err != cudaSuccess) {
      printf("Error: %s\n", cudaGetErrorString(err));
      exit(0);
    }

    size_t size;
    cudaDeviceGetLimit(&size, cudaLimitStackSize);
    printf("Stack Size %lld\n", size);
    cudaDeviceGetLimit(&size, cudaLimitMallocHeapSize);
    printf("Heap Size %lld\n", size);
    */

    
    err = cudaMalloc((void**)&inputAddress, _64K * 2);
    if (err != cudaSuccess) {
        printf("GPUEngine: Allocate address memory: %s\n", cudaGetErrorString(err));
        return;
    }
    err = cudaHostAlloc(&inputAddressPinned, _64K * 2, cudaHostAllocWriteCombined | cudaHostAllocMapped);
    if (err != cudaSuccess) {
        printf("GPUEngine: Allocate address pinned memory: %s\n", cudaGetErrorString(err));
        return;
    }
    err = cudaMalloc((void**)&inputKey, nbThread * 32 * 2);
    if (err != cudaSuccess) {
        printf("GPUEngine: Allocate input memory: %s\n", cudaGetErrorString(err));
        return;
    }
    err = cudaHostAlloc(&inputKeyPinned, nbThread * 32 * 2, cudaHostAllocWriteCombined | cudaHostAllocMapped);
    if (err != cudaSuccess) {
        printf("GPUEngine: Allocate input pinned memory: %s\n", cudaGetErrorString(err));
        return;
    }
    err = cudaMalloc((void**)&outputBuffer, outputSize);
    if (err != cudaSuccess) {
        printf("GPUEngine: Allocate output memory: %s\n", cudaGetErrorString(err));
        return;
    }
    err = cudaHostAlloc(&outputBufferPinned, outputSize, cudaHostAllocMapped);
    if (err != cudaSuccess) {
        printf("GPUEngine: Allocate output pinned memory: %s\n", cudaGetErrorString(err));
        return;
    }

    searchMode = SEARCH_COMPRESSED;
    searchType = P2PKH;
    initialised = true;
    pattern = "";
    hasPattern = false;
    inputAddressLookUp = NULL;

}

GPUEngine::~GPUEngine() {

    cudaFree(inputKey);
    cudaFree(inputAddress);
    if (inputAddressLookUp) cudaFree(inputAddressLookUp);
    cudaFreeHost(outputBufferPinned);
    cudaFree(outputBuffer);
}

void GPUEngine::PrintCudaInfo() {

    int deviceCount = 0;
    cudaError_t error_id = cudaGetDeviceCount(&deviceCount);

    if(error_id != cudaSuccess || deviceCount == 0) {
        printf("No CUDA-capable GPU detected.\n");
        return;
    }

    printf("[+] Detected %d GPU(s):\n", deviceCount);
    for(int i = 0; i < deviceCount; i++) {
        cudaDeviceProp p;
        cudaGetDeviceProperties(&p, i);

        int smVer = p.major * 10 + p.minor;
        const char* arch = "Unknown";
        if     (smVer >= 90) arch = "Hopper/Blackwell";
        else if(smVer >= 89) arch = "Ada Lovelace";
        else if(smVer >= 80) arch = "Ampere";
        else if(smVer >= 75) arch = "Turing";
        else if(smVer >= 70) arch = "Volta";
        else if(smVer >= 60) arch = "Pascal";
        else if(smVer >= 50) arch = "Maxwell";

        printf("  GPU #%d: %s [sm_%d%d] (%s)\n",
               i, p.name, p.major, p.minor, arch);
        printf("          %d SMs x %d cores = %d CUDA cores\n",
               p.multiProcessorCount,
               _ConvertSMVer2Cores(p.major, p.minor),
               p.multiProcessorCount * _ConvertSMVer2Cores(p.major, p.minor));
        printf("          VRAM: %zu MB\n",
               p.totalGlobalMem / (1024*1024));
    }
}
int GPUEngine::GetNbThread() {
    return nbThread;
}

void GPUEngine::SetSearchMode(int searchMode) {
    this->searchMode = searchMode;
}

void GPUEngine::SetAddress(std::vector<address_t> addresses) {

    memset(inputAddressPinned, 0, _64K * 2);
    for (int i = 0;i < (int)addresses.size();i++)
        inputAddressPinned[addresses[i]] = 1;

    // Fill device memory
    cudaMemcpy(inputAddress, inputAddressPinned, _64K * 2, cudaMemcpyHostToDevice);

    // We do not need the input pinned memory anymore
    cudaFreeHost(inputAddressPinned);
    inputAddressPinned = NULL;
    lostWarning = false;

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("GPUEngine: SetAddress: %s\n", cudaGetErrorString(err));
    }

}

void GPUEngine::SetPattern(const char* pattern) {

    strcpy((char*)inputAddressPinned, pattern);

    
    cudaMemcpy(inputAddress, inputAddressPinned, _64K * 2, cudaMemcpyHostToDevice);

    // We do not need the input pinned memory anymore
    cudaFreeHost(inputAddressPinned);
    inputAddressPinned = NULL;
    lostWarning = false;

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("GPUEngine: SetPattern: %s\n", cudaGetErrorString(err));
    }

    hasPattern = true;

}

void GPUEngine::SetAddress(std::vector<LADDRESS> addresses, uint32_t totalAddress) {

    
    cudaError_t err = cudaMalloc((void**)&inputAddressLookUp, (_64K + totalAddress) * 4);
    if (err != cudaSuccess) {
        printf("GPUEngine: Allocate address lookup memory: %s\n", cudaGetErrorString(err));
        return;
    }
    err = cudaHostAlloc(&inputAddressLookUpPinned, (_64K + totalAddress) * 4, cudaHostAllocWriteCombined | cudaHostAllocMapped);
    if (err != cudaSuccess) {
        printf("GPUEngine: Allocate address lookup pinned memory: %s\n", cudaGetErrorString(err));
        return;
    }

    uint32_t offset = _64K;
    memset(inputAddressPinned, 0, _64K * 2);
    memset(inputAddressLookUpPinned, 0, _64K * 4);
    for (int i = 0; i < (int)addresses.size(); i++) {
        int nbLAddress = (int)addresses[i].lAddresses.size();
        inputAddressPinned[addresses[i].sAddress] = (uint16_t)nbLAddress;
        inputAddressLookUpPinned[addresses[i].sAddress] = offset;
        for (int j = 0; j < nbLAddress; j++) {
            inputAddressLookUpPinned[offset++] = addresses[i].lAddresses[j];
        }
    }

    if (offset != (_64K + totalAddress)) {
        printf("GPUEngine: Wrong totalAddress %d!=%d!\n", offset - _64K, totalAddress);
        return;
    }

    // Fill device memory
    cudaMemcpy(inputAddress, inputAddressPinned, _64K * 2, cudaMemcpyHostToDevice);
    cudaMemcpy(inputAddressLookUp, inputAddressLookUpPinned, (_64K + totalAddress) * 4, cudaMemcpyHostToDevice);

    // We do not need the input pinned memory anymore
    cudaFreeHost(inputAddressPinned);
    inputAddressPinned = NULL;
    cudaFreeHost(inputAddressLookUpPinned);
    inputAddressLookUpPinned = NULL;
    lostWarning = false;

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("GPUEngine: SetAddress (large): %s\n", cudaGetErrorString(err));
    }

}

int GPUEngine::GetStepSize() {

    return STEP_SIZE;

}

int GPUEngine::GetGroupSize() {

    return GRP_SIZE;

}

bool GPUEngine::callKernel() {

    
    cudaMemset(outputBuffer, 0, 4);

    comp_keys << < nbThread / NB_TRHEAD_PER_GROUP, NB_TRHEAD_PER_GROUP >> >
        (inputAddress, inputAddressLookUp, inputKey, outputBuffer);

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("GPUEngine: Kernel: %s\n", cudaGetErrorString(err));
        return false;
    }

    //cudaFree(d_dx);

    return true;

}

bool GPUEngine::SetKeys(Point* p) {

    // Re-allocate pinned input buffer if it was freed by a previous SetKeys.
    // Deep/PR/Jerboa modes call SetKeys repeatedly (once per slot jump).
    if (inputKeyPinned == NULL) {
        cudaError_t aerr = cudaHostAlloc(&inputKeyPinned, nbThread * 32 * 2,
                                cudaHostAllocWriteCombined | cudaHostAllocMapped);
        if (aerr != cudaSuccess || inputKeyPinned == NULL) {
            printf("GPUEngine: SetKeys realloc failed: %s\n",
                   cudaGetErrorString(aerr));
            return false;
        }
    }

    for (int i = 0; i < nbThread; i += NB_TRHEAD_PER_GROUP) {
        for (int j = 0; j < NB_TRHEAD_PER_GROUP; j++) {

            inputKeyPinned[8 * i + j + 0 * NB_TRHEAD_PER_GROUP] = p[i + j].x.bits64[0];
            inputKeyPinned[8 * i + j + 1 * NB_TRHEAD_PER_GROUP] = p[i + j].x.bits64[1];
            inputKeyPinned[8 * i + j + 2 * NB_TRHEAD_PER_GROUP] = p[i + j].x.bits64[2];
            inputKeyPinned[8 * i + j + 3 * NB_TRHEAD_PER_GROUP] = p[i + j].x.bits64[3];

            inputKeyPinned[8 * i + j + 4 * NB_TRHEAD_PER_GROUP] = p[i + j].y.bits64[0];
            inputKeyPinned[8 * i + j + 5 * NB_TRHEAD_PER_GROUP] = p[i + j].y.bits64[1];
            inputKeyPinned[8 * i + j + 6 * NB_TRHEAD_PER_GROUP] = p[i + j].y.bits64[2];
            inputKeyPinned[8 * i + j + 7 * NB_TRHEAD_PER_GROUP] = p[i + j].y.bits64[3];

        }
    }

    

    cudaMemcpy(inputKey, inputKeyPinned, nbThread * 32 * 2, cudaMemcpyHostToDevice);
    // Keep inputKeyPinned allocated — SetKeys may be called again (slot jumps).
    // Freed once in destructor instead. (was: cudaFreeHost + NULL)

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("GPUEngine: SetKeys: %s\n", cudaGetErrorString(err));
    }

    return callKernel();
    //return true;

}

uint64_t new_2Gnx[4];
uint64_t new_2Gny[4];

bool GPUEngine::SetRandomJump(Point p) {

    new_2Gnx[0] = p.x.bits64[0];
    new_2Gnx[1] = p.x.bits64[1];
    new_2Gnx[2] = p.x.bits64[2];
    new_2Gnx[3] = p.x.bits64[3];

    new_2Gny[0] = p.y.bits64[0];
    new_2Gny[1] = p.y.bits64[1];
    new_2Gny[2] = p.y.bits64[2];
    new_2Gny[3] = p.y.bits64[3];

    cudaError_t err;

    err = cudaMemcpyToSymbol(_2Gnx, new_2Gnx, sizeof(new_2Gnx));
    if (err != cudaSuccess) {
        printf("GPUEngine: SetRandomJump _2Gnx: %s\n", cudaGetErrorString(err));
        return false;
    }

    err = cudaMemcpyToSymbol(_2Gny, new_2Gny, sizeof(new_2Gny));
    if (err != cudaSuccess) {
        printf("GPUEngine: SetRandomJump _2Gny: %s\n", cudaGetErrorString(err));
        return false;
    }

    return true;
    //return callKernel();

}

bool GPUEngine::Launch(std::vector<ITEM>& addressFound, bool spinWait) {

    addressFound.clear();
    
    // Get the result

    if(spinWait) {

      cudaMemcpy(outputBufferPinned, outputBuffer, outputSize, cudaMemcpyDeviceToHost);

    } else {

      // Use cudaMemcpyAsync to avoid default spin wait of cudaMemcpy wich takes 100% CPU
      cudaEvent_t evt;
      cudaEventCreate(&evt);

      //cudaMemcpy(outputBufferPinned, outputBuffer, 4, cudaMemcpyDeviceToHost);
      cudaMemcpyAsync(outputBufferPinned, outputBuffer, 4, cudaMemcpyDeviceToHost, 0);

      cudaEventRecord(evt, 0);
      while (cudaEventQuery(evt) == cudaErrorNotReady) {
        // Sleep 1 ms to free the CPU
        Timer::SleepMillis(1);
      }
      cudaEventDestroy(evt);

    }

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
      printf("GPUEngine: Launch: %s\n", cudaGetErrorString(err));
      return false;
    }

    // Look for address found
    uint32_t nbFound = outputBufferPinned[0];

    if (nbFound > maxFound) {
      // address has been lost
      if (!lostWarning) {
        printf("\nWarning, %d items lost\nHint: Search with less addresses/prefixes or increase maxFound (-m) using multiple of 65536\n", (nbFound - maxFound));
        lostWarning = true;
      }
      nbFound = maxFound;
    }

    // When can perform a standard copy, the kernel is eneded
    cudaMemcpy(outputBufferPinned, outputBuffer, nbFound * ITEM_SIZE + 4, cudaMemcpyDeviceToHost);

    for (uint32_t i = 0; i < nbFound; i++) {
        uint32_t* itemPtr = outputBufferPinned + (i * ITEM_SIZE32 + 1);
        ITEM it;
        it.thId = itemPtr[0];
        int16_t* ptr = (int16_t*)&(itemPtr[1]);
        it.endo = ptr[0] & 0x7FFF;
        it.mode = (ptr[0] & 0x8000) != 0;
        it.incr = ptr[1];
        it.hash = (uint8_t*)(itemPtr + 2);
        addressFound.push_back(it);
    }

    return callKernel();

}

void GPUEngine::SetTargetPublicKey(const uint64_t* pubKeyX, int pubKeyParity) {
    cudaError_t err;

    
    
    
    uint32_t searchInfoValue = SEARCH_MODE_PUBKEY | (pubKeyParity << 1);

    
    err = cudaMemcpyToSymbol(d_targetPubKeyX, pubKeyX, sizeof(uint64_t) * 4);
    if (err != cudaSuccess) {
        printf("GPUEngine: CudaMemcpyToSymbol d_targetPubKeyX failed: %s\n", cudaGetErrorString(err));
    }

    
    err = cudaMemcpyToSymbol(d_searchInfo, &searchInfoValue, sizeof(uint32_t));
    if (err != cudaSuccess) {
        printf("GPUEngine: CudaMemcpyToSymbol d_searchInfo failed: %s\n", cudaGetErrorString(err));
    }
}

void GPUEngine::SetSearchType(int searchType) {
    this->searchType = searchType;
    
    
    uint32_t searchInfoValue = SEARCH_MODE_ADDRESS; 
    cudaError_t err = cudaMemcpyToSymbol(d_searchInfo, &searchInfoValue, sizeof(uint32_t));
    if (err != cudaSuccess) {
        printf("GPUEngine: CudaMemcpyToSymbol d_searchInfo (address mode) failed: %s\n", cudaGetErrorString(err));
    }
}

// -----------------------------------------------------------------------
// OPT: SetHash160Target
// Загружает целевой hash160 в constant memory GPU (d_hash160Target).
// Если isSingle=true — устанавливает флаг SINGLE_TARGET_MODE в d_searchInfo,
// активируя быстрый путь CheckPoint_Opt без global memory reads.
//
// Вызов из Vanity.cpp:
//   if (numAddresses == 1)
//       g->SetHash160Target(targetHash160_20bytes, true);
// -----------------------------------------------------------------------
void GPUEngine::SetHash160Target(const uint8_t* hash160, bool isSingle) {
    cudaError_t err;

    // Загружаем 20 байт (5 × uint32) в constant memory
    err = cudaMemcpyToSymbol(d_hash160Target, hash160, 5 * sizeof(uint32_t));
    if (err != cudaSuccess) {
        printf("GPUEngine: SetHash160Target d_hash160Target failed: %s\n",
               cudaGetErrorString(err));
        return;
    }

    if (isSingle) {
        // Читаем текущее значение d_searchInfo и устанавливаем бит SINGLE_TARGET_MODE
        uint32_t current = 0;
        err = cudaMemcpyFromSymbol(&current, d_searchInfo, sizeof(uint32_t));
        if (err != cudaSuccess) {
            printf("GPUEngine: SetHash160Target read d_searchInfo failed: %s\n",
                   cudaGetErrorString(err));
            return;
        }
        current |= (1u << 2);  // SINGLE_TARGET_MODE
        err = cudaMemcpyToSymbol(d_searchInfo, &current, sizeof(uint32_t));
        if (err != cudaSuccess) {
            printf("GPUEngine: SetHash160Target write d_searchInfo failed: %s\n",
                   cudaGetErrorString(err));
            return;
        }
        // Печатаем только один раз (не при каждом прыжке)
        static bool s_printed = false;
        if(!s_printed){
            printf("GPUEngine: Single-target fast path ENABLED (SINGLE_TARGET_MODE)\n");
            s_printed = true;
        }
    }
}


bool GPUEngine::IsInitialised() {
    return initialised;
}

std::string toHex(unsigned char* data, int length) {

    string ret;
    char tmp[3];
    for (int i = 0; i < length; i++) {
        if (i && i % 4 == 0) ret.append(" ");
        sprintf(tmp, "%02hhX", (int)data[i]);
        ret.append(tmp);
    }
    return ret;

}

void GPUEngine::FreeGPUEngine() {  

    
    cudaDeviceSynchronize();

    
    cudaFree(inputKey);
    cudaFree(inputAddress);
    
    if (inputAddressLookUp) {
    cudaFree(inputAddressLookUp);  
    }
    cudaFree(outputBuffer);

    
    cudaFreeHost(inputAddressPinned);
    cudaFreeHost(inputKeyPinned);
    cudaFreeHost(outputBufferPinned);

    
    inputAddressPinned = NULL;
    inputKeyPinned = NULL;
    outputBufferPinned = NULL;
    inputAddressLookUpPinned = NULL;
    inputAddressLookUp = NULL;

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("GPUEngine: Error freeing memory: %s\n", cudaGetErrorString(err));
    }

    cudaDeviceReset();

}

bool GPUEngine::CheckHash(uint8_t* h, vector<ITEM>& found, int tid, int incr, int endo, int* nbOK) {

    return true;
}

bool GPUEngine::Check(Secp256K1* secp) {

    return true;
}

