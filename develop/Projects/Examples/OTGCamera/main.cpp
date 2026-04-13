/**
 * @file main.cpp
 * @brief USB Camera Capture với VPSS Dual Channel + MJPEG Streaming + YOLO
 * 
 * Chương trình sử dụng VPSS để tạo 2 channel từ 1 nguồn camera:
 * - Channel 0: Configurable WxH RGB_888_PLANAR cho YOLO inference
 * - Channel 1: Configurable WxH NV12 -> VENC JPEG -> HTTP Stream
 * 
 * Sử dụng hardware acceleration tối đa (VPSS + VENC) để giảm tải CPU.
 * 
 * Tham khảo: YoloOTGCamera_stable2.cpp và stream_stable.cpp
 * 
 * Usage:
 *   ./Yolo_CSIStream <model.cvimodel> [options]
 * Options:
 *   --cam WxH        Camera capture resolution (default: 640x480)
 *   --stream WxH     Stream output resolution  (default: same as cam)
 *   --yolo N         YOLO input size NxN        (default: 640)
 *   --quality Q      JPEG quality 1-100         (default: 70)
 *   --conf T         YOLO confidence threshold  (default: 0.5)
 *   --nms T          YOLO NMS threshold         (default: 0.5)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <termios.h>  // UART serial
#include <iostream>
#include <vector>
#include <atomic>

// --- CVI HEADERS ---
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vpss.h"
#include "cvi_venc.h"
#include "cvi_comm_video.h"
#include "cvi_tdl.h"
#include "cvi_tdl_media.h"

// --- CONFIG ---
#define CAM_DEV "/dev/video0"

// Configurable resolutions (set from command-line)
static int g_cam_w     = 640;
static int g_cam_h     = 480;
static int g_stream_w  = 640;
static int g_stream_h  = 480;
static int g_yolo_w    = 640;
static int g_yolo_h    = 640;
static int g_jpeg_quality = 70;    // Lower default for faster encoding
static float g_conf_thresh = 0.5f;
static float g_nms_thresh  = 0.5f;
static bool g_no_yolo  = false;    // Stream-only mode (no YOLO)
static bool g_use_userptr = false; // Set true if V4L2 USERPTR is supported
static const char* g_uart_dev = NULL;  // UART device path (NULL = disabled)
static int g_uart_baud = 115200;       // UART baudrate

#define BUF_CNT 4           // V4L2 camera buffers (4 is enough with fast processing)
#define VB_POOL_CNT 4       // VB input cache blocks (reduced - 4 is sufficient)

// API Timeouts (ms)
#define API_TIMEOUT 20       // Fast timeout for stream
#define YOLO_TIMEOUT 50      // Longer timeout for YOLO

// VPSS Channel IDs
#define VPSS_CHN_YOLO   0
#define VPSS_CHN_STREAM 1

// VENC Config
#define VENC_CHN_JPEG 0

// HTTP Stream
#define STREAM_PORT 8080
#define METADATA_PORT 8081

// --- YOLO PARAMS ---
#define MODEL_SCALE 1.0 
#define MODEL_MEAN 0.0
#define MODEL_CLASS_CNT 80

// --- Stride Alignment ---
// CVI hardware requires 64-byte aligned strides for optimal DMA performance
#define STRIDE_ALIGN 64
static inline int align_up(int val, int align) {
    return (val + align - 1) & ~(align - 1);
}

// --- RESOURCES ---
static struct {
    VB_BLK blk;
    CVI_U64 phy_addr;
    uint8_t *vir_addr;
    std::atomic<int> in_use;
} vb_cache[VB_POOL_CNT];

struct CamBuf {
    void *addr;
    size_t len;
};

static CamBuf cam_bufs[BUF_CNT];
static VPSS_GRP vpss_grp = 0;
static std::atomic<int> running(1);

// Computed sizes (set in main after parsing args)
static int g_cam_frame_size    = 0;  // cam_stride * cam_h
static int g_cam_stride        = 0;  // aligned stride for YUYV
static int g_yolo_frame_size   = 0;
static int g_stream_nv12_size  = 0;

// TDL handle for YOLO
static cvitdl_handle_t tdl_handle = NULL;

// Stream client management - supports multiple simultaneous clients
#define MAX_STREAM_CLIENTS 4
static int stream_clients[MAX_STREAM_CLIENTS] = {-1, -1, -1, -1};
static int stream_client_count = 0;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

// UDP socket for metadata broadcast
static int metadata_socket = -1;
static sockaddr_in metadata_broadcast_addr;

// UART serial output for YOLO detections
static int g_uart_fd = -1;

// --- External LED Control ---
// Status LED on A24 (GPIOA24 = Linux GPIO 503) - Always ON when running OK
#define STATUS_LED_GPIO "503"
#define STATUS_LED_PATH "/sys/class/gpio/gpio" STATUS_LED_GPIO

// YOLO Detection LED on A23 (GPIOA23 = Linux GPIO 504) - ON when objects detected
#define DETECT_LED_GPIO "504"
#define DETECT_LED_PATH "/sys/class/gpio/gpio" DETECT_LED_GPIO

#define GPIO_EXPORT   "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"

static int g_status_led_fd = -1; // Cached fd for Status LED (A24)
static int g_detect_led_fd = -1; // Cached fd for Detection LED (A23)

/**
 * @brief Write a string to a sysfs file
 */
static int sysfs_write(const char* path, const char* value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, value, strlen(value));
    close(fd);
    return 0;
}

/**
 * @brief Initialize both Status (A24) and Detection (A23) LEDs
 */
void led_init() {
    // Export both GPIOs
    sysfs_write(GPIO_EXPORT, STATUS_LED_GPIO);
    sysfs_write(GPIO_EXPORT, DETECT_LED_GPIO);
    usleep(100000);  // Wait for sysfs nodes to appear

    // Configure Status LED (P18)
    sysfs_write(STATUS_LED_PATH "/direction", "out");
    g_status_led_fd = open(STATUS_LED_PATH "/value", O_WRONLY);
    if (g_status_led_fd >= 0) {
        write(g_status_led_fd, "0", 1);  // Start OFF
        printf("[LED] Status LED initialized on A24 (GPIO %s)\n", STATUS_LED_GPIO);
    } else {
        printf("[LED] WARNING: Could not initialize Status LED on A24 (GPIO %s)\n", STATUS_LED_GPIO);
    }

    // Configure Detection LED (P19)
    sysfs_write(DETECT_LED_PATH "/direction", "out");
    g_detect_led_fd = open(DETECT_LED_PATH "/value", O_WRONLY);
    if (g_detect_led_fd >= 0) {
        write(g_detect_led_fd, "0", 1);  // Start OFF
        printf("[LED] Detection LED initialized on A23 (GPIO %s)\n", DETECT_LED_GPIO);
    } else {
        printf("[LED] WARNING: Could not initialize Detection LED on A23 (GPIO %s)\n", DETECT_LED_GPIO);
    }
}

/**
 * @brief Set Status LED (P18) state: 1 = ON, 0 = OFF
 */
void status_led_set(int on) {
    if (g_status_led_fd < 0) return;
    const char* val = on ? "1" : "0";
    lseek(g_status_led_fd, 0, SEEK_SET);
    write(g_status_led_fd, val, 1);
}

/**
 * @brief Set Detection LED (P19) state: 1 = ON, 0 = OFF
 */
void detect_led_set(int on) {
    if (g_detect_led_fd < 0) return;
    const char* val = on ? "1" : "0";
    lseek(g_detect_led_fd, 0, SEEK_SET);
    write(g_detect_led_fd, val, 1);
}

/**
 * @brief Cleanup both LEDs: turn off and unexport GPIOs
 */
