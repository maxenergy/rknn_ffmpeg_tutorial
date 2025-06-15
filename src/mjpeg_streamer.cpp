#include "mjpeg_streamer.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

const char* MJPEGStreamer::BOUNDARY = "mjpegstream";

// SimpleHTTPServer implementation
SimpleHTTPServer::SimpleHTTPServer(int port)
    : port_(port), server_fd_(-1), running_(false), should_stop_(false) {
}

SimpleHTTPServer::~SimpleHTTPServer() {
    stop();
}

bool SimpleHTTPServer::start() {
    if (running_) {
        return false;
    }

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        printf("HTTP Server: Failed to create socket\n");
        return false;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        printf("HTTP Server: Failed to bind to port %d\n", port_);
        close(server_fd_);
        return false;
    }

    if (listen(server_fd_, 10) < 0) {
        printf("HTTP Server: Failed to listen on port %d\n", port_);
        close(server_fd_);
        return false;
    }

    should_stop_ = false;
    running_ = true;
    server_thread_ = std::thread(&SimpleHTTPServer::server_worker, this);

    printf("HTTP Server started on port %d\n", port_);
    return true;
}

void SimpleHTTPServer::stop() {
    if (!running_) {
        return;
    }

    should_stop_ = true;

    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    running_ = false;
    printf("HTTP Server stopped\n");
}

void SimpleHTTPServer::set_mjpeg_provider(std::function<bool(std::vector<uint8_t>&)> provider) {
    mjpeg_provider_ = provider;
}

void SimpleHTTPServer::server_worker() {
    while (!should_stop_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (!should_stop_) {
                printf("HTTP Server: Accept failed: %s\n", strerror(errno));
            }
            continue;
        }

        // Handle client in separate thread to support multiple connections
        std::thread client_thread(&SimpleHTTPServer::handle_client, this, client_fd);
        client_thread.detach();
    }
}

void SimpleHTTPServer::handle_client(int client_fd) {
    char buffer[1024] = {0};
    int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }

    std::string request(buffer);

    if (request.find("GET /mjpeg") != std::string::npos || request.find("GET /stream") != std::string::npos) {
        send_mjpeg_stream(client_fd);
    } else if (request.find("GET /stats") != std::string::npos) {
        std::string stats_response;
        // Basic stats response - can be enhanced
        stats_response = "{\"status\":\"running\",\"clients\":1}";
        send_http_response(client_fd, "application/json", stats_response);
    } else if (request.find("GET /multi") != std::string::npos) {
        send_multi_stream_page(client_fd);
    } else {
        send_index_page(client_fd);
    }

    close(client_fd);
}

