#ifndef ANLAND_CAMERA_H
#define ANLAND_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

int  anland_camera_start(void);
void anland_camera_stop(void);
void anland_camera_set_resources(int ctrl_fd, const int *stream_fds, int num_cameras);
void anland_camera_clear(void);

#ifdef __cplusplus
}
#endif

#endif