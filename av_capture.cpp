#include "av_capture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

namespace {

std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string shellQuote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string runCommandCapture(const char* command) {
    std::array<char, 4096> buffer{};
    std::string output;
    FILE* pipe = popen(command, "r");
    if (!pipe) return output;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output += buffer.data();
    pclose(pipe);
    return output;
}

bool commandExists(const char* name) {
    std::string command = "command -v ";
    command += name;
    command += " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

std::string timestampForFilename() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buffer[64];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "capture_%04d%02d%02d_%02d%02d%02d.mp4",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec);
    return buffer;
}

std::vector<std::string> splitTabLine(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return parts;
}

std::string encoderOptions(const std::string& encoder, int bitrate_mbps, int output_width, int output_height) {
    const int bitrate = std::clamp(bitrate_mbps, 2, 80);
    const int width = std::max(2, output_width & ~1);
    const int height = std::max(2, output_height & ~1);
    std::ostringstream ss;
    if (encoder == "h264_nvenc") {
        ss << "-vf scale=" << width << ":" << height << ":flags=bicubic ";
        ss << "-c:v h264_nvenc -preset p4 -tune ll -rc vbr -b:v " << bitrate
           << "M -maxrate " << (bitrate + bitrate / 2) << "M -bufsize " << (bitrate * 2) << "M ";
    } else if (encoder == "h264_qsv") {
        ss << "-vf scale=" << width << ":" << height << ":flags=bicubic ";
        ss << "-c:v h264_qsv -preset veryfast -b:v " << bitrate << "M ";
    } else if (encoder == "h264_vaapi") {
        ss << "-vaapi_device /dev/dri/renderD128 -vf 'scale=" << width << ":" << height
           << ":flags=bicubic,format=nv12,hwupload' -c:v h264_vaapi -b:v " << bitrate << "M ";
    } else {
        ss << "-vf scale=" << width << ":" << height << ":flags=bicubic ";
        ss << "-c:v libx264 -preset veryfast -crf 20 ";
    }
    return ss.str();
}

} // namespace

void refreshAvAudioSources(AvCaptureState& state) {
    state.audio_sources.clear();
    state.audio_sources.push_back({"default", "Default Pulse/PipeWire input"});

    if (!commandExists("pactl")) {
        if (state.selected_audio_source.empty()) state.selected_audio_source = "default";
        state.selected_audio_source_idx = 0;
        return;
    }

    const std::string output = runCommandCapture("pactl list short sources 2>/dev/null");
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        const std::vector<std::string> parts = splitTabLine(line);
        if (parts.size() < 2) continue;
        AvAudioSource source;
        source.name = trimCopy(parts[1]);
        source.description = source.name;
        if (parts.size() >= 3 && !trimCopy(parts[2]).empty()) source.description += " (" + trimCopy(parts[2]) + ")";
        if (!source.name.empty()) state.audio_sources.push_back(source);
    }

    if (state.selected_audio_source.empty()) state.selected_audio_source = state.audio_sources.front().name;
    state.selected_audio_source_idx = 0;
    for (size_t i = 0; i < state.audio_sources.size(); ++i) {
        if (state.audio_sources[i].name == state.selected_audio_source) {
            state.selected_audio_source_idx = static_cast<int>(i);
            break;
        }
    }
}

std::string detectAvHardwareEncoder() {
    if (!commandExists("ffmpeg")) return {};
    const std::string encoders = runCommandCapture("ffmpeg -hide_banner -encoders 2>/dev/null");
    if (encoders.find("h264_nvenc") != std::string::npos) return "h264_nvenc";
    if (encoders.find("h264_qsv") != std::string::npos) return "h264_qsv";
    if (encoders.find("h264_vaapi") != std::string::npos) return "h264_vaapi";
    return "libx264";
}

bool startAvRecording(AvCaptureState& state, const AvCaptureStartOptions& options) {
    if (state.recording) return true;
    if (!commandExists("ffmpeg")) {
        state.status = "FFmpeg is not available on PATH.";
        return false;
    }
    if (options.width <= 0 || options.height <= 0) {
        state.status = "Cannot start recording: invalid capture size.";
        return false;
    }
    if (options.display.empty()) {
        state.status = "Cannot start recording: DISPLAY is unset for X11 capture.";
        return false;
    }

    if (state.audio_sources.empty()) refreshAvAudioSources(state);
    if (state.encoder_name.empty()) state.encoder_name = detectAvHardwareEncoder();
    if (state.encoder_name.empty()) state.encoder_name = "libx264";

    const int width = std::max(2, options.width & ~1);
    const int height = std::max(2, options.height & ~1);
    const int output_width = std::clamp(state.output_width, 320, 7680) & ~1;
    const int output_height = std::clamp(state.output_height, 180, 4320) & ~1;
    state.output_width = std::max(2, output_width);
    state.output_height = std::max(2, output_height);
    const int fps = std::clamp(state.framerate, 10, 120);

    const std::filesystem::path out_dir = options.root / "recordings";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        state.status = "Failed to create recordings directory: " + ec.message();
        return false;
    }
    state.output_path = out_dir / timestampForFilename();

    std::string audio_source = state.selected_audio_source.empty() ? "default" : state.selected_audio_source;
    if (state.selected_audio_source_idx >= 0 &&
        static_cast<size_t>(state.selected_audio_source_idx) < state.audio_sources.size()) {
        audio_source = state.audio_sources[static_cast<size_t>(state.selected_audio_source_idx)].name;
        state.selected_audio_source = audio_source;
    }

    std::ostringstream command;
    command << "ffmpeg -hide_banner -loglevel warning -nostats -y "
            << "-f x11grab -draw_mouse 1 -framerate " << fps
            << " -video_size " << width << "x" << height
            << " -i " << shellQuote(options.display + "+" + std::to_string(options.window_x) + "," + std::to_string(options.window_y)) << " ";

    const bool use_audio = state.include_audio && !audio_source.empty();
    if (use_audio) {
        command << "-f pulse -i " << shellQuote(audio_source) << " -map 0:v:0 -map 1:a:0 ";
    } else {
        command << "-map 0:v:0 ";
    }
    command << encoderOptions(state.encoder_name, state.video_bitrate_mbps, state.output_width, state.output_height);
    if (use_audio) command << "-c:a aac -b:a 160k ";
    command << "-movflags +faststart " << shellQuote(state.output_path.string()) << " 2>&1";

    state.ffmpeg_pipe = popen(command.str().c_str(), "w");
    if (!state.ffmpeg_pipe) {
        state.status = "Failed to launch FFmpeg recorder.";
        state.output_path.clear();
        return false;
    }
    state.recording = true;
    state.status =
        "Recording " + std::to_string(state.output_width) + "x" + std::to_string(state.output_height) +
        " to " + state.output_path.string() + " using " + state.encoder_name + ".";
    return true;
}

bool stopAvRecording(AvCaptureState& state) {
    if (!state.recording) return true;
    if (state.ffmpeg_pipe) {
        std::fputs("q\n", state.ffmpeg_pipe);
        std::fflush(state.ffmpeg_pipe);
        const int rc = pclose(state.ffmpeg_pipe);
        state.ffmpeg_pipe = nullptr;
        state.status = rc == 0
            ? "Recording saved to " + state.output_path.string() + "."
            : "Recording stopped; FFmpeg exited with status " + std::to_string(rc) + ".";
    } else {
        state.status = "Recording stopped.";
    }
    state.recording = false;
    return true;
}

bool toggleAvRecording(AvCaptureState& state, const AvCaptureStartOptions& options) {
    return state.recording ? stopAvRecording(state) : startAvRecording(state, options);
}
