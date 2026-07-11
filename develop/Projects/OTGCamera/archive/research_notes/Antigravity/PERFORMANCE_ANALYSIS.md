# 🔬 Performance Analysis & Optimization Guide

## Current Architecture Analysis

### Pipeline hiện tại (Sequential)
```
Camera → memcpy → VPSS → YOLO (blocking) → Stream → Send
         ↑                    ↑               ↑
      ~1.5ms              ~70ms           ~2ms
```

**Problem**: YOLO inference (~70ms/frame = ~14 FPS) **BLOCKS** stream output!

---

## 🔴 Bottlenecks Identified

### 1. **Sequential Processing (CRITICAL)**
```cpp
// Current: YOLO blocks Stream
CVI_VPSS_GetChnFrame(YOLO) → CVI_TDL_YOLOV8_Detection() → CVI_VPSS_GetChnFrame(STREAM)
```
- YOLO takes ~70ms per frame
- Stream must wait for YOLO to complete
- **Result**: Both limited to ~14 FPS

### 2. **memcpy for Every Frame**
```cpp
memcpy(vb_cache[vb_idx].vir_addr, cam_bufs[buf.index].addr, CAM_W * CAM_H * 2);
```
- 614,400 bytes copied per frame
- ~1.5ms overhead per frame

### 3. **VB Pool Contention**
```cpp
#define VB_POOL_CNT 3  // Only 3 input buffers
```
- Too few buffers → potential starvation
- Blocks when all buffers in use

### 4. **Timeout Values Too Long**
```cpp
CVI_VPSS_SendFrame(vpss_grp, &frame_in, 100);     // 100ms timeout
CVI_VPSS_GetChnFrame(vpss_grp, VPSS_CHN_YOLO, &frame_yolo, 100);
```
- 100ms timeouts add latency when errors occur

### 5. **malloc/free Per Frame (VENC)**
```cpp
venc_stream.pstPack = (VENC_PACK_S *)malloc(...);  // Every frame!
free(venc_stream.pstPack);
```
- Heap allocation overhead

### 6. **Cache Flush Overhead**
```cpp
CVI_SYS_IonFlushCache(..., CAM_W * CAM_H * 2);  // ~614KB flush
```
- Full buffer flush every frame

---

## 🟢 Optimization Strategies

### Strategy 1: Parallel Processing (BIGGEST WIN) ⭐⭐⭐

**Separate YOLO and Stream into independent threads:**

```
Main Thread:
  Camera → memcpy → VPSS SendFrame
  
YOLO Thread:
  VPSS GetChnFrame(YOLO) → TDL Detection → UDP Metadata
  (runs at ~14 FPS, independent)
  
Stream Thread (existing):
  VPSS GetChnFrame(STREAM) → VENC → HTTP
  (runs at 25-30 FPS, not blocked by YOLO)
```

**Expected improvement**: Stream: 14 FPS → 25-30 FPS

### Strategy 2: Increase VB Pool Size ⭐⭐

```cpp
#define VB_POOL_CNT 6  // Was 3
vb.astCommPool[0].u32BlkCnt = 6;  // Input pool
vb.astCommPool[1].u32BlkCnt = 4;  // YOLO output
vb.astCommPool[2].u32BlkCnt = 4;  // Stream output
```

**Expected improvement**: Reduce frame drops, smoother pipeline

### Strategy 3: Pre-allocate VENC Pack Buffer ⭐

```cpp
// Global pre-allocated buffer
static VENC_PACK_S venc_pack_buffer[8];

// In loop:
venc_stream.pstPack = venc_pack_buffer;  // No malloc!
```

**Expected improvement**: ~0.1ms per frame

### Strategy 4: Reduce Timeouts ⭐

```cpp
#define VPSS_TIMEOUT 30  // Was 100ms
#define VENC_TIMEOUT 20  // Was 100ms
```

**Expected improvement**: Faster error recovery

### Strategy 5: Skip YOLO Frames ⭐

```cpp
static int frame_counter = 0;
#define YOLO_SKIP_FRAMES 2  // Process every 2nd frame

if (++frame_counter % YOLO_SKIP_FRAMES == 0) {
    // Do YOLO inference
}
```

**Expected improvement**: YOLO "FPS" doubles, latency halves

### Strategy 6: VPSS Channel Depth ⭐⭐

```cpp
chn_attr.u32Depth = 2;  // Was 1 - allow 2 frames buffered
```

**Expected improvement**: Better pipelining

---

## 📊 Optimization Priority Matrix

| Optimization | Complexity | Impact | Priority |
|-------------|-----------|--------|----------|
| Parallel YOLO/Stream | High | ⭐⭐⭐⭐⭐ | 1 |
| Increase VB Pools | Low | ⭐⭐⭐ | 2 |
| VPSS Depth=2 | Low | ⭐⭐ | 3 |
| Pre-alloc VENC Pack | Low | ⭐ | 4 |
| Skip YOLO Frames | Low | ⭐⭐ | 5 |
| Reduce Timeouts | Low | ⭐ | 6 |

---

## 🚀 Recommended Implementation

### Quick Wins (Low effort, immediate):
1. `VB_POOL_CNT = 6`
2. `u32Depth = 2` for VPSS channels
3. Reduce timeouts to 30ms
4. Pre-allocate VENC pack buffer

### Major Refactor (High effort, big impact):
1. Create separate YOLO thread
2. Use ring buffer/queue for frame sharing
3. Stream thread pulls from VPSS independently

---

## Expected Results After Optimization

| Metric | Before | After |
|--------|--------|-------|
| Stream FPS | 10-14 | 25-30 |
| YOLO FPS | 10-14 | 14-16 |
| Latency | ~100ms | ~50ms |
| CPU Usage | High | Lower |

---

## Code Changes for Quick Wins

### 1. VB Pool Size
```cpp
#define VB_POOL_CNT 6

// In sys_vb_init():
vb.astCommPool[0].u32BlkCnt = 6;  // Input
vb.astCommPool[1].u32BlkCnt = 4;  // YOLO
vb.astCommPool[2].u32BlkCnt = 4;  // Stream
```

### 2. VPSS Depth
```cpp
chn0_attr.u32Depth = 2;  // YOLO channel
chn1_attr.u32Depth = 2;  // Stream channel
```

### 3. Pre-allocate VENC Pack
```cpp
// Global
static VENC_PACK_S g_venc_pack[8];

// In loop (replace malloc)
venc_stream.pstPack = g_venc_pack;
// Remove free()
```

### 4. Reduce Timeouts
```cpp
#define API_TIMEOUT 30  // Use everywhere instead of 100
```
