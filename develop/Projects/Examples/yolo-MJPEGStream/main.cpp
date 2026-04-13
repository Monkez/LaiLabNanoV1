#include "MJPEGWriter.h"
#include <opencv2/opencv.hpp>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <chrono>
#include <string>

#include "core/cvi_tdl_types_mem_internal.h"
#include "core/utils/vpss_helper.h"
#include "cvi_tdl.h"
#include "cvi_tdl_media.h"

#define MODEL_SCALE 0.0039216
#define MODEL_MEAN 0.0
#define MODEL_CLASS_CNT 80
#define MODEL_THRESH 0.5
#define MODEL_NMS_THRESH 0.45

volatile uint8_t interrupted = 0;

void interrupt_handler(int signum)
{
    interrupted = 1;
}

const char* COCO_CLASSES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
    "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier",
    "toothbrush"
};

CVI_S32 init_param(const cvitdl_handle_t tdl_handle)
{
    YoloPreParam preprocess_cfg =
        CVI_TDL_Get_YOLO_Preparam(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);

    for (int i = 0; i < 3; i++)
    {
        preprocess_cfg.factor[i] = MODEL_SCALE;
        preprocess_cfg.mean[i] = MODEL_MEAN;
    }
    preprocess_cfg.format = PIXEL_FORMAT_RGB_888_PLANAR;

    CVI_S32 ret = CVI_TDL_Set_YOLO_Preparam(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION,
                                            preprocess_cfg);
    if (ret != CVI_SUCCESS)
    {
        printf("Can not set yolov8 preprocess parameters %#x\n", ret);
        return ret;
    }

    YoloAlgParam yolov8_param =
        CVI_TDL_Get_YOLO_Algparam(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION);
    yolov8_param.cls = MODEL_CLASS_CNT;

    ret = CVI_TDL_Set_YOLO_Algparam(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, yolov8_param);
    if (ret != CVI_SUCCESS)
    {
        printf("Can not set yolov8 algorithm parameters %#x\n", ret);
        return ret;
    }

    CVI_TDL_SetModelThreshold(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, MODEL_THRESH);
    CVI_TDL_SetModelNmsThreshold(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, MODEL_NMS_THRESH);

    printf("yolov8 algorithm parameters setup success!\n");
    return ret;
}

