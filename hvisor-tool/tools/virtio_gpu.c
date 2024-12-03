#include "virtio_gpu.h"
#include "linux/stddef.h"
#include "linux/types.h"
#include "linux/virtio_gpu.h"
#include "log.h"
#include "sys/queue.h"
#include "virtio.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

void virtio_gpu_ctrl_response(VirtIODevice *vdev, GPUCommand *gcmd,
                              GPUControlHeader *resp, size_t resp_len) {
  log_debug("sending response");
  // 因为每个response结构体的头部都是GPUControlHeader，因此其地址就是response结构体的地址

  // iov[0]所对应的第一个描述符一定是只读的，因此从第二个开始
  size_t s =
      buf_to_iov(&gcmd->resp_iov[1], gcmd->resp_iov_cnt - 1, 0, resp, resp_len);

  if (s != resp_len) {
    log_error("%s cannot copy buffer to iov with correct size", __func__);
    // 继续返回，交由前端处理
  }

  // 根据命令选择返回vq
  int sel = GPU_CONTROL_QUEUE;

  if (gcmd->control_header.type == VIRTIO_GPU_CMD_UPDATE_CURSOR ||
      gcmd->control_header.type == VIRTIO_GPU_CMD_MOVE_CURSOR) {
    sel = GPU_CURSOR_QUEUE;
  }

  update_used_ring(&vdev->vqs[sel], gcmd->resp_idx, resp_len);

  gcmd->finished = true;
}

void virtio_gpu_ctrl_response_nodata(VirtIODevice *vdev, GPUCommand *gcmd,
                                     enum virtio_gpu_ctrl_type type) {
  log_debug("entering %s", __func__);

  GPUControlHeader resp;

  memset(&resp, 0, sizeof(resp));
  resp.type = type;
  virtio_gpu_ctrl_response(vdev, gcmd, &resp, sizeof(resp));
}

void virtio_gpu_get_display_info(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);

  struct virtio_gpu_resp_display_info display_info;
  GPUDev *gdev = vdev->dev;

  memset(&display_info, 0, sizeof(display_info));
  display_info.hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;

  // 向响应结构体填入display信息
  for (int i = 0; i < HVISOR_VIRTIO_GPU_MAX_SCANOUTS; ++i) {
    if (gdev->enabled_scanout_bitmask & (1 << i)) {
      display_info.pmodes[i].enabled = 1;
      display_info.pmodes[i].r.width = gdev->requested_states[i].width;
      display_info.pmodes[i].r.height = gdev->requested_states[i].height;
      log_debug("return display info of scanout %d with width %d, height %d", i,
                display_info.pmodes[i].r.width,
                display_info.pmodes[i].r.height);
    }
  }

  virtio_gpu_ctrl_response(vdev, gcmd, &display_info.hdr, sizeof(display_info));
}

void virtio_gpu_get_edid(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);
  // TODO(root):
}

void virtio_gpu_resource_create_2d(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);

  GPUSimpleResource *res;
  GPUDev *gdev = vdev->dev;
  struct virtio_gpu_resource_create_2d create_2d;

  VIRTIO_GPU_FILL_CMD(gcmd->resp_iov, gcmd->resp_iov_cnt, create_2d);

  // 检查想要创建的resource_id是否是0(无法使用0)
  if (create_2d.resource_id == 0) {
    log_error("%s trying to create 2d resource with id 0", __func__);
    gcmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    return;
  }

  // 检查资源是否已经创建
  res = virtio_gpu_find_resource(gdev, create_2d.resource_id);
  if (res) {
    log_error("%s trying to create an existing resource with id %d", __func__,
              create_2d.resource_id);
    gcmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    return;
  }

  // 否则新建一个resource
  res = calloc(1, sizeof(GPUSimpleResource));
  memset(res, 0, sizeof(GPUSimpleResource));

  res->width = create_2d.width;
  res->height = create_2d.height;
  res->format = create_2d.format;
  res->resource_id = create_2d.resource_id;
  res->scanout_bitmask = 0;
  res->iov = NULL;
  res->iov_cnt = 0;

  // 计算resource所占用的内存大小
  // 默认只支持bpp为4 bytes大小的format
  res->hostmem = calc_image_hostmem(32, create_2d.width, create_2d.height);
  if (res->hostmem + gdev->hostmem >= VIRTIO_GPU_MAX_HOSTMEM) {
    log_error("virtio gpu for zone %d out of hostmem when trying to create "
              "resource %d",
              vdev->zone_id, create_2d.resource_id);
    free(res);
    return;
  }

  // 内存足够，将res加入virtio gpu下管理
  TAILQ_INSERT_HEAD(&gdev->resource_list, res, next);
  gdev->hostmem += res->hostmem;

  log_debug("add a resource %d to gpu dev of zone %d, width: %d height: %d "
            "format: %d mem: %d bytes host-hostmem: %d bytes",
            res->resource_id, vdev->zone_id, res->width, res->height,
            res->format, res->hostmem, gdev->hostmem);
}

