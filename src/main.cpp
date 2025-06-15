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

#include "ffmpeg.h"

bool g_flag_run = 1;

long long current_timestamp()
{
	struct timeval te;
	gettimeofday(&te, NULL);
	long long milliseconds = te.tv_sec * 1000000LL + te.tv_usec;
	return milliseconds;
}

// Global channel pointer for signal handling
FFmpegStreamChannel *g_channel = nullptr;

static void signal_process(int signo)
{
	printf("\nReceived signal %d, shutting down gracefully...\n", signo);
	g_flag_run = false;

	if (g_channel) {
		g_channel->stop_processing();
	}

	// Give some time for cleanup
	sleep(2);
	exit(0);
}

int main(int argc, char *argv[])
{
	printf("DEBUG: Starting main function\n");
	signal(SIGINT, signal_process);
	signal(SIGPIPE, SIG_IGN);

	if (argc < 2) {
		printf("Usage: %s <stream_url>\n", argv[0]);
		printf("Example: %s rtsp://example.com/stream\n", argv[0]);
		return -1;
	}

	printf("DEBUG: Creating FFmpegStreamChannel\n");
	FFmpegStreamChannel *channel = new FFmpegStreamChannel();
	g_channel = channel;  // Set global pointer for signal handling

	printf("DEBUG: Channel created, starting continuous decode with: %s\n", argv[1]);
	printf("INFO: MJPEG stream will be available at http://localhost:8090/mjpeg\n");
	printf("INFO: Web interface available at http://localhost:8090/\n");
	printf("INFO: Press Ctrl+C to stop gracefully\n\n");

	bool result = channel->decode_continuous(argv[1]);
	printf("DEBUG: Continuous decode completed with result: %s\n", result ? "success" : "failure");

	g_channel = nullptr;
	delete channel;
	return result ? 0 : -1;
}