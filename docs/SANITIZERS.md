# Memory and undefined-behavior checks

All six DSP/quality executables and an instrumented actual-plugin host passed
AddressSanitizer, UndefinedBehaviorSanitizer, and float divide-by-zero checking
against the final 1.5.0 source. Host checks also verified that processing and
sample-rate/parameter reconfiguration allocate no C++ heap memory.

Flags: -std=c++17 -O1 -g -fsanitize=address,undefined,float-divide-by-zero
-fno-omit-frame-pointer. Both plugin and host were instrumented; the Linux
plugin retained -shared -fPIC and the host retained -rdynamic -ldl -pthread.

ASAN_OPTIONS=detect_leaks=0 was required because LeakSanitizer's process
inspection is blocked in this ptrace-managed environment. These checks do not
establish leak freedom. Full suite counts and logs are included beside the
validation record in logs/.
