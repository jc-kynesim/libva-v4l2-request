/*
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _UTILS_H_
#define _UTILS_H_

#include <stdarg.h>
#include "va/va_backend.h"

#define STRING(x) _STRING2(x)
#define _STRING2(x) #x

#define request_log(...)\
	_request_log(__FILE__ ":" STRING(__LINE__) ": " __VA_ARGS__)
#define request_info(_dc, ...)\
	_request_info(_dc, __VA_ARGS__)
#define request_err(_dc, ...)\
	_request_err(_dc, __VA_ARGS__)

/* Just stderr - just use for debug */
void _request_log(const char *format, ...);
/* VA reporting channels */
void _request_err(const VADriverContextP dc, const char *fmt, ...);
void _request_info(const VADriverContextP dc, const char *fmt, ...);

#endif
