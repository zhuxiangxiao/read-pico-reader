/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：隔空投送（Apple AirDrop 接收端）组件公开接口。
 *
 * English: Public C interface for the Apple AirDrop receiver component.
 *
 * 冻结：仅支持挂载的 TF 卡写入，不回退内部 Flash；离页或拔卡立即终止并清理半截文件。
 * Frozen: Write only to a mounted TF card without internal flash fallback; stop and clean up partial files on exit or media loss.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 隔空投送运行状态。/ AirDrop service lifecycle states.
typedef enum {
    READ_PICO_AIRDROP_STOPPED = 0,  ///< 已停止 / Stopped
    READ_PICO_AIRDROP_STARTING,     ///< 正在启动 / Starting
    READ_PICO_AIRDROP_READY,        ///< 等待投送 / Waiting for sender
    READ_PICO_AIRDROP_RECEIVING,    ///< 正在接收 / Actively receiving
    READ_PICO_AIRDROP_DONE,         ///< 接收完成 / Transfer completed
    READ_PICO_AIRDROP_ERROR,        ///< 发生错误 / Error occurred
} read_pico_airdrop_state_t;

/// 隔空投送目录配置。/ Target directory configuration for AirDrop.
typedef struct {
    const char *books_dir;     ///< 书籍目录 (.epub/.txt) / Books directory
    const char *fonts_dir;     ///< 字体目录 (.ttf/.otf) / Fonts directory
    const char *pictures_dir;  ///< 图片目录 (.jpg/.jpeg/.png) / Pictures directory
    const char *other_dir;     ///< 其他文件目录 / Directory for other files
} read_pico_airdrop_cfg_t;

/// 隔空投送实时状态快照。/ Snapshot of AirDrop service status.
typedef struct {
    read_pico_airdrop_state_t state;  ///< 当前状态 / Current state
    bool rx_active;                   ///< 是否正在接收数据 / Whether a transfer is active
    char device_name[33];             ///< 广播设备名 (kiikoread-XXXX) / Broadcast device name
    char current_file[241];           ///< 当前或最近文件名 / Current or latest filename
    char last_error[96];              ///< 最近错误提示 / Latest human-readable error message
    unsigned completed_files;         ///< 本次会话已完成文件数 / Completed file count in session
    unsigned books_received;          ///< 本次会话已收书籍数 / Received book count in session
    unsigned fonts_received;          ///< 本次会话已收字体数 / Received font count in session
    size_t current_bytes;             ///< 当前或最近文件已写字节 / Bytes written for current/last file
    size_t current_total;             ///< 当前或最近文件总字节 / Total bytes for current/last file
} read_pico_airdrop_status_t;

/// 启动隔空投送接收服务。/ Start the AirDrop receiver service.
esp_err_t read_pico_airdrop_start(const read_pico_airdrop_cfg_t *cfg);

/// 停止隔空投送接收服务并清理未完成文件。/ Stop the AirDrop service and clean up any partial file.
void read_pico_airdrop_stop(void);

/// 主循环周期性推进 AWDL 与流式解包消费。/ Advance AWDL cadence and streaming archive consumer.
void read_pico_airdrop_poll(void);

/// 读取当前隔空投送状态快照（服务未运行时同样有效，用于展示最近一次错误）。
/// Copy the current AirDrop status snapshot (valid while stopped too, for last-error display).
void read_pico_airdrop_get_status(read_pico_airdrop_status_t *out);

/// 服务是否处于运行态。/ Return true when the AirDrop service is running.
bool read_pico_airdrop_active(void);

/// 是否正处于活动接收过程（锁屏前用于避免打断接收）：正在收包、有打开中的文件，或已有
/// 整包归档回了 200 但尚未交给文件回调。
/// Return true while a file transfer is actively in flight: bytes arriving, an open file, or
/// a whole archive already acknowledged with 200 but not yet handed to the file sink.
bool read_pico_airdrop_busy(void);

/// 浅睡前暂停隔空投送（若正在接收则拒绝浅睡）。/ Pause AirDrop before light sleep (refuses if actively receiving).
bool read_pico_airdrop_pause_for_sleep(void);

/// 浅睡唤醒后恢复隔空投送。失败时服务保持停止、状态置 ERROR 且保留 last_error；调用方
/// 即使 active() 为 false 也应读取状态并刷新界面。
/// Resume AirDrop after waking from light sleep. On failure the service stays stopped and
/// the status is left at ERROR with last_error set, so the caller should read the status
/// even when active() is false and repaint from it.
void read_pico_airdrop_resume_after_sleep(void);

#ifdef __cplusplus
}
#endif
