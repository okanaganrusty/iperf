# iperf3 with DPDK Support

This is a modified version of iperf3 that uses Intel's Data Plane Development Kit (DPDK) instead of traditional Linux sockets for network I/O. This enables high-performance packet processing with kernel bypass.

## Overview

The DPDK integration replaces the standard Linux socket API with DPDK's direct NIC access capabilities, allowing for:
- Kernel bypass for significantly reduced latency
- Higher packet rates (millions of packets per second)
- Direct memory access to NIC buffers
- CPU-efficient packet processing

## Prerequisites

### System Requirements
- Linux kernel 3.16 or later
- x86_64 or ARM64 architecture
- At least 2GB of hugepage memory
- DPDK-compatible network interface card

### Software Dependencies
- DPDK 20.11 or later (recommended: latest LTS version)
- pkg-config
- Standard build tools (gcc, make, autoconf, automake, libtool)

### Install DPDK

#### On Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y dpdk dpdk-dev pkg-config
sudo apt-get install -y build-essential autoconf automake libtool
```

#### From Source:
```bash
# Download DPDK
wget https://fast.dpdk.org/rel/dpdk-22.11.1.tar.xz
tar xf dpdk-22.11.1.tar.xz
cd dpdk-22.11.1

# Build and install
meson build
cd build
ninja
sudo ninja install
sudo ldconfig
```

## Building iperf3 with DPDK

### Configure Hugepages

DPDK requires hugepages for efficient memory management:

```bash
# Allocate 1GB of hugepage memory (adjust as needed)
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Mount hugepages
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge

# Make persistent across reboots (optional)
echo "nodev /mnt/huge hugetlbfs defaults 0 0" | sudo tee -a /etc/fstab
```

### Bind Network Interface to DPDK

```bash
# Find your network interface
ip link show

# Bind to DPDK (example with interface eth1)
sudo dpdk-devbind.py --status
sudo dpdk-devbind.py --bind=vfio-pci eth1

# To unbind later:
# sudo dpdk-devbind.py --bind=<original_driver> eth1
```

### Build iperf3-dpdk

```bash
cd /root/iperf-dpdk

# Generate configure script
./bootstrap.sh

# Configure with DPDK support
./configure --with-dpdk

# Build
make

# Install (optional)
sudo make install
```

## Running iperf3 with DPDK

### Basic Usage

#### Server Mode:
```bash
sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip 192.168.1.100 \
    -- -l 0 -n 4 --proc-type=primary
```

#### Client Mode:
```bash
sudo ./src/iperf3 -c 192.168.1.100 --dpdk --dpdk-port 0 --dpdk-ip 192.168.1.101 \
    -- -l 0 -n 4 --proc-type=primary
```

### Command-Line Options

#### DPDK-Specific Options:
- `--dpdk`: Enable DPDK mode
- `--dpdk-port <N>`: Specify DPDK port ID (default: 0)
- `--dpdk-ip <IP>`: Specify IP address for the DPDK interface

#### DPDK EAL Parameters (after `--`):
Common EAL (Environment Abstraction Layer) parameters:
- `-l <cores>`: Logical cores to use (e.g., `-l 0-3` or `-l 0,2,4`)
- `-n <channels>`: Number of memory channels (typically 2 or 4)
- `--proc-type <type>`: Process type (`primary` or `secondary`)
- `--file-prefix <prefix>`: Prefix for shared memory files
- `--huge-dir <dir>`: Directory where hugepages are mounted

### Examples

#### TCP Performance Test:
```bash
# Server
sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip 10.0.0.1 \
    -- -l 0-1 -n 4

# Client
sudo ./src/iperf3 -c 10.0.0.1 -t 30 --dpdk --dpdk-port 0 --dpdk-ip 10.0.0.2 \
    -- -l 2-3 -n 4
```

#### UDP Bandwidth Test:
```bash
# Server
sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip 10.0.0.1 \
    -- -l 0 -n 4

# Client (1 Gbps UDP)
sudo ./src/iperf3 -c 10.0.0.1 -u -b 1G --dpdk --dpdk-port 0 --dpdk-ip 10.0.0.2 \
    -- -l 1 -n 4
```

#### Parallel Streams:
```bash
# Client with 4 parallel streams
sudo ./src/iperf3 -c 10.0.0.1 -P 4 --dpdk --dpdk-port 0 --dpdk-ip 10.0.0.2 \
    -- -l 0-3 -n 4
