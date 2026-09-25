# OBS-VST
Use VST 2.x plugins as audio filters in OBS.

![Plugin Preview](screenshot.png)

## Tests

`tests/lock_regression_test.cpp` is a standalone regression test for
`VSTPlugin`'s `m_effectStatusMutex` locking scheme. It doesn't link against
libobs or gRPC — it reproduces the actual synchronization pattern (shared
locks for `process()` and the state-read/write calls, exclusive for
load/unload) and asserts that an audio thread doesn't get starved while a
save (`getChunk()`) is in progress.

### Via CMake / ctest

```sh
cmake -DSLVST_BUILD_TESTS=ON <path to plugins/sl-vst>
cmake --build . --target sl-vst-lock-regression-test
ctest -R sl-vst-lock-regression-test --output-on-failure
```

`SLVST_BUILD_TESTS` is `OFF` by default, so it has no effect on a normal
`obs-vst` build unless you opt in.

### Standalone, without CMake

```sh
g++ -std=c++17 -O2 -pthread tests/lock_regression_test.cpp -o lock_regression_test
./lock_regression_test
```

On Windows with MSVC (from a "x64 Native Tools Command Prompt for VS"):

```bat
cl /std:c++17 /O2 /EHsc tests\lock_regression_test.cpp
lock_regression_test.exe
```

A passing run prints `dropped=0` for the fixed (shared_mutex) design and a
nonzero `dropped` count for the pre-fix (single exclusive mutex) design,
then `All checks passed.`

## Research
### Sites
*  http://teragonaudio.com/article/How-to-make-your-own-VST-host.html
*  http://www.reaper.fm/sdk/vst/vst_ext.php
*  https://forum.juce.com/t/mac-64-bit/6295/5
*  https://gist.github.com/t-mat/206e3e7dfc3f89421bc1
*  https://github.com/audacity/audacity/blob/17afc51644b2b327e173a23d6066dde598838c03/src/effects/VST/aeffectx.h

### Info
> In general VST 2.4 is platform independent. There are only three platform
  dependent opcodes :  
  effEditOpen  
  audioMasterGetDirectory  
  audioMasterOpenFileSelector
> 
> Here are the required API changes for 64 bit Mac OS X:
>
> effEditOpen:  
  the [ptr] argument is a WindowRef on 32 bit Mac.  
  On 64 bit this is a NSView pointer. The plug-in needs to add its own NSView as
  subview of it.
>
> audioMasterGetDirectory:  
  the [return value] is a FSSpec on 32 bit Mac.  
  On 64 bit this is a char pointer pointing to an UTF-8 encoded string.
>
> audioMasterOpenFileSelector:  
  the VstFileSelect struct uses FSSpec's on 32 bit Mac.  
  On 64 bit Mac these are char pointers pointing to UTF-8 encoded strings.