void led_cleanup() {
    if (g_status_led_fd >= 0) {
        write(g_status_led_fd, "0", 1);  // Turn off
        close(g_status_led_fd);
        g_status_led_fd = -1;
    }
    if (g_detect_led_fd >= 0) {
        write(g_detect_led_fd, "0", 1);  // Turn off
        close(g_detect_led_fd);
        g_detect_led_fd = -1;
    }
    sysfs_write(GPIO_UNEXPORT, STATUS_LED_GPIO);
    sysfs_write(GPIO_UNEXPORT, DETECT_LED_GPIO);
}

// ============================================================
// UART Serial Output for YOLO Detections
// ============================================================
// Protocol: NMEA-like format for easy Arduino/ESP32 parsing
//
// Format: $YOLO,<ts_ms>,<count>[,<cls>,<x1>,<y1>,<x2>,<y2>,<score>]*<XX>\r\n
//
// <ts_ms>  = timestamp in milliseconds
// <count>  = number of objects (0 = no detection)
// <cls>    = COCO class ID (0-79)
// <x1,y1>  = top-left corner (in YOLO input coords)
// <x2,y2>  = bottom-right corner
// <score>  = confidence * 100 (integer, e.g. 93 = 0.93)
// *XX      = XOR checksum of everything between $ and * (hex)
//
// Examples:
//   $YOLO,12345678,0*3A\r\n                           (no objects)
//   $YOLO,12345678,1,0,120,80,320,380,93*4B\r\n       (1 person)
//   $YOLO,12345678,2,0,120,80,320,380,93,67,50,200,150,87*5C\r\n
//
// Arduino parsing example:
//   if (line.startsWith("$YOLO,")) {
//     char* tok = strtok(buf+6, ",*");  // skip "$YOLO,"
//     unsigned long ts = atol(tok);
//     tok = strtok(NULL, ",*");
//     int count = atoi(tok);
//     for (int i = 0; i < count; i++) {
//       int cls = atoi(strtok(NULL, ",*"));
//       int x1  = atoi(strtok(NULL, ",*"));
//       // ... etc
//     }
//   }
// ============================================================

/**
 * @brief Convert baudrate int to termios speed_t
 */
static speed_t baud_to_speed(int baud) {
    switch(baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}

/**
 * @brief Initialize UART serial port
 */
int uart_init() {
    if (!g_uart_dev) return 0;  // UART not enabled
    
    g_uart_fd = open(g_uart_dev, O_WRONLY | O_NOCTTY | O_NONBLOCK);
    if (g_uart_fd < 0) {
        printf("[UART] ERROR: Cannot open %s\n", g_uart_dev);
        return -1;
    }
    
    // Configure serial port
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(g_uart_fd, &tty) != 0) {
        printf("[UART] ERROR: tcgetattr failed\n");
        close(g_uart_fd);
        g_uart_fd = -1;
        return -1;
    }
    
    speed_t speed = baud_to_speed(g_uart_baud);
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);
    
    // 8N1, no flow control
    tty.c_cflag &= ~PARENB;   // No parity
    tty.c_cflag &= ~CSTOPB;   // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;        // 8 data bits
    tty.c_cflag &= ~CRTSCTS;  // No hardware flow control
    tty.c_cflag |= CLOCAL;    // Ignore modem control lines
    
    // Raw output (no processing)
    tty.c_oflag &= ~OPOST;
    tty.c_lflag = 0;          // Raw mode
    tty.c_iflag = 0;          // No input processing
    
    tcsetattr(g_uart_fd, TCSANOW, &tty);
    tcflush(g_uart_fd, TCOFLUSH);
    
    printf("[UART] Initialized: %s @ %d baud (8N1)\n", g_uart_dev, g_uart_baud);
    return 0;
}

/**
 * @brief Calculate XOR checksum (NMEA-style, between $ and *)
 */
static uint8_t nmea_checksum(const char* str) {
    uint8_t cs = 0;
    // Skip leading $
    if (*str == '$') str++;
    while (*str && *str != '*') {
        cs ^= (uint8_t)*str++;
    }
    return cs;
}

/**
 * @brief Send YOLO detections via UART in NMEA-like format
 * Max 10 objects per message to keep within typical UART buffer sizes
 */
void uart_send_detections(const cvtdl_object_t* obj_meta) {
    if (g_uart_fd < 0) return;
    
    char msg[1024];
    int pos = 0;
    
    // Timestamp in milliseconds
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ts_ms = tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
    
    // Build message body (without $ prefix and checksum)
    pos += snprintf(msg + pos, sizeof(msg) - pos, "$YOLO,%llu,%u,%d,%d",
                    (unsigned long long)(ts_ms % 100000000ULL),  // Keep 8 digits max
                    obj_meta->size,
                    g_cam_w, g_cam_h);
    
    // Append up to 10 objects
    int max_objs = obj_meta->size > 10 ? 10 : obj_meta->size;
    // Calculate scale factors to map YOLO output space back to Camera space
    float scale_x = (float)g_cam_w / g_yolo_w;
    float scale_y = (float)g_cam_h / g_yolo_h;

    for (int i = 0; i < max_objs; i++) {
        const cvtdl_object_info_t* info = &obj_meta->info[i];
        
        // Scale coordinates back to original camera resolution
        int x1 = (int)(info->bbox.x1 * scale_x);
        int y1 = (int)(info->bbox.y1 * scale_y);
        int x2 = (int)(info->bbox.x2 * scale_x);
        int y2 = (int)(info->bbox.y2 * scale_y);

        // Clamp to screen boundaries just in case
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > g_cam_w) x2 = g_cam_w;
        if (y2 > g_cam_h) y2 = g_cam_h;

        pos += snprintf(msg + pos, sizeof(msg) - pos, ",%d,%d,%d,%d,%d,%d",
                        info->classes,
                        x1, y1,
                        x2, y2,
                        (int)(info->bbox.score * 100));
    }
    
    // Calculate checksum and append
    uint8_t cs = nmea_checksum(msg);
    pos += snprintf(msg + pos, sizeof(msg) - pos, "*%02X\r\n", cs);
    
    // Single write for atomicity
    write(g_uart_fd, msg, pos);
}

/**
 * @brief Cleanup UART
 */
void uart_cleanup() {
    if (g_uart_fd >= 0) {
        // Send goodbye message
        const char* bye = "$YOLO,0,0*00\r\n";
        write(g_uart_fd, bye, strlen(bye));
        close(g_uart_fd);
        g_uart_fd = -1;
        printf("[UART] Closed\n");
    }
}
static VENC_PACK_S g_venc_pack[8];

// JPEG send thread (decouples network I/O from capture pipeline)
static pthread_t jpeg_send_thread_id;
static pthread_mutex_t jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  jpeg_cond  = PTHREAD_COND_INITIALIZER;
static void* g_jpeg_buf = NULL;    // Double-buffered JPEG data
static int   g_jpeg_size = 0;
static bool  g_jpeg_ready = false;

// Atomic counters for parallel YOLO thread
static std::atomic<int> yolo_frame_cnt(0);
static std::atomic<int> yolo_object_cnt(0);
static std::atomic<int> stream_frame_cnt(0);

// --- Monotonic clock for accurate timing (no NTP jumps) ---
static inline double get_monotonic_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/**
 * @brief Print usage
 */
void print_usage(const char* progname) {
    printf("Usage: %s <model.cvimodel> [options]\n", progname);
    printf("\nOptions:\n");
    printf("  --cam WxH        Camera capture resolution (default: 640x480)\n");
    printf("  --stream WxH     Stream output resolution  (default: same as cam)\n");
    printf("  --yolo N         YOLO input size NxN        (default: 640)\n");
    printf("  --quality Q      JPEG quality 1-100         (default: 70)\n");
    printf("  --conf T         YOLO confidence threshold  (default: 0.5)\n");
    printf("  --nms T          YOLO NMS threshold         (default: 0.5)\n");
    printf("  --no-yolo        Stream-only mode (no YOLO detection)\n");
    printf("  --uart DEV       Send YOLO output via UART (e.g. /dev/ttyS0)\n");
    printf("  --baud RATE      UART baudrate (default: 115200)\n");
    printf("\nExamples:\n");
    printf("  %s yolov8n.cvimodel\n", progname);
    printf("  %s yolov8n.cvimodel --cam 640x480 --yolo 320\n", progname);
    printf("  %s yolov8n.cvimodel --cam 1280x720 --stream 640x480 --yolo 640 --quality 60\n", progname);
    printf("  %s yolov8n.cvimodel --no-yolo --quality 80  # Stream only, no YOLO\n", progname);
}

