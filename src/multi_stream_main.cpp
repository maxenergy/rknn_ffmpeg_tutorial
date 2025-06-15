#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <chrono>

#include "ffmpeg.h"

bool g_flag_run = 1;
std::atomic<bool> g_shutdown_requested{false};

long long current_timestamp()
{
    struct timeval te;
    gettimeofday(&te, NULL);
    long long milliseconds = te.tv_sec * 1000000LL + te.tv_usec;
    return milliseconds;
}

// Global channels for signal handling
std::vector<std::unique_ptr<FFmpegStreamChannel>> g_channels;

static void signal_process(int signo)
{
    printf("\nReceived signal %d, shutting down gracefully...\n", signo);
    g_flag_run = false;
    g_shutdown_requested = true;

    // Stop all channels
    for (auto& channel : g_channels) {
        if (channel) {
            channel->stop_processing();
        }
    }

    // Give some time for cleanup
    sleep(3);
    exit(0);
}

// Structure to hold stream configuration
struct StreamConfig {
    std::string video_path;
    int mjpeg_port;
    int stream_id;
};

// Worker function for each video stream
void stream_worker(const StreamConfig& config) {
    printf("INFO: Starting stream %d - %s on port %d\n",
           config.stream_id, config.video_path.c_str(), config.mjpeg_port);

    auto channel = std::make_unique<FFmpegStreamChannel>();

    // Configure the channel for this specific stream
    if (!channel->init_for_multi_stream(config.mjpeg_port)) {
        printf("ERROR: Failed to initialize channel for stream %d\n", config.stream_id);
        return;
    }

    // Start continuous decoding
    bool result = channel->decode_continuous(config.video_path.c_str());

    printf("INFO: Stream %d completed with result: %s\n",
           config.stream_id, result ? "success" : "failure");
}

int main(int argc, char *argv[])
{
    printf("=== Multi-Stream Video Processing System ===\n");
    printf("INFO: Starting 8-channel concurrent video processing\n");

    signal(SIGINT, signal_process);
    signal(SIGPIPE, SIG_IGN);

    // Define video files and port assignments
    std::vector<StreamConfig> stream_configs = {
        {"/userdata/videos/2.mp4", 8090, 1},
        {"/userdata/videos/3.mp4", 8091, 2},
        {"/userdata/videos/4.mp4", 8092, 3},
        {"/userdata/videos/5.mp4", 8093, 4},
        {"/userdata/videos/6.mp4", 8094, 5},
        {"/userdata/videos/7.mp4", 8095, 6},
        {"/userdata/videos/8.mp4", 8096, 7},
        {"/userdata/videos/9.mp4", 8097, 8}
    };

    // Check if video files exist
    printf("INFO: Checking video files availability...\n");
    for (const auto& config : stream_configs) {
        FILE* file = fopen(config.video_path.c_str(), "r");
        if (file) {
            fclose(file);
            printf("  ✓ Stream %d: %s\n", config.stream_id, config.video_path.c_str());
        } else {
            printf("  ✗ Stream %d: %s (file not found)\n", config.stream_id, config.video_path.c_str());
        }
    }

    printf("\nINFO: Starting concurrent video streams...\n");
    printf("INFO: MJPEG streams will be available on ports 8090-8097 (accessible from any network)\n");
    printf("INFO: Web interface will be available at http://YOUR_SERVER_IP:8090/multi\n");
    printf("INFO: Individual streams: http://YOUR_SERVER_IP:809X/mjpeg (X=0-7)\n");
    printf("INFO: Press Ctrl+C to stop all streams gracefully\n\n");

    // Create and start worker threads for each stream
    std::vector<std::thread> worker_threads;

    for (const auto& config : stream_configs) {
        worker_threads.emplace_back(stream_worker, config);

        // Small delay between starting streams to avoid resource conflicts
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Wait for all threads to complete or shutdown signal
    for (auto& thread : worker_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    printf("INFO: All streams have been stopped\n");
    return 0;
}
