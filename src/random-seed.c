/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

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

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include "log.h"
#include "util.h"

int main(int argc, char *argv[]) {
        int seed_fd = -1, random_fd = -1;
        int ret = 1;
        uint8_t buf[512];
        ssize_t r;

        if (argc != 2) {
                log_error("This program requires one argument.");
                return 1;
        }

        log_parse_environment();

        /* When we load the seed we read it and write it to the device
         * and then immediately update the saved seed with new data,
         * to make sure the next boot gets seeded differently. */

        if (streq(argv[1], "load")) {

                if ((seed_fd = open(RANDOM_SEED, O_RDWR|O_CLOEXEC|O_NOCTTY|O_CREAT, 0600)) < 0) {
                        if ((seed_fd = open(RANDOM_SEED, O_RDONLY|O_CLOEXEC|O_NOCTTY)) < 0) {
                                log_error("Failed to open random seed: %m");
                                goto finish;
                        }
                }

                if ((random_fd = open("/dev/urandom", O_RDWR|O_CLOEXEC|O_NOCTTY, 0600)) < 0) {
                        if ((random_fd = open("/dev/urandom", O_WRONLY|O_CLOEXEC|O_NOCTTY, 0600)) < 0) {
                                log_error("Failed to open /dev/urandom: %m");
                                goto finish;
                        }
                }

                if ((r = loop_read(seed_fd, buf, sizeof(buf), false)) != sizeof(buf))
                        log_error("Failed to read seed file: %s", r < 0 ? strerror(errno) : "EOF");
                else {
                        lseek(seed_fd, 0, SEEK_SET);

                        if ((r = loop_write(random_fd, buf, sizeof(buf), false)) != sizeof(buf))
                                log_error("Failed to write seed to /dev/random: %s", r < 0 ? strerror(errno) : "short write");
                }

        } else if (streq(argv[1], "save")) {

                if ((seed_fd = open(RANDOM_SEED, O_WRONLY|O_CLOEXEC|O_NOCTTY|O_CREAT, 0600)) < 0) {
                        log_error("Failed to open random seed: %m");
                        goto finish;
                }

                if ((random_fd = open("/dev/urandom", O_RDONLY|O_CLOEXEC|O_NOCTTY)) < 0) {
                        log_error("Failed to open /dev/urandom: %m");
                        goto finish;
                }
        } else {
                log_error("Unknown verb %s.", argv[1]);
                goto finish;
        }

        /* This is just a safety measure. Given that we are root and
         * most likely created the file ourselves the mode and owner
         * should be correct anyway. */
        fchmod(seed_fd, 0600);
        fchown(seed_fd, 0, 0);

        if ((r = loop_read(random_fd, buf, sizeof(buf), false)) != sizeof(buf))
                log_error("Failed to read new seed from /dev/urandom: %s", r < 0 ? strerror(errno) : "EOF");
        else {
                if ((r = loop_write(seed_fd, buf, sizeof(buf), false)) != sizeof(buf))
                        log_error("Failed to write new random seed file: %s", r < 0 ? strerror(errno) : "short write");
        }

        ret = 0;

finish:
        if (random_fd >= 0)
                close_nointr_nofail(random_fd);

        if (seed_fd >= 0)
                close_nointr_nofail(seed_fd);

        return ret;
}