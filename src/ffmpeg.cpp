#include "ffmpeg.h"
#include <thread>
#include <chrono>
#include <sys/mman.h>
#include <errno.h>

int FFmpegStreamChannel::init_rga_drm()
{
	/* init drm */
	memset(&drm_ctx, 0, sizeof(drm_context));
	int drm_fd = rknn_drm_init(&drm_ctx);

	/* drm mem1 */
	drm_buf_for_rga1.drm_buf_ptr = rknn_drm_buf_alloc(&drm_ctx, drm_fd, 2560, 1440,
							  4 * 8, // 4 channel x 8bit
							  &drm_buf_for_rga1.drm_buf_fd, &drm_buf_for_rga1.drm_buf_handle, &drm_buf_for_rga1.drm_buf_size);

	/* drm mem2 */
	drm_buf_for_rga2.drm_buf_ptr = rknn_drm_buf_alloc(&drm_ctx, drm_fd, 2560, 1440,
							  4 * 8, // 4 channel x 8bit
							  &drm_buf_for_rga2.drm_buf_fd, &drm_buf_for_rga2.drm_buf_handle, &drm_buf_for_rga2.drm_buf_size);

	/* init rga only if hardware acceleration is enabled */
	memset(&rga_ctx, 0, sizeof(rga_context));
	if (ENABLE_RGA_HARDWARE) {
		int rga_ret = rknn_rga_init(&rga_ctx);
		if (rga_ret != 0) {
			printf("RGA initialization failed (ret=%d), forcing software-only mode\n", rga_ret);
			use_software_only = true;
		}
	} else {
		printf("RGA hardware acceleration disabled by configuration\n");
		use_software_only = true;
	}

	return 0;
}

int FFmpegStreamChannel::init_window()
{
	window_name = "RK3588";
	// cv::namedWindow(window_name, cv::WINDOW_OPENGL);
	// cv::setOpenGlContext(window_name);
	return 0;
}

