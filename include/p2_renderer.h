#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int p2_renderer_initialize(int argc,char** argv);
void p2_renderer_shutdown(void);
void p2_renderer_begin_frame(void);
void p2_renderer_end_frame(void);
// Called once at the start of p2_renderer_shutdown (before SDL is torn down).
void p2_renderer_set_shutdown_hook(void (*hook)(void));
#ifdef __cplusplus
}
#endif
