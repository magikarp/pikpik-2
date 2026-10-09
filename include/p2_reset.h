#pragma once
struct RenderModeInfo;
// Configure the native process reset before original game startup.
bool p2_reset_initialize(int argc,char** argv,void (*shutdown)());
RenderModeInfo* p2_reset_video_state();
