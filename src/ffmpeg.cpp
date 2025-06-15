#include "ffmpeg.h"
#include <thread>
#include <chrono>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>

int FFmpegStreamChannel::init_rga_drm()
{
	printf("=== Initializing DRM and RGA Hardware Acceleration ===\n");

	/* init drm */
	memset(&drm_ctx, 0, sizeof(drm_context));
	int drm_fd = rknn_drm_init(&drm_ctx);

	if (drm_fd < 0) {
		printf("ERROR: DRM initialization failed (fd=%d), forcing software-only mode\n", drm_fd);
		use_software_only = true;
		return -1;
	}

	printf("DRM initialization successful (fd=%d)\n", drm_fd);

	/* drm mem1 - for RKNN input (RGB888) */
	printf("Allocating DRM buffer 1 for RKNN input...\n");
	drm_buf_for_rga1.drm_buf_ptr = rknn_drm_buf_alloc(&drm_ctx, drm_fd, 2560, 1440,
							  4 * 8, // 4 channel x 8bit
							  &drm_buf_for_rga1.drm_buf_fd, &drm_buf_for_rga1.drm_buf_handle, &drm_buf_for_rga1.drm_buf_size);

	if (!drm_buf_for_rga1.drm_buf_ptr || drm_buf_for_rga1.drm_buf_fd < 0) {
		printf("ERROR: Failed to allocate DRM buffer 1 (ptr=%p, fd=%d)\n",
			   drm_buf_for_rga1.drm_buf_ptr, drm_buf_for_rga1.drm_buf_fd);
		use_software_only = true;
		return -1;
	}

	printf("DRM buffer 1 allocated: fd=%d, size=%zu bytes\n",
		   drm_buf_for_rga1.drm_buf_fd, drm_buf_for_rga1.drm_buf_size);

	/* drm mem2 - for display output (BGR888) */
	printf("Allocating DRM buffer 2 for display output...\n");
	drm_buf_for_rga2.drm_buf_ptr = rknn_drm_buf_alloc(&drm_ctx, drm_fd, 2560, 1440,
							  4 * 8, // 4 channel x 8bit
							  &drm_buf_for_rga2.drm_buf_fd, &drm_buf_for_rga2.drm_buf_handle, &drm_buf_for_rga2.drm_buf_size);

	if (!drm_buf_for_rga2.drm_buf_ptr || drm_buf_for_rga2.drm_buf_fd < 0) {
		printf("ERROR: Failed to allocate DRM buffer 2 (ptr=%p, fd=%d)\n",
			   drm_buf_for_rga2.drm_buf_ptr, drm_buf_for_rga2.drm_buf_fd);
		use_software_only = true;
		return -1;
	}

	printf("DRM buffer 2 allocated: fd=%d, size=%zu bytes\n",
		   drm_buf_for_rga2.drm_buf_fd, drm_buf_for_rga2.drm_buf_size);

	/* init rga only if hardware acceleration is enabled */
	memset(&rga_ctx, 0, sizeof(rga_context));
	if (ENABLE_RGA_HARDWARE) {
		printf("Initializing RGA hardware acceleration...\n");
		int rga_ret = rknn_rga_init(&rga_ctx);
		if (rga_ret != 0) {
			printf("ERROR: RGA initialization failed (ret=%d), forcing software-only mode\n", rga_ret);
			use_software_only = true;
			return -1;
		}
		printf("RGA hardware acceleration initialized successfully\n");
	} else {
		printf("RGA hardware acceleration disabled by configuration\n");
		use_software_only = true;
	}

	printf("=== DRM/RGA Hardware Acceleration Setup Complete ===\n");
	printf("Hardware acceleration status: %s\n", use_software_only ? "DISABLED" : "ENABLED");

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
int FFmpegStreamChannel::process_frame_hardware(int fd, int src_w, int src_h, int src_pitch)
{
	printf("DEBUG: Hardware processing %dx%d (pitch=%d) -> RKNN: %dx%d, Display: %dx%d\n",
		   src_w, src_h, src_pitch, rknn_width_, rknn_height_, display_width_, display_height_);

	// Validate pitch - should be >= width for proper stride handling
	if (src_pitch < src_w) {
		printf("WARNING: Invalid pitch %d < width %d, using width as pitch\n", src_pitch, src_w);
		src_pitch = src_w;
	}

	// CRITICAL FIX: Ensure proper alignment for RGA operations
	// RGA requires width/height to be aligned to specific boundaries
	int aligned_src_w = (src_w + 15) & ~15;  // Align to 16-byte boundary
	int aligned_src_h = (src_h + 1) & ~1;    // Align to 2-pixel boundary

	if (aligned_src_w != src_w || aligned_src_h != src_h) {
		printf("DEBUG: Aligned source dimensions from %dx%d to %dx%d for RGA compatibility\n",
			   src_w, src_h, aligned_src_w, aligned_src_h);
		src_w = aligned_src_w;
		src_h = aligned_src_h;
	}

	// Try different YUV formats for RGA processing
	// RKMPP typically outputs NV12 (YUV420SP), but format detection might fail
	const struct {
		int rga_format;
		const char* name;
	} yuv_formats[] = {
		{RK_FORMAT_YCbCr_420_SP, "NV12/YUV420SP"},
		{RK_FORMAT_YCbCr_420_P, "YUV420P"},
		{RK_FORMAT_YCrCb_420_SP, "NV21"}
	};

	int ret1 = -1, ret2 = -1;
	bool success = false;

	// Try each format until one works, testing both RGB and BGR output formats
	for (int i = 0; i < 3 && !success; i++) {
		// Test both BGR and RGB formats to find the correct color ordering
		const struct {
			int rga_format;
			const char* name;
		} color_formats[] = {
			{RK_FORMAT_BGR_888, "BGR888"},
			{RK_FORMAT_RGB_888, "RGB888"}
		};

		for (int color_fmt = 0; color_fmt < 2 && !success; color_fmt++) {
			printf("DEBUG: Trying RGA format %s -> %s for RKNN conversion (stride-aware)\n",
				   yuv_formats[i].name, color_formats[color_fmt].name);

			ret1 = rknn_img_resize_phy_to_phy_stride(&rga_ctx,
				fd, src_w, src_h, src_pitch, yuv_formats[i].rga_format,
				drm_buf_for_rga1.drm_buf_fd, rknn_width_, rknn_height_, color_formats[color_fmt].rga_format);

			if (ret1 == 0) {
				printf("DEBUG: RGA RKNN conversion successful with %s -> %s format (stride=%d)\n",
					   yuv_formats[i].name, color_formats[color_fmt].name, src_pitch);

				// Try display conversion with BGR format (OpenCV expects BGR)
				printf("DEBUG: Trying RGA format %s -> BGR888 for display conversion (stride-aware)\n", yuv_formats[i].name);
				ret2 = rknn_img_resize_phy_to_phy_stride(&rga_ctx,
					fd, src_w, src_h, src_pitch, yuv_formats[i].rga_format,
					drm_buf_for_rga2.drm_buf_fd, display_width_, display_height_, RK_FORMAT_BGR_888);

				if (ret2 == 0) {
					printf("DEBUG: RGA display conversion successful with %s -> BGR888 format (stride=%d)\n",
						   yuv_formats[i].name, src_pitch);

					// Store the successful format for future reference
					printf("SUCCESS: Using YUV format %s with RKNN format %s and Display format BGR888\n",
						   yuv_formats[i].name, color_formats[color_fmt].name);
					success = true;
					break;
				} else {
					printf("DEBUG: RGA display conversion failed with %s -> BGR888 format (ret=%d, stride=%d)\n",
						   yuv_formats[i].name, ret2, src_pitch);
				}
			} else {
				printf("DEBUG: RGA RKNN conversion failed with %s -> %s format (ret=%d, stride=%d)\n",
					   yuv_formats[i].name, color_formats[color_fmt].name, ret1, src_pitch);
			}
		}
	}

	if (!success) {
		printf("ERROR: All RGA format attempts failed (stride=%d) - RKNN ret=%d, Display ret=%d\n", src_pitch, ret1, ret2);
		return -1;
	}

	// Enhanced color debugging: Sample multiple pixels to verify color conversion
	if (drm_buf_for_rga2.drm_buf_ptr && drm_buf_for_rga1.drm_buf_ptr) {
		// Debug display buffer (BGR format for OpenCV)
		uint8_t* display_data = (uint8_t*)drm_buf_for_rga2.drm_buf_ptr;
		int center_x = display_width_ / 2;
		int center_y = display_height_ / 2;
		int center_idx = (center_y * display_width_ + center_x) * 3;

		printf("DEBUG: Display buffer (BGR) center pixel: B=%d, G=%d, R=%d\n",
			   display_data[center_idx], display_data[center_idx + 1], display_data[center_idx + 2]);

		// Debug RKNN buffer (format depends on what was successful)
		uint8_t* rknn_data = (uint8_t*)drm_buf_for_rga1.drm_buf_ptr;
		int rknn_center_x = rknn_width_ / 2;
		int rknn_center_y = rknn_height_ / 2;
		int rknn_center_idx = (rknn_center_y * rknn_width_ + rknn_center_x) * 3;

		printf("DEBUG: RKNN buffer center pixel: Ch0=%d, Ch1=%d, Ch2=%d\n",
			   rknn_data[rknn_center_idx], rknn_data[rknn_center_idx + 1], rknn_data[rknn_center_idx + 2]);

		// Sample corner pixels to verify consistency
		int corner_idx = 0; // Top-left corner
		printf("DEBUG: Display corner (0,0) BGR: B=%d, G=%d, R=%d\n",
			   display_data[corner_idx], display_data[corner_idx + 1], display_data[corner_idx + 2]);

		// Check for obvious color swapping patterns
		bool likely_rgb_in_bgr = (display_data[center_idx] > display_data[center_idx + 2]) &&
								 (display_data[center_idx + 2] > display_data[center_idx + 1]);
		if (likely_rgb_in_bgr) {
			printf("WARNING: Color channels may be swapped - Blue > Red > Green suggests RGB data in BGR buffer\n");
		}
	}

	printf("Hardware processing completed successfully with stride=%d\n", src_pitch);
	return 0;
}

int FFmpegStreamChannel::process_frame_software_fallback(AVFrame* frame, int src_w, int src_h, int src_pitch)
{
	printf("DEBUG: Software fallback processing %dx%d (pitch=%d) -> RKNN: %dx%d, Display: %dx%d\n",
		   src_w, src_h, src_pitch, rknn_width_, rknn_height_, display_width_, display_height_);

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
			printf("DEBUG: Successfully mapped DRM buffer, size=%zu\n", av_drm_frame->objects[0].size);
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

	// Determine YUV format based on frame type
	bool is_nv12_format = (frame->format == AV_PIX_FMT_DRM_PRIME);

	// Validate pitch - should be >= width for proper stride handling
	if (src_pitch < src_w) {
		printf("WARNING: Invalid pitch %d < width %d, using width as pitch\n", src_pitch, src_w);
		src_pitch = src_w;
	}

	// Process for RKNN (YUV -> BGR) - RKNN models typically expect BGR input
	printf("DEBUG: Software RKNN conversion: %s(%dx%d, stride=%d) -> BGR888(%dx%d)\n",
		   is_nv12_format ? "NV12" : "YUV420P", src_w, src_h, src_pitch, rknn_width_, rknn_height_);
	if (is_nv12_format) {
		nv12_to_bgr888_stride_rknn(yuv_data, (uint8_t*)drm_buf_for_rga1.drm_buf_ptr, src_w, src_h, src_pitch);
	} else {
		yuv420p_to_bgr888_stride_rknn(yuv_data, (uint8_t*)drm_buf_for_rga1.drm_buf_ptr, src_w, src_h, src_pitch);
	}

	// Process for display (YUV -> BGR)
	printf("DEBUG: Software Display conversion: %s(%dx%d, stride=%d) -> BGR888(%dx%d)\n",
		   is_nv12_format ? "NV12" : "YUV420P", src_w, src_h, src_pitch, display_width_, display_height_);
	if (is_nv12_format) {
		nv12_to_bgr888_stride(yuv_data, (uint8_t*)drm_buf_for_rga2.drm_buf_ptr, src_w, src_h, src_pitch);
	} else {
		yuv420p_to_bgr888_stride(yuv_data, (uint8_t*)drm_buf_for_rga2.drm_buf_ptr, src_w, src_h, src_pitch);
	}

	// Enhanced software fallback color debugging
	if (drm_buf_for_rga2.drm_buf_ptr && drm_buf_for_rga1.drm_buf_ptr) {
		// Debug display buffer (BGR format for OpenCV)
		uint8_t* display_data = (uint8_t*)drm_buf_for_rga2.drm_buf_ptr;
		int center_x = display_width_ / 2;
		int center_y = display_height_ / 2;
		int center_idx = (center_y * display_width_ + center_x) * 3;

		printf("DEBUG: Software Display buffer (BGR) center pixel: B=%d, G=%d, R=%d\n",
			   display_data[center_idx], display_data[center_idx + 1], display_data[center_idx + 2]);

		// Debug RKNN buffer (BGR format for RKNN)
		uint8_t* rknn_data = (uint8_t*)drm_buf_for_rga1.drm_buf_ptr;
		int rknn_center_x = rknn_width_ / 2;
		int rknn_center_y = rknn_height_ / 2;
		int rknn_center_idx = (rknn_center_y * rknn_width_ + rknn_center_x) * 3;

		printf("DEBUG: Software RKNN buffer (BGR) center pixel: B=%d, G=%d, R=%d\n",
			   rknn_data[rknn_center_idx], rknn_data[rknn_center_idx + 1], rknn_data[rknn_center_idx + 2]);

		// Compare values to detect potential issues
		if (abs(display_data[center_idx] - rknn_data[rknn_center_idx]) > 50) {
			printf("WARNING: Large difference between display and RKNN blue channels (%d vs %d)\n",
				   display_data[center_idx], rknn_data[rknn_center_idx]);
		}

		// Check for obvious color swapping patterns
		bool likely_rgb_in_bgr = (display_data[center_idx] > display_data[center_idx + 2]) &&
								 (display_data[center_idx + 2] > display_data[center_idx + 1]);
		if (likely_rgb_in_bgr) {
			printf("WARNING: Software path - Color channels may be swapped - Blue > Red > Green suggests RGB data in BGR buffer\n");
		}
	}

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
	// YUV420P to RGB888 conversion with proper ITU-R BT.601 coefficients and bilinear scaling to RKNN dimensions
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

			// YUV to RGB conversion using proper ITU-R BT.601 coefficients
			// Handle both limited range (16-235) and full range (0-255) YUV
			// Most video content uses limited range, so try that first
			int y_full, u_full, v_full;

			// Check if this looks like limited range (Y should be mostly 16-235)
			if (y >= 16 && y <= 235) {
				// Limited range conversion
				y_full = ((y - 16) * 255) / 219;
				u_full = (u * 255) / 224;  // u is already centered at 0
				v_full = (v * 255) / 224;  // v is already centered at 0
			} else {
				// Full range conversion
				y_full = y;
				u_full = u;
				v_full = v;
			}

			// Clamp Y to valid range
			y_full = std::max(0, std::min(255, y_full));

			int r = y_full + (1.402f * v_full);
			int g = y_full - (0.344136f * u_full) - (0.714136f * v_full);
			int b = y_full + (1.772f * u_full);

			// Clamp values to valid range
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
	// YUV420P to BGR888 conversion with proper ITU-R BT.601 coefficients and bilinear scaling to display dimensions
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

			// YUV to RGB conversion using proper ITU-R BT.601 coefficients
			// Handle both limited range (16-235) and full range (0-255) YUV
			// Most video content uses limited range, so try that first
			int y_full, u_full, v_full;

			// Check if this looks like limited range (Y should be mostly 16-235)
			if (y >= 16 && y <= 235) {
				// Limited range conversion
				y_full = ((y - 16) * 255) / 219;
				u_full = (u * 255) / 224;  // u is already centered at 0
				v_full = (v * 255) / 224;  // v is already centered at 0
			} else {
				// Full range conversion
				y_full = y;
				u_full = u;
				v_full = v;
			}

			// Clamp Y to valid range
			y_full = std::max(0, std::min(255, y_full));

			int r = y_full + (1.402f * v_full);
			int g = y_full - (0.344136f * u_full) - (0.714136f * v_full);
			int b = y_full + (1.772f * u_full);

			// Clamp values to valid range
			r = std::max(0, std::min(255, r));
			g = std::max(0, std::min(255, g));
			b = std::max(0, std::min(255, b));

			int dst_idx = (dst_y * display_width_ + dst_x) * 3;
			bgr_data[dst_idx] = b;     // BGR format: Blue first
			bgr_data[dst_idx + 1] = g; // Green second
			bgr_data[dst_idx + 2] = r; // Red third
		}
	}
}

