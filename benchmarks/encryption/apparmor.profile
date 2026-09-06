# SPDX-FileCopyrightText: Copyright The Moby Authors
# SPDX-License-Identifier: Apache-2.0
# Rendered from https://github.com/moby/profiles/blob/main/apparmor/template.go
# Dedicated benchmark profile; ABI 3 preserves the upstream Unix-socket rule.
abi <abi/3.0>,
#include <tunables/global>
profile aimee-encryption-benchmark flags=(attach_disconnected,mediate_deleted) {
  #include <abstractions/base>
  network,
  deny network alg,
  deny network vsock,
  capability,
  file,
  umount,
  signal (receive) peer=unconfined,
  signal (receive) peer=runc,
  signal (receive) peer=crun,
  signal (send,receive) peer=aimee-encryption-benchmark,
  deny @{PROC}/* w,
  deny @{PROC}/{[^1-9/],[^1-9/][^0-9/],[^1-9s/][^0-9y/][^0-9s/],[^1-9/][^0-9/][^0-9/][^0-9/]*}/** w,
  deny @{PROC}/sys/[^k]** w,
  deny @{PROC}/sys/kernel/{?,??,[^s][^h][^m]**} w,
  deny @{PROC}/sysrq-trigger rwklx,
  deny @{PROC}/kcore rwklx,
  deny mount,
  deny /sys/[^f]*/** wklx,
  deny /sys/f[^s]*/** wklx,
  deny /sys/fs/[^c]*/** wklx,
  deny /sys/fs/c[^g]*/** wklx,
  deny /sys/fs/cg[^r]*/** wklx,
  deny /sys/firmware/** rwklx,
  deny /sys/devices/virtual/powercap/** rwklx,
  deny /sys/kernel/security/** rwklx,
  ptrace (trace,tracedby,read,readby) peer=aimee-encryption-benchmark,
}
