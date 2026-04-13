#include "MJPEGWriter.h"
#include <opencv2/opencv.hpp>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <vector>
#include <chrono>

volatile uint8_t interrupted = 0;

void interrupt_handler(int signum)
{
    interrupted = 1;
}

int main()
{
    signal(SIGINT, interrupt_handler);

    /* ================== CONFIG ================== */
    const int WIDTH  = 320;
    const int HEIGHT = 240;
    const int STREAM_PORT = 7777;   // ⚠️ không dùng PORT
    const int FPS_LIMIT_US = 33000; // ~30 FPS
    /* ============================================ */

    /* ---------- Create stream (FIX vexing parse) ---------- */
    MJPEGWriter stream(7777, "192.168.100.2", 60);

    /* ---------- Load image ---------- */
    cv::Mat img = cv::imread("bus.jpg", cv::IMREAD_GRAYSCALE);
    if (img.empty())
    {
        printf("Failed to load bus.jpg\n");
        return -1;
    }

    cv::resize(img, img, cv::Size(WIDTH, HEIGHT));

    /* ---------- Precompute rotated frames ---------- */
    std::vector<cv::Mat> frames;
    frames.reserve(360);

    cv::Point2f center(WIDTH / 2.0f, HEIGHT / 2.0f);

    for (int angle = 0; angle < 360; angle++)
    {
        cv::Mat rot, dst;
        rot = cv::getRotationMatrix2D(center, angle, 1.0);
        cv::warpAffine(
            img,
            dst,
            rot,
            img.size(),
            cv::INTER_LINEAR,
            cv::BORDER_CONSTANT,
            cv::Scalar(0)
        );
        frames.push_back(dst);
    }

    /* ---------- Start stream ---------- */
    stream.write(frames[0]);
    stream.start();

    int frame_idx = 0;
    int fps_cnt = 0;
    auto t0 = std::chrono::steady_clock::now();

    while (!interrupted)
    {
        stream.write(frames[frame_idx]);
        frame_idx = (frame_idx + 1) % 360;
        fps_cnt++;

        auto now = std::chrono::steady_clock::now();
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();

        if (ms >= 1000)
        {
            printf("FPS: %d\n", fps_cnt);
            fps_cnt = 0;
            t0 = now;
        }

        usleep(FPS_LIMIT_US);
    }

    stream.stop();
    return 0;
}
