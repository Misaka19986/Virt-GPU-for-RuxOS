#include "log.h"
#include "virtio.h"
#include "virtio_gpu.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

GPUDev *init_gpu_dev(GPURequestedState *requested_state) {
  log_info("initializing GPUDev");

  if (requested_state == NULL) {
    log_error("null requested state");
    return NULL;
  }

  // 分配内存
  GPUDev *dev = malloc(sizeof(GPUDev));

  if (dev == NULL) {
    log_error("failed to initialize GPUDev");
    // 让调用函数来释放内存
    // free(requested_state);
    return NULL;
  }

  memset(dev, 0, sizeof(GPUDev));

  // 初始化config
  dev->config.events_read = 0;
  dev->config.events_clear = 0;
  dev->config.num_scanouts = HVISOR_VIRTIO_GPU_MAX_SCANOUTS;
  dev->config.num_capsets = 0;

  // 初始化scanouts
  dev->scanouts_num = 1;

  // 初始化scanout 0
  // TODO: 使用json初始化，或者读取设备
  dev->scanouts[0].width = 1080;
  dev->scanouts[0].height = 720;

  log_debug("set scanouts[0] width: %d height: %d", dev->scanouts[0].width,
            dev->scanouts[0].height);

  // dev->scanouts[0].x = 0;
  // dev->scanouts[0].y = 0;
  // dev->scanouts[0].resource_id = 0;
  dev->scanouts[0].current_cursor = NULL;
  dev->scanouts[0].enable = true;

  // TODO: 多组requested_states(需要更改json解析)
  dev->requested_states[0].width = requested_state->width;
  dev->requested_states[0].height = requested_state->height;

  log_debug("requested state from json, width: %d height: %d",
            dev->requested_states[0].width, dev->requested_states[0].height);

  free(requested_state);

  return dev;
}

int virtio_gpu_init(VirtIODevice *vdev) {
  // TODO: 显示设备初始化
  vdev->virtio_close = virtio_gpu_close;
  return 0;
}

void virtio_gpu_close(VirtIODevice *vdev) {
  log_info("virtio_gpu close");

  // 回收相关内存
  GPUDev *gdev = (GPUDev *)vdev->dev;
  for (int i = 0; i < gdev->scanouts_num; ++i) {
    free(gdev->scanouts[i].current_cursor);
  }

  free(gdev);
  gdev = NULL;
  // for (int i = 0; i < vdev->vqs_len; ++i) {
  //   // 释放描述符表
  //   if (vdev->vqs[i].desc_table != NULL) {
  //     free((void *)vdev->vqs[i].desc_table);
  //     vdev->vqs[i].desc_table = NULL;
  //   }
  //   // 释放可用环
  //   if (vdev->vqs[i].avail_ring != NULL) {
  //     free((void *)vdev->vqs[i].avail_ring);
  //     vdev->vqs[i].avail_ring = NULL;
  //   }
  //   // 释放已用环
  //   if (vdev->vqs[i].used_ring != NULL) {
  //     free((void *)vdev->vqs[i].used_ring);
  //     vdev->vqs[i].used_ring = NULL;
  //   }
  //   // 销毁互斥锁
  //   pthread_mutex_destroy(&vdev->vqs[i].used_ring_lock);
  // }
  free(vdev->vqs);
  free(vdev);
}

void virtio_gpu_reset() {
  // TODO
}

int virtio_gpu_ctrl_notify_handler(VirtIODevice *vdev, VirtQueue *vq) {
  log_debug("entering %s", __func__);
  GPUDev *gdev = vdev->dev;

  virtqueue_disable_notify(vq);
  while (!virtqueue_is_empty(vq)) {
    int err = virtio_gpu_handle_single_request(vdev, vq);
    if (err < 0) {
      log_error("notify handle failed at zone %d, device %s", vdev->zone_id,
                virtio_device_type_to_string(vdev->type));
      // return -1;
    }
  }
  virtqueue_enable_notify(vq);

  virtio_inject_irq(vq);

  return 0;
}

