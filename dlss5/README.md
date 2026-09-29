# DLSS5 / gfx12 compiler branch

This branch establishes an independently built AMDGPU compiler for the
DLSSNR-on-RDNA4 project. The first experimental optimization is a default-off
post-RA VOPD lookahead in `GCNCreateVOPD.cpp`.

## Baseline

- Public AMD LLVM base: `6d585d872fbd3c594da7a3c09ac9b22eef4167f6`.
- This is the last LLVM 21 commit on the public `amd-staging` first-parent
  history before the LLVM 22 version merge, not a claim of exact correspondence
  with AMD's internal driver compiler.
- The deployed Windows COMGR 3.0 compiler identifies itself as internal commit
  `590b9320a5be90e40268759c6203c01fde121e68` (Clang/LLD 21.0.0git).
- Checked public release tags: ROCm 7.0.2 and 7.1.1 report LLVM 20; ROCm 7.2.4
  reports LLVM 22. Those labels alone cannot select the required LLVM 21 source.

## Reproduction pipeline

The canonical build scripts and evidence are maintained with the kernel recipes:

- [Build and module tools](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting/tree/main/Development/tools/llvm-fork)
- [2026-09-29 evidence](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting/tree/main/Development/results/llvm-fork-20260929)

On an aarch64 DGX Spark, the native Release build enables only the AMDGPU target
and Clang/LLD projects. Modules follow the observed COMGR pipeline: HIP to
optimized bitcode, bitcode to object, LLD shared ELF; Windows x86_64 auxiliary
ABI, C++14 and no device libraries are explicit. All 30 canonical recipes build
for both gfx1201 and gfx1200. Build artifacts stay outside Git.

Acceptance is output equality against the approved float-FMA production
baseline, not identity with the original NVIDIA half arithmetic and not
machine-code byte identity. gfx1201 is tested on the RX 9070 XT; gfx1200 is
compile/static-analysis only. See the evidence directory for final regression
results and per-kernel code/resource differences.

## First patch policy

`-mllvm -amdgpu-dlss5-vopd-lookahead=4` searches for independent nearby VOPD
partners on gfx1200/gfx1201 wave32 only. Default `0` preserves the ordinary
adjacent-only pass. The opt-in pass stops at memory, WMMA/MFMA, inline assembly,
bundles and synchronization, checks physical register aliases and implicit
dependencies, and clears obsolete kill flags. The original downstream passes
recompute waits and hazards. Strict-FP functions retain the original behavior.

Two MIR/FileCheck tests cover the enable/disable and target gates, dependencies,
aliasing, kill flags, boundaries and search distance. Default-off rebuilding
matches all 60 previous public21 module code/metadata sections. Enabled modules
pass all 168 EXACT/AE golden frames, with identical adaptive decisions.

The 2026-09-29 measurements did **not** meet the 0.5% whole-network gate against
the deployed driver compiler: 900p improved 0.15–0.36%, 1080p regressed
0.03–0.15%. It remains an experimental switch, not a production default.
[Evidence and reproduction](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting/tree/main/Development/results/llvm-patch1-20260929).

No blanket fast-math or silent change to rounding, non-finite values or signed
zero is permitted. Existing source-level arithmetic workarounds were not changed.
