# Station integration patch series

This branch preserves local broadcast-station changes on CasparCG v2.5.1-stable
(ec38d94442387c95f05c816e66e6719c2e9e197f). It is a review branch, not an upstream
release or a claim that all changes are ready to merge. Upstream GPL terms and
notices are retained. No proprietary NDI runtime, station configuration, music
library, camera captures or credentials are included.

## Review sequence

Each change has a separate commit. Later changes may depend on earlier ones,
especially the NDI chain and graphics-cache work. Review in order before
cherry-picking. Native source matches the checksum-verified source/patch build.

| Patch | Commit | Change |
|---|---|---|
| owned-readback.patch | d33e7821669a | Keep BGRA readback storage alive through FFmpeg frame ownership |
| independent-audio.patch | e496710450a1 | Decouple FFmpeg audio delivery from video queuing |
| monitor-snapshot.patch | 88a0aaed9b25 | Expose a bounded program monitor snapshot |
| ndi-bounded-output.patch | 9981ddf9175f | Bound pending NDI video output |
| ndi-independent-audio.patch | d32e34033b0e | Deliver NDI audio independently of video conversion |
| ndi-program-scale.patch | 85c5a21f0596 | Allow scaled NDI program monitoring |
| ndi-readback-release.patch | d0b9f9d1e0b6 | Release program readback after NDI video conversion |
| delivered-video-counter.patch | 42dfb45cbee2 | Report delivered video frame counters |
| bounded-video-queue.patch | 04c759e97278 | Bound the FFmpeg output video queue |
| bounded-graphics-cache.patch | a3fec38155c1 | Bound retained graphics resources |
| cef-fork-stack.patch | 11a536b80a85 | Avoid inherited stack-canary checks across CEF fork interception |
| ndi-library-lifetime.patch | 331c7406d9a7 | Keep the NDI library loaded for its consumers |
| ndi-box-reduction.patch | 5fb4b617bb75 | Use a three-to-one box filter for NDI confidence pictures |
| live-camera-latest.patch | 75ca93fa24c7 | Prefer recent live camera frames under decode pressure |
| camera-keyframe-recovery.patch | d91619cfddcf | Recover damaged live camera decoding at a keyframe |
| idle-graphics-cache.patch | f773bbef5f18 | Budget idle OpenGL buffers and textures without evicting active owners |

## Checks

The station-regressions workflow compiles the idle-cache and NDI reduction
tests without a GPU or NDI runtime. A separate Linux compile job checks the
engine against the upstream CEF package. These checks do not qualify a live
HDMI/encoder/audio path. No binaries are published by this workflow.

Before an upstream PR, reproduce the relevant defect against current upstream,
remove station-specific policy, compare against recent upstream fixes, add a
focused regression test and document measured resource/timing changes. NDI
needs its own supported runtime and listening tests. The stack-canary change
needs upstream security review. The output queue and live-camera policies need
audio/video sync and failure-recovery review. Do not submit the complete series
as one unreviewed PR.