GPUSimpleResource *virtio_gpu_find_resource(GPUDev *gdev,
                                            uint32_t resource_id) {
  GPUSimpleResource *temp_res = NULL;
  TAILQ_FOREACH(temp_res, &gdev->resource_list, next) {
    if (temp_res->resource_id == resource_id) {
      return temp_res;
    }
  }
  return NULL;
}

GPUSimpleResource *virtio_gpu_check_resource(VirtIODevice *vdev,
                                             uint32_t resource_id,
                                             const char *caller,
                                             uint32_t *error) {
  GPUSimpleResource *res = NULL;
  GPUDev *gdev = vdev->dev;

  res = virtio_gpu_find_resource(gdev, resource_id);
  if (!res) {
    log_error("%s cannot find resource by id %d", caller, resource_id);
    if (error) {
      *error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    }
    return NULL;
  }

  if (!res->iov || res->hostmem <= 0) {
    log_error("%s found resource %d has no backing storage", caller,
              resource_id);
    if (error) {
      *error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    }
    return NULL;
  }

  return res;
}

uint32_t calc_image_hostmem(int bits_per_pixel, uint32_t width,
                            uint32_t height) {
  // 0x1f = 31, >> 5等价于除32，即将每行的bits数对齐到4 bytes的倍数
  // 最后乘sizeof(uint32)获得字节数
  int stride = ((width * bits_per_pixel + 0x1f) >> 5) * sizeof(uint32_t);
  // 返回所占内存的总大小
  return height * stride;
}

void virtio_gpu_resource_unref(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);
}

void virtio_gpu_resource_flush(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);

  GPUDev *gdev = vdev->dev;

  GPUSimpleResource *res = NULL;
  GPUScanout *scanout = NULL;
  struct virtio_gpu_resource_flush resource_flush;

  VIRTIO_GPU_FILL_CMD(gcmd->resp_iov, gcmd->resp_iov_cnt, resource_flush);

  res = virtio_gpu_check_resource(vdev, resource_flush.resource_id, __func__,
                                  &gcmd->error);
  if (!res) {
    return;
  }

  if (resource_flush.r.x > res->width || resource_flush.r.y > res->height ||
      resource_flush.r.width > res->width ||
      resource_flush.r.height > res->height ||
      resource_flush.r.x + resource_flush.r.width > res->width ||
      resource_flush.r.y + resource_flush.r.height > res->height) {
    log_error("%s flush bounds outside resource %d bounds, (%d, %d) + %d, %d "
              "vs %d, %d",
              __func__, resource_flush.resource_id, resource_flush.r.x,
              resource_flush.r.y, resource_flush.r.width,
              resource_flush.r.height, res->width, res->height);
    gcmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    return;
  }

  for (int i = 0; i < HVISOR_VIRTIO_GPU_MAX_SCANOUTS; ++i) {
    // 遍历resource所对应的scanout
    if (!(res->scanout_bitmask & (1 << i))) {
      continue;
    }
    scanout = &gdev->scanouts[i];

    if (!scanout->frame_buffer.enabled) {
      virtio_gpu_create_framebuffer(scanout, &gcmd->error);
      if (gcmd->error) {
        return;
      }

      virtio_gpu_copy_and_flush(scanout, res, &gcmd->error);
    } else {
      // TODO: 已经分配过一次framebuffer，若大小不合适，则需要重新分配
      // TODO: 包括munmap，以及调用drm的销毁函数
      // TODO: 将没分配过的情况封装
      virtio_gpu_copy_and_flush(scanout, res, &gcmd->error);
    }
  }
}

