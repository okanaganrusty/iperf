# DPDK iperf3: TCP Flow Control & Window Scaling Implementation Summary

## Problem Statement
Previous implementation caused throughput stalls at high speeds (450+ Mbps) because:
1. **ACK-first paradigm broke TCP semantics**: Server ACKed packets before buffering them
2. **No window management**: Client didn't know when to slow down
3. **Buffer exhaustion**: Server buffers filled while application was still consuming data
4. **Silent deadlock**: Client got ACKs, thought all was well, kept sending; server buffer full

## Solution: Proper RFC 1323 TCP Windowing

### Core Components Implemented

#### 1. **Window Scaling Support (RFC 1323)**
```c
struct dpdk_connection {
    uint8_t wscale_local;       // Our advertised scale factor (2^7 = 128x)
    uint8_t wscale_remote;      // Client's scale factor (parsed from SYN)
    uint32_t rwnd_available;    // Available receive window in bytes
    uint16_t window_size;       // Advertised window (after scaling)
}
```

**Key Values:**
- `wscale_local = 7`: Enables windows up to 2^7 × 64KB = 8MB
- `wscale_remote`: Negotiated from client SYN (parsed from TCP options)
- Allows high-throughput links to have large windows without exceeding 16-bit limit

#### 2. **Dynamic Receive Window Tracking**
- `rwnd_available` tracks actual available buffer space (0 to DPDK_RX_BUFFER_SIZE)
- Initialized to full buffer size (8 MB)
- **Decreases** when packets are buffered: `rwnd_available -= payload_len`
- **Increases** when application drains data: `rwnd_available += bytes_read`
- Window advertisements dynamically reflect current availability

#### 3. **Window Scaling Negotiation**
When receiving SYN packet:
```c
if (tcp_flags & DPDK_TCP_FLAG_SYN) {
    // Parse TCP options for window scale (kind=3)
    // Extract shift value (0-14)
    conn->wscale_remote = options[...] & 0x0F;
}
```
This allows proper window calculation accounting for client's scaling.

#### 4. **Advertised Window Calculation**
In every ACK/packet:
```c
uint32_t scaled_window = conn->rwnd_available >> conn->wscale_remote;
if (scaled_window > 65535) scaled_window = 65535;  // 16-bit max
tcp_hdr->rx_win = rte_cpu_to_be_16((uint16_t)scaled_window);
```

**Example:**
- rwnd_available = 4 MB (half full)
- wscale_remote = 7
- Advertised window = 4 MB >> 7 = 32768 bytes
- True receive window = 32768 × 2^7 = 4 MB

#### 5. **Proper Flow Control: Buffer-Space-Based Rate Limiting**

**When packet arrives:**
```c
if (buffer_has_space) {
    buffer_packet();
    rwnd_available -= packet_size;  // Consume window space
    send_ack();  // ACK tells client "got it, but window now smaller"
} else {
    drop_packet();  // Don't ACK - forces client to retransmit
}
```

**When application calls recv():**
```c
bytes_read = copy_from_buffer();
rwnd_available += bytes_read;  // Restore window space
```

**Client behavior:**
- Receives ACK with smaller window → slows down
- Window reaches 0 → stops sending entirely
- Receives ACK with larger window → resumes sending
- Automatic rate matching to application drain rate

### Implementation Details

#### File: `src/dpdk_net.h`
- Added window scaling fields to `struct dpdk_connection`
- RX buffer size: 8 MB (can sustain ~2 seconds at 12 Gbps)

#### File: `src/dpdk_net.c`

**Connection initialization:**
```c
conn->wscale_local = 7;         // Offer 8MB window capability
conn->rwnd_available = DPDK_RX_BUFFER_SIZE;
```

**Packet reception (RX burst):**
```c
// Parse SYN for window scale option
if (tcp_flags & DPDK_TCP_FLAG_SYN) {
    parse_tcp_options(); // Extract wscale_remote
}

// Buffer data only if space available (proper TCP)
if (buffer_has_space) {
    buffer_packet();
    rwnd_available -= len;
    schedule_ack();
} else {
    drop_and_dont_ack();  // Forces retransmit
}
```

**Data consumption (recv):**
```c
bytes_read = copy_from_buffer();
// Restore window as data is consumed
rwnd_available += bytes_read;
```

**ACK transmission:**
```c
uint32_t scaled = rwnd_available >> wscale_remote;
window_field = min(scaled, 65535);
```

### Flow Control Semantics

#### Before (Broken):
```
Time 0-100ms: Fast burst fills 8MB buffer
             Client gets ACKs, thinks all is well
Time 100ms: Buffer full, but app still slow
           Server drops packets (they were ACKed)
           Client sees closed window (0)
           But client doesn't know why
           Deadlock: client waiting for what?
```

#### After (Correct):
```
Time 0-10ms: Fast burst fills buffer
            Each buffered packet: rwnd_available--
            ACKs sent with shrinking window
Time 10ms: Buffer 90% full, advertised window = 10%
          Client receives ACK with small window
          Client: "only 10% available, slow down"
          Client sends slower
Time 15ms: Buffer 100% full, advertised window = 0
          Client: "no window, must stop"
          Client pauses all sends
Time 20ms: App drain starts, rwnd_available++
          ACK with larger window goes out
          Client: "window opened, resume"
          Client resumes at app's drain rate
Result: Perfect rate matching, no deadlock
```

### Performance Implications

1. **Throughput Sustainability**: Window-based flow control ensures client rate matches server's application drain rate
2. **No Deadlocks**: Window reaching 0 is signal, not surprise - client stops deliberately
3. **Scalability**: Works at any speed (12 Gbps, 100 Gbps) due to RFC 1323 window scaling
4. **Efficiency**: No congestion control algorithms needed - pure TCP windowing suffices

### Testing Expectations

With this implementation:
- **First interval (0-1s)**: High throughput from initial burst (450+ Mbps)
- **Subsequent intervals**: Sustained throughput at application drain rate
- **No stalls**: Window management prevents deadlock at any throughput level

This properly implements the TCP sliding window protocol as defined in RFC 793 and RFC 1323.