void SimpleHTTPServer::send_mjpeg_stream(int client_fd) {
    std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" + std::string(MJPEGStreamer::BOUNDARY) + "\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    send(client_fd, headers.c_str(), headers.length(), 0);

    while (!should_stop_) {
        if (!mjpeg_provider_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            continue;
        }

        std::vector<uint8_t> jpeg_data;
        if (mjpeg_provider_(jpeg_data) && !jpeg_data.empty()) {
            std::string frame_header =
                "\r\n--" + std::string(MJPEGStreamer::BOUNDARY) + "\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: " + std::to_string(jpeg_data.size()) + "\r\n"
                "\r\n";

            if (send(client_fd, frame_header.c_str(), frame_header.length(), 0) < 0) {
                break;
            }

            if (send(client_fd, jpeg_data.data(), jpeg_data.size(), 0) < 0) {
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS
    }
}

void SimpleHTTPServer::send_http_response(int client_fd, const std::string& content_type, const std::string& content) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: " + content_type + "\r\n"
        "Content-Length: " + std::to_string(content.length()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + content;

    send(client_fd, response.c_str(), response.length(), 0);
}

void SimpleHTTPServer::send_index_page(int client_fd) {
    std::string html =
        "<!DOCTYPE html>\n"
        "<html><head><title>MJPEG Stream</title></head>\n"
        "<body>\n"
        "<h1>Real-time Object Detection Stream</h1>\n"
        "<img src=\"/mjpeg\" style=\"max-width:100%; height:auto;\">\n"
        "<p><a href=\"/stats\">View Statistics</a></p>\n"
        "</body></html>";

    send_http_response(client_fd, "text/html", html);
}

void SimpleHTTPServer::send_multi_stream_page(int client_fd) {
    std::string html =
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <title>Multi-Stream Object Detection Dashboard</title>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <style>\n"
        "        body {\n"
        "            margin: 0;\n"
        "            padding: 20px;\n"
        "            font-family: Arial, sans-serif;\n"
        "            background-color: #1a1a1a;\n"
        "            color: white;\n"
        "        }\n"
        "        .header {\n"
        "            text-align: center;\n"
        "            margin-bottom: 20px;\n"
        "        }\n"
        "        .grid-container {\n"
        "            display: grid;\n"
        "            grid-template-columns: repeat(4, 1fr);\n"
        "            grid-template-rows: repeat(2, 1fr);\n"
        "            gap: 10px;\n"
        "            height: calc(100vh - 120px);\n"
        "            max-width: 1920px;\n"
        "            margin: 0 auto;\n"
        "        }\n"
        "        .stream-container {\n"
        "            position: relative;\n"
        "            background-color: #2a2a2a;\n"
        "            border: 2px solid #444;\n"
        "            border-radius: 8px;\n"
        "            overflow: hidden;\n"
        "            transition: transform 0.2s, border-color 0.2s;\n"
        "        }\n"
        "        .stream-container:hover {\n"
        "            transform: scale(1.02);\n"
        "            border-color: #0066cc;\n"
        "        }\n"
        "        .stream-container.fullscreen {\n"
        "            position: fixed;\n"
        "            top: 0;\n"
        "            left: 0;\n"
        "            width: 100vw;\n"
        "            height: 100vh;\n"
        "            z-index: 1000;\n"
        "            transform: none;\n"
        "            border-radius: 0;\n"
        "        }\n"
        "        .stream-image {\n"
        "            width: 100%;\n"
        "            height: calc(100% - 40px);\n"
        "            object-fit: contain;\n"
        "            background-color: #000;\n"
        "        }\n"
        "        .stream-label {\n"
        "            position: absolute;\n"
        "            bottom: 0;\n"
        "            left: 0;\n"
        "            right: 0;\n"
        "            background: linear-gradient(transparent, rgba(0,0,0,0.8));\n"
        "            color: white;\n"
        "            padding: 10px;\n"
        "            text-align: center;\n"
        "            font-size: 14px;\n"
        "            font-weight: bold;\n"
        "        }\n"
        "        .status-indicator {\n"
        "            position: absolute;\n"
        "            top: 10px;\n"
        "            right: 10px;\n"
        "            width: 12px;\n"
        "            height: 12px;\n"
        "            border-radius: 50%;\n"
        "            background-color: #00ff00;\n"
        "            box-shadow: 0 0 10px rgba(0,255,0,0.5);\n"
        "        }\n"
        "        .controls {\n"
        "            text-align: center;\n"
        "            margin-top: 20px;\n"
        "        }\n"
        "        .btn {\n"
        "            background-color: #0066cc;\n"
        "            color: white;\n"
        "            border: none;\n"
        "            padding: 10px 20px;\n"
        "            margin: 0 5px;\n"
        "            border-radius: 5px;\n"
        "            cursor: pointer;\n"
        "            font-size: 14px;\n"
        "        }\n"
        "        .btn:hover {\n"
        "            background-color: #0052a3;\n"
        "        }\n"
        "        @media (max-width: 1200px) {\n"
        "            .grid-container {\n"
        "                grid-template-columns: repeat(2, 1fr);\n"
        "                grid-template-rows: repeat(4, 1fr);\n"
        "            }\n"
        "        }\n"
        "        @media (max-width: 768px) {\n"
        "            .grid-container {\n"
        "                grid-template-columns: 1fr;\n"
        "                grid-template-rows: repeat(8, 200px);\n"
        "                height: auto;\n"
        "            }\n"
        "        }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"header\">\n"
        "        <h1>🎥 Multi-Stream Object Detection Dashboard</h1>\n"
        "        <p>Real-time AI-powered object detection across 8 video streams</p>\n"
        "    </div>\n"
        "\n"
        "    <div class=\"grid-container\" id=\"gridContainer\">\n"
        "        <div class=\"stream-container\" onclick=\"toggleFullscreen(this)\">\n"
        "            <div class=\"status-indicator\"></div>\n"
        "            <img class=\"stream-image\" src=\"http://localhost:8090/mjpeg\" alt=\"Stream 1\">\n"
        "            <div class=\"stream-label\">Stream 1 - /userdata/videos/2.mp4</div>\n"
        "        </div>\n"
        "        <div class=\"stream-container\" onclick=\"toggleFullscreen(this)\">\n"
        "            <div class=\"status-indicator\"></div>\n"
        "            <img class=\"stream-image\" src=\"http://localhost:8091/mjpeg\" alt=\"Stream 2\">\n"
        "            <div class=\"stream-label\">Stream 2 - /userdata/videos/3.mp4</div>\n"
        "        </div>\n"
        "        <div class=\"stream-container\" onclick=\"toggleFullscreen(this)\">\n"
        "            <div class=\"status-indicator\"></div>\n"
        "            <img class=\"stream-image\" src=\"http://localhost:8092/mjpeg\" alt=\"Stream 3\">\n"
        "            <div class=\"stream-label\">Stream 3 - /userdata/videos/4.mp4</div>\n"
        "        </div>\n"
        "        <div class=\"stream-container\" onclick=\"toggleFullscreen(this)\">\n"
        "            <div class=\"status-indicator\"></div>\n"
        "            <img class=\"stream-image\" src=\"http://localhost:8093/mjpeg\" alt=\"Stream 4\">\n"
        "            <div class=\"stream-label\">Stream 4 - /userdata/videos/5.mp4</div>\n"
        "        </div>\n"
        "        <div class=\"stream-container\" onclick=\"toggleFullscreen(this)\">\n"
        "            <div class=\"status-indicator\"></div>\n"
        "            <img class=\"stream-image\" src=\"http://localhost:8094/mjpeg\" alt=\"Stream 5\">\n"
        "            <div class=\"stream-label\">Stream 5 - /userdata/videos/6.mp4</div>\n"
        "        </div>\n"
        "        <div class=\"stream-container\" onclick=\"toggleFullscreen(this)\">\n"
        "            <div class=\"status-indicator\"></div>\n"
        "            <img class=\"stream-image\" src=\"http://localhost:8095/mjpeg\" alt=\"Stream 6\">\n"
        "            <div class=\"stream-label\">Stream 6 - /userdata/videos/7.mp4</div>\n"
        "        </div>\n"
        "        <div class=\"stream-container\" onclick=\"toggleFullscreen(this)\">\n"
        "            <div class=\"status-indicator\"></div>\n"
        "            <img class=\"stream-image\" src=\"http://localhost:8096/mjpeg\" alt=\"Stream 7\">\n"
        "            <div class=\"stream-label\">Stream 7 - /userdata/videos/8.mp4</div>\n"
        "        </div>\n"
        "        <div class=\"stream-container\" onclick=\"toggleFullscreen(this)\">\n"
        "            <div class=\"status-indicator\"></div>\n"
        "            <img class=\"stream-image\" src=\"http://localhost:8097/mjpeg\" alt=\"Stream 8\">\n"
        "            <div class=\"stream-label\">Stream 8 - /userdata/videos/9.mp4</div>\n"
        "        </div>\n"
        "    </div>\n"
        "\n"
        "    <div class=\"controls\">\n"
        "        <button class=\"btn\" onclick=\"refreshAllStreams()\">🔄 Refresh All</button>\n"
        "        <button class=\"btn\" onclick=\"toggleGrid()\">📐 Toggle Layout</button>\n"
        "        <button class=\"btn\" onclick=\"window.location.href='/stats'\">📊 Statistics</button>\n"
        "    </div>\n"
        "\n"
        "    <script>\n"
        "        let isGridMode = true;\n"
        "        \n"
        "        function toggleFullscreen(container) {\n"
        "            if (container.classList.contains('fullscreen')) {\n"
        "                container.classList.remove('fullscreen');\n"
        "                document.body.style.overflow = 'auto';\n"
        "            } else {\n"
        "                // Remove fullscreen from any other container\n"
        "                document.querySelectorAll('.stream-container.fullscreen').forEach(c => {\n"
        "                    c.classList.remove('fullscreen');\n"
        "                });\n"
        "                container.classList.add('fullscreen');\n"
        "                document.body.style.overflow = 'hidden';\n"
        "            }\n"
        "        }\n"
        "        \n"
        "        function refreshAllStreams() {\n"
        "            document.querySelectorAll('.stream-image').forEach(img => {\n"
        "                const src = img.src;\n"
        "                img.src = '';\n"
        "                setTimeout(() => { img.src = src; }, 100);\n"
        "            });\n"
        "        }\n"
        "        \n"
        "        function toggleGrid() {\n"
        "            const container = document.getElementById('gridContainer');\n"
        "            if (isGridMode) {\n"
        "                container.style.gridTemplateColumns = 'repeat(2, 1fr)';\n"
        "                container.style.gridTemplateRows = 'repeat(4, 1fr)';\n"
        "            } else {\n"
        "                container.style.gridTemplateColumns = 'repeat(4, 1fr)';\n"
        "                container.style.gridTemplateRows = 'repeat(2, 1fr)';\n"
        "            }\n"
        "            isGridMode = !isGridMode;\n"
        "        }\n"
        "        \n"
        "        // Close fullscreen on Escape key\n"
        "        document.addEventListener('keydown', function(e) {\n"
        "            if (e.key === 'Escape') {\n"
        "                document.querySelectorAll('.stream-container.fullscreen').forEach(c => {\n"
        "                    c.classList.remove('fullscreen');\n"
        "                });\n"
        "                document.body.style.overflow = 'auto';\n"
        "            }\n"
        "        });\n"
        "        \n"
        "        // Auto-refresh page every 30 seconds to handle connection issues\n"
        "        setTimeout(() => {\n"
        "            location.reload();\n"
        "        }, 30000);\n"
        "    </script>\n"
        "</body>\n"
        "</html>";

    send_http_response(client_fd, "text/html", html);
}

// MJPEGStreamer implementation
MJPEGStreamer::MJPEGStreamer()
    : server_(nullptr), encoder_(nullptr), port_(8090), width_(1280), height_(720),
      running_(false), should_stop_(false), clients_connected_(0), frames_encoded_(0),
      frames_dropped_(0), avg_encode_time_ms_(0.0), fps_(0.0) {
}

MJPEGStreamer::~MJPEGStreamer() {
    stop();
}

int MJPEGStreamer::init(int port, int width, int height) {
    port_ = port;
    width_ = width;
    height_ = height;

    // Initialize MPP encoder with higher bitrate for better quality
    encoder_.reset(new MPPEncoder());
    if (encoder_->init(width_, height_, 30, 8000000) != 0) {  // Increased from 2Mbps to 8Mbps
        printf("MJPEG Streamer: Failed to initialize MPP encoder\n");
        return -1;
    }

    // Initialize HTTP server
    server_.reset(new SimpleHTTPServer(port_));
    server_->set_mjpeg_provider([this](std::vector<uint8_t>& jpeg_data) {
        return get_current_jpeg(jpeg_data);
    });

    printf("MJPEG Streamer initialized: %dx%d, port=%d\n", width_, height_, port_);
    return 0;
}

int MJPEGStreamer::start() {
    if (running_) {
        printf("MJPEG Streamer: Already running\n");
        return -1;
    }

    should_stop_ = false;
    running_ = true;

    // Start encoder worker thread
    encoder_thread_ = std::thread(&MJPEGStreamer::encoder_worker, this);

    // Start HTTP server
    if (!server_->start()) {
        printf("MJPEG Streamer: Failed to start HTTP server\n");
        stop();
        return -1;
    }

    printf("MJPEG Streamer started on port %d\n", port_);
    printf("Access the stream at: http://0.0.0.0:%d/mjpeg (or use your server IP)\n", port_);
    printf("View web interface at: http://0.0.0.0:%d/ (or use your server IP)\n", port_);

    return 0;
}

void MJPEGStreamer::stop() {
    if (!running_) {
        return;
    }

    should_stop_ = true;

    // Stop HTTP server
    if (server_) {
        server_->stop();
    }

    // Wake up encoder thread
    queue_cv_.notify_all();
    jpeg_cv_.notify_all();

    // Wait for encoder thread to finish
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    // Clear queues
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!frame_queue_.empty()) {
            frame_queue_.pop();
        }
    }

    {
        std::lock_guard<std::mutex> lock(jpeg_mutex_);
        current_jpeg_.clear();
    }

    running_ = false;
    printf("MJPEG Streamer stopped\n");
}

