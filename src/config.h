#ifndef __CONFIG_H__
#define __CONFIG_H__

#define CLEAR(x) memset(&(x), 0, sizeof(x))

#define BUFFER_COUNT 4
#define FMT_NUM_PLANES 1

#define WIDTH_P 1280
#define HEIGHT_P 720

// RGA Hardware Acceleration Control
// Set to 0 to disable RGA and use software-only processing
// Set to 1 to attempt RGA with software fallback
#ifndef ENABLE_RGA_HARDWARE
#define ENABLE_RGA_HARDWARE 1
#endif

// Software processing optimization flags
#define ENABLE_SIMD_OPTIMIZATION 1
#define ENABLE_MULTITHREADED_CONVERSION 1
#define SOFTWARE_PROCESSING_THREADS 2

struct drm_buf {
	int drm_buf_fd = -1;
	unsigned int drm_buf_handle;
	void *drm_buf_ptr = NULL;
	size_t drm_buf_size = 0;
};

long long current_timestamp();

#endif