#ifndef __MPP_ENCODER_H__
#define __MPP_ENCODER_H__

#include <vector>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

extern "C" {
#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_type.h>
}

// Define MPP_ALIGN if not available
#ifndef MPP_ALIGN
#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

class MPPEncoder {
public:
    MPPEncoder();
    ~MPPEncoder();

    // Initialize the encoder
    int init(int width, int height, int fps = 30, int bitrate = 2000000);

    // Encode a BGR frame to JPEG
    int encode_frame(const cv::Mat& frame, std::vector<uint8_t>& jpeg_data);

    // Encode raw BGR data to JPEG
    int encode_frame_raw(const uint8_t* bgr_data, int width, int height, std::vector<uint8_t>& jpeg_data);

    // Cleanup
    void cleanup();

    // Check if encoder is initialized
    bool is_initialized() const { return initialized_; }

private:
    // MPP context and interface
    MppCtx mpp_ctx_;
    MppApi* mpi_;
    MppEncCfg cfg_;

    // Buffer management
    MppBufferGroup frm_grp_;
    MppBufferGroup pkt_grp_;
    MppBuffer frm_buf_;
    MppBuffer pkt_buf_;
    MppFrame frame_;
    MppPacket packet_;

    // Encoder parameters
    int width_;
    int height_;
    int fps_;
    int bitrate_;
    bool initialized_;

    // Helper methods
    int prepare_frame_buffer();
    int convert_bgr_to_yuv420(const uint8_t* bgr_data, uint8_t* yuv_data);
    int convert_mat_to_yuv420(const cv::Mat& bgr_mat, uint8_t* yuv_data);
};

#endif // __MPP_ENCODER_H__
