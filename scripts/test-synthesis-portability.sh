#!/usr/bin/env bash
# Run the shipped executable on the host and on x86-64 without AVX. No models,
# credentials, network, or persistent resources are needed for these probes.
set -euo pipefail
image=${1:?usage: test-synthesis-portability.sh IMAGE}
qemu=${AIMEE_TEST_QEMU_X86_64:-/usr/bin/qemu-x86_64-static}
[[ -x $qemu ]] || { echo 'qemu-user-static is required for the CPU portability gate' >&2; exit 1; }
llama=/opt/aimee/llama.cpp/llama-server
for probe in --version --list-devices; do
    timeout 60 docker run --rm --network none --entrypoint "$llama" "$image" "$probe"
    timeout 60 docker run --rm --network none \
        --mount "type=bind,src=$qemu,dst=/aimee-test-qemu,readonly" \
        --entrypoint /aimee-test-qemu "$image" -cpu Nehalem "$llama" "$probe"
done
echo 'synthesis CPU portability: host and Nehalem passed'
