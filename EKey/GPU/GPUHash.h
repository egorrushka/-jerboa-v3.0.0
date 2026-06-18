//   GPU/GPUHash.h
// OPT: SHA256 + RIPEMD160 оптимизации:
//   1. SHA256Transform      → __forceinline__ (устранение call overhead)
//   2. SHA256Transform_inplace → новый: пишет результат обратно в w[0..7]
//   3. RIPEMD160Transform   → __forceinline__ (устранение call overhead)
//   4. _GetHash160Comp      → единый массив w[16] вместо двух arrays
//      publicKeyBytes[16] + s[16] = 128 байт → w[16] = 64 байта
//      Экономия 64 байта локальной памяти на поток
//      ~96 MB меньше давления на кеш при 1.5M потоков

__device__ __constant__ uint32_t K[] =
{
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

#define ROR(x,n) ((x>>n)|(x<<(32-n)))
#define S0(x) (ROR(x,2) ^ ROR(x,13) ^ ROR(x,22))
#define S1(x) (ROR(x,6) ^ ROR(x,11) ^ ROR(x,25))
#define s0(x) (ROR(x,7) ^ ROR(x,18) ^ (x >> 3))
#define s1(x) (ROR(x,17) ^ ROR(x,19) ^ (x >> 10))

#define Maj(x,y,z) ((x & y) | (z & (x | y)))
#define Ch(x,y,z)  (z ^ (x & (y ^ z)))

#define S2Round(a, b, c, d, e, f, g, h, k, w) \
    t1 = h + S1(e) + Ch(e,f,g) + k + (w); \
    d += t1; \
    h = t1 + S0(a) + Maj(a,b,c);

__device__ __constant__ uint32_t K_wmix_init[] =
{
    0xA50000, 0x10420023,
};

#define WMIX_init() { \
w[0] += s0(w[1]);\
w[1] +=  K_wmix_init[0] + s0(w[2]);\
w[2] += s1(w[0]) + s0(w[3]);\
w[3] += s1(w[1]) + s0(w[4]);\
w[4] += s1(w[2]) + s0(w[5]);\
w[5] += s1(w[3]) + s0(w[6]);\
w[6] += s1(w[4]) + w[15] + s0(w[7]);\
w[7] += s1(w[5]) + w[0] + s0(w[8]);\
w[8] += s1(w[6]) + w[1];\
w[9] += s1(w[7]) + w[2];\
w[10] += s1(w[8]) + w[3];\
w[11] += s1(w[9]) + w[4];\
w[12] += s1(w[10]) + w[5];\
w[13] += s1(w[11]) + w[6];\
w[14] += s1(w[12]) + w[7] +  K_wmix_init[1];\
w[15] += s1(w[13]) + w[8] + s0(w[0]);\
}

#define WMIX() { \
w[0] += s1(w[14]) + w[9] + s0(w[1]);\
w[1] += s1(w[15]) + w[10] + s0(w[2]);\
w[2] += s1(w[0]) + w[11] + s0(w[3]);\
w[3] += s1(w[1]) + w[12] + s0(w[4]);\
w[4] += s1(w[2]) + w[13] + s0(w[5]);\
w[5] += s1(w[3]) + w[14] + s0(w[6]);\
w[6] += s1(w[4]) + w[15] + s0(w[7]);\
w[7] += s1(w[5]) + w[0] + s0(w[8]);\
w[8] += s1(w[6]) + w[1] + s0(w[9]);\
w[9] += s1(w[7]) + w[2] + s0(w[10]);\
w[10] += s1(w[8]) + w[3] + s0(w[11]);\
w[11] += s1(w[9]) + w[4] + s0(w[12]);\
w[12] += s1(w[10]) + w[5] + s0(w[13]);\
w[13] += s1(w[11]) + w[6] + s0(w[14]);\
w[14] += s1(w[12]) + w[7] + s0(w[15]);\
w[15] += s1(w[13]) + w[8] + s0(w[0]);\
}

