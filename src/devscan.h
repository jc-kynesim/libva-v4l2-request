#ifndef _DEVSCAN_H_
#define _DEVSCAN_H_

#include <va/va_backend.h>

struct devscan;
struct decdev;

const char *decdev_media_path(const struct decdev *const dev);
const char *decdev_video_path(const struct decdev *const dev);
const struct decdev *devscan_find(struct devscan *const scan,
				  const uint32_t src_fmt_v4l2);

VAStatus devscan_build(const VADriverContextP dc, struct devscan **pscan);
void devscan_delete(struct devscan *const scan);

#endif