void FFmpegStreamChannel::nv12_to_rgb888(const uint8_t* nv12_data, uint8_t* rgb_data, int width, int height)
{
	// NV12 to RGB888 conversion with proper color space handling (BT.709 + full range for modern content)
	// NV12 format: Y plane followed by interleaved UV plane
	const uint8_t* y_plane = nv12_data;
	const uint8_t* uv_plane = nv12_data + width * height;

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

			// NV12 has interleaved UV: UVUVUV...
			int uv_x = (src_x / 2) * 2;  // Align to even pixel
			int uv_y = src_y / 2;
			int uv_idx = uv_y * width + uv_x;
			int u = uv_plane[uv_idx] - 128;
			int v = uv_plane[uv_idx + 1] - 128;

			// CRITICAL FIX: Use BT.709 coefficients for modern HD content (like our HEVC video)
			// BT.709 is standard for HD content, BT.601 is for SD content
			// Also handle full range (0-255) vs limited range (16-235)

			// For full range content (which our video uses), Y is already 0-255
			// For limited range, we'd need to scale: Y = (Y - 16) * 255 / 219

			// BT.709 coefficients (more accurate for HD content)
			float r_f = y + (1.5748f * v);
			float g_f = y - (0.1873f * u) - (0.4681f * v);
			float b_f = y + (1.8556f * u);

			// Clamp values to valid range
			int r = std::max(0, std::min(255, (int)r_f));
			int g = std::max(0, std::min(255, (int)g_f));
			int b = std::max(0, std::min(255, (int)b_f));

			int dst_idx = (dst_y * rknn_width_ + dst_x) * 3;
			rgb_data[dst_idx] = r;
			rgb_data[dst_idx + 1] = g;
			rgb_data[dst_idx + 2] = b;
		}
	}
}

