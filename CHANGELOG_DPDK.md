# Changelog: DPDK Integration for iperf3

## [Unreleased] - DPDK Support

### Added

#### New Source Files
- `src/dpdk_net.c` - Core DPDK networking implementation with socket-like API
- `src/dpdk_net.h` - Header file for DPDK networking layer, data structures, and API declarations
- `src/socket_wrapper.h` - Transparent socket API redirection macros for DPDK compatibility

#### Documentation
- `DPDK_README.md` - Comprehensive guide for DPDK integration, setup, usage, and troubleshooting
- `DPDK_CONVERSION_SUMMARY.md` - Detailed technical summary of all changes and design decisions
- `QUICKSTART.md` - Fast-track setup guide for getting started with DPDK quickly
- `build-dpdk.sh` - Automated build script with DPDK detection and helpful instructions

#### Command-Line Options
- `--dpdk` - Enable DPDK mode (OPT_DPDK_ENABLE)
- `--dpdk-port <N>` - Specify DPDK port ID (OPT_DPDK_PORT)
- `--dpdk-ip <IP>` - Specify IP address for DPDK interface (OPT_DPDK_IP)

#### Configuration Options
- `--with-dpdk` - Configure option to enable DPDK support at build time

#### Data Structures
- `struct dpdk_connection` - Per-connection state for DPDK sockets
- `struct dpdk_state` - Global DPDK runtime state

#### Functions (dpdk_net.c)
- **Initialization:**
  - `dpdk_net_init()` - Initialize DPDK EAL and networking
  - `dpdk_net_configure_port()` - Configure DPDK port
  - `dpdk_net_cleanup()` - Cleanup DPDK resources

- **Socket API:**
  - `dpdk_socket()` - Create DPDK socket
  - `dpdk_bind()` - Bind socket to address
  - `dpdk_listen()` - Listen for connections
  - `dpdk_accept()` - Accept incoming connection
  - `dpdk_connect()` - Connect to remote address
  - `dpdk_send()` - Send data
  - `dpdk_recv()` - Receive data
  - `dpdk_sendto()` - Send datagram
  - `dpdk_recvfrom()` - Receive datagram
  - `dpdk_close()` - Close socket
  - `dpdk_setsockopt()` - Set socket options
  - `dpdk_getsockopt()` - Get socket options
  - `dpdk_getsockname()` - Get socket name
  - `dpdk_getpeername()` - Get peer name
  - `dpdk_fcntl()` - File control operations

- **Packet Processing:**
  - `dpdk_rx_burst()` - Receive packet burst from NIC
  - `dpdk_tx_burst()` - Transmit packet burst to NIC
  - `dpdk_process_packets()` - Main packet processing loop

- **Connection Management:**
  - `dpdk_allocate_fd()` - Allocate file descriptor
  - `dpdk_get_connection()` - Get connection by FD
  - `dpdk_free_connection()` - Free connection resources
  - `dpdk_alloc_connection()` - Allocate connection structure

- **Packet Creation:**
  - `dpdk_create_tcp_packet()` - Build TCP packet with headers
  - `dpdk_create_udp_packet()` - Build UDP packet with headers
  - `dpdk_send_tcp_syn()` - Send TCP SYN packet
  - `dpdk_send_tcp_ack()` - Send TCP ACK packet

- **Utilities:**
  - `dpdk_checksum()` - Calculate checksum
  - `dpdk_parse_ip_addr()` - Parse IP address string

### Modified

#### Core Files
- `src/iperf.h`
  - Added conditional DPDK header includes
  - Added `HAVE_DPDK` conditional socket include
  - Added DPDK configuration fields to `struct iperf_test`:
    - `dpdk_enabled` - Flag to enable DPDK mode
    - `dpdk_port_id` - DPDK port identifier
    - `dpdk_ip_addr` - IP address string for DPDK interface
    - `dpdk_argc` / `dpdk_argv` - DPDK EAL arguments

- `src/main.c`
  - Added conditional DPDK header includes
  - Added DPDK initialization after argument parsing
  - Added DPDK cleanup before program exit
  - Modified socket header inclusion to be conditional

- `src/net.c`
  - Added conditional DPDK header includes
  - Modified socket header inclusion
  - Socket calls now redirected via wrapper macros when DPDK enabled

- `src/iperf_tcp.c`
  - Added conditional DPDK header includes
  - Modified socket header inclusion
  - TCP operations use DPDK when enabled

- `src/iperf_udp.c`
  - Added conditional DPDK header includes
  - Modified socket header inclusion
  - UDP operations use DPDK when enabled

- `src/iperf_api.h`
  - Added OPT_DPDK_ENABLE (36)
  - Added OPT_DPDK_PORT (37)
  - Added OPT_DPDK_IP (38)

