#include "MJPEGWriter.h"
#include <opencv2/opencv.hpp>
#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <stdio.h>

#include "core/cvi_tdl_types_mem_internal.h"
#include "core/utils/vpss_helper.h"
#include "cvi_tdl.h"
#include "cvi_tdl_media.h"

/* ================= CONFIG ================= */
#define WIDTH   320
#define HEIGHT  240
#define PORT    7777

#define MODEL_SCALE 0.0039216
#define MODEL_MEAN  0.0
#define MODEL_THRESH 0.5
#define MODEL_NMS_THRESH 0.5
#define MODEL_CLASS_CNT 80
/* ========================================== */

volatile int stop_flag = 0;
void sigint_handler(int) { stop_flag = 1; }

/* ---------- YOLO PARAM ---------- */
CVI_S32 init_param(cvitdl_handle_t tdl)
{
    YoloPreParam pre =
        CVI_TDL_Get_YOLO_Preparam(tdl, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);

    for (int i = 0; i < 3; i++) {
        pre.factor[i] = MODEL_SCALE;
        pre.mean[i]   = MODEL_MEAN;
    }
    pre.format = PIXEL_FORMAT_RGB_888_PLANAR;

    CVI_TDL_Set_YOLO_Preparam(
        tdl, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, pre);

    YoloAlgParam alg =
        CVI_TDL_Get_YOLO_Algparam(tdl, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);

    alg.cls = MODEL_CLASS_CNT;
    CVI_TDL_Set_YOLO_Algparam(
        tdl, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, alg);

    CVI_TDL_SetModelThreshold(
        tdl, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, MODEL_THRESH);
    CVI_TDL_SetModelNmsThreshold(
        tdl, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, MODEL_NMS_THRESH);

    return CVI_SUCCESS;
}

/* ================= MAIN ================= */
int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: %s yolov8.cvimodel bus.jpg\n", argv[0]);
        return -1;
    }

    signal(SIGINT, sigint_handler);

    /* ---------- MMF INIT ---------- */
    MMF_INIT_HELPER2(
        WIDTH, HEIGHT, PIXEL_FORMAT_RGB_888, 1,
        WIDTH, HEIGHT, PIXEL_FORMAT_RGB_888, 1);

    /* ---------- YOLO INIT ---------- */
    cvitdl_handle_t tdl = NULL;
    CVI_TDL_CreateHandle(&tdl);
    init_param(tdl);
    CVI_TDL_OpenModel(
        tdl,
        CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION,
        argv[1]);

    imgprocess_t img_proc;
    CVI_TDL_Create_ImageProcessor(&img_proc);

    /* ---------- LOAD IMAGE ---------- */
    cv::Mat img = cv::imread(argv[2]);
    if (img.empty()) {
        printf("Failed to load image\n");
        return -1;
    }
    cv::resize(img, img, {WIDTH, HEIGHT});

    /* ---------- MMF FRAME ---------- */
    VIDEO_FRAME_INFO_S frame;
    CVI_TDL_CreateImageFrame(
        &frame, WIDTH, HEIGHT, PIXEL_FORMAT_RGB_888_PLANAR);

    /* ---------- STREAM ---------- */
    MJPEGWriter stream{(uint16_t)PORT};
    stream.write(img);   // bắt buộc
    stream.start();

    /* ---------- FPS ---------- */
    int frame_cnt = 0;
    double fps = 0.0;
    auto t_start = std::chrono::steady_clock::now();

    /* ============ LOOP ============ */
    while (!stop_flag)
    {
        /* copy OpenCV → MMF */
        CVI_TDL_CopyImageFromMat(img_proc, img, &frame);

        /* YOLO */
        cvtdl_object_t obj = {0};
        CVI_TDL_YOLOV8_Detection(tdl, &frame, &obj);

        /* draw bbox */
        for (uint32_t i = 0; i < obj.size; i++) {
            auto &b = obj.info[i].bbox;
            cv::rectangle(
                img,
                {int(b.x1), int(b.y1)},
                {int(b.x2), int(b.y2)},
                {0, 255, 0}, 2);
        }

        /* FPS calc */
        frame_cnt++;
        auto now = std::chrono::steady_clock::now();
        double sec =
            std::chrono::duration<double>(now - t_start).count();

        if (sec >= 1.0) {
            fps = frame_cnt / sec;
            frame_cnt = 0;
            t_start = now;
        }

        /* draw FPS */
        char fps_txt[32];
        snprintf(fps_txt, sizeof(fps_txt), "FPS: %.1f", fps);
        cv::putText(
            img, fps_txt,
            {10, 30},
            cv::FONT_HERSHEY_SIMPLEX,
            0.9,
            {0, 255, 255},
            2);

        stream.write(img);
    }

    /* ---------- CLEAN ---------- */
    stream.stop();
    CVI_TDL_Destroy_ImageProcessor(img_proc);
    CVI_TDL_DestroyHandle(tdl);

    return 0;
}
