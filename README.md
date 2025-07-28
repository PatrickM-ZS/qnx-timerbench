## QNX timer benchmark

Test to investigate QNX timing behaviour and benchmark various clock functions.

How to build:

`cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=qnx-sdp710-aarch64le.cmake -DCMAKE_PREFIX_PATH=... && cmake --build build`

Note: set `CMAKE_PREFIX_PATH` (or `CycloneDDS_DIR`) to a directory with a CycloneDDS built for QNX.

Note: the high-resolution sleep (`dds_hr_sleepfor()`) function changes QNX timer tolerance which requires a special privilege. You're best off running it as `root` user.

Usage: 
```
# ./TimerBench

```