void FFmpegStreamChannel::nv12_to_bgr888(const uint8_t* nv12_data, uint8_t* bgr_data, int width, int height)
{
	// NV12 to BGR888 conversion with proper color space handling (BT.709 + full range for modern content)
	// NV12 format: Y plane followed by interleaved UV plane
	const uint8_t* y_plane = nv12_data;
	const uint8_t* uv_plane = nv12_data + width * height;

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

			// NV12 has interleaved UV: UVUVUV...
			int uv_x = (src_x / 2) * 2;  // Align to even pixel
			int uv_y = src_y / 2;
			int uv_idx = uv_y * width + uv_x;
			int u = uv_plane[uv_idx] - 128;
			int v = uv_plane[uv_idx + 1] - 128;

			// CRITICAL FIX: Use BT.709 coefficients for modern HD content (like our HEVC video)
			// BT.709 is standard for HD content, BT.601 is for SD content
			// Also handle full range (0-255) vs limited range (16-235)

			// For full range content (which our video uses), Y is already 0-255
			// For limited range, we'd need to scale: Y = (Y - 16) * 255 / 219

			// BT.709 coefficients (more accurate for HD content)
			float r_f = y + (1.5748f * v);
			float g_f = y - (0.1873f * u) - (0.4681f * v);
			float b_f = y + (1.8556f * u);

			// Clamp values to valid range
			int r = std::max(0, std::min(255, (int)r_f));
			int g = std::max(0, std::min(255, (int)g_f));
			int b = std::max(0, std::min(255, (int)b_f));

			int dst_idx = (dst_y * display_width_ + dst_x) * 3;
			bgr_data[dst_idx] = b;     // BGR format: Blue first
			bgr_data[dst_idx + 1] = g; // Green second
			bgr_data[dst_idx + 2] = r; // Red third
		}
	}
}