/**
 * @brief Parse WxH resolution string (e.g. "640x480" or "1280x720")
 * @return 0 on success, -1 on failure
 */
int parse_resolution(const char* str, int* w, int* h) {
    // Try WxH format
    if (sscanf(str, "%dx%d", w, h) == 2) return 0;
    // Try W*H format  
    if (sscanf(str, "%d*%d", w, h) == 2) return 0;
    return -1;
}

/**
 * @brief Parse command-line arguments
 * @return model path or NULL on error
 */
const char* parse_args(int argc, char* argv[]) {
    if (argc < 2) {
        // Check if --no-yolo is the only argument
        print_usage(argv[0]);
        return NULL;
    }
    
    // Check for --no-yolo first (model path not required)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-yolo") == 0) {
            g_no_yolo = true;
            break;
        }
    }
    
    const char* model_path = g_no_yolo ? NULL : argv[1];
    bool stream_res_set = false;
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--cam") == 0 && i + 1 < argc) {
            if (parse_resolution(argv[++i], &g_cam_w, &g_cam_h) != 0) {
                printf("[ERROR] Invalid --cam format: %s (use WxH, e.g. 640x480)\n", argv[i]);
                return NULL;
            }
            // If stream resolution was not explicitly set, follow camera resolution
            if (!stream_res_set) {
                g_stream_w = g_cam_w;
                g_stream_h = g_cam_h;
            }
        } else if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc) {
            if (parse_resolution(argv[++i], &g_stream_w, &g_stream_h) != 0) {
                printf("[ERROR] Invalid --stream format: %s (use WxH, e.g. 640x480)\n", argv[i]);
                return NULL;
            }
            stream_res_set = true;
        } else if (strcmp(argv[i], "--yolo") == 0 && i + 1 < argc) {
            g_yolo_w = atoi(argv[++i]);
            g_yolo_h = g_yolo_w;  // YOLO always square
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            g_jpeg_quality = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--conf") == 0 && i + 1 < argc) {
            g_conf_thresh = atof(argv[++i]);
        } else if (strcmp(argv[i], "--nms") == 0 && i + 1 < argc) {
            g_nms_thresh = atof(argv[++i]);
        } else if (strcmp(argv[i], "--no-yolo") == 0) {
            g_no_yolo = true;
        } else if (strcmp(argv[i], "--uart") == 0 && i + 1 < argc) {
            g_uart_dev = argv[++i];
        } else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            g_uart_baud = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return NULL;
        } else {
            printf("[ERROR] Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return NULL;
        }
    }
    
    // Validate resolutions
    if (g_cam_w < 160 || g_cam_w > 1920 || g_cam_h < 120 || g_cam_h > 1080) {
        printf("[ERROR] Camera resolution %dx%d out of range (160-1920 x 120-1080)\n", g_cam_w, g_cam_h);
        return NULL;
    }
    if (g_stream_w < 160 || g_stream_w > 1920 || g_stream_h < 120 || g_stream_h > 1080) {
        printf("[ERROR] Stream resolution %dx%d out of range (160-1920 x 120-1080)\n", g_stream_w, g_stream_h);
        return NULL;
    }
    if (g_stream_w > g_cam_w || g_stream_h > g_cam_h) {
        printf("[ERROR] Stream resolution %dx%d cannot exceed camera resolution %dx%d\n", 
               g_stream_w, g_stream_h, g_cam_w, g_cam_h);
        return NULL;
    }
    if (g_yolo_w < 128 || g_yolo_w > 1024 || (g_yolo_w % 32) != 0) {
        printf("[ERROR] YOLO size %d must be 128-1024 and multiple of 32\n", g_yolo_w);
        return NULL;
    }
    if (g_jpeg_quality < 1 || g_jpeg_quality > 100) {
        printf("[ERROR] JPEG quality %d must be 1-100\n", g_jpeg_quality);
        return NULL;
    }
    if (g_conf_thresh < 0.01f || g_conf_thresh > 1.0f) {
        printf("[ERROR] Confidence threshold %.2f must be 0.01-1.0\n", g_conf_thresh);
        return NULL;
    }
    if (g_nms_thresh < 0.01f || g_nms_thresh > 1.0f) {
        printf("[ERROR] NMS threshold %.2f must be 0.01-1.0\n", g_nms_thresh);
        return NULL;
    }
    
    return model_path;
}

/**
 * @brief Lấy IP address của board
 */
