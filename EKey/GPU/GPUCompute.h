//  GPU/GPUCompute.h

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
// 8891689_MOD: 确保包含了 GPUEngine.h 以获取 ITEM_SIZE32 等定义
#include "GPUEngine.h" 

// CUDA Kernel main function
// We use affine coordinates for elliptic curve point (ie Z=1)

// 8891689_MOD: 定义新的搜索模式標誌，供內核使用
#define SEARCH_MODE_ADDRESS 0
#define SEARCH_MODE_PUBKEY  1

// 8891689_MOD: 為公鑰搜索模式定義常量內存
__device__ __constant__ uint64_t d_targetPubKeyX[4];
__device__ __constant__ uint32_t d_searchInfo;

// -----------------------------------------------------------------------
// OPT: Constant memory для target hash160 (single-target fast path)
// Загружается из CPU через SetHash160Target() перед запуском ядра.
// Находится в L1-кеше GPU (~4 такта vs ~100+ для global memory).
// Флаг SINGLE_TARGET_MODE устанавливается в d_searchInfo бит 2.
// -----------------------------------------------------------------------
__device__ __constant__ uint32_t d_hash160Target[5];
#define SINGLE_TARGET_MODE (1u << 2)

// 8891689_MOD: 新的公鑰匹配函數 ,https://github.com/8891689
__device__ __noinline__ void CheckPointPubKey(uint64_t *px, uint8_t py_is_odd, int32_t incr, uint32_t *out) {
    if (px[0] == d_targetPubKeyX[0] && px[1] == d_targetPubKeyX[1] &&
        px[2] == d_targetPubKeyX[2] && px[3] == d_targetPubKeyX[3]) {
        
        uint32_t target_y_is_odd = d_searchInfo & 2; // bit 1
        if ((py_is_odd << 1) == target_y_is_odd) {
            uint32_t pos = atomicAdd(out, 1);
            uint32_t tid = (blockIdx.x * blockDim.x) + threadIdx.x;
            uint32_t* item_ptr = out + (pos * ITEM_SIZE32 + 1);
            
            item_ptr[0] = tid;
            item_ptr[1] = (uint32_t)(incr << 16) | (uint32_t)(1 << 15);
            
            uint32_t* px_32 = (uint32_t*)px;
            item_ptr[2] = px_32[0];
            item_ptr[3] = px_32[1];
            item_ptr[4] = px_32[2];
            item_ptr[5] = px_32[3];
            item_ptr[6] = px_32[4];
        }
    }
}

// 原始 CheckPoint 函數，已移除 maxFound 检查 (оставлена без изменений)
__device__ __noinline__ void CheckPoint(uint32_t *_h, int32_t incr, address_t *address, uint32_t *lookup32, uint32_t *out) {

  uint32_t   off;
  addressl_t  l32;
  address_t   pr0;
  address_t   hit;
  uint32_t   st;
  uint32_t   ed;
  uint32_t   mi;
  uint32_t   lmi;
  
    pr0 = *(address_t *)(_h);
    hit = address[pr0];

    if (hit) {
        if (lookup32) {
            off = lookup32[pr0];
            l32 = _h[0];
            st = off;
            ed = off + hit - 1;
            while (st <= ed) {
                mi = (st + ed) / 2;
                lmi = lookup32[mi];
                if (l32 < lmi) {
                    ed = mi - 1;
                }
                else if (l32 == lmi) {
                    goto addItem;
                }
                else {
                    st = mi + 1;
                }
            }
            return;
        }

    addItem:
        uint32_t pos = atomicAdd(out, 1);
        uint32_t tid = (blockIdx.x * blockDim.x) + threadIdx.x;
        uint32_t* item_ptr = out + (pos * ITEM_SIZE32 + 1);
        
        item_ptr[0] = tid;
        item_ptr[1] = (uint32_t)(incr << 16) | (uint32_t)(1 << 15);
        item_ptr[2] = _h[0];
        item_ptr[3] = _h[1];
        item_ptr[4] = _h[2];
        item_ptr[5] = _h[3];
        item_ptr[6] = _h[4];
    }
}

// -----------------------------------------------------------------------
// OPT: CheckPoint_Opt — оптимизированная замена CheckPoint
//
// PATH 1 — SINGLE_TARGET_MODE (бит 2 в d_searchInfo):
//   Каскадное сравнение 5 слов hash160 в регистрах vs constant memory.
//   Нулевые обращения к global memory для 99.9999999% ключей.
//   Запись результата происходит ~1 раз за всё время поиска.
//
// PATH 2 — Multi-target (обычный режим):
//   Warp-level voting (__any_sync) перед вызовом CheckPoint.
//   Если ни один из 32 потоков варпа не прошёл 16-bit sAddress фильтр —
//   весь варп пропускает тело CheckPoint без ветвлений.
//   Вероятность пропуска: (1 - 1/65536)^32 ≈ 99.95% варпов.
// -----------------------------------------------------------------------
__device__ __noinline__ void CheckPoint_Opt(
    uint32_t *_h, int32_t incr,
    address_t *address, uint32_t *lookup32, uint32_t *out)
{
    // ------------------------------------------------------------------
    // PATH 1: Single-target fast path — только регистры + constant mem
    // ------------------------------------------------------------------
    if (d_searchInfo & SINGLE_TARGET_MODE) {

        // Каскад: каждый if отсекает ~4 млрд из 4 млрд ключей
        if (_h[0] != d_hash160Target[0]) return;  // 99.9999999% уходят здесь
        if (_h[1] != d_hash160Target[1]) return;
        if (_h[2] != d_hash160Target[2]) return;
        if (_h[3] != d_hash160Target[3]) return;
        if (_h[4] != d_hash160Target[4]) return;

        // Полное совпадение 160 бит — НАШЛИ КЛЮЧ
        uint32_t pos = atomicAdd(out, 1);
        uint32_t tid = (blockIdx.x * blockDim.x) + threadIdx.x;
        uint32_t* item_ptr = out + (pos * ITEM_SIZE32 + 1);
        item_ptr[0] = tid;
        item_ptr[1] = (uint32_t)(incr << 16) | (uint32_t)(1 << 15);
        item_ptr[2] = _h[0];
        item_ptr[3] = _h[1];
        item_ptr[4] = _h[2];
        item_ptr[5] = _h[3];
        item_ptr[6] = _h[4];
        return;
    }

    // ------------------------------------------------------------------
    // PATH 2: Multi-target с warp voting
    // sAddress читается для каждого потока (16-bit, неизбежно),
    // но CheckPoint вызывается только если хотя бы 1 поток в варпе
    // имеет потенциальное совпадение.
    // ------------------------------------------------------------------
    address_t pr0  = *(address_t*)(_h);
    address_t hit  = address[pr0];

    // OPT: Warp vote — __any_sync проверяет 32 потока за 1 инструкцию.
    // Если ВСЕ 32 потока варпа имеют hit==0 → пропускаем CheckPoint целиком.
    if (!__any_sync(__activemask(), (uint32_t)hit)) return;

    // Хотя бы один поток имеет кандидата
    if (hit) {
        CheckPoint(_h, incr, address, lookup32, out);
    }
}