void virtio_gpu_create_framebuffer(GPUScanout *scanout, uint32_t *error) {
  if (scanout->frame_buffer.enabled) {
    return;
  }

  // 若scanout的frame_buffer还没有初始化过
  GPUFrameBuffer *fb = &scanout->frame_buffer;
}

void virtio_gpu_copy_and_flush(GPUScanout *scanout, GPUSimpleResource *res,
                               uint32_t *error) {

  uint32_t format = 0;
  uint32_t stride = 0;
  uint32_t src_offset = 0;
  uint32_t dst_offset = 0;
  int bpp = 0;

  GPUFrameBuffer *fb = &scanout->frame_buffer;

  if (!res || !res->iov || res->hostmem <= 0) {
    log_error("%s found res is not create yet");
    *error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    return;
  }

  if (!fb || !fb->enabled || !fb->fb_addr) {
    log_error("%s found drm_framebuffer is not enabled yet");
    *error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    return;
  }

  format = res->format;
  bpp = 32;
  stride = res->hostmem / res->height;

  // 从res拷贝到缓冲区
  if (res->transfer_rect.x || res->transfer_rect.width != res->width) {
    for (int h = 0; h < res->transfer_rect.height; h++) {
      src_offset = res->transfer_offset + stride * h;
      dst_offset =
          (res->transfer_rect.y + h) * stride + (res->transfer_rect.x * bpp);

      iov_to_buf(res->iov, res->iov_cnt, src_offset, fb->fb_addr + dst_offset,
                 res->transfer_rect.width * bpp);
    }
  } else {
    src_offset = res->transfer_offset;
    dst_offset = res->transfer_rect.y * stride + res->transfer_rect.x * bpp;

    iov_to_buf(res->iov, res->iov_cnt, src_offset, fb->fb_addr + dst_offset,
               stride * res->transfer_rect.height);
  }
}

void virtio_gpu_remove_framebuffer(GPUScanout *scanout, uint32_t *error) {
  GPUFrameBuffer *fb = &scanout->frame_buffer;

  if (!fb || !fb->enabled || !fb->fb_addr) {
    log_error("%s found drm_framebuffer is not enabled yet");
    *error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    return;
  }
}

void virtio_gpu_set_scanout(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);

  GPUDev *gdev = vdev->dev;

  GPUSimpleResource *res = NULL;
  GPUFrameBuffer fb = {0};
  struct virtio_gpu_set_scanout set_scanout;

  VIRTIO_GPU_FILL_CMD(gcmd->resp_iov, gcmd->resp_iov_cnt, set_scanout);

  // 检查scanout id是否有效
  if (set_scanout.scanout_id >= gdev->scanouts_num) {
    log_error("%s setting invalid scanout with scanout_id %d", __func__,
              set_scanout.scanout_id);
    gcmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
    return;
  }

  // 不能使用id为0的resource
  // if (set_scanout.resource_id == 0) {

  // }

  res = virtio_gpu_check_resource(vdev, set_scanout.resource_id, __func__,
                                  &gcmd->error);
  if (!res) {
    return;
  }

  log_debug("%s setting scanout %d with resource %d", __func__,
            set_scanout.scanout_id, set_scanout.resource_id);

  fb.format = res->format;
  fb.bytes_pp = 4; // format都是32bits pp，即4bytes pp
  fb.width = res->width;
  fb.height = res->height;
  fb.stride = res->hostmem / res->height; // hostmem = height * stride
  fb.offset = set_scanout.r.x * fb.bytes_pp + set_scanout.r.y * fb.stride;
  fb.fb_addr = NULL;
  fb.enabled = false;

  // 进入真正的设置函数
  virtio_gpu_do_set_scanout(vdev, set_scanout.scanout_id, &fb, res,
                            &set_scanout.r, &gcmd->error);
}