std::string get_ip() {
    struct ifaddrs *ifaddr, *ifa;
    char ip[INET_ADDRSTRLEN] = {0};

    if (getifaddrs(&ifaddr) == -1) return "UNKNOWN";

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (strncmp(ifa->ifa_name, "eth", 3) == 0 ||
                strncmp(ifa->ifa_name, "usb", 3) == 0 ||
                strncmp(ifa->ifa_name, "wlan", 4) == 0) {
                auto* addr = (sockaddr_in*)ifa->ifa_addr;
                inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return strlen(ip) ? ip : "UNKNOWN";
}

/**
 * @brief Khởi tạo YOLO parameters
 */
CVI_S32 init_yolo_params(const cvitdl_handle_t handle) {
    YoloPreParam preprocess_cfg = CVI_TDL_Get_YOLO_Preparam(handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);
    for (int i = 0; i < 3; i++) {
        preprocess_cfg.factor[i] = MODEL_SCALE;
        preprocess_cfg.mean[i] = MODEL_MEAN;
    }
    preprocess_cfg.format = PIXEL_FORMAT_RGB_888_PLANAR;
    CVI_TDL_Set_YOLO_Preparam(handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, preprocess_cfg);
    
    YoloAlgParam yolov8_param = CVI_TDL_Get_YOLO_Algparam(handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);
    yolov8_param.cls = MODEL_CLASS_CNT;
    CVI_TDL_Set_YOLO_Algparam(handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, yolov8_param);
    CVI_TDL_SetModelThreshold(handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, g_conf_thresh);
    CVI_TDL_SetModelNmsThreshold(handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, g_nms_thresh);
    return CVI_SUCCESS;
}

/**
 * @brief Khởi tạo UDP socket cho YOLO metadata broadcast
 */
int init_metadata_socket() {
    metadata_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (metadata_socket < 0) {
        printf("[WARN] Cannot create metadata socket\n");
        return -1;
    }
    
    int broadcast = 1;
    setsockopt(metadata_socket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    
    // Calculate subnet broadcast address automatically based on current IP (assuming /24 subnet)
    std::string ip = get_ip();
    std::string bcast_ip = "255.255.255.255";
    if (ip != "UNKNOWN") {
        size_t last_dot = ip.find_last_of('.');
        if (last_dot != std::string::npos) {
            bcast_ip = ip.substr(0, last_dot) + ".255";
        }
    }

    memset(&metadata_broadcast_addr, 0, sizeof(metadata_broadcast_addr));
    metadata_broadcast_addr.sin_family = AF_INET;
    metadata_broadcast_addr.sin_port = htons(METADATA_PORT);
    metadata_broadcast_addr.sin_addr.s_addr = inet_addr(bcast_ip.c_str());
    
    printf("[META] UDP metadata broadcast on port %d (Target: %s)\n", METADATA_PORT, bcast_ip.c_str());
    return 0;
}

/**
 * @brief Gửi YOLO detection results qua UDP (JSON format)
 * Includes stream and yolo resolution info for proper coordinate mapping
 */
void send_yolo_metadata(const cvtdl_object_t* obj_meta) {
    if (metadata_socket < 0) return;
    
    char json[2048];
    int pos = 0;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ts_ms = tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
    
    // Include resolution info so Python client can scale coordinates
    pos += snprintf(json + pos, sizeof(json) - pos, 
        "{\"ts\":%llu,\"cnt\":%u,\"yw\":%d,\"yh\":%d,\"sw\":%d,\"sh\":%d,\"objs\":[", 
        (unsigned long long)ts_ms, obj_meta->size,
        g_yolo_w, g_yolo_h, g_stream_w, g_stream_h);
    
    int max_objs = obj_meta->size > 20 ? 20 : obj_meta->size;
    for (int i = 0; i < max_objs; i++) {
        const cvtdl_object_info_t* info = &obj_meta->info[i];
        if (i > 0) json[pos++] = ',';
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"c\":%d,\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f,\"s\":%.2f}",
            info->classes,
            info->bbox.x1, info->bbox.y1,
            info->bbox.x2 - info->bbox.x1,
            info->bbox.y2 - info->bbox.y1,
            info->bbox.score);
    }
    
    pos += snprintf(json + pos, sizeof(json) - pos, "]}");
    
    sendto(metadata_socket, json, pos, MSG_DONTWAIT,
           (sockaddr*)&metadata_broadcast_addr, sizeof(metadata_broadcast_addr));
}

/**
 * @brief Khởi tạo Video Buffer pools
 * 
 * Pool sizing strategy: Each pool is sized based on the actual stride-aligned
 * frame size for that stage. This prevents both over-allocation (wasting limited 
 * memory on LicheeRV Nano) and under-allocation (causing VB_GetBlock failures).
 */
int sys_vb_init() {
    VB_CONFIG_S vb;
    memset(&vb, 0, sizeof(vb));
    vb.u32MaxPoolCnt = 4;
    
    // Pool 0: Input (YUYV) - cho camera
    vb.astCommPool[0].u32BlkCnt  = VB_POOL_CNT;
    vb.astCommPool[0].u32BlkSize = g_cam_frame_size;
    vb.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
    
    // Pool 1: Output YOLO (RGB_888_PLANAR)
    vb.astCommPool[1].u32BlkCnt  = 4;
    vb.astCommPool[1].u32BlkSize = g_yolo_frame_size;
    vb.astCommPool[1].enRemapMode = VB_REMAP_MODE_CACHED;

    // Pool 2: Output Stream (NV12)
    vb.astCommPool[2].u32BlkCnt  = 3;
    vb.astCommPool[2].u32BlkSize = g_stream_nv12_size;
    vb.astCommPool[2].enRemapMode = VB_REMAP_MODE_CACHED;

    // Pool 3: VENC output buffer
    // JPEG output is typically smaller than raw, but need headroom
    vb.astCommPool[3].u32BlkCnt  = 3;
    vb.astCommPool[3].u32BlkSize = g_stream_nv12_size * 2;
    vb.astCommPool[3].enRemapMode = VB_REMAP_MODE_CACHED;

    printf("[VB] Pool sizes: Input=%d, YOLO=%d, Stream=%d, VENC=%d\n",
           g_cam_frame_size, g_yolo_frame_size, g_stream_nv12_size, g_stream_nv12_size * 2);

    if (CVI_VB_SetConfig(&vb) != CVI_SUCCESS) {
        printf("[ERROR] CVI_VB_SetConfig failed\n");
        return -1;
    }
    if (CVI_VB_Init() != CVI_SUCCESS) {
        printf("[ERROR] CVI_VB_Init failed\n");
        return -1;
    }
    if (CVI_SYS_Init() != CVI_SUCCESS) {
        printf("[ERROR] CVI_SYS_Init failed\n");
        return -1;
    }

    // Pre-allocate VB blocks cho input
    for (int i = 0; i < VB_POOL_CNT; i++) {
        vb_cache[i].blk = CVI_VB_GetBlock(0, g_cam_frame_size);
        if (vb_cache[i].blk == VB_INVALID_HANDLE) {
            printf("[ERROR] CVI_VB_GetBlock failed for block %d (size=%d)\n", i, g_cam_frame_size);
            return -1;
        }
        vb_cache[i].phy_addr = CVI_VB_Handle2PhysAddr(vb_cache[i].blk);
        vb_cache[i].vir_addr = (uint8_t *)CVI_SYS_Mmap(vb_cache[i].phy_addr, g_cam_frame_size);
        if (!vb_cache[i].vir_addr) {
            printf("[ERROR] CVI_SYS_Mmap failed for block %d\n", i);
            return -1;
        }
        vb_cache[i].in_use.store(0, std::memory_order_relaxed);
    }
    
    printf("[VB] Initialized with 4 pools (%d input blocks)\n", VB_POOL_CNT);
    return 0;
}

/**
 * @brief Get a free VB buffer using atomic compare-exchange (lock-free)
 */
int get_free_vb_buffer() {
    for (int i = 0; i < VB_POOL_CNT; i++) {
        int expected = 0;
        if (vb_cache[i].in_use.compare_exchange_strong(expected, 1, std::memory_order_acquire)) {
            return i;
        }
    }
    return -1;
}

void release_vb_buffer(int idx) {
    if (idx >= 0 && idx < VB_POOL_CNT)
        vb_cache[idx].in_use.store(0, std::memory_order_release);
}

/**
 * @brief Khởi tạo VPSS với 2 channels
 */
int vpss_init() {
    VPSS_GRP_ATTR_S grp_attr;
    memset(&grp_attr, 0, sizeof(grp_attr));
    grp_attr.u32MaxW = g_cam_w;
    grp_attr.u32MaxH = g_cam_h;
    grp_attr.enPixelFormat = PIXEL_FORMAT_YUYV;
    
    CVI_S32 ret = CVI_VPSS_CreateGrp(vpss_grp, &grp_attr);
    if (ret != CVI_SUCCESS) {
        printf("[ERROR] CVI_VPSS_CreateGrp failed: 0x%X\n", ret);
        return -1;
    }

    // Channel 0: YOLO (only if YOLO enabled)
    if (!g_no_yolo) {
        VPSS_CHN_ATTR_S chn0_attr;
        memset(&chn0_attr, 0, sizeof(chn0_attr));
        chn0_attr.u32Width  = g_yolo_w;
        chn0_attr.u32Height = g_yolo_h;
        chn0_attr.enPixelFormat = PIXEL_FORMAT_RGB_888_PLANAR;
        chn0_attr.u32Depth = 2;
        chn0_attr.bMirror = CVI_FALSE;
        chn0_attr.bFlip   = CVI_FALSE;
        chn0_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
        chn0_attr.stAspectRatio.u32BgColor = 0x000000;
        
        ret = CVI_VPSS_SetChnAttr(vpss_grp, VPSS_CHN_YOLO, &chn0_attr);
        if (ret != CVI_SUCCESS) {
            printf("[ERROR] CVI_VPSS_SetChnAttr CHN0 failed: 0x%X\n", ret);
            return -1;
        }
        CVI_VPSS_EnableChn(vpss_grp, VPSS_CHN_YOLO);
        printf("[VPSS] Channel 0 (YOLO): %dx%d RGB_888_PLANAR\n", g_yolo_w, g_yolo_h);
    }

    // Channel 1: Stream (dynamic resolution, NV12)
    VPSS_CHN_ATTR_S chn1_attr;
    memset(&chn1_attr, 0, sizeof(chn1_attr));
    chn1_attr.u32Width  = g_stream_w;
    chn1_attr.u32Height = g_stream_h;
    chn1_attr.enPixelFormat = PIXEL_FORMAT_NV12;
    chn1_attr.u32Depth = 1;  // Minimal depth: we consume immediately
    chn1_attr.bMirror = CVI_FALSE;
    chn1_attr.bFlip   = CVI_FALSE;
    chn1_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
    
    ret = CVI_VPSS_SetChnAttr(vpss_grp, VPSS_CHN_STREAM, &chn1_attr);
    if (ret != CVI_SUCCESS) {
        printf("[ERROR] CVI_VPSS_SetChnAttr CHN1 failed: 0x%X\n", ret);
        return -1;
    }
    CVI_VPSS_EnableChn(vpss_grp, VPSS_CHN_STREAM);
    printf("[VPSS] Channel 1 (Stream): %dx%d NV12\n", g_stream_w, g_stream_h);

    ret = CVI_VPSS_StartGrp(vpss_grp);
    if (ret != CVI_SUCCESS) {
        printf("[ERROR] CVI_VPSS_StartGrp failed: 0x%X\n", ret);
        return -1;
    }
    
    printf("[VPSS] Group started with 2 channels\n");
    return 0;
}

/**
 * @brief Khởi tạo VENC cho JPEG encoding (hardware)
 */
int venc_init() {
    VENC_CHN_ATTR_S venc_chn_attr;
    memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
    
    venc_chn_attr.stVencAttr.enType = PT_JPEG;
    venc_chn_attr.stVencAttr.u32MaxPicWidth = g_stream_w;
    venc_chn_attr.stVencAttr.u32MaxPicHeight = g_stream_h;
    venc_chn_attr.stVencAttr.u32PicWidth = g_stream_w;
    venc_chn_attr.stVencAttr.u32PicHeight = g_stream_h;
    venc_chn_attr.stVencAttr.u32BufSize = g_stream_nv12_size * 2;
    venc_chn_attr.stVencAttr.u32Profile = 0;
    venc_chn_attr.stVencAttr.bByFrame = CVI_TRUE;
    
    venc_chn_attr.stVencAttr.stAttrJpege.bSupportDCF = CVI_FALSE;
    venc_chn_attr.stVencAttr.stAttrJpege.stMPFCfg.u8LargeThumbNailNum = 0;
    venc_chn_attr.stVencAttr.stAttrJpege.enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
    
    CVI_S32 ret = CVI_VENC_CreateChn(VENC_CHN_JPEG, &venc_chn_attr);
    if (ret != CVI_SUCCESS) {
        printf("[ERROR] CVI_VENC_CreateChn failed: 0x%X\n", ret);
        return -1;
    }
    
    // Set JPEG quality
    VENC_JPEG_PARAM_S jpeg_param;
    memset(&jpeg_param, 0, sizeof(jpeg_param));
    CVI_VENC_GetJpegParam(VENC_CHN_JPEG, &jpeg_param);
    jpeg_param.u32Qfactor = g_jpeg_quality;
    CVI_VENC_SetJpegParam(VENC_CHN_JPEG, &jpeg_param);
    
    // Start receiving frames
    VENC_RECV_PIC_PARAM_S recv_param;
    recv_param.s32RecvPicNum = -1;
    ret = CVI_VENC_StartRecvFrame(VENC_CHN_JPEG, &recv_param);
    if (ret != CVI_SUCCESS) {
        printf("[ERROR] CVI_VENC_StartRecvFrame failed: 0x%X\n", ret);
        return -1;
    }
    
    printf("[VENC] JPEG encoder initialized (Quality: %d%%)\n", g_jpeg_quality);
    return 0;
}

/**
 * @brief Probe camera: open device, negotiate format, return actual resolution.
 * Must be called BEFORE VB/VPSS init so we know the real resolution.
 * The fd is kept open for v4l2_start() to use later.
 */
int v4l2_probe(int *fd) {
    *fd = open(CAM_DEV, O_RDWR);
    if (*fd < 0) {
        printf("[ERROR] Cannot open %s\n", CAM_DEV);
        return -1;
    }
    
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = g_cam_w;
    fmt.fmt.pix.height = g_cam_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    
    if (ioctl(*fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("[ERROR] VIDIOC_S_FMT failed\n");
        close(*fd);
        *fd = -1;
        return -1;
    }
    
    // V4L2 driver may adjust the resolution - MUST use actual values
    int actual_w = (int)fmt.fmt.pix.width;
    int actual_h = (int)fmt.fmt.pix.height;
    
    if (actual_w != g_cam_w || actual_h != g_cam_h) {
        printf("[WARN] Camera adjusted resolution: %dx%d -> %dx%d\n",
               g_cam_w, g_cam_h, actual_w, actual_h);
        
        // Update camera resolution to actual
        g_cam_w = actual_w;
        g_cam_h = actual_h;
        
        // If stream resolution exceeds actual camera, clamp it
        if (g_stream_w > g_cam_w) g_stream_w = g_cam_w;
        if (g_stream_h > g_cam_h) g_stream_h = g_cam_h;
        
        printf("[INFO] Updated: cam=%dx%d, stream=%dx%d\n",
               g_cam_w, g_cam_h, g_stream_w, g_stream_h);
    }
    
    // Try to set camera framerate to 30fps
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if (ioctl(*fd, VIDIOC_S_PARM, &parm) == 0) {
        float actual_fps = (float)parm.parm.capture.timeperframe.denominator / 
                           (float)parm.parm.capture.timeperframe.numerator;
        printf("[V4L2] Framerate: %.1f FPS\n", actual_fps);
    } else {
        printf("[V4L2] Could not set framerate (driver may not support it)\n");
    }
    
    // Probe USERPTR support for zero-copy
    struct v4l2_requestbuffers probe_req;
    memset(&probe_req, 0, sizeof(probe_req));
    probe_req.count = 1;
    probe_req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    probe_req.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(*fd, VIDIOC_REQBUFS, &probe_req) == 0) {
        g_use_userptr = true;
        printf("[V4L2] USERPTR mode supported -> zero-copy enabled\n");
    } else {
        g_use_userptr = false;
        printf("[V4L2] USERPTR not supported -> using MMAP + memcpy\n");
    }
    
    printf("[V4L2] Probed: %dx%d YUYV\n", g_cam_w, g_cam_h);
    return 0;
}

/**
 * @brief Start camera streaming: request buffers, mmap/userptr, start capture.
 * Must be called AFTER VB/VPSS init. fd must be from v4l2_probe().
 * 
 * Uses USERPTR mode if supported (zero-copy), otherwise falls back to MMAP.
 */
int v4l2_start(int fd) {
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUF_CNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (g_use_userptr) {
        // USERPTR mode: camera writes directly to VB buffers (zero-copy)
        req.memory = V4L2_MEMORY_USERPTR;
        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            printf("[WARN] USERPTR REQBUFS failed, falling back to MMAP\n");
            g_use_userptr = false;
        }
    }
    
    if (g_use_userptr) {
        // Queue VB buffers as USERPTR targets
        for (int i = 0; i < BUF_CNT && i < VB_POOL_CNT; i++) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_USERPTR;
            buf.index = i;
            buf.m.userptr = (unsigned long)vb_cache[i].vir_addr;
            buf.length = g_cam_frame_size;
            if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
                printf("[WARN] USERPTR QBUF failed for buf %d, falling back to MMAP\n", i);
                g_use_userptr = false;
                break;
            }
            cam_bufs[i].addr = vb_cache[i].vir_addr;
            cam_bufs[i].len = g_cam_frame_size;
        }
        if (g_use_userptr) {
            printf("[V4L2] USERPTR mode: zero-copy active\n");
        }
    }
    
    if (!g_use_userptr) {
        // MMAP mode: standard path
        memset(&req, 0, sizeof(req));
        req.count = BUF_CNT;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            printf("[ERROR] VIDIOC_REQBUFS failed\n");
            return -1;
        }
        for (int i = 0; i < BUF_CNT; i++) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = req.type;
            buf.memory = req.memory;
            buf.index = i;
            ioctl(fd, VIDIOC_QUERYBUF, &buf);
            cam_bufs[i].len = buf.length;
            cam_bufs[i].addr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
            if (cam_bufs[i].addr == MAP_FAILED) {
                printf("[ERROR] mmap failed for cam buffer %d\n", i);
                return -1;
            }
            ioctl(fd, VIDIOC_QBUF, &buf);
        }
        printf("[V4L2] MMAP mode: memcpy path\n");
    }
    
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        printf("[ERROR] VIDIOC_STREAMON failed\n");
        return -1;
    }
    
    printf("[V4L2] Stream started with %d buffers\n", BUF_CNT);
    return 0;
}

