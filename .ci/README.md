# Local CI/CD

Builds the AURORA images on this machine and publishes them. Nothing here runs on GitHub: GitHub only executes
workflows under `.github/workflows/`, and this one lives in `.ci/`.

| Image | Docker Hub repo | Source |
|-------|-----------------|--------|
| gNB, DU, CU, CU-CP, CU-UP (FHI 7.2, DPDK, AVX512) | `24whitedevil/netsys-ocudu-gnb-fhi72-aurora` | `docker/Dockerfile`, target `runtime-dpdk`, `COMPONENT="gnb du cu cu-cp cu-up"` (binaries `gnb`, `odu`, `ocu`, `ocucp`, `ocuup`) |
| aurora_reader sidecar | `24whitedevil/netsys-ocudu-aurora-reader-fhi72` | `docker/aurora_reader/Dockerfile` |

Each image is tagged `:<10-char commit SHA>` and `:<branch>` (`/` in branch names becomes `-`).

## Usage

Run this instead of `git push`:

```bash
.ci/run.sh          # build both images -> push them to Docker Hub -> git push origin aurora-port
.ci/run.sh --list   # show the jobs only
```

There is no dry run: act's `-n` still executes `run:` steps when jobs run on the host, so it would build and push.

1. Both images are built (in parallel).
2. Only if both builds succeed are they pushed to Docker Hub.
3. Only if the push succeeds, and the current branch is `aurora-port`, the branch is pushed with `git push origin
   aurora-port`. On other branches the images are still published but the code is not pushed.

The script refuses to run with uncommitted changes to tracked files, because the images are tagged with the HEAD SHA.

## Requirements

- [`act`](https://github.com/nektos/act) and Docker on the host.
- The host Docker logged in to Docker Hub as `24whitedevil` (`docker login`). Jobs run on the host, so they use that
  login; no secrets file is needed.

## Settings

Edit the `env:` block of `aurora-images.yml`:

- `MARCH`: CPU target of the gNB server. `x86-64-v4` needs AVX512; a wrong value crashes the gNB with "illegal
  instruction".
- `NUM_JOBS`: parallel compile jobs of the gNB build. Each needs about 2 GB of RAM.
- `DPDK_VERSION`: DPDK built into the gNB image.