// Stride-aware conversion functions for proper pitch handling
void FFmpegStreamChannel::nv12_to_rgb888_stride(const uint8_t* nv12_data, uint8_t* rgb_data, int width, int height, int stride)
{
	// NV12 to RGB888 conversion with proper stride handling for pitch != width
	const uint8_t* y_plane = nv12_data;
	const uint8_t* uv_plane = nv12_data + stride * height;  // Use stride instead of width

	float scale_x = (float)width / rknn_width_;
	float scale_y = (float)height / rknn_height_;

	for (int dst_y = 0; dst_y < rknn_height_; dst_y++) {
		for (int dst_x = 0; dst_x < rknn_width_; dst_x++) {
			int src_x = (int)(dst_x * scale_x);
			int src_y = (int)(dst_y * scale_y);

			// Clamp to source bounds
			src_x = std::min(src_x, width - 1);
			src_y = std::min(src_y, height - 1);

			// Use stride for Y plane indexing
			int y = y_plane[src_y * stride + src_x];

			// NV12 has interleaved UV: UVUVUV...
			int uv_x = (src_x / 2) * 2;  // Align to even pixel
			int uv_y = src_y / 2;
			int uv_idx = uv_y * stride + uv_x;  // Use stride for UV plane indexing
			int u = uv_plane[uv_idx] - 128;
			int v = uv_plane[uv_idx + 1] - 128;

			// BT.709 coefficients for HD content
			float r_f = y + (1.5748f * v);
			float g_f = y - (0.1873f * u) - (0.4681f * v);
			float b_f = y + (1.8556f * u);

			// Clamp values to valid range
			int r = std::max(0, std::min(255, (int)r_f));
			int g = std::max(0, std::min(255, (int)g_f));
			int b = std::max(0, std::min(255, (int)b_f));

			int dst_idx = (dst_y * rknn_width_ + dst_x) * 3;
			rgb_data[dst_idx] = r;
			rgb_data[dst_idx + 1] = g;
			rgb_data[dst_idx + 2] = b;
		}
	}
}

void FFmpegStreamChannel::nv12_to_bgr888_stride(const uint8_t* nv12_data, uint8_t* bgr_data, int width, int height, int stride)
{
	// NV12 to BGR888 conversion with proper stride handling for pitch != width
	const uint8_t* y_plane = nv12_data;
	const uint8_t* uv_plane = nv12_data + stride * height;  // Use stride instead of width

	float scale_x = (float)width / display_width_;
	float scale_y = (float)height / display_height_;

	for (int dst_y = 0; dst_y < display_height_; dst_y++) {
		for (int dst_x = 0; dst_x < display_width_; dst_x++) {
			int src_x = (int)(dst_x * scale_x);
			int src_y = (int)(dst_y * scale_y);

			// Clamp to source bounds
			src_x = std::min(src_x, width - 1);
			src_y = std::min(src_y, height - 1);

			// Use stride for Y plane indexing
			int y = y_plane[src_y * stride + src_x];

			// NV12 has interleaved UV: UVUVUV...
			int uv_x = (src_x / 2) * 2;  // Align to even pixel
			int uv_y = src_y / 2;
			int uv_idx = uv_y * stride + uv_x;  // Use stride for UV plane indexing
			int u = uv_plane[uv_idx] - 128;
			int v = uv_plane[uv_idx + 1] - 128;

			// BT.709 coefficients for HD content
			float r_f = y + (1.5748f * v);
			float g_f = y - (0.1873f * u) - (0.4681f * v);
			float b_f = y + (1.8556f * u);

			// Clamp values to valid range
			int r = std::max(0, std::min(255, (int)r_f));
			int g = std::max(0, std::min(255, (int)g_f));
			int b = std::max(0, std::min(255, (int)b_f));

			int dst_idx = (dst_y * display_width_ + dst_x) * 3;
			bgr_data[dst_idx] = b;     // BGR format: Blue first
			bgr_data[dst_idx + 1] = g; // Green second
			bgr_data[dst_idx + 2] = r; // Red third
		}
	}
}

void FFmpegStreamChannel::yuv420p_to_rgb888_stride(const uint8_t* yuv_data, uint8_t* rgb_data, int width, int height, int stride)
{
	// YUV420P to RGB888 conversion with proper stride handling
	const uint8_t* y_plane = yuv_data;
	const uint8_t* u_plane = yuv_data + stride * height;
	const uint8_t* v_plane = yuv_data + stride * height + (stride/2) * (height/2);

	float scale_x = (float)width / rknn_width_;
	float scale_y = (float)height / rknn_height_;

	for (int dst_y = 0; dst_y < rknn_height_; dst_y++) {
		for (int dst_x = 0; dst_x < rknn_width_; dst_x++) {
			int src_x = (int)(dst_x * scale_x);
			int src_y = (int)(dst_y * scale_y);

			// Clamp to source bounds
			src_x = std::min(src_x, width - 1);
			src_y = std::min(src_y, height - 1);

			// Use stride for plane indexing
			int y = y_plane[src_y * stride + src_x];
			int u = u_plane[(src_y/2) * (stride/2) + (src_x/2)] - 128;
			int v = v_plane[(src_y/2) * (stride/2) + (src_x/2)] - 128;

			// BT.709 coefficients for HD content
			float r_f = y + (1.5748f * v);
			float g_f = y - (0.1873f * u) - (0.4681f * v);
			float b_f = y + (1.8556f * u);

			// Clamp values to valid range
			int r = std::max(0, std::min(255, (int)r_f));
			int g = std::max(0, std::min(255, (int)g_f));
			int b = std::max(0, std::min(255, (int)b_f));

			int dst_idx = (dst_y * rknn_width_ + dst_x) * 3;
			rgb_data[dst_idx] = r;
			rgb_data[dst_idx + 1] = g;
			rgb_data[dst_idx + 2] = b;
		}
	}
}