/**
 * @brief JPEG send thread: decouples network I/O from capture pipeline
 * Main loop caches JPEG, this thread broadcasts to all clients
 */
static int jpeg_debug_cnt = 0;

void* jpeg_send_thread_func(void* arg) {
    (void)arg;
    // Allocate send buffer (max possible JPEG size)
    int buf_capacity = 512 * 1024;  // 512KB should be enough for any JPEG
    void* send_buf = malloc(buf_capacity);
    int send_size = 0;
    
    while (running) {
        // Wait for new JPEG data
        pthread_mutex_lock(&jpeg_mutex);
        while (!g_jpeg_ready && running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;  // 100ms timeout
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&jpeg_cond, &jpeg_mutex, &ts);
        }
        if (!running) { pthread_mutex_unlock(&jpeg_mutex); break; }
        
        // Copy JPEG data to local buffer
        if (g_jpeg_size <= buf_capacity && g_jpeg_buf) {
            memcpy(send_buf, g_jpeg_buf, g_jpeg_size);
            send_size = g_jpeg_size;
        }
        g_jpeg_ready = false;
        pthread_mutex_unlock(&jpeg_mutex);
        
        if (send_size == 0) continue;
        
        // Now broadcast to all clients WITHOUT holding jpeg_mutex
        if (pthread_mutex_trylock(&client_mutex) != 0) {
            continue;  // Skip if client list is being modified
        }
        
        if (stream_client_count == 0) {
            pthread_mutex_unlock(&client_mutex);
            continue;
        }
        
        if (++jpeg_debug_cnt % 60 == 1) {
            printf("[JPEG] Size: %d bytes (%.1f KB) -> %d clients\n", 
                   send_size, send_size / 1024.0, stream_client_count);
        }
        
        char header[128];
        int header_len = snprintf(header, sizeof(header),
            "--myboundary\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %d\r\n\r\n",
            send_size);
        
        int alive = 0;
        for (int i = 0; i < stream_client_count; i++) {
            int fd = stream_clients[i];
            bool ok = send(fd, header, header_len, MSG_NOSIGNAL) >= 0 &&
                      send(fd, send_buf, send_size, MSG_NOSIGNAL) >= 0 &&
                      send(fd, "\r\n", 2, MSG_NOSIGNAL) >= 0;
            if (ok) {
                stream_clients[alive++] = fd;
            } else {
                printf("[Stream] Client %d disconnected\n", i);
                close(fd);
            }
        }
        stream_client_count = alive;
        pthread_mutex_unlock(&client_mutex);
    }
    
    free(send_buf);
    return NULL;
}

