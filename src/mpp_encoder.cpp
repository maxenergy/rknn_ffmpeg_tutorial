#include "mpp_encoder.h"
#include <cstring>
#include <cstdio>

MPPEncoder::MPPEncoder()
    : mpp_ctx_(nullptr), mpi_(nullptr), cfg_(nullptr),
      frm_grp_(nullptr), pkt_grp_(nullptr), frm_buf_(nullptr), pkt_buf_(nullptr),
      frame_(nullptr), packet_(nullptr),
      width_(0), height_(0), fps_(30), bitrate_(2000000), initialized_(false) {
}

MPPEncoder::~MPPEncoder() {
    cleanup();
}

void MPPEncoder::cleanup() {
    if (packet_) {
        mpp_packet_deinit(&packet_);
        packet_ = nullptr;
    }

    if (frame_) {
        mpp_frame_deinit(&frame_);
        frame_ = nullptr;
    }

    if (pkt_buf_) {
        mpp_buffer_put(pkt_buf_);
        pkt_buf_ = nullptr;
    }

    if (frm_buf_) {
        mpp_buffer_put(frm_buf_);
        frm_buf_ = nullptr;
    }

    if (pkt_grp_) {
        mpp_buffer_group_put(pkt_grp_);
        pkt_grp_ = nullptr;
    }

    if (frm_grp_) {
        mpp_buffer_group_put(frm_grp_);
        frm_grp_ = nullptr;
    }

    if (cfg_) {
        mpp_enc_cfg_deinit(cfg_);
        cfg_ = nullptr;
    }

    if (mpp_ctx_) {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = nullptr;
        mpi_ = nullptr;
    }

    initialized_ = false;
}

int MPPEncoder::init(int width, int height, int fps, int bitrate) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_ = bitrate;

    MPP_RET ret = MPP_OK;

    // Create MPP context
    ret = mpp_create(&mpp_ctx_, &mpi_);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to create mpp context\n");
        return -1;
    }

    // Initialize encoder
    ret = mpp_init(mpp_ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to init mpp context\n");
        mpp_destroy(mpp_ctx_);
        return -1;
    }

    // Create encoder config
    ret = mpp_enc_cfg_init(&cfg_);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to init encoder config\n");
        mpp_destroy(mpp_ctx_);
        return -1;
    }

    // Get default config
    ret = mpi_->control(mpp_ctx_, MPP_ENC_GET_CFG, cfg_);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to get encoder config\n");
        return -1;
    }

    // Set encoding parameters
    mpp_enc_cfg_set_s32(cfg_, "prep:width", width_);
    mpp_enc_cfg_set_s32(cfg_, "prep:height", height_);
    mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", MPP_ALIGN(width_, 16));
    mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", MPP_ALIGN(height_, 16));
    mpp_enc_cfg_set_s32(cfg_, "prep:format", MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", bitrate_);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", bitrate_ * 17 / 16);
    mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", bitrate_ * 15 / 16);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", fps_);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", fps_);
    mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_denorm", 1);

    mpp_enc_cfg_set_s32(cfg_, "codec:type", MPP_VIDEO_CodingMJPEG);
    mpp_enc_cfg_set_s32(cfg_, "jpeg:q_factor", 95);  // Increased from 80 to 95 for higher quality
    mpp_enc_cfg_set_s32(cfg_, "jpeg:qf_max", 99);
    mpp_enc_cfg_set_s32(cfg_, "jpeg:qf_min", 1);

    // Apply config
    ret = mpi_->control(mpp_ctx_, MPP_ENC_SET_CFG, cfg_);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to set encoder config\n");
        return -1;
    }

    // Create buffer groups
    ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to get frame buffer group\n");
        return -1;
    }

    ret = mpp_buffer_group_get_internal(&pkt_grp_, MPP_BUFFER_TYPE_ION);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to get packet buffer group\n");
        return -1;
    }

    // Prepare frame buffer
    if (prepare_frame_buffer() != 0) {
        printf("MPP encoder: failed to prepare frame buffer\n");
        return -1;
    }

    initialized_ = true;
    printf("MPP encoder initialized: %dx%d, fps=%d, bitrate=%d\n", width_, height_, fps_, bitrate_);
    return 0;
}