void FFmpegStreamChannel::yuv420p_to_bgr888_stride(const uint8_t* yuv_data, uint8_t* bgr_data, int width, int height, int stride)
{
	// YUV420P to BGR888 conversion with proper stride handling
	const uint8_t* y_plane = yuv_data;
	const uint8_t* u_plane = yuv_data + stride * height;
	const uint8_t* v_plane = yuv_data + stride * height + (stride/2) * (height/2);

	float scale_x = (float)width / display_width_;
	float scale_y = (float)height / display_height_;

	for (int dst_y = 0; dst_y < display_height_; dst_y++) {
		for (int dst_x = 0; dst_x < display_width_; dst_x++) {
			int src_x = (int)(dst_x * scale_x);
			int src_y = (int)(dst_y * scale_y);

			// Clamp to source bounds
			src_x = std::min(src_x, width - 1);
			src_y = std::min(src_y, height - 1);

			// Use stride for plane indexing
			int y = y_plane[src_y * stride + src_x];
			int u = u_plane[(src_y/2) * (stride/2) + (src_x/2)] - 128;
			int v = v_plane[(src_y/2) * (stride/2) + (src_x/2)] - 128;

			// BT.709 coefficients for HD content
			float r_f = y + (1.5748f * v);
			float g_f = y - (0.1873f * u) - (0.4681f * v);
			float b_f = y + (1.8556f * u);

			// Clamp values to valid range
			int r = std::max(0, std::min(255, (int)r_f));
			int g = std::max(0, std::min(255, (int)g_f));
			int b = std::max(0, std::min(255, (int)b_f));

			int dst_idx = (dst_y * display_width_ + dst_x) * 3;
			bgr_data[dst_idx] = b;     // BGR format: Blue first
			bgr_data[dst_idx + 1] = g; // Green second
			bgr_data[dst_idx + 2] = r; // Red third
		}
	}
}

// Additional stride-aware conversion functions for RKNN BGR input
void FFmpegStreamChannel::yuv420p_to_bgr888_stride_rknn(const uint8_t* yuv_data, uint8_t* bgr_data, int width, int height, int stride)
{
	// YUV420P to BGR888 conversion with proper stride handling for RKNN dimensions
	const uint8_t* y_plane = yuv_data;
	const uint8_t* u_plane = yuv_data + stride * height;
	const uint8_t* v_plane = yuv_data + stride * height + (stride/2) * (height/2);

	float scale_x = (float)width / rknn_width_;
	float scale_y = (float)height / rknn_height_;

	for (int dst_y = 0; dst_y < rknn_height_; dst_y++) {
		for (int dst_x = 0; dst_x < rknn_width_; dst_x++) {
			int src_x = (int)(dst_x * scale_x);
			int src_y = (int)(dst_y * scale_y);

			// Clamp to source bounds
			src_x = std::min(src_x, width - 1);
			src_y = std::min(src_y, height - 1);

			// Use stride for plane indexing
			int y = y_plane[src_y * stride + src_x];
			int u = u_plane[(src_y/2) * (stride/2) + (src_x/2)] - 128;
			int v = v_plane[(src_y/2) * (stride/2) + (src_x/2)] - 128;

			// BT.709 coefficients for HD content
			float r_f = y + (1.5748f * v);
			float g_f = y - (0.1873f * u) - (0.4681f * v);
			float b_f = y + (1.8556f * u);

			// Clamp values to valid range
			int r = std::max(0, std::min(255, (int)r_f));
			int g = std::max(0, std::min(255, (int)g_f));
			int b = std::max(0, std::min(255, (int)b_f));

			int dst_idx = (dst_y * rknn_width_ + dst_x) * 3;
			bgr_data[dst_idx] = b;     // BGR format: Blue first
			bgr_data[dst_idx + 1] = g; // Green second
			bgr_data[dst_idx + 2] = r; // Red third
		}
	}
}

void FFmpegStreamChannel::nv12_to_bgr888_stride_rknn(const uint8_t* nv12_data, uint8_t* bgr_data, int width, int height, int stride)
{
	// NV12 to BGR888 conversion with proper stride handling for RKNN dimensions
	const uint8_t* y_plane = nv12_data;
	const uint8_t* uv_plane = nv12_data + stride * height;  // Use stride instead of width

	float scale_x = (float)width / rknn_width_;
	float scale_y = (float)height / rknn_height_;

	for (int dst_y = 0; dst_y < rknn_height_; dst_y++) {
		for (int dst_x = 0; dst_x < rknn_width_; dst_x++) {
			int src_x = (int)(dst_x * scale_x);
			int src_y = (int)(dst_y * scale_y);

			// Clamp to source bounds
			src_x = std::min(src_x, width - 1);
			src_y = std::min(src_y, height - 1);

			// Use stride for Y plane indexing
			int y = y_plane[src_y * stride + src_x];

			// NV12 has interleaved UV: UVUVUV...
			int uv_x = (src_x / 2) * 2;  // Align to even pixel
			int uv_y = src_y / 2;
			int uv_idx = uv_y * stride + uv_x;  // Use stride for UV plane indexing
			int u = uv_plane[uv_idx] - 128;
			int v = uv_plane[uv_idx + 1] - 128;

			// BT.709 coefficients for HD content
			float r_f = y + (1.5748f * v);
			float g_f = y - (0.1873f * u) - (0.4681f * v);
			float b_f = y + (1.8556f * u);

			// Clamp values to valid range
			int r = std::max(0, std::min(255, (int)r_f));
			int g = std::max(0, std::min(255, (int)g_f));
			int b = std::max(0, std::min(255, (int)b_f));

			int dst_idx = (dst_y * rknn_width_ + dst_x) * 3;
			bgr_data[dst_idx] = b;     // BGR format: Blue first
			bgr_data[dst_idx + 1] = g; // Green second
			bgr_data[dst_idx + 2] = r; // Red third
		}
	}
}

bool FFmpegStreamChannel::check_rkmpp_decoder_availability(const char* decoder_name)
{
	printf("Checking decoder: %s\n", decoder_name);

	AVCodec* decoder = avcodec_find_decoder_by_name(decoder_name);
	if (!decoder) {
		printf("❌ RKMPP decoder %s not found\n", decoder_name);
		return false;
	}

	printf("✅ Decoder %s found (type: %s)\n", decoder_name,
		   decoder->type == AVMEDIA_TYPE_VIDEO ? "video" : "other");

	// Check if decoder supports DRM PRIME format
	bool drm_prime_supported = false;
	bool nv12_supported = false;
	int format_count = 0;

	printf("   Supported pixel formats: ");
	if (decoder->pix_fmts) {
		for (int i = 0; decoder->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
			format_count++;
			const char* fmt_name = av_get_pix_fmt_name(decoder->pix_fmts[i]);
			printf("%s ", fmt_name ? fmt_name : "unknown");

			if (decoder->pix_fmts[i] == AV_PIX_FMT_DRM_PRIME) {
				drm_prime_supported = true;
			} else if (decoder->pix_fmts[i] == AV_PIX_FMT_NV12) {
				nv12_supported = true;
			}
		}
	} else {
		printf("(format list not available)");
	}
	printf("\n");

	printf("   Format support: DRM_PRIME=%s, NV12=%s, total_formats=%d\n",
		   drm_prime_supported ? "YES" : "NO",
		   nv12_supported ? "YES" : "NO",
		   format_count);

	return true;
}