bool virtio_gpu_do_set_scanout(VirtIODevice *vdev, uint32_t scanout_id,
                               GPUFrameBuffer *fb, GPUSimpleResource *res,
                               struct virtio_gpu_rect *r, uint32_t *error) {
  GPUDev *gdev = vdev->dev;
  GPUScanout *scanout = NULL;

  scanout = &gdev->scanouts[scanout_id];

  if (r->x > fb->width || r->y > fb->width || r->width < 16 || r->height < 16 ||
      r->width > fb->width || r->height > fb->height ||
      r->x + r->width > fb->width || r->y + r->height > fb->height) {
    log_error("%s found illegal scanout %d bounds for resource %d, rect (%d, "
              "%d) + %d, %d, fb %d, %d",
              __func__, scanout_id, res->resource_id, r->x, r->y, r->width,
              r->height, fb->width, fb->height);
    *error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    return false;
  }

  log_debug("%s set scanout display region (%d, %d) + %d, %d, with framebuffer "
            "%d, %d",
            __func__, r->x, r->y, r->width, r->height, fb->width, fb->height);

  // TODO(root): blob相关逻辑(若支持VIRTIO_GPU_F_RESOURCE_BLOB特性)

  // 更新scanout
  virtio_gpu_update_scanout(vdev, scanout_id, fb, res, r);
  return true;
}

void virtio_gpu_update_scanout(VirtIODevice *vdev, uint32_t scanout_id,
                               GPUFrameBuffer *fb, GPUSimpleResource *res,
                               struct virtio_gpu_rect *r) {
  GPUDev *gdev = vdev->dev;
  GPUSimpleResource *origin_res = NULL;
  GPUScanout *scanout = NULL;

  scanout = &gdev->scanouts[scanout_id];
  origin_res = virtio_gpu_find_resource(gdev, scanout->resource_id);
  if (origin_res) {
    // 解除原来绑定的resource
    origin_res->scanout_bitmask &= ~(1 << scanout_id);
  }

  // 绑定新的resource
  res->scanout_bitmask |= (1 << scanout_id);
  log_debug("%s updated scanout %d to resource %d", __func__, scanout_id,
            res->resource_id);
  // 更新scanout参数和framebuffer
  scanout->resource_id = res->resource_id;
  scanout->x = r->x;
  scanout->y = r->y;
  scanout->width = r->width;
  scanout->height = r->height;
  scanout->frame_buffer = *fb;
}

void virtio_gpu_transfer_to_host_2d(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);

  GPUDev *gdev = vdev->dev;

  GPUSimpleResource *res = NULL;
  uint32_t src_offset = 0;
  uint32_t dst_offset = 0;
  struct virtio_gpu_transfer_to_host_2d transfer_2d;

  VIRTIO_GPU_FILL_CMD(gcmd->resp_iov, gcmd->resp_iov_cnt, transfer_2d);

  res = virtio_gpu_check_resource(vdev, transfer_2d.resource_id, __func__,
                                  &gcmd->error);
  if (!res) {
    return;
  }

  if (transfer_2d.r.x > res->width || transfer_2d.r.y > res->height ||
      transfer_2d.r.width > res->width || transfer_2d.r.height > res->height ||
      transfer_2d.r.x + transfer_2d.r.width > res->width ||
      transfer_2d.r.y + transfer_2d.r.height > res->height) {
    log_error("%s trying to transfer bounds outside resource %d bounds, (%d, "
              "%d) + %d, %d vs %d, %d",
              __func__, res->resource_id, transfer_2d.r.x, transfer_2d.r.y,
              transfer_2d.r.width, transfer_2d.r.height, res->width,
              res->height);
    gcmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    return;
  }

  log_debug("%s transfering a region (%d, %d) + %d, %d from resource %d %d, %d",
            __func__, transfer_2d.r.x, transfer_2d.r.y, transfer_2d.r.width,
            transfer_2d.r.height, res->resource_id, res->width, res->height);

  // 保留transfer的信息，到flush时再真正拷贝
  res->transfer_rect.x = transfer_2d.r.x;
  res->transfer_rect.y = transfer_2d.r.y;
  res->transfer_rect.width = transfer_2d.r.width;
  res->transfer_rect.height = transfer_2d.r.height;
  res->transfer_offset = transfer_2d.offset;
}