int virtio_gpu_cursor_notify_handler(VirtIODevice *vdev, VirtQueue *vq) {
  log_debug("entering %s", __func__);
  GPUDev *gdev = vdev->dev;

  virtqueue_disable_notify(vq);
  while (!virtqueue_is_empty(vq)) {
    int err = virtio_gpu_handle_single_request(vdev, vq);
    if (err < 0) {
      log_error("notify handle failed at zone %d, device %s", vdev->zone_id,
                virtio_device_type_to_string(vdev->type));
      // return -1;
    }
  }
  virtqueue_enable_notify(vq);

  virtio_inject_irq(vq);

  return 0;
}

int virtio_gpu_handle_single_request(VirtIODevice *vdev, VirtQueue *vq) {
  // 描述符链的起始idx
  uint16_t first_idx_on_chain;
  // 通信使用的iovec
  struct iovec *iov = NULL;
  // 描述符链上的所有描述符的flags
  uint16_t *flags;
  // 处理的描述符数
  // 描述符数等于缓冲区数，也就等于iov数组的长度
  int desc_processed_num;

  // 根据描述符链，将通信的所有buffer集中到iov进行管理
  desc_processed_num =
      process_descriptor_chain(vq, &first_idx_on_chain, &iov, &flags, 0, true);
  if (desc_processed_num < 1) {
    log_debug("no more desc at %s", __func__);
    return 0;
  }

  // debug
  for (int i = 0; i < desc_processed_num; ++i) {
    char s[50] = "";

    if (flags[i] & VRING_DESC_F_WRITE) {
      strcat(s, "VRING_DESC_F_WRITE | ");
    } else {
      strcat(s, "VRING_DESC_F_READ | ");
    }

    if (flags[i] & VRING_DESC_F_NEXT) {
      strcat(s, "VRING_DESC_F_NEXT | ");
    }
    if (flags[i] & VRING_DESC_F_INDIRECT) {
      strcat(s, "VRING_DESC_F_INDIRECT");
    }

    log_debug("desc %d get flags: %s", i, s);
  }

  // 解析iov，获得相应指令并处理
  // TODO: 把这部分交给其他线程处理，hvisor进程直接从当前函数返回
  int err = virtio_gpu_simple_process_cmd(iov, desc_processed_num,
                                          first_idx_on_chain, vdev);
  if (err < 0) {
    // 丢弃这条指令
    free(flags);
    return -1;
  }

  return 0;
}

size_t iov_to_buf_full(const struct iovec *iov, const int iov_cnt,
                       size_t offset, void *buf, size_t bytes_need_copy) {
  size_t done;
  unsigned int i;
  for (i = 0, done = 0; (offset || done < bytes_need_copy) && i < iov_cnt;
       i++) {
    if (offset < iov[i].iov_len) {
      size_t len = MIN(iov[i].iov_len - offset, bytes_need_copy - done);
      memcpy(buf + done, iov[i].iov_base + offset, len);
      done += len;
      offset = 0;
    } else {
      offset -= iov[i].iov_len;
    }
  }
  if (offset != 0) {
    log_error("failed to copy iov to buf");
    return 0;
  }

  return done;
}

size_t buf_to_iov_full(const struct iovec *iov, int iov_cnt, size_t offset,
                       const void *buf, size_t bytes_need_copy) {
  size_t done;
  unsigned int i;
  for (i = 0, done = 0; (offset || done < bytes_need_copy) && i < iov_cnt;
       i++) {
    if (offset < iov[i].iov_len) {
      size_t len = MIN(iov[i].iov_len - offset, bytes_need_copy - done);
      memcpy(iov[i].iov_base + offset, buf + done, len);
      done += len;
      offset = 0;
    } else {
      offset -= iov[i].iov_len;
    }
  }
  if (offset != 0) {
    log_error("failed to copy buf to iov");
    return 0;
  }

  return done;
}