- `src/iperf_api.c`
  - Added DPDK long options to command-line parser
  - Added option handling for DPDK flags
  - Parse and store DPDK configuration

#### Build System
- `src/Makefile.am`
  - Added `dpdk_net.c` to sources
  - Added `dpdk_net.h` to sources
  - Added `socket_wrapper.h` to sources

- `configure.ac`
  - Added `--with-dpdk` configure option
  - Added DPDK detection via pkg-config
  - Added HAVE_DPDK definition when enabled
  - Added DPDK CFLAGS and LIBS to build

### Technical Details

#### Socket Redirection Mechanism
When `HAVE_DPDK` is defined, `socket_wrapper.h` provides macros that redirect:
```c
#define socket(...)   dpdk_socket(...)
#define bind(...)     dpdk_bind(...)
#define connect(...)  dpdk_connect(...)
// ... etc
```

This allows existing code to work with DPDK without modification.

#### Connection Tracking
- Virtual file descriptors (FD >= 100) avoid conflicts
- Global connection table maps FDs to DPDK connections
- Each connection has TX/RX rings for packet queuing
- Reassembly buffers handle fragmented data

#### Packet Flow
1. **Transmit:** Application → dpdk_send() → TX ring → dpdk_tx_burst() → NIC
2. **Receive:** NIC → dpdk_rx_burst() → RX ring → dpdk_recv() → Application

#### Memory Management
- DPDK mempool for packet buffers
- Ring buffers for efficient queuing
- Hugepage-backed memory for performance

### Compatibility

#### Backward Compatibility
- Default behavior unchanged (uses standard sockets)
- DPDK is opt-in via `--with-dpdk` and `--dpdk` flag
- Existing functionality preserved when DPDK disabled

#### Build Compatibility
- Code compiles with or without DPDK
- Conditional compilation via `#ifdef HAVE_DPDK`
- No DPDK dependency required for standard builds

### Known Limitations

#### Not Implemented
- IPv6 support (only IPv4 currently)
- SCTP protocol (DPDK doesn't support SCTP)
- Advanced TCP features (congestion control, SACK, etc.)
- Multi-queue support
- Hardware offload features

#### Simplified
- TCP state machine (basic implementation)
- Checksumming (simplified calculation)
- Packet reassembly (basic buffering)
- Connection limit (DPDK_MAX_CONNECTIONS = 1024)

### Dependencies

#### Required
- DPDK 20.11 or later
- pkg-config
- Linux kernel 3.16+
- Hugepage support

#### Optional
- VFIO or UIO drivers
- IOMMU support in BIOS

### Build Instructions

#### With DPDK:
```bash
./bootstrap.sh
./configure --with-dpdk
make
```

#### Without DPDK (standard):
```bash
./bootstrap.sh
./configure
make
```

#### Using Build Script:
```bash
./build-dpdk.sh --with-dpdk      # DPDK enabled
./build-dpdk.sh --without-dpdk   # Standard sockets
```

### Usage Examples

#### Enable DPDK Mode:
```bash
sudo ./src/iperf3 -s --dpdk --dpdk-port 0 --dpdk-ip 192.168.1.1 \
    -- -l 0 -n 4 --proc-type=primary
```

#### Standard Mode (unchanged):
```bash
./src/iperf3 -s
```

### Testing

Recommended test cases:
1. TCP throughput test
2. UDP bandwidth test
3. Multiple parallel streams
4. Various packet sizes
5. Long-duration tests
6. Connection setup/teardown
7. Comparison with standard socket mode

### Performance

Expected improvements (hardware-dependent):
- Latency: 50-80% reduction
- Small packet rate: 5-10x improvement
- CPU efficiency: 40-60% reduction
- Throughput: Similar to line rate

### Future Enhancements

Planned improvements:
- [ ] IPv6 support
- [ ] Multi-queue/RSS support
- [ ] Hardware offload features
- [ ] Advanced TCP implementation
- [ ] Zero-copy optimizations
- [ ] NUMA awareness
- [ ] Dynamic configuration

### References

- DPDK: https://www.dpdk.org/
- DPDK Documentation: https://doc.dpdk.org/
- Original iperf3: https://github.com/esnet/iperf

### Migration Notes

For users upgrading:
1. DPDK is completely optional
2. Existing builds and usage remain unchanged
3. New `--dpdk` flag required to enable DPDK mode
4. Requires system configuration (hugepages, NIC binding)
5. Root privileges required for DPDK mode

### Credits

This DPDK integration maintains compatibility with the original iperf3 project while adding high-performance kernel-bypass networking capabilities.