#define SHA256_RND(k) {\
S2Round(a, b, c, d, e, f, g, h, K[k],      w[0]);\
S2Round(h, a, b, c, d, e, f, g, K[k + 1],  w[1]);\
S2Round(g, h, a, b, c, d, e, f, K[k + 2],  w[2]);\
S2Round(f, g, h, a, b, c, d, e, K[k + 3],  w[3]);\
S2Round(e, f, g, h, a, b, c, d, K[k + 4],  w[4]);\
S2Round(d, e, f, g, h, a, b, c, K[k + 5],  w[5]);\
S2Round(c, d, e, f, g, h, a, b, K[k + 6],  w[6]);\
S2Round(b, c, d, e, f, g, h, a, K[k + 7],  w[7]);\
S2Round(a, b, c, d, e, f, g, h, K[k + 8],  w[8]);\
S2Round(h, a, b, c, d, e, f, g, K[k + 9],  w[9]);\
S2Round(g, h, a, b, c, d, e, f, K[k + 10], w[10]);\
S2Round(f, g, h, a, b, c, d, e, K[k + 11], w[11]);\
S2Round(e, f, g, h, a, b, c, d, K[k + 12], w[12]);\
S2Round(d, e, f, g, h, a, b, c, K[k + 13], w[13]);\
S2Round(c, d, e, f, g, h, a, b, K[k + 14], w[14]);\
S2Round(b, c, d, e, f, g, h, a, K[k + 15], w[15]);\
}

