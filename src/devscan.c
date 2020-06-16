#include <errno.h>
#include <fcntl.h>
#include <h264-ctrls.h>
#include <hevc-ctrls.h>
#include <libudev.h>
#include <mpeg2-ctrls.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/media.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <va/va_backend.h>

#include "devscan.h"
#include "utils.h"
#include "v4l2.h"


struct decdev {
	uint32_t src_fmt_v4l2;
	const char * vname;
	const char * mname;
};

struct devscan {
	struct decdev env;
	unsigned int dev_size;
	unsigned int dev_count;
	struct decdev *devs;
};

static VAStatus devscan_add(struct devscan *const scan,
			    uint32_t src_fmt_v4l2,
			    const char * vname,
			    const char * mname)
{
	struct decdev *d;

	if (scan->dev_size <= scan->dev_count) {
		unsigned int n = !scan->dev_size ? 4 : scan->dev_size * 2;
		d = realloc(scan->devs, n * sizeof(*d));
		if (!d)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		scan->devs = d;
		scan->dev_size = n;
	}

	d = scan->devs + scan->dev_count;
	d->vname = strdup(vname);
	if (!d->vname)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	d->mname = strdup(mname);
	if (!d->mname) {
		free((char *)d->vname);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	d->src_fmt_v4l2 = src_fmt_v4l2;
	++scan->dev_count;
	return VA_STATUS_SUCCESS;
}

void devscan_delete(struct devscan *const scan)
{
	unsigned int i;
	if (!scan)
		return;
	for (i = 0; i < scan->dev_count; ++i) {
		free((char*)scan->devs[i].mname);
		free((char*)scan->devs[i].vname);
	}
	free(scan->devs);
	free(scan);
}

static bool decode_format_supported(uint32_t pixfmt)
{
	return pixfmt == V4L2_PIX_FMT_H264_SLICE_RAW ||
		pixfmt == V4L2_PIX_FMT_HEVC_SLICE ||
		pixfmt == V4L2_PIX_FMT_MPEG2_SLICE;
}

#define REQ_BUF_CAPS (\
	V4L2_BUF_CAP_SUPPORTS_DMABUF |\
	V4L2_BUF_CAP_SUPPORTS_REQUESTS |\
	V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF)

static void probe_formats(const VADriverContextP dc,
			  struct devscan *const scan,
			  const int fd,
			  const unsigned int type_v4l2,
			  const char *const mpath,
			  const char *const vpath)
{
	unsigned int i;
	for (i = 0;; ++i) {
		struct v4l2_fmtdesc fmtdesc = {
			.index = i,
			.type = type_v4l2
		};
		struct v4l2_requestbuffers rbufs = {
			.count = 0,
			.type = type_v4l2,
			.memory = V4L2_MEMORY_MMAP
		};
		while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
			if (errno != EINTR)
				return;
		}
		if (!decode_format_supported(fmtdesc.pixelformat))
			continue;

		if (v4l2_set_format(fd, type_v4l2, fmtdesc.pixelformat, 720, 480))
			continue;

		while (ioctl(fd, VIDIOC_REQBUFS, &rbufs)) {
			if (errno != EINTR) {
				request_info(dc, "%s: Reqbufs failed\n", vpath);
				continue;
			}
		}

		if ((rbufs.capabilities & REQ_BUF_CAPS) != REQ_BUF_CAPS) {
			request_info(dc, "%s: Buf caps %#x insufficient\n", vpath);
			continue;
		}

		request_info(dc, "Adding: %s,%s pix=%#x, type=%d\n",
			     mpath, vpath, fmtdesc.pixelformat, type_v4l2);
		devscan_add(scan, fmtdesc.pixelformat, vpath, mpath);
	}
}


static VAStatus probe_video_device(const VADriverContextP dc,
				   struct udev_device *const device,
				   struct devscan *const scan,
				   const char *const mpath)
{
	VAStatus ret = VA_STATUS_ERROR_OPERATION_FAILED;
	struct v4l2_capability capability = { 0 };
	unsigned int capabilities = 0;
	int video_fd = -1;
	int rv;

	const char *path = udev_device_get_devnode(device);
	if (!path) {
		request_err(dc, "%s: get video device devnode failed\n", __func__);
		goto fail;
	}

	video_fd = open(path, O_RDWR, 0);
	if (video_fd == -1) {
		request_err(dc, "%s: opening %s failed, %s (%d)\n", __func__, path, strerror(errno), errno);
		goto fail;
	}

	rv = ioctl(video_fd, VIDIOC_QUERYCAP, &capability);
	if (rv < 0) {
		request_err(dc, "%s: get video capability failed, %s (%d)\n", __func__, strerror(errno), errno);
		goto fail;
	}

	if (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
		capabilities = capability.device_caps;
	else
		capabilities = capability.capabilities;

	request_info(dc, "%s: path=%s capabilities=%#x\n", __func__, path, capabilities);

	if (!(capabilities & V4L2_CAP_STREAMING)) {
		request_info(dc, "%s: missing required streaming capability\n", __func__);
		goto fail;
	}

	if (!(capabilities & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_M2M))) {
		request_info(dc, "%s: missing required mem2mem capability\n", __func__);
		goto fail;
	}

	/* Should check capture formats too... */
	if ((capabilities & V4L2_CAP_VIDEO_M2M) != 0)
		probe_formats(dc, scan, video_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, mpath, path);
	if ((capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) != 0)
		probe_formats(dc, scan, video_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, mpath, path);

	close(video_fd);
	return VA_STATUS_SUCCESS;

fail:
	if (video_fd >= 0)
		close(video_fd);
	return ret;
}

static VAStatus probe_media_device(const VADriverContextP dc,
				   struct udev_device *const device,
				   struct devscan *const scan)
{
	VAStatus ret = VA_STATUS_ERROR_OPERATION_FAILED;
	int rv;
	struct media_device_info device_info = { 0 };
	struct media_v2_topology topology = { 0 };
	struct media_v2_interface *interfaces = NULL;
	struct udev *udev = udev_device_get_udev(device);
	struct udev_device *video_device;
	dev_t devnum;
	int media_fd = -1;

	const char *path = udev_device_get_devnode(device);
	if (!path) {
		request_err(dc, "%s: get media device devnode failed\n", __func__);
		goto fail;
	}

	media_fd = open(path, O_RDWR, 0);
	if (media_fd < 0) {
		request_err(dc, "%s: opening %s failed, %s (%d)\n", __func__, path, strerror(errno), errno);
		goto fail;
	}

	rv = ioctl(media_fd, MEDIA_IOC_DEVICE_INFO, &device_info);
	if (rv < 0) {
		request_err(dc, "%s: get media device info failed, %s (%d)\n", __func__, strerror(errno), errno);
		goto fail;
	}

	rv = ioctl(media_fd, MEDIA_IOC_G_TOPOLOGY, &topology);
	if (rv < 0) {
		request_err(dc, "%s: get media topology failed, %s (%d)\n", __func__, strerror(errno), errno);
		goto fail;
	}

	if (topology.num_interfaces <= 0) {
		request_err(dc, "%s: media device has no interfaces\n", __func__);
		goto fail;
	}

	interfaces = calloc(topology.num_interfaces, sizeof(*interfaces));
	if (!interfaces) {
		request_err(dc, "%s: allocating media interface struct failed\n", __func__);
		ret = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto fail;
	}

	topology.ptr_interfaces = (__u64)(uintptr_t)interfaces;
	rv = ioctl(media_fd, MEDIA_IOC_G_TOPOLOGY, &topology);
	if (rv < 0) {
		request_err(dc, "%s: get media topology failed, %s (%d)\n", __func__, strerror(errno), errno);
		goto fail;
	}

	for (int i = 0; i < topology.num_interfaces; i++) {
		if (interfaces[i].intf_type != MEDIA_INTF_T_V4L_VIDEO)
			continue;

		devnum = makedev(interfaces[i].devnode.major, interfaces[i].devnode.minor);
		video_device = udev_device_new_from_devnum(udev, 'c', devnum);
		if (!video_device) {
			request_err(dc, "%s: video_device[%d]=%p\n", __func__, i, video_device);
			continue;
		}

		ret = probe_video_device(dc, video_device, scan, path);
		udev_device_unref(video_device);

		if (ret != VA_STATUS_SUCCESS)
			goto fail;
	}

	free(interfaces);
	return ret;

fail:
	free(interfaces);
	if (media_fd != -1)
		close(media_fd);
	return ret;
}

const char *decdev_media_path(const struct decdev *const dev)
{
	return !dev ? NULL : dev->mname;
}

const char *decdev_video_path(const struct decdev *const dev)
{
	return !dev ? NULL : dev->vname;
}

const struct decdev *devscan_find(struct devscan *const scan,
				  const uint32_t src_fmt_v4l2)
{
	unsigned int i;

	if (scan->env.mname && scan->env.vname)
		return &scan->env;

	if (!src_fmt_v4l2)
		return scan->dev_count ? scan->devs + 0 : NULL;

	for (i = 0; i != scan->dev_count; ++i) {
		if (scan->devs[i].src_fmt_v4l2 == src_fmt_v4l2)
			return scan->devs + i;
	}
	return NULL;
}

VAStatus devscan_build(const VADriverContextP dc, struct devscan **pscan)
{
	VAStatus ret = VA_STATUS_ERROR_OPERATION_FAILED;
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices;
	struct udev_list_entry *entry;
	struct udev_device *device;
	struct devscan * scan;

	*pscan = NULL;

	scan = calloc(1, sizeof(*scan));
	if (!scan) {
		ret = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto fail;
	}

	scan->env.mname = getenv("LIBVA_V4L2_REQUEST_MEDIA_PATH");
	scan->env.vname = getenv("LIBVA_V4L2_REQUEST_VIDEO_PATH");
	if (scan->env.mname && scan->env.vname) {
		request_info(dc, "Media/video device env overrides found: %s,%s\n",
			     scan->env.mname, scan->env.vname);
		*pscan = scan;
		return VA_STATUS_SUCCESS;
	}

	udev = udev_new();
	if (!udev) {
		request_err(dc, "%s: allocating udev context failed\n", __func__);
		ret = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto fail;
	}

	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		request_err(dc, "%s: allocating udev enumerator failed\n", __func__);
		ret = VA_STATUS_ERROR_ALLOCATION_FAILED;
		goto fail;
	}

	udev_enumerate_add_match_subsystem(enumerate, "media");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);
	udev_list_entry_foreach(entry, devices) {
		const char *path = udev_list_entry_get_name(entry);
		if (!path)
			continue;

		device = udev_device_new_from_syspath(udev, path);
		if (!device)
			continue;

		probe_media_device(dc, device, scan);
		udev_device_unref(device);
	}

	udev_enumerate_unref(enumerate);

	*pscan = scan;
	return VA_STATUS_SUCCESS;

fail:
	udev_unref(udev);
	devscan_delete(scan);
	return ret;
}