// Helper function to save frame to file and use CVI_TDL_ReadImage
CVI_S32 MatToVideoFrame(imgprocess_t img_handle, const cv::Mat& mat, 
                        VIDEO_FRAME_INFO_S *frame, int frame_num)
{
    // Save to temporary file
    char temp_path[128];
    snprintf(temp_path, sizeof(temp_path), "/tmp/yolo_frame_%d.jpg", frame_num % 2);
    
    if (!cv::imwrite(temp_path, mat))
    {
        printf("Failed to save temp image\n");
        return CVI_FAILURE;
    }
    
    // Read using CVI_TDL to get proper format
    CVI_S32 ret = CVI_TDL_ReadImage(img_handle, temp_path, frame, PIXEL_FORMAT_RGB_888_PLANAR);
    
    return ret;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Usage: %s <model_path>\n", argv[0]);
        return -1;
    }

    signal(SIGINT, interrupt_handler);

    /* ================== CONFIG ================== */
    const int WIDTH  = 640;
    const int HEIGHT = 480;
    const int STREAM_PORT = 7777;
    const int FPS_LIMIT_US = 33000; // ~30 FPS
    const int DETECT_EVERY_N_FRAMES = 5; // Only detect every 5 frames for speed
    /* ============================================ */

    /* ---------- Initialize TDL System FIRST ---------- */
    printf("Initializing CVI TDL system...\n");
    
    CVI_S32 ret = MMF_INIT_HELPER2(WIDTH, HEIGHT, PIXEL_FORMAT_RGB_888, 1,
                                   WIDTH, HEIGHT, PIXEL_FORMAT_RGB_888, 1);
    if (ret != CVI_TDL_SUCCESS)
    {
        printf("Init sys failed with %#x!\n", ret);
        return ret;
    }

    cvitdl_handle_t tdl_handle = NULL;
    ret = CVI_TDL_CreateHandle(&tdl_handle);
    if (ret != CVI_SUCCESS)
    {
        printf("Create tdl handle failed with %#x!\n", ret);
        return ret;
    }

    ret = init_param(tdl_handle);
    if (ret != CVI_SUCCESS)
    {
        printf("Init param failed!\n");
        return ret;
    }

    printf("Opening model: %s\n", argv[1]);
    ret = CVI_TDL_OpenModel(tdl_handle, CVI_TDL_SUPPORTED_MODEL_YOLOV8_DETECTION, argv[1]);
    if (ret != CVI_SUCCESS)
    {
        printf("open model failed with %#x!\n", ret);
        return ret;
    }

    imgprocess_t img_handle;
    CVI_TDL_Create_ImageProcessor(&img_handle);

    /* ---------- Load image using CVI_TDL ---------- */
    printf("Loading bus.jpg using CVI_TDL...\n");
    VIDEO_FRAME_INFO_S original_frame;
    ret = CVI_TDL_ReadImage(img_handle, "bus.jpg", &original_frame, PIXEL_FORMAT_RGB_888_PLANAR);
    if (ret != CVI_SUCCESS)
    {
        printf("Failed to load bus.jpg with %#x\n", ret);
        return ret;
    }
    
    int orig_width = original_frame.stVFrame.u32Width;
    int orig_height = original_frame.stVFrame.u32Height;
    printf("Image loaded: %dx%d\n", orig_width, orig_height);

    // Test detection on original image first
    printf("\n=== Testing detection on original image ===\n");
    cvtdl_object_t test_obj = {0};
    ret = CVI_TDL_YOLOV8_Detection(tdl_handle, &original_frame, &test_obj);
    if (ret == CVI_SUCCESS)
    {
        printf("Original image detections: %d objects\n", test_obj.size);
        for (uint32_t i = 0; i < test_obj.size; i++)
        {
            printf("  - %s: %.2f at (%.0f,%.0f,%.0f,%.0f)\n",
                   COCO_CLASSES[test_obj.info[i].classes],
                   test_obj.info[i].bbox.score,
                   test_obj.info[i].bbox.x1,
                   test_obj.info[i].bbox.y1,
                   test_obj.info[i].bbox.x2,
                   test_obj.info[i].bbox.y2);
        }
        CVI_TDL_Free(&test_obj);
    }
    printf("===========================================\n\n");

    /* ---------- Convert to OpenCV Mat and resize ---------- */
    cv::Mat base_img(orig_height, orig_width, CV_8UC3);
    
    // Convert RGB planar to BGR interleaved for OpenCV
    uint8_t *r_plane = (uint8_t *)original_frame.stVFrame.pu8VirAddr[0];
    uint8_t *g_plane = r_plane + orig_width * orig_height;
    uint8_t *b_plane = g_plane + orig_width * orig_height;
    
    for (int y = 0; y < orig_height; y++)
    {
        for (int x = 0; x < orig_width; x++)
        {
            int idx = y * orig_width + x;
            base_img.at<cv::Vec3b>(y, x)[0] = b_plane[idx]; // B
            base_img.at<cv::Vec3b>(y, x)[1] = g_plane[idx]; // G
            base_img.at<cv::Vec3b>(y, x)[2] = r_plane[idx]; // R
        }
    }
    
    CVI_TDL_ReleaseImage(img_handle, &original_frame);

    // Resize using OpenCV
    cv::Mat resized_img;
    cv::resize(base_img, resized_img, cv::Size(WIDTH, HEIGHT), 0, 0, cv::INTER_LINEAR);
    printf("Image resized to %dx%d\n", WIDTH, HEIGHT);

    /* ---------- Precompute rotated frames ---------- */
    printf("Precomputing rotated frames...\n");
    std::vector<cv::Mat> frames;
    frames.reserve(360);

    cv::Point2f center(WIDTH / 2.0f, HEIGHT / 2.0f);

    for (int angle = 0; angle < 360; angle++)
    {
        cv::Mat rot, dst;
        rot = cv::getRotationMatrix2D(center, angle, 1.0);
        cv::warpAffine(resized_img, dst, rot, resized_img.size(), cv::INTER_LINEAR,
                       cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        frames.push_back(dst);
        
        if ((angle + 1) % 60 == 0)
        {
            printf("  Generated %d/360 frames\n", angle + 1);
        }
    }

    printf("Precomputed %lu rotated frames\n", frames.size());

    /* ---------- Start stream ---------- */
    printf("Starting MJPEG stream on port %d...\n", STREAM_PORT);
    MJPEGWriter stream{(uint16_t)STREAM_PORT};
    stream.write(frames[0]);
    stream.start();

    printf("\n=================================\n");
    printf("Stream started successfully!\n");
    printf("Access at: http://<IP>:%d\n", STREAM_PORT);
    printf("Press Ctrl+C to stop...\n");
    printf("=================================\n\n");

    int frame_idx = 0;
    int fps_cnt = 0;
    int detection_cnt = 0;
    auto t0 = std::chrono::steady_clock::now();
    
    cvtdl_object_t last_detections = {0};

    while (!interrupted)
    {
        cv::Mat current_frame = frames[frame_idx].clone();

        // Only run detection every N frames to improve FPS
        if (frame_idx % DETECT_EVERY_N_FRAMES == 0)
        {
            VIDEO_FRAME_INFO_S detect_frame;
            
            // Use CVI_TDL_ReadImage for proper format
            ret = MatToVideoFrame(img_handle, current_frame, &detect_frame, frame_idx);
            
            if (ret == CVI_SUCCESS)
            {
                cvtdl_object_t obj_meta = {0};
                
                // YOLO inference
                auto inf_begin = std::chrono::steady_clock::now();
                ret = CVI_TDL_YOLOV8_Detection(tdl_handle, &detect_frame, &obj_meta);
                auto inf_end = std::chrono::steady_clock::now();
                
                CVI_TDL_ReleaseImage(img_handle, &detect_frame);
                
                if (ret == CVI_SUCCESS)
                {
                    double inf_time = std::chrono::duration<double>(inf_end - inf_begin).count();
                    
                    // Free previous detections
                    if (last_detections.size > 0)
                    {
                        CVI_TDL_Free(&last_detections);
                    }
                    
                    // Copy current detections
                    last_detections = obj_meta;
                    
                    if (obj_meta.size > 0)
                    {
                        detection_cnt++;
                        printf("[Frame %3d] Detected %d objects (%.3fs)\n", 
                               frame_idx, obj_meta.size, inf_time);
                    }
                }
            }
        }

        // Draw boxes from last detection
        for (uint32_t i = 0; i < last_detections.size; i++)
        {
            int x1 = (int)last_detections.info[i].bbox.x1;
            int y1 = (int)last_detections.info[i].bbox.y1;
            int x2 = (int)last_detections.info[i].bbox.x2;
            int y2 = (int)last_detections.info[i].bbox.y2;
            int cls = last_detections.info[i].classes;
            float score = last_detections.info[i].bbox.score;

            // Ensure coordinates are within bounds
            x1 = std::max(0, std::min(x1, WIDTH - 1));
            y1 = std::max(0, std::min(y1, HEIGHT - 1));
            x2 = std::max(0, std::min(x2, WIDTH - 1));
            y2 = std::max(0, std::min(y2, HEIGHT - 1));

            // Draw rectangle
            cv::rectangle(current_frame, cv::Point(x1, y1), cv::Point(x2, y2),
                         cv::Scalar(0, 255, 0), 2);

            // Draw label
            char label[128];
            if (cls >= 0 && cls < MODEL_CLASS_CNT)
            {
                snprintf(label, sizeof(label), "%s %.2f", COCO_CLASSES[cls], score);
            }
            else
            {
                snprintf(label, sizeof(label), "cls%d %.2f", cls, score);
            }

            int baseline = 0;
            cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 
                                                 0.5, 1, &baseline);
            
            // Draw background for text
            cv::rectangle(current_frame, 
                         cv::Point(x1, y1 - text_size.height - 5),
                         cv::Point(x1 + text_size.width, y1),
                         cv::Scalar(0, 255, 0), -1);

            cv::putText(current_frame, label, cv::Point(x1, y1 - 5),
                       cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 1);
        }

        // Stream the frame
        stream.write(current_frame);

        frame_idx = (frame_idx + 1) % 360;
        fps_cnt++;

        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();

        if (ms >= 1000)
        {
            printf("Stream FPS: %d | Detections: %d\n", fps_cnt, detection_cnt);
            fps_cnt = 0;
            detection_cnt = 0;
            t0 = now;
        }

        usleep(FPS_LIMIT_US);
    }

    printf("\nStopping stream...\n");
    stream.stop();
    
    if (last_detections.size > 0)
    {
        CVI_TDL_Free(&last_detections);
    }
    
    CVI_TDL_DestroyHandle(tdl_handle);
    CVI_TDL_Destroy_ImageProcessor(img_handle);

    printf("Done!\n");
    return 0;
}