void FFmpegStreamChannel::bind_cv_mat_to_gl_texture(cv::Mat &image, GLuint &imageTexture)
{
	//glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glGenTextures(1, &imageTexture);
	glBindTexture(GL_TEXTURE_2D, imageTexture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// Set texture clamping method
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

	// cv::cvtColor(image, image, CV_RGB2BGR);

	glTexImage2D(GL_TEXTURE_2D, // Type of texture
		     0, // Pyramid level (for mip-mapping) - 0 is the top level
		     GL_RGB, // Internal colour format to convert to
		     image.cols, // Image width  i.e. 640 for Kinect in standard mode
		     image.rows, // Image height i.e. 480 for Kinect in standard mode
		     0, // Border width in pixels (can either be 1 or 0)
		     GL_RGB, // Input image format (i.e. GL_RGB, GL_RGBA, GL_BGR etc.)
		     GL_UNSIGNED_BYTE, // Image data type
		     image.ptr()); // The actual image data itself
}

int FFmpegStreamChannel::init_rknn2()
{
	printf("Loading mode...\n");
	int model_data_size = 0;
	unsigned char *model_data = load_model(MODEL_PATH, &model_data_size);
	int ret = rknn_init(&rknn_ctx, model_data, model_data_size, 0, NULL);
	if (ret < 0) {
		printf("rknn_init error ret=%d\n", ret);
		return -1;
	}

	rknn_sdk_version version;
	ret = rknn_query(rknn_ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
	if (ret < 0) {
		printf("rknn_init error ret=%d\n", ret);
		return -1;
	}
	printf("sdk version: %s driver version: %s\n", version.api_version, version.drv_version);

	ret = rknn_query(rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
	if (ret < 0) {
		printf("rknn_init error ret=%d\n", ret);
		return -1;
	}
	printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

	rknn_tensor_attr input_attrs[io_num.n_input];
	memset(input_attrs, 0, sizeof(input_attrs));
	for (int i = 0; i < io_num.n_input; i++) {
		input_attrs[i].index = i;
		ret = rknn_query(rknn_ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
		if (ret < 0) {
			printf("rknn_init error ret=%d\n", ret);
			return -1;
		}
		dump_tensor_attr(&(input_attrs[i]));
	}

	output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
	memset(output_attrs, 0, sizeof(output_attrs));
	for (int i = 0; i < io_num.n_output; i++) {
		output_attrs[i].index = i;
		ret = rknn_query(rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
		dump_tensor_attr(&(output_attrs[i]));
	}

	if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
		printf("model is NCHW input fmt\n");
		rknn_input_channel = input_attrs[0].dims[1];
		rknn_input_width = input_attrs[0].dims[2];
		rknn_input_height = input_attrs[0].dims[3];
	} else {
		printf("model is NHWC input fmt\n");
		rknn_input_width = input_attrs[0].dims[1];
		rknn_input_height = input_attrs[0].dims[2];
		rknn_input_channel = input_attrs[0].dims[3];
	}
	printf("model input height=%d, width=%d, channel=%d\n", rknn_input_height, rknn_input_width, rknn_input_channel);

	// Update member variables to prevent corruption
	rknn_width_ = rknn_input_width;
	rknn_height_ = rknn_input_height;
	printf("DEBUG: Updated member dimensions - rknn: %dx%d, display: %dx%d\n",
		   rknn_width_, rknn_height_, display_width_, display_height_);

	memset(inputs, 0, sizeof(inputs));
	inputs[0].index = 0;
	inputs[0].type = RKNN_TENSOR_UINT8;
	inputs[0].size = rknn_input_width * rknn_input_height * rknn_input_channel;
	inputs[0].fmt = RKNN_TENSOR_NHWC;
	inputs[0].pass_through = 0;

	return 0;
}

// Hardware acceleration helper functions
int FFmpegStreamChannel::process_frame_hardware(int fd, int src_w, int src_h)
{
	printf("DEBUG: Hardware processing %dx%d -> RKNN: %dx%d, Display: %dx%d\n",
		   src_w, src_h, rknn_width_, rknn_height_, display_width_, display_height_);

	// Process for RKNN using RGA hardware acceleration
	int ret1 = rknn_img_resize_phy_to_phy(&rga_ctx,
		fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP,
		drm_buf_for_rga1.drm_buf_fd, rknn_width_, rknn_height_, RK_FORMAT_RGB_888);

	if (ret1 != 0) {
		printf("RGA resize for RKNN failed (ret=%d)\n", ret1);
		return ret1;
	}

	// Process for display using RGA hardware acceleration
	int ret2 = rknn_img_resize_phy_to_phy(&rga_ctx,
		fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP,
		drm_buf_for_rga2.drm_buf_fd, display_width_, display_height_, RK_FORMAT_BGR_888);

	if (ret2 != 0) {
		printf("RGA resize for display failed (ret=%d)\n", ret2);
		return ret2;
	}

	printf("Hardware processing completed successfully\n");
	return 0;
}

int FFmpegStreamChannel::process_frame_software_fallback(AVFrame* frame, int src_w, int src_h)
{
	printf("DEBUG: Software fallback processing %dx%d -> RKNN: %dx%d, Display: %dx%d\n",
		   src_w, src_h, rknn_width_, rknn_height_, display_width_, display_height_);

	if (!frame || !frame->data[0]) {
		printf("Error: Invalid frame data for software processing\n");
		return -1;
	}

	// Prepare YUV data buffer
	static std::vector<uint8_t> yuv_buffer;
	uint8_t* yuv_data = nullptr;
	bool need_unmap = false;

	if (frame->format == AV_PIX_FMT_DRM_PRIME) {
		// Handle DRM PRIME frames
		const AVDRMFrameDescriptor *av_drm_frame = reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);
		int fd = av_drm_frame->objects[0].fd;

		void* mapped_ptr = mmap(NULL, av_drm_frame->objects[0].size, PROT_READ, MAP_SHARED, fd, 0);
		if (mapped_ptr != MAP_FAILED) {
			yuv_data = (uint8_t*)mapped_ptr;
			need_unmap = true;
		} else {
			printf("Error: Failed to map DRM buffer: %s\n", strerror(errno));
			return -1;
		}
	} else if (frame->format == AV_PIX_FMT_YUV420P) {
		// Handle YUV420P frames - reconstruct planar data
		int y_size = src_w * src_h;
		int uv_size = (src_w / 2) * (src_h / 2);
		size_t total_size = y_size + 2 * uv_size;

		yuv_buffer.resize(total_size);

		// Copy Y plane
		for (int row = 0; row < src_h; ++row) {
			memcpy(yuv_buffer.data() + row * src_w,
				   frame->data[0] + row * frame->linesize[0], src_w);
		}

		// Copy U plane
		uint8_t* u_dest = yuv_buffer.data() + y_size;
		for (int row = 0; row < src_h/2; ++row) {
			memcpy(u_dest + row * (src_w/2),
				   frame->data[1] + row * frame->linesize[1], src_w/2);
		}

		// Copy V plane
		uint8_t* v_dest = yuv_buffer.data() + y_size + uv_size;
		for (int row = 0; row < src_h/2; ++row) {
			memcpy(v_dest + row * (src_w/2),
				   frame->data[2] + row * frame->linesize[2], src_w/2);
		}

		yuv_data = yuv_buffer.data();
	} else {
		printf("Error: Unsupported frame format for software processing: %d\n", frame->format);
		return -1;
	}

	if (!yuv_data) {
		printf("Error: No YUV data available for software processing\n");
		return -1;
	}

	// Process for RKNN (YUV -> RGB)
	yuv420p_to_rgb888(yuv_data, (uint8_t*)drm_buf_for_rga1.drm_buf_ptr, src_w, src_h);

	// Process for display (YUV -> BGR)
	yuv420p_to_bgr888(yuv_data, (uint8_t*)drm_buf_for_rga2.drm_buf_ptr, src_w, src_h);

	// Cleanup
	if (need_unmap && yuv_data) {
		const AVDRMFrameDescriptor *av_drm_frame = reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);
		munmap(yuv_data, av_drm_frame->objects[0].size);
	}

	printf("Software fallback processing completed\n");
	return 0;
}

void FFmpegStreamChannel::yuv420p_to_rgb888(const uint8_t* yuv_data, uint8_t* rgb_data, int width, int height)
{
	// Simple YUV420P to RGB888 conversion with bilinear scaling to RKNN dimensions
	const uint8_t* y_plane = yuv_data;
	const uint8_t* u_plane = yuv_data + width * height;
	const uint8_t* v_plane = yuv_data + width * height + (width/2) * (height/2);

	float scale_x = (float)width / rknn_width_;
	float scale_y = (float)height / rknn_height_;

	for (int dst_y = 0; dst_y < rknn_height_; dst_y++) {
		for (int dst_x = 0; dst_x < rknn_width_; dst_x++) {
			int src_x = (int)(dst_x * scale_x);
			int src_y = (int)(dst_y * scale_y);

			// Clamp to source bounds
			src_x = std::min(src_x, width - 1);
			src_y = std::min(src_y, height - 1);

			int y = y_plane[src_y * width + src_x];
			int u = u_plane[(src_y/2) * (width/2) + (src_x/2)] - 128;
			int v = v_plane[(src_y/2) * (width/2) + (src_x/2)] - 128;

			// YUV to RGB conversion
			int r = y + (1.402 * v);
			int g = y - (0.344 * u) - (0.714 * v);
			int b = y + (1.772 * u);

			// Clamp values
			r = std::max(0, std::min(255, r));
			g = std::max(0, std::min(255, g));
			b = std::max(0, std::min(255, b));

			int dst_idx = (dst_y * rknn_width_ + dst_x) * 3;
			rgb_data[dst_idx] = r;
			rgb_data[dst_idx + 1] = g;
			rgb_data[dst_idx + 2] = b;
		}
	}
}

void FFmpegStreamChannel::yuv420p_to_bgr888(const uint8_t* yuv_data, uint8_t* bgr_data, int width, int height)
{
	// Simple YUV420P to BGR888 conversion with bilinear scaling to display dimensions
	const uint8_t* y_plane = yuv_data;
	const uint8_t* u_plane = yuv_data + width * height;
	const uint8_t* v_plane = yuv_data + width * height + (width/2) * (height/2);

	float scale_x = (float)width / display_width_;
	float scale_y = (float)height / display_height_;

	for (int dst_y = 0; dst_y < display_height_; dst_y++) {
		for (int dst_x = 0; dst_x < display_width_; dst_x++) {
			int src_x = (int)(dst_x * scale_x);
			int src_y = (int)(dst_y * scale_y);

			// Clamp to source bounds
			src_x = std::min(src_x, width - 1);
			src_y = std::min(src_y, height - 1);

			int y = y_plane[src_y * width + src_x];
			int u = u_plane[(src_y/2) * (width/2) + (src_x/2)] - 128;
			int v = v_plane[(src_y/2) * (width/2) + (src_x/2)] - 128;

			// YUV to RGB conversion
			int r = y + (1.402 * v);
			int g = y - (0.344 * u) - (0.714 * v);
			int b = y + (1.772 * u);

			// Clamp values
			r = std::max(0, std::min(255, r));
			g = std::max(0, std::min(255, g));
			b = std::max(0, std::min(255, b));

			int dst_idx = (dst_y * display_width_ + dst_x) * 3;
			bgr_data[dst_idx] = b;     // BGR format
			bgr_data[dst_idx + 1] = g;
			bgr_data[dst_idx + 2] = r;
		}
	}
}

bool FFmpegStreamChannel::check_rkmpp_decoder_availability(const char* decoder_name)
{
	AVCodec* decoder = avcodec_find_decoder_by_name(decoder_name);
	if (!decoder) {
		printf("RKMPP decoder %s not found\n", decoder_name);
		return false;
	}

	// Check if decoder supports DRM PRIME format
	bool drm_prime_supported = false;
	if (decoder->pix_fmts) {
		for (int i = 0; decoder->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
			if (decoder->pix_fmts[i] == AV_PIX_FMT_DRM_PRIME) {
				drm_prime_supported = true;
				break;
			}
		}
	}

	printf("RKMPP decoder %s: available=%s, DRM_PRIME_support=%s\n",
		   decoder_name, "YES", drm_prime_supported ? "YES" : "NO");

	return true;
}

bool FFmpegStreamChannel::decode(const char *input_stream_url)
{
	int ret;
	long long ts_mark = 0;

	av_register_all();
	avformat_network_init();

	av_log_set_level(AV_LOG_INFO);

	// Check RKMPP decoder availability at startup
	if (!use_software_only) {
		printf("=== Checking RKMPP decoder availability ===\n");
		check_rkmpp_decoder_availability("h264_rkmpp");
		check_rkmpp_decoder_availability("hevc_rkmpp");
		check_rkmpp_decoder_availability("av1_rkmpp");
		check_rkmpp_decoder_availability("vp9_rkmpp");
		printf("=== RKMPP decoder check completed ===\n");
	}

	AVDictionary *opts = NULL;
	av_dict_set(&opts, "rtsp_transport", "+udp+tcp", 0);
	av_dict_set(&opts, "rtsp_flags", "+prefer_tcp", 0);
	av_dict_set(&opts, "threads", "auto", 0);

	format_context_input = avformat_alloc_context();
	ret = avformat_open_input(&format_context_input, input_stream_url, NULL, &opts);
	if (ret < 0) {
		printf("avformat_open_input filed: %d\n", ret);
		return false;
	}

	ret = avformat_find_stream_info(format_context_input, NULL);
	if (ret < 0) {
		printf("avformat_find_stream_info filed: %d\n", ret);
		return false;
	}

	av_dump_format(format_context_input, 0, input_stream_url, 0);

	{
		video_stream_index_input = av_find_best_stream(format_context_input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
		AVStream *stream_input = format_context_input->streams[video_stream_index_input];

		// Detect codec type from stream
		AVCodecID codec_id = stream_input->codec->codec_id;
		const char* codec_name = avcodec_get_name(codec_id);
		printf("Detected video codec: %s (ID: %d)\n", codec_name, codec_id);

		// For software-only mode, prefer software decoder
		if (use_software_only) {
			printf("Using software decoder for software-only mode\n");
			codec_input_video = avcodec_find_decoder(codec_id);
		} else {
			// Select appropriate hardware decoder based on codec type
			const char* hw_decoder_name = nullptr;

			if (codec_id == AV_CODEC_ID_H264) {
				hw_decoder_name = "h264_rkmpp";
			} else if (codec_id == AV_CODEC_ID_HEVC) {
				hw_decoder_name = "hevc_rkmpp";
			} else {
				printf("Codec %s not supported by hardware decoders, using software decoder\n", codec_name);
			}

			// Try hardware decoder first if available for this codec
			if (hw_decoder_name != nullptr) {
				codec_input_video = avcodec_find_decoder_by_name(hw_decoder_name);
				if (codec_input_video != NULL) {
					printf("Using %s hardware decoder for %s\n", hw_decoder_name, codec_name);
				} else {
					printf("%s hardware decoder not available, trying software decoder\n", hw_decoder_name);
				}
			}

			// Fallback to software decoder if hardware decoder not found or not available
			if (codec_input_video == NULL) {
				codec_input_video = avcodec_find_decoder(codec_id);
				if (codec_input_video != NULL) {
					printf("Using software decoder: %s\n", codec_input_video->name);
				}
			}
		}

		if (codec_input_video == NULL) {
			printf("avcodec_find_decoder failed...\n");
			return false;
		}

		codec_ctx_input_video = avcodec_alloc_context3(codec_input_video);
		if (codec_ctx_input_video == NULL) {
			printf("avcodec_alloc_context3 failed...\n");
			return false;
		}

		avcodec_copy_context(codec_ctx_input_video, stream_input->codec);

		// Configure decoder based on type
		bool is_hardware_decoder = (strstr(codec_input_video->name, "_rkmpp") != nullptr);

		codec_ctx_input_video->lowres = codec_input_video->max_lowres;
		codec_ctx_input_video->flags2 |= AV_CODEC_FLAG2_FAST;

		AVDictionary *opts = NULL;
		av_dict_set(&opts, "strict", "1", 0);

		if (use_software_only || !is_hardware_decoder) {
			// Software decoder configuration
			printf("Configuring software decoder (%s) with YUV420P format\n", codec_input_video->name);
		} else {
			// Hardware decoder (h264_rkmpp or hevc_rkmpp) configuration for DRM PRIME output
			printf("Configuring hardware decoder (%s) for DRM PRIME output\n", codec_input_video->name);

			// CRITICAL: Force DRM PRIME output format for hardware decoders
			// This is equivalent to -hwaccel_output_format drm_prime in FFmpeg CLI
			codec_ctx_input_video->get_format = [](struct AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) -> enum AVPixelFormat {
				printf("get_format callback: Available formats: ");
				for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
					printf("%s ", av_get_pix_fmt_name(pix_fmts[i]));
					if (pix_fmts[i] == AV_PIX_FMT_DRM_PRIME) {
						printf("\nget_format callback: Selecting DRM_PRIME format\n");
						return AV_PIX_FMT_DRM_PRIME;
					}
				}
				printf("\nget_format callback: DRM_PRIME not available, using first format: %s\n",
					   av_get_pix_fmt_name(pix_fmts[0]));
				return pix_fmts[0];
			};

			// Enable AFBC (ARM Frame Buffer Compression) if RGA3 is available for better performance
			av_dict_set(&opts, "afbc", "rga", 0);

			// Enable fast parsing for better parallelism
			av_dict_set(&opts, "fast_parse", "1", 0);

			// Set buffer mode for optimal performance
			av_dict_set(&opts, "buf_mode", "half", 0);

			// Enable de-interlacing if needed
			av_dict_set(&opts, "deint", "1", 0);

			printf("Hardware decoder options: afbc=rga, fast_parse=1, buf_mode=half, deint=1\n");
		}

		ret = avcodec_open2(codec_ctx_input_video, codec_input_video, &opts);
		if (ret < 0) {
			printf("avcodec_open2 failed: %d\n", ret);
			av_dict_free(&opts);
			return false;
		}

		av_dict_free(&opts);

		// Verify the actual pixel format after opening the decoder
		printf("Video decoder initialized successfully: %s\n", codec_input_video->name);
		printf("Decoder output pixel format: %s (%d)\n",
			   av_get_pix_fmt_name(codec_ctx_input_video->pix_fmt), codec_ctx_input_video->pix_fmt);

		// For hardware decoders, check if DRM PRIME format is supported
		if (is_hardware_decoder && !use_software_only) {
			bool drm_prime_supported = false;
			for (int i = 0; codec_input_video->pix_fmts && codec_input_video->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
				if (codec_input_video->pix_fmts[i] == AV_PIX_FMT_DRM_PRIME) {
					drm_prime_supported = true;
					break;
				}
			}
			printf("Hardware decoder DRM PRIME support: %s\n", drm_prime_supported ? "YES" : "NO");
		}
	}

	{
		audio_stream_index_input = av_find_best_stream(format_context_input, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
		AVStream *stream_input = format_context_input->streams[video_stream_index_input];
		codec_input_audio = avcodec_find_decoder(stream_input->codec->codec_id);
		codec_ctx_input_audio = avcodec_alloc_context3(codec_input_audio);
		avcodec_copy_context(codec_ctx_input_audio, stream_input->codec);
	}

	/* Init Mat */
	cv::Mat *mat4show = new cv::Mat(cv::Size(display_width_, display_height_), CV_8UC3, drm_buf_for_rga2.drm_buf_ptr);

	printf("DEBUG: Starting frame processing loop...\n");
	printf("DEBUG: Hardware acceleration: %s\n", use_software_only ? "DISABLED" : "ENABLED");
	printf("DEBUG: RKNN dimensions: %dx%d (member: %dx%d)\n", rknn_input_width, rknn_input_height, rknn_width_, rknn_height_);
	printf("DEBUG: Display dimensions: %dx%d (member: %dx%d)\n", WIDTH_P, HEIGHT_P, display_width_, display_height_);

	AVPacket *packet_input_tmp = av_packet_alloc();
	AVFrame *frame_input_tmp = av_frame_alloc();
	while (true) {
		ret = av_read_frame(format_context_input, packet_input_tmp);
		if (ret < 0) {
			break;
		}

		AVStream *stream_input = format_context_input->streams[packet_input_tmp->stream_index];

		/* video */
		if (packet_input_tmp->stream_index == video_stream_index_input) {
			video_frame_size += packet_input_tmp->size;
			video_frame_count++;

			ret = avcodec_send_packet(codec_ctx_input_video, packet_input_tmp);
			if (ret < 0) {
				printf("avcodec_send_packet filed: %d\n", ret);
				return false;
			}

			while (ret >= 0) {
				ret = avcodec_receive_frame(codec_ctx_input_video, frame_input_tmp);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				} else if (ret < 0) {
					printf("avcodec_receive_frame filed: %d\n", ret);
					return false;
				}

				// Handle different frame formats with detailed debugging
				const AVDRMFrameDescriptor *av_drm_frame = nullptr;
				int fd = -1;

				printf("Debug: Received frame format=%d (%s), decoder=%s\n",
					   frame_input_tmp->format,
					   av_get_pix_fmt_name((AVPixelFormat)frame_input_tmp->format),
					   codec_input_video->name);

				if (frame_input_tmp->format == AV_PIX_FMT_DRM_PRIME) {
					av_drm_frame = reinterpret_cast<const AVDRMFrameDescriptor *>(frame_input_tmp->data[0]);
					if (av_drm_frame && av_drm_frame->nb_objects > 0) {
						fd = av_drm_frame->objects[0].fd;
						printf("Debug: DRM PRIME frame - fd=%d, nb_objects=%d, nb_layers=%d\n",
							   fd, av_drm_frame->nb_objects, av_drm_frame->nb_layers);

						// Print layer information for debugging
						for (int i = 0; i < av_drm_frame->nb_layers; i++) {
							const AVDRMLayerDescriptor *layer = &av_drm_frame->layers[i];
							printf("Debug: Layer %d - format=0x%x, nb_planes=%d\n",
								   i, layer->format, layer->nb_planes);
						}
					} else {
						printf("Debug: Invalid DRM PRIME frame descriptor\n");
						fd = -1;
					}
				} else {
					printf("Debug: Software decoded frame, format=%d (%s)\n",
						   frame_input_tmp->format,
						   av_get_pix_fmt_name((AVPixelFormat)frame_input_tmp->format));
				}

				// Fix: Use proper width/height calculation from codec context
				int w = codec_ctx_input_video->width;
				int h = codec_ctx_input_video->height;

				// Validate dimensions to prevent RGA errors
				if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
					printf("Invalid frame dimensions: %dx%d, skipping frame\n", w, h);
					continue;
				}

				// Check if RKNN is properly initialized using member variables
				if (rknn_width_ <= 0 || rknn_height_ <= 0) {
					printf("RKNN not initialized (target size: %dx%d), skipping AI inference\n",
						   rknn_width_, rknn_height_);
					continue;
				}

				// Ensure YUV alignment (align to 2-byte boundary)
				w = (w + 1) & ~1;  // Round up to even number
				h = (h + 1) & ~1;  // Round up to even number

				printf("Processing frame: %dx%d, fd=%d -> RKNN: %dx%d, Display: %dx%d\n",
					   w, h, fd, rknn_width_, rknn_height_, display_width_, display_height_);

				ts_mark = current_timestamp();

				// Unified frame processing using hardware acceleration with software fallback
				int processing_ret = 0;

				if (!use_software_only && fd >= 0) {
					// Try hardware acceleration first (DRM PRIME frames)
					processing_ret = process_frame_hardware(fd, w, h);
					if (processing_ret == 0) {
						printf("Hardware acceleration completed successfully\n");
					} else {
						printf("Hardware acceleration failed (ret=%d), falling back to software\n", processing_ret);
						processing_ret = process_frame_software_fallback(frame_input_tmp, w, h);
					}
				} else {
					// Use software processing (either forced or no DRM fd available)
					printf("Using software processing (hardware %s, fd=%d)\n",
						   use_software_only ? "disabled" : "unavailable", fd);
					processing_ret = process_frame_software_fallback(frame_input_tmp, w, h);
				}

				if (processing_ret != 0) {
					printf("Frame processing failed, skipping RKNN inference\n");
					continue;
				}

				/* rknn2 compute */
				inputs[0].buf = drm_buf_for_rga1.drm_buf_ptr;
				rknn_inputs_set(rknn_ctx, io_num.n_input, inputs);

				rknn_output outputs[io_num.n_output];
				memset(outputs, 0, sizeof(outputs));
				for (int i = 0; i < io_num.n_output; i++) {
					outputs[i].want_float = 0;
				}

				ret = rknn_run(rknn_ctx, NULL);
				ret = rknn_outputs_get(rknn_ctx, io_num.n_output, outputs, NULL);
				printf("DETECT OK---->[%fms]\n", ((double)(current_timestamp() - ts_mark)) / 1000);

				/* post process */
				float scale_w = (float)rknn_width_ / display_width_;
				float scale_h = (float)rknn_height_ / display_height_;

				detect_result_group_t detect_result_group;
				std::vector<float> out_scales;
				std::vector<int32_t> out_zps;
				for (int i = 0; i < io_num.n_output; ++i) {
					out_scales.push_back(output_attrs[i].scale);
					out_zps.push_back(output_attrs[i].zp);
				}
				post_process((int8_t *)outputs[0].buf, (int8_t *)outputs[1].buf, (int8_t *)outputs[2].buf, rknn_height_, rknn_width_,
					     box_conf_threshold, nms_threshold, scale_w, scale_h, out_zps, out_scales, &detect_result_group);
				printf("POST PROCESS OK---->[%fms]\n", ((double)(current_timestamp() - ts_mark)) / 1000);

				/* Draw Objects */
				char text[256];
				char file_name[256];
				for (int i = 0; i < detect_result_group.count; i++) {
					detect_result_t *det_result = &(detect_result_group.results[i]);
					sprintf(text, "%s %.1f%%", det_result->name, det_result->prop * 100);
					sprintf(file_name, "%ld_%s_%.1f", frame_input_tmp->pkt_pts, det_result->name, det_result->prop * 100);

					printf("---->%s @ (%d %d %d %d) %f\n", det_result->name, det_result->box.left, det_result->box.top,
					       det_result->box.right, det_result->box.bottom, det_result->prop);

					int x1 = det_result->box.left;
					int y1 = det_result->box.top;
					int x2 = det_result->box.right;
					int y2 = det_result->box.bottom;

					if (frame_input_tmp->pkt_pts > 0 && std::string(det_result->name) == "person" && processing_ret == 0) {
						// Only save if display buffer is valid
						cv::Rect rect_box(x1, y1, x2 - x1, y2 - y1);
						// Use current directory instead of non-existent path
						std::string save_path = "./detections/" + std::string(file_name) + ".jpg";

						// Create directory if it doesn't exist
						system("mkdir -p ./detections");

						try {
							cv::imwrite(save_path, cv::Mat(*mat4show, rect_box));
							printf("Saved detection: %s\n", save_path.c_str());
						} catch (const std::exception& e) {
							printf("Failed to save detection: %s\n", e.what());
						}
					}

					// rectangle(*mat4show, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(255, 0, 0, 255), 2);
					// putText(*mat4show, text, cv::Point(x1, y1 + 12), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
				}
				printf("DRAW BOX OK---->[%fms]\n", ((double)(current_timestamp() - ts_mark)) / 1000);

				/* Free Outputs */
				rknn_outputs_release(rknn_ctx, io_num.n_output, outputs);

				/* OpenGL */
				// if (image_texture != 0) {
				// 	glDeleteTextures(1, &image_texture);
				// 	image_texture = 0;
				// }
				// bind_cv_mat_to_gl_texture(*mat4show, image_texture);

				/* Opencv */
				// cv::imshow(window_name, *mat4show);
				// cv::waitKey(1);

				printf("SHOW OK---->[%fms]\n", ((double)(current_timestamp() - ts_mark)) / 1000);
			}
		}

		/* audio */
		if (packet_input_tmp->stream_index == audio_stream_index_input) {
			audio_frame_size += packet_input_tmp->size;
			audio_frame_count++;
		}

		av_packet_unref(packet_input_tmp);
		av_frame_unref(frame_input_tmp);
	}

	av_packet_free(&packet_input_tmp);
	avformat_close_input(&format_context_input);
	return true;
}

void FFmpegStreamChannel::stop_processing() {
	should_stop_processing = true;
	printf("Stop processing requested\n");
}

bool FFmpegStreamChannel::decode_continuous(const char *input_stream_url) {
	printf("Starting continuous decode for: %s\n", input_stream_url);

	while (!should_stop_processing) {
		printf("=== Starting video processing session ===\n");

		bool success = decode(input_stream_url);

		if (should_stop_processing) {
			printf("Stop requested during processing, exiting...\n");
			break;
		}

		if (success) {
			printf("Video processing completed normally, restarting...\n");
		} else {
			printf("Video processing failed, retrying in 2 seconds...\n");
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}

		// Brief pause between restarts
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	printf("Continuous decode terminated\n");
	return true;
}
