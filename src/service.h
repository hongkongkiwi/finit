/* Finit service monitor, task starter and generic API for managing svc_t
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2022  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef FINIT_SERVICE_H_
#define FINIT_SERVICE_H_

#include "svc.h"

void	  service_runlevel	 (int newlevel);
int	  service_register	 (int type, char *line, struct rlimit rlimit[], char *file);
void      service_unregister     (svc_t *svc);

void      service_runtask_clean  (void);
void      service_reload_dynamic (void);
void      service_update_rdeps   (void);

int       service_timeout_after  (svc_t *svc, int timeout, void (*cb)(svc_t *svc));
int       service_timeout_cancel (svc_t *svc);

void      service_forked         (svc_t *svc);

int       service_stop           (svc_t *svc);
int       service_step           (svc_t *svc);
void      service_step_all       (int types);
void      service_worker         (void *unused);

int       service_completed      (void);

void      service_init           (void);

#endif	/* FINIT_SERVICE_H_ */

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
