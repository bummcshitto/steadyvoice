# SteadyVoice 1.5.0 download

[Download the Windows x64 ZIP](https://github.com/bummcshitto/steadyvoice/raw/refs/heads/steadyvoice-1.5.0-download/SteadyVoice-1.5.0-Windows-x64.zip)

If the link opens a file page, use **Download raw file**. This repository is
private, so sign in with an account that has access.

The archive includes the Windows DLL, complete source, build recipes, licenses,
quick start, measured validation records, and an optional synthetic test host.
Unload the old plugin before replacing its DLL. In Equalizer APO, preserve the
working microphone routing, including Copy: R=L after the plugin where needed.

Select **Quality voice** for Adaptive guard On, 8× peak detection, and 5 ms
lookahead. Total delay is 6.3125 ms at 48 kHz, or 5.328125 ms at 192 kHz.
The new options remain disabled in migrated older settings. Peak detection is
selectable; the Instance rate readout does not change Windows/host capture format.

Estimated oversampled protection uses 0.9 / 0.8 dB internal headroom at 4× / 8×
and at least 5 ms guard lookahead. The native sample ceiling remains enforced
with Guard On and Bypass Off. Arbitrary-waveform or downstream encoded true-peak
limits are not guaranteed. Read the included validation record for test results
and measured CPU limits.

The archive SHA-256 is provided in the adjacent .sha256 file. This separate
download branch leaves main unchanged.
