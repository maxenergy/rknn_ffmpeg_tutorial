#!/bin/bash

# Test script for MJPEG streaming functionality
echo "=== MJPEG Streaming Test ==="

# Check if the executable exists
if [ ! -f "src/build/ffmpeg_tutorial" ]; then
    echo "Error: ffmpeg_tutorial executable not found!"
    echo "Please compile the project first."
    exit 1
fi

# Check if a test video file exists
TEST_VIDEO=""
if [ -f "/dev/video0" ]; then
    TEST_VIDEO="/dev/video0"
    echo "Using camera device: $TEST_VIDEO"
elif [ -f "test.mp4" ]; then
    TEST_VIDEO="test.mp4"
    echo "Using test video file: $TEST_VIDEO"
else
    echo "No test video source found."
    echo "You can:"
    echo "1. Connect a USB camera (will use /dev/video0)"
    echo "2. Place a test video file named 'test.mp4' in the current directory"
    echo "3. Use any RTSP stream URL"
    echo ""
    echo "Example usage:"
    echo "  ./src/build/ffmpeg_tutorial rtsp://your-camera-ip/stream"
    echo "  ./src/build/ffmpeg_tutorial /path/to/video.mp4"
    echo ""
    echo "Once running, access the MJPEG stream at:"
    echo "  http://localhost:8090/mjpeg"
    echo "  http://localhost:8090/ (web interface)"
    exit 1
fi

echo ""
echo "Starting MJPEG streaming test..."
echo "The application will:"
echo "1. Process video frames with YOLO object detection"
echo "2. Encode frames using hardware MJPEG encoder"
echo "3. Serve MJPEG stream on http://localhost:8090/mjpeg"
echo "4. Provide web interface at http://localhost:8090/"
echo ""
echo "Press Ctrl+C to stop the application"
echo ""

# Start the application
./src/build/ffmpeg_tutorial "$TEST_VIDEO"
