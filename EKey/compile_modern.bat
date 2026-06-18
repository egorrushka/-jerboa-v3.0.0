@echo off
cd C:\TEST\EKey

echo Compiling resources...
rc.exe /fo app.res app.rc
if %ERRORLEVEL% NEQ 0 (
  echo [WARNING] rc.exe failed - building without icon
  SET RES_FILE=
) else (
  SET RES_FILE=app.res
)

nvcc -O3 -arch=sm_75 -m64 -DWIN64 -DWITHGPU ^
  -gencode arch=compute_75,code=sm_75 ^
  -gencode arch=compute_86,code=sm_86 ^
  -gencode arch=compute_89,code=sm_89 ^
  -gencode arch=compute_89,code=compute_89 ^
  main.cpp Vanity.cpp Jerboa.cpp ^
  Int.cpp IntMod.cpp IntGroup.cpp ^
  Point.cpp SECP256K1.cpp ^
  Base58.cpp Bech32.cpp ^
  Timer.cpp Random.cpp Wildcard.cpp ^
  hash/sha256.cpp hash/sha256_sse.cpp ^
  hash/ripemd160.cpp hash/ripemd160_sse.cpp ^
  hash/sha512.cpp ^
  GPU/GPUEngine.cu GPU/GPUGenerate.cpp ^
  -I. ^
  -Xcompiler "/openmp /O2 /W3 /MD" ^
  -Xlinker advapi32.lib ^
  -Xlinker "%RES_FILE%" ^
  -o EKey-Jerboa.exe

if %ERRORLEVEL% EQU 0 (
  echo.
  echo [OK] EKey-Jerboa.exe - READY
  echo      sm_75: RTX 2060-2080 Ti
  echo      sm_86: RTX 3070-3090 / A4000
  echo      sm_89: RTX 4070-4090
  echo      PTX  : RTX 5xxx+ JIT
) else (
  echo.
  echo [FAIL] Build error!
)
echo.
pause
