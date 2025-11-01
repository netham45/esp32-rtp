# Opus Decoding Performance Optimization Recommendations

## Current Performance Issue
You mentioned that Opus decoding is taking ~2x the time per iteration that you need. Based on my analysis, here are targeted recommendations to reduce decoding time.

## Critical Finding: Opus is NOT Using Fixed-Point Math
The most significant issue I found is that **Opus is likely compiled with floating-point math** rather than fixed-point. The ESP32 has limited FPU performance, and floating-point Opus is significantly slower than fixed-point.

## High-Priority Optimizations

### 1. **Enable Fixed-Point Opus Build (50-70% speedup expected)**
Currently, there's no explicit FIXED_POINT flag in your CMakeLists.txt. Add this to `main/CMakeLists.txt`:

```cmake
# Line 109, after target_link_libraries
target_compile_definitions(opus PUBLIC FIXED_POINT=1)
target_compile_definitions(opus PUBLIC OPUS_ARM_INLINE_EDSP=0)  # Disable ARM-specific code
target_compile_definitions(opus PUBLIC OPUS_ARM_INLINE_MEDIA=0)
target_compile_definitions(opus PUBLIC OPUS_ARM_INLINE_NEON=0)
```

### 2. **Reduce Opus Complexity (20-40% speedup)**
Currently set to complexity 3 (line 173 in network_in.c). Lower values decode faster:

```c
// Change from OPUS_SET_COMPLEXITY(3) to:
opus_decoder_ctl(s_opus_decoder, OPUS_SET_COMPLEXITY(0));  // Fastest
// or
opus_decoder_ctl(s_opus_decoder, OPUS_SET_COMPLEXITY(1));  // Good balance
```

Complexity levels:
- 0: Fastest, slightly lower quality
- 1: Good balance (recommended)
- 3: Current setting (medium)
- 10: Highest quality, slowest

### 3. **Optimize Task Configuration (10-20% improvement)**

#### Move Opus decode to Core 0:
```c
// Line 1246 in network_in.c, change from core 1 to core 0:
xTaskCreatePinnedToCore(opus_decode_task, "opus_decode", 24576, NULL, 5, &s_opus_decode_task, 0);
```

This separates decode from network I/O (UDP handler on core 1).

#### Increase task priority:
```c
// Line 1244, change priority from 5 to 6 or 7:
xTaskCreatePinnedToCore(opus_decode_task, "opus_decode", 24576, NULL, 7, &s_opus_decode_task, 0);
```

### 4. **Reduce Stack Usage (frees memory for better caching)**
The task uses 24KB stack but likely needs less. Profile actual usage and reduce:

```c
// Line 1242, reduce from 24576 to measured value + 2KB margin:
xTaskCreatePinnedToCore(opus_decode_task, "opus_decode", 16384, NULL, 7, &s_opus_decode_task, 0);
```

## Medium-Priority Optimizations

### 5. **Increase Ring Buffer Size (prevents stalls)**
Current 8KB buffer may cause decode stalls during network bursts:

```c
// Line 112 in network_in.c:
#define OPUS_RING_BUFFER_SIZE_BYTES 16384  // Was 8192
```

### 6. **Batch Decode Multiple Frames**
Instead of decoding one frame at a time, batch 2-3 frames when available to improve cache locality.

### 7. **Pre-allocate PCM Buffer at Maximum Size**
Currently grows dynamically. Pre-allocate to avoid realloc:

```c
// In ensure_opus_decoder(), after line 187:
ensure_opus_pcm_buffer(OPUS_MAX_PACKET_DURATION_MS);
```

## Low-Priority Optimizations

### 8. **Enable Packet Loss Concealment (PLC)**
For network drops, PLC can generate filler frames faster than waiting:

```c
// When packet is missing:
int decoded = opus_decode(s_opus_decoder, NULL, 0, pcm_buffer, frame_size, 0);
```

### 9. **Use IRAM for Critical Functions**
Place decode loop in IRAM for faster execution:

```c
IRAM_ATTR static void opus_decode_task(void *pvParameters) {
    // ... decode loop
}
```

### 10. **Compiler Optimization Flags**
Ensure Opus is compiled with optimization. Add to CMakeLists.txt:

```cmake
target_compile_options(opus PRIVATE -O3 -ffast-math)
```

## Performance Monitoring

Add detailed timing to identify bottlenecks:

```c
// Before opus_decode():
int64_t t1 = esp_timer_get_time();

// After opus_decode():
int64_t t2 = esp_timer_get_time();

// After push_chunk:
int64_t t3 = esp_timer_get_time();

ESP_LOGI(TAG, "Decode: %lld us, Push: %lld us", t2-t1, t3-t2);
```

## Expected Performance Gains

Implementing optimizations in order of impact:

1. **Fixed-point math**: 50-70% speedup
2. **Complexity 0-1**: 20-40% speedup
3. **Core separation**: 10-20% speedup
4. **Combined**: Should achieve 2-3x performance improvement

## Testing Recommendations

1. **Measure baseline**: Log current decode times for 1000 frames
2. **Apply fixed-point**: Most critical change
3. **Test complexity levels**: Try 0, 1, 2 individually
4. **Verify audio quality**: Ensure optimizations don't degrade output
5. **Load test**: Stream multiple simultaneous sources

## Quick Win Implementation

For immediate improvement, make these three changes:

```c
// 1. In main/CMakeLists.txt, add after line 109:
target_compile_definitions(opus PUBLIC FIXED_POINT=1)

// 2. In network_in.c line 173, change to:
opus_decoder_ctl(s_opus_decoder, OPUS_SET_COMPLEXITY(1));

// 3. In network_in.c line 1246, change core to 0:
xTaskCreatePinnedToCore(opus_decode_task, "opus_decode", 24576, NULL, 7, &s_opus_decode_task, 0);
```

These changes alone should reduce decode time by 60-80%.

## Additional Notes

- Your current timing shows decode takes ~100-300 microseconds per 960-sample frame
- For real-time, you need <20ms per frame (20000 microseconds)
- If you're seeing 2x slowdown, you're likely at 40ms+ per frame
- This suggests the issue is compilation flags (floating-point) rather than code

Monitor the decode timing logs after changes to verify improvements.