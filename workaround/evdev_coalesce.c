/*
 * LD_PRELOAD shim for Parsec on Linux: fixes stuck gamepad buttons.
 *
 * Bug: libmatoya's mty_evdev_poll() poll()s all evdev fds and then read()s
 * exactly ONE struct input_event per ready fd per app-loop iteration. A
 * device that produces more events per second than Parsec's loop rate
 * (e.g. Steam's virtual pad fed by a Steam Controller's trackpads/gyro)
 * builds a backlog in the kernel's evdev client buffer. When that buffer
 * overflows the kernel throws the queue away and emits SYN_DROPPED, which
 * matoya ignores -> button *release* events are lost -> stuck buttons
 * (and growing input lag before that).
 *
 * Fix: for 24-byte read()s on evdev nodes, drain the whole kernel queue into
 * a private per-fd queue in which
 *   - EV_KEY events are always kept (never dropped),
 *   - EV_ABS events are coalesced: a pending event for the same axis is
 *     updated in place to the newest value,
 *   - EV_SYN / EV_MSC are dropped (matoya ignores them anyway),
 *   - SYN_DROPPED triggers a resync from EVIOCGKEY/EVIOCGABS,
 * and hand matoya one event per read() from that queue, keys first.
 * poll() is wrapped so fds with pending private events report POLLIN.
 *
 * Also: open() of Steam's own virtual pads (28de:11ff, not our proxy) fails
 * with EACCES, which libmatoya skips silently. Parsec then only ever sees the
 * "Parsec Steam Pad Proxy" mirror, so the silenced (grabbed) Steam pad can't
 * show up as a second, dead controller.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define MAX_FD   1024
#define QCAP     512

struct evq {
	int known;          /* 0 = not checked, 1 = evdev, -1 = other */
	int n;
	struct input_event ev[QCAP];
};

static struct evq *q_tab[MAX_FD];
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;

static ssize_t (*real_read)(int, void *, size_t);
static int (*real_poll)(struct pollfd *, nfds_t, int);
static int (*real_close)(int);
static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);

#define PROXY_NAME "Parsec Steam Pad Proxy"

__attribute__((constructor)) static void init(void)
{
	real_open = dlsym(RTLD_NEXT, "open");
	real_open64 = dlsym(RTLD_NEXT, "open64");
	real_read = dlsym(RTLD_NEXT, "read");
	real_poll = dlsym(RTLD_NEXT, "poll");
	real_close = dlsym(RTLD_NEXT, "close");
}

static struct evq *get_q(int fd)
{
	if (fd < 0 || fd >= MAX_FD)
		return NULL;

	struct evq *q = q_tab[fd];
	if (!q) {
		static struct evq pool[64];
		struct stat st;
		int is_evdev = fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 13 &&
			minor(st.st_rdev) >= 64;
		if (!is_evdev)
			return NULL;   /* not cached: fd may become evdev later */
		for (int i = 0; i < 64 && !q; i++)
			if (pool[i].known == 0)
				q = &pool[i];
		if (!q)
			return NULL;
		memset(q, 0, sizeof(*q));
		q->known = 1;
		q_tab[fd] = q;
	}
	return q->known == 1 ? q : NULL;
}

static int is_discrete(const struct input_event *e)
{
	/* buttons and d-pad hats must never be coalesced: a tap is press+release */
	return e->type == EV_KEY || (e->type == EV_ABS && e->code >= ABS_HAT0X && e->code <= ABS_HAT3Y);
}

static void push(struct evq *q, const struct input_event *e)
{
	if (is_discrete(e)) {
		/* append below */
	} else if (e->type == EV_ABS) {
		for (int i = 0; i < q->n; i++) {
			if (q->ev[i].type == EV_ABS && q->ev[i].code == e->code) {
				q->ev[i] = *e;
				return;
			}
		}
	} else if (e->type != EV_KEY) {
		return;
	}

	if (q->n < QCAP) {
		q->ev[q->n++] = *e;
	} else if (is_discrete(e)) {
		/* full (extremely unlikely): drop oldest analog ABS to keep the key */
		for (int i = 0; i < q->n; i++) {
			if (!is_discrete(&q->ev[i])) {
				memmove(&q->ev[i], &q->ev[i + 1], (q->n - i - 1) * sizeof(q->ev[0]));
				q->ev[q->n - 1] = *e;
				return;
			}
		}
	}
}

static void resync(int fd, struct evq *q)
{
	/* Kernel dropped events: replace the queue with the true current state. */
	unsigned char keys[KEY_MAX / 8 + 1] = {0};
	unsigned long absbits[ABS_MAX / (8 * sizeof(long)) + 1] = {0};
	struct input_event e = {0};

	q->n = 0;

	if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
		for (int code = BTN_MISC; code < KEY_MAX; code++) {
			e.type = EV_KEY;
			e.code = code;
			e.value = (keys[code / 8] >> (code % 8)) & 1;
			/* only buttons the gamepad layer cares about */
			if (code >= BTN_MISC && code < BTN_DIGI)
				push(q, &e);
		}
	}
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
		for (int code = 0; code < ABS_CNT; code++) {
			if (!(absbits[code / (8 * sizeof(long))] & (1UL << (code % (8 * sizeof(long))))))
				continue;
			struct input_absinfo ai;
			if (ioctl(fd, EVIOCGABS(code), &ai) < 0)
				continue;
			e.type = EV_ABS;
			e.code = code;
			e.value = ai.value;
			push(q, &e);
		}
	}
}

static void drain(int fd, struct evq *q)
{
	struct input_event buf[64];
	for (;;) {
		ssize_t r = real_read(fd, buf, sizeof(buf));
		if (r <= 0)
			break;
		int cnt = r / sizeof(buf[0]);
		for (int i = 0; i < cnt; i++) {
			if (buf[i].type == EV_SYN && buf[i].code == SYN_DROPPED) {
				resync(fd, q);
				/* skip until next SYN_REPORT as per evdev protocol */
				while (++i < cnt && !(buf[i].type == EV_SYN && buf[i].code == SYN_REPORT))
					;
				continue;
			}
			push(q, &buf[i]);
		}
		if (cnt < 64)
			break;
	}
}

static int pop(struct evq *q, struct input_event *out)
{
	if (q->n == 0)
		return 0;
	int idx = 0;
	for (int i = 0; i < q->n; i++) {     /* buttons/d-pad first, in order */
		if (is_discrete(&q->ev[i])) {
			idx = i;
			break;
		}
	}
	*out = q->ev[idx];
	memmove(&q->ev[idx], &q->ev[idx + 1], (q->n - idx - 1) * sizeof(q->ev[0]));
	q->n--;
	return 1;
}

ssize_t read(int fd, void *buf, size_t count)
{
	if (!real_read)
		init();
	if (count != sizeof(struct input_event))
		return real_read(fd, buf, count);

	pthread_mutex_lock(&q_mtx);
	struct evq *q = get_q(fd);
	if (!q) {
		pthread_mutex_unlock(&q_mtx);
		return real_read(fd, buf, count);
	}

	int saved = errno;
	drain(fd, q);
	errno = saved;

	struct input_event e;
	ssize_t ret;
	if (pop(q, &e)) {
		memcpy(buf, &e, sizeof(e));
		ret = sizeof(e);
	} else {
		errno = EAGAIN;
		ret = -1;
	}
	pthread_mutex_unlock(&q_mtx);
	return ret;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	if (!real_poll)
		init();

	int pending = 0;
	pthread_mutex_lock(&q_mtx);
	for (nfds_t i = 0; i < nfds; i++) {
		int fd = fds[i].fd;
		if (fd >= 0 && fd < MAX_FD && q_tab[fd] && q_tab[fd]->n > 0)
			pending = 1;
	}
	pthread_mutex_unlock(&q_mtx);

	int r = real_poll(fds, nfds, pending ? 0 : timeout);
	if (!pending || r < 0)
		return r;

	r = 0;
	pthread_mutex_lock(&q_mtx);
	for (nfds_t i = 0; i < nfds; i++) {
		int fd = fds[i].fd;
		if (fd >= 0 && fd < MAX_FD && q_tab[fd] && q_tab[fd]->n > 0 && (fds[i].events & POLLIN))
			fds[i].revents |= POLLIN;
		if (fds[i].revents)
			r++;
	}
	pthread_mutex_unlock(&q_mtx);
	return r;
}

int close(int fd)
{
	if (!real_close)
		init();
	if (fd >= 0 && fd < MAX_FD) {
		pthread_mutex_lock(&q_mtx);
		if (q_tab[fd]) {
			q_tab[fd]->n = 0;
			q_tab[fd]->known = 0;   /* return slot to pool */
			q_tab[fd] = NULL;
		}
		pthread_mutex_unlock(&q_mtx);
	}
	return real_close(fd);
}

static int filter_open(int fd, const char *path)
{
	if (fd < 0 || !path || strncmp(path, "/dev/input/event", 16))
		return fd;

	struct input_id id = {0};
	char name[128] = {0};
	ioctl(fd, EVIOCGID, &id);
	ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);

	if (id.vendor == 0x28de && id.product == 0x11ff && strcmp(name, PROXY_NAME)) {
		fprintf(stderr, "[parsec-evdev-fix] hiding %s (%s) from Parsec\n", path, name);
		real_close(fd);
		errno = EACCES;
		return -1;
	}
	fprintf(stderr, "[parsec-evdev-fix] Parsec opened %s (%04x:%04x %s)\n", path, id.vendor, id.product, name);
	return fd;
}

int open(const char *path, int flags, ...)
{
	if (!real_open)
		init();
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return filter_open(real_open(path, flags, mode), path);
}

int open64(const char *path, int flags, ...)
{
	if (!real_open64)
		init();
	mode_t mode = 0;
	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return filter_open(real_open64(path, flags, mode), path);
}
