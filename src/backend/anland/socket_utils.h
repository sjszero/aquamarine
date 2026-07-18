// src/backend/anland/socket_utils.h
#ifndef ANLAND_SOCKET_UTILS_H
#define ANLAND_SOCKET_UTILS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 通过 SCM_RIGHTS 发送文件描述符
 *
 * @param sock      套接字 fd
 * @param data      要发送的数据
 * @param data_len  数据长度
 * @param fds       要发送的 fd 数组
 * @param fd_count  fd 数量
 * @return 0 成功, -1 失败
 */
int send_fds(int sock, const void *data, size_t data_len,
             const int *fds, int fd_count);

/*
 * 通过 SCM_RIGHTS 接收文件描述符
 *
 * @param sock           套接字 fd
 * @param data           接收数据缓冲区
 * @param data_len       数据缓冲区大小
 * @param fds            接收 fd 的数组
 * @param fd_count       数组容量
 * @param fds_received   实际接收的 fd 数量
 * @return 接收的数据长度, -1 失败
 */
int recv_fds(int sock, void *data, size_t data_len,
             int *fds, int fd_count, int *fds_received);

/*
 * 连接到 Unix Domain Socket
 *
 * @param path   socket 路径
 * @return fd, -1 失败
 */
int connect_unix(const char *path);

/*
 * 发送全部数据（阻塞直到全部发送）
 *
 * @param fd   套接字 fd
 * @param buf  数据缓冲区
 * @param len  数据长度
 * @return 0 成功, -1 失败
 */
int send_all(int fd, const void *buf, size_t len);

/*
 * 接收全部数据（阻塞直到全部接收）
 *
 * @param fd   套接字 fd
 * @param buf  数据缓冲区
 * @param len  数据长度
 * @return 0 成功, -1 失败
 */
int recv_all(int fd, void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_SOCKET_UTILS_H */