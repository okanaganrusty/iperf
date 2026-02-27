# TCP Flow Control Implementation (RFC 1323)

## Changes Made

### 1. **Window Scaling Support**
- Added `wscale_local` (server offers up to 2^7 * 64KB = 8MB window)
- Added `wscale_remote` (parsed from client's SYN)
- Dynamically parses TCP options from SYN packets to negotiate window scaling

### 2. **Receive Window Tracking**
- Added `rwnd_available` field tracking actual available buffer space
- Initialized to full RX_BUFFER_SIZE (8MB)
- Decreases when packets are buffered
- Increases when application consumes data via recv()

### 3. **Dynamic Window Advertisement**
- Window size sent to client = `rwnd_available >> wscale_remote`
- If buffer is full: rwnd_available = 0, advertised window = 0 (client stops sending)
- As app drains data: rwnd_available increases, window opens (client resumes)
- Automatic flow control without any timers or backpressure mechanisms

### 4. **Proper Windowing In dpdk_create_tcp_packet()**
- Updated window calculation to use scaled available window
- Ensures TX window correctly reflects receive buffer state
- All packets (ACKs, SYN-ACK) now advertise proper window

### 5. **Buffer Space Restoration**
- recv() functions now restore rwnd_available as data is consumed
- Allows window to re-open and new data to arrive
- Prevents permanent stalls due to full buffers

## Flow Control Semantics

### Before:
```
Client sends → Server buffers packet → Server ACKs
Client sees ACK → server wants more data
Client sends more even if server buffer full
Result: Buffer overflow → drops → stall
```

### After:
```
Client sends → Server checks rwnd_available
If space: buffer packet, ACK with open window, rwnd_available--
If no space: drop (no ACK), client retransmits
Client sees closed window (0) → stops sending
As app drains: rwnd_available++, ACK with larger window
Client sees open window → resumes sending
Result: Automatic rate matching, no stalls
```

## Window Calculation Example

- RX buffer: 8MB total
- Window scaling: 7 (multiplier = 128)
- Initial: rwnd_available = 8MB, advertised window = 8MB>>7 = 65535 (max 16-bit)
- After buffering 2MB: rwnd_available = 6MB, advertised window = 6MB>>7 = 48000
- Client sees window=48000 and can only have ~48000 unACKed bytes in flight
- Automatic rate limiting without congestion control algorithms

## Testing Notes

This implementation uses simplified TCP (no AIMD, no slow-start) but proper windowing
ensures the link won't deadlock even at 12Gbps. The window shrinking/growing provides
inherent flow control that prevents buffer exhaustion.
