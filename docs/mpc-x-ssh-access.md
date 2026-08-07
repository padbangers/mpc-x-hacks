---
name: mpc-x-ssh-access
description: "SSH access details for the user's Akai MPC X hardware on the LAN"
metadata: 
  node_type: memory
  type: project
  originSessionId: cefc650b-bdda-417b-9236-830a6cdc55f3
  modified: 2026-08-06T05:31:55.516Z
---

The user's Akai MPC X is reachable over SSH on the LAN at `192.168.178.58` (port 22, OpenSSH 9.6). Login is user `root` (uid=0) with key-based auth already trusted from this machine — no password needed. Device hostname is `mpc-x`, running Linux 6.18.x PREEMPT_RT (realtime kernel) on `armv7l` (32-bit ARM, Buildroot-style firmware).

An SSH alias `mpcx` is configured in `~/.ssh/config` → connect with `ssh mpcx`.

Home LAN is a Fritz!Box network `192.168.178.0/24` (gateway `192.168.178.1`). The MPC X Hacks project dir is currently empty — work is just starting.