void MJPEGStreamer::push_frame(const cv::Mat& frame, const detect_result_group_t& detection_results) {
    if (!running_) {
        return;
    }

    std::unique_lock<std::mutex> lock(queue_mutex_);

    // Drop frames if queue is full
    if (frame_queue_.size() >= MAX_QUEUE_SIZE) {
        frame_queue_.pop();
        frames_dropped_++;
    }

    frame_queue_.emplace(frame, detection_results, std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    lock.unlock();
    queue_cv_.notify_one();
}

void MJPEGStreamer::push_frame_raw(const uint8_t* bgr_data, int width, int height, const detect_result_group_t& detection_results) {
    if (width != width_ || height != height_) {
        printf("MJPEG Streamer: Frame size mismatch: expected %dx%d, got %dx%d\n",
               width_, height_, width, height);
        return;
    }

    // Validate and potentially correct color format
    cv::Mat frame = validate_and_correct_color_format(bgr_data, width, height);
    push_frame(frame, detection_results);
}

void MJPEGStreamer::encoder_worker() {
    printf("MJPEG Streamer: Encoder worker started\n");

    auto last_fps_time = std::chrono::steady_clock::now();
    int frame_count = 0;
    double total_encode_time = 0.0;

    while (!should_stop_) {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        // Wait for frames
        queue_cv_.wait(lock, [this] { return !frame_queue_.empty() || should_stop_; });

        if (should_stop_) {
            break;
        }

        if (frame_queue_.empty()) {
            continue;
        }

        // Get the latest frame
        FrameData frame_data = frame_queue_.front();
        frame_queue_.pop();
        lock.unlock();

        // Draw detection results on frame
        cv::Mat annotated_frame = draw_detection_results(frame_data.frame, frame_data.detection_results);

        // Encode frame
        auto encode_start = std::chrono::high_resolution_clock::now();

        std::vector<uint8_t> jpeg_data;
        if (encoder_->encode_frame(annotated_frame, jpeg_data) == 0) {
            auto encode_end = std::chrono::high_resolution_clock::now();
            double encode_time = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();

            // Update statistics
            total_encode_time += encode_time;
            frame_count++;
            frames_encoded_++;

            // Update average encode time
            avg_encode_time_ms_ = total_encode_time / frame_count;

            // Update current JPEG data
            {
                std::lock_guard<std::mutex> jpeg_lock(jpeg_mutex_);
                current_jpeg_ = std::move(jpeg_data);
            }
            jpeg_cv_.notify_all();
        } else {
            printf("MJPEG Streamer: Failed to encode frame\n");
        }

        // Calculate FPS every second
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_fps_time);
        if (duration.count() >= 1) {
            fps_ = frame_count / duration.count();
            frame_count = 0;
            total_encode_time = 0.0;
            last_fps_time = now;
        }
    }

    printf("MJPEG Streamer: Encoder worker stopped\n");
}

