# Linux tuning for the latency numbers

The engine spins on dedicated cores. These settings let it actually have them.
Without them, p50 barely moves, but p99.9 measures the scheduler, not the
code.

```bash
# 1. Isolate cores 2-4 from the scheduler, timer ticks and RCU callbacks (kernel cmdline, reboot):
#    isolcpus=2-4 nohz_full=2-4 rcu_nocbs=2-4
# 2. Pin: feed, engine, writer
hft-engine --input unix:/tmp/hft-gateway.sock --pin 2,3,4 ...
# 3. Keep the CPU out of deep sleep and at a fixed frequency
sudo cpupower frequency-set -g performance
echo 0 | sudo tee /sys/devices/system/cpu/cpu{2,3,4}/cpuidle/state*/disable  # or idle=poll for benchmarks only
# 4. Move IRQs off the isolated cores
for irq in /proc/irq/*/smp_affinity_list; do echo 0-1 | sudo tee $irq; done 2>/dev/null
# 5. No transparent huge page compaction stalls
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/defrag
# 6. Check isolation took effect
cat /sys/devices/system/cpu/isolated
```

| Setting | What it removes from the tail |
|---|---|
| `isolcpus` + pinning | other processes and kernel threads preempting the engine |
| `nohz_full` | the 1-4 ms scheduler tick interrupting a spinning core |
| `performance` governor, C-states off | frequency ramp-up and wake-from-sleep (tens of µs) |
| IRQ affinity | NIC and disk interrupts landing on the hot core |
| THP defrag off | multi-ms compaction stalls on allocation (the engine doesn't allocate after startup, but the kernel can still stall the page fault path) |

**Kernel bypass** (stretch goal): with a Solarflare/Xilinx NIC, `onload` runs the
gateway's socket stack in user space, removing the kernel network path. That
needs a direct exchange feed. Binance's public WebSocket is several
milliseconds away regardless, so this project simulates it by measuring the
engine from the Unix socket inward.

macOS has no hard affinity or core isolation. `--pin` is a no-op there, and
benchmark numbers from a laptop are for regression tracking, not for a CV.