// -----------------------------------------------------------------------
// OPT: SHA256Transform — __forceinline__
// Оригинальная функция, теперь inline → устранение call overhead,
// компилятор видит полный SHA256 в контексте _GetHash160Comp.
// -----------------------------------------------------------------------
__device__ __forceinline__ void SHA256Transform(uint32_t s[8], uint32_t* w) {

  uint32_t t1;

  s[0] = 0x6a09e667ul; s[1] = 0xbb67ae85ul;
  s[2] = 0x3c6ef372ul; s[3] = 0xa54ff53aul;
  s[4] = 0x510e527ful; s[5] = 0x9b05688cul;
  s[6] = 0x1f83d9abul; s[7] = 0x5be0cd19ul;

  uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
  uint32_t e = s[4], f = s[5], g = s[6], h = s[7];

  SHA256_RND(0);  WMIX_init();
  SHA256_RND(16); WMIX();
  SHA256_RND(32); WMIX();
  SHA256_RND(48);

  s[0] += a; s[1] += b; s[2] += c; s[3] += d;
  s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

// -----------------------------------------------------------------------
// OPT: SHA256Transform_inplace
// НОВАЯ функция — результат пишется ОБРАТНО в w[0..7].
// Устраняет промежуточный массив publicKeyBytes[16] в _GetHash160Comp.
// После вызова: w[0..7] = SHA256 хеш, w[8..15] = мусор (будут перезаписаны).
//
// Принцип работы:
//   SHA256_RND только ЧИТАЕТ w[], не пишет.
//   WMIX модифицирует w[] для расширения message schedule.
//   Финальное состояние a..h + IV пишется в w[0..7].
// -----------------------------------------------------------------------
__device__ __forceinline__ void SHA256Transform_inplace(uint32_t* w) {

  uint32_t t1;
  // Инициализируем из констант (не из w[], в отличие от оригинала)
  uint32_t a = 0x6a09e667ul, b = 0xbb67ae85ul;
  uint32_t c = 0x3c6ef372ul, d = 0xa54ff53aul;
  uint32_t e = 0x510e527ful, f = 0x9b05688cul;
  uint32_t g = 0x1f83d9abul, h = 0x5be0cd19ul;

  // 64 раунда SHA256 + 3 расширения message schedule (те же что оригинал)
  SHA256_RND(0);  WMIX_init();
  SHA256_RND(16); WMIX();
  SHA256_RND(32); WMIX();
  SHA256_RND(48);

  // Пишем результат обратно в w[0..7] (IV + финальное состояние)
  w[0] = 0x6a09e667ul + a;
  w[1] = 0xbb67ae85ul + b;
  w[2] = 0x3c6ef372ul + c;
  w[3] = 0xa54ff53aul + d;
  w[4] = 0x510e527ful + e;
  w[5] = 0x9b05688cul + f;
  w[6] = 0x1f83d9abul + g;
  w[7] = 0x5be0cd19ul + h;
}

// -----------------------------------------------------------------------
// RIPEMD160
// -----------------------------------------------------------------------
__device__ __constant__ uint32_t K160[] =
{
    0x5A827999ul, 0x6ED9EBA1ul, 0x8F1BBCDCul, 0xA953FD4Eul,
    0x50A28BE6ul, 0x5C4DD124ul, 0x6D703EF3ul, 0x7A6D76E9ul,
};

__device__ __forceinline__ void RIPEMD160Initialize(uint32_t s[5]) {
  s[0] = 0x67452301ul; s[1] = 0xEFCDAB89ul; s[2] = 0x98BADCFEul;
  s[3] = 0x10325476ul; s[4] = 0xC3D2E1F0ul;
}

#define ROL(x,n)   ((x>>(32-n))|(x<<n))
#define ROL10(x)   ((x>>(22))|(x<<10))
#define f1(x,y,z)  (x ^ y ^ z)
#define f2(x,y,z)  ((x & y) | (~x & z))
#define f3(x,y,z)  ((x | ~y) ^ z)
#define f4(x,y,z)  ((x & z) | (~z & y))
#define f5(x,y,z)  (x ^ (y | ~z))

#define RPRound(a,b,c,d,e,f,x,k,r) \
  u = a + f + x + k; \
  a = ROL(u, r) + e; \
  c = ROL10(c);

#define RPRound_k(a,b,c,d,e,f,x,r) \
  u = a + f + x; \
  a = ROL(u, r) + e; \
  c = ROL10(c);

#define R11(a,b,c,d,e,x,r) RPRound_k(a,b,c,d,e,f1(b,c,d),x,r)
#define R21(a,b,c,d,e,x,r) RPRound(a,b,c,d,e,f2(b,c,d),x,K160[0],r)
#define R31(a,b,c,d,e,x,r) RPRound(a,b,c,d,e,f3(b,c,d),x,K160[1],r)
#define R41(a,b,c,d,e,x,r) RPRound(a,b,c,d,e,f4(b,c,d),x,K160[2],r)
#define R51(a,b,c,d,e,x,r) RPRound(a,b,c,d,e,f5(b,c,d),x,K160[3],r)
#define R12(a,b,c,d,e,x,r) RPRound(a,b,c,d,e,f5(b,c,d),x,K160[4],r)
#define R22(a,b,c,d,e,x,r) RPRound(a,b,c,d,e,f4(b,c,d),x,K160[5],r)
#define R32(a,b,c,d,e,x,r) RPRound(a,b,c,d,e,f3(b,c,d),x,K160[6],r)
#define R42(a,b,c,d,e,x,r) RPRound(a,b,c,d,e,f2(b,c,d),x,K160[7],r)
#define R52(a,b,c,d,e,x,r) RPRound_k(a,b,c,d,e,f1(b,c,d),x,r)

// -----------------------------------------------------------------------
// OPT: RIPEMD160Transform — __forceinline__
// Устраняет call overhead, даёт компилятору видеть полную цепочку
// SHA256 → RIPEMD160 внутри _GetHash160Comp.
// -----------------------------------------------------------------------
__device__ __forceinline__ void RIPEMD160Transform(uint32_t s[5], uint32_t* w) {

  uint32_t u;
  uint32_t a1=s[0],b1=s[1],c1=s[2],d1=s[3],e1=s[4];
  uint32_t a2=a1, b2=b1, c2=c1, d2=d1, e2=e1;

  R11(a1,b1,c1,d1,e1,w[0],11);  R12(a2,b2,c2,d2,e2,w[5],8);
  R11(e1,a1,b1,c1,d1,w[1],14);  R12(e2,a2,b2,c2,d2,w[14],9);
  R11(d1,e1,a1,b1,c1,w[2],15);  R12(d2,e2,a2,b2,c2,w[7],9);
  R11(c1,d1,e1,a1,b1,w[3],12);  R12(c2,d2,e2,a2,b2,w[0],11);
  R11(b1,c1,d1,e1,a1,w[4],5);   R12(b2,c2,d2,e2,a2,w[9],13);
  R11(a1,b1,c1,d1,e1,w[5],8);   R12(a2,b2,c2,d2,e2,w[2],15);
  R11(e1,a1,b1,c1,d1,w[6],7);   R12(e2,a2,b2,c2,d2,w[11],15);
  R11(d1,e1,a1,b1,c1,w[7],9);   R12(d2,e2,a2,b2,c2,w[4],5);
  R11(c1,d1,e1,a1,b1,w[8],11);  R12(c2,d2,e2,a2,b2,w[13],7);
  R11(b1,c1,d1,e1,a1,w[9],13);  R12(b2,c2,d2,e2,a2,w[6],7);
  R11(a1,b1,c1,d1,e1,w[10],14); R12(a2,b2,c2,d2,e2,w[15],8);
  R11(e1,a1,b1,c1,d1,w[11],15); R12(e2,a2,b2,c2,d2,w[8],11);
  R11(d1,e1,a1,b1,c1,w[12],6);  R12(d2,e2,a2,b2,c2,w[1],14);
  R11(c1,d1,e1,a1,b1,w[13],7);  R12(c2,d2,e2,a2,b2,w[10],14);
  R11(b1,c1,d1,e1,a1,w[14],9);  R12(b2,c2,d2,e2,a2,w[3],12);
  R11(a1,b1,c1,d1,e1,w[15],8);  R12(a2,b2,c2,d2,e2,w[12],6);

  R21(e1,a1,b1,c1,d1,w[7],7);   R22(e2,a2,b2,c2,d2,w[6],9);
  R21(d1,e1,a1,b1,c1,w[4],6);   R22(d2,e2,a2,b2,c2,w[11],13);
  R21(c1,d1,e1,a1,b1,w[13],8);  R22(c2,d2,e2,a2,b2,w[3],15);
  R21(b1,c1,d1,e1,a1,w[1],13);  R22(b2,c2,d2,e2,a2,w[7],7);
  R21(a1,b1,c1,d1,e1,w[10],11); R22(a2,b2,c2,d2,e2,w[0],12);
  R21(e1,a1,b1,c1,d1,w[6],9);   R22(e2,a2,b2,c2,d2,w[13],8);
  R21(d1,e1,a1,b1,c1,w[15],7);  R22(d2,e2,a2,b2,c2,w[5],9);
  R21(c1,d1,e1,a1,b1,w[3],15);  R22(c2,d2,e2,a2,b2,w[10],11);
  R21(b1,c1,d1,e1,a1,w[12],7);  R22(b2,c2,d2,e2,a2,w[14],7);
  R21(a1,b1,c1,d1,e1,w[0],12);  R22(a2,b2,c2,d2,e2,w[15],7);
  R21(e1,a1,b1,c1,d1,w[9],15);  R22(e2,a2,b2,c2,d2,w[8],12);
  R21(d1,e1,a1,b1,c1,w[5],9);   R22(d2,e2,a2,b2,c2,w[12],7);
  R21(c1,d1,e1,a1,b1,w[2],11);  R22(c2,d2,e2,a2,b2,w[4],6);
  R21(b1,c1,d1,e1,a1,w[14],7);  R22(b2,c2,d2,e2,a2,w[9],15);
  R21(a1,b1,c1,d1,e1,w[11],13); R22(a2,b2,c2,d2,e2,w[1],13);
  R21(e1,a1,b1,c1,d1,w[8],12);  R22(e2,a2,b2,c2,d2,w[2],11);

  R31(d1,e1,a1,b1,c1,w[3],11);  R32(d2,e2,a2,b2,c2,w[15],9);
  R31(c1,d1,e1,a1,b1,w[10],13); R32(c2,d2,e2,a2,b2,w[5],7);
  R31(b1,c1,d1,e1,a1,w[14],6);  R32(b2,c2,d2,e2,a2,w[1],15);
  R31(a1,b1,c1,d1,e1,w[4],7);   R32(a2,b2,c2,d2,e2,w[3],11);
  R31(e1,a1,b1,c1,d1,w[9],14);  R32(e2,a2,b2,c2,d2,w[7],8);
  R31(d1,e1,a1,b1,c1,w[15],9);  R32(d2,e2,a2,b2,c2,w[14],6);
  R31(c1,d1,e1,a1,b1,w[8],13);  R32(c2,d2,e2,a2,b2,w[6],6);
  R31(b1,c1,d1,e1,a1,w[1],15);  R32(b2,c2,d2,e2,a2,w[9],14);
  R31(a1,b1,c1,d1,e1,w[2],14);  R32(a2,b2,c2,d2,e2,w[11],12);
  R31(e1,a1,b1,c1,d1,w[7],8);   R32(e2,a2,b2,c2,d2,w[8],13);
  R31(d1,e1,a1,b1,c1,w[0],13);  R32(d2,e2,a2,b2,c2,w[12],5);
  R31(c1,d1,e1,a1,b1,w[6],6);   R32(c2,d2,e2,a2,b2,w[2],14);
  R31(b1,c1,d1,e1,a1,w[13],5);  R32(b2,c2,d2,e2,a2,w[10],13);
  R31(a1,b1,c1,d1,e1,w[11],12); R32(a2,b2,c2,d2,e2,w[0],13);
  R31(e1,a1,b1,c1,d1,w[5],7);   R32(e2,a2,b2,c2,d2,w[4],7);
  R31(d1,e1,a1,b1,c1,w[12],5);  R32(d2,e2,a2,b2,c2,w[13],5);

  R41(c1,d1,e1,a1,b1,w[1],11);  R42(c2,d2,e2,a2,b2,w[8],15);
  R41(b1,c1,d1,e1,a1,w[9],12);  R42(b2,c2,d2,e2,a2,w[6],5);
  R41(a1,b1,c1,d1,e1,w[11],14); R42(a2,b2,c2,d2,e2,w[4],8);
  R41(e1,a1,b1,c1,d1,w[10],15); R42(e2,a2,b2,c2,d2,w[1],11);
  R41(d1,e1,a1,b1,c1,w[0],14);  R42(d2,e2,a2,b2,c2,w[3],14);
  R41(c1,d1,e1,a1,b1,w[8],15);  R42(c2,d2,e2,a2,b2,w[11],14);
  R41(b1,c1,d1,e1,a1,w[12],9);  R42(b2,c2,d2,e2,a2,w[15],6);
  R41(a1,b1,c1,d1,e1,w[4],8);   R42(a2,b2,c2,d2,e2,w[0],14);
  R41(e1,a1,b1,c1,d1,w[13],9);  R42(e2,a2,b2,c2,d2,w[5],6);
  R41(d1,e1,a1,b1,c1,w[3],14);  R42(d2,e2,a2,b2,c2,w[12],9);
  R41(c1,d1,e1,a1,b1,w[7],5);   R42(c2,d2,e2,a2,b2,w[2],12);
  R41(b1,c1,d1,e1,a1,w[15],6);  R42(b2,c2,d2,e2,a2,w[13],9);
  R41(a1,b1,c1,d1,e1,w[14],8);  R42(a2,b2,c2,d2,e2,w[9],12);
  R41(e1,a1,b1,c1,d1,w[5],6);   R42(e2,a2,b2,c2,d2,w[7],5);
  R41(d1,e1,a1,b1,c1,w[6],5);   R42(d2,e2,a2,b2,c2,w[10],15);
  R41(c1,d1,e1,a1,b1,w[2],12);  R42(c2,d2,e2,a2,b2,w[14],8);

  R51(b1,c1,d1,e1,a1,w[4],9);   R52(b2,c2,d2,e2,a2,w[12],8);
  R51(a1,b1,c1,d1,e1,w[0],15);  R52(a2,b2,c2,d2,e2,w[15],5);
  R51(e1,a1,b1,c1,d1,w[5],5);   R52(e2,a2,b2,c2,d2,w[10],12);
  R51(d1,e1,a1,b1,c1,w[9],11);  R52(d2,e2,a2,b2,c2,w[4],9);
  R51(c1,d1,e1,a1,b1,w[7],6);   R52(c2,d2,e2,a2,b2,w[1],12);
  R51(b1,c1,d1,e1,a1,w[12],8);  R52(b2,c2,d2,e2,a2,w[5],5);
  R51(a1,b1,c1,d1,e1,w[2],13);  R52(a2,b2,c2,d2,e2,w[8],14);
  R51(e1,a1,b1,c1,d1,w[10],12); R52(e2,a2,b2,c2,d2,w[7],6);
  R51(d1,e1,a1,b1,c1,w[14],5);  R52(d2,e2,a2,b2,c2,w[6],8);
  R51(c1,d1,e1,a1,b1,w[1],12);  R52(c2,d2,e2,a2,b2,w[2],13);
  R51(b1,c1,d1,e1,a1,w[3],13);  R52(b2,c2,d2,e2,a2,w[13],6);
  R51(a1,b1,c1,d1,e1,w[8],14);  R52(a2,b2,c2,d2,e2,w[14],5);
  R51(e1,a1,b1,c1,d1,w[11],11); R52(e2,a2,b2,c2,d2,w[0],15);
  R51(d1,e1,a1,b1,c1,w[6],8);   R52(d2,e2,a2,b2,c2,w[3],13);
  R51(c1,d1,e1,a1,b1,w[15],5);  R52(c2,d2,e2,a2,b2,w[9],11);
  R51(b1,c1,d1,e1,a1,w[13],6);  R52(b2,c2,d2,e2,a2,w[11],11);

  uint32_t t = s[0];
  s[0] = s[1] + c1 + d2;
  s[1] = s[2] + d1 + e2;
  s[2] = s[3] + e1 + a2;
  s[3] = s[4] + a1 + b2;
  s[4] = t    + b1 + c2;
}

// -----------------------------------------------------------------------
// OPT: _GetHash160Comp — единый массив w[16]
//
// БЫЛО: publicKeyBytes[16] + s[16] = 128 байт локальной памяти
// СТАЛО: w[16] = 64 байта локальной памяти
//
// Поток:
//   1. w[0..15] = compressed pubkey bytes (SHA256 message schedule)
//   2. SHA256Transform_inplace(w) → w[0..7] = SHA256 hash
//   3. byte_perm w[0..7] (big→little endian для RIPEMD)
//   4. w[8..15] = RIPEMD160 padding для 32-байтного входа
//   5. RIPEMD160Transform(hash, w) → hash[0..4] = hash160
// -----------------------------------------------------------------------
__device__ __noinline__ void _GetHash160Comp(uint64_t *x, uint8_t isOdd, uint8_t *hash) {

  uint32_t *x32 = (uint32_t *)(x);
  uint32_t w[16];  // OPT: единый буфер (было: publicKeyBytes[16] + s[16])

  // ── Шаг 1: SHA256 message schedule (compressed pubkey, big-endian) ──
  w[0]  = __byte_perm(x32[7], 0x2 + isOdd, 0x4321);
  w[1]  = __byte_perm(x32[7], x32[6], 0x0765);
  w[2]  = __byte_perm(x32[6], x32[5], 0x0765);
  w[3]  = __byte_perm(x32[5], x32[4], 0x0765);
  w[4]  = __byte_perm(x32[4], x32[3], 0x0765);
  w[5]  = __byte_perm(x32[3], x32[2], 0x0765);
  w[6]  = __byte_perm(x32[2], x32[1], 0x0765);
  w[7]  = __byte_perm(x32[1], x32[0], 0x0765);
  w[8]  = __byte_perm(x32[0], 0x80, 0x0456);
  w[9]  = 0; w[10] = 0; w[11] = 0;
  w[12] = 0; w[13] = 0; w[14] = 0;
  w[15] = 0x108;

  // ── Шаг 2: SHA256 inplace → результат в w[0..7] ──
  SHA256Transform_inplace(w);

  // ── Шаг 3: byte_perm SHA256 output (big→little endian для RIPEMD) ──
  w[0] = __byte_perm(w[0], 0, 0x0123);
  w[1] = __byte_perm(w[1], 0, 0x0123);
  w[2] = __byte_perm(w[2], 0, 0x0123);
  w[3] = __byte_perm(w[3], 0, 0x0123);
  w[4] = __byte_perm(w[4], 0, 0x0123);
  w[5] = __byte_perm(w[5], 0, 0x0123);
  w[6] = __byte_perm(w[6], 0, 0x0123);
  w[7] = __byte_perm(w[7], 0, 0x0123);

  // ── Шаг 4: RIPEMD160 padding для 32-байтного входа ──
  // SHA256 output = 32 bytes → length = 256 bits = 0x100
  *(uint64_t *)(w + 8)  = 0x80ULL;   // w[8]=0x80, w[9]=0
  *(uint64_t *)(w + 10) = 0ULL;      // w[10]=0, w[11]=0
  *(uint64_t *)(w + 12) = 0ULL;      // w[12]=0, w[13]=0
  *(uint64_t *)(w + 14) = 32 << 3;   // w[14]=0x100 (256 bits), w[15]=0

  // ── Шаг 5: RIPEMD160 → hash[0..4] = hash160 ──
  RIPEMD160Initialize((uint32_t *)hash);
  RIPEMD160Transform((uint32_t *)hash, w);
}