cv::Mat MJPEGStreamer::draw_detection_results(const cv::Mat& frame, const detect_result_group_t& results) {
    cv::Mat annotated_frame = frame.clone();

    // Draw bounding boxes and labels
    for (int i = 0; i < results.count; i++) {
        const detect_result_t* result = &results.results[i];

        // Draw bounding box
        cv::Point pt1(result->box.left, result->box.top);
        cv::Point pt2(result->box.right, result->box.bottom);
        cv::rectangle(annotated_frame, pt1, pt2, cv::Scalar(0, 255, 0), 2);

        // Prepare label text
        char label_text[256];
        snprintf(label_text, sizeof(label_text), "%s %.1f%%", result->name, result->prop * 100);

        // Calculate text size and background
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label_text, cv::FONT_HERSHEY_SIMPLEX, 0.6, 2, &baseline);

        // Draw label background
        cv::Point label_bg_pt1(result->box.left, result->box.top - text_size.height - 10);
        cv::Point label_bg_pt2(result->box.left + text_size.width, result->box.top);
        cv::rectangle(annotated_frame, label_bg_pt1, label_bg_pt2, cv::Scalar(0, 255, 0), -1);

        // Draw label text
        cv::Point text_pt(result->box.left, result->box.top - 5);
        cv::putText(annotated_frame, label_text, text_pt, cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 0), 2);
    }

    // Draw timestamp and frame info
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    ss << " | Objects: " << results.count;
    ss << " | FPS: " << std::fixed << std::setprecision(1) << fps_.load();

    cv::putText(annotated_frame, ss.str(), cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 2);
    cv::putText(annotated_frame, ss.str(), cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 0), 1);

    return annotated_frame;
}

