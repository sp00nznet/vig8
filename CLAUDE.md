# Claude Code Project Memory

## LAN Multiplayer Implementation Status (2025-02-25)

### What was done
- Implemented full LAN multiplayer networking in `project/src/net.h` and `project/src/net.cpp` (~1084 lines added)
- Overrides 13 XNet/QoS/Winsock functions via `/force:multiple` linker flag
- Background discovery thread broadcasts beacons on UDP port 3074
- Custom protocol: magic `0xD8`, BEACON (`0x01`) and PROBE (`0x02`) packet types
- Peer table tracks XNADDR <-> native IP mappings
- QoS data relayed verbatim from host's session blob
- XNetQosLookup reads stack params at offsets +84, +92, +100, +108 (determined from generated wrapper at `vig8_recomp.9.cpp:47532`)
- WSARecvFrom reads overlapped ptr from stack at offset +84 (from wrapper at `vig8_recomp.9.cpp:50280`)
- Guest memory allocated via `kernel_state()->memory()->SystemHeapAlloc()` for XNQOS results
- XNQOSINFO struct: 24 bytes per entry (flags, probes, cbData, pbData, RTT, bandwidth)

### Build & test status
- Builds clean with Clang 21 (Release + Debug), only expected `/force:multiple` linker warnings
- Game launches successfully with the networking code (no crash on startup)
- **NOT YET TESTED**: navigating to multiplayer menu, hosting/joining a LAN game
- Need to verify: QoS data format compatibility, async receive timing, discovery thread stability

### Key files
- `project/src/net.h` (new) - Types, constants, `NetInit()`/`NetShutdown()` interface
- `project/src/net.cpp` (new) - All networking overrides + discovery thread
- `project/src/stubs.cpp` - Removed 5 old networking stubs (now in net.cpp)
- `project/CMakeLists.txt` - Added `net.cpp` to both `vig8` and `vig8_test` targets
- `project/src/settings.h` / `settings.cpp` - Added `lan_port = 3074` in `[network]` section
- `project/src/main.cpp` - Calls `NetInit()`/`NetShutdown()`

### Build command
```bash
PATH="/c/Program Files/LLVM/bin:$PATH"
cd E:/vig8/project && cmake --preset win-amd64
cd out/build/win-amd64 && cmake --build . --target vig8 --config Release -- -j8
```

### Run command
```bash
cd E:/vig8/project/out/build/win-amd64/Release && ./vig8.exe E:\vig8\extracted
```

### Next steps to test
1. Enable `show_console = true` in `vig8_settings.toml` to see REXLOG output
2. Navigate to System Link menu in-game to trigger `XNetGetTitleXnAddr` / QoS calls
3. Run two instances on same LAN, verify host beacons visible in Wireshark (UDP port 3074)
4. Check if `XNetQosLookup` properly returns results and game lists them
5. If game crashes in multiplayer, check `vig8_crash.log` for the failing function
6. **Firewall**: ensure UDP port 3074 is open on both machines