bool FFmpegStreamChannel::validate_hardware_acceleration()
{
	printf("=== Validating Hardware Acceleration Capabilities ===\n");

	// Check DRM device access
	printf("Checking DRM device access...\n");
	int test_fd = open("/dev/dri/card0", O_RDWR);
	if (test_fd < 0) {
		printf("❌ Cannot access /dev/dri/card0: %s\n", strerror(errno));
		return false;
	}
	close(test_fd);
	printf("✅ DRM device accessible\n");

	// Check RGA library availability
	printf("Checking RGA library availability...\n");
	void* rga_handle = dlopen("/usr/lib/aarch64-linux-gnu/librga.so", RTLD_LAZY);
	if (!rga_handle) {
		printf("❌ RGA library not found: %s\n", dlerror());
		return false;
	}

	// Check for required RGA functions
	void* init_func = dlsym(rga_handle, "c_RkRgaInit");
	void* blit_func = dlsym(rga_handle, "c_RkRgaBlit");
	void* deinit_func = dlsym(rga_handle, "c_RkRgaDeInit");

	if (!init_func || !blit_func || !deinit_func) {
		printf("❌ Required RGA functions not found\n");
		dlclose(rga_handle);
		return false;
	}

	dlclose(rga_handle);
	printf("✅ RGA library and functions available\n");

	// Check RKNN library availability
	printf("Checking RKNN library availability...\n");
	void* rknn_handle = dlopen("/lib/librknnrt.so", RTLD_LAZY);
	if (!rknn_handle) {
		// Try alternative location
		rknn_handle = dlopen("/usr/lib/aarch64-linux-gnu/librknnrt.so", RTLD_LAZY);
		if (!rknn_handle) {
			// Try system search
			rknn_handle = dlopen("librknnrt.so", RTLD_LAZY);
			if (!rknn_handle) {
				printf("❌ RKNN library not found: %s\n", dlerror());
				printf("   Tried: /lib/librknnrt.so, /usr/lib/aarch64-linux-gnu/librknnrt.so, librknnrt.so\n");
				return false;
			}
		}
	}
	dlclose(rknn_handle);
	printf("✅ RKNN library available\n");

	printf("=== Hardware Acceleration Validation Complete ===\n");
	return true;
}