bool MJPEGStreamer::get_current_jpeg(std::vector<uint8_t>& jpeg_data) {
    std::lock_guard<std::mutex> lock(jpeg_mutex_);
    if (current_jpeg_.empty()) {
        return false;
    }

    jpeg_data = current_jpeg_;
    return true;
}

cv::Mat MJPEGStreamer::validate_and_correct_color_format(const uint8_t* bgr_data, int width, int height) {
    // Create initial frame assuming BGR format
    cv::Mat frame(height, width, CV_8UC3, (void*)bgr_data);

    // Sample center pixel for color validation
    int center_x = width / 2;
    int center_y = height / 2;
    cv::Vec3b center_pixel = frame.at<cv::Vec3b>(center_y, center_x);

    // Sample corner pixels for additional validation
    cv::Vec3b corner_tl = frame.at<cv::Vec3b>(0, 0);
    cv::Vec3b corner_br = frame.at<cv::Vec3b>(height-1, width-1);

    printf("DEBUG: MJPEG input validation - Center pixel BGR: B=%d, G=%d, R=%d\n",
           center_pixel[0], center_pixel[1], center_pixel[2]);

    // Detect potential RGB data in BGR buffer
    // Look for patterns where blue channel has values that seem more like red
    bool likely_rgb_swapped = false;

    // Check if blue channel values are consistently higher than red (suggesting RGB->BGR swap)
    int blue_vs_red_diff = center_pixel[0] - center_pixel[2];
    int corner_blue_vs_red_diff = corner_tl[0] - corner_tl[2];

    if (blue_vs_red_diff > 30 && corner_blue_vs_red_diff > 30) {
        // Blue significantly higher than red in multiple samples suggests RGB data
        likely_rgb_swapped = true;
        printf("WARNING: MJPEG detected likely RGB data in BGR buffer (blue > red by %d)\n", blue_vs_red_diff);
    }

    // Check for unrealistic color values that might indicate format issues
    bool has_extreme_values = (center_pixel[0] > 250 && center_pixel[2] < 50) ||
                             (center_pixel[2] > 250 && center_pixel[0] < 50);

    if (has_extreme_values) {
        printf("WARNING: MJPEG detected extreme color values - possible format mismatch\n");
        likely_rgb_swapped = true;
    }

    if (likely_rgb_swapped) {
        printf("INFO: MJPEG applying RGB->BGR color correction\n");
        cv::Mat corrected_frame;
        cv::cvtColor(frame, corrected_frame, cv::COLOR_RGB2BGR);

        // Verify correction
        cv::Vec3b corrected_center = corrected_frame.at<cv::Vec3b>(center_y, center_x);
        printf("DEBUG: MJPEG after correction - Center pixel BGR: B=%d, G=%d, R=%d\n",
               corrected_center[0], corrected_center[1], corrected_center[2]);

        return corrected_frame;
    }

    return frame;
}

// Debug frame saving function removed for production use

MJPEGStreamer::StreamStats MJPEGStreamer::get_stats() const {
    StreamStats stats;
    stats.clients_connected = clients_connected_.load();
    stats.frames_encoded = frames_encoded_.load();
    stats.frames_dropped = frames_dropped_.load();
    stats.avg_encode_time_ms = avg_encode_time_ms_.load();
    stats.fps = fps_.load();
    return stats;
}