/**
 * @brief Queue JPEG data for async sending (non-blocking)
 * Called from main loop. Copies JPEG to shared buffer and signals send thread.
 */
void queue_jpeg_for_send(void *jpeg_data, int jpeg_size) {
    pthread_mutex_lock(&jpeg_mutex);
    if (!g_jpeg_buf) {
        g_jpeg_buf = malloc(512 * 1024);
    }
    if (g_jpeg_buf && jpeg_size <= 512 * 1024) {
        memcpy(g_jpeg_buf, jpeg_data, jpeg_size);
        g_jpeg_size = jpeg_size;
        g_jpeg_ready = true;
        pthread_cond_signal(&jpeg_cond);
    }
    pthread_mutex_unlock(&jpeg_mutex);
}

/**
 * @brief HTTP Server thread - chờ và xử lý client connections
 */
void* http_server_thread(void* arg) {
    (void)arg;
    
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        printf("[Stream] Cannot create socket\n");
        return NULL;
    }
    
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(STREAM_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(server, (sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[Stream] Cannot bind port %d\n", STREAM_PORT);
        close(server);
        return NULL;
    }
    
    listen(server, 5);
    
    std::string ip = get_ip();
    printf("\n");
    printf("========================================\n");
    printf("  MJPEG Stream: http://%s:%d\n", ip.c_str(), STREAM_PORT);
    printf("========================================\n\n");
    
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server, &fds);
        
        timeval tv = {1, 0};
        if (select(server + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        
        int client = accept(server, NULL, NULL);
        if (client < 0) continue;
        
        // Set TCP_NODELAY to reduce latency (disable Nagle's algorithm)
        int nodelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        
        // Set send timeout: prevents send() from blocking for 30+ seconds
        // on a dead/slow client, allowing fast handover to new client
        struct timeval snd_timeout = {0, 500000};  // 500ms
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));
        
        // Set larger send buffer for smoother streaming
        int sndbuf = 128 * 1024;
        setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        
        // Đọc HTTP request
        char request[512];
        int req_len = recv(client, request, sizeof(request) - 1, 0);
        if (req_len <= 0) {
            close(client);
            continue;
        }
        request[req_len] = '\0';
        
        // Favicon -> 404
        if (strstr(request, "favicon.ico") != NULL) {
            const char* not_found = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            send(client, not_found, strlen(not_found), 0);
            close(client);
            continue;
        }
        
        if (strstr(request, "GET /") == NULL) {
            close(client);
            continue;
        }
        
        printf("[Stream] Client connected\n");
        
        const char* http_header =
            "HTTP/1.0 200 OK\r\n"
            "Server: LicheeRV-Nano\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Pragma: no-cache\r\n"
            "Connection: close\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=myboundary\r\n\r\n";
        
        if (send(client, http_header, strlen(http_header), 0) < 0) {
            close(client);
            continue;
        }
        
        // Add new client to the list
        pthread_mutex_lock(&client_mutex);
        if (stream_client_count >= MAX_STREAM_CLIENTS) {
            // Full: close oldest client to make room
            printf("[Stream] Max clients reached, removing oldest\n");
            shutdown(stream_clients[0], SHUT_WR);
            close(stream_clients[0]);
            // Shift remaining clients down
            for (int i = 1; i < stream_client_count; i++) {
                stream_clients[i - 1] = stream_clients[i];
            }
            stream_client_count--;
        }
        stream_clients[stream_client_count++] = client;
        pthread_mutex_unlock(&client_mutex);
        printf("[Stream] Client added (total: %d/%d)\n", stream_client_count, MAX_STREAM_CLIENTS);
    }
    
    close(server);
    return NULL;
}

/**
 * @brief YOLO Inference Thread - Runs INDEPENDENTLY from main/stream loop
 * 
 * This thread continuously fetches frames from VPSS Channel 0 (YOLO)
 * and performs YOLO detection. It does NOT block the stream pipeline.
 * 
 * Performance optimization: When YOLO is slower than capture rate, 
 * we drain stale frames to always process the freshest frame available.
 */
