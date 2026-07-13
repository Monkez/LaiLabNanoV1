#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>
#include <time.h>

#define BUFFER_COUNT 4

struct buffer {
    void *data;
    size_t length;
};

static void print_control(int fd, unsigned int id, const char *name) {
    struct v4l2_queryctrl query = {0};
    struct v4l2_control control = {0};
    query.id = id;
    control.id = id;
    if (ioctl(fd, VIDIOC_QUERYCTRL, &query) == 0 &&
        !(query.flags & V4L2_CTRL_FLAG_DISABLED) &&
        ioctl(fd, VIDIOC_G_CTRL, &control) == 0) {
        printf("[V4L2 control] %s=%d range=%d..%d step=%d default=%d\n",
               name, control.value, query.minimum, query.maximum,
               query.step, query.default_value);
    }
}

static int set_control(int fd, unsigned int id, int value, const char *name) {
    struct v4l2_control control = {.id = id, .value = value};
    if (ioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
        fprintf(stderr, "[V4L2 control] cannot set %s=%d: %s\n",
                name, value, strerror(errno));
        return -1;
    }
    printf("[V4L2 control] set %s=%d\n", name, value);
    return 0;
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

int main(int argc, char **argv) {
    const char *device = argc > 1 ? argv[1] : "/dev/video1";
    const int target_frames = argc > 2 ? atoi(argv[2]) : 120;
    const int use_mjpeg = argc > 3 && strcmp(argv[3], "mjpeg") == 0;
    const int restore_auto = argc > 4 && strcmp(argv[4], "auto") == 0;
    const int manual_exposure = argc > 4 && !restore_auto ? atoi(argv[4]) : -1;
    const int width = argc > 5 ? atoi(argv[5]) : 640;
    const int height = argc > 6 ? atoi(argv[6]) : 480;
    struct buffer buffers[BUFFER_COUNT] = {0};
    int fd = open(device, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    print_control(fd, V4L2_CID_EXPOSURE_AUTO, "exposure_auto");
    print_control(fd, V4L2_CID_EXPOSURE_ABSOLUTE, "exposure_absolute");
    print_control(fd, V4L2_CID_GAIN, "gain");
    print_control(fd, V4L2_CID_POWER_LINE_FREQUENCY, "power_line_frequency");

    printf("[V4L2 test] formats advertised by %s:\n", device);
    for (unsigned int index = 0;; ++index) {
        struct v4l2_fmtdesc desc = {0};
        desc.index = index;
        desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) break;
        printf("  %c%c%c%c  %s\n", desc.pixelformat & 0xff,
               (desc.pixelformat >> 8) & 0xff, (desc.pixelformat >> 16) & 0xff,
               (desc.pixelformat >> 24) & 0xff, desc.description);
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = use_mjpeg ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("VIDIOC_S_FMT"); close(fd); return 1; }
    printf("[V4L2 test] %s: %ux%u bytesperline=%u sizeimage=%u\n", device,
           fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

    struct v4l2_frmivalenum interval = {0};
    interval.pixel_format = fmt.fmt.pix.pixelformat;
    interval.width = fmt.fmt.pix.width;
    interval.height = fmt.fmt.pix.height;
    printf("[V4L2 test] advertised frame intervals:");
    for (interval.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0; interval.index++) {
        if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE)
            printf(" %.1f", (double)interval.discrete.denominator / interval.discrete.numerator);
    }
    printf(" FPS\n");

    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        perror("VIDIOC_S_PARM");
    } else {
        const struct v4l2_fract *actual = &parm.parm.capture.timeperframe;
        printf("[V4L2 test] requested=30 FPS accepted=%.2f FPS\n",
               actual->numerator ? (double)actual->denominator / actual->numerator : 0.0);
    }

    // Some UVC cameras reset exposure controls when S_FMT/S_PARM commits a
    // new streaming profile. Apply exposure only after both negotiations.
    if (restore_auto) {
        set_control(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_APERTURE_PRIORITY, "exposure_auto");
    } else if (manual_exposure >= 0) {
        set_control(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL, "exposure_auto");
        set_control(fd, V4L2_CID_EXPOSURE_ABSOLUTE, manual_exposure, "exposure_absolute");
    }
    print_control(fd, V4L2_CID_EXPOSURE_AUTO, "exposure_auto_after_format");
    print_control(fd, V4L2_CID_EXPOSURE_ABSOLUTE, "exposure_absolute_after_format");

    struct v4l2_requestbuffers req = {0};
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) { perror("VIDIOC_REQBUFS"); close(fd); return 1; }

    for (unsigned int i = 0; i < req.count; ++i) {
        struct v4l2_buffer buf = {0};
        buf.type = req.type; buf.memory = req.memory; buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("VIDIOC_QUERYBUF"); return 1; }
        buffers[i].length = buf.length;
        buffers[i].data = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].data == MAP_FAILED) { perror("mmap"); return 1; }
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF initial"); return 1; }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); return 1; }

    int captured = 0;
    const double started = monotonic_seconds();
    while (captured < target_frames) {
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        struct timeval timeout = {2, 0};
        if (select(fd + 1, &fds, NULL, NULL, &timeout) <= 0) {
            fprintf(stderr, "[V4L2 test] timeout after %d frames\n", captured);
            break;
        }
        struct v4l2_buffer buf = {0};
        buf.type = type; buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("VIDIOC_DQBUF"); break; }
        captured++;
        if (captured == 1 || captured % 30 == 0) printf("[V4L2 test] frame=%d index=%u bytes=%u\n", captured, buf.index, buf.bytesused);
        buf.bytesused = 0;
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF requeue"); break; }
    }

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    const double elapsed = monotonic_seconds() - started;
    printf("[V4L2 test] captured=%d elapsed=%.3fs measured=%.2f FPS\n",
           captured, elapsed, elapsed > 0 ? captured / elapsed : 0.0);
    for (unsigned int i = 0; i < req.count; ++i) if (buffers[i].data) munmap(buffers[i].data, buffers[i].length);
    close(fd);
    return captured == target_frames ? 0 : 2;
}
