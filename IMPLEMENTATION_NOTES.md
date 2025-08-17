# KWin wlroots Screencopy Implementation Guide

## Overview
This implementation adds support for the wlroots screencopy protocol to KWin, enabling efficient screen capturing for VNC applications like wayvnc.

## Integration Steps

### 1. Protocol Integration (src/wayland/CMakeLists.txt)

Add the protocol XML to the protocol list around line 42:
```cmake
${PROJECT_SOURCE_DIR}/src/wayland/protocols/wlr-screencopy-unstable-v1.xml
```

Add the implementation files to target_sources around line 123:
```cmake
screencopy_v1.cpp
```

Add the header to the install list around line 213:
```cmake
screencopy_v1.h
```

### 2. Wayland Server Integration (src/wayland_server.cpp)

Add include:
```cpp
#include "wayland/screencopy_v1.h"
```

In WaylandServer initialization, add:
```cpp
m_screencopyManager = new ScreencopyManagerV1Interface(m_display, this);
connect(m_screencopyManager, &ScreencopyManagerV1Interface::frameRequested,
        this, &WaylandServer::handleScreencopyFrame);
```

### 3. Key Implementation Points

#### Buffer Copying Strategy
- Use existing `Compositor::textureForOutput()` for getting screen content
- Support both wl_shm and DMA-BUF buffer types
- Implement efficient texture-to-buffer copying

#### Damage Tracking
- Hook into KWin's damage tracking system (Output::outputChange signal)
- Queue damage regions for copy_with_damage requests
- Send damage events before ready events

#### Performance Optimizations
- Zero-copy operations where possible (DMA-BUF)
- Efficient memory mapping for wl_shm buffers
- Minimal texture reads and format conversions

### 4. Testing Strategy

1. **Basic functionality**: Test with simple screencopy clients
2. **wayvnc integration**: Test VNC performance improvement
3. **Multi-output scenarios**: Ensure proper output handling
4. **Damage tracking**: Verify only changed regions are copied

### 5. Security Considerations

- Follow KWin's existing permission model
- Consider adding screencopy to restricted interfaces list
- Implement proper client validation

## Expected Performance Impact

Based on the protocol design:
- **Reduced CPU usage**: Direct buffer copying vs PipeWire streaming
- **Lower latency**: No video encoding/decoding pipeline
- **Bandwidth efficiency**: Damage tracking minimizes data transfer
- **VNC optimization**: Designed specifically for screen sharing use cases

## Implementation Complexity: Medium

- **Protocol complexity**: Low (well-defined wlroots protocol)
- **Integration complexity**: Medium (existing patterns to follow)
- **Testing complexity**: Medium (multiple buffer types and scenarios)
- **Estimated timeline**: 4-6 weeks for full implementation

This will give you the smooth, hardware-like VNC experience you're looking for!