void* yolo_inference_thread(void* arg) {
    (void)arg;
    
    // Set thread priority (not too high to avoid starving stream)
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO) - 2;
    // Note: SCHED_FIFO may fail without root - that's OK, we continue
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        printf("[YOLO Thread] Warning: Could not set RT priority (non-root?)\n");
    }
    
    printf("[YOLO Thread] Started (input: %dx%d)\n", g_yolo_w, g_yolo_h);
    
    while (running) {
        VIDEO_FRAME_INFO_S frame_yolo;
        
        // Get frame from YOLO channel
        if (CVI_VPSS_GetChnFrame(vpss_grp, VPSS_CHN_YOLO, &frame_yolo, YOLO_TIMEOUT) == CVI_SUCCESS) {
            
            // Drain stale frames: if more frames are queued, skip to newest
            // This ensures we always run inference on the most recent frame
            VIDEO_FRAME_INFO_S frame_newer;
            int drained = 0;
            while (CVI_VPSS_GetChnFrame(vpss_grp, VPSS_CHN_YOLO, &frame_newer, 0) == CVI_SUCCESS) {
                CVI_VPSS_ReleaseChnFrame(vpss_grp, VPSS_CHN_YOLO, &frame_yolo);
                frame_yolo = frame_newer;
                drained++;
            }
            if (drained > 0) {
                // This is normal under high capture rate - not an error
            }
            
            // Perform YOLO detection
            cvtdl_object_t obj_meta = {0};
            CVI_TDL_YOLOV8_Detection(tdl_handle, &frame_yolo, &obj_meta);
            
            yolo_object_cnt.fetch_add(obj_meta.size, std::memory_order_relaxed);
            yolo_frame_cnt.fetch_add(1, std::memory_order_relaxed);
            
            // Print detection results (only when objects detected, throttled)
            if (obj_meta.size > 0) {
                static int detect_log_cnt = 0;
                if (++detect_log_cnt % 5 == 1) {  // Log every 5th detection
                    printf("[YOLO] %d objects: ", obj_meta.size);
                    for (uint32_t i = 0; i < obj_meta.size && i < 3; i++) {
                        printf("C%d(%.2f) ", obj_meta.info[i].classes, obj_meta.info[i].bbox.score);
                    }
                    if (obj_meta.size > 3) printf("...");
                    printf("\n");
                }
            }
            
            // Send metadata (even if 0 objects)
            send_yolo_metadata(&obj_meta);
            
            // Send to UART
            uart_send_detections(&obj_meta);
            
            // Control Detection LED (P19) based on YOLO output
            detect_led_set(obj_meta.size > 0 ? 1 : 0);
            
            CVI_TDL_Free(&obj_meta);
            CVI_VPSS_ReleaseChnFrame(vpss_grp, VPSS_CHN_YOLO, &frame_yolo);
        }
    }
    
    detect_led_set(0);  // Turn off detection LED when stopping
    printf("[YOLO Thread] Stopped\n");
    return NULL;
}

void cleanup() {
    running = 0;
    
    // Cleanup LED and UART
    led_cleanup();
    uart_cleanup();
    
    // Close all stream clients
    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < stream_client_count; i++) {
        close(stream_clients[i]);
    }
    stream_client_count = 0;
    pthread_mutex_unlock(&client_mutex);
    
    // Destroy TDL handle
    if (tdl_handle) {
        CVI_TDL_DestroyHandle(tdl_handle);
        tdl_handle = NULL;
    }
    
    // Close metadata socket
    if (metadata_socket >= 0) {
        close(metadata_socket);
        metadata_socket = -1;
    }
    
    // Stop VENC
    CVI_VENC_StopRecvFrame(VENC_CHN_JPEG);
    CVI_VENC_DestroyChn(VENC_CHN_JPEG);
    
    // Stop VPSS
    if (!g_no_yolo) {
        CVI_VPSS_DisableChn(vpss_grp, VPSS_CHN_YOLO);
    }
    CVI_VPSS_DisableChn(vpss_grp, VPSS_CHN_STREAM);
    CVI_VPSS_StopGrp(vpss_grp);
    CVI_VPSS_DestroyGrp(vpss_grp);
    
    // Release VB
    for (int i = 0; i < VB_POOL_CNT; i++) {
        if (vb_cache[i].vir_addr) 
            CVI_SYS_Munmap(vb_cache[i].vir_addr, g_cam_frame_size);
        if (vb_cache[i].blk != VB_INVALID_HANDLE) 
            CVI_VB_ReleaseBlock(vb_cache[i].blk);
    }
    CVI_VB_Exit();
    CVI_SYS_Exit();
    printf("\n[Cleanup] Done.\n");
}

void signal_handler(int sig) {
    printf("\n[Signal] Received signal %d, cleaning up...\n", sig);
    cleanup();
    exit(0);
}

