#ifndef __MJPEG_STREAMER_H__
#define __MJPEG_STREAMER_H__

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include <vector>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <functional>
#include "config.h"
#include "mpp_encoder.h"
#include "yolov5s_postprocess.h"

// Simple HTTP server implementation
class SimpleHTTPServer {
public:
    SimpleHTTPServer(int port);
    ~SimpleHTTPServer();

    bool start();
    void stop();
    bool is_running() const { return running_; }

    // Set the MJPEG data provider callback
    void set_mjpeg_provider(std::function<bool(std::vector<uint8_t>&)> provider);

private:
    int port_;
    int server_fd_;
    std::atomic<bool> running_;
    std::atomic<bool> should_stop_;
    std::thread server_thread_;
    std::function<bool(std::vector<uint8_t>&)> mjpeg_provider_;

    void server_worker();
    void handle_client(int client_fd);
    void send_mjpeg_stream(int client_fd);
    void send_http_response(int client_fd, const std::string& content_type, const std::string& content);
    void send_index_page(int client_fd);
};

// Frame data structure for thread-safe communication
struct FrameData {
    cv::Mat frame;
    detect_result_group_t detection_results;
    long long timestamp;

    FrameData() : timestamp(0) {
        memset(&detection_results, 0, sizeof(detect_result_group_t));
    }

    FrameData(const cv::Mat& f, const detect_result_group_t& results, long long ts)
        : frame(f.clone()), detection_results(results), timestamp(ts) {}
};

class MJPEGStreamer {
public:
    static const char* BOUNDARY;

    MJPEGStreamer();
    ~MJPEGStreamer();

    // Initialize the streamer
    int init(int port = 8090, int width = 1280, int height = 720);

    // Start the HTTP server in a separate thread
    int start();

    // Stop the HTTP server
    void stop();

    // Add a new frame with detection results to the streaming queue
    void push_frame(const cv::Mat& frame, const detect_result_group_t& detection_results);

    // Push frame from raw BGR data
    void push_frame_raw(const uint8_t* bgr_data, int width, int height, const detect_result_group_t& detection_results);

    // Check if the streamer is running
    bool is_running() const { return running_; }

    // Get streaming statistics
    struct StreamStats {
        int clients_connected;
        int frames_encoded;
        int frames_dropped;
        double avg_encode_time_ms;
        double fps;
    };

    StreamStats get_stats() const;

private:
    std::unique_ptr<SimpleHTTPServer> server_;
    std::unique_ptr<MPPEncoder> encoder_;

    int port_;
    int width_;
    int height_;

    std::atomic<bool> running_;
    std::atomic<bool> should_stop_;

    // Frame processing
    std::thread encoder_thread_;
    std::queue<FrameData> frame_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // JPEG data management
    std::vector<uint8_t> current_jpeg_;
    std::mutex jpeg_mutex_;
    std::condition_variable jpeg_cv_;

    // Statistics
    std::atomic<int> clients_connected_;
    std::atomic<int> frames_encoded_;
    std::atomic<int> frames_dropped_;
    std::atomic<double> avg_encode_time_ms_;
    std::atomic<double> fps_;

    // Worker threads
    void encoder_worker();

    // Frame processing
    cv::Mat draw_detection_results(const cv::Mat& frame, const detect_result_group_t& results);
    cv::Mat validate_and_correct_color_format(const uint8_t* bgr_data, int width, int height);
    void save_debug_frames(const cv::Mat& original_frame, const cv::Mat& annotated_frame, int frame_number);

    // MJPEG provider for HTTP server
    bool get_current_jpeg(std::vector<uint8_t>& jpeg_data);

    // HTTP request handlers
    void handle_index_request(std::string& response);
    void handle_stats_request(std::string& response);

    static const int MAX_QUEUE_SIZE = 5;
};

#endif // __MJPEG_STREAMER_H__
