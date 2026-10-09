#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// One retrace event on the calling/render thread. VIWaitForRetrace also paces it.
void p2_video_retrace(void);
int p2_video_is_black(void);
#ifdef __cplusplus
}
#endif
