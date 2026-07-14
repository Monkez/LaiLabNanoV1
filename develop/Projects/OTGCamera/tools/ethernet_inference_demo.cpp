/**
 * Ethernet-to-TPU benchmark for LicheeRV Nano.
 *
 * Input protocol (TCP): repeated 20-byte network-order header plus either
 * 320x320 RGB888 planar or NV12 payload. Three DMA buffers overlap TCP
 * reception with VPSS conversion and TPU inference.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vpss.h"
#include "cvi_tdl.h"
#include "core/utils/vpss_helper.h"

static const uint32_t kMagic = 0x4c4e4631;  // "LNF1"
static const int kWidth = 320;
static const int kHeight = 320;
static const int kPlaneBytes = kWidth * kHeight;
static const int kRgbFrameBytes = kPlaneBytes * 3;
static const int kNv12FrameBytes = kPlaneBytes * 3 / 2;
static const int kSlotCount = 3;
static const VPSS_GRP kVpssGroup = 1;
static const VPSS_CHN kVpssChannel = 0;

enum InputFormat { INPUT_RGB, INPUT_NV12 };

static volatile sig_atomic_t g_running = 1;

static uint64_t monotonic_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static struct timespec wait_deadline_ms(long milliseconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += milliseconds * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    return deadline;
}

static uint64_t ntoh64(uint64_t value) {
    const uint32_t high = ntohl((uint32_t)(value >> 32));
    const uint32_t low = ntohl((uint32_t)(value & 0xffffffffU));
    return ((uint64_t)low << 32) | high;
}

static void on_signal(int) {
    g_running = 0;
}

#pragma pack(push, 1)
struct WireHeader {
    uint32_t magic;
    uint32_t frame_id;
    uint64_t sender_time_us;
    uint32_t payload_size;
};

struct UartHeader {
    uint16_t magic;
    uint8_t version;
    uint8_t flags;
    uint32_t frame_id;
    uint32_t inference_us;
    uint16_t object_count;
    uint16_t dropped_results;
};

struct UartObject {
    uint16_t class_id;
    uint16_t score_q15;
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
};
#pragma pack(pop)

static uint16_t crc16_ccitt(const uint8_t *data, size_t size) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default: return B921600;
    }
}

static int open_uart(const char *path, int baud) {
    if (!path || strcmp(path, "none") == 0) return -1;
    int fd = open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        printf("[UART] Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        printf("[UART] tcgetattr failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    cfsetospeed(&tty, baud_to_speed(baud));
    cfsetispeed(&tty, baud_to_speed(baud));
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CLOCAL;
    tty.c_oflag &= ~OPOST;
    tty.c_lflag = 0;
    tty.c_iflag = 0;
    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCOFLUSH);
    printf("[UART] Binary results: %s @ %d baud\n", path, baud);
    return fd;
}

enum SlotState { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_INFER };

struct FrameSlot {
    VB_BLK block;
    uint64_t physical;
    uint8_t *virtual_addr;
    uint32_t mapped_size;
    VIDEO_FRAME_INFO_S frame;
    SlotState state;
    uint32_t frame_id;
    uint64_t sender_time_us;
    uint64_t received_time_us;
};

struct SharedState {
    FrameSlot slots[kSlotCount];
    pthread_mutex_t mutex;
    pthread_cond_t free_cond;
    pthread_cond_t ready_cond;
    uint64_t received;
    uint64_t network_errors;
    int listen_fd;
    int client_fd;
    int port;
    InputFormat input_format;
    uint32_t payload_bytes;
};

static bool recv_exact(int fd, void *destination, size_t size) {
    uint8_t *cursor = static_cast<uint8_t *>(destination);
    while (size > 0 && g_running) {
        ssize_t count = recv(fd, cursor, size, 0);
        if (count > 0) {
            cursor += count;
            size -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return size == 0;
}

static int acquire_free_slot(SharedState *shared) {
    pthread_mutex_lock(&shared->mutex);
    while (g_running) {
        for (int i = 0; i < kSlotCount; ++i) {
            if (shared->slots[i].state == SLOT_FREE) {
                shared->slots[i].state = SLOT_FILLING;
                pthread_mutex_unlock(&shared->mutex);
                return i;
            }
        }
        struct timespec deadline = wait_deadline_ms(200);
        pthread_cond_timedwait(&shared->free_cond, &shared->mutex, &deadline);
    }
    pthread_mutex_unlock(&shared->mutex);
    return -1;
}

static int create_server(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int receive_buffer = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *receiver_thread(void *argument) {
    SharedState *shared = static_cast<SharedState *>(argument);
    shared->listen_fd = create_server(shared->port);
    if (shared->listen_fd < 0) {
        printf("[NET] Cannot listen on TCP %d: %s\n", shared->port, strerror(errno));
        g_running = 0;
        pthread_cond_broadcast(&shared->ready_cond);
        return NULL;
    }
    printf("[NET] Waiting for %s stream on TCP %d\n",
           shared->input_format == INPUT_NV12 ? "NV12" : "RGB planar", shared->port);
    while (g_running) {
        struct sockaddr_in peer;
        socklen_t peer_size = sizeof(peer);
        int client = accept(shared->listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_size);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        shared->client_fd = client;
        printf("[NET] Sender connected: %s\n", inet_ntoa(peer.sin_addr));

        while (g_running) {
            WireHeader wire;
            if (!recv_exact(client, &wire, sizeof(wire))) break;
            const uint32_t magic = ntohl(wire.magic);
            const uint32_t frame_id = ntohl(wire.frame_id);
            const uint32_t payload_size = ntohl(wire.payload_size);
            if (magic != kMagic || payload_size != shared->payload_bytes) {
                printf("[NET] Invalid frame header magic=0x%08x size=%u\n", magic, payload_size);
                shared->network_errors++;
                break;
            }
            int slot_index = acquire_free_slot(shared);
            if (slot_index < 0) break;
            FrameSlot &slot = shared->slots[slot_index];
            if (!recv_exact(client, slot.virtual_addr, shared->payload_bytes)) {
                pthread_mutex_lock(&shared->mutex);
                slot.state = SLOT_FREE;
                pthread_cond_signal(&shared->free_cond);
                pthread_mutex_unlock(&shared->mutex);
                break;
            }
            CVI_SYS_IonFlushCache(slot.physical, slot.virtual_addr, shared->payload_bytes);
            slot.frame_id = frame_id;
            slot.sender_time_us = ntoh64(wire.sender_time_us);
            slot.received_time_us = monotonic_us();
            slot.frame.stVFrame.u64PTS = slot.received_time_us;

            pthread_mutex_lock(&shared->mutex);
            slot.state = SLOT_READY;
            shared->received++;
            pthread_cond_signal(&shared->ready_cond);
            pthread_mutex_unlock(&shared->mutex);
        }
        close(client);
        shared->client_fd = -1;
        printf("[NET] Sender disconnected\n");
    }
    return NULL;
}

static int acquire_ready_slot(SharedState *shared) {
    pthread_mutex_lock(&shared->mutex);
    while (g_running) {
        int selected = -1;
        uint32_t oldest = 0;
        for (int i = 0; i < kSlotCount; ++i) {
            if (shared->slots[i].state == SLOT_READY &&
                (selected < 0 || shared->slots[i].frame_id < oldest)) {
                selected = i;
                oldest = shared->slots[i].frame_id;
            }
        }
        if (selected >= 0) {
            shared->slots[selected].state = SLOT_INFER;
            pthread_mutex_unlock(&shared->mutex);
            return selected;
        }
        struct timespec deadline = wait_deadline_ms(200);
        pthread_cond_timedwait(&shared->ready_cond, &shared->mutex, &deadline);
    }
    pthread_mutex_unlock(&shared->mutex);
    return -1;
}

static void release_slot(SharedState *shared, int index) {
    pthread_mutex_lock(&shared->mutex);
    shared->slots[index].state = SLOT_FREE;
    pthread_cond_signal(&shared->free_cond);
    pthread_mutex_unlock(&shared->mutex);
}

static int init_buffers(SharedState *shared) {
    const PIXEL_FORMAT_E input_pixel_format = shared->input_format == INPUT_NV12
                                                   ? PIXEL_FORMAT_NV12
                                                   : PIXEL_FORMAT_RGB_888_PLANAR;
    const uint32_t block_size = COMMON_GetPicBufferSize(
        kWidth, kHeight, input_pixel_format, DATA_BITWIDTH_8,
        COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    const uint32_t rgb_block_size = COMMON_GetPicBufferSize(
        kWidth, kHeight, PIXEL_FORMAT_RGB_888_PLANAR, DATA_BITWIDTH_8,
        COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    printf("[VB] Input=%s payload=%u block=%u bytes\n",
           shared->input_format == INPUT_NV12 ? "NV12" : "RGB planar",
           shared->payload_bytes, block_size);
    CVI_SYS_Exit();
    CVI_VB_Exit();
    VB_CONFIG_S config;
    memset(&config, 0, sizeof(config));
    config.u32MaxPoolCnt = shared->input_format == INPUT_NV12 ? 2 : 1;
    config.astCommPool[0].u32BlkSize = block_size;
    config.astCommPool[0].u32BlkCnt = kSlotCount;
    config.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
    if (shared->input_format == INPUT_NV12) {
        config.astCommPool[1].u32BlkSize = rgb_block_size;
        config.astCommPool[1].u32BlkCnt = kSlotCount;
        config.astCommPool[1].enRemapMode = VB_REMAP_MODE_NOCACHE;
    }
    if (CVI_VB_SetConfig(&config) != CVI_SUCCESS ||
        CVI_VB_Init() != CVI_SUCCESS || CVI_SYS_Init() != CVI_SUCCESS)
        return CVI_FAILURE;
    for (int i = 0; i < kSlotCount; ++i) {
        FrameSlot &slot = shared->slots[i];
        memset(&slot, 0, sizeof(slot));
        slot.block = CVI_VB_GetBlock(0, block_size);
        if (slot.block == VB_INVALID_HANDLE) return CVI_FAILURE;
        slot.physical = CVI_VB_Handle2PhysAddr(slot.block);
        slot.mapped_size = block_size;
        slot.virtual_addr = static_cast<uint8_t *>(CVI_SYS_Mmap(slot.physical, block_size));
        if (!slot.virtual_addr) return CVI_FAILURE;
        slot.state = SLOT_FREE;

        VIDEO_FRAME_S &frame = slot.frame.stVFrame;
        frame.u32Width = kWidth;
        frame.u32Height = kHeight;
        frame.enPixelFormat = input_pixel_format;
        frame.enVideoFormat = VIDEO_FORMAT_LINEAR;
        if (shared->input_format == INPUT_NV12) {
            frame.u32Stride[0] = kWidth;
            frame.u32Stride[1] = kWidth;
            frame.u32Length[0] = kPlaneBytes;
            frame.u32Length[1] = kPlaneBytes / 2;
            frame.u64PhyAddr[0] = slot.physical;
            frame.u64PhyAddr[1] = slot.physical + kPlaneBytes;
            frame.pu8VirAddr[0] = slot.virtual_addr;
            frame.pu8VirAddr[1] = slot.virtual_addr + kPlaneBytes;
        } else {
            for (int plane = 0; plane < 3; ++plane) {
                frame.u32Stride[plane] = kWidth;
                frame.u32Length[plane] = kPlaneBytes;
                frame.u64PhyAddr[plane] = slot.physical + (uint64_t)plane * kPlaneBytes;
                frame.pu8VirAddr[plane] = slot.virtual_addr + plane * kPlaneBytes;
            }
        }
    }
    return CVI_SUCCESS;
}

static int init_vpss_conversion() {
    VPSS_GRP_ATTR_S group_attr;
    memset(&group_attr, 0, sizeof(group_attr));
    group_attr.u32MaxW = kWidth;
    group_attr.u32MaxH = kHeight;
    group_attr.enPixelFormat = PIXEL_FORMAT_NV12;
    CVI_S32 result = CVI_VPSS_CreateGrp(kVpssGroup, &group_attr);
    if (result != CVI_SUCCESS) {
        printf("[VPSS] Create group %d failed: 0x%x\n", kVpssGroup, result);
        return result;
    }

    VPSS_CHN_ATTR_S channel_attr;
    memset(&channel_attr, 0, sizeof(channel_attr));
    channel_attr.u32Width = kWidth;
    channel_attr.u32Height = kHeight;
    channel_attr.enPixelFormat = PIXEL_FORMAT_RGB_888_PLANAR;
    channel_attr.u32Depth = 2;
    channel_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
    result = CVI_VPSS_SetChnAttr(kVpssGroup, kVpssChannel, &channel_attr);
    if (result == CVI_SUCCESS)
        result = CVI_VPSS_EnableChn(kVpssGroup, kVpssChannel);
    if (result == CVI_SUCCESS)
        result = CVI_VPSS_StartGrp(kVpssGroup);
    if (result != CVI_SUCCESS) {
        printf("[VPSS] Configure group %d failed: 0x%x\n", kVpssGroup, result);
        CVI_VPSS_DestroyGrp(kVpssGroup);
        return result;
    }
    printf("[VPSS] NV12 -> RGB planar on group %d channel %d\n",
           kVpssGroup, kVpssChannel);
    return CVI_SUCCESS;
}

static void stop_vpss_conversion() {
    CVI_VPSS_DisableChn(kVpssGroup, kVpssChannel);
    CVI_VPSS_StopGrp(kVpssGroup);
    CVI_VPSS_DestroyGrp(kVpssGroup);
}

static void release_buffers(SharedState *shared) {
    for (int i = 0; i < kSlotCount; ++i) {
        FrameSlot &slot = shared->slots[i];
        if (slot.virtual_addr) CVI_SYS_Munmap(slot.virtual_addr, slot.mapped_size);
        if (slot.block != VB_INVALID_HANDLE) CVI_VB_ReleaseBlock(slot.block);
        slot.virtual_addr = NULL;
        slot.block = VB_INVALID_HANDLE;
    }
    CVI_SYS_Exit();
    CVI_VB_Exit();
}

static int init_yolo(cvitdl_handle_t *handle, const char *model_path, bool skip_vpss) {
    if (CVI_TDL_CreateHandle(handle) != CVI_SUCCESS) return CVI_FAILURE;
    YoloPreParam preprocess = CVI_TDL_Get_YOLO_Preparam(*handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);
    for (int i = 0; i < 3; ++i) {
        preprocess.factor[i] = 1.0f;
        preprocess.mean[i] = 0.0f;
    }
    preprocess.format = PIXEL_FORMAT_RGB_888_PLANAR;
    CVI_TDL_Set_YOLO_Preparam(*handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, preprocess);
    YoloAlgParam algorithm = CVI_TDL_Get_YOLO_Algparam(*handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);
    algorithm.cls = 80;
    CVI_TDL_Set_YOLO_Algparam(*handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, algorithm);
    CVI_TDL_SetModelThreshold(*handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, 0.5f);
    CVI_TDL_SetModelNmsThreshold(*handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, 0.5f);
    CVI_TDL_SetSkipVpssPreprocess(*handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, skip_vpss);
    CVI_S32 result = CVI_TDL_OpenModel(*handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, model_path);
    if (result == CVI_SUCCESS)
        printf("[TPU] Model loaded; VPSS preprocess=%s\n", skip_vpss ? "skipped" : "enabled");
    return result;
}

static void send_uart_result(int fd, uint32_t frame_id, uint32_t inference_us,
                             const cvtdl_object_t &objects, uint16_t *dropped) {
    if (fd < 0) return;
    uint8_t packet[sizeof(UartHeader) + 10 * sizeof(UartObject) + 2];
    UartHeader header;
    header.magic = 0x4f59;  // bytes "YO" on little endian
    header.version = 1;
    header.flags = 0;
    header.frame_id = frame_id;
    header.inference_us = inference_us;
    header.object_count = (uint16_t)(objects.size > 10 ? 10 : objects.size);
    header.dropped_results = *dropped;
    memcpy(packet, &header, sizeof(header));
    size_t offset = sizeof(header);
    for (uint16_t i = 0; i < header.object_count; ++i) {
        const cvtdl_object_info_t &source = objects.info[i];
        UartObject object;
        object.class_id = (uint16_t)source.classes;
        float score = source.bbox.score < 0 ? 0 : (source.bbox.score > 1 ? 1 : source.bbox.score);
        object.score_q15 = (uint16_t)(score * 32767.0f);
        object.x1 = (uint16_t)source.bbox.x1;
        object.y1 = (uint16_t)source.bbox.y1;
        object.x2 = (uint16_t)source.bbox.x2;
        object.y2 = (uint16_t)source.bbox.y2;
        memcpy(packet + offset, &object, sizeof(object));
        offset += sizeof(object);
    }
    const uint16_t crc = crc16_ccitt(packet, offset);
    memcpy(packet + offset, &crc, sizeof(crc));
    offset += sizeof(crc);
    ssize_t written = write(fd, packet, offset);
    if (written != (ssize_t)offset) (*dropped)++;
    else *dropped = 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s MODEL [--input rgb|nv12] [--port 9000] "
               "[--uart /dev/ttyS0|none] [--baud 921600] [--skip-vpss]\n", argv[0]);
        return 2;
    }
    const char *model_path = argv[1];
    const char *uart_path = "/dev/ttyS0";
    int baud = 921600;
    int port = 9000;
    bool skip_vpss = false;
    InputFormat input_format = INPUT_RGB;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--uart") == 0 && i + 1 < argc) uart_path = argv[++i];
        else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) baud = atoi(argv[++i]);
        else if (strcmp(argv[i], "--skip-vpss") == 0) skip_vpss = true;
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            const char *format = argv[++i];
            if (strcmp(format, "rgb") == 0) input_format = INPUT_RGB;
            else if (strcmp(format, "nv12") == 0) input_format = INPUT_NV12;
            else {
                printf("--input must be rgb or nv12\n");
                return 2;
            }
        }
        else {
            printf("Unknown option: %s\n", argv[i]);
            return 2;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    SharedState shared;
    memset(&shared, 0, sizeof(shared));
    shared.listen_fd = -1;
    shared.client_fd = -1;
    shared.port = port;
    shared.input_format = input_format;
    shared.payload_bytes = input_format == INPUT_NV12 ? kNv12FrameBytes : kRgbFrameBytes;
    pthread_mutex_init(&shared.mutex, NULL);
    pthread_cond_init(&shared.free_cond, NULL);
    pthread_cond_init(&shared.ready_cond, NULL);
    for (int i = 0; i < kSlotCount; ++i) shared.slots[i].block = VB_INVALID_HANDLE;

    if (init_buffers(&shared) != CVI_SUCCESS) {
        printf("[FATAL] DMA/VB initialization failed\n");
        release_buffers(&shared);
        return 1;
    }
    cvitdl_handle_t tdl = NULL;
    // NV12 is converted by our explicit group 1 pipeline, so TDL receives an
    // exact-size RGB planar frame and must not start another preprocess pass.
    const bool tdl_skip_vpss = input_format == INPUT_NV12 || skip_vpss;
    if (init_yolo(&tdl, model_path, tdl_skip_vpss) != CVI_SUCCESS) {
        printf("[FATAL] YOLO model initialization failed\n");
        if (tdl) CVI_TDL_DestroyHandle(tdl);
        release_buffers(&shared);
        return 1;
    }
    bool vpss_started = false;
    if (input_format == INPUT_NV12) {
        if (init_vpss_conversion() != CVI_SUCCESS) {
            printf("[FATAL] NV12 VPSS conversion initialization failed\n");
            CVI_TDL_DestroyHandle(tdl);
            release_buffers(&shared);
            return 1;
        }
        vpss_started = true;
    }
    int uart_fd = open_uart(uart_path, baud);
    pthread_t receiver;
    pthread_create(&receiver, NULL, receiver_thread, &shared);

    uint64_t inferred = 0;
    uint64_t inference_sum_us = 0;
    uint64_t vpss_sum_us = 0;
    uint64_t processing_sum_us = 0;
    uint64_t queue_sum_us = 0;
    uint64_t vpss_failures = 0;
    uint64_t detected_frames = 0;
    uint64_t object_sum = 0;
    uint64_t last_received = 0;
    uint64_t last_inferred = 0;
    uint64_t stats_time = monotonic_us();
    uint16_t uart_dropped = 0;

    while (g_running) {
        int index = acquire_ready_slot(&shared);
        if (index < 0) break;
        FrameSlot &slot = shared.slots[index];
        VIDEO_FRAME_INFO_S converted_frame;
        VIDEO_FRAME_INFO_S *inference_frame = &slot.frame;
        const uint64_t process_start = monotonic_us();
        uint64_t vpss_us = 0;
        bool converted_acquired = false;
        if (input_format == INPUT_NV12) {
            memset(&converted_frame, 0, sizeof(converted_frame));
            const uint64_t vpss_start = monotonic_us();
            CVI_S32 send_result = CVI_VPSS_SendFrame(kVpssGroup, &slot.frame, 100);
            CVI_S32 get_result = send_result == CVI_SUCCESS
                                     ? CVI_VPSS_GetChnFrame(kVpssGroup, kVpssChannel,
                                                           &converted_frame, 100)
                                     : send_result;
            vpss_us = monotonic_us() - vpss_start;
            if (get_result != CVI_SUCCESS) {
                vpss_failures++;
                release_slot(&shared, index);
                continue;
            }
            converted_acquired = true;
            inference_frame = &converted_frame;
        }
        const uint64_t infer_start = monotonic_us();
        cvtdl_object_t objects;
        memset(&objects, 0, sizeof(objects));
        CVI_S32 result = CVI_TDL_YOLOV8_Detection(tdl, inference_frame, &objects);
        const uint64_t infer_end = monotonic_us();
        if (result == CVI_SUCCESS) {
            const uint32_t inference_us = (uint32_t)(infer_end - infer_start);
            inferred++;
            inference_sum_us += inference_us;
            vpss_sum_us += vpss_us;
            processing_sum_us += infer_end - process_start;
            queue_sum_us += infer_start - slot.received_time_us;
            if (objects.size > 0) detected_frames++;
            object_sum += objects.size;
            send_uart_result(uart_fd, slot.frame_id, inference_us, objects, &uart_dropped);
        } else {
            printf("[TPU] Inference failed: 0x%x\n", result);
        }
        CVI_TDL_Free(&objects);
        if (converted_acquired)
            CVI_VPSS_ReleaseChnFrame(kVpssGroup, kVpssChannel, &converted_frame);
        release_slot(&shared, index);

        const uint64_t now = monotonic_us();
        if (now - stats_time >= 2000000ULL) {
            const double seconds = (now - stats_time) / 1000000.0;
            const uint64_t receive_delta = shared.received - last_received;
            const uint64_t infer_delta = inferred - last_inferred;
            printf("[STATS] recv=%.2f fps infer=%.2f fps vpss_avg=%.2f ms "
                   "infer_avg=%.2f ms process_avg=%.2f ms queue_avg=%.2f ms "
                   "total=%llu detected=%llu objects=%llu net_err=%llu "
                   "vpss_fail=%llu uart_drop=%u\n",
                   receive_delta / seconds, infer_delta / seconds,
                   inferred ? vpss_sum_us / (1000.0 * inferred) : 0.0,
                   inferred ? inference_sum_us / (1000.0 * inferred) : 0.0,
                   inferred ? processing_sum_us / (1000.0 * inferred) : 0.0,
                   inferred ? queue_sum_us / (1000.0 * inferred) : 0.0,
                   (unsigned long long)inferred, (unsigned long long)detected_frames,
                   (unsigned long long)object_sum, (unsigned long long)shared.network_errors,
                   (unsigned long long)vpss_failures, uart_dropped);
            fflush(stdout);
            last_received = shared.received;
            last_inferred = inferred;
            stats_time = now;
        }
    }

    g_running = 0;
    if (shared.client_fd >= 0) shutdown(shared.client_fd, SHUT_RDWR);
    if (shared.listen_fd >= 0) {
        shutdown(shared.listen_fd, SHUT_RDWR);
        close(shared.listen_fd);
    }
    pthread_cond_broadcast(&shared.ready_cond);
    pthread_cond_broadcast(&shared.free_cond);
    pthread_join(receiver, NULL);
    if (uart_fd >= 0) close(uart_fd);
    if (vpss_started) stop_vpss_conversion();
    CVI_TDL_DestroyHandle(tdl);
    release_buffers(&shared);
    pthread_cond_destroy(&shared.ready_cond);
    pthread_cond_destroy(&shared.free_cond);
    pthread_mutex_destroy(&shared.mutex);
    return 0;
}
