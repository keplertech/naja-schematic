#pragma once

#include <string>

struct AppState;

// Records a scripted walk through the viewer to a video: the app drawn
// offscreen at a fixed size, one frame per fixed time step, no mouse,
// driven by a JSON script of steps (camera moves, crossfades, opening
// instances, traces, selection, styles, captions), each frame piped to
// ffmpeg. See DemoRecorder.cpp for the script format.
namespace DemoRecorder {

// Reads the script; false (and a message on stderr) when it can't.
bool load(const std::string& scriptPath);
// Window size the script asks for (pixels).
int width();
int height();
// Sets the panels' sizes and installs the frame hooks on `state`. Call
// after the GL context and ImGui are set up.
void install(AppState& state);
// The script ran to its end (or failed): the app should quit.
bool done();
// Exit status: 0 when the video was written.
int status();

} // namespace DemoRecorder