void virtio_gpu_resource_attach_backing(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);

  GPUSimpleResource *res = NULL;
  struct virtio_gpu_resource_attach_backing attach_backing;
  GPUDev *gdev = vdev->dev;

  VIRTIO_GPU_FILL_CMD(gcmd->resp_iov, gcmd->resp_iov_cnt, attach_backing);

  // 检查需要绑定的resource是否已经注册
  res = virtio_gpu_find_resource(gdev, attach_backing.resource_id);
  if (!res) {
    log_error("%s cannot find resource with id %d", __func__,
              attach_backing.resource_id);
    gcmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    return;
  }

  if (res->iov) {
    log_error("%s found resource %d already has iov", __func__,
              attach_backing.resource_id);
    gcmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    return;
  }

  log_debug("attaching guest mem to resource %d of gpu dev from zone %d",
            res->resource_id, vdev->zone_id);

  int err = virtio_gpu_create_mapping_iov(vdev, attach_backing.nr_entries,
                                          sizeof(attach_backing), gcmd,
                                          &res->iov, &res->iov_cnt);
  if (err != 0) {
    log_error("%s failed to map guest memory to iov", __func__);
    gcmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    return;
  }
}

int virtio_gpu_create_mapping_iov(VirtIODevice *vdev, uint32_t nr_entries,
                                  uint32_t offset,
                                  GPUCommand *gcmd, /*uint64_t **addr,*/
                                  struct iovec **iov, uint32_t *niov) {
  log_debug("entering %s", __func__);
  GPUDev *gdev = vdev->dev;

  struct virtio_gpu_mem_entry *entries = NULL; // 存储所有guest内存入口的数组
  size_t entries_size = 0;
  int e = 0;
  int v = 0;

  if (nr_entries > 16384) {
    log_error(
        "%s found number of entries %d is too big (need to less than 16384)",
        __func__, nr_entries);
    return -1;
  }

  // 分配内存，并且将guest mem用iov管理
  // guest内存到host内存需要一层转换
  entries_size = sizeof(*entries) * nr_entries;
  entries = malloc(entries_size);
  log_debug("%s got %d entries with total size %d", __func__, nr_entries,
            entries_size);
  // 先将请求中附带的所有内存入口拷贝到entries
  size_t s = iov_to_buf(gcmd->resp_iov, gcmd->resp_iov_cnt, offset, entries,
                        entries_size);
  if (s != entries_size) {
    log_error("%s failed to copy memory entries to buffer", __func__);
    free(entries);
    return -1;
  }

  *iov = NULL;
  // if (addr) {
  //   *addr = NULL;
  // }

  for (e = 0, v = 0; e < nr_entries; ++e, ++v) {
    uint64_t e_addr = entries[e].addr;     // guest内存块的起始位置
    uint32_t e_length = entries[e].length; // guest内存块的长度

    // 由于zonex的全部内存会映射到zone0
    // 而zone start时会将zonex的所有内存映射到hvisor-tool的虚拟内存空间
    // 因此这里只需要将zonex用来存储资源数据的内存块的虚拟地址用iov管理即可

    // iov以16个为一组进行分配，如果不够则重新分配内存
    if (!(v % 16)) {
      struct iovec *temp = realloc(*iov, (v + 16) * sizeof(struct iovec));
      if (temp == NULL) {
        // 无法分配
        log_error("%s cannot alloc enough memory for iov", __func__);
        free(*iov); // 直接释放iov数组
        free(entries);
        *iov = NULL;
        return -1;
      }
      *iov = temp;
      // if (addr) {
      //   *addr = realloc(*addr, (v * 16) * sizeof(uint64_t));
      // }
    }

    (*iov)[v].iov_base = get_virt_addr((void *)e_addr, vdev->zone_id);
    (*iov)[v].iov_len = e_length;
    log_debug("guest addr %x map to %x with size %d", e_addr,
              (*iov)[v].iov_base, (*iov)[v].iov_len);
    // if (addr) {
    //   (*addr)[v] = e_addr;
    // }

    // 考虑到后期更改时，也许zonex到zone0的映射并不是直接的，而是通过dma等方式重新分配
    // 因此保留e、v来应对entries和iov不一一对应的情况
  }
  *niov = v;
  log_debug("%d memory blocks mapped", *niov);

  // 释放entries
  free(entries);

  return 0;
}

