#include "MJPEGWriter.h"

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>

/* ================= CONSTRUCTOR / DESTRUCTOR ================= */

MJPEGWriter::MJPEGWriter(uint16_t p,
                         const std::string& ip,
                         int quality)
    : port(p),
      bind_ip(ip),
      jpeg_quality(quality)
{
    pthread_mutex_init(&mtx_frame, nullptr);
    pthread_mutex_init(&mtx_clients, nullptr);
}

MJPEGWriter::~MJPEGWriter()
{
    stop();
    pthread_mutex_destroy(&mtx_frame);
    pthread_mutex_destroy(&mtx_clients);
}

/* ================= SOCKET ================= */

bool MJPEGWriter::openSocket()
{
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return false;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        server_fd = -1;
        return false;
    }

    if (listen(server_fd, 4) < 0)
    {
        perror("listen");
        close(server_fd);
        server_fd = -1;
        return false;
    }

    std::cout << "MJPEG stream listening on "
              << bind_ip << ":" << port << std::endl;

    return true;
}

void MJPEGWriter::closeSocket()
{
    if (server_fd >= 0)
    {
        close(server_fd);
        server_fd = -1;
    }
}

/* ================= PUBLIC API ================= */

bool MJPEGWriter::start()
{
    if (running)
        return true;

    if (!openSocket())
        return false;

    running = true;
    pthread_create(&th_listener, nullptr, listenerThread, this);
    pthread_create(&th_writer, nullptr, writerThread, this);
    return true;
}

void MJPEGWriter::stop()
{
    if (!running)
        return;

    running = false;
    closeSocket();

    pthread_join(th_listener, nullptr);
    pthread_join(th_writer, nullptr);

    pthread_mutex_lock(&mtx_clients);
    for (int c : clients)
        close(c);
    clients.clear();
    pthread_mutex_unlock(&mtx_clients);
}

void MJPEGWriter::write(const cv::Mat& frame)
{
    pthread_mutex_lock(&mtx_frame);
    frame.copyTo(lastFrame);
    pthread_mutex_unlock(&mtx_frame);
}

/* ================= THREAD HELPERS ================= */

void* MJPEGWriter::listenerThread(void* arg)
{
    static_cast<MJPEGWriter*>(arg)->listenerLoop();
    return nullptr;
}

void* MJPEGWriter::writerThread(void* arg)
{
    static_cast<MJPEGWriter*>(arg)->writerLoop();
    return nullptr;
}

/* ================= LISTENER ================= */

void MJPEGWriter::listenerLoop()
{
    while (running)
    {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);

        int client = accept(server_fd, (sockaddr*)&cli, &len);
        if (client < 0)
            continue;

        const char* header =
            "HTTP/1.0 200 OK\r\n"
            "Cache-Control: no-cache\r\n"
            "Pragma: no-cache\r\n"
            "Connection: close\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";

        ::send(client, header, strlen(header), MSG_NOSIGNAL);

        pthread_mutex_lock(&mtx_clients);
        clients.push_back(client);
        pthread_mutex_unlock(&mtx_clients);

        std::cout << "Client connected (" << client << ")\n";
    }
}

/* ================= WRITER ================= */

void MJPEGWriter::writerLoop()
{
    std::vector<uchar> jpg;
    std::vector<int> params = {
        cv::IMWRITE_JPEG_QUALITY,
        jpeg_quality
    };

    while (running)
    {
        pthread_mutex_lock(&mtx_frame);
        if (lastFrame.empty())
        {
            pthread_mutex_unlock(&mtx_frame);
            usleep(10000);
            continue;
        }

        cv::imencode(".jpg", lastFrame, jpg, params);
        pthread_mutex_unlock(&mtx_frame);

        std::string head =
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: " +
            std::to_string(jpg.size()) + "\r\n\r\n";

        pthread_mutex_lock(&mtx_clients);
        for (auto it = clients.begin(); it != clients.end();)
        {
            int c = *it;

            if (::send(c, head.data(), head.size(), MSG_NOSIGNAL) < 0 ||
                ::send(c, jpg.data(), jpg.size(), MSG_NOSIGNAL) < 0)
            {
                std::cout << "Client disconnected (" << c << ")\n";
                close(c);
                it = clients.erase(it);
            }
            else
            {
                ++it;
            }
        }
        pthread_mutex_unlock(&mtx_clients);

        usleep(33000); // ~30 FPS
    }
}