```

## Architecture

### Key Components

1. **dpdk_net.h / dpdk_net.c**: Core DPDK networking layer
   - Packet buffer management
   - Connection tracking
   - TX/RX packet processing
   - Socket-like API implementation

2. **socket_wrapper.h**: Transparent socket API redirection
   - Redirects standard socket calls to DPDK equivalents
   - Enabled via `HAVE_DPDK` macro

3. **Modified Core Files**:
   - `iperf.h`: Added DPDK configuration fields
   - `main.c`: DPDK initialization and cleanup
   - `net.c`, `iperf_tcp.c`, `iperf_udp.c`: Now use wrapper macros

### Connection Management

The DPDK implementation maintains a connection table that maps file descriptors to DPDK connection structures. Each connection includes:
- TCP/UDP state machine
- Packet queues (TX/RX)
- Reassembly buffers
- Address information

### Packet Flow

1. **RX Path**:
   - `dpdk_rx_burst()` polls NIC for packets
   - Packets are parsed and routed to connections
   - Data is buffered for application read

2. **TX Path**:
   - `dpdk_send()` creates packets with headers
   - Packets are queued in connection TX ring
   - `dpdk_tx_burst()` transmits queued packets

## Performance Considerations

### CPU Affinity
Use core isolation and affinity for best performance:
```bash
# Isolate cores 2-7 for DPDK (add to kernel boot parameters)
isolcpus=2-7

# Run iperf3 on isolated cores
sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip 192.168.1.1 \
    -- -l 2-3 -n 4
```

### Memory Configuration
- Allocate sufficient hugepages (recommended: at least 1GB per DPDK process)
- Use appropriate memory channel configuration (`-n` parameter)

### NIC Configuration
- Enable RSS (Receive Side Scaling) for multi-queue support
- Configure appropriate MTU size
- Disable offload features if causing issues

## Troubleshooting

### Common Issues

1. **"Cannot find hugepages"**
   - Ensure hugepages are allocated and mounted
   - Check: `grep Huge /proc/meminfo`

2. **"Cannot bind device"**
   - Verify device supports VFIO or UIO
   - Check IOMMU is enabled in BIOS
   - Try: `sudo modprobe vfio-pci`

3. **"Primary process already running"**
   - Either kill existing DPDK processes or use different `--file-prefix`
   - Check: `ls -la /var/run/dpdk/`

4. **"No packets transmitted/received"**
   - Verify MAC addresses are configured correctly
   - Check ARP tables and routing
   - Enable promiscuous mode on the NIC

### Debug Mode

Enable debug output:
```bash
sudo ./src/iperf3 -c <server> --dpdk --dpdk-port 0 --dpdk-ip <ip> --debug \
    -- -l 0 -n 4 --log-level=8
```

## Limitations

Current implementation limitations:
1. **Simplified TCP State Machine**: Basic TCP implementation; advanced features like congestion control may not work as expected
2. **No SCTP Support**: DPDK mode does not support SCTP protocol
3. **Single Port**: Currently supports one DPDK port at a time
4. **No IPv6**: Currently only IPv4 is implemented
5. **Simplified Connection Tracking**: Limited number of simultaneous connections (configured by `DPDK_MAX_CONNECTIONS`)

## Performance Comparison

Expected performance improvements over standard sockets:
- **Latency**: 50-80% reduction
- **Small Packet Rate**: 2-10x improvement
- **CPU Efficiency**: 40-60% less CPU usage at high packet rates

Actual performance depends on:
- Hardware capabilities (CPU, NIC, memory)
- Network configuration
- Packet size and pattern
- System tuning

## Contributing

When contributing DPDK-related changes:
1. Ensure code compiles with and without `--with-dpdk`
2. Test both socket and DPDK modes
3. Update this documentation for new features
4. Follow existing code style

## References

- [DPDK Documentation](https://doc.dpdk.org/)
- [iperf3 Original Project](https://github.com/esnet/iperf)
- [DPDK Getting Started Guide](https://doc.dpdk.org/guides/linux_gsg/index.html)

## License

This DPDK integration maintains the original iperf3 BSD-style license.

## Support

For DPDK-specific issues, please provide:
- DPDK version (`dpdk-devbind.py --version`)
- NIC model and driver
- Full command line used
- Error messages or unexpected behavior
- System specifications
