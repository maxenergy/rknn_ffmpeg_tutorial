#ifndef __FFMPEG_H__
#define __FFMPEG_H__

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <iostream>
#include <atomic>
#include <memory>
#include <sys/ioctl.h>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/time.h>

#include <GL/gl.h>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/imgproc/types_c.h>
#include <opencv2/core/opengl.hpp>

#include "rknn_api.h"
#include "yolov5s_postprocess.h"
#include "rknn_utils.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#ifdef __cplusplus
}
#endif

#include "config.h"
#include "drm_func.h"
#include "rga_func.h"
#include "mjpeg_streamer.h"

class FFmpegStreamChannel {
    public:
	/* ffmpeg */
	AVFormatContext *format_context_input;
	int video_stream_index_input;
	int audio_stream_index_input;

	AVCodec *codec_input_video;
	AVCodec *codec_input_audio;

	AVCodecContext *codec_ctx_input_video;
	AVCodecContext *codec_ctx_input_audio;

	int video_frame_size = 0;
	int audio_frame_size = 0;
	int video_frame_count = 0;
	int audio_frame_count = 0;

	drm_context drm_ctx;
	rga_context rga_ctx;
	struct drm_buf drm_buf_for_rga1;
	struct drm_buf drm_buf_for_rga2;

	// Hardware acceleration control
	bool use_software_only = !ENABLE_RGA_HARDWARE;

	// Dimension member variables to prevent corruption
	int display_width_ = WIDTH_P;   // 1280
	int display_height_ = HEIGHT_P; // 720
	int rknn_width_ = 640;          // Will be set from model
	int rknn_height_ = 640;         // Will be set from model

	// MJPEG streaming
	std::unique_ptr<MJPEGStreamer> mjpeg_streamer_;
	bool enable_mjpeg_streaming_ = true;

	bool decode(const char *);
	bool decode_continuous(const char *);
	void stop_processing();
	void cleanup_ffmpeg_contexts();

	// Processing control
	std::atomic<bool> should_stop_processing{false};

	/* rknn */
	const float nms_threshold = NMS_THRESH;
	const float box_conf_threshold = BOX_THRESH;
	rknn_context rknn_ctx;
	int rknn_input_channel = 3;
	int rknn_input_width = 0;
	int rknn_input_height = 0;
	rknn_input inputs[1];
	rknn_input_output_num io_num;
	rknn_tensor_attr *output_attrs;
	int init_rga_drm();
	int init_rknn2();

	// Hardware acceleration helper functions
	int process_frame_hardware(int fd, int src_w, int src_h, int src_pitch);
	int process_frame_software_fallback(AVFrame* frame, int src_w, int src_h, int src_pitch);
	void yuv420p_to_rgb888(const uint8_t* yuv_data, uint8_t* rgb_data, int width, int height);
	void yuv420p_to_bgr888(const uint8_t* yuv_data, uint8_t* bgr_data, int width, int height);
	void nv12_to_rgb888(const uint8_t* nv12_data, uint8_t* rgb_data, int width, int height);
	void nv12_to_bgr888(const uint8_t* nv12_data, uint8_t* bgr_data, int width, int height);

	// Stride-aware conversion functions for proper pitch handling
	void nv12_to_rgb888_stride(const uint8_t* nv12_data, uint8_t* rgb_data, int width, int height, int stride);
	void nv12_to_bgr888_stride(const uint8_t* nv12_data, uint8_t* bgr_data, int width, int height, int stride);
	void yuv420p_to_rgb888_stride(const uint8_t* yuv_data, uint8_t* rgb_data, int width, int height, int stride);
	void yuv420p_to_bgr888_stride(const uint8_t* yuv_data, uint8_t* bgr_data, int width, int height, int stride);

	// RKNN-specific BGR conversion functions
	void nv12_to_bgr888_stride_rknn(const uint8_t* nv12_data, uint8_t* bgr_data, int width, int height, int stride);
	void yuv420p_to_bgr888_stride_rknn(const uint8_t* yuv_data, uint8_t* bgr_data, int width, int height, int stride);

	bool check_rkmpp_decoder_availability(const char* decoder_name);
	bool validate_hardware_acceleration();

	// MJPEG streaming methods
	int init_mjpeg_streaming(int port = 8090);
	void start_mjpeg_streaming();
	void stop_mjpeg_streaming();

	/* opencv */
	std::string window_name;
	GLuint image_texture;
	int init_window();
	void bind_cv_mat_to_gl_texture(cv::Mat& image, GLuint& imageTexture);

	FFmpegStreamChannel()
	{
		printf("DEBUG: Starting FFmpegStreamChannel constructor\n");
		printf("DEBUG: Initial dimensions - display: %dx%d, rknn: %dx%d\n",
			   display_width_, display_height_, rknn_width_, rknn_height_);

		// Initialize FFmpeg pointers to nullptr
		format_context_input = nullptr;
		codec_ctx_input_video = nullptr;
		codec_ctx_input_audio = nullptr;
		codec_input_video = nullptr;
		codec_input_audio = nullptr;
		video_stream_index_input = -1;
		audio_stream_index_input = -1;
		output_attrs = nullptr;

		printf("DEBUG: Calling init_rga_drm()\n");
		if (init_rga_drm() != 0) {
			printf("WARNING: init_rga_drm() failed, hardware acceleration disabled\n");
			use_software_only = true;
		}
		printf("DEBUG: init_rga_drm() completed\n");

		printf("DEBUG: Calling init_rknn2()\n");
		init_rknn2();
		printf("DEBUG: init_rknn2() completed\n");

		printf("DEBUG: Calling init_window()\n");
		init_window();
		printf("DEBUG: init_window() completed\n");

		if (use_software_only) {
			printf("RGA hardware acceleration DISABLED - using software-only processing\n");
		} else {
			printf("RGA hardware acceleration ENABLED with software fallback\n");
		}

		// Initialize MJPEG streaming
		if (enable_mjpeg_streaming_) {
			printf("DEBUG: Initializing MJPEG streaming\n");
			init_mjpeg_streaming();
			start_mjpeg_streaming();
			printf("DEBUG: MJPEG streaming initialized\n");
		}

		printf("DEBUG: Final dimensions - display: %dx%d, rknn: %dx%d\n",
			   display_width_, display_height_, rknn_width_, rknn_height_);
		printf("DEBUG: FFmpegStreamChannel constructor completed\n");
	}

	~FFmpegStreamChannel()
	{
		stop_mjpeg_streaming();
		cleanup_ffmpeg_contexts();
		if (output_attrs) {
			free(output_attrs);
			output_attrs = nullptr;
		}
	}
};

#endif