// SPDX-License-Identifier: GPL-2.0-only
/*
 * mycam004m-fake: standalone V4L2 capture test driver.
 *
 * NOT part of the real mycam004m driver, and not loaded on a real board.
 * This registers MYCAM004M_NUM_CAMS independent /dev/videoX capture
 * devices, each replaying a single static YUYV reference frame (loaded
 * via the firmware loader, see tools/gen_fake_frames.py) at
 * MYCAM004M_FPS. It exists so a calling application's V4L2 capture code
 * (open, format negotiation, REQBUFS/QBUF/STREAMON/DQBUF) can be
 * exercised against something that delivers real frame buffers, without
 * needing MY-CAM004M hardware or its still-unwritten register tables
 * (see mycam004m-regs.h).
 *
 * There is no I2C, GPIO, devicetree, or media-controller-routing code
 * in this file -- it is a plain vb2 capture device with no upstream
 * pipeline, modeled on drivers/media/test-drivers/vimc/vimc-capture.c's
 * vb2 plumbing (queue/ops/fops/ioctl_ops wiring) but self-driven by a
 * kthread instead of pulling from an upstream media entity, since
 * there's no upstream entity here to pull from -- closer to how
 * drivers/media/test-drivers/vivid's capture kthread works.
 *
 * Video device naming is deliberately "<name> context <N>" (N = 0..3),
 * matching the exact suffix TI's j721e-csi2rx.c uses for its 4 real
 * per-VC capture nodes (see ti_csi2rx_video_register(), which does
 * snprintf(vdev->name, ..., "%s context %u", dev_name(csi->dev),
 * ctx->idx)). scripts/select-camera-backend.sh matches on that "context
 * N" suffix alone, so the exact same resolution logic finds either this
 * fake driver's nodes or the real ti-csi2rx nodes -- whichever is
 * loaded -- and symlinks them to the same /dev/mycam/camN paths.
 */

#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "mycam004m.h"

#define MYCAM004M_FAKE_NAME	"mycam004m-fake"

/* Same fixed-format philosophy as the real driver's mycam004m_set_fmt():
 * there's exactly one target mode, so *_fmt just coerces to it rather
 * than negotiating. Keeps behavior identical from the calling app's
 * perspective whether it's talking to this or the real capture node.
 */
static const struct v4l2_pix_format mycam004m_fake_pixfmt = {
	.width		= MYCAM004M_WIDTH,
	.height		= MYCAM004M_HEIGHT,
	.pixelformat	= V4L2_PIX_FMT_YUYV,
	.field		= V4L2_FIELD_NONE,
	.bytesperline	= MYCAM004M_WIDTH * 2,
	.sizeimage	= MYCAM004M_WIDTH * 2 * MYCAM004M_HEIGHT,
	.colorspace	= V4L2_COLORSPACE_SMPTE170M,
};

struct mycam004m_fake_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct mycam004m_fake_cam {
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex lock;		/* serializes ioctls/vb2 queue ops */
	spinlock_t qlock;		/* protects buf_list */
	struct list_head buf_list;
	struct task_struct *kthread;
	u32 sequence;
	unsigned int index;		/* 0..MYCAM004M_NUM_CAMS-1 */

	const struct firmware *fw;	/* pinned for module lifetime */
};

static struct platform_device *mycam004m_fake_pdev;
static struct v4l2_device mycam004m_fake_v4l2_dev;
static struct mycam004m_fake_cam mycam004m_fake_cams[MYCAM004M_NUM_CAMS];
static unsigned int mycam004m_fake_registered;	/* how many came up, for rollback */

/* ---- frame delivery ---------------------------------------------- */

static void mycam004m_fake_fill_next_buffer(struct mycam004m_fake_cam *cam)
{
	struct mycam004m_fake_buffer *buf;
	void *vaddr;

	spin_lock(&cam->qlock);
	buf = list_first_entry_or_null(&cam->buf_list,
					struct mycam004m_fake_buffer, list);
	if (buf)
		list_del(&buf->list);
	spin_unlock(&cam->qlock);

	/* Nothing queued -- app isn't keeping up, or hasn't queued yet.
	 * Just skip this tick; there's no hardware FIFO to overflow.
	 */
	if (!buf)
		return;

	vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	memcpy(vaddr, cam->fw->data, cam->fw->size);

	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence = cam->sequence++;
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, cam->fw->size);
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static int mycam004m_fake_thread(void *data)
{
	struct mycam004m_fake_cam *cam = data;

	/* Not a real-time frame clock -- +/- a few ms of jitter around
	 * 1/MYCAM004M_FPS doesn't matter for a static-reference-image test
	 * aid, so plain jiffies sleep is enough (no hrtimer needed).
	 */
	while (!kthread_should_stop()) {
		mycam004m_fake_fill_next_buffer(cam);
		schedule_timeout_interruptible(msecs_to_jiffies(1000 / MYCAM004M_FPS));
	}

	return 0;
}

/* ---- vb2 ops -------------------------------------------------------- */

static void mycam004m_fake_return_all_buffers(struct mycam004m_fake_cam *cam,
					       enum vb2_buffer_state state)
{
	struct mycam004m_fake_buffer *buf, *tmp;

	spin_lock(&cam->qlock);
	list_for_each_entry_safe(buf, tmp, &cam->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock(&cam->qlock);
}

static int mycam004m_fake_queue_setup(struct vb2_queue *vq,
				       unsigned int *nbuffers,
				       unsigned int *nplanes,
				       unsigned int sizes[],
				       struct device *alloc_devs[])
{
	if (*nplanes)
		return sizes[0] < mycam004m_fake_pixfmt.sizeimage ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = mycam004m_fake_pixfmt.sizeimage;
	return 0;
}

static int mycam004m_fake_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < mycam004m_fake_pixfmt.sizeimage)
		return -EINVAL;
	return 0;
}

static void mycam004m_fake_buf_queue(struct vb2_buffer *vb2_buf)
{
	struct mycam004m_fake_cam *cam = vb2_get_drv_priv(vb2_buf->vb2_queue);
	struct mycam004m_fake_buffer *buf =
		container_of(vb2_buf, struct mycam004m_fake_buffer, vb.vb2_buf);

	spin_lock(&cam->qlock);
	list_add_tail(&buf->list, &cam->buf_list);
	spin_unlock(&cam->qlock);
}

static int mycam004m_fake_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mycam004m_fake_cam *cam = vb2_get_drv_priv(vq);

	cam->sequence = 0;
	cam->kthread = kthread_run(mycam004m_fake_thread, cam,
				    "mycam004m-fake/%u", cam->index);
	if (IS_ERR(cam->kthread)) {
		int ret = PTR_ERR(cam->kthread);

		cam->kthread = NULL;
		mycam004m_fake_return_all_buffers(cam, VB2_BUF_STATE_QUEUED);
		return ret;
	}

	return 0;
}

static void mycam004m_fake_stop_streaming(struct vb2_queue *vq)
{
	struct mycam004m_fake_cam *cam = vb2_get_drv_priv(vq);

	if (cam->kthread) {
		kthread_stop(cam->kthread);
		cam->kthread = NULL;
	}

	mycam004m_fake_return_all_buffers(cam, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops mycam004m_fake_qops = {
	.queue_setup		= mycam004m_fake_queue_setup,
	.buf_prepare		= mycam004m_fake_buf_prepare,
	.buf_queue		= mycam004m_fake_buf_queue,
	.start_streaming	= mycam004m_fake_start_streaming,
	.stop_streaming		= mycam004m_fake_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

/* ---- V4L2 ioctls ------------------------------------------------- */

static int mycam004m_fake_querycap(struct file *file, void *priv,
				    struct v4l2_capability *cap)
{
	struct mycam004m_fake_cam *cam = video_drvdata(file);

	strscpy(cap->driver, MYCAM004M_FAKE_NAME, sizeof(cap->driver));
	strscpy(cap->card, cam->vdev.name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 MYCAM004M_FAKE_NAME);
	return 0;
}

static int mycam004m_fake_g_fmt_vid_cap(struct file *file, void *priv,
					 struct v4l2_format *f)
{
	f->fmt.pix = mycam004m_fake_pixfmt;
	return 0;
}

/* Fixed format only -- see mycam004m_fake_pixfmt's comment. Same
 * coerce-not-negotiate behavior as mycam004m_set_fmt() in the real
 * driver: whatever the caller asks for, they get told what they'll
 * actually receive.
 */
static int mycam004m_fake_try_fmt_vid_cap(struct file *file, void *priv,
					   struct v4l2_format *f)
{
	f->fmt.pix = mycam004m_fake_pixfmt;
	return 0;
}

static int mycam004m_fake_s_fmt_vid_cap(struct file *file, void *priv,
					 struct v4l2_format *f)
{
	struct mycam004m_fake_cam *cam = video_drvdata(file);

	if (vb2_is_busy(&cam->queue))
		return -EBUSY;

	f->fmt.pix = mycam004m_fake_pixfmt;
	return 0;
}

static int mycam004m_fake_enum_fmt_vid_cap(struct file *file, void *priv,
					    struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int mycam004m_fake_enum_framesizes(struct file *file, void *priv,
					   struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index > 0 || fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = MYCAM004M_WIDTH;
	fsize->discrete.height = MYCAM004M_HEIGHT;
	return 0;
}

static const struct v4l2_file_operations mycam004m_fake_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= vb2_fop_mmap,
};

static const struct v4l2_ioctl_ops mycam004m_fake_ioctl_ops = {
	.vidioc_querycap		= mycam004m_fake_querycap,
	.vidioc_g_fmt_vid_cap		= mycam004m_fake_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= mycam004m_fake_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= mycam004m_fake_try_fmt_vid_cap,
	.vidioc_enum_fmt_vid_cap	= mycam004m_fake_enum_fmt_vid_cap,
	.vidioc_enum_framesizes		= mycam004m_fake_enum_framesizes,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
};

/* ---- setup / teardown --------------------------------------------- */

static void mycam004m_fake_cam_remove(struct mycam004m_fake_cam *cam)
{
	vb2_video_unregister_device(&cam->vdev);
	mutex_destroy(&cam->lock);
	if (cam->fw)
		release_firmware(cam->fw);
}

static int mycam004m_fake_cam_init(struct mycam004m_fake_cam *cam, unsigned int index)
{
	struct vb2_queue *q = &cam->queue;
	char fw_name[32];
	int ret;

	cam->index = index;
	mutex_init(&cam->lock);
	spin_lock_init(&cam->qlock);
	INIT_LIST_HEAD(&cam->buf_list);

	snprintf(fw_name, sizeof(fw_name), "mycam004m-fake/cam%u.bin", index + 1);
	ret = request_firmware(&cam->fw, fw_name, &mycam004m_fake_pdev->dev);
	if (ret) {
		dev_err(&mycam004m_fake_pdev->dev,
			"firmware '%s' not found (%d) -- generate it with "
			"tools/gen_fake_frames.py and install to "
			"/lib/firmware/mycam004m-fake/, see README.md\n",
			fw_name, ret);
		goto err_mutex;
	}
	if (cam->fw->size != mycam004m_fake_pixfmt.sizeimage) {
		dev_err(&mycam004m_fake_pdev->dev,
			"firmware '%s' is %zu bytes, expected %u (%ux%u YUYV)\n",
			fw_name, cam->fw->size, mycam004m_fake_pixfmt.sizeimage,
			MYCAM004M_WIDTH, MYCAM004M_HEIGHT);
		ret = -EINVAL;
		goto err_fw;
	}

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = cam;
	q->buf_struct_size = sizeof(struct mycam004m_fake_buffer);
	q->ops = &mycam004m_fake_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_reqbufs_allocation = 2;
	q->lock = &cam->lock;
	q->dev = &mycam004m_fake_pdev->dev;

	ret = vb2_queue_init(q);
	if (ret) {
		dev_err(&mycam004m_fake_pdev->dev,
			"cam%u: vb2 queue init failed (%d)\n", index + 1, ret);
		goto err_fw;
	}

	snprintf(cam->vdev.name, sizeof(cam->vdev.name), "%s context %u",
		 MYCAM004M_FAKE_NAME, index);
	cam->vdev.fops = &mycam004m_fake_fops;
	cam->vdev.ioctl_ops = &mycam004m_fake_ioctl_ops;
	cam->vdev.release = video_device_release_empty;
	cam->vdev.lock = &cam->lock;
	cam->vdev.queue = q;
	cam->vdev.v4l2_dev = &mycam004m_fake_v4l2_dev;
	cam->vdev.vfl_dir = VFL_DIR_RX;
	cam->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
				V4L2_CAP_READWRITE;
	video_set_drvdata(&cam->vdev, cam);

	ret = video_register_device(&cam->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&mycam004m_fake_pdev->dev,
			"cam%u: video_register_device failed (%d)\n", index + 1, ret);
		goto err_fw;
	}

	dev_info(&mycam004m_fake_pdev->dev, "cam%u: %s ready as %s (from %s)\n",
		 index + 1, cam->vdev.name, video_device_node_name(&cam->vdev),
		 fw_name);
	return 0;

err_fw:
	release_firmware(cam->fw);
	cam->fw = NULL;
err_mutex:
	mutex_destroy(&cam->lock);
	return ret;
}

static int __init mycam004m_fake_init(void)
{
	unsigned int i;
	int ret;

	mycam004m_fake_pdev = platform_device_register_simple(MYCAM004M_FAKE_NAME,
								-1, NULL, 0);
	if (IS_ERR(mycam004m_fake_pdev))
		return PTR_ERR(mycam004m_fake_pdev);

	/* v4l2_device_register() falls back to "%s %s", dev->driver->name,
	 * dev_name(dev) when v4l2_dev->name[0] is empty -- but this device
	 * has no bound driver (platform_device_register_simple() doesn't
	 * register a matching platform_driver), so dev->driver is NULL and
	 * that fallback NULL-derefs. Set the name explicitly so it's never
	 * reached. (Found by actually loading this on real hardware --
	 * W=1 compiles clean either way.)
	 */
	strscpy(mycam004m_fake_v4l2_dev.name, MYCAM004M_FAKE_NAME,
		sizeof(mycam004m_fake_v4l2_dev.name));
	ret = v4l2_device_register(&mycam004m_fake_pdev->dev, &mycam004m_fake_v4l2_dev);
	if (ret)
		goto err_pdev;

	for (i = 0; i < MYCAM004M_NUM_CAMS; i++) {
		ret = mycam004m_fake_cam_init(&mycam004m_fake_cams[i], i);
		if (ret)
			goto err_cams;
		mycam004m_fake_registered++;
	}

	return 0;

err_cams:
	while (mycam004m_fake_registered--)
		mycam004m_fake_cam_remove(&mycam004m_fake_cams[mycam004m_fake_registered]);
	v4l2_device_unregister(&mycam004m_fake_v4l2_dev);
err_pdev:
	platform_device_unregister(mycam004m_fake_pdev);
	return ret;
}

static void __exit mycam004m_fake_exit(void)
{
	while (mycam004m_fake_registered--)
		mycam004m_fake_cam_remove(&mycam004m_fake_cams[mycam004m_fake_registered]);
	v4l2_device_unregister(&mycam004m_fake_v4l2_dev);
	platform_device_unregister(mycam004m_fake_pdev);
}

module_init(mycam004m_fake_init);
module_exit(mycam004m_fake_exit);

MODULE_DESCRIPTION("mycam004m fake capture test driver (static reference images)");
MODULE_AUTHOR("Josh Ellis");
MODULE_LICENSE("GPL");
