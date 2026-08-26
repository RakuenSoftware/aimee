# Where the numbers were produced

Two hosts. Neither is a clean-room; both are machines that do other work, and
that shows up in the timings.

## .253: Proxmox host, LXC 140 `aimee-tiera-bench`

| | |
| --- | --- |
| GPU | NVIDIA RTX 5080, 16GB |
| CPU | 20 cores allocated to the container |
| RAM | 48GB |
| storage | Optane-backed ZFS, 240GB to the container |
| backend | CUDA |
| llama.cpp | build 11, commit `0005475` |
| quantisation | Q8_0 throughout |

The container was created on Optane storage per the operating constraint that
nothing modifies the Proxmox host itself.

16GB is the binding constraint on model choice. A 30B-A4B MoE fits by routing
expert tensors to CPU (`-ot ".ffn_.*_exps.=CPU"`); a 27B or 32B dense model at
Q8_0 does not fit at all, and llama.cpp's auto-fit silently serves it from CPU
instead. That produced a directory named `gpu` full of CPU runs. See
MEASUREMENT_LOG.md defect 15.

## .254: SmoothNAS, bare metal

| | |
| --- | --- |
| GPU | AMD Radeon RX 7900 XTX, 24GB (RADV NAVI31) |
| second GPU | PHOENIX iGPU, 8GB: masked with `GGML_VK_VISIBLE_DEVICES=1` |
| CPU | 16 cores |
| RAM | 14GB total, ~3GB free |
| storage | `/mnt/media`, 43TB free |
| backend | Vulkan (RADV) |
| llama.cpp | build 10210, commit `0005475`: the same commit as .253 |
| quantisation | Q6_K, and Q5_K_M for 32B dense |

Runs unprivileged from prebuilt binaries and a userspace venv on `/mnt/media`.

Two constraints shape what can run here. 24GB of VRAM is more than .253 has, but
3GB of free system RAM is far less, so there is no room to offload experts: a
model must fit the card whole. And fitting a 24B-32B dense model whole means
dropping below Q8_0, which is why the quantisation column differs between hosts.

The iGPU matters. `--list-devices` reports it alongside the 7900 XTX, and left
visible it takes layers and wrecks both fit and throughput without saying so.

## What is comparable to what

`.253` and `.254` numbers are **not** directly comparable, and the first attempt
to measure the gap was itself invalid.

`gemma-4-12B` was run in three configurations to quantify it, producing
quantisation +0.0112 and backend +0.0203 for a combined ~0.03. **Those figures
are withdrawn.** The `.254` sweep passed neither `--thinking` nor
`--max-tokens`, so it ran with reasoning suppressed against the runner's default
512-token cap, while the `.253` side ran with reasoning on at 2048. The
comparison changed four variables, not two. See MEASUREMENT_LOG.md defect 24.

The control study is being redone with both sides at `--thinking
--max-tokens 8192`. Until it lands, the honest statement is that the
cross-host correction is **unknown**, not that it is 0.03.

What remains true and is worth keeping: a control was necessary, and it caught
this. A challenger "beating Gemma 4" on a different host, quantisation and
backend means nothing without one.

## Timings are contended and should not be quoted

Both hosts ran other work. On .253, a CPU-lane model was served alongside the
GPU ladder on the assumption that the two would not contend; an MoE with its
experts on CPU is a CPU workload, so they did, and load average hit 39.6 on 20
cores. `gemma-4-26B-A4B` fell from 27.32 tok/s to 3.03 (defect 19). Every
latency figure taken during that window is a contended figure.

Accuracy is unaffected by any of this. The same GGUF produces the same output
wherever its tensors sit, which the E4B llama.cpp-versus-transformers control
established.