// ========== MAIN ==========
int main(int argc, char *argv[]) {
    // Parse command-line arguments
    const char* model_path = parse_args(argc, argv);
    if (!model_path && !g_no_yolo) return -1;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // === STEP 0: Probe camera FIRST to get actual resolution ===
    // Camera may not support requested resolution and will adjust it.
    // We MUST know the real resolution before allocating VB pools.
    printf("[INIT] Probing Camera...\n");
    int cam_fd;
    if (v4l2_probe(&cam_fd) < 0) {
        printf("[FATAL] Camera probe failed\n");
        return -1;
    }

    // Now compute stride-aligned frame sizes with ACTUAL resolution
    g_cam_stride      = align_up(g_cam_w * 2, STRIDE_ALIGN);
    g_cam_frame_size  = g_cam_stride * g_cam_h;
    int yolo_plane_stride = align_up(g_yolo_w, STRIDE_ALIGN);
    g_yolo_frame_size = yolo_plane_stride * g_yolo_h * 3;
    int stream_y_stride = align_up(g_stream_w, STRIDE_ALIGN);
    g_stream_nv12_size = stream_y_stride * g_stream_h * 3 / 2;

    printf("\n");
    printf("================================================\n");
    printf("  VPSS Dual Channel + YOLO + MJPEG Stream\n");
    printf("  Camera:  %dx%d YUYV (stride=%d)\n", g_cam_w, g_cam_h, g_cam_stride);
    if (!g_no_yolo) {
        printf("  YOLO:    %dx%d RGB_888_PLANAR\n", g_yolo_w, g_yolo_h);
    }
    printf("  Stream:  %dx%d NV12 -> JPEG (Q=%d%%)\n", g_stream_w, g_stream_h, g_jpeg_quality);
    if (!g_no_yolo) {
        printf("  Model:   %s\n", model_path);
        printf("  Conf:    %.2f  NMS: %.2f\n", g_conf_thresh, g_nms_thresh);
    } else {
        printf("  Mode:    Stream-only (no YOLO)\n");
    }
    printf("================================================\n\n");

    // 1. Initialize VB pools (uses actual camera resolution)
    printf("[INIT] Setting up Video Buffers...\n");
    if (sys_vb_init() < 0) {
        printf("[FATAL] VB init failed\n");
        close(cam_fd);
        return -1;
    }

    // 2. Initialize VPSS
    printf("[INIT] Setting up VPSS...\n");
    if (vpss_init() < 0) {
        printf("[FATAL] VPSS init failed\n");
        close(cam_fd);
        cleanup();
        return -1;
    }

    // 3. Initialize VENC (JPEG encoder)
    printf("[INIT] Setting up VENC...\n");
    if (venc_init() < 0) {
        printf("[FATAL] VENC init failed\n");
        close(cam_fd);
        cleanup();
        return -1;
    }

    // 4. Initialize TDL (YOLO) - skip if --no-yolo
    pthread_t yolo_thread;
    bool yolo_thread_started = false;
    if (!g_no_yolo) {
        printf("[INIT] Setting up TDL (YOLO)...\n");
        if (CVI_TDL_CreateHandle(&tdl_handle) != CVI_SUCCESS) {
            printf("[FATAL] CVI_TDL_CreateHandle failed\n");
            close(cam_fd);
            cleanup();
            return -1;
        }
        CVI_TDL_SetVpssTimeout(tdl_handle, 100);
        init_yolo_params(tdl_handle);
        
        if (CVI_TDL_OpenModel(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, model_path) != CVI_SUCCESS) {
            printf("[FATAL] CVI_TDL_OpenModel failed: %s\n", model_path);
            close(cam_fd);
            cleanup();
            return -1;
        }
        printf("[TDL] YOLO model loaded: %s\n", model_path);
    } else {
        printf("[INIT] Skipping YOLO (--no-yolo mode)\n");
    }
    
    // 4.5 Initialize metadata broadcast socket
    if (!g_no_yolo) {
        init_metadata_socket();
    }
    
    // 4.6 Initialize External LEDs (P18 and P19)
    led_init();
    
    // 4.7 Initialize UART serial output
    if (!g_no_yolo && g_uart_dev) {
        uart_init();
    }

    // 5. Start camera streaming (buffers mmap + STREAMON)
    printf("[INIT] Starting Camera stream...\n");
    if (v4l2_start(cam_fd) < 0) {
        printf("[FATAL] Camera start failed\n");
        close(cam_fd);
        cleanup();
        return -1;
    }

    // 6. Start HTTP server thread
    pthread_t http_thread;
    pthread_create(&http_thread, NULL, http_server_thread, NULL);
    
    // 6.5 Start JPEG send thread
    pthread_create(&jpeg_send_thread_id, NULL, jpeg_send_thread_func, NULL);
    
    // 7. Start YOLO inference thread (if enabled)
    if (!g_no_yolo) {
        pthread_create(&yolo_thread, NULL, yolo_inference_thread, NULL);
        yolo_thread_started = true;
    }

    // Turn on Status LED (A24) - program initialized successfully
    status_led_set(1);
    printf("[LED] Status LED ON (A24) - system ready\n");

    printf("\n[RUNNING] %s mode started. Press Ctrl+C to exit.\n",
           g_no_yolo ? "Stream-only" : "Parallel");
    if (!g_no_yolo) {
        printf("[INFO] YOLO runs in separate thread - Stream is NOT blocked!\n");
    }
    printf("\n");

    double last_time = get_monotonic_time();
    int frame_cnt = 0;
    int skip_cnt = 0;

    // Pre-initialize frame_in ONCE (optimization #2: avoid memset every frame)
    VIDEO_FRAME_INFO_S frame_in;
    memset(&frame_in, 0, sizeof(frame_in));
    frame_in.stVFrame.u32Width = g_cam_w;
    frame_in.stVFrame.u32Height = g_cam_h;
    frame_in.stVFrame.enPixelFormat = PIXEL_FORMAT_YUYV;
    frame_in.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
    frame_in.stVFrame.u32Stride[0] = g_cam_w * 2;
    frame_in.stVFrame.u32Stride[1] = 0;
    frame_in.stVFrame.u32Stride[2] = 0;

    while (running) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = g_use_userptr ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;

        // 1. Dequeue frame từ camera
        if (ioctl(cam_fd, VIDIOC_DQBUF, &buf) < 0) {
            usleep(1000);
            continue;
        }

        int vb_idx;
        size_t copy_len;
        
        if (g_use_userptr) {
            // USERPTR: camera wrote directly to VB buffer, find which one
            vb_idx = buf.index;
            if (vb_idx >= VB_POOL_CNT) { ioctl(cam_fd, VIDIOC_QBUF, &buf); continue; }
            copy_len = buf.bytesused > 0 ? buf.bytesused : (size_t)g_cam_frame_size;
            // Flush cache (still needed even with USERPTR)
            CVI_SYS_IonFlushCache(vb_cache[vb_idx].phy_addr, vb_cache[vb_idx].vir_addr, copy_len);
        } else {
            // MMAP: need to copy to VB buffer
            vb_idx = get_free_vb_buffer();
            if (vb_idx < 0) {
                ioctl(cam_fd, VIDIOC_QBUF, &buf);
                skip_cnt++;
                continue;
            }
            copy_len = cam_bufs[buf.index].len;
            if (copy_len > (size_t)g_cam_frame_size) copy_len = g_cam_frame_size;
            memcpy(vb_cache[vb_idx].vir_addr, cam_bufs[buf.index].addr, copy_len);
            CVI_SYS_IonFlushCache(vb_cache[vb_idx].phy_addr, vb_cache[vb_idx].vir_addr, copy_len);
        }
        
        ioctl(cam_fd, VIDIOC_QBUF, &buf);

        // 3. Update only the per-frame fields (rest is pre-initialized)
        frame_in.stVFrame.u64PhyAddr[0] = vb_cache[vb_idx].phy_addr;
        frame_in.stVFrame.pu8VirAddr[0] = vb_cache[vb_idx].vir_addr;
        frame_in.stVFrame.u32Length[0] = copy_len;
        struct timeval tv_pts;
        gettimeofday(&tv_pts, NULL);
        frame_in.stVFrame.u64PTS = tv_pts.tv_sec * 1000000ULL + tv_pts.tv_usec;

        // 4. Gửi frame vào VPSS (broadcasts to both channels)
        if (CVI_VPSS_SendFrame(vpss_grp, &frame_in, API_TIMEOUT) != CVI_SUCCESS) {
            release_vb_buffer(vb_idx);
            continue;
        }

        // 5. Lấy frame từ Channel 1 (Stream) -> VENC -> HTTP
        VIDEO_FRAME_INFO_S frame_stream;
        if (CVI_VPSS_GetChnFrame(vpss_grp, VPSS_CHN_STREAM, &frame_stream, API_TIMEOUT) == CVI_SUCCESS) {
            CVI_S32 venc_ret = CVI_VENC_SendFrame(VENC_CHN_JPEG, &frame_stream, API_TIMEOUT);
            if (venc_ret == CVI_SUCCESS) {
                VENC_CHN_STATUS_S stStat;
                memset(&stStat, 0, sizeof(stStat));
                
                venc_ret = CVI_VENC_QueryStatus(VENC_CHN_JPEG, &stStat);
                if (venc_ret == CVI_SUCCESS && stStat.u32CurPacks > 0 && stStat.u32CurPacks <= 8) {
                    VENC_STREAM_S venc_stream;
                    memset(&venc_stream, 0, sizeof(venc_stream));
                    venc_stream.pstPack = g_venc_pack;
                    
                    venc_ret = CVI_VENC_GetStream(VENC_CHN_JPEG, &venc_stream, API_TIMEOUT);
                    if (venc_ret == CVI_SUCCESS && venc_stream.u32PackCount > 0) {
                        void *jpeg_data = venc_stream.pstPack[0].pu8Addr + venc_stream.pstPack[0].u32Offset;
                        int jpeg_size = venc_stream.pstPack[0].u32Len - venc_stream.pstPack[0].u32Offset;
                        
                        queue_jpeg_for_send(jpeg_data, jpeg_size);
                        stream_frame_cnt.fetch_add(1, std::memory_order_relaxed);
                    }
                    CVI_VENC_ReleaseStream(VENC_CHN_JPEG, &venc_stream);
                }
            }
            CVI_VPSS_ReleaseChnFrame(vpss_grp, VPSS_CHN_STREAM, &frame_stream);
        }

        if (!g_use_userptr) {
            release_vb_buffer(vb_idx);
        }

        // 6. Hiển thị FPS stats
        frame_cnt++;
        double current_time = get_monotonic_time();
        if (current_time - last_time >= 2.0) {  // Every 2 seconds to reduce log spam
            double elapsed = current_time - last_time;
            
            int yolo_frames = yolo_frame_cnt.exchange(0);
            int yolo_objects = yolo_object_cnt.exchange(0);
            int stream_frames = stream_frame_cnt.exchange(0);
            
            printf("[Stats] Cap:%.1f | Stream:%.1f | YOLO:%.1f fps | Objs:%d",
                   frame_cnt / elapsed,
                   stream_frames / elapsed,
                   yolo_frames / elapsed,
                   yolo_objects);
            if (skip_cnt > 0) {
                printf(" | Skip:%d", skip_cnt);
                skip_cnt = 0;
            }
            printf("\n");
            
            last_time = current_time;
            frame_cnt = 0;
        }
    }

    close(cam_fd);
    
    // Signal send thread to stop
    pthread_mutex_lock(&jpeg_mutex);
    g_jpeg_ready = true;  // Wake up send thread
    pthread_cond_signal(&jpeg_cond);
    pthread_mutex_unlock(&jpeg_mutex);
    
    pthread_join(http_thread, NULL);
    pthread_join(jpeg_send_thread_id, NULL);
    if (yolo_thread_started) {
        pthread_join(yolo_thread, NULL);
    }
    
    // Free JPEG buffer
    if (g_jpeg_buf) { free(g_jpeg_buf); g_jpeg_buf = NULL; }
    
    cleanup();
    return 0;
}