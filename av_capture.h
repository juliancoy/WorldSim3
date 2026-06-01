#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

struct AvAudioSource {
    std::string name;
    std::string description;
};

struct AvCaptureStartOptions {
    std::filesystem::path root;
    int window_x = 0;
    int window_y = 0;
    int width = 0;
    int height = 0;
    std::string display;
};

struct AvCaptureState {
    bool recording = false;
    bool include_audio = true;
    int selected_audio_source_idx = 0;
    int framerate = 60;
    int video_bitrate_mbps = 12;
    int output_width = 1920;
    int output_height = 1080;
    std::string selected_audio_source;
    std::string encoder_name;
    std::string status;
    std::filesystem::path output_path;
    std::vector<AvAudioSource> audio_sources;
    FILE* ffmpeg_pipe = nullptr;
};

void refreshAvAudioSources(AvCaptureState& state);
std::string detectAvHardwareEncoder();
bool startAvRecording(AvCaptureState& state, const AvCaptureStartOptions& options);
bool stopAvRecording(AvCaptureState& state);
bool toggleAvRecording(AvCaptureState& state, const AvCaptureStartOptions& options);
