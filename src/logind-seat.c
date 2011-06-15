/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <string.h>

#include "logind-seat.h"
#include "logind-acl.h"
#include "util.h"

Seat *seat_new(Manager *m, const char *id) {
        Seat *s;

        assert(m);
        assert(id);

        s = new0(Seat, 1);
        if (!s)
                return NULL;

        s->state_file = strappend("/run/systemd/seat/", id);
        if (!s->state_file) {
                free(s);
                return NULL;
        }

        s->id = file_name_from_path(s->state_file);
        s->manager = m;

        if (hashmap_put(m->seats, s->id, s) < 0) {
                free(s->state_file);
                free(s);
                return NULL;
        }

        return s;
}

void seat_free(Seat *s) {
        assert(s);

        if (s->in_gc_queue)
                LIST_REMOVE(Seat, gc_queue, s->manager->seat_gc_queue, s);

        while (s->sessions)
                session_free(s->sessions);

        assert(!s->active);

        while (s->devices)
                device_free(s->devices);

        hashmap_remove(s->manager->seats, s->id);

        free(s->state_file);
        free(s);
}

int seat_save(Seat *s) {
        int r;
        FILE *f;
        char *temp_path;

        assert(s);

        r = safe_mkdir("/run/systemd/seat", 0755, 0, 0);
        if (r < 0)
                goto finish;

        r = fopen_temporary(s->state_file, &f, &temp_path);
        if (r < 0)
                goto finish;

        fchmod(fileno(f), 0644);

        fprintf(f,
                "# This is private data. Do not parse.\n"
                "IS_VTCONSOLE=%i\n",
                s->manager->vtconsole == s);

        if (s->active) {
                assert(s->active->user);

                fprintf(f,
                        "ACTIVE=%s\n"
                        "ACTIVE_UID=%lu\n",
                        s->active->id,
                        (unsigned long) s->active->user->uid);
        }

        if (s->sessions) {
                Session *i;

                fputs("OTHER=", f);
                LIST_FOREACH(sessions_by_seat, i, s->sessions) {
                        if (i == s->active)
                                continue;

                        fprintf(f,
                                "%s%c",
                                i->id,
                                i->sessions_by_seat_next ? ' ' : '\n');
                }

                fputs("OTHER_UIDS=", f);
                LIST_FOREACH(sessions_by_seat, i, s->sessions) {
                        if (i == s->active)
                                continue;

                        fprintf(f,
                                "%lu%c",
                                (unsigned long) i->user->uid,
                                i->sessions_by_seat_next ? ' ' : '\n');
                }
        }

        fflush(f);

        if (ferror(f) || rename(temp_path, s->state_file) < 0) {
                r = -errno;
                unlink(s->state_file);
                unlink(temp_path);
        }

        fclose(f);
        free(temp_path);

finish:
        if (r < 0)
                log_error("Failed to save seat data for %s: %s", s->id, strerror(-r));

        return r;
}

int seat_load(Seat *s) {
        assert(s);

        return 0;
}

static int vt_allocate(int vtnr) {
        int fd, r;
        char *p;

        assert(vtnr >= 1);

        if (asprintf(&p, "/dev/tty%i", vtnr) < 0)
                return -ENOMEM;

        fd = open_terminal(p, O_RDWR|O_NOCTTY|O_CLOEXEC);
        free(p);

        r = fd < 0 ? -errno : 0;

        if (fd >= 0)
                close_nointr_nofail(fd);

        return r;
}

static int seat_preallocate_vts(Seat *s) {
        int r = 0;
        unsigned i;

        assert(s);
        assert(s->manager);

        if (s->manager->n_autovts <= 0)
                return 0;

        if (s->manager->vtconsole != s)
                return 0;

        for (i = 1; i < s->manager->n_autovts; i++) {
                int q;

                q = vt_allocate(i);
                if (q < 0) {
                        log_error("Failed to preallocate VT %i: %s", i, strerror(-q));
                        r = q;
                }
        }

        return r;
}

int seat_apply_acls(Seat *s, Session *old_active) {
        int r;

        assert(s);

        r = devnode_acl_all(s->manager->udev,
                            s->id,
                            false,
                            !!old_active, old_active ? old_active->user->uid : 0,
                            !!s->active, s->active ? s->active->user->uid : 0);

        if (r < 0)
                log_error("Failed to apply ACLs: %s", strerror(-r));

        return r;
}

int seat_active_vt_changed(Seat *s, int vtnr) {
        Session *i, *new_active = NULL, *old_active;

        assert(s);
        assert(vtnr >= 1);

        if (s->manager->vtconsole != s)
                return -EINVAL;

        log_debug("VT changed to %i", vtnr);

        LIST_FOREACH(sessions_by_seat, i, s->sessions)
                if (i->vtnr == vtnr) {
                        new_active = i;
                        break;
                }

        if (new_active == s->active)
                return 0;

        old_active = s->active;
        s->active = new_active;

        seat_apply_acls(s, old_active);
        manager_spawn_autovt(s->manager, vtnr);

        return 0;
}

int seat_read_active_vt(Seat *s) {
        char t[64];
        ssize_t k;
        int r, vtnr;

        assert(s);

        if (s->manager->vtconsole != s)
                return 0;

        lseek(s->manager->console_active_fd, SEEK_SET, 0);

        k = read(s->manager->console_active_fd, t, sizeof(t)-1);
        if (k <= 0) {
                log_error("Failed to read current console: %s", k < 0 ? strerror(-errno) : "EOF");
                return k < 0 ? -errno : -EIO;
        }

        t[k] = 0;
        truncate_nl(t);

        if (!startswith(t, "tty")) {
                log_error("Hm, /sys/class/tty/tty0/active is badly formatted.");
                return -EIO;
        }

        r = safe_atoi(t+3, &vtnr);
        if (r < 0) {
                log_error("Failed to parse VT number %s", t+3);
                return r;
        }

        if (vtnr <= 0) {
                log_error("VT number invalid: %s", t+3);
                return -EIO;
        }

        return seat_active_vt_changed(s, vtnr);
}

int seat_start(Seat *s) {
        assert(s);

        if (s->started)
                return 0;

        log_info("New seat %s.", s->id);

        /* Initialize VT magic stuff */
        seat_preallocate_vts(s);

        /* Read current VT */
        seat_read_active_vt(s);

        /* Save seat data */
        seat_save(s);

        s->started = true;

        return 0;
}

int seat_stop(Seat *s) {
        Session *session;
        int r = 0, k;

        assert(s);

        if (!s->started)
                return 0;

        log_info("Removed seat %s.", s->id);

        LIST_FOREACH(sessions_by_seat, session, s->sessions) {
                k = session_stop(session);
                if (k < 0)
                        r = k;
        }

        unlink(s->state_file);
        seat_add_to_gc_queue(s);

        s->started = false;

        return r;
}

int seat_check_gc(Seat *s) {
        assert(s);

        if (s->manager->vtconsole == s)
                return 1;

        return !!s->devices;
}

void seat_add_to_gc_queue(Seat *s) {
        assert(s);

        if (s->in_gc_queue)
                return;

        LIST_PREPEND(Seat, gc_queue, s->manager->seat_gc_queue, s);
        s->in_gc_queue = true;
}

static bool seat_name_valid_char(char c) {
        return
                (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '-' ||
                c == '_';
}

bool seat_name_is_valid(const char *name) {
        const char *p;

        assert(name);

        if (!startswith(name, "seat"))
                return false;

        if (!name[4])
                return false;

        for (p = name; *p; p++)
                if (!seat_name_valid_char(*p))
                        return false;

        return true;
}