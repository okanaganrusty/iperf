# Quick Start Guide: iperf3 with DPDK

This guide provides a fast-track setup for running iperf3 with DPDK support.

## Prerequisites Check

```bash
# Check if DPDK is installed
pkg-config --exists libdpdk && echo "DPDK found" || echo "DPDK not found"

# Check DPDK version
pkg-config --modversion libdpdk

# Check for hugepages
grep Huge /proc/meminfo
```

## 5-Minute Setup

### 1. Install DPDK (if not already installed)

**Ubuntu/Debian:**
```bash
sudo apt-get update
sudo apt-get install -y dpdk dpdk-dev pkg-config python3-pyelftools
```

### 2. Configure System

```bash
# Setup hugepages (requires root)
sudo mkdir -p /mnt/huge
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mount -t hugetlbfs nodev /mnt/huge

# Verify hugepages
grep HugePages_Total /proc/meminfo
```

### 3. Bind Network Interface

```bash
# Find your network interfaces
ip link show

# Load VFIO driver
sudo modprobe vfio-pci

# Bind interface to DPDK (replace enp0s8 with your interface)
sudo dpdk-devbind.py --bind=vfio-pci enp0s8

# Verify binding
sudo dpdk-devbind.py --status
```

**Important:** You'll lose network connectivity on the bound interface!

### 4. Build iperf3

```bash
cd /root/iperf-dpdk

# Quick build with DPDK
./build-dpdk.sh --with-dpdk

# Or manual build
./bootstrap.sh
./configure --with-dpdk
make -j$(nproc)
```

### 5. Run Tests

**On Server (Machine A):**
```bash
# Set IP for your setup
SERVER_IP="192.168.1.100"

# Run iperf3 server with DPDK
sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip $SERVER_IP \
    -- -l 0 -n 4 --proc-type=primary
```

**On Client (Machine B):**
```bash
# Set IPs for your setup
SERVER_IP="192.168.1.100"
CLIENT_IP="192.168.1.101"

# Run iperf3 client with DPDK
sudo ./src/iperf3 -c $SERVER_IP --dpdk --dpdk-port 0 --dpdk-ip $CLIENT_IP \
    -- -l 1 -n 4 --proc-type=primary
```

## Common Test Scenarios

### TCP Throughput Test
```bash
# Client
sudo ./src/iperf3 -c $SERVER_IP -t 10 --dpdk --dpdk-port 0 --dpdk-ip $CLIENT_IP \
    -- -l 0 -n 4
```

### UDP Bandwidth Test (1 Gbps)
```bash
# Client
sudo ./src/iperf3 -c $SERVER_IP -u -b 1G --dpdk --dpdk-port 0 --dpdk-ip $CLIENT_IP \
    -- -l 0 -n 4
```

### Parallel Streams (4 streams)
```bash
# Client
sudo ./src/iperf3 -c $SERVER_IP -P 4 --dpdk --dpdk-port 0 --dpdk-ip $CLIENT_IP \
    -- -l 0-3 -n 4
```

### Reverse Mode Test
```bash
# Client
sudo ./src/iperf3 -c $SERVER_IP -R --dpdk --dpdk-port 0 --dpdk-ip $CLIENT_IP \
    -- -l 0 -n 4
```

## Running Without DPDK

To use standard socket mode (no DPDK):

```bash
# Build without DPDK
./configure
make

# No need for hugepages or special NIC binding
./src/iperf3 -s              # Server
./src/iperf3 -c <server_ip>  # Client
```

## Troubleshooting Quick Fixes

### "Cannot allocate memory"
```bash
# Increase hugepages
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### "Cannot find DPDK device"
```bash
# Check DPDK device status
sudo dpdk-devbind.py --status

# Rebind if needed
sudo dpdk-devbind.py --bind=vfio-pci <device>
```

### "Permission denied"
```bash
# DPDK requires root or capabilities
sudo ./src/iperf3 ...

# Or set capabilities (advanced)
sudo setcap cap_sys_rawio,cap_net_raw,cap_sys_admin+eip ./src/iperf3
```

### "Primary process conflict"
```bash
# Clean up shared memory
sudo rm -rf /var/run/dpdk/rte/*

# Or use different file prefix
sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip $IP \
    -- -l 0 -n 4 --file-prefix=iperf3_server
```

## Unbinding Network Interface

When done testing, restore your network interface:

```bash
# Find original driver (e.g., e1000, ixgbe, i40e)
sudo dpdk-devbind.py --status

# Unbind and restore
sudo dpdk-devbind.py --bind=<original_driver> <device>

# For example:
sudo dpdk-devbind.py --bind=e1000 enp0s8

# Restart networking
sudo systemctl restart networking
```

## Performance Tips

### For Best Performance

1. **Isolate CPUs:**
   ```bash
   # Add to kernel boot parameters (edit /etc/default/grub)
   isolcpus=1-3 nohz_full=1-3 rcu_nocbs=1-3

   # Then update grub and reboot
   sudo update-grub
   sudo reboot
   ```

2. **Use Dedicated Cores:**
   ```bash
   # Use isolated cores for DPDK
   sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip $IP \
       -- -l 1-2 -n 4
   ```

3. **Disable Power Saving:**
   ```bash
   # Set CPU governor to performance
   echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
   ```

4. **Large Hugepages (if available):**
   ```bash
   # Use 1GB hugepages if supported
   echo 2 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages

   # Add to DPDK command
   sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip $IP \
       -- -l 0 -n 4 --huge-unlink
   ```

## Next Steps

- Read [DPDK_README.md](DPDK_README.md) for detailed documentation
- Review [DPDK_CONVERSION_SUMMARY.md](DPDK_CONVERSION_SUMMARY.md) for technical details
- Check [DPDK documentation](https://doc.dpdk.org/) for advanced tuning

## Support

If you encounter issues:

1. Check DPDK status: `sudo dpdk-devbind.py --status`
2. Verify hugepages: `grep Huge /proc/meminfo`
3. Check logs: Add `--debug` flag and DPDK `--log-level=8`
4. Review dmesg: `dmesg | tail -50`

## Example: Complete Two-Machine Setup

**Machine A (10.0.0.1) - Server:**
```bash
# Setup
sudo modprobe vfio-pci
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo dpdk-devbind.py --bind=vfio-pci eth1

# Run
cd /root/iperf-dpdk
sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip 10.0.0.1 \
    -- -l 0 -n 4 --proc-type=primary
```

**Machine B (10.0.0.2) - Client:**
```bash
# Setup
sudo modprobe vfio-pci
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo dpdk-devbind.py --bind=vfio-pci eth1

# Run
cd /root/iperf-dpdk
sudo ./src/iperf3 -c 10.0.0.1 -t 30 --dpdk --dpdk-port 0 --dpdk-ip 10.0.0.2 \
    -- -l 0 -n 4 --proc-type=primary
```

This should give you working DPDK-accelerated network testing between two machines!