int MPPEncoder::prepare_frame_buffer() {
    MPP_RET ret = MPP_OK;

    // Calculate frame size for YUV420SP
    size_t frame_size = MPP_ALIGN(width_, 16) * MPP_ALIGN(height_, 16) * 3 / 2;

    // Create frame buffer
    ret = mpp_buffer_get(frm_grp_, &frm_buf_, frame_size);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to get frame buffer\n");
        return -1;
    }

    // Create frame
    ret = mpp_frame_init(&frame_);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to init frame\n");
        return -1;
    }

    mpp_frame_set_width(frame_, width_);
    mpp_frame_set_height(frame_, height_);
    mpp_frame_set_hor_stride(frame_, MPP_ALIGN(width_, 16));
    mpp_frame_set_ver_stride(frame_, MPP_ALIGN(height_, 16));
    mpp_frame_set_fmt(frame_, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame_, frm_buf_);

    // Create packet buffer
    size_t packet_size = width_ * height_;
    ret = mpp_buffer_get(pkt_grp_, &pkt_buf_, packet_size);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to get packet buffer\n");
        return -1;
    }

    // Create packet
    ret = mpp_packet_init_with_buffer(&packet_, pkt_buf_);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to init packet\n");
        return -1;
    }

    return 0;
}

int MPPEncoder::convert_mat_to_yuv420(const cv::Mat& bgr_mat, uint8_t* yuv_data) {
    if (bgr_mat.cols != width_ || bgr_mat.rows != height_) {
        printf("MPP encoder: frame size mismatch: expected %dx%d, got %dx%d\n",
               width_, height_, bgr_mat.cols, bgr_mat.rows);
        return -1;
    }

    // Convert BGR to YUV420SP (NV12)
    cv::Mat yuv_mat;
    cv::cvtColor(bgr_mat, yuv_mat, cv::COLOR_BGR2YUV_I420);

    int hor_stride = MPP_ALIGN(width_, 16);
    int ver_stride = MPP_ALIGN(height_, 16);

    // Copy Y plane
    uint8_t* y_src = yuv_mat.data;
    uint8_t* y_dst = yuv_data;
    for (int i = 0; i < height_; i++) {
        memcpy(y_dst + i * hor_stride, y_src + i * width_, width_);
    }

    // Convert planar UV to interleaved UV (NV12 format)
    uint8_t* u_src = yuv_mat.data + width_ * height_;
    uint8_t* v_src = yuv_mat.data + width_ * height_ + (width_ * height_) / 4;
    uint8_t* uv_dst = yuv_data + hor_stride * ver_stride;

    for (int i = 0; i < height_ / 2; i++) {
        for (int j = 0; j < width_ / 2; j++) {
            uv_dst[i * hor_stride + j * 2] = u_src[i * width_ / 2 + j];
            uv_dst[i * hor_stride + j * 2 + 1] = v_src[i * width_ / 2 + j];
        }
    }

    return 0;
}

int MPPEncoder::convert_bgr_to_yuv420(const uint8_t* bgr_data, uint8_t* yuv_data) {
    // Create OpenCV Mat from raw BGR data
    cv::Mat bgr_mat(height_, width_, CV_8UC3, (void*)bgr_data);
    return convert_mat_to_yuv420(bgr_mat, yuv_data);
}

int MPPEncoder::encode_frame(const cv::Mat& frame, std::vector<uint8_t>& jpeg_data) {
    if (!initialized_) {
        printf("MPP encoder: encoder not initialized\n");
        return -1;
    }

    MPP_RET ret = MPP_OK;

    // Convert BGR frame to YUV420SP
    uint8_t* yuv_data = (uint8_t*)mpp_buffer_get_ptr(frm_buf_);
    if (convert_mat_to_yuv420(frame, yuv_data) != 0) {
        printf("MPP encoder: failed to convert frame to YUV420\n");
        return -1;
    }

    // Encode frame
    ret = mpi_->encode_put_frame(mpp_ctx_, frame_);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to put frame\n");
        return -1;
    }

    // Get encoded packet
    ret = mpi_->encode_get_packet(mpp_ctx_, &packet_);
    if (ret != MPP_OK) {
        printf("MPP encoder: failed to get packet\n");
        return -1;
    }

    if (packet_) {
        void* pkt_data = mpp_packet_get_data(packet_);
        size_t pkt_size = mpp_packet_get_length(packet_);

        jpeg_data.resize(pkt_size);
        memcpy(jpeg_data.data(), pkt_data, pkt_size);

        mpp_packet_deinit(&packet_);
        mpp_packet_init_with_buffer(&packet_, pkt_buf_);
    }

    return 0;
}

int MPPEncoder::encode_frame_raw(const uint8_t* bgr_data, int width, int height, std::vector<uint8_t>& jpeg_data) {
    if (width != width_ || height != height_) {
        printf("MPP encoder: frame size mismatch: expected %dx%d, got %dx%d\n",
               width_, height_, width, height);
        return -1;
    }

    cv::Mat frame(height, width, CV_8UC3, (void*)bgr_data);
    return encode_frame(frame, jpeg_data);
}