void virtio_gpu_resource_detach_backing(VirtIODevice *vdev, GPUCommand *gcmd) {
  log_debug("entering %s", __func__);
}

void virtio_gpu_simple_process_cmd(struct iovec *iov, unsigned int iov_cnt,
                                   uint16_t resp_idx, VirtIODevice *vdev) {
  log_debug("------ entering %s ------", __func__);

  GPUCommand gcmd;
  gcmd.resp_iov = iov;
  gcmd.resp_iov_cnt = iov_cnt;
  gcmd.resp_idx = resp_idx;
  gcmd.error = 0;
  gcmd.finished = false;

  // 先填充每个请求都有的cmd_hdr
  VIRTIO_GPU_FILL_CMD(iov, iov_cnt, gcmd.control_header);

  // 根据cmd_hdr的类型跳转到对应的处理函数
  /**********************************
   * 一般的2D渲染调用链是get_display_info->resource_create_2d->resource_attach_backing->set_scanout->get_display_info(确定是否设置成功)
   * ->transfer_to_host_2d->resource_flush->*重复transfer和flush*->结束
   */
  switch (gcmd.control_header.type) {
  case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
    virtio_gpu_get_display_info(vdev, &gcmd);
    break;
  case VIRTIO_GPU_CMD_GET_EDID:
    virtio_gpu_get_edid(vdev, &gcmd);
    break;
  case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
    virtio_gpu_resource_create_2d(vdev, &gcmd);
    break;
  case VIRTIO_GPU_CMD_RESOURCE_UNREF:
    virtio_gpu_resource_unref(vdev, &gcmd);
    break;
  case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
    virtio_gpu_resource_flush(vdev, &gcmd);
    break;
  case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
    virtio_gpu_transfer_to_host_2d(vdev, &gcmd);
    break;
  case VIRTIO_GPU_CMD_SET_SCANOUT:
    virtio_gpu_set_scanout(vdev, &gcmd);
    break;
  case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
    virtio_gpu_resource_attach_backing(vdev, &gcmd);
    break;
  case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
    virtio_gpu_resource_detach_backing(vdev, &gcmd);
    break;
  default:
    log_error("unknown request type");
    gcmd.error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    break;
  }

  if (!gcmd.finished) {
    // 如果没有直接返回有data的响应，那么检查是否产生错误并返回无data响应
    if (gcmd.error) {
      log_error("failed to handle virtio gpu request from zone %d, and request "
                "type is %d, error type is %d",
                vdev->zone_id, gcmd.control_header.type, gcmd.error);
    }
    virtio_gpu_ctrl_response_nodata(
        vdev, &gcmd, gcmd.error ? gcmd.error : VIRTIO_GPU_RESP_OK_NODATA);
  }

  // 处理完毕，不需要iov
  free(gcmd.resp_iov);

  log_debug("------ leaving %s ------", __func__);
}