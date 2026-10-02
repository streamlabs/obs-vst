# OBS-VST
Use VST 2.x plugins as audio filters in OBS.

![Plugin Preview](screenshot.png)

## Tests
`tests/save_while_processing_test.cpp` checks that saving the filter (`vst_save()`) doesn't interrupt live audio, and that `getChunk()` rejects a chunk whose reported size is larger than the data received. It runs the real `VSTPlugin` code against a fake in-process proxy, so no VST plug-in or `win-streamlabs-vst.exe` is needed. It's Windows only.

The test is only built when `BUILD_TESTING` is on. From the obs-studio root:

```
cmake -S . -B build_x64 -DBUILD_TESTING=ON
cmake --build build_x64 --config RelWithDebInfo --target sl-vst-save-while-processing-test
ctest --test-dir build_x64 -C RelWithDebInfo -L sl-vst --output-on-failure
```

A passing run prints `PASSED: audio kept processing during save`. You can also run `build_x64/plugins/sl-vst/RelWithDebInfo/sl-vst-save-while-processing-test.exe` directly, but `ctest` adds the libobs and FFmpeg DLL folders to `PATH` for you.

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