bool FFmpegStreamChannel::decode(const char *input_stream_url)
{
	int ret;
	long long ts_mark = 0;

	av_register_all();
	avformat_network_init();

	av_log_set_level(AV_LOG_INFO);

	// Validate hardware acceleration capabilities at startup
	if (!use_software_only) {
		printf("=== Hardware Acceleration Startup Validation ===\n");

		// First validate the hardware environment
		if (!validate_hardware_acceleration()) {
			printf("❌ Hardware acceleration validation failed, forcing software-only mode\n");
			use_software_only = true;
		} else {
			printf("✅ Hardware acceleration environment validated\n");

			// Then check RKMPP decoder availability
			printf("=== Checking RKMPP decoder availability ===\n");
			check_rkmpp_decoder_availability("h264_rkmpp");
			check_rkmpp_decoder_availability("hevc_rkmpp");
			check_rkmpp_decoder_availability("av1_rkmpp");
			check_rkmpp_decoder_availability("vp9_rkmpp");
			printf("=== RKMPP decoder check completed ===\n");
		}
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
				printf("=== get_format callback triggered ===\n");
				printf("Decoder: %s\n", ctx->codec ? ctx->codec->name : "unknown");
				printf("Available pixel formats: ");

				bool drm_prime_available = false;
				bool nv12_available = false;
				enum AVPixelFormat fallback_format = AV_PIX_FMT_NONE;

				for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
					const char* fmt_name = av_get_pix_fmt_name(pix_fmts[i]);
					printf("%s(%d) ", fmt_name ? fmt_name : "unknown", pix_fmts[i]);

					if (pix_fmts[i] == AV_PIX_FMT_DRM_PRIME) {
						drm_prime_available = true;
					} else if (pix_fmts[i] == AV_PIX_FMT_NV12) {
						nv12_available = true;
					}

					if (fallback_format == AV_PIX_FMT_NONE) {
						fallback_format = pix_fmts[i];
					}
				}
				printf("\n");

				// Priority selection: DRM_PRIME > NV12 > first available
				if (drm_prime_available) {
					printf("✅ Selecting DRM_PRIME format for hardware acceleration\n");
					printf("=== get_format callback complete ===\n");
					return AV_PIX_FMT_DRM_PRIME;
				} else if (nv12_available) {
					printf("⚠️  DRM_PRIME not available, selecting NV12 format\n");
					printf("=== get_format callback complete ===\n");
					return AV_PIX_FMT_NV12;
				} else {
					printf("❌ Neither DRM_PRIME nor NV12 available, using fallback: %s\n",
						   av_get_pix_fmt_name(fallback_format));
					printf("=== get_format callback complete ===\n");
					return fallback_format;
				}
			};

			// Configure hardware decoder for optimal DRM PRIME output
			printf("Configuring hardware decoder options for DRM PRIME output...\n");

			// Enable AFBC (ARM Frame Buffer Compression) if RGA3 is available for better performance
			av_dict_set(&opts, "afbc", "rga", 0);
			printf("   - AFBC enabled for RGA compatibility\n");

			// Enable fast parsing for better parallelism
			av_dict_set(&opts, "fast_parse", "1", 0);
			printf("   - Fast parsing enabled\n");

			// Set buffer mode for optimal performance
			av_dict_set(&opts, "buf_mode", "half", 0);
			printf("   - Buffer mode set to half\n");

			// Enable de-interlacing if needed - CRITICAL for scan line artifacts
			av_dict_set(&opts, "deint", "1", 0);
			printf("   - De-interlacing enabled\n");

			// Force DRM PRIME output format - CRITICAL for hardware acceleration
			av_dict_set(&opts, "output_format", "drm_prime", 0);
			printf("   - Output format forced to DRM PRIME\n");

			// Force NV12 pixel format for RGA compatibility
			av_dict_set(&opts, "pixel_format", "nv12", 0);
			printf("   - Pixel format set to NV12\n");

			// Additional options for better hardware acceleration
			av_dict_set(&opts, "zero_copy", "1", 0);
			printf("   - Zero-copy mode enabled\n");

			printf("Hardware decoder configuration complete\n");
		}

		printf("Opening video codec with configured options...\n");
		ret = avcodec_open2(codec_ctx_input_video, codec_input_video, &opts);
		if (ret < 0) {
			char error_buf[256];
			av_strerror(ret, error_buf, sizeof(error_buf));
			printf("❌ avcodec_open2 failed: %d (%s)\n", ret, error_buf);

			// If hardware decoder failed, try software fallback
			if (is_hardware_decoder && !use_software_only) {
				printf("Hardware decoder failed, attempting software fallback...\n");
				use_software_only = true;

				// Find software decoder
				codec_input_video = avcodec_find_decoder(codec_ctx_input_video->codec_id);
				if (codec_input_video) {
					printf("Found software decoder: %s\n", codec_input_video->name);

					// Clear hardware-specific options
					av_dict_free(&opts);

					// Retry with software decoder
					ret = avcodec_open2(codec_ctx_input_video, codec_input_video, NULL);
					if (ret < 0) {
						av_strerror(ret, error_buf, sizeof(error_buf));
						printf("❌ Software decoder also failed: %d (%s)\n", ret, error_buf);
						return false;
					}
					printf("✅ Software decoder opened successfully\n");
				} else {
					printf("❌ No software decoder available\n");
					av_dict_free(&opts);
					return false;
				}
			} else {
				av_dict_free(&opts);
				return false;
			}
		} else {
			printf("✅ Video codec opened successfully\n");
			if (is_hardware_decoder && !use_software_only) {
				printf("Hardware decoder (%s) is active and ready for DRM PRIME output\n", codec_input_video->name);
			} else {
				printf("Software decoder (%s) is active\n", codec_input_video->name);
			}
			av_dict_free(&opts);
		}

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
				printf("avcodec_send_packet failed: %d (recoverable error, skipping packet...)\n", ret);
				continue;  // Skip this packet and continue with next
			}

			while (ret >= 0) {
				ret = avcodec_receive_frame(codec_ctx_input_video, frame_input_tmp);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				} else if (ret < 0) {
					printf("avcodec_receive_frame failed: %d (recoverable error, continuing...)\n", ret);
					break;  // Break from inner loop but continue processing
				}

				// Handle different frame formats with detailed debugging
				const AVDRMFrameDescriptor *av_drm_frame = nullptr;
				int fd = -1;

				printf("=== Frame Processing Debug ===\n");
				printf("Received frame: format=%d (%s), decoder=%s\n",
					   frame_input_tmp->format,
					   av_get_pix_fmt_name((AVPixelFormat)frame_input_tmp->format),
					   codec_input_video->name);
				printf("Frame dimensions: %dx%d\n", frame_input_tmp->width, frame_input_tmp->height);

				if (frame_input_tmp->format == AV_PIX_FMT_DRM_PRIME) {
					printf("✅ DRM PRIME frame detected - hardware acceleration possible\n");
					av_drm_frame = reinterpret_cast<const AVDRMFrameDescriptor *>(frame_input_tmp->data[0]);
					if (av_drm_frame && av_drm_frame->nb_objects > 0) {
						fd = av_drm_frame->objects[0].fd;
						printf("DRM PRIME details:\n");
						printf("   - File descriptor: %d\n", fd);
						printf("   - Number of objects: %d\n", av_drm_frame->nb_objects);
						printf("   - Number of layers: %d\n", av_drm_frame->nb_layers);

						// Validate the DRM frame structure
						if (fd > 0 && av_drm_frame->nb_layers > 0) {
							printf("   - Object size: %zu bytes\n", av_drm_frame->objects[0].size);
							if (av_drm_frame->layers[0].nb_planes > 0) {
								printf("   - First plane format: %u\n", av_drm_frame->layers[0].format);
								printf("   - First plane pitch: %d\n", av_drm_frame->layers[0].planes[0].pitch);
								printf("   - First plane offset: %d\n", av_drm_frame->layers[0].planes[0].offset);
							}
							printf("✅ DRM PRIME frame structure is valid\n");
						} else {
							printf("❌ DRM PRIME frame structure is invalid (fd=%d, layers=%d)\n",
								   fd, av_drm_frame->nb_layers);
							fd = -1;
						}

						// Print layer information for debugging
						for (int i = 0; i < av_drm_frame->nb_layers; i++) {
							const AVDRMLayerDescriptor *layer = &av_drm_frame->layers[i];
							printf("Debug: Layer %d - format=0x%x, nb_planes=%d\n",
								   i, layer->format, layer->nb_planes);

							// Check for common YUV formats
							const char* format_name = "UNKNOWN";
							switch (layer->format) {
								case 0x3231564e: format_name = "NV12"; break;  // 'NV12'
								case 0x3132564e: format_name = "NV21"; break;  // 'NV21'
								case 0x30323449: format_name = "I420"; break;  // 'I420'
								case 0x56595559: format_name = "YUYV"; break;  // 'YUYV'
								case 0x59565955: format_name = "UYVY"; break;  // 'UYVY'
								default: break;
							}
							printf("Debug: Layer %d format name: %s\n", i, format_name);

							// CRITICAL FIX: If format is 0x0 (unknown), infer format from frame characteristics
							if (layer->format == 0x0 && layer->nb_planes == 2) {
								// For RKMPP decoders, 2-plane format is typically NV12
								printf("Debug: Format 0x0 detected with 2 planes - inferring NV12 format\n");

								// Verify this looks like NV12 by checking plane characteristics
								if (layer->nb_planes >= 2) {
									int y_plane_size = layer->planes[0].pitch * codec_ctx_input_video->height;
									int uv_plane_offset = layer->planes[1].offset;

									printf("Debug: Y plane size=%d, UV plane offset=%d\n", y_plane_size, uv_plane_offset);

									// NV12 has UV plane starting after Y plane
									if (uv_plane_offset >= y_plane_size * 3/4) {  // Allow some tolerance
										format_name = "NV12 (inferred)";
										printf("Debug: Frame characteristics match NV12 format\n");
									}
								}
							}

							// Print plane information
							for (int j = 0; j < layer->nb_planes; j++) {
								printf("Debug: Layer %d, Plane %d - offset=%d, pitch=%d\n",
									   i, j, layer->planes[j].offset, layer->planes[j].pitch);
							}
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

				// CRITICAL FIX: Use frame dimensions instead of codec context for actual frame size
				// This prevents scan line artifacts caused by dimension mismatches
				int w, h, pitch = 0;

				if (frame_input_tmp->format == AV_PIX_FMT_DRM_PRIME) {
					// For DRM PRIME frames, use the actual frame dimensions
					w = frame_input_tmp->width;
					h = frame_input_tmp->height;

					// Extract pitch from DRM frame descriptor
					if (av_drm_frame && av_drm_frame->nb_layers > 0 && av_drm_frame->layers[0].nb_planes > 0) {
						pitch = av_drm_frame->layers[0].planes[0].pitch;
						printf("DEBUG: Extracted pitch=%d from DRM frame (width=%d)\n", pitch, w);
					} else {
						pitch = w;  // Fallback to width if pitch extraction fails
						printf("DEBUG: Failed to extract pitch, using width=%d as pitch\n", pitch);
					}

					// If frame dimensions are 0, fall back to codec context
					if (w <= 0 || h <= 0) {
						w = codec_ctx_input_video->width;
						h = codec_ctx_input_video->height;
						if (pitch <= 0) pitch = w;  // Update pitch if dimensions changed
					}
				} else {
					// For software frames, use codec context dimensions
					w = codec_ctx_input_video->width;
					h = codec_ctx_input_video->height;
					pitch = w;  // For software frames, pitch equals width
				}

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

				// CRITICAL: Ensure proper YUV alignment to prevent scan line artifacts
				// Different alignment requirements for different processing paths
				if (frame_input_tmp->format == AV_PIX_FMT_DRM_PRIME) {
					// For hardware DRM frames, align to 16-byte boundaries (RGA requirement)
					w = (w + 15) & ~15;  // Align to 16-byte boundary
					h = (h + 1) & ~1;    // Align to 2-pixel boundary
				} else {
					// For software frames, align to 2-byte boundaries
					w = (w + 1) & ~1;  // Round up to even number
					h = (h + 1) & ~1;  // Round up to even number
				}

				printf("Processing frame: %dx%d (pitch=%d), fd=%d -> RKNN: %dx%d, Display: %dx%d\n",
					   w, h, pitch, fd, rknn_width_, rknn_height_, display_width_, display_height_);

				ts_mark = current_timestamp();

				// Unified frame processing using hardware acceleration with software fallback
				int processing_ret = 0;

				if (!use_software_only && fd >= 0) {
					// Try hardware acceleration first (DRM PRIME frames)
					processing_ret = process_frame_hardware(fd, w, h, pitch);
					if (processing_ret == 0) {
						printf("Hardware acceleration completed successfully\n");
					} else {
						printf("Hardware acceleration failed (ret=%d), falling back to software\n", processing_ret);
						processing_ret = process_frame_software_fallback(frame_input_tmp, w, h, pitch);
					}
				} else {
					// Use software processing (either forced or no DRM fd available)
					printf("Using software processing (hardware %s, fd=%d)\n",
						   use_software_only ? "disabled" : "unavailable", fd);
					processing_ret = process_frame_software_fallback(frame_input_tmp, w, h, pitch);
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

				/* MJPEG Streaming */
				if (mjpeg_streamer_ && mjpeg_streamer_->is_running()) {
					// Push frame to MJPEG streamer with detection results
					mjpeg_streamer_->push_frame_raw((uint8_t*)drm_buf_for_rga2.drm_buf_ptr,
													display_width_, display_height_,
													detect_result_group);
				}

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

void FFmpegStreamChannel::cleanup_ffmpeg_contexts() {
	printf("Cleaning up FFmpeg contexts...\n");

	// Close codec contexts
	if (codec_ctx_input_video) {
		avcodec_close(codec_ctx_input_video);
		avcodec_free_context(&codec_ctx_input_video);
		codec_ctx_input_video = nullptr;
	}

	if (codec_ctx_input_audio) {
		avcodec_close(codec_ctx_input_audio);
		avcodec_free_context(&codec_ctx_input_audio);
		codec_ctx_input_audio = nullptr;
	}

	// Close format context
	if (format_context_input) {
		avformat_close_input(&format_context_input);
		format_context_input = nullptr;
	}

	// Reset stream indices
	video_stream_index_input = -1;
	audio_stream_index_input = -1;

	// Reset codec pointers
	codec_input_video = nullptr;
	codec_input_audio = nullptr;

	printf("FFmpeg contexts cleaned up\n");
}

bool FFmpegStreamChannel::decode_continuous(const char *input_stream_url) {
	printf("Starting continuous decode for: %s\n", input_stream_url);

	int restart_count = 0;
	int consecutive_failures = 0;
	const int max_consecutive_failures = 5;

	while (!should_stop_processing) {
		restart_count++;
		printf("=== Starting video processing session #%d ===\n", restart_count);

		// Clean up any existing contexts before starting (skip on first run)
		if (restart_count > 1) {
			cleanup_ffmpeg_contexts();
		}

		bool success = decode(input_stream_url);

		if (should_stop_processing) {
			printf("Stop requested during processing, exiting...\n");
			break;
		}

		if (success) {
			printf("Video processing completed normally (session #%d), restarting...\n", restart_count);
			consecutive_failures = 0;  // Reset failure counter on success
		} else {
			consecutive_failures++;
			printf("Video processing failed (session #%d, failure #%d/%d), retrying in 2 seconds...\n",
				   restart_count, consecutive_failures, max_consecutive_failures);

			if (consecutive_failures >= max_consecutive_failures) {
				printf("ERROR: Too many consecutive failures (%d), stopping continuous processing\n", consecutive_failures);
				break;
			}

			std::this_thread::sleep_for(std::chrono::seconds(2));
		}

		// Brief pause between restarts to prevent rapid cycling
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	// Final cleanup
	cleanup_ffmpeg_contexts();
	printf("Continuous decode terminated after %d sessions\n", restart_count);
	return true;
}

// MJPEG streaming methods
int FFmpegStreamChannel::init_mjpeg_streaming(int port) {
	if (!enable_mjpeg_streaming_) {
		return 0;
	}

	mjpeg_streamer_.reset(new MJPEGStreamer());
	if (mjpeg_streamer_->init(port, display_width_, display_height_) != 0) {
		printf("Failed to initialize MJPEG streamer\n");
		mjpeg_streamer_.reset();
		return -1;
	}

	printf("MJPEG streamer initialized on port %d\n", port);
	return 0;
}

void FFmpegStreamChannel::start_mjpeg_streaming() {
	if (!mjpeg_streamer_) {
		return;
	}

	if (mjpeg_streamer_->start() != 0) {
		printf("Failed to start MJPEG streamer\n");
		mjpeg_streamer_.reset();
	} else {
		printf("MJPEG streaming started successfully\n");
	}
}

void FFmpegStreamChannel::stop_mjpeg_streaming() {
	if (mjpeg_streamer_) {
		mjpeg_streamer_->stop();
		mjpeg_streamer_.reset();
		printf("MJPEG streaming stopped\n");
	}
}

bool FFmpegStreamChannel::init_for_multi_stream(int mjpeg_port) {
	printf("Initializing channel for multi-stream on port %d\n", mjpeg_port);

	// Stop any existing MJPEG streaming
	stop_mjpeg_streaming();

	// Initialize MJPEG streaming on the specified port
	if (init_mjpeg_streaming(mjpeg_port) != 0) {
		printf("ERROR: Failed to initialize MJPEG streaming on port %d\n", mjpeg_port);
		return false;
	}

	// Start MJPEG streaming
	start_mjpeg_streaming();

	printf("Multi-stream channel initialized successfully on port %d\n", mjpeg_port);
	return true;
}
