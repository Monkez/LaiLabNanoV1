#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>
#include <pthread.h>
#include <netinet/in.h>

class MJPEGWriter
{
public:
    MJPEGWriter(uint16_t port,
                const std::string& bind_ip = "0.0.0.0",
                int jpeg_quality = 70);

    ~MJPEGWriter();

    bool start();
    void stop();

    void write(const cv::Mat& frame);
    bool isOpened() const { return running; }

private:
    // socket
    int server_fd = -1;
    uint16_t port;
    std::string bind_ip;

    // state
    bool running = false;

    // frame
    cv::Mat lastFrame;
    int jpeg_quality;

    // clients
    std::vector<int> clients;

    // threads
    pthread_t th_listener{};
    pthread_t th_writer{};

    // sync
    pthread_mutex_t mtx_frame;
    pthread_mutex_t mtx_clients;

    // internal
    bool openSocket();
    void closeSocket();

    static void* listenerThread(void* arg);
    static void* writerThread(void* arg);

    void listenerLoop();
    void writerLoop();
